#include <torch/csrc/inductor/aoti_package/model_package_loader.h>
#include <torch/csrc/inductor/aoti_runner/model_container_runner.h>
#include <torch/csrc/inductor/aoti_runner/model_container_runner_cpu.h>
#ifdef USE_CUDA
#include <torch/csrc/inductor/aoti_runner/model_container_runner_cuda.h>
#endif

#include <torch/csrc/autograd/python_variable.h>
#include <torch/csrc/inductor/aoti_runner/pybind.h>
#include <torch/csrc/utils/pybind.h>

namespace torch::inductor {

class AOTIModelPackageLoaderPybind : public AOTIModelPackageLoader {
 public:
  AOTIModelPackageLoaderPybind(
      const std::string& model_package_path,
      const std::string& model_name,
      const bool run_single_threaded)
      : AOTIModelPackageLoader(
            model_package_path,
            model_name,
            run_single_threaded) {}

  py::list boxed_run(py::list& inputs, void* stream_handle = nullptr) {
    std::vector<at::Tensor> input_tensors;
    input_tensors.reserve(inputs.size());
    for (auto& item : inputs) {
      input_tensors.emplace_back(py::cast<at::Tensor>(item));
    }
    // Explicitly clear the passed-in Python list
    inputs.attr("clear")();

    std::vector<at::Tensor> result_tensors = AOTIModelPackageLoader::boxed_run(
        std::move(input_tensors), stream_handle);

    py::list outputs;
    for (const auto& tensor : result_tensors) {
      outputs.append(
          py::reinterpret_steal<py::object>(THPVariable_Wrap(tensor)));
    }
    return outputs;
  }
};

// NOLINTNEXTLINE(misc-use-internal-linkage)
void initAOTIPackageBindings(PyObject* module) {
  auto rootModule = py::handle(module).cast<py::module>();
  auto m = rootModule.def_submodule("_aoti");

  py::class_<AOTIModelPackageLoaderPybind>(m, "AOTIModelPackageLoader")
      .def(py::init<const std::string&, const std::string&, const bool>())
      .def("get_metadata", &AOTIModelPackageLoaderPybind::get_metadata)
      .def(
          "run",
          &AOTIModelPackageLoaderPybind::run,
          py::arg("inputs"),
          py::arg("stream_handle") = nullptr)
      .def(
          "boxed_run",
          &AOTIModelPackageLoaderPybind::boxed_run,
          py::arg("inputs"),
          py::arg("stream_handle") = nullptr)
      .def("get_call_spec", &AOTIModelPackageLoaderPybind::get_call_spec)
      .def(
          "get_constant_fqns", &AOTIModelPackageLoaderPybind::get_constant_fqns)
      .def("load_constants", &AOTIModelPackageLoaderPybind::load_constants)
      .def(
          "update_constant_buffer",
          &AOTIModelPackageLoaderPybind::update_constant_buffer);
}
} // namespace torch::inductor
