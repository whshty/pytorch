#!/usr/bin/env python3
from __future__ import absolute_import, division, print_function, unicode_literals

import sys
import unittest

import torch
import torch.distributed as dist
from common_distributed import MultiProcessTestCase
from common_utils import load_tests, run_tests


# it is used to test python user defined function over rpc
def my_function(a, b, c):
    return a + b + c


# it is used to test python user defined function over rpc
def no_result():
    print("do nothing")


def nested_rpc(dst):
    return dist.rpc(dst, torch.add, args=(torch.ones(2, 2), 1))


def light_rpc():
    return 0


def heavy_rpc(tensor):
    for i in range(1, 100):
        tensor *= i
        tensor /= i + 1
    return 0


# it is used to test python user defined function over rpc
def raise_func():
    raise ValueError("Expected error")


# it is used to test python user defined class and methods over rpc
class my_class:
    def __init__(self, a):
        self.a = a

    def my_instance_method(self, b):
        return self.a + b

    @classmethod
    def my_class_method(cls, d, e):
        return d + e

    @staticmethod
    def my_static_method(f):
        return f > 10


# load_tests from common_utils is used to automatically filter tests for
# sharding on sandcastle. This line silences flake warnings
load_tests = load_tests


if not dist.is_available():
    print("c10d not available, skipping tests")
    sys.exit(0)


def _wrap_with_rpc(func):
    def wrapper(self):
        store = dist.FileStore(self.file.name, self.world_size)
        dist.init_process_group(backend='gloo', rank=self.rank,
                                world_size=self.world_size, store=store)
        dist.init_model_parallel('worker%d' % self.rank)
        func(self)
        dist.join_rpc()

    return wrapper


