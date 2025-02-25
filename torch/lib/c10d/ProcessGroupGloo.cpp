#include <c10d/ProcessGroupGloo.hpp>

#include <gloo/allgather.h>
#include <gloo/allreduce.h>
#include <gloo/barrier.h>
#include <gloo/broadcast.h>
#include <gloo/gather.h>
#include <gloo/reduce.h>
#include <gloo/scatter.h>

#include <ATen/SparseTensorUtils.h>

#ifdef USE_CUDA
#include <ATen/cuda/CUDAEvent.h>
#include <ATen/cuda/Exceptions.h>
#include <ATen/cuda/PinnedMemoryAllocator.h>
#include <c10/cuda/CUDACachingAllocator.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#endif

#include <gloo/rendezvous/context.h>
#include <gloo/rendezvous/prefix_store.h>
#include <gloo/transport/tcp/device.h>

#define GENERATE_ALL_TYPES(type, func, args...)        \
  switch (type) {                                      \
    case ::at::ScalarType::Float:                      \
      func<float>(args);                               \
      break;                                           \
    case ::at::ScalarType::Double:                     \
      func<double>(args);                              \
      break;                                           \
    case ::at::ScalarType::Half:                       \
      func<gloo::float16>(args);                       \
      break;                                           \
    case ::at::ScalarType::Char:                       \
      func<int8_t>(args);                              \
      break;                                           \
    case ::at::ScalarType::Byte:                       \
      func<uint8_t>(args);                             \
      break;                                           \
    case ::at::ScalarType::Int:                        \
      func<int32_t>(args);                             \
      break;                                           \
    case ::at::ScalarType::Long:                       \
      func<int64_t>(args);                             \
      break;                                           \
    default:                                           \
      throw std::runtime_error("Invalid scalar type"); \
  }

namespace c10d {

namespace {

// Wrap c10d store as Gloo store
class GlooStore : public ::gloo::rendezvous::Store {
 public:
  GlooStore(const std::shared_ptr<::c10d::Store>& store) : store_(store) {}

  void set(const std::string& key, const std::vector<char>& value) override {
    std::vector<uint8_t> tmp(value.begin(), value.end());
    store_->set(key, tmp);
  }

  std::vector<char> get(const std::string& key) override {
    auto value = store_->get(key);
    return std::vector<char>(value.begin(), value.end());
  }

  void wait(const std::vector<std::string>& keys) override {
    store_->wait(keys, Store::kDefaultTimeout);
  }

  void wait(
      const std::vector<std::string>& keys,
      const std::chrono::milliseconds& timeout) override {
    store_->wait(keys, timeout);
  }

 protected:
  std::shared_ptr<::c10d::Store> store_;
};

typedef void (*ReduceFunc)(void*, const void*, const void*, size_t);

template <typename T>
ReduceFunc toFunction(const ReduceOp& r) {
  switch (r) {
    case ReduceOp::SUM:
      return ReduceFunc(&::gloo::sum<T>);
    case ReduceOp::PRODUCT:
      return ReduceFunc(&::gloo::product<T>);
    case ReduceOp::MIN:
      return ReduceFunc(&::gloo::min<T>);
    case ReduceOp::MAX:
      return ReduceFunc(&::gloo::max<T>);
    case ReduceOp::UNUSED:
      break;
  }

  throw std::runtime_error("Unhandled ReduceOp");
}

template <typename T, typename O>
void setInputs(O& opts, std::vector<at::Tensor>& tensors) {
  opts.setInputs(getDataPointers<T>(tensors), tensors[0].numel());
}

template <typename T, typename O>
void setInput(O& opts, at::Tensor& tensor) {
  opts.setInput(getDataPointer<T>(tensor), tensor.numel());
}

template <typename T, typename O>
void setOutputs(O& opts, std::vector<at::Tensor>& tensors) {
  opts.setOutputs(getDataPointers<T>(tensors), tensors[0].numel());
}

template <typename T, typename O>
void setOutput(O& opts, at::Tensor& tensor) {
  opts.setOutput(getDataPointer<T>(tensor), tensor.numel());
}

#ifdef USE_CUDA

at::Tensor pinnedLike(at::Tensor& tensor) {
  auto* allocator = at::cuda::getPinnedMemoryAllocator();
  auto storage = c10::Storage(
      tensor.dtype(),
      at::detail::computeStorageSize(tensor.sizes(), tensor.strides()),
      allocator,
      /*resizable=*/false);
  return at::empty({0}, tensor.options().device(at::kCPU))
      .set_(storage, 0, tensor.sizes(), tensor.strides());
}

// This function initializes a vector of CUDA streams, one for every
// tensor in the input tensor vector, and ensures that these streams are
// synchronized with the current default streams. This is needed so
// that new work on the new streams is serialized w.r.t. all operations
// on the tensors.
void initializeStreamsEvents(
    std::vector<at::Tensor>& tensors,
    std::vector<at::cuda::CUDAStream>& streams,
    std::vector<at::cuda::CUDAEvent>& events) {
  at::cuda::OptionalCUDAGuard guard;
  streams.reserve(tensors.size());
  events.resize(tensors.size());
  for (size_t i = 0; i < tensors.size(); i++) {
    guard.set_index(tensors[i].device().index());
    // Record event on current stream
    events[i].record(at::cuda::getCurrentCUDAStream());
    // Get a non-default stream to execute asynchronous CUDA operations
    // on for this device. This ensures that the default stream used
    // by the caller is not occupied by c10d related operations.
    streams.push_back(at::cuda::getStreamFromPool(
        /* isHighPriority */ true, tensors[i].device().index()));
    // Ensure the new stream is synchronized with the current stream.
    events[i].block(streams[i]);

    // `tensors` are created on a different stream. Hence, they must record
    // new streams in this Work to prevent being freed before the Work finishes.
    if (tensors[i].is_sparse()) {
      if (tensors[i].is_coalesced()) {
        c10::cuda::CUDACachingAllocator::recordStream(
            tensors[i].indices().storage().data(), streams[i]);
        c10::cuda::CUDACachingAllocator::recordStream(
            tensors[i].values().storage().data(), streams[i]);
      } else {
        // We will need to coalesce first, which means new tensors will
        // be allocated on the streams we just allocated, and there
        // is no need to record them separately.
      }
    } else {
      c10::cuda::CUDACachingAllocator::recordStream(
          tensors[i].storage().data(), streams[i]);
    }
  }
}

// This function initializes a vector of CUDA streams, one per device,
// and ensures that these streams are synchronized with the current default
// streams. It is assumed that the tensors in the nested tensor vectors are
// on the same device.
void initializeStreamsEvents(
    std::vector<std::vector<at::Tensor>>& tensors,
    std::vector<at::cuda::CUDAStream>& streams,
    std::vector<at::cuda::CUDAEvent>& events) {
  // Ensure that the tensors in the nested tensor vectors are on the same
  // device.
  for (size_t i = 0; i < tensors.size(); i++) {
    auto device_id = tensors[i][0].device().index();
    for (size_t j = 1; j < tensors[i].size(); j++) {
      if (tensors[i][j].device().index() != device_id) {
        throw std::runtime_error(
            "tensors in the nested tensor vectors need to be on the same device");
      }
    }
  }

  at::cuda::OptionalCUDAGuard guard;
  streams.reserve(tensors.size());
  events.resize(tensors.size());
  for (size_t i = 0; i < tensors.size(); i++) {
    guard.set_index(tensors[i][0].device().index());
    // Record event on current stream
    events[i].record(at::cuda::getCurrentCUDAStream());
    // Get a non-default stream to execute asynchronous CUDA operations
    // on for this output. This ensures that the default stream used
    // by the caller is not occupied by c10d related operations.
    streams.push_back(at::cuda::getStreamFromPool(
        /* isHighPriority */ true, tensors[i][0].device().index()));
    // Ensure the new stream is synchronized with the current stream.
    events[i].block(streams[i]);

    for (at::Tensor& tensor : tensors[i]) {
      // `tensors` are created on a different stream. Hence, they must record
      // new streams in this Work to prevent being freed before the Work
      // finishes.
      c10::cuda::CUDACachingAllocator::recordStream(
          tensor.storage().data(), streams[i]);
    }
  }
}

#endif

} // namespace

ProcessGroupGloo::SendWork::SendWork(
    at::Tensor& tensor,
    std::unique_ptr<::gloo::transport::UnboundBuffer> buffer)
    : tensor_(tensor), buffer_(std::move(buffer)) {}

void ProcessGroupGloo::SendWork::wait() {
  std::unique_lock<std::mutex> lock(mutex_);
  try {
    buffer_->waitSend();
  } catch (...) {
    exception_ = std::current_exception();
  }

  completed_ = true;
  if (exception_) {
    std::rethrow_exception(exception_);
  }
}

ProcessGroupGloo::RecvWork::RecvWork(
    at::Tensor& tensor,
    std::unique_ptr<::gloo::transport::UnboundBuffer> buffer)
    : tensor_(tensor), buffer_(std::move(buffer)), srcRank_(-1) {}

int ProcessGroupGloo::RecvWork::sourceRank() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return srcRank_;
}

