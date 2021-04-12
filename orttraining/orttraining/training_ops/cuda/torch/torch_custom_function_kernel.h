// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "core/common/common.h"
#include "core/providers/cuda/cuda_kernel.h"
#include "core/language_interop_ops/pyop/pyop_lib_proxy.h"
#include "core/torch_custom_function/torch_custom_function_register.h"

namespace onnxruntime {
namespace cuda {

// Pytorch's torch.autograd.Function.apply(...) wrapper.
class PythonOp final : public CudaKernel {
 public:
  PythonOp(const OpKernelInfo& info) : CudaKernel(info) {
    ORT_THROW_IF_ERROR(info.GetAttr("name", &name_));
    inplace_ = info.GetAttrOrDefault("inplace", static_cast<int64_t>(0));
    ORT_THROW_IF_ERROR(info.GetAttr("call_convention", &call_convention_));

    // Input tensors.
    input_tensor_types_ = info.GetAttrsOrDefault("input_tensor_types", std::vector<int64_t>());
    input_tensor_requires_grads_ = info.GetAttrsOrDefault("input_tensor_requires_grads", std::vector<int64_t>());

    // Input int scalars.
    input_int_scalars_ = info.GetAttrsOrDefault("input_int_scalars", std::vector<int64_t>());
    input_int_scalar_positions_ = info.GetAttrsOrDefault("input_int_scalar_positions", std::vector<int64_t>());

    // Input float scalars.
    input_float_scalars_ = info.GetAttrsOrDefault("input_float_scalars", std::vector<float>());
    input_float_scalar_positions_ = info.GetAttrsOrDefault("input_float_scalar_positions", std::vector<int64_t>());

    // Input int tuples.
    input_int_tuples_ = info.GetAttrsOrDefault("input_int_tuples", std::vector<int64_t>());
    input_int_tuple_positions_ = info.GetAttrsOrDefault("input_int_tuple_positions", std::vector<int64_t>());
    input_int_tuple_begins_ = info.GetAttrsOrDefault("input_int_tuple_begins", std::vector<int64_t>());

    // Input float tuples.
    input_float_tuples_ = info.GetAttrsOrDefault("input_float_tuples", std::vector<float>());
    input_float_tuple_positions_ = info.GetAttrsOrDefault("input_float_tuple_positions", std::vector<int64_t>());
    input_float_tuple_begins_ = info.GetAttrsOrDefault("input_float_tuple_begins", std::vector<int64_t>());

    // Output tensors.
    output_tensor_types_ = info.GetAttrsOrDefault("output_tensor_types", std::vector<int64_t>());
    output_tensor_requires_grads_ = info.GetAttrsOrDefault("output_tensor_requires_grads", std::vector<int64_t>());

    std::string err;
    auto py_func = onnxruntime::python::OrtTorchFunctionPool::GetInstance().GetForward(name_);
    auto state = PyOpLibProxy::GetInstance().GetGil();
    ORT_ENFORCE(PyOpLibProxy::GetInstance().Initialized(), "Py library not properly initialized.");
    instance_ = PyOpLibProxy::GetInstance().NewInstance(reinterpret_cast<void*>(py_func));
    ORT_ENFORCE(instance_ != nullptr, "Python run instance_ should not be nullptr");
    PyOpLibProxy::GetInstance().PutGil(state);
    ORT_ENFORCE(nullptr != instance_, PyOpLibProxy::GetInstance().GetLastErrorMessage(err));
  };

  Status ComputeInternal(OpKernelContext* context) const override;

  ~PythonOp() {
    if (nullptr != instance_) {
      auto state = PyOpLibProxy::GetInstance().GetGil();
      PyOpLibProxy::GetInstance().ReleaseInstance(instance_);
      PyOpLibProxy::GetInstance().PutGil(state);
      instance_ = nullptr;
    }
  }

 private:
  void* instance_ = nullptr;

  // Name of containing class. For example, MyReLU.
  std::string name_;
  int64_t inplace_;
  std::string call_convention_;

  // Attributes of input tensors for calling MyReLU.apply(...).
  // Types. input_tensor_types_[i] is the element type of the i-th tensor.
  std::vector<int64_t> input_tensor_types_;
  // input_tensor_types_[i] indicates if the i-th tensor should have gradient.
  std::vector<int64_t> input_tensor_requires_grads_;

  // Concatenation of all floats from apply(...) 's inputs.
  std::vector<int64_t> input_int_scalars_;
  std::vector<int64_t> input_int_scalar_positions_;

  // Concatenation of all ints from apply(...) 's inputs.
  std::vector<float> input_float_scalars_;
  std::vector<int64_t> input_float_scalar_positions_;

  // Concatenation of all int tuples from apply(...) 's inputs.
  std::vector<int64_t> input_int_tuples_;
  std::vector<int64_t> input_int_tuple_positions_;
  std::vector<int64_t> input_int_tuple_begins_;

  // Concatenation of all float tuples from apply(...) 's inputs.
  std::vector<float> input_float_tuples_;
  std::vector<int64_t> input_float_tuple_positions_;
  std::vector<int64_t> input_float_tuple_begins_;

  // Output types of MyReLU.apply(...).
  std::vector<int64_t> output_tensor_types_;
  std::vector<int64_t> output_tensor_requires_grads_;
};

// Pytorch's torch.autograd.Function.backward(...) wrapper.
class PythonOpGrad final : public CudaKernel {
 public:
  PythonOpGrad(const OpKernelInfo& info) : CudaKernel(info) {
    ORT_THROW_IF_ERROR(info.GetAttr("name", &name_));
    ORT_THROW_IF_ERROR(info.GetAttrs("input_tensor_types", input_tensor_types_));
    ORT_THROW_IF_ERROR(info.GetAttrs("output_tensor_types", output_tensor_types_));

    std::string err;
    auto py_func = onnxruntime::python::OrtTorchFunctionPool::GetInstance().GetBackward(name_);
    auto state = PyOpLibProxy::GetInstance().GetGil();
    ORT_ENFORCE(PyOpLibProxy::GetInstance().Initialized(), "Py library not properly initialized.");
    instance_ = PyOpLibProxy::GetInstance().NewInstance(reinterpret_cast<void*>(py_func));
    ORT_ENFORCE(instance_ != nullptr, "Python run instance_ should not be nullptr");
    PyOpLibProxy::GetInstance().PutGil(state);
    ORT_ENFORCE(nullptr != instance_, PyOpLibProxy::GetInstance().GetLastErrorMessage(err));
  }

  Status ComputeInternal(OpKernelContext* context) const override;

  ~PythonOpGrad() {
    if (nullptr != instance_) {
      auto state = PyOpLibProxy::GetInstance().GetGil();
      PyOpLibProxy::GetInstance().ReleaseInstance(instance_);
      PyOpLibProxy::GetInstance().PutGil(state);
      instance_ = nullptr;
    }
  }

 private:
  // Name of containing class. For example, MyReLU.
  std::string name_;
  // Input types of MyReLU.backward(...).
  std::vector<int64_t> input_tensor_types_;
  // Output types of MyReLU.apply(...).
  std::vector<int64_t> output_tensor_types_;
  void* instance_ = nullptr;
};

}  // namespace cuda
}  // namespace onnxruntime
