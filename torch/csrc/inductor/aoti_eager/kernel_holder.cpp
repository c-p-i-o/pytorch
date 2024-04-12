#if !defined(C10_MOBILE) && !defined(ANDROID)
#include <torch/csrc/inductor/aoti_eager/kernel_holder.h>

#include <ATen/ATen.h>

#include <ATen/core/dispatch/Dispatcher.h>
#include <torch/csrc/PyInterpreter.h>
#include <torch/csrc/autograd/python_variable.h>
#include <torch/csrc/inductor/aoti_runner/model_container_runner_cpu.h>
#include <torch/csrc/inductor/aoti_runner/model_container_runner_cuda.h>
#include <torch/csrc/jit/frontend/function_schema_parser.h>

#include <torch/csrc/inductor/aoti_runner/model_container_runner_cpu.h>
#include <torch/csrc/inductor/aoti_torch/c/shim.h>
#include <torch/csrc/inductor/aoti_torch/tensor_converter.h>
#ifdef USE_CUDA
#include <torch/csrc/inductor/aoti_runner/model_container_runner_cuda.h>
#endif
#include <ATen/core/jit_type.h>

namespace torch::inductor {

namespace {

c10::ScalarType parse_dtype(const std::string& dtype_str) {
  // The dtype format is torch.float32, float32, torch.int32, int32, etc.
  std::string to_remove = "torch.";
  std::string canonicalized_dtype_str = dtype_str;
  size_t start_pos = dtype_str.find(to_remove);
  if (start_pos != std::string::npos) {
    canonicalized_dtype_str = dtype_str.substr(start_pos + to_remove.length());
  }

  if (canonicalized_dtype_str == "float32") {
    return c10::ScalarType::Float;
  } else if (canonicalized_dtype_str == "int32") {
    return c10::ScalarType::Int;
  } else if (canonicalized_dtype_str == "int64") {
    return c10::ScalarType::Long;
  } else if (canonicalized_dtype_str == "bool") {
    return c10::ScalarType::Bool;
  } else if (canonicalized_dtype_str == "bfloat16") {
    return c10::ScalarType::BFloat16;
  } else if (canonicalized_dtype_str == "float16") {
    return c10::ScalarType::Half;
  } else if (canonicalized_dtype_str == "float64") {
    return c10::ScalarType::Double;
  } else if (canonicalized_dtype_str == "uint8") {
    return c10::ScalarType::Byte;
  } else if (canonicalized_dtype_str == "int8") {
    return c10::ScalarType::Char;
  } else if (canonicalized_dtype_str == "complex64") {
    return c10::ScalarType::ComplexFloat;
  } else if (canonicalized_dtype_str == "complex128") {
    return c10::ScalarType::ComplexDouble;
  } else {
    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
        false, "Unsupported dtype: ", canonicalized_dtype_str);
    return c10::ScalarType::Undefined;
  }
}

enum class IValueType : uint8_t {
  Tensor,
  TensorList,
  OptionalTensorList,
  Scalar,
  Invalid,
};

static bool HandleTensor(
    const c10::IValue& ivalue,
    const c10::Device& device,
    std::vector<at::Tensor>& inputs) {
  inputs.push_back(ivalue.toTensor());
  return true;
}

static bool HandleTensorList(
    const c10::IValue& ivalue,
    const c10::Device& device,
    std::vector<at::Tensor>& inputs) {
  for (const auto& item : ivalue.toListRef()) {
    if (!item.isNone()) {
      inputs.push_back(item.toTensor());
    }
  }
  return true;
}

static bool HandleOptionalTensorList(
    const c10::IValue& ivalue,
    const c10::Device& device,
    std::vector<at::Tensor>& inputs) {
  return HandleTensorList(ivalue, device, inputs);
}

static bool HandleScalar(
    const c10::IValue& ivalue,
    const c10::Device& device,
    std::vector<at::Tensor>& inputs) {
  inputs.push_back(at::scalar_tensor(
      ivalue.toScalar(),
      c10::TensorOptions().device(device).dtype(ivalue.toScalar().type())));
  return true;
}

typedef bool (*HandlerFunc)(
    const c10::IValue& ivalue,
    const c10::Device& device,
    std::vector<at::Tensor>& inputs);
std::unordered_map<IValueType, HandlerFunc> handlers_ = {
    {IValueType::Tensor, &HandleTensor},
    {IValueType::TensorList, &HandleTensorList},
    {IValueType::OptionalTensorList, &HandleOptionalTensorList},
    {IValueType::Scalar, &HandleScalar}};

bool HandleIValue(
    const c10::IValue& ivalue,
    const c10::Device& device,
    std::vector<at::Tensor>& inputs) {
  IValueType ivalue_type = IValueType::Invalid;
  if (ivalue.isTensor()) {
    ivalue_type = IValueType::Tensor;
  } else if (ivalue.isTensorList()) {
    ivalue_type = IValueType::TensorList;
  } else if (ivalue.isOptionalTensorList()) {
    ivalue_type = IValueType::OptionalTensorList;
  } else if (ivalue.isScalar()) {
    ivalue_type = IValueType::Scalar;
  }

  auto it = handlers_.find(ivalue_type);
  if (it != handlers_.end()) {
    return it->second(ivalue, device, inputs);
  }

  // Handle unsupported types or add a default handler
  return false;
}

bool unpackTensors(
    const torch::jit::Stack& stack,
    const c10::Device& device,
    std::vector<at::Tensor>& inputs) {
  for (const auto& ivalue : stack) {
    if (!HandleIValue(ivalue, device, inputs)) {
      return false;
    }
  }
  return true;
}

} // namespace