void ProcessGroupGloo::RecvWork::wait() {
  std::unique_lock<std::mutex> lock(mutex_);
  try {
    buffer_->waitRecv(&srcRank_);
  } catch (...) {
    exception_ = std::current_exception();
  }

  completed_ = true;
  if (exception_) {
    std::rethrow_exception(exception_);
  }
}

ProcessGroupGloo::Options::Options()
    : timeout(std::chrono::milliseconds(10 * 1000)), threads(2) {}

ProcessGroupGloo::ProcessGroupGloo(
    const std::shared_ptr<Store>& store,
    int rank,
    int size,
    Options options)
    : ProcessGroup(rank, size),
      store_(new GlooStore(store)),
      stop_(false),
      collectiveCounter_(0) {
  auto& devices = options.devices;
  if (devices.empty()) {
    throw std::runtime_error("No device(s) specified");
  }

  // Create and connect a context for every device.
  //
  // Note that the same device can be specified multiple times, either
  // the same object, or the same logical device as different objects.
  // Either mode is fine and only has performance implications.
  //
  // Using the same object multiple times means all contexts share a
  // single I/O thread. If you use different objects for the same
  // logical device they will have independent I/O threads. The latter
  // option is needed if you have a fast NIC that cannot be saturated
  // by a single I/O thread.
  //
  contexts_.reserve(options.devices.size());
  for (size_t i = 0; i < options.devices.size(); i++) {
    auto context = std::make_shared<::gloo::rendezvous::Context>(rank_, size_);
    auto store = ::gloo::rendezvous::PrefixStore(std::to_string(i), *store_);
    context->setTimeout(options.timeout);
    context->connectFullMesh(store, options.devices[i]);
    contexts_.push_back(std::move(context));
  }

  // Every worker thread stores the AsyncWork object it's currently
  // working on in the workInProgress_ vector. It must have size equal
  // to the number of workers such that they can simply index into it
  // using the worker index they are started with.
  workInProgress_.resize(options.threads);

  threads_.resize(options.threads);
  for (size_t i = 0; i < threads_.size(); i++) {
    threads_[i] = std::thread(&ProcessGroupGloo::runLoop, this, i);
  }
}

ProcessGroupGloo::~ProcessGroupGloo() {
  std::unique_lock<std::mutex> lock(workMutex_);
  workConsumeCV_.wait(lock, [&] { return workQueue_.empty(); });

  // Queue is empty, signal stop
  stop_ = true;

  // Release lock to allow threads to terminate
  lock.unlock();

  workProduceCV_.notify_all();

  // Wait for worker threads to terminate
  for (auto& thread : threads_) {
    thread.join();
  }
}

uint32_t ProcessGroupGloo::nextTag() {
  return collectiveCounter_++;
}

std::shared_ptr<::gloo::Context> ProcessGroupGloo::getContext(uint32_t tag) {
  return contexts_[tag % contexts_.size()];
}

void ProcessGroupGloo::runLoop(int workerIndex) {
  std::unique_lock<std::mutex> lock(workMutex_);

  while (!stop_) {
    if (workQueue_.empty()) {
      workProduceCV_.wait(lock);
      continue;
    }

    auto work = std::move(workQueue_.front());
    workQueue_.pop_front();
    workInProgress_[workerIndex] = work;
    lock.unlock();

    // Notify after releasing the lock so that the waiter
    // does not immediately block.
    workConsumeCV_.notify_one();

    AsyncWork::execute(std::move(work));
    lock.lock();
    workInProgress_[workerIndex] = nullptr;
  }
}

void ProcessGroupGloo::enqueue(std::shared_ptr<AsyncWork> work) {
  std::unique_lock<std::mutex> lock(workMutex_);
  workQueue_.push_back(std::move(work));
  lock.unlock();

  // Notify after releasing the lock so that the waiter
  // does not immediately block.
  workProduceCV_.notify_one();
}

namespace {

class AsyncBroadcastWork : public ProcessGroupGloo::AsyncWork {
 public:
  AsyncBroadcastWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<at::Tensor>& inputs,
      int rootRank,
      int rootTensor,
      uint32_t tag)
      : context(context),
        inputs(inputs),
        rootRank(rootRank),
        rootTensor(rootTensor),
        tag(tag) {}

  std::shared_ptr<gloo::Context> context;
  std::vector<at::Tensor> inputs;
  const int rootRank;
  const int rootTensor;
  const uint32_t tag;

  void broadcast(at::Tensor& tensor) {
    const auto& scalarType = tensor.scalar_type();
    gloo::BroadcastOptions opts(context);
    opts.setRoot(rootRank);
    opts.setTag(tag);
    GENERATE_ALL_TYPES(scalarType, setOutput, opts, tensor);
    gloo::broadcast(opts);
  }

  void run() override {
    broadcast(inputs[rootTensor]);

    // Copy to non-root tensors
    for (size_t i = 0; i < inputs.size(); i++) {
      if (i == static_cast<size_t>(rootTensor)) {
        continue;
      }
      inputs[i].copy_(inputs[rootTensor]);
    }
  }
};

#ifdef USE_CUDA

class AsyncBroadcastCUDAWork : public AsyncBroadcastWork {
 public:
  AsyncBroadcastCUDAWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<at::Tensor>& inputs,
      int rootRank,
      int rootTensor,
      uint32_t tag)
      : AsyncBroadcastWork(context, inputs, rootRank, rootTensor, tag) {
    initializeStreamsEvents(inputs, streams, events);

    // Create pinned host side tensors.
    tmp = pinnedLike(inputs[rootTensor]);
    at::cuda::OptionalCUDAStreamGuard guard;
    if (context->rank == rootRank) {
      guard.reset_stream(streams[rootTensor]);
      tmp.copy_(inputs[rootTensor], /* non_blocking */ true);
    }
  }

  void run() override {
    at::cuda::OptionalCUDAStreamGuard guard;

    // Synchronize with copy operation if applicable.
    if (context->rank == rootRank) {
      guard.reset_stream(streams[rootTensor]);
      AT_CUDA_CHECK(cudaStreamSynchronize(streams[rootTensor]));
    }

    // Run broadcast on host side tensors.
    broadcast(tmp);

    // Kick off copy back to the CUDA tensors.
    for (size_t i = 0; i < inputs.size(); i++) {
      guard.reset_stream(streams[i]);
      inputs[i].copy_(tmp, /* non_blocking */ true);
      events[i].record(streams[i]);
    }
  }

  void synchronize() override {
    at::cuda::OptionalCUDAGuard guard;

    // Synchronize with the copy back to CUDA tensors.
    for (size_t i = 0; i < inputs.size(); i++) {
      guard.set_index(inputs[i].device().index());
      events[i].block(at::cuda::getCurrentCUDAStream());
    }
  }

  at::Tensor tmp;
  std::vector<at::cuda::CUDAStream> streams;
  std::vector<at::cuda::CUDAEvent> events;
};

