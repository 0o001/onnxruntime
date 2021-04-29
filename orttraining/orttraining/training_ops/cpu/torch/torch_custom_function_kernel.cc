// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "torch_custom_function_kernel.h"
#include "core/language_interop_ops/pyop/pyop_lib_proxy.h"
#include "core/torch_custom_function/torch_custom_function_register.h"
#include <thread>

namespace onnxruntime {
namespace contrib {

ONNX_OPERATOR_KERNEL_EX(
    PythonOp,
    kMSDomain,
    1,
    kCpuExecutionProvider,
    KernelDefBuilder()
        .TypeConstraint("T", DataTypeImpl::AllTensorAndSequenceTensorTypes())
        .TypeConstraint("TInt64", DataTypeImpl::GetTensorType<int64_t>()),
    PythonOp);

ONNX_OPERATOR_KERNEL_EX(
    PythonOpGrad,
    kMSDomain,
    1,
    kCpuExecutionProvider,
    KernelDefBuilder()
        .TypeConstraint("T", DataTypeImpl::AllTensorAndSequenceTensorTypes())
        .TypeConstraint("TInt64", DataTypeImpl::GetTensorType<int64_t>()),
    PythonOpGrad);

Status PythonOp::Compute(OpKernelContext* context) const {
  ORT_ENFORCE(context);
  std::cout << "std::this_thread::get_id() is : " << std::this_thread::get_id() << std::endl;

  auto* ctx_internal = reinterpret_cast<onnxruntime::OpKernelContextInternal*>(context);
  ORT_ENFORCE(nullptr != context);
  auto inputs_count = (size_t)ctx_internal->InputCount();
  std::vector<OrtValue*> inputs;
  std::vector<void*> outputs;

  for (size_t i = 0; i < inputs_count; ++i) {
    inputs.push_back(const_cast<OrtValue*>(ctx_internal->GetInputMLValue(i)));
  }

  auto log_func = [&](const char* msg) {
    std::cout << "InvokePythonAutoGradFunc logging:" << msg << std::endl;
    //LOGS_DEFAULT(WARNING) << msg << std::endl;
  };

  std::string err;
  auto state = PyOpLibProxy::GetInstance().GetGil();
  ORT_ENFORCE(PyOpLibProxy::GetInstance().InvokePythonAutoGradFunc(instance_, "compute", nullptr, inputs, outputs,
                                                                   log_func),
              PyOpLibProxy::GetInstance().GetLastErrorMessage(err));  //ORT_ENFORCE
  PyOpLibProxy::GetInstance().PutGil(state);

  // We had the assumption:
  // The 1st output is context index of auto grad function.
  // The 2nd output is address of OrtValue we got from Python script run.
  PyObject* ctx_addr = reinterpret_cast<PyObject*>(outputs[0]);
  ORT_ENFORCE(ctx_addr, "Context object pointer should not be null");
  int64_t ctx_index = onnxruntime::python::OrtTorchFunctionPool::GetInstance().RegisterContext(ctx_addr);

  Tensor* first_output_tensor = context->Output(0, {1});
  ORT_ENFORCE(first_output_tensor != nullptr, "first_output_tensor should not be null.");
  int64_t* step_data_new = first_output_tensor->template MutableData<int64_t>();
  *step_data_new = ctx_index;

  void* forward_ret_ortvalue_addr = outputs[1];
  // OrtValue is not release til now because we keep the values in Python side until the Python class instance is destoyed.
  // If we don't want Python do the lifecycle guarantee, we need consider PY_INCRE here as well, but be careful, need
  // operate on PyObject, directly operating on OrtValue will bring unexpected results.
  auto* forward_ret_ortvalue_ptr = reinterpret_cast<OrtValue*>(forward_ret_ortvalue_addr);
  ORT_ENFORCE(forward_ret_ortvalue_ptr != nullptr, "forward_ret_ortvalue_ptr should not be null");

  Tensor* t = forward_ret_ortvalue_ptr->GetMutable<Tensor>();
  const auto& input_shape = t->Shape();
  const auto num_dim = input_shape.NumDimensions();
  std::cout << "ortvalue addr:" << forward_ret_ortvalue_ptr << ", tenosr addr: " << t
            << ", tensor->MutableDataRaw() addr :" << reinterpret_cast<int64_t>(t->MutableDataRaw())
            << ", num_dim: " << num_dim << std::endl;

  for (size_t i = 0; i < num_dim; ++i) {
    std::cout << "PythonOp::Compute shape : " << input_shape.GetDims()[i] << std::endl;
  }
  ORT_RETURN_IF_ERROR(ctx_internal->SetOutputMLValue(1, *forward_ret_ortvalue_ptr));
  return Status::OK();
}

Status PythonOpGrad::Compute(OpKernelContext* context) const {
  ORT_ENFORCE(context);

  auto* ctx_internal = reinterpret_cast<onnxruntime::OpKernelContextInternal*>(context);
  ORT_ENFORCE(nullptr != context);
  auto inputs_count = (size_t)ctx_internal->InputCount();
  std::vector<OrtValue*> inputs;
  std::vector<void*> outputs;

  const Tensor* first_output_tensor = context->Input<Tensor>(0);
  ORT_ENFORCE(first_output_tensor != nullptr, "first_output_tensor should not be null.");
  const int64_t* context_index_ptr = first_output_tensor->template Data<int64_t>();
  PyObject* ctx_ptr = onnxruntime::python::OrtTorchFunctionPool::GetInstance().GetContext(*context_index_ptr);

  for (size_t i = 1; i < inputs_count; ++i) {
    inputs.push_back(const_cast<OrtValue*>(ctx_internal->GetInputMLValue(i)));
  }

  auto log_func = [&](const char* msg) {
    std::cout << "InvokePythonAutoGradFunc logging:" << msg << std::endl;
    //LOGS_DEFAULT(WARNING) << msg << std::endl;
  };

  std::cout << "context_address_value_ptr got within PythonOpGrad::Compute:" << reinterpret_cast<void*>(ctx_ptr) << std::endl;
  //int64_t ctx_index = onnxruntime::python::OrtTorchFunctionPool::GetInstance().RegisterContext(ctx_addr);

  std::string err;
  auto state = PyOpLibProxy::GetInstance().GetGil();
  ORT_ENFORCE(PyOpLibProxy::GetInstance().InvokePythonAutoGradFunc(instance_, "backward_compute", ctx_ptr, inputs, outputs,
                                                                   log_func),
              PyOpLibProxy::GetInstance().GetLastErrorMessage(err));
  PyOpLibProxy::GetInstance().PutGil(state);

  void* backward_ret_ortvalue_addr = outputs[0];
  auto* backward_ret_ortvalue_ptr = reinterpret_cast<OrtValue*>(backward_ret_ortvalue_addr);
  ORT_ENFORCE(backward_ret_ortvalue_ptr != nullptr, "backward_ret_ortvalue_ptr should not be null");

  Tensor* t = backward_ret_ortvalue_ptr->GetMutable<Tensor>();
  const auto& input_shape = t->Shape();
  const auto num_dim = input_shape.NumDimensions();
  std::cout << "ortvalue addr:" << backward_ret_ortvalue_ptr << ", tenosr addr: " << t
            << ", tensor->MutableDataRaw() addr :" << reinterpret_cast<int64_t>(t->MutableDataRaw())
            << ", num_dim: " << num_dim << std::endl;

  for (size_t i = 0; i < num_dim; ++i) {
    std::cout << "PythonOp::Compute shape : " << input_shape.GetDims()[i] << std::endl;
  }
  ORT_RETURN_IF_ERROR(ctx_internal->SetOutputMLValue(0, *backward_ret_ortvalue_ptr));

  return Status::OK();
}

}  // namespace contrib
}  // namespace onnxruntime
