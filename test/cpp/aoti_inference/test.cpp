#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <vector>

#include <torch/csrc/inductor/aoti_package/model_package_loader.h>
#include <torch/csrc/inductor/aoti_runner/model_container_runner_cpu.h>
#if defined(USE_CUDA)
#include <cuda_runtime.h>
#endif
#if defined(USE_CUDA) || defined(USE_ROCM)
#include <torch/csrc/inductor/aoti_runner/model_container_runner_cuda.h>
#endif
#include <torch/script.h>
#include <torch/torch.h>

#define STR_VALUE(x) #x
#define STRINGIZE(x) STR_VALUE(x)

namespace {

void test_aoti(const std::string& device, bool use_runtime_constant_folding) {
  torch::NoGradGuard no_grad;

  std::string data_path =
      (std::filesystem::path(STRINGIZE(CMAKE_CURRENT_BINARY_DIR)) / "data.pt")
           .string();
  torch::jit::script::Module data_loader = torch::jit::load(data_path);
  std::string suffix = use_runtime_constant_folding
      ? device + "_use_runtime_constant_folding"
      : device;
  std::string path_attr = "model_so_path_" + suffix;
  std::string inputs_attr = "inputs_" + suffix;
  std::string outputs_attr = "outputs_" + suffix;
  const auto& model_so_path = data_loader.attr(path_attr.c_str()).toStringRef();
  const auto& ref_output_tensors =
      data_loader.attr(outputs_attr.c_str()).toTensorList().vec();

  std::unique_ptr<torch::inductor::AOTIModelContainerRunner> runner;
  if (device == "cpu") {
    runner = std::make_unique<torch::inductor::AOTIModelContainerRunnerCpu>(
        model_so_path);
#if defined(USE_CUDA) || defined(USE_ROCM)
  } else if (device == "cuda") {
    runner = std::make_unique<torch::inductor::AOTIModelContainerRunnerCuda>(
        model_so_path);
#endif
  } else {
    testing::AssertionFailure() << "unsupported device: " << device;
  }
  auto actual_output_tensors =
      runner->run(data_loader.attr(inputs_attr.c_str()).toTensorList().vec());
  ASSERT_TRUE(torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));
}

void test_aoti_script(const std::string& device) {
  torch::NoGradGuard no_grad;

  std::string script_model = "script_model_" + device + ".pt";
  std::string model_path =
      (std::filesystem::path(
           STRINGIZE(CMAKE_CURRENT_BINARY_DIR)) / script_model.c_str())
           .string();
  torch::jit::script::Module model = torch::jit::load(model_path);

  std::string sample_data_path =
      (std::filesystem::path(
           STRINGIZE(CMAKE_CURRENT_BINARY_DIR)) / "script_data.pt")
           .string();
  torch::jit::script::Module sample_data = torch::jit::load(sample_data_path);
  std::string inputs_attr = "inputs_" + device;
  std::string outputs_attr = "outputs_" + device;
  const auto& inputs = sample_data.attr(inputs_attr.c_str()).toList().vec();
  const auto& ref_output_tensors =
      sample_data.attr(outputs_attr.c_str()).toTensorVector();
  auto outputs = model.forward(inputs).toTuple()->elements();
  ASSERT_EQ(outputs.size(), ref_output_tensors.size());
  for (size_t i = 0; i < ref_output_tensors.size(); i++) {
    ASSERT_TRUE(torch::allclose(outputs[i].toTensor(), ref_output_tensors[i]));
  }
}