#endif

} // namespace

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::broadcast(
    std::vector<at::Tensor>& inputs,
    const BroadcastOptions& opts) {
  static auto invalidArgument = [](const std::string& msg) {
    throw std::invalid_argument("ProcessGroupGloo::broadcast: " + msg);
  };

  assertRootRank(invalidArgument, opts.rootRank, size_);
  assertRootTensor(invalidArgument, opts.rootTensor, inputs.size());
  assertDense(invalidArgument, inputs);
  assertTypeAndSizesMatch(invalidArgument, inputs);

  const auto& device = inputs[0].device();
  switch (device.type()) {
    case at::kCPU:
#ifdef USE_CUDA
    case at::kCUDA:
#endif
      break;
    default:
      invalidArgument("unsupported device type");
  }

  std::shared_ptr<AsyncBroadcastWork> work;
  auto tag = nextTag();
  auto context = getContext(tag);
  if (device.type() == at::kCPU) {
    work = std::make_shared<AsyncBroadcastWork>(
        std::move(context), inputs, opts.rootRank, opts.rootTensor, tag);
#ifdef USE_CUDA
  } else if (device.type() == at::kCUDA) {
    work = std::make_shared<AsyncBroadcastCUDAWork>(
        std::move(context), inputs, opts.rootRank, opts.rootTensor, tag);
#endif
  } else {
    throw std::runtime_error("Invalid backend");
  }

  enqueue(work);
  return work;
}

namespace {

class AsyncAllreduceWork : public ProcessGroupGloo::AsyncWork {
 public:
  AsyncAllreduceWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<at::Tensor>& inputs,
      ReduceOp reduceOp,
      uint32_t tag)
      : context(context), inputs(inputs), reduceOp(reduceOp), tag(tag) {}

  std::shared_ptr<gloo::Context> context;
  std::vector<at::Tensor> inputs;
  const ReduceOp reduceOp;
  const uint32_t tag;

  void allreduce(std::vector<at::Tensor>& tensors) {
    const auto& scalarType = tensors[0].scalar_type();
    gloo::AllreduceOptions opts(context);
    opts.setReduceFunction(getFunction(scalarType, reduceOp));
    opts.setTag(tag);
    GENERATE_ALL_TYPES(scalarType, setOutputs, opts, tensors);
    gloo::allreduce(opts);
  }

  void run() override {
    allreduce(inputs);

    // Only the first output in the tensor list contains the results.
    // See https://github.com/facebookincubator/gloo/issues/152.
    // The contents is the same for every entry in the tensor list, so
    // we can use the first entry as the source of the copy below.
    for (size_t i = 1; i < inputs.size(); i++) {
      inputs[i].copy_(inputs[0]);
    }
  }

  template <typename T>
  void getFunction(gloo::AllreduceOptions::Func& fn, const ReduceOp op) {
    fn = toFunction<T>(op);
  }

  gloo::AllreduceOptions::Func getFunction(
      const at::ScalarType& dtype,
      const ReduceOp op) {
    gloo::AllreduceOptions::Func fn;
    GENERATE_ALL_TYPES(dtype, getFunction, fn, op);
    return fn;
  }
};

class AsyncAllreduceCoalescedWork : public AsyncAllreduceWork {
 public:
  AsyncAllreduceCoalescedWork(
    const std::shared_ptr<gloo::Context>& context,
    std::vector<at::Tensor>& inputs,
    ReduceOp reduceOp,
    uint32_t tag)
    : AsyncAllreduceWork(context, inputs, reduceOp, tag) {}

  void run() override {
    allreduceCoalesced(inputs);
  }

 private:
  void allreduceCoalesced(std::vector<at::Tensor>& tensors) {
    // reduce coalesced, flattened tensors.
    at::Tensor coalescedTensor = flattenDenseTensors(tensors);
    std::vector<at::Tensor> allreduceInput = {coalescedTensor};
    allreduce(allreduceInput);

    // separate and reshape tensors.
    size_t offset = 0;
    for (at::Tensor& tensor : tensors) {
      const int64_t tensorNumel = tensor.numel();
      const c10::IntArrayRef tensorShape = tensor.sizes();
      tensor.copy_(
        coalescedTensor.slice(0, offset, offset+tensorNumel).view(tensorShape));
      offset += tensorNumel;
    }
  }
};

class AsyncSparseAllreduceWork : public ProcessGroupGloo::AsyncWork {
 public:
  AsyncSparseAllreduceWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<at::Tensor>& inputs,
      uint32_t tag)
      : context(context), inputs(inputs), tag(tag) {}

  std::shared_ptr<gloo::Context> context;
  std::vector<at::Tensor> inputs;
  std::vector<at::Tensor> outputs;
  const uint32_t tag;

  // We share dimensionality about the sparse tensors before collecting
  // their contents. We assume here that the maximum number of sparse
  // and dense dimensions is 4. This is stored in a contiguous piece of
  // memory so that we can easily run allgather on it.
  //
  // The layout of this memory is as follows:
  //
  //   - [0:4]: sparse dims
  //   - [4:8]: dense dims
  //   -   [8]: nnz
  //
  class SparseTensorMetadata {
   public:
    static constexpr auto dim = 9;

    // Construct from an existing metadata tensor to facilitate structured
    // access to metadata from peers, after gathering it.
    explicit SparseTensorMetadata(at::Tensor metadata)
        : metadata_(metadata), data_(metadata_.data_ptr<long>()) {
      AT_ASSERT(metadata.scalar_type() == at::kLong);
      AT_ASSERT(metadata.dim() == 1);
      AT_ASSERT(metadata.size(0) == dim);
    }

    // Populate the metadata.
    void populate_from_sparse_tensor(const at::Tensor& tensor) {
      const auto sparse_dim = tensor.sparse_dim();
      AT_ASSERT(sparse_dim <= 4);
      for (auto i = 0; i < 4; i++) {
        if (i < sparse_dim) {
          data_[i] = tensor.size(i);
        }
      }
      const auto dense_dim = tensor.dense_dim();
      AT_ASSERT(dense_dim <= 4);
      for (auto i = 0; i < 4; i++) {
        if (i < dense_dim) {
          data_[i + 4] = tensor.size(sparse_dim + i);
        }
      }
      data_[8] = tensor._nnz();
    }

    std::vector<int64_t> sizes() const {
      std::vector<int64_t> sizes;
      // Sparse sizes
      for (auto i = 0; i < 4; i++) {
        if (data_[i] <= 0) {
          break;
        }
        sizes.push_back(data_[i]);
      }
      // Dense sizes
      for (auto i = 4; i < 8; i++) {
        if (data_[i] <= 0) {
          break;
        }
        sizes.push_back(data_[i]);
      }
      return sizes;
    }

    int64_t nnz() const {
      return data_[8];
    }

   protected:
    at::Tensor metadata_;
    long* data_;
  };

  // Sparse allreduce is implemented with allgather on indices and values.
  // Every process then sums the resulting sparse tensors locally.
  // The nnz for sparse tensors may be different across processes, so first
  // we run allgather on the nnz, and then allgather with max(nnz).
  // We could use an allgatherv for this, if it were available.
  at::Tensor allreduce(std::vector<at::Tensor>& tensors) {
    auto input = tensors[0];

    // Perform local reduction if we have multiple inputs.
    for (size_t i = 1; i < tensors.size(); i++) {
      input += tensors[i];
    }

    // Need to coalesce before we can access indices and values.
    input = input.coalesce();

    // Gather metadata information from all ranks.
    auto metadata = allgather_metadata(input);

    // Sanity check dimensionality across ranks.
    {
      const auto expected = metadata[context->rank].sizes();
      for (size_t i = 0; i < context->size; i++) {
        if (i == context->rank) {
          continue;
        }
        const auto actual = metadata[i].sizes();
        AT_CHECK(actual == expected, "Sparse dimensions do not match");
      }
    }

    // Gather all indices and all values.
    auto indices = allgather_indices(input, metadata);
    auto values = allgather_values(input, metadata);

    // Perform global reduction.
    AT_ASSERT(indices.size() == context->size);
    AT_ASSERT(values.size() == context->size);
    auto output = at::sparse_coo_tensor(
        indices[0], values[0], input.sizes(), input.options());
    for (size_t i = 1; i < context->size; i++) {
      output += at::sparse_coo_tensor(
          indices[i], values[i], input.sizes(), input.options());
    }

    // Coalesce for good measure.
    return output.coalesce();
  }

  void run() override {
    auto output = allreduce(inputs);

    // Copy back to input tensors.
    outputs.reserve(inputs.size());
    for (size_t i = 0; i < inputs.size(); i++) {
      outputs.push_back(output.clone());
    }
  }

  std::vector<at::Tensor> result() const override {
    return outputs;
  }

 private:
  std::vector<SparseTensorMetadata> allgather_metadata(
      const at::Tensor& tensor) {
    auto buffer =
        at::zeros({context->size, SparseTensorMetadata::dim}, at::kLong);

    // Prepare metadata vector (1 entry per rank)
    std::vector<SparseTensorMetadata> metadata;
    metadata.reserve(context->size);
    for (auto i = 0; i < context->size; i++) {
      metadata.emplace_back(buffer.select(0, i));
    }

    // Populate data for this rank
    metadata[context->rank].populate_from_sparse_tensor(tensor);

    // Allgather metadata
    gloo::AllgatherOptions opts(context);
    opts.setOutput(buffer.data_ptr<long>(), buffer.numel());
    opts.setTag(tag);
    gloo::allgather(opts);

    return metadata;
  }

  std::vector<at::Tensor> allgather_indices(
      const at::Tensor& tensor,
      const std::vector<SparseTensorMetadata>& metadata) {
    auto max_nnz = metadata[0].nnz();
    for (size_t i = 1; i < metadata.size(); i++) {
      max_nnz = std::max(max_nnz, metadata[i].nnz());
    }

    // There are #sparse_dim() 1-dimensional tensors with nnz elems per rank.
    auto buffer =
        at::empty({context->size, tensor.sparse_dim(), max_nnz}, at::kLong);
    buffer.select(0, context->rank)
        .narrow(1, 0, tensor._nnz())
        .copy_(tensor.indices());

    // Allgather indices.
    gloo::AllgatherOptions opts(context);
    opts.setOutput(buffer.data_ptr<long>(), buffer.numel());
    opts.setTag(tag);
    gloo::allgather(opts);

    // Compile indices tensor per rank.
    std::vector<at::Tensor> indices;
    indices.reserve(metadata.size());
    for (size_t i = 0; i < metadata.size(); i++) {
      indices.push_back(buffer.select(0, i).narrow(1, 0, metadata[i].nnz()));
    }

    return indices;
  }

  std::vector<at::Tensor> allgather_values(
      const at::Tensor& tensor,
      const std::vector<SparseTensorMetadata>& metadata) {
    auto max_nnz = metadata[0].nnz();
    for (size_t i = 1; i < metadata.size(); i++) {
      max_nnz = std::max(max_nnz, metadata[i].nnz());
    }

    // There are nnz #dense_dim()-dimensional tensors per rank.
    const auto value_shape = tensor.sizes().slice(tensor.sparse_dim());
    auto buffer_shape = std::vector<int64_t>({context->size, max_nnz});
    std::copy(
        value_shape.begin(),
        value_shape.end(),
        std::back_inserter(buffer_shape));
    auto buffer = at::empty(buffer_shape, tensor.scalar_type());
    buffer.select(0, context->rank)
        .narrow(0, 0, tensor._nnz())
        .copy_(tensor.values());

    // Allgather values.
    gloo::AllgatherOptions opts(context);
    GENERATE_ALL_TYPES(tensor.scalar_type(), setOutput, opts, buffer);
    opts.setTag(tag);
    gloo::allgather(opts);

    // Compile values tensor per rank.
    std::vector<at::Tensor> values;
    values.reserve(metadata.size());
    for (size_t i = 0; i < metadata.size(); i++) {
      values.push_back(buffer.select(0, i).narrow(0, 0, metadata[i].nnz()));
    }

    return values;
  }
};