AOTIPythonKernelHolder::AOTIPythonKernelHolder(
    py::object func,
    c10::DispatchKey dispatch_key,
    c10::string_view ns,
    c10::string_view op_name,
    c10::string_view op_overload_name,
    bool is_symbolic)
    : python_kernel_holder_(func, dispatch_key),
      dispatch_key_(dispatch_key),
      ns_(std::string(ns)),
      op_name_(std::string(op_name)),
      op_overload_name_(std::string(op_overload_name)),
      is_symbolic_(is_symbolic),
      is_fall_back_(func.ptr() == Py_None),
      device_(c10::Device(c10::dispatchKeyToDeviceType(dispatch_key_), 0)),
      pyinterpreter_(getPyInterpreter()) {
  auto pos = op_name_.find("::");
  TORCH_INTERNAL_ASSERT(pos != std::string::npos, op_name_);
  // Remove the namespace from the op_name as ns is already set
  op_name_ = op_name_.substr(pos + strlen("::"));

  (void)is_symbolic_; // Suppress unused variable warning

  // Initialize the AOTI kernel cache
  init_aoti_kernel_cache();
}

void AOTIPythonKernelHolder::operator()(
    const c10::OperatorHandle& op,
    c10::DispatchKeySet keyset,
    torch::jit::Stack* stack) {
  AOTIKernelState kernel_state;
  if (detect_cache(op, keyset, stack, kernel_state)) {
    cache_hit(kernel_state, op, keyset, stack);
  } else {
    cache_miss(op, keyset, stack);
  }
}