void test_aoti_package_loader(
    const std::string& device,
    bool use_runtime_constant_folding) {
  torch::NoGradGuard no_grad;

  std::string data_path =
      (std::filesystem::path(STRINGIZE(CMAKE_CURRENT_BINARY_DIR)) / "data.pt")
           .string();
  torch::jit::script::Module data_loader = torch::jit::load(data_path);
  std::string suffix = use_runtime_constant_folding
      ? device + "_use_runtime_constant_folding"
      : device;
  std::string path_attr = "pt2_package_path_" + suffix;
  std::string inputs_attr = "inputs_" + suffix;
  std::string outputs_attr = "outputs_" + suffix;
  const auto& pt2_package_path =
      data_loader.attr(path_attr.c_str()).toStringRef();
  const auto& ref_output_tensors =
      data_loader.attr(outputs_attr.c_str()).toTensorList().vec();

  torch::inductor::AOTIModelPackageLoader runner(pt2_package_path);
  auto actual_output_tensors =
      runner.run(data_loader.attr(inputs_attr.c_str()).toTensorList().vec());
  ASSERT_TRUE(torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));
}

void test_aoti_constants_update(
    const std::string& device,
    bool use_runtime_constant_folding) {
  torch::NoGradGuard no_grad;

  std::string data_path =
      (std::filesystem::path(STRINGIZE(CMAKE_CURRENT_BINARY_DIR)) / "data.pt")
           .string();

  torch::jit::script::Module data_loader = torch::jit::load(data_path);
  std::string suffix = use_runtime_constant_folding
      ? device + "_use_runtime_constant_folding"
      : device;
  std::string path_attr = "model_so_path_" + suffix;
  std::string inputs_attr = "inputs_" + suffix;
  std::string outputs_attr = "outputs_" + suffix;
  std::string weights_attr = "w_pre_" + suffix;
  std::string add_attr = "w_add_" + suffix;
  const auto& model_so_path = data_loader.attr(path_attr.c_str()).toStringRef();
  auto input_tensors =
      data_loader.attr(inputs_attr.c_str()).toTensorList().vec();
  const auto& ref_output_tensors =
      data_loader.attr(outputs_attr.c_str()).toTensorList().vec();

  const auto& weight_tensors =
      data_loader.attr(weights_attr.c_str()).toTensor();
  const auto& add_tensors = data_loader.attr(add_attr.c_str()).toTensor();

  torch::inductor::TensorConstantMap missing_map, rand_map, real_map;
  missing_map.emplace("L__self___w_pre", new at::Tensor(at::randn({4, 4})));
  rand_map.emplace("L__self___w_pre", new at::Tensor(at::randn({10})));
  rand_map.emplace("L__self___w_add", new at::Tensor(at::randn({10})));
  real_map.emplace("L__self___w_pre", new at::Tensor(weight_tensors));
  real_map.emplace("L__self___w_add", new at::Tensor(add_tensors));

  std::unique_ptr<torch::inductor::AOTIModelContainerRunner> runner;
  if (device == "cpu") {
    runner = std::make_unique<torch::inductor::AOTIModelContainerRunnerCpu>(
        model_so_path);
#if defined(USE_CUDA) || defined(USE_ROCM)
  } else if (device == "cuda") {
    runner = std::make_unique<torch::inductor::AOTIModelContainerRunnerCuda>(
        model_so_path);
#endif
  } else {
    testing::AssertionFailure() << "unsupported device: " << device;
  }
  // By default, buffer #1 get loaded with burned in weights. Correct results.
  auto actual_output_tensors = runner->run(input_tensors);
  ASSERT_TRUE(torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));

  // Update with missing map which should throw.
  // Somehow EXPECT_THROW doesn't work here when running tests in a row, but
  // works when running AotInductorTest.RuntimeUpdateConstantsCuda individually.
  try {
    runner->update_constant_buffer(missing_map, false, true);
  } catch (const std::runtime_error& e) {
    EXPECT_THAT(e.what(), ::testing::HasSubstr("API call failed at"));
  }

  // Update random weight to buffer #1.
  runner->update_constant_buffer(missing_map, false, false);
  actual_output_tensors = runner->run(input_tensors);
  if (use_runtime_constant_folding) {
    // At this moment, this update is applied on the original weight.
    // The weight being consumed is "folded", so will have no affect.
    ASSERT_TRUE(
        torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));
    runner->run_const_fold(/* use_inactive = */ false);
    actual_output_tensors = runner->run(input_tensors);
  }
  ASSERT_FALSE(
      torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));

  // Update with real map.
  runner->update_constant_buffer(real_map, false, false);
  actual_output_tensors = runner->run(input_tensors);
  if (use_runtime_constant_folding) {
    runner->run_const_fold(/* use_inactive = */ false);
  }
  actual_output_tensors = runner->run(input_tensors);
  ASSERT_TRUE(torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));

  // Update with full random map.
  runner->update_constant_buffer(rand_map, false, false);
  if (use_runtime_constant_folding) {
    runner->run_const_fold(/* use_inactive = */ false);
  }
  actual_output_tensors = runner->run(input_tensors);
  ASSERT_FALSE(
      torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));
}