#ifdef USE_CUDA

class AsyncAllreduceCUDAWork : public AsyncAllreduceWork {
 public:
  AsyncAllreduceCUDAWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<at::Tensor>& inputs,
      ReduceOp reduceOp,
      uint32_t tag)
      : AsyncAllreduceWork(context, inputs, reduceOp, tag) {
    initializeStreamsEvents(inputs, streams, events);

    // Kick off copy from CUDA tensors to pinned CPU tensors.
    tmp.reserve(inputs.size());
    at::cuda::OptionalCUDAStreamGuard guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      guard.reset_stream(streams[i]);
      tmp.push_back(pinnedLike(inputs[i]).copy_(inputs[i], true));
    }
  }

  void run() override {
    // Synchronize with copy operations.
    at::cuda::OptionalCUDAGuard device_guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      device_guard.set_index(inputs[i].device().index());
      AT_CUDA_CHECK(cudaStreamSynchronize(streams[i]));
    }

    // Run allreduce on host side tensors.
    allreduce(tmp);

    // Kick off copy back to the CUDA tensors.
    // Only the first output in the tensor list contains the results.
    // See https://github.com/facebookincubator/gloo/issues/152.
    // The contents is the same for every entry in the tensor list, so
    // we can use the first entry as the source of the copy below.
    at::cuda::OptionalCUDAStreamGuard stream_guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      stream_guard.reset_stream(streams[i]);
      inputs[i].copy_(tmp[0], /* non_blocking */ true);
      events[i].record(streams[i]);
    }
  }

  void synchronize() override {
    // Synchronize with the copy back to CUDA tensors.
    at::cuda::OptionalCUDAGuard guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      guard.set_index(inputs[i].device().index());
      events[i].block(at::cuda::getCurrentCUDAStream());
    }
  }

  std::vector<at::Tensor> tmp;
  std::vector<at::cuda::CUDAStream> streams;
  std::vector<at::cuda::CUDAEvent> events;
};

class AsyncSparseAllreduceCUDAWork : public AsyncSparseAllreduceWork {
 public:
  AsyncSparseAllreduceCUDAWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<at::Tensor>& inputs,
      uint32_t tag)
      : AsyncSparseAllreduceWork(context, inputs, tag) {
    initializeStreamsEvents(inputs, streams, events);

    // Kick off copy from CUDA tensors to CPU tensors.
    // Note that both coalescing the sparse tensor and copying it to CPU
    // memory must be performed asynchronously, or we block the caller.
    tmp.reserve(inputs.size());
    at::cuda::OptionalCUDAStreamGuard guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      guard.reset_stream(streams[i]);
      tmp.push_back(
          inputs[i].coalesce().to(at::DeviceType::CPU, /*non_blocking=*/true));
    }
  }

  void run() override {
    // Synchronize with copy operations.
    at::cuda::OptionalCUDAGuard device_guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      device_guard.set_index(inputs[i].device().index());
      AT_CUDA_CHECK(cudaStreamSynchronize(streams[i]));
    }

    // Run allreduce on host side tensors.
    auto output = allreduce(tmp);

    // Kick off copy back to the CUDA tensors.
    at::cuda::OptionalCUDAStreamGuard stream_guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      stream_guard.reset_stream(streams[i]);
      outputs.push_back(output.to(inputs[i].device(), /*non_blocking=*/true));
      events[i].record(streams[i]);
    }
  }

  void synchronize() override {
    // Synchronize with the copy back to CUDA tensors.
    at::cuda::OptionalCUDAGuard guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      guard.set_index(inputs[i].device().index());
      events[i].block(at::cuda::getCurrentCUDAStream());
    }
  }

  std::vector<at::Tensor> tmp;
  std::vector<at::cuda::CUDAStream> streams;
  std::vector<at::cuda::CUDAEvent> events;
};