bool AOTIPythonKernelHolder::detect_cache(
    const c10::OperatorHandle& op,
    const c10::DispatchKeySet& keyset,
    const torch::jit::Stack* stack,
    AOTIKernelState& kernel_state) {
  auto return_arguments = op.schema().returns();
  // Only support single return value now and will extend to multiple return
  if (return_arguments.size() != 1) {
    return false;
  }

  auto arg = return_arguments[0];
  // Only support return single tensor.
  // TODO: Extend scope to support tensor vector
  if (!arg.type()->isSubtypeOf(c10::TensorType::get())) {
    return false;
  }

  std::vector<at::Tensor> inputs;
  auto res = unpackTensors(*stack, device_, inputs);
  if (!res || inputs.empty()) {
    return false;
  }

  auto inputs_meta_info = get_inputs_meta_info(inputs);
  auto aoti_kernel_state = aoti_kernel_cache_.find(inputs_meta_info);
  if (aoti_kernel_state == aoti_kernel_cache_.end()) {
    return false;
  }

  if (aoti_kernel_state->second.tensor_checks_.size() != inputs.size()) {
    return false;
  }

  LocalState local_state;
  local_state.overrideDispatchKeySet(c10::DispatchKeySet(dispatch_key_));

  for (size_t i = 0; i < inputs.size(); ++i) {
    bool pass = aoti_kernel_state->second.tensor_checks_[i].check(
        local_state, inputs[i]);
    if (!pass) {
      return false;
    }
  }

  kernel_state = aoti_kernel_state->second;
  return true;
}

void AOTIPythonKernelHolder::cache_hit(
    const AOTIKernelState& kernel_state,
    const c10::OperatorHandle& op,
    const c10::DispatchKeySet& keyset,
    torch::jit::Stack* stack) {
  std::vector<at::Tensor> inputs;
  unpackTensors(*stack, device_, inputs);
  torch::jit::drop(*stack, op.schema().arguments().size());

  auto outputs = kernel_state.kernel_runner_->run(inputs);
  for (auto& output : outputs) {
    stack->push_back(output);
  }
}

AOTIKernelMetaInfo AOTIPythonKernelHolder::get_inputs_meta_info(
    const std::vector<at::Tensor>& inputs) {
  AOTIKernelMetaInfo inputs_meta_info;
  for (const auto& input : inputs) {
    inputs_meta_info.emplace_back(
        is_symbolic_,
        input.scalar_type(),
        input.device(),
        input.sym_sizes().vec(),
        input.sym_strides().vec());
  }
  return inputs_meta_info;
}

/**
 * Initializes the cache for AOTInductor kernels within the
 * AOTIPythonKernelHolder class.
 *
 * The path of AOTI kernels for eager is
 *  - ${TORCHINDUCTOR_CACHE_DIR}/${kernel_path}/${kernel_id}.so
 *
 * Besides the kernel library, there is also a metadata file for each kernel
 * library.
 *  - ${TORCHINDUCTOR_CACHE_DIR}/aten_eager/${op_name}.json
 *
 * The kernels are loaded from the path and cached in the
 * AOTIPythonKernelHolder.
 *
 * Process:
 * 1. Device Type Check: It first checks if the device type is the compile-time
 * maximum. If so, the function exits early, as no initialization is needed for
 * these device types.
 *
 * 2. Environment Variable Retrieval: Attempts to retrieve the Eager AOTI kernel
 * path from the "TORCHINDUCTOR_CACHE_DIR" environment variable. If this
 * variable isn't set, the function exits early, indicating no path is provided.
 *
 * 3. AOTI Kernel Path Construction: Constructs the path to the AOTI kernels by
 * combining the base path from the environment variable with subdirectories
 * based on the device type (cpu, cuda, xpu) and operation name. This results in
 * a specific path targeting the required AOTI kernel for the current operation
 * and device.
 *
 * 4. Path Existence Check: Checks if the constructed path exists in the file
 * system. If not, the function returns, as there are no kernels to load.
 *
 * 5. Kernel File Processing: If the path exists, iterates through each file in
 * the directory. For files with a .so extension, it replaces this with .conf to
 * locate the corresponding kernel configuration file.
 *
 * 6. Kernel Metadata Loading and Caching: Reads the kernel metadata from each
 * .conf file (using TensorMetaInfo::fromConfig). If successful, adds this
 * metadata to the aoti_kernel_cache_. The cache maps the kernel metadata to a
 * corresponding AOTI model container runner, obtained via
 * getAOTIModelContainerRunner.
 *
 * This function is crucial for setting up the AOTI kernel infrastructure,
 * enabling efficient inference operations tailored to the specific runtime
 * environment.
 */