void test_aoti_double_buffering(
    const std::string& device,
    bool use_runtime_constant_folding) {
  torch::NoGradGuard no_grad;

  std::string data_path =
      (std::filesystem::path(STRINGIZE(CMAKE_CURRENT_BINARY_DIR)) / "data.pt")
           .string();

  torch::jit::script::Module data_loader = torch::jit::load(data_path);
  std::string suffix = use_runtime_constant_folding
      ? device + "_use_runtime_constant_folding"
      : device;
  std::string path_attr = "model_so_path_" + suffix;
  std::string inputs_attr = "inputs_" + suffix;
  std::string outputs_attr = "outputs_" + suffix;
  std::string weights_attr = "w_pre_" + suffix;
  std::string add_attr = "w_add_" + suffix;
  const auto& model_so_path = data_loader.attr(path_attr.c_str()).toStringRef();
  auto input_tensors =
      data_loader.attr(inputs_attr.c_str()).toTensorList().vec();
  const auto& ref_output_tensors =
      data_loader.attr(outputs_attr.c_str()).toTensorList().vec();

  const auto& weight_tensors =
      data_loader.attr(weights_attr.c_str()).toTensor();
  const auto& add_tensors = data_loader.attr(add_attr.c_str()).toTensor();

  torch::inductor::TensorConstantMap rand_map, real_map;
  rand_map.emplace("L__self___w_pre", new at::Tensor(at::randn({4, 4})));
  rand_map.emplace("L__self___w_add", new at::Tensor(at::randn({4, 4})));
  real_map.emplace("L__self___w_pre", new at::Tensor(weight_tensors));
  real_map.emplace("L__self___w_add", new at::Tensor(add_tensors));

  std::unique_ptr<torch::inductor::AOTIModelContainerRunner> runner;
  if (device == "cpu") {
    runner = std::make_unique<torch::inductor::AOTIModelContainerRunnerCpu>(
        model_so_path);
#if defined(USE_CUDA) || defined(USE_ROCM)
  } else if (device == "cuda") {
    runner = std::make_unique<torch::inductor::AOTIModelContainerRunnerCuda>(
        model_so_path);
#endif
  } else {
    testing::AssertionFailure() << "unsupported device: " << device;
  }
  // By default, buffer #1 get loaded with burned in weights. Correct results.
  auto actual_output_tensors = runner->run(input_tensors);
  ASSERT_TRUE(torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));

  // We update the weights to buffer #2 and activate it. This should still
  // produce correct result, as it's the real constant map.
  runner->update_inactive_constant_buffer(real_map);
  if (use_runtime_constant_folding) {
    runner->run_const_fold(/* use_inactive = */ true);
  }
  runner->swap_constant_buffer();
  actual_output_tensors = runner->run(input_tensors);
  ASSERT_TRUE(torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));

  // We update random weights to buffer #1. But do not swap in the weight yet.
  runner->update_inactive_constant_buffer(rand_map);
  if (use_runtime_constant_folding) {
    runner->run_const_fold(/* use_inactive = */ true);
  }
  actual_output_tensors = runner->run(input_tensors);
  ASSERT_TRUE(torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));

  // We swap and activate the weight to buffer #1. This is random weight and
  // should produce incorrect results.
  runner->swap_constant_buffer();
  actual_output_tensors = runner->run(input_tensors);
  ASSERT_FALSE(
      torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));

  // Swap back to buffer #2 which is the real constants.
  runner->swap_constant_buffer();
  actual_output_tensors = runner->run(input_tensors);
  ASSERT_TRUE(torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));
}