#endif

} // namespace

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::allreduce(
    std::vector<at::Tensor>& inputs,
    const AllreduceOptions& opts) {
  static auto invalidArgument = [](const std::string& msg) {
    throw std::invalid_argument("ProcessGroupGloo::allreduce: " + msg);
  };

  assertNonEmpty(invalidArgument, inputs);
  assertLayoutMatch(invalidArgument, inputs);
  assertTypeAndSizesMatch(invalidArgument, inputs);

  const auto& device = inputs[0].device();
  switch (device.type()) {
    case at::kCPU:
#ifdef USE_CUDA
    case at::kCUDA:
#endif
      break;
    default:
      invalidArgument("unsupported device type");
  }

  const auto& layout = inputs[0].layout();
  if (layout == c10::kSparse && opts.reduceOp != ReduceOp::SUM) {
    invalidArgument(
        "unsupported reduction operation "
        "(allreduce of sparse tensors only works with ReduceOp.SUM)");
  }

  std::shared_ptr<AsyncWork> work;
  auto tag = nextTag();
  auto context = getContext(tag);
  if (device.type() == at::kCPU) {
    if (layout == c10::kStrided) {
      work = std::make_shared<AsyncAllreduceWork>(
          std::move(context), inputs, opts.reduceOp, tag);
    } else if (layout == c10::kSparse) {
      work = std::make_shared<AsyncSparseAllreduceWork>(
          std::move(context), inputs, tag);
    } else {
      invalidArgument("unsupported layout");
    }
#ifdef USE_CUDA
  } else if (device.type() == at::kCUDA) {
    if (layout == c10::kStrided) {
      work = std::make_shared<AsyncAllreduceCUDAWork>(
          std::move(context), inputs, opts.reduceOp, tag);
    } else if (layout == c10::kSparse) {
      work = std::make_shared<AsyncSparseAllreduceCUDAWork>(
          std::move(context), inputs, tag);
    } else {
      invalidArgument("unsupported layout");
    }
#endif
  } else {
    throw std::runtime_error("Invalid backend");
  }

  enqueue(work);
  return work;
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::allreduce_coalesced(
  std::vector<at::Tensor>& tensors,
  const AllreduceCoalescedOptions& opts) {
    static auto invalidArgument = [](const std::string& msg) {
      throw std::invalid_argument("ProcessGroupGloo::allreduce_coalesced: " + msg);
    };
    assertNonEmpty(invalidArgument, tensors);

    // tensors will be flattened and concatenated (coalesced). This means that input
    // tensors must have the same device, layout and type.
    assertLayoutMatch(invalidArgument, tensors);
    if (!std::all_of(tensors.begin(),
                     tensors.end(),
                     [&](at::Tensor& t){return t.type() == tensors[0].type();})) {
      invalidArgument("tensors must all have the same type");
    }
    if (!std::all_of(tensors.begin(),
                     tensors.end(),
                     [&](at::Tensor& t){return t.device() == tensors[0].device();})) {
      invalidArgument("tensors must all be on the same device");
    }

    const c10::Device& device = tensors[0].device();
    const c10::Layout& layout = tensors[0].layout();

    // invalid arguments are detected early here before any calls to nextTag()
    // which result in the collectiveCounter_ being incremented.
    switch (device.type()) {
      case c10::kCPU:
        break;
      default:
        invalidArgument("unsupported device type");
    }

    switch (layout) {
      case c10::kStrided:
        break;
      default:
        invalidArgument("unsupported layout");
    }

    std::shared_ptr<AsyncWork> work;
    const uint32_t tag = nextTag();
    std::shared_ptr<gloo::Context> context = getContext(tag);
    if (device.type() == c10::kCPU) {
      if (layout == c10::kStrided) {
        work = std::make_shared<AsyncAllreduceCoalescedWork>(
            std::move(context), tensors, opts.reduceOp, tag);
      } else {
        invalidArgument("unsupported layout");
      }
    } else {
      throw std::runtime_error("Invalid backend");
    }
    enqueue(work);
    return work;
}

namespace {

class AsyncReduceWork : public ProcessGroupGloo::AsyncWork {
 public:
  AsyncReduceWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<at::Tensor>& inputs,
      int rootRank,
      int rootTensor,
      ReduceOp reduceOp,
      uint32_t tag)
      : context(context),
        inputs(inputs),
        rootRank(rootRank),
        rootTensor(rootTensor),
        reduceOp(reduceOp),
        tag(tag) {}

  std::shared_ptr<gloo::Context> context;
  std::vector<at::Tensor> inputs;
  const int rootRank;
  const int rootTensor;
  const ReduceOp reduceOp;
  const uint32_t tag;

  void reduce(std::vector<at::Tensor>& tensors) {
    const auto& scalarType = tensors[0].scalar_type();
    gloo::ReduceOptions opts(context);
    opts.setRoot(rootRank);
    opts.setTag(tag);
    opts.setReduceFunction(getFunction(scalarType, reduceOp));
    GENERATE_ALL_TYPES(scalarType, setOutput, opts, tensors[0]);
    gloo::reduce(opts);
  }

  void run() override {
    reduce(inputs);
  }

 protected:
  template <typename T>
  void getFunction(gloo::ReduceOptions::Func& fn, const ReduceOp op) {
    fn = toFunction<T>(op);
  }

  gloo::ReduceOptions::Func getFunction(
      const at::ScalarType& dtype,
      const ReduceOp op) {
    gloo::ReduceOptions::Func fn;
    GENERATE_ALL_TYPES(dtype, getFunction, fn, op);
    return fn;
  }
};

#ifdef USE_CUDA

class AsyncReduceCUDAWork : public AsyncReduceWork {
 public:
  AsyncReduceCUDAWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<at::Tensor>& inputs,
      int rootRank,
      int rootTensor,
      ReduceOp reduceOp,
      uint32_t tag)
      : AsyncReduceWork(context, inputs, rootRank, rootTensor, reduceOp, tag) {
    initializeStreamsEvents(inputs, streams, events);

    // Kick off copy from CUDA tensors to pinned CPU tensors.
    tmp.reserve(inputs.size());
    at::cuda::OptionalCUDAStreamGuard guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      guard.reset_stream(streams[i]);
      tmp.push_back(pinnedLike(inputs[i]).copy_(inputs[i], true));
    }
  }

  void run() override {
    // Synchronize with copy operations.
    at::cuda::OptionalCUDAGuard device_guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      device_guard.set_index(inputs[i].device().index());
      AT_CUDA_CHECK(cudaStreamSynchronize(streams[i]));
    }

    // Run reduce on host side tensors.
    reduce(tmp);

    // Kick off copy back to the CUDA tensors.
    at::cuda::OptionalCUDAStreamGuard stream_guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      stream_guard.reset_stream(streams[i]);
      inputs[i].copy_(tmp[i], /* non_blocking */ true);
      events[i].record(streams[i]);
    }
  }

  void synchronize() override {
    // Synchronize with the copy back to CUDA tensors.
    at::cuda::OptionalCUDAGuard guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      guard.set_index(inputs[i].device().index());
      events[i].block(at::cuda::getCurrentCUDAStream());
    }
  }

  std::vector<at::Tensor> tmp;
  std::vector<at::cuda::CUDAStream> streams;
  std::vector<at::cuda::CUDAEvent> events;
};

#endif

} // namespace

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::reduce(
    std::vector<at::Tensor>& inputs,
    const ReduceOptions& opts) {
  static auto invalidArgument = [](const std::string& msg) {
    throw std::invalid_argument("ProcessGroupGloo::reduce: " + msg);
  };

  assertRootRank(invalidArgument, opts.rootRank, size_);
  assertRootTensor(invalidArgument, opts.rootTensor, inputs.size());
  assertSingleElement(invalidArgument, inputs);
  assertDense(invalidArgument, inputs);

  const auto& device = inputs[0].device();
  switch (device.type()) {
    case at::kCPU:
#ifdef USE_CUDA
    case at::kCUDA:
#endif
      break;
    default:
      invalidArgument("unsupported device type");
  }

  std::shared_ptr<AsyncReduceWork> work;
  auto tag = nextTag();
  auto context = getContext(tag);
  if (device.type() == at::kCPU) {
    work = std::make_shared<AsyncReduceWork>(
        std::move(context),
        inputs,
        opts.rootRank,
        opts.rootTensor,
        opts.reduceOp,
        tag);
#ifdef USE_CUDA
  } else if (device.type() == at::kCUDA) {
    work = std::make_shared<AsyncReduceCUDAWork>(
        std::move(context),
        inputs,
        opts.rootRank,
        opts.rootTensor,
        opts.reduceOp,
        tag);
#endif
  } else {
    throw std::runtime_error("Invalid backend");
  }
  enqueue(work);
  return work;
}