void AOTIPythonKernelHolder::init_aoti_kernel_cache() {
  if (device_.type() == c10::DeviceType::COMPILE_TIME_MAX_DEVICE_TYPES) {
    return;
  }

  py::gil_scoped_acquire gil;

  py::handle load_aoti_eager_cache_function =
      py::module::import("torch._inductor.utils").attr("load_aoti_eager_cache");
  if (load_aoti_eager_cache_function.ptr() == nullptr) {
    return;
  }

  auto result = py::reinterpret_steal<py::object>(PyObject_CallFunctionObjArgs(
      load_aoti_eager_cache_function.ptr(),
      py::str(ns_).ptr(),
      py::str(op_name_).ptr(),
      py::str(op_overload_name_).ptr(),
      py::str(c10::DeviceTypeName(device_.type())).ptr(),
      nullptr));
  if (result.ptr() == nullptr || result.ptr() == Py_None) {
    return;
  }

  auto kernel_info_list = result.cast<py::list>();
  for (auto kernel_info : kernel_info_list) {
    auto item_dict = kernel_info.cast<py::dict>();

    // Access the kernel_path field
    auto kernel_path = item_dict["kernel_path"].cast<std::string>();

    // Access the meta_info list
    auto meta_info_list = item_dict["meta_info"].cast<py::list>();

    std::vector<TensorCheck> tensor_checks;
    std::vector<TensorMetaInfo> tensor_meta_info_list;

    LocalState state;
    // Loop over the meta_info list
    for (auto meta_info : meta_info_list) {
      // Convert the handle to a dict
      auto meta_info_dict = meta_info.cast<py::dict>();

      // Access the fields of each meta_info dict
      auto is_dynamic = meta_info_dict["is_dynamic"].cast<bool>();
      auto device_type = meta_info_dict["device_type"].cast<std::string>();
      auto dtype = meta_info_dict["dtype"].cast<std::string>();
      auto sizes = meta_info_dict["sizes"].cast<std::vector<int64_t>>();
      auto strides = meta_info_dict["strides"].cast<std::vector<int64_t>>();

      std::vector<c10::SymInt> sym_sizes;
      std::vector<c10::SymInt> sym_strides;
      std::vector<std::optional<c10::SymInt>> sym_optional_sizes;
      std::vector<std::optional<c10::SymInt>> sym_optional_strides;
      for (int64_t size : sizes) {
        sym_sizes.push_back(c10::SymInt(size));
        sym_optional_sizes.push_back(std::optional<c10::SymInt>(size));
      }
      for (int64_t stride : strides) {
        sym_strides.push_back(c10::SymInt(stride));
        sym_optional_strides.push_back(std::optional<c10::SymInt>(stride));
      }

      // Now you can use these variables in your code
      tensor_meta_info_list.emplace_back(
          is_dynamic,
          parse_dtype(dtype),
          c10::Device(device_type),
          sym_sizes,
          sym_strides);
      tensor_checks.emplace_back(
          state,
          nullptr,
          uint64_t(c10::DispatchKeySet(dispatch_key_).raw_repr()),
          parse_dtype(dtype),
          c10::DeviceIndex(0),
          sym_optional_sizes,
          sym_optional_strides);
    }

    AOTIKernelState aoti_kernel_state;
    aoti_kernel_state.kernel_runner_ = load_aoti_model_runner(kernel_path);
    aoti_kernel_state.tensor_checks_ = tensor_checks;
    aoti_kernel_cache_[tensor_meta_info_list] = aoti_kernel_state;
  }
}

std::shared_ptr<AOTIModelContainerRunner> AOTIPythonKernelHolder::
    load_aoti_model_runner(const std::string& so_path) {
  if (device_.type() == c10::DeviceType::CUDA) {
#ifdef USE_CUDA
    return std::make_shared<AOTIModelContainerRunnerCpu>(so_path);
#else
    return nullptr;
#endif
  } else if (device_.type() == c10::DeviceType::CPU) {
    return std::make_shared<AOTIModelContainerRunnerCpu>(so_path);
  } else {
    TORCH_WARN("Unsupported device type");
    return nullptr;
  }
}