#if defined(USE_CUDA) || defined(USE_ROCM)
void test_aoti_double_buffering_with_tensor_constants() {
  torch::NoGradGuard no_grad;

  std::string data_path = (std::filesystem::path(
                               STRINGIZE(CMAKE_CURRENT_BINARY_DIR)) /
                               "data_with_tensor_constants.pt")
                               .string();

  torch::jit::script::Module data_loader = torch::jit::load(data_path);
  std::string path_attr = "model_so_path";
  std::string inputs_attr = "inputs";
  std::string w_attr = "w";
  std::string outputs_attr = "outputs";
  const auto& model_so_path = data_loader.attr(path_attr.c_str()).toStringRef();
  auto input_tensors =
      data_loader.attr(inputs_attr.c_str()).toTensorList().vec();
  const auto& w_tensors = data_loader.attr(w_attr.c_str()).toTensor();
  const auto& ref_output_tensors =
      data_loader.attr(outputs_attr.c_str()).toTensorList().vec();

  torch::inductor::TensorConstantMap real_map;
  real_map.emplace("L__self___w", new at::Tensor(w_tensors));

  std::unique_ptr<torch::inductor::AOTIModelContainerRunner> runner;
  runner = std::make_unique<torch::inductor::AOTIModelContainerRunnerCuda>(
      model_so_path.c_str());

  // By default, buffer #1 get loaded with burned in weights. Correct results.
  auto actual_output_tensors = runner->run(input_tensors);
  ASSERT_TRUE(torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));

  // We update the weights to buffer #2 and activate it. This should still
  // produce correct result, since we would have copied the tensor_constants.
  runner->update_inactive_constant_buffer(real_map);
  runner->swap_constant_buffer();
  actual_output_tensors = runner->run(input_tensors);
  ASSERT_TRUE(torch::allclose(ref_output_tensors[0], actual_output_tensors[0]));
}
#endif