namespace {

class AsyncAllgatherWork : public ProcessGroupGloo::AsyncWork {
 public:
  AsyncAllgatherWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<std::vector<at::Tensor>>& outputs,
      std::vector<at::Tensor>& inputs,
      uint32_t tag)
      : context(context), outputs(outputs), inputs(inputs), tag(tag) {}

  std::shared_ptr<gloo::Context> context;
  std::vector<std::vector<at::Tensor>> outputs;
  std::vector<at::Tensor> inputs;
  const uint32_t tag;

  void allgather(
      std::vector<std::vector<at::Tensor>>& outputs,
      std::vector<at::Tensor>& inputs) {
    const auto& scalarType = inputs[0].scalar_type();
    gloo::AllgatherOptions opts(context);
    opts.setTag(tag);

    // Use single flattened input tensor.
    at::Tensor flatInputTensor = flattenDenseTensors(inputs);
    GENERATE_ALL_TYPES(scalarType, setInput, opts, flatInputTensor);

    // Use single flat output tensor.
    // The first dimension corresponds to the index into outputs[N],
    // so copying into the actual output later is easy.
    at::Tensor flatOutputTensor = newLikeFlat(outputs[0]);
    GENERATE_ALL_TYPES(scalarType, setOutput, opts, flatOutputTensor);
    gloo::allgather(opts);

    // Unflatten into output tensors.
    for (size_t i = 0; i < outputs.size(); i++) {
      for (size_t j = 0; j < outputs[i].size(); j++) {
        outputs[i][j].copy_(flatOutputTensor[j]);
      }
    }
  }

  void run() override {
    allgather(outputs, inputs);
  }
};

#ifdef USE_CUDA

// Note: current CUDA implementation holds the assumption that the
// tensors in the nested output tensor vectors are on the same device.
class AsyncAllgatherCUDAWork : public AsyncAllgatherWork {
 public:
  AsyncAllgatherCUDAWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<std::vector<at::Tensor>>& outputs,
      std::vector<at::Tensor>& inputs,
      uint32_t tag)
      : AsyncAllgatherWork(context, outputs, inputs, tag) {
    initializeStreamsEvents(inputs, inputStreams, inputEvents);
    initializeStreamsEvents(outputs, outputStreams, outputEvents);

    // Kick off copy from CUDA tensors to pinned CPU tensors.
    tmpInputs.reserve(inputs.size());
    at::cuda::OptionalCUDAStreamGuard guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      guard.reset_stream(inputStreams[i]);
      tmpInputs.push_back(pinnedLike(inputs[i]).copy_(inputs[i], true));
    }

    tmpOutputs.resize(outputs.size());
    for (size_t i = 0; i < outputs.size(); i++) {
      tmpOutputs[i].reserve(outputs[i].size());
      for (size_t j = 0; j < outputs[i].size(); j++) {
        tmpOutputs[i].push_back(pinnedLike(outputs[i][j]));
      }
    }
  }

  void run() override {
    // Synchronize with copy operations.
    at::cuda::OptionalCUDAGuard device_guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      device_guard.set_index(inputs[i].device().index());
      AT_CUDA_CHECK(cudaStreamSynchronize(inputStreams[i]));
    }

    for (size_t i = 0; i < outputs.size(); i++) {
      device_guard.set_index(outputs[i][0].device().index());
      AT_CUDA_CHECK(cudaStreamSynchronize(outputStreams[i]));
    }

    // Run allgather on host side tensors.
    allgather(tmpOutputs, tmpInputs);

    // Kick off copy back to the CUDA tensors.
    at::cuda::OptionalCUDAStreamGuard stream_guard;
    for (size_t i = 0; i < outputs.size(); i++) {
      stream_guard.reset_stream(outputStreams[i]);
      for (size_t j = 0; j < outputs[i].size(); j++) {
        outputs[i][j].copy_(tmpOutputs[i][j], /* non_blocking */ true);
      }
      outputEvents[i].record(outputStreams[i]);
    }
  }

  void synchronize() override {
    // Synchronize with the copy back to CUDA tensors.
    at::cuda::OptionalCUDAGuard guard;
    for (size_t i = 0; i < outputs.size(); i++) {
      guard.set_index(outputs[i][0].device().index());
      outputEvents[i].block(at::cuda::getCurrentCUDAStream());
    }
  }

  std::vector<at::Tensor> tmpInputs;
  std::vector<at::cuda::CUDAStream> inputStreams;
  std::vector<at::cuda::CUDAEvent> inputEvents;

  std::vector<std::vector<at::Tensor>> tmpOutputs;
  std::vector<at::cuda::CUDAStream> outputStreams;
  std::vector<at::cuda::CUDAEvent> outputEvents;
};

#endif

} // namespace

// Note: current CUDA implementation holds the assumption that the
// tensors in the nested output tensor vectors are on the same device.
std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::allgather(
    std::vector<std::vector<at::Tensor>>& outputs,
    std::vector<at::Tensor>& inputs,
    const AllgatherOptions& opts) {
  static auto invalidArgument = [](const std::string& msg) {
    throw std::invalid_argument("ProcessGroupGloo::allgather: " + msg);
  };

  if (inputs.size() == 0) {
    invalidArgument("requires non-empty input tensor list");
  }

  if (inputs.size() != outputs.size()) {
    invalidArgument(
        "requires input/output tensor lists to have the same length");
  }

  for (size_t i = 0; i < outputs.size(); i++) {
    const auto expected = inputs.size() * getSize();
    const auto actual = outputs[i].size();
    if (actual != expected) {
      invalidArgument(
          "invalid output tensor list at index " + std::to_string(i) +
          " (expected length " + std::to_string(expected) + ", got " +
          std::to_string(actual) + ")");
    }
  }

  assertDense(invalidArgument, inputs);

  // Expect all input/output tensors to have the same type and sizes
  const auto& type = inputs[0].type();
  const auto& sizes = inputs[0].sizes();
  assertTypeAndSizesMatch(invalidArgument, inputs, type, sizes);
  for (size_t i = 0; i < outputs.size(); i++) {
    assertTypeAndSizesMatch(invalidArgument, outputs[i], type, sizes);
  }

  const auto& device = inputs[0].device();
  switch (device.type()) {
    case at::kCPU:
#ifdef USE_CUDA
    case at::kCUDA:
#endif
      break;
    default:
      invalidArgument("unsupported device type");
  }

  std::shared_ptr<AsyncAllgatherWork> work;
  auto tag = nextTag();
  auto context = getContext(tag);
  if (device.type() == at::kCPU) {
    work = std::make_shared<AsyncAllgatherWork>(
        std::move(context), outputs, inputs, tag);
#ifdef USE_CUDA
  } else if (device.type() == at::kCUDA) {
    work = std::make_shared<AsyncAllgatherCUDAWork>(
        std::move(context), outputs, inputs, tag);
#endif
  } else {
    throw std::runtime_error("Invalid backend");
  }
  enqueue(work);
  return work;
}

namespace {

class AsyncGatherWork : public ProcessGroupGloo::AsyncWork {
 public:
  AsyncGatherWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<std::vector<at::Tensor>>& outputs,
      std::vector<at::Tensor>& inputs,
      int root,
      uint32_t tag)
      : context(context),
        outputs(outputs),
        inputs(inputs),
        root(root),
        tag(tag) {}

  std::shared_ptr<gloo::Context> context;
  std::vector<std::vector<at::Tensor>> outputs;
  std::vector<at::Tensor> inputs;
  const int root;
  const uint32_t tag;

  void gather(
      std::vector<std::vector<at::Tensor>>& outputs,
      std::vector<at::Tensor>& inputs) {
    const auto scalarType = inputs[0].scalar_type();
    gloo::GatherOptions opts(context);
    opts.setRoot(root);
    opts.setTag(tag);

    // Set single temporary tensor on root process.
    // This is later scattered to the separate output tensors.
    at::Tensor flatOutputTensor;
    if (context->rank == root) {
      flatOutputTensor = newLikeFlat(outputs[0]);
      GENERATE_ALL_TYPES(scalarType, setOutput, opts, flatOutputTensor);
    }

    // Set single input tensor on all processes.
    GENERATE_ALL_TYPES(scalarType, setInput, opts, inputs[0]);
    gloo::gather(opts);

    // Unflatten into output tensors on root process.
    if (context->rank == root) {
      for (size_t i = 0; i < outputs[0].size(); i++) {
        outputs[0][i].copy_(flatOutputTensor[i]);
      }
    }
  }

