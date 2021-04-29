// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "torch_custom_function_register.h"

namespace onnxruntime {
namespace python {

void OrtTorchFunctionPool::RegisterForward(
    const std::string& custom_function_name,
    pybind11::object forward_fn) {
  // This should be "apply" of
  // class custom_function_name(autograd.Function):
  //   @staticmethod
  //   def forward(ctx, ...):
  //     ...
  //   @staticmethod
  //   def backward(ctx, ...):
  //     ...
  // That is,
  //   forward_fn = custom_function_name.apply
  forward_pool[custom_function_name] = forward_fn;
}

void OrtTorchFunctionPool::RegisterBackward(
    const std::string& custom_function_name,
    pybind11::object backward_fn) {
  // This should be "backward" of
  // class custom_function_name(autograd.Function):
  //   @staticmethod
  //   def forward(ctx, ...):
  //     ...
  //   @staticmethod
  //   def backward(ctx, ...):
  //     ...
  // That is,
  //   backward_fn = custom_function_name.backward
  backward_pool[custom_function_name] = backward_fn;
}

pybind11::object OrtTorchFunctionPool::GetForward(
    const std::string& custom_function_name) {
  return forward_pool.at(custom_function_name);
};

pybind11::object OrtTorchFunctionPool::GetBackward(
    const std::string& custom_function_name) {
  return backward_pool.at(custom_function_name);
};

}  // namespace python
}  // namespace onnxruntime