#if defined(USE_CUDA)
void test_aoti_free_buffer() {
  torch::NoGradGuard no_grad;

  std::string data_path =
      (std::filesystem::path(
           STRINGIZE(CMAKE_CURRENT_BINARY_DIR)) / "large_data.pt")
           .string();

  // Memory information variable
  cudaError_t cudaStatus;
  size_t DATASIZE = 128 * 1024 * 1024; // We have 128MB of weight data.

  torch::jit::script::Module data_loader = torch::jit::load(data_path);
  std::string path_attr = "model_so_path";
  std::string inputs_attr = "inputs";
  std::string outputs_attr = "outputs";
  std::string weights_attr = "w_pre";
  std::string add_attr = "w_add";
  const auto& model_so_path = data_loader.attr(path_attr.c_str()).toStringRef();
  auto input_tensors =
      data_loader.attr(inputs_attr.c_str()).toTensorList().vec();
  const auto& ref_output_tensors =
      data_loader.attr(outputs_attr.c_str()).toTensorList().vec();

  const auto& weight_tensors =
      data_loader.attr(weights_attr.c_str()).toTensor();
  const auto& add_tensors = data_loader.attr(add_attr.c_str()).toTensor();

  torch::inductor::TensorConstantMap rand_map, real_map;
  rand_map.emplace("L__self___w_pre", new at::Tensor(at::randn({4096, 4096})));
  rand_map.emplace("L__self___w_add", new at::Tensor(at::randn({4096, 4096})));
  real_map.emplace("L__self___w_pre", new at::Tensor(weight_tensors));
  real_map.emplace("L__self___w_add", new at::Tensor(add_tensors));

  std::unique_ptr<torch::inductor::AOTIModelContainerRunner> runner;
  runner = std::make_unique<torch::inductor::AOTIModelContainerRunnerCuda>(
      model_so_path);

  // We extract the initial memory here.
  size_t initMemory = 0;
  size_t totalMemory = 0;
  cudaStatus = cudaMemGetInfo(&initMemory, &totalMemory);
  if (cudaStatus != cudaSuccess) {
    throw std::runtime_error("cudaMemGetInfo failed!");
  }

  // We update inactive buffer, this should create one copy (128MB)
  runner->update_inactive_constant_buffer(real_map);
  size_t updateMemory2 = 0;
  cudaStatus = cudaMemGetInfo(&updateMemory2, &totalMemory);
  if (cudaStatus != cudaSuccess) {
    throw std::runtime_error("cudaMemGetInfo failed!");
  }
  ASSERT_EQ(initMemory - DATASIZE, updateMemory2);

  // We swap and free the inactive buffer.
  runner->swap_constant_buffer();
  runner->free_inactive_constant_buffer();
  size_t postFreeMemory = 0;
  cudaStatus = cudaMemGetInfo(&postFreeMemory, &totalMemory);
  if (cudaStatus != cudaSuccess) {
    throw std::runtime_error("cudaMemGetInfo failed!");
  }
  // We should only have one set of buffer (#2), memory used should equal
  // initial memory.
  ASSERT_EQ(initMemory, postFreeMemory);

  // We update random weights to buffer #1.
  runner->update_inactive_constant_buffer(rand_map);
  size_t updateMemory1 = 0;
  cudaStatus = cudaMemGetInfo(&updateMemory1, &totalMemory);
  if (cudaStatus != cudaSuccess) {
    throw std::runtime_error("cudaMemGetInfo failed!");
  }
  ASSERT_EQ(initMemory - DATASIZE, updateMemory1);

  // Test if we directly free the buffer #1.
  runner->free_inactive_constant_buffer();
  size_t finalMemory = 0;
  cudaStatus = cudaMemGetInfo(&finalMemory, &totalMemory);
  if (cudaStatus != cudaSuccess) {
    throw std::runtime_error("cudaMemGetInfo failed!");
  }
  ASSERT_EQ(initMemory, finalMemory);
}
#endif
} // namespace

namespace torch {
namespace aot_inductor {

TEST(AotInductorTest, BasicTestCpu) {
  test_aoti("cpu", false);
}

TEST(AotInductorTest, BasicScriptTestCpu) {
  test_aoti_script("cpu");
}

TEST(AotInductorTest, BasicPackageLoaderTestCpu) {
  test_aoti_package_loader("cpu", false);
}

#ifdef USE_CUDA
TEST(AotInductorTest, BasicTestCuda) {
  test_aoti("cuda", true);
  test_aoti("cuda", false);
}

TEST(AotInductorTest, BasicScriptTestCuda) {
  test_aoti_script("cuda");
}

TEST(AotInductorTest, BasicPackageLoaderTestCuda) {
  test_aoti_package_loader("cuda", false);
}

TEST(AotInductorTest, RuntimeUpdateConstantsCuda) {
  test_aoti_constants_update("cuda", true);
}

TEST(AotInductorTest, UpdateConstantsCuda) {
  test_aoti_constants_update("cuda", false);
}

TEST(AotInductorTest, RuntimeUpdateInactiveConstantsCuda) {
  test_aoti_double_buffering("cuda", true);
}

TEST(AotInductorTest, UpdateInactiveConstantsCuda) {
  test_aoti_double_buffering("cuda", false);
}

TEST(AotInductorTest, UpdateInactiveConstantsWithTensorConstantsCuda) {
  test_aoti_double_buffering_with_tensor_constants();
}

TEST(AotInductorTest, FreeInactiveConstantBufferCuda) {
  test_aoti_free_buffer();
}
#endif

} // namespace aot_inductor
} // namespace torch