  void run() override {
    gather(outputs, inputs);
  }
};

#ifdef USE_CUDA

// Note: current CUDA implementation holds the assumptions:
//     - inputs.size() is 1
//     - outputs.size() is 1
//     - the size of the nested output tensors is world size, i.e.,
//       outputs[0].size, is world size
class AsyncGatherCUDAWork : public AsyncGatherWork {
 public:
  AsyncGatherCUDAWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<std::vector<at::Tensor>>& outputs,
      std::vector<at::Tensor>& inputs,
      int root,
      uint32_t tag)
      : AsyncGatherWork(context, outputs, inputs, root, tag) {
    initializeStreamsEvents(inputs, inputStreams, inputEvents);
    initializeStreamsEvents(outputs, outputStreams, outputEvents);

    // Kick off copy from CUDA tensors to pinned CPU tensors.
    tmpInputs.reserve(inputs.size());
    at::cuda::OptionalCUDAStreamGuard guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      guard.reset_stream(inputStreams[i]);
      tmpInputs.push_back(pinnedLike(inputs[i]).copy_(inputs[i], true));
    }

    tmpOutputs.resize(outputs.size());
    for (size_t i = 0; i < outputs.size(); i++) {
      tmpOutputs[i].reserve(outputs[i].size());
      for (size_t j = 0; j < outputs[i].size(); j++) {
        tmpOutputs[i].push_back(pinnedLike(outputs[i][j]));
      }
    }
  }

  void run() override {
    // Synchronize with copy operations.
    at::cuda::OptionalCUDAGuard device_guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      device_guard.set_index(inputs[i].get_device());
      AT_CUDA_CHECK(cudaStreamSynchronize(inputStreams[i]));
    }

    for (size_t i = 0; i < outputs.size(); i++) {
      device_guard.set_index(outputs[i][0].get_device());
      AT_CUDA_CHECK(cudaStreamSynchronize(outputStreams[i]));
    }

    // Run gather on host side tensors.
    gather(tmpOutputs, tmpInputs);

    // Kick off copy back to the CUDA tensors.
    at::cuda::OptionalCUDAStreamGuard stream_guard;
    for (size_t i = 0; i < outputs.size(); i++) {
      stream_guard.reset_stream(outputStreams[i]);
      for (size_t j = 0; j < outputs[i].size(); j++) {
        outputs[i][j].copy_(tmpOutputs[i][j], /* non_blocking */ true);
      }
      outputEvents[i].record(outputStreams[i]);
    }
  }

  void synchronize() override {
    // Synchronize with the copy back to CUDA tensors.
    at::cuda::OptionalCUDAGuard guard;
    for (size_t i = 0; i < outputs.size(); i++) {
      guard.set_index(static_cast<at::DeviceIndex>(outputs[i][0].get_device()));
      outputEvents[i].block(at::cuda::getCurrentCUDAStream());
    }
  }

  std::vector<at::Tensor> tmpInputs;
  std::vector<at::cuda::CUDAStream> inputStreams;
  std::vector<at::cuda::CUDAEvent> inputEvents;

  std::vector<std::vector<at::Tensor>> tmpOutputs;
  std::vector<at::cuda::CUDAStream> outputStreams;
  std::vector<at::cuda::CUDAEvent> outputEvents;
};

#endif

} // namespace

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::gather(
    std::vector<std::vector<at::Tensor>>& outputs,
    std::vector<at::Tensor>& inputs,
    const GatherOptions& opts) {
  static auto invalidArgument = [](const std::string& msg) {
    throw std::invalid_argument("ProcessGroupGloo::gather: " + msg);
  };

  assertRootRank(invalidArgument, opts.rootRank, size_);
  assertSingleElementInput(invalidArgument, inputs);
  assertDense(invalidArgument, inputs);

  if (getRank() == opts.rootRank) {
    if (outputs.size() != 1 ||
        outputs[0].size() != static_cast<size_t>(getSize())) {
      invalidArgument(
          "requires a single-element output list "
          "containing a list with <size> tensors");
    }

    const auto& type = inputs[0].type();
    const auto& sizes = inputs[0].sizes();
    assertTypeAndSizesMatch(invalidArgument, outputs[0], type, sizes);
  } else {
    if (outputs.size() != 0) {
      invalidArgument("requires empty output on non-root");
    }
  }

  const auto& device = inputs[0].device();
  switch (device.type()) {
    case at::kCPU:
#ifdef USE_CUDA
    case at::kCUDA:
#endif
      break;
    default:
      invalidArgument("unsupported device type");
  }

  std::shared_ptr<AsyncGatherWork> work;
  auto tag = nextTag();
  auto context = getContext(tag);
  if (device.type() == at::kCPU) {
    work = std::make_shared<AsyncGatherWork>(
        std::move(context), outputs, inputs, opts.rootRank, tag);
#ifdef USE_CUDA
  } else if (device.type() == at::kCUDA) {
    work = std::make_shared<AsyncGatherCUDAWork>(
        std::move(context), outputs, inputs, opts.rootRank, tag);
#endif
  } else {
    throw std::runtime_error("Invalid backend");
  }
  enqueue(work);
  return work;
}

namespace {

class AsyncScatterWork : public ProcessGroupGloo::AsyncWork {
 public:
  AsyncScatterWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<at::Tensor>& outputs,
      std::vector<std::vector<at::Tensor>>& inputs,
      int root,
      uint32_t tag)
      : context(context),
        outputs(outputs),
        inputs(inputs),
        root(root),
        tag(tag) {}

  std::shared_ptr<gloo::Context> context;
  std::vector<at::Tensor> outputs;
  std::vector<std::vector<at::Tensor>> inputs;
  const int root;
  const uint32_t tag;

  void scatter(
      std::vector<at::Tensor>& outputs,
      std::vector<std::vector<at::Tensor>>& inputs) {
    const auto scalarType = outputs[0].scalar_type();
    gloo::ScatterOptions opts(context);
    opts.setRoot(root);
    opts.setTag(tag);

    // Set list of input tensors on root process
    if (context->rank == root) {
      GENERATE_ALL_TYPES(scalarType, setInputs, opts, inputs[0]);
    }

    // Set single output tensor on all processes
    GENERATE_ALL_TYPES(scalarType, setOutput, opts, outputs[0]);
    gloo::scatter(opts);
  }

  void run() override {
    scatter(outputs, inputs);
  }
};

#ifdef USE_CUDA

