import ctypes
import logging
import threading
import time

import torch

from ._meta_parser import (
    OpenRegTensorData,
    receive_after_sending,
    safe_str,
    validate_send_queue_args,
)


log = logging.getLogger(__name__)
mp_context = torch.multiprocessing.get_context("spawn")

# Constant properties of our device
NUM_DEVICES = 2


# Our allocator
class Allocator:
    def __init__(self):
        self.allocated = {}

    def malloc(self, size):
        mem = ctypes.create_string_buffer(size)
        ptr = ctypes.addressof(mem)
        self.allocated[ptr] = (size, mem)
        return ptr

    def free(self, ptr):
        if ptr not in self.allocated:
            return False
        else:
            del self.allocated[ptr]
            return True


class HostAllocator(Allocator):
    def is_pinned_ptr(self, ptr):
        return ptr in self.allocated or any(
            ptr_ <= ptr and ptr < ptr_ + size
            for ptr_, (size, _) in self.allocated.items()
        )


class DeviceAllocator(Allocator):
    def tensor_from_meta(self, meta):
        def create_tensor_from_data_ptr(ptr, size):
            storage = torch._C._construct_storage_from_data_pointer(
                ptr, torch.device("cpu"), size
            )
            return torch.Tensor(storage)

        found_base = None
        # Usual case, we're receiving a known Tensor
        if meta.data_ptr in self.allocated:
            found_base = create_tensor_from_data_ptr(
                meta.data_ptr, self.allocated[meta.data_ptr][0]
            )

        # Might be a rewrap of another storage at a different offset
        # Slow path to try and find the corresponding storage
        if found_base is None:
            for tag, (size, _) in self.allocated.items():
                # t is always a 1D uint8 storage!
                if meta.data_ptr > tag and meta.data_ptr < tag + size:
                    # Blame @ngimel for this
                    slice_size = size - (meta.data_ptr - tag)
                    found_base = create_tensor_from_data_ptr(meta.data_ptr, slice_size)

        # Might be an empty tensor
        if found_base is None and meta.nelem_in_bytes == 0:
            found_base = torch.tensor((), dtype=torch.uint8)

        # This pointer is not allocated here, segfault !
        if found_base is None:
            log.info("Currently allocated blocks:\n %s", safe_str(self.allocated))
            log.info("Trying to access %s", meta)
            raise RuntimeError("SEGFAULT!")

        # Raw 1d uint8 data
        raw = found_base
        # Reinterpret cast in the right dtype
        as_dtype = raw.view(dtype=meta.dtype)
        # View to the right shape/stride/offset
        view = as_dtype.as_strided(meta.size, meta.stride, meta.storage_offset)
        return view


def register(registry):
    def func(fn):
        registry[fn.__name__] = fn
        return fn

    return func


class Driver:
    def __init__(self, num_devices):
        super().__init__()
        self.num_devices = num_devices
        self.is_initialized = False
        self.rlock = threading.RLock()

    def _lazy_init(self):
        if self.is_initialized:
            return

        # State of our driver
        self.curr_device_idx = 0
        self.curr_streams = {}

        # Allocated memory belongs to which device
        self.memory_belong = {}
        self.host_allocator = HostAllocator()
        self.event_belong = {}

        self.devices = []

        for i in range(self.num_devices):
            req_queue = mp_context.Queue()
            ans_queue = mp_context.Queue()
            runner = mp_context.Process(
                target=_Executor(i).run_forever,
                args=(req_queue, ans_queue),
                daemon=True,
            )
            runner.start()
            self.devices.append((req_queue, ans_queue, runner))

        self.is_initialized = True

    def exec(self, cmd, *args):
        with self.rlock:
            self._lazy_init()
            log.info("Main process launched: %s(*%s)", cmd, safe_str(args))

            if cmd in Driver.registry:
                res = Driver.registry[cmd](self, *args)
            else:
                res = self.run_on_executor(self.curr_device_idx, cmd, *args)

            log.info("Main process result for %s received: %s", cmd, safe_str(res))
            if res == "ERROR":
                raise RuntimeError(f"Error in daemon while executing {cmd}, see logs")
            else:
                return res

    def run_on_executor(self, device_idx, cmd, *args):
        req_queue, ans_queue, _ = self.devices[device_idx]
        stream = self.getStream(device_idx)
        validate_send_queue_args(cmd, args)
        req_queue.put((stream, cmd) + args)
        return ans_queue.get()

    registry = {}

    @register(registry)
    def hasPrimaryContext(self, device_idx):
        return device_idx >= 0 and device_idx < len(self.devices)

    @register(registry)
    def deviceCount(self, *args):
        assert len(args) == 0
        return self.num_devices

    @register(registry)
    def getDevice(self):
        return self.curr_device_idx

    @register(registry)
    def setDevice(self, device_idx):
        assert device_idx >= 0 and device_idx < self.num_devices
        self.curr_device_idx = device_idx

    @register(registry)
    def uncheckedSetDevice(self, *args):
        assert len(args) == 1
        self.curr_device_idx = int(args[0])

    @register(registry)
    def exchangeDevice(self, *args):
        assert len(args) == 1
        res = self.curr_device_idx
        self.curr_device_idx = int(args[0])
        return res

    @register(registry)
    def malloc(self, size):
        ptr = self.run_on_executor(self.curr_device_idx, "malloc", size)
        self.memory_belong[ptr] = self.curr_device_idx
        return ptr

    @register(registry)
    def free(self, ptr):
        device_idx = self.memory_belong.pop(ptr, None)
        if device_idx is None:
            return False
        return self.run_on_executor(device_idx, "free", ptr)

    @register(registry)
    def isPinnedPtr(self, ptr):
        return self.host_allocator.is_pinned_ptr(ptr)

    @register(registry)
    def hostMalloc(self, size):
        return self.host_allocator.malloc(size)

    @register(registry)
    def hostFree(self, ptr):
        return self.host_allocator.free(ptr)

    @register(registry)
    def getNewStream(self, device_idx, priority):
        return self.run_on_executor(device_idx, "getNewStream", priority)

    @register(registry)
    def queryStream(self, stream):
        return self.run_on_executor(
            stream.device_index, "queryStream", stream.stream_id
        )

    @register(registry)
    def getStream(self, device_idx):
        return self.curr_streams.get(device_idx, 0)

    @register(registry)
    def exchangeStream(self, stream):
        stream_id = self.curr_streams.get(stream.device_index, 0)
        self.curr_streams[stream.device_index] = stream.stream_id
        return stream_id

    @register(registry)
    def synchronizeStream(self, stream):
        self.run_on_executor(stream.device_index, "synchronizeStream", stream.stream_id)

    @register(registry)
    def record(self, event, stream, device_index, flags):
        event_ptr = ctypes.cast(event, ctypes.POINTER(ctypes.c_int64))
        # Create event if needed
        if event_ptr.contents.value == 0:
            event_ptr.contents.value = self.run_on_executor(
                stream.device_index, "eventCreateWithFlags", flags
            )
            self.event_belong[event_ptr.contents.value] = stream.device_index

        # Record event
        self.run_on_executor(
            stream.device_index,
            "eventRecord",
            event_ptr.contents.value,
            stream.stream_id,
        )

    @register(registry)
    def destroyEvent(self, event, device_index):
        self.run_on_executor(device_index, "eventDestroy", event)
        self.event_belong.pop(event)

    @register(registry)
    def synchronizeEvent(self, event):
        self.run_on_executor(self.event_belong[event], "eventSynchronize", event)

    @register(registry)
    def queryEvent(self, event):
        return self.run_on_executor(self.event_belong[event], "eventQuery", event)

    @register(registry)
    def elapsedTime(self, e1, e2, device_index):
        return self.run_on_executor(device_index, "eventElapsedTime", e1, e2)

    @register(registry)
    def block(self, event, stream):
        self.run_on_executor(stream.device_index, "block", event, stream.stream_id)