void AOTIPythonKernelHolder::cache_miss(
    const c10::OperatorHandle& op,
    const c10::DispatchKeySet& keyset,
    torch::jit::Stack* stack) {
  auto kernel_lib_path = produce_aoti_kernel_lib(op, keyset, stack);
  if (!kernel_lib_path.empty()) {
    auto device_type = c10::dispatchKeyToDeviceType(dispatch_key_);
    auto device_index = 0;
    auto device = c10::Device(device_type, device_index);

    std::shared_ptr<AOTIModelContainerRunner> kernel =
        load_aoti_model_runner(kernel_lib_path);
    if (kernel) {
      std::vector<at::Tensor> inputs;
      if (unpackTensors(*stack, device_, inputs)) {
        auto outputs = kernel->run(inputs);
        if (outputs.size() > 0) {
          torch::jit::drop(*stack, op.schema().arguments().size());
          // TODO: Check output spec
          for (auto& output : outputs) {
            torch::jit::push(*stack, std::move(output));
          }
          return;
        }
      }
    }
  }

  if (is_fall_back_) {
    python_kernel_holder_(op, keyset, stack);
  } else {
    TORCH_INTERNAL_ASSERT(
        false,
        "Failed to produce or load AOTI kernel and no fallback function.");
  }
}

std::string AOTIPythonKernelHolder::produce_aoti_kernel_lib(
    const c10::OperatorHandle& op,
    const c10::DispatchKeySet& keyset,
    const torch::jit::Stack* stack) {
  auto arguments = torch::jit::last(*stack, op.schema().arguments().size());

  const auto& schema = op.schema();
  const auto& qualified_name = op.operator_name().name;
  const auto& overload_name = schema.overload_name();
  auto pos = qualified_name.find("::");
  TORCH_INTERNAL_ASSERT(pos != std::string::npos, qualified_name);
  std::string ns_str(qualified_name.begin(), qualified_name.begin() + pos);
  std::string func_name(
      qualified_name.begin() + pos + strlen("::"), qualified_name.end());

  std::string kernel_lib_path("");

  py::gil_scoped_acquire gil;
  py::handle op_py_func = op.getPythonOp(pyinterpreter_, [&]() -> PyObject* {
    // Parse the name into namespace and name (no overload_name)
    py::handle torch_api_function = py::module::import("torch")
                                        .attr("ops")
                                        .attr(ns_str.c_str())
                                        .attr(func_name.c_str());
    if (overload_name.empty()) {
      return torch_api_function.attr("default").ptr();
    } else {
      return torch_api_function.attr(overload_name.c_str()).ptr();
    }
  });

  if (op_py_func.ptr() == nullptr) {
    return kernel_lib_path;
  }

  py::handle aot_compile_function =
      py::module::import("torch._inductor.utils")
          .attr("aot_compile_with_persistent_cache");
  if (aot_compile_function.ptr() == nullptr) {
    return kernel_lib_path;
  }

  auto args_kwargs = parseIValuesToPyArgsKwargs(op, arguments.vec());
  auto result = py::reinterpret_steal<py::object>(PyObject_CallFunctionObjArgs(
      aot_compile_function.ptr(),
      py::str(ns_str).ptr(),
      py::str(func_name).ptr(),
      py::str(overload_name).ptr(),
      py::str(c10::DeviceTypeName(device_.type())).ptr(),
      py::bool_(is_symbolic_).ptr(),
      op_py_func.ptr(),
      args_kwargs.first.ptr(),
      args_kwargs.second.ptr(),
      nullptr));
  if (result.ptr() != nullptr) {
    kernel_lib_path = py::cast<std::string>(result);
  }

  return kernel_lib_path;
}

} // namespace torch::inductor
#endif