class AsyncScatterCUDAWork : public AsyncScatterWork {
 public:
  AsyncScatterCUDAWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<at::Tensor>& outputs,
      std::vector<std::vector<at::Tensor>>& inputs,
      int root,
      uint32_t tag)
      : AsyncScatterWork(context, outputs, inputs, root, tag) {
    initializeStreamsEvents(inputs, inputStreams, inputEvents);
    initializeStreamsEvents(outputs, outputStreams, outputEvents);

    // Kick off copy from CUDA tensors to pinned CPU tensors.
    tmpInputs.resize(inputs.size());
    at::cuda::OptionalCUDAStreamGuard guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      guard.reset_stream(inputStreams[i]);
      tmpInputs[i].reserve(inputs[i].size());
      for (size_t j = 0; j < inputs[i].size(); j++) {
        tmpInputs[i].push_back(
            pinnedLike(inputs[i][j]).copy_(inputs[i][j], true));
      }
    }

    tmpOutputs.reserve(outputs.size());
    for (size_t i = 0; i < outputs.size(); i++) {
      tmpOutputs.push_back(pinnedLike(outputs[i]));
    }
  }

  void run() override {
    // Synchronize with copy operations.
    at::cuda::OptionalCUDAGuard device_guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      device_guard.set_index(inputs[i][0].get_device());
      AT_CUDA_CHECK(cudaStreamSynchronize(inputStreams[i]));
    }
    for (size_t i = 0; i < outputs.size(); i++) {
      device_guard.set_index(outputs[i].get_device());
      AT_CUDA_CHECK(cudaStreamSynchronize(outputStreams[i]));
    }

    // Run scatter on host side tensors.
    scatter(tmpOutputs, tmpInputs);

    // Kick off copy back to the CUDA tensors.
    at::cuda::OptionalCUDAStreamGuard stream_guard;
    for (size_t i = 0; i < outputs.size(); i++) {
      stream_guard.reset_stream(outputStreams[i]);
      outputs[i].copy_(tmpOutputs[i], /* non_blocking */ true);
      outputEvents[i].record(outputStreams[i]);
    }
  }

  void synchronize() override {
    // Synchronize with the copy back to CUDA tensors.
    at::cuda::OptionalCUDAGuard guard;
    for (size_t i = 0; i < outputs.size(); i++) {
      guard.set_index(static_cast<at::DeviceIndex>(outputs[i].get_device()));
      outputEvents[i].block(at::cuda::getCurrentCUDAStream());
    }
  }

  std::vector<at::Tensor> tmpOutputs;
  std::vector<at::cuda::CUDAStream> outputStreams;
  std::vector<at::cuda::CUDAEvent> outputEvents;

  std::vector<std::vector<at::Tensor>> tmpInputs;
  std::vector<at::cuda::CUDAStream> inputStreams;
  std::vector<at::cuda::CUDAEvent> inputEvents;
};

#endif

} // namespace

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::scatter(
    std::vector<at::Tensor>& outputs,
    std::vector<std::vector<at::Tensor>>& inputs,
    const ScatterOptions& opts) {
  static auto invalidArgument = [](const std::string& msg) {
    throw std::invalid_argument("ProcessGroupGloo::scatter: " + msg);
  };

  assertRootRank(invalidArgument, opts.rootRank, size_);
  assertSingleElementOutput(invalidArgument, outputs);
  assertDense(invalidArgument, outputs);

  if (getRank() == opts.rootRank) {
    if (inputs.size() != 1 ||
        inputs[0].size() != static_cast<size_t>(getSize())) {
      invalidArgument(
          "requires a single-element input list "
          "containing a list with <size> tensors");
    }
    const auto& type = outputs[0].type();
    const auto& sizes = outputs[0].sizes();
    assertTypeAndSizesMatch(invalidArgument, inputs[0], type, sizes);
  } else {
    if (inputs.size() != 0) {
      invalidArgument("requires empty input on non-root");
    }
  }

  const auto& device = outputs[0].device();
  switch (device.type()) {
    case at::kCPU:
#ifdef USE_CUDA
    case at::kCUDA:
#endif
      break;
    default:
      invalidArgument("unsupported device type");
  }

  std::shared_ptr<AsyncScatterWork> work;
  auto tag = nextTag();
  auto context = getContext(tag);
  if (device.type() == at::kCPU) {
    work = std::make_shared<AsyncScatterWork>(
        std::move(context), outputs, inputs, opts.rootRank, tag);
#ifdef USE_CUDA
  } else if (device.type() == at::kCUDA) {
    work = std::make_shared<AsyncScatterCUDAWork>(
        std::move(context), outputs, inputs, opts.rootRank, tag);
#endif
  } else {
    throw std::runtime_error("Invalid backend");
  }
  enqueue(work);
  return work;
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::reduce_scatter(
    std::vector<at::Tensor>& outputs,
    std::vector<std::vector<at::Tensor>>& inputs,
    const ReduceScatterOptions& opts) {
  throw std::runtime_error("ProcessGroupGloo does not support reduce_scatter");
}

at::Tensor& checkSingleTensor(std::vector<at::Tensor>& tensors) {
  if (tensors.size() != 1) {
    throw std::runtime_error("ProcessGroupGloo::send takes a single tensor");
  }
  auto& tensor = tensors[0];
  if (!tensor.is_contiguous()) {
    throw std::runtime_error("input tensor has to be contiguous");
  }
  if (tensor.is_sparse()) {
    throw std::runtime_error("input tensor has to be dense");
  }
  return tensor;
}

uint32_t checkTag(int32_t tag) {
  if (tag < 0) {
    throw std::runtime_error("Tag must be >= 0");
  }
  return (uint32_t)tag;
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::send(
    std::vector<at::Tensor>& tensors,
    int dstRank,
    int tag) {
  auto& tensor = checkSingleTensor(tensors);
  auto utag = checkTag(tag);
  auto ptr = tensor.data_ptr();
  auto size = tensor.numel() * tensor.element_size();

  // Construct unbound buffer.
  auto context = getContext(tag);
  auto buf = context->createUnboundBuffer(ptr, size);
  buf->send(dstRank, utag);

  // The work captures the tensor to prevent it being deallocated and
  // the unbound buffer to synchronize on completion of the send.
  return std::make_shared<SendWork>(tensor, std::move(buf));
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::recv(
    std::vector<at::Tensor>& tensors,
    int srcRank,
    int tag) {
  auto& tensor = checkSingleTensor(tensors);
  auto utag = checkTag(tag);
  auto ptr = tensor.data_ptr();
  auto size = tensor.numel() * tensor.element_size();

  // Construct unbound buffer.
  auto context = getContext(tag);
  auto buf = context->createUnboundBuffer(ptr, size);
  buf->recv(srcRank, utag);

  // The work captures the tensor to prevent it being deallocated and
  // the unbound buffer to synchronize on completion of the recv.
  return std::make_shared<RecvWork>(tensor, std::move(buf));
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::recvAnysource(
    std::vector<at::Tensor>& tensors,
    int tag) {
  auto& tensor = checkSingleTensor(tensors);
  auto utag = checkTag(tag);
  auto ptr = tensor.data_ptr();
  auto size = tensor.numel() * tensor.element_size();

  // Construct unbound buffer.
  auto context = getContext(tag);
  auto buf = context->createUnboundBuffer(ptr, size);

  // Build list of ranks that this operation can recv from. In these
  // bindings we don't differentiate between ranks and can receive
  // from any other process in the group.
  std::vector<int> srcRanks;
  srcRanks.resize(size_);
  for (auto i = 0; i < size_; i++) {
    srcRanks.push_back(i);
  }

  buf->recv(srcRanks, utag);

  // The work captures the tensor to prevent it being deallocated and
  // the unbound buffer to synchronize on completion of the recv.
  return std::make_shared<RecvWork>(tensor, std::move(buf));
}

namespace {

class AsyncBarrierWork : public ProcessGroupGloo::AsyncWork {
 public:
  AsyncBarrierWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<std::weak_ptr<AsyncWork>> priorWork,
      uint32_t tag)
      : context(context), priorWork(std::move(priorWork)), tag(tag) {}

  std::shared_ptr<gloo::Context> context;
  std::vector<std::weak_ptr<AsyncWork>> priorWork;
  const uint32_t tag;

  void run() override {
    // Wait on prior work to complete
    for (auto& weakWork : priorWork) {
      auto work = weakWork.lock();
      if (work) {
        work->wait();
      }
    }

    gloo::BarrierOptions opts(context);
    opts.setTag(tag);
    gloo::barrier(opts);
  }
};

} // namespace

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::barrier(
    const BarrierOptions& opts) {
  std::vector<std::weak_ptr<AsyncWork>> priorWork;

  // Snapshot all in progress and pending work as weak_ptr.
  // When executing a barrier, we need to ensure that all prior work
  // has completed before completing itself.
  {
    std::unique_lock<std::mutex> lock(workMutex_);
    priorWork.insert(
        priorWork.end(), workInProgress_.begin(), workInProgress_.end());
    priorWork.insert(priorWork.end(), workQueue_.begin(), workQueue_.end());
  }

  auto tag = nextTag();
  auto context = getContext(tag);
  auto work = std::make_shared<AsyncBarrierWork>(
      std::move(context), std::move(priorWork), tag);
  enqueue(work);
  return work;
}

} // namespace c10d
