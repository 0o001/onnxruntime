// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "core/providers/cuda/cuda_kernel.h"

namespace onnxruntime {
namespace contrib {
namespace cuda {

using namespace onnxruntime::cuda;

class FasterTransformerBert final : public CudaKernel {
 public:
  explicit FasterTransformerBert(const OpKernelInfo& info);

  Status ComputeInternal(OpKernelContext* context) const override;

/* int bertExample(size_t batch_size,
                size_t num_layers,
                size_t seq_len,
                size_t head_num,
                size_t size_per_head,
                bool   is_remove_padding); */
};

}  // namespace cuda
}  // namespace contrib
}  // namespace onnxruntime
