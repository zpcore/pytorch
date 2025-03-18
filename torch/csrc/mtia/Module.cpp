#include <ATen/ATen.h>
#include <c10/core/DeviceType.h>
#include <c10/core/Stream.h>
#include <torch/csrc/Generator.h>
#include <torch/csrc/Stream.h>
#include <torch/csrc/mtia/Module.h>
#include <torch/csrc/python_headers.h>
#include <torch/csrc/utils/device_lazy_init.h>
#include <torch/csrc/utils/pybind.h>
#ifndef WIN32
#include <pthread.h>
#endif

namespace torch::mtia {

static bool in_bad_fork = false; // True for children forked after mtia init

#ifndef WIN32
// Called in the forked child if mtia has already been initialized
static void forked_child() {
  in_bad_fork = true;
  torch::utils::set_requires_device_init(at::kMTIA, true);
}
#endif

// Should be called before the first mtia call.
// Note: This is distinct from initExtension because a stub mtia implementation
// has some working functions (e.g. device_count) but cannot fully initialize.
static void poison_fork() {
#ifndef WIN32
  static auto result [[maybe_unused]] =
      pthread_atfork(nullptr, nullptr, forked_child);
#endif
}

void initModule(PyObject* module) {
  auto m = py::handle(module).cast<py::module>();

  m.def("_mtia_init", []() {
    TORCH_INTERNAL_ASSERT(!in_bad_fork); // Handled at python level
    poison_fork();
    at::globalContext().lazyInitDevice(c10::DeviceType::MTIA);
  });

  m.def("_mtia_isBuilt", []() {
    // Check if the MTIAHooks class has been registered with the registry.
    return at::detail::isMTIAHooksBuilt();
  });

  m.def("_mtia_isInBadFork", []() { return in_bad_fork; });

  m.def("_mtia_getCurrentStream", [](c10::DeviceIndex device_index) {
    torch::utils::device_lazy_init(at::kMTIA);
    return at::detail::getMTIAHooks().getCurrentStream(device_index);
  });

  m.def("_mtia_deviceSynchronize", []() {
    torch::utils::device_lazy_init(at::kMTIA);
    at::detail::getMTIAHooks().deviceSynchronize(
        at::detail::getMTIAHooks().getCurrentDevice());
  });

  m.def("_mtia_exchangeDevice", [](c10::DeviceIndex device_index) {
    if (device_index < 0) {
      return static_cast<c10::DeviceIndex>(-1);
    }
    return at::detail::getMTIAHooks().exchangeDevice(device_index);
  });

  m.def("_mtia_getDefaultStream", [](c10::DeviceIndex device_index) {
    torch::utils::device_lazy_init(at::kMTIA);
    return at::detail::getMTIAHooks().getDefaultStream(device_index);
  });

  m.def("_mtia_setCurrentStream", [](const c10::Stream& stream) {
    torch::utils::device_lazy_init(at::kMTIA);
    auto device = at::detail::getMTIAHooks().getCurrentDevice();
    if (device != stream.device_index()) {
      at::detail::getMTIAHooks().setCurrentDevice(stream.device_index());
    }
    at::detail::getMTIAHooks().setCurrentStream(stream);
  });

  m.def("_mtia_memoryStats", [](c10::DeviceIndex device_index) {
    PyObject* raw_pyobject =
        at::detail::getMTIAHooks().memoryStats(device_index);
    return py::reinterpret_steal<py::object>(raw_pyobject);
  });

  m.def("_mtia_getDeviceCapability", [](c10::DeviceIndex device_index) {
    PyObject* raw_pyobject =
        at::detail::getMTIAHooks().getDeviceCapability(device_index);
    return py::reinterpret_steal<py::object>(raw_pyobject);
  });

  m.def("_mtia_emptyCache", []() { at::detail::getMTIAHooks().emptyCache(); });

  m.def(
      "_mtia_recordMemoryHistory",
      [](const std::optional<std::string>& enabled,
         const std::string& stacks,
         size_t max_entries) {
        at::detail::getMTIAHooks().recordMemoryHistory(
            enabled, stacks, max_entries);
      });

  m.def("_mtia_memorySnapshot", []() {
    PyObject* raw_pyobject = at::detail::getMTIAHooks().memorySnapshot();
    return py::reinterpret_steal<py::object>(raw_pyobject);
  });

  m.def("_mtia_getDeviceCount", []() {
    return at::detail::getMTIAHooks().deviceCount();
  });

  m.def("_mtia_resetPeakMemoryStats", [](c10::DeviceIndex device_index) {
    at::detail::getMTIAHooks().resetPeakMemoryStats(device_index);
  });
}

} // namespace torch::mtia