@unittest.skipIf(
    sys.version_info < (3, 0),
    "Pytorch distributed rpc package " "does not support python2",
)
class RpcTest(MultiProcessTestCase):
    @property
    def world_size(self):
        return 4

    @_wrap_with_rpc
    def test_add(self):
        n = self.rank + 1
        dst_rank = n % self.world_size
        ret = dist.rpc(
            "worker{}".format(dst_rank),
            torch.add,
            args=(torch.ones(n, n), torch.ones(n, n)),
        )
        self.assertEqual(ret, torch.ones(n, n) * 2)

    @_wrap_with_rpc
    def test_scalar_add(self):
        n = self.rank + 1
        dst_rank = n % self.world_size
        ret = dist.rpc(
            "worker{}".format(dst_rank), torch.add, args=(torch.ones(n, n), n)
        )
        self.assertEqual(ret, (torch.ones(n, n) + n))

    @_wrap_with_rpc
    def test_async_add(self):
        n = self.rank + 1
        dst_rank = n % self.world_size
        fut = dist.rpc(
            "worker{}".format(dst_rank),
            torch.add,
            args=(torch.ones(n, n), torch.ones(n, n)),
            async_call=True,
        )
        self.assertEqual(fut.wait(), torch.ones(n, n) * 2)

    @_wrap_with_rpc
    def test_nonzero(self):
        n = self.rank + 1
        dst_rank = n % self.world_size
        x = torch.ones(self.world_size, self.world_size)
        x[self.rank][self.rank] = 0
        ret = dist.rpc("worker{}".format(dst_rank), torch.nonzero, args=(x,))
        self.assertEqual(ret, x.nonzero())

    @_wrap_with_rpc
    def test_multi_rpc(self):
        dst_rank = (self.rank + 1) % self.world_size
        for i in range(20):
            n = i + self.rank + 1
            ret = dist.rpc(
                "worker{}".format(dst_rank),
                torch.add,
                args=(torch.ones(n, n), torch.ones(n, n)),
            )
            self.assertEqual(ret, torch.ones(n, n) * 2)

    @_wrap_with_rpc
    def test_sync_rpc(self):
        dst_rank = (self.rank + 1) % self.world_size
        for i in range(20):
            dist.sync_rpc()
            n = i + self.rank + 1
            ret1 = dist.rpc(
                "worker{}".format(dst_rank),
                torch.add,
                args=(torch.ones(n, n), torch.ones(n, n)),
            )
            dist.sync_rpc()
            ret2 = dist.rpc(
                "worker{}".format(dst_rank), torch.add, args=(torch.ones(n, n), 2)
            )
            dist.sync_rpc()
            self.assertEqual(ret1, torch.ones(n, n) * 2)
            self.assertEqual(ret2, torch.ones(n, n) * 3)

    @_wrap_with_rpc
    def test_join_rpc(self):
        n = self.rank + 1
        dst_rank = n % self.world_size
        ret = dist.rpc(
            "worker{}".format(dst_rank),
            torch.add,
            args=(torch.ones(n, n), torch.ones(n, n)),
        )
        self.assertEqual(ret, torch.ones(n, n) * 2)
        dist.join_rpc()

        with self.assertRaisesRegex(RuntimeError, "^RPC has not been initialized"):
            dist.rpc(
                "worker{}".format(dst_rank),
                torch.add,
                args=(torch.ones(n, n), torch.ones(n, n)),
            )

        # it's safe to call join_rpc() multiple times
        dist.join_rpc()

    @_wrap_with_rpc
    def test_py_built_in(self):
        n = self.rank + 1
        dst_rank = n % self.world_size
        ret = dist.rpc("worker{}".format(dst_rank), min, args=(n, n + 1, n + 2))
        self.assertEqual(ret, min(n, n + 1, n + 2))

    @_wrap_with_rpc
    def test_py_user_defined(self):
        n = self.rank + 1
        dst_rank = n % self.world_size
        ret = dist.rpc(
            "worker{}".format(dst_rank),
            my_function,
            kwargs={"a": n, "b": n + 1, "c": n + 2},
        )
        self.assertEqual(ret, my_function(n, n + 1, n + 2))

    @_wrap_with_rpc
    def test_py_class_constructor(self):
        n = self.rank + 1
        dst_rank = n % self.world_size
        ret = dist.rpc("worker{}".format(dst_rank), my_class, args=(n,))
        self.assertEqual(ret.a, n)

    @_wrap_with_rpc
    def test_py_class_instance_method(self):
        n = self.rank + 1
        dst_rank = n % self.world_size
        ret = dist.rpc(
            "worker{}".format(dst_rank), my_class(2).my_instance_method, args=(n,)
        )
        self.assertEqual(ret, my_class(2).my_instance_method(n))

    @_wrap_with_rpc
    def test_py_class_method(self):
        n = self.rank + 1
        dst_rank = n % self.world_size
        ret = dist.rpc(
            "worker{}".format(dst_rank), my_class.my_class_method, args=(n, n + 1)
        )
        self.assertEqual(ret, my_class.my_class_method(n, n + 1))

    @_wrap_with_rpc
    def test_py_class_static_method(self):
        n = self.rank + 1
        dst_rank = n % self.world_size
        ret = dist.rpc(
            "worker{}".format(dst_rank), my_class.my_static_method, args=(n + 10,)
        )
        self.assertEqual(ret, my_class.my_static_method(n + 10))

    @_wrap_with_rpc
    def test_py_multi_async_call(self):
        n = self.rank + 1
        dst_rank = n % self.world_size
        fut1 = dist.rpc(
            "worker{}".format(dst_rank),
            my_class.my_static_method,
            args=(n + 10,),
            async_call=True,
        )
        fut2 = dist.rpc(
            "worker{}".format(dst_rank), min, args=(n, n + 1, n + 2), async_call=True
        )
        self.assertEqual(fut1.wait(), my_class.my_static_method(n + 10))
        self.assertEqual(fut2.wait(), min(n, n + 1, n + 2))

    @_wrap_with_rpc
    def test_py_no_return_result(self):
        n = self.rank + 1
        dst_rank = n % self.world_size
        ret = dist.rpc("worker{}".format(dst_rank), no_result)
        self.assertEqual(ret, no_result())

    @_wrap_with_rpc
    def test_py_function_exception(self):
        n = self.rank + 1
        dst_rank = n % self.world_size
        with self.assertRaisesRegex(Exception, "TypeError"):
            ret = dist.rpc("worker{}".format(dst_rank), no_result, args=(10,))

    @_wrap_with_rpc
    def test_py_raise_in_user_func(self):
        n = self.rank + 1
        dst_rank = n % self.world_size
        fut = dist.rpc("worker{}".format(dst_rank), raise_func, async_call=True)
        with self.assertRaisesRegex(Exception, "ValueError"):
            fut.wait()

    @_wrap_with_rpc
    def test_nested_rpc(self):
        n = self.rank + 1
        dst_rank = n % self.world_size
        ret = dist.rpc(
            "worker{}".format(dst_rank),
            nested_rpc,
            args=("worker{}".format(self.rank),),
        )
        self.assertEqual(ret, torch.ones(2, 2) + 1)

    def _stress_test_rpc(self, f, repeat=1000, args=()):
        import time

        n = self.rank + 1
        dst_rank = n % self.world_size
        futs = []
        tik = time.time()
        for _ in range(repeat):
            fut = dist.rpc("worker{}".format(dst_rank), f, args=args, async_call=True)
            futs.append(fut)

        for fut in futs:
            self.assertEqual(fut.wait(), 0)
        tok = time.time()
        print(
            "Rank {} finished testing {} {} times in {} seconds.".format(
                self.rank, f.__name__, repeat, tok - tik
            )
        )

    @_wrap_with_rpc
    def test_stress_light_rpc(self):
        self._stress_test_rpc(light_rpc)

    @_wrap_with_rpc
    def test_stress_heavy_rpc(self):
        self._stress_test_rpc(heavy_rpc, repeat=20, args=(torch.ones(100, 100),))


if __name__ == "__main__":
    run_tests()
