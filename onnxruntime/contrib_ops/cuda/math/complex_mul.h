// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "core/providers/cuda/cuda_kernel.h"

namespace onnxruntime {
namespace contrib {
namespace cuda {

using namespace ::onnxruntime::cuda;

template <typename T, bool IsConj>
class ComplexMul : public CudaKernel {
 public:
  ComplexMul(const OpKernelInfo& info) : CudaKernel{info} {}
  Status ComputeInternal(OpKernelContext* context) const override;
};

}  // namespace cuda
}  // namespace contrib
}  // namespace onnxruntime