class _Executor:
    def __init__(self, id):
        self.id = id
        self.allocator = DeviceAllocator()
        self.stream = 0
        self.event_incr_id = 0
        self.events = {}

    def run_forever(self, req_queue, ans_queue):
        # Serve all requests
        while True:
            # Ignore stream since cpu backend doesn't support asynchronous execution
            _, cmd, *args = req_queue.get()
            log.info("Worker executing: %s", cmd)
            if cmd in _Executor.registry:
                res = _Executor.registry[cmd](self, *args)
            else:
                log.warning("Bad command in worker")
                res = "ERROR"

            log.info("Worker answering to: %s", cmd)
            ans_queue.put(res)

    registry = {}

    @register(registry)
    def malloc(self, size):
        return self.allocator.malloc(size)

    @register(registry)
    def free(self, ptr):
        return self.allocator.free(ptr)

    def _run_op(self, op_name, args, kwargs):
        op, _ = torch._C._jit_get_operation(op_name)
        args, kwargs = receive_after_sending(self.allocator, args, kwargs)
        return op(*args, **kwargs)

    @register(registry)
    def run_op(self, op_name, args, kwargs):
        self._run_op(op_name, args, kwargs)

    @register(registry)
    def get_op_output_shape(self, op_name, args, kwargs):
        return self._run_op(op_name, args, kwargs).size()

    @register(registry)
    def send_data(self, *args):
        assert len(args) == 1
        return OpenRegTensorData.from_meta(self.allocator, args[0])

    @register(registry)
    def recv_data(self, host_tensor, dev_mem):
        dev_tensor = OpenRegTensorData.from_meta(self.allocator, dev_mem)
        dev_tensor.copy_(host_tensor)

    @register(registry)
    def getNewStream(self, priority):
        self.stream += 1
        return self.stream

    @register(registry)
    def queryStream(self, stream):
        return True

    @register(registry)
    def synchronizeStream(self, stream):
        # no-op
        pass

    @register(registry)
    def eventCreateWithFlags(self, flags):
        self.event_incr_id += 1
        self.events[self.event_incr_id] = [flags, None]
        return self.event_incr_id

    @register(registry)
    def eventRecord(self, event, stream):
        # Only flags == 1 enables timing
        if self.events[event][0] == 1:
            self.events[event][1] = time.time() * 1000
        return 0

    @register(registry)
    def eventDestroy(self, event):
        self.events.pop(event)

    @register(registry)
    def eventSynchronize(self, event):
        assert self.events.get(event) is not None
        return 0

    @register(registry)
    def eventQuery(self, event):
        assert self.events.get(event) is not None
        return True

    @register(registry)
    def eventElapsedTime(self, e1, e2):
        time_1 = self.events[e1][1]
        time_2 = self.events[e2][1]
        assert time_1 is not None and time_2 is not None
        return time_2 - time_1

    @register(registry)
    def block(self, event, stream):
        # no-op
        pass


driver = Driver(NUM_DEVICES)
