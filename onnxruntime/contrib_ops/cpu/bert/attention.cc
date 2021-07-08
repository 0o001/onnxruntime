// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "attention_cpu_base.h"
#include "attention_helper.h"
#include "core/framework/tensorprotoutils.h"
#include "core/graph/onnx_protobuf.h"
#include "core/util/math.h"
#include "core/util/math_cpuonly.h"
#include "core/common/safeint.h"
#include "core/platform/threadpool.h"

using onnxruntime::concurrency::ThreadPool;

namespace onnxruntime {
namespace contrib {

template <typename T>
class Attention : public OpKernel, public AttentionCPUBase {
 public:
  explicit Attention(const OpKernelInfo& info);

  Status Compute(OpKernelContext* context) const override;

  Status PrePack(const Tensor& tensor, int input_idx, AllocatorPtr alloc,
                 /*out*/ bool& is_packed,
                 /*out*/ PrePackedWeights* prepacked_weights) override;

  Status UseSharedPrePackedBuffers(std::vector<BufferUniquePtr>& prepacked_buffers,
                                   int input_idx,
                                   /*out*/ bool& used_shared_buffers) override;

 private:
  BufferUniquePtr packed_weights_;
  size_t packed_weights_size_ = 0;

  BufferUniquePtr qk_packed_weights_;
  BufferUniquePtr v_packed_weights_;

  size_t qk_packed_weights_size_ = 0;
  size_t v_packed_weights_size_ = 0;
  TensorShape weight_shape_;
};

// These ops are internal-only, so register outside of onnx
ONNX_OPERATOR_TYPED_KERNEL_EX(
    Attention,
    kMSDomain,
    1,
    float,
    kCpuExecutionProvider,
    KernelDefBuilder()
        .TypeConstraint("T", DataTypeImpl::GetTensorType<float>()),
    Attention<float>);

Status AttentionBase::CheckInputs(const TensorShape& input_shape,
                                  const TensorShape& weights_shape,
                                  const TensorShape& bias_shape,
                                  const Tensor*& mask_index,
                                  const Tensor* past) const {
  // Input shapes:
  //   input       : (batch_size, sequence_length, input_hidden_size)
  //   weights     : (input_hidden_size, 3 * hidden_size)
  //   bias        : (3 * hidden_size)
  //   mask_index  : nullptr, (batch_size), (2 * batch_size),
  //                 or (batch_size, 1), (1, 1)
  //                 or (batch_size, past_sequence_length + sequence_length)
  //                 or (batch_size, sequence_length, past_sequence_length + sequence_length)
  //   past        : (2, batch_size, num_heads, past_sequence_length, head_size)
  //   extra_add_qk: (batch_size, num_heads, sequence_length, sequence_length)
  //
  // Where hidden_size = num_heads * head_size.
  // When a model is pruned (like some attention heads are removed), hidden_size < input_hidden_size.

  const auto& dims = input_shape.GetDims();
  if (dims.size() != 3) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Input 'input' is expected to have 3 dimensions, got ",
                           dims.size());
  }
  int batch_size = static_cast<int>(dims[0]);
  int sequence_length = static_cast<int>(dims[1]);

  const auto& weights_dims = weights_shape.GetDims();
  if (weights_dims.size() != 2) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Input 'weights' is expected to have 2 dimensions, got ",
                           weights_dims.size());
  }
  if (weights_dims[0] != dims[2]) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                           "Input 1 dimension 0 should have same length as dimension 2 of input 0");
  }

  const auto& bias_dims = bias_shape.GetDims();
  if (bias_dims.size() != 1) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Input 'bias' is expected to have 1 dimension, got ",
                           bias_dims.size());
  }

  int hidden_size = 0;

  if (qkv_hidden_sizes_.size() == 0) {
    hidden_size = static_cast<int>(weights_dims[1]) / 3;
    if (3 * hidden_size != static_cast<int>(weights_dims[1])) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "Input 1 dimension 1 should be 3 times of hidden dimension");
    }

    if (bias_dims[0] != weights_dims[1]) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "Input 'bias' dimension 0 should have same length as dimension 1 of input 'weights'");
    }

    if (hidden_size % num_heads_ != 0) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "hidden_size should be divisiable by num_heads.");
    }
  } else {
    int qkv_sizes = 0;

    for (size_t i = 0; i < qkv_hidden_sizes_.size(); i++) {
      qkv_sizes += static_cast<int>(qkv_hidden_sizes_[i]);
    }

    int qkv_hidden_sizes_sum = static_cast<int>(weights_dims[1]);
    if (qkv_hidden_sizes_sum != qkv_sizes) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "qkv_sizes doesn't match the wights dimension");
    }

    //TODO
    // Do all the verifications on hidden layers here:
    // 1. q_hidden_size == k_hidden_size for weights
    // 2. bias sizes  same for Q, K, V paths
    // 3. q, k, v sizes are divisible by num_heads_
  }

  if (hidden_size % num_heads_ != 0) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "hidden_size should be divisiable by num_heads.");
  }

  //TODO how this changes with new params
  int past_sequence_length = 0;
  if (past != nullptr) {  // past is optional
    const auto& past_dims = past->Shape().GetDims();
    if (past_dims.size() != 5) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Input 'past' is expected to have 5 dimension, got ",
                             past_dims.size());
    }
    if (static_cast<int>(past_dims[0]) != 2) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Inputs 'past' dimension 0 shall have length of 2");
    }
    if (static_cast<int>(past_dims[1]) != batch_size) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Inputs 'past' dimension 1 shall have same length as dimension 0 of input 0");
    }
    if (static_cast<int>(past_dims[2]) != num_heads_) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Inputs 'past' dimension 2 shall have length of num_heads", num_heads_);
    }
    if (static_cast<int>(past_dims[4]) != hidden_size / num_heads_) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Inputs 'past' dimension 2 shall have length of ", hidden_size / num_heads_);
    }
    past_sequence_length = static_cast<int>(past_dims[3]);
  }

  if (mask_index != nullptr) {  // mask_index is optional
    const auto& mask_dims = mask_index->Shape().GetDims();
    if (mask_dims.size() == 1) {
      if (static_cast<int>(mask_dims[0]) != batch_size && static_cast<int>(mask_dims[0]) != 2 * batch_size) {
        return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Inputs 'mask_index' with 1D data shall have length of batch_size or 2 * batch_size");
      }
    } else if (mask_dims.size() == 2) {
      if (static_cast<int>(mask_dims[0]) != batch_size || static_cast<int>(mask_dims[1]) != past_sequence_length + sequence_length) {
        // Add operator supports broadcasting. Here we handle a case with only one element in the 2nd dimension.
        if ((static_cast<int>(mask_dims[0]) == batch_size || static_cast<int>(mask_dims[0]) == 1) && static_cast<int>(mask_dims[1]) == 1) {
          // Mask will have same value after propogation, which has same effect as no mask.
          mask_index = nullptr;
        } else {
          return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Inputs 'mask_index' with 2D data shall have shape batch_size x (past_sequence_length + sequence_length)");
        }
      }
    } else if (mask_dims.size() == 3) {
      if (static_cast<int>(mask_dims[0]) != batch_size || mask_dims[1] != sequence_length || static_cast<int>(mask_dims[2]) != past_sequence_length + sequence_length) {
        return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Inputs 'mask_index' with 3D data shall have shape batch_size x sequence_length x (past_sequence_length + sequence_length)");
      }
    } else if (mask_dims.size() == 4) {
      if (static_cast<int>(mask_dims[0]) != batch_size || mask_dims[1] != 1 || mask_dims[2] != mask_dims[3] || mask_dims[2] < past_sequence_length + sequence_length) {
        return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Inputs 'mask_index' with 4D data shall have shape batch_size x 1 x max_sequence_length x max_sequence_length)");
      }
      if (is_unidirectional_ == true) {
        return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Inputs 'mask_index' with 4D data shall have is_unidirectional_ set to false");
      }
    } else {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Input 'mask_index' is expected to have 1, 2, 3 or 4 dimensions, got ",
                             mask_dims.size());
    }
  }

  return Status::OK();
}

Status AttentionBase::CheckInputs(const TensorShape& input_shape,
                                  const TensorShape& weights_shape,
                                  const TensorShape& bias_shape,
                                  const Tensor*& mask_index,
                                  const Tensor* past,
                                  const int max_threads_per_block) const {
  if (num_heads_ > max_threads_per_block) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "num_heads should be no larger than ", max_threads_per_block);
  }

  return CheckInputs(input_shape, weights_shape, bias_shape, mask_index, past);
}

Tensor* AttentionBase::GetPresent(OpKernelContext* context,
                                  const Tensor* past,
                                  int batch_size,
                                  int head_size,
                                  int sequence_length,
                                  int& past_sequence_length) const {
  // Input and output shapes:
  //   past        : (2, batch_size, num_heads, past_sequence_length, head_size)
  //   present     : (2, batch_size, num_heads, past_sequence_length + sequence_length, head_size)

  std::vector<int64_t> present_dims{2, batch_size, num_heads_, sequence_length, head_size};
  if (nullptr != past) {
    const auto& past_dims = past->Shape().GetDims();
    past_sequence_length = static_cast<int>(past_dims[3]);
    present_dims[3] += past_dims[3];
  }

  TensorShape present_shape(present_dims);
  Tensor* present = context->Output(1, present_shape);
  if (nullptr != past && nullptr == present) {
    ORT_THROW("Expect to have present state output when past state input is given");
  }

  return present;
}

template <typename T>
Attention<T>::Attention(const OpKernelInfo& info) : OpKernel(info), AttentionCPUBase(info) {
}

template <typename T>
Status Attention<T>::PrePack(const Tensor& weights, int input_idx, AllocatorPtr alloc,
                             /*out*/ bool& is_packed,
                             /*out*/ PrePackedWeights* prepacked_weights) {
  is_packed = false;
  
  //prepacked_weights = nullptr;
  //if (input_idx == 10000) {
  //    std::cout << weights.Shape() <<std::endl;
  //}
  //
  //return Status::OK();

  if (1 != input_idx) {
    return Status::OK();
  }

  weight_shape_ = weights.Shape();
  const auto& weights_dims = weight_shape_.GetDims();
  if (weights_dims.size() != 2) {
    return Status::OK();
  }

  const auto* weights_data = weights.Data<T>();

#ifdef PRINT_PREPACK
  for (int64_t i = 0; i < weights_dims[0]; i++) {
    for (int64_t j = 0; j < weights_dims[1]; j++) {
      std::cout << weights_data[i * weights_dims[1] + j] << ",";
    }
    std::cout << std::endl;
  }
#endif

  const size_t input_hidden_size = static_cast<size_t>(weights_dims[0]);
  const size_t hidden_size_x3 = static_cast<size_t>(weights_dims[1]);

  size_t qk_hidden_size;
  size_t v_hidden_size;
  if (qkv_hidden_sizes_.size() != 0) {
    qk_hidden_size = static_cast<size_t>(qkv_hidden_sizes_[0]);
    v_hidden_size = static_cast<size_t>(qkv_hidden_sizes_[2]);

    const size_t qk_head_size = qk_hidden_size / num_heads_;
    const size_t v_head_size = v_hidden_size / num_heads_;

    if (qk_hidden_size == 0 || v_hidden_size == 0 || (qk_hidden_size % num_heads_) != 0 || (v_hidden_size % num_heads_) != 0) {
      return Status::OK();
    }

    qk_packed_weights_size_ = MlasGemmPackBSize(qk_head_size, input_hidden_size);
    if (qk_packed_weights_size_ == 0) {
      return Status::OK();
    }

    size_t loop_len = static_cast<size_t>(2) * num_heads_;
    size_t qk_packed_weights_data_size = qk_packed_weights_size_ * loop_len;  // The same size would be computed by AllocArray() below
    auto* packed_weights_data = static_cast<uint8_t*>(alloc->AllocArray(qk_packed_weights_size_, loop_len));
#ifdef PRINT_PREPACK
    auto* packed_weights_data_float = reinterpret_cast<float*> (packed_weights_data);
#endif
    memset(packed_weights_data, 0, qk_packed_weights_data_size);
    qk_packed_weights_ = BufferUniquePtr(packed_weights_data, BufferDeleter(alloc));

    for (size_t i = 0; i < loop_len; i++) {
      MlasGemmPackB(CblasNoTrans, qk_head_size, input_hidden_size, weights_data, size_t(2) * qk_hidden_size + v_hidden_size, packed_weights_data);
      packed_weights_data += qk_packed_weights_size_;
      weights_data += qk_head_size;
    }
    
#ifdef PRINT_PREPACK
    std::cout<<"Printing from 'qk packed data' packed_weights_data_float:"<<std::endl;
    for (size_t i = 0; i < loop_len; i++) {
      size_t st = qk_packed_weights_size_/(sizeof(float)/sizeof(uint8_t));
      for (size_t j = 0; j < st; j++) {
        if (j % 16 == 0) {
          std::cout<< std::endl;
        }
        std::cout << packed_weights_data_float[i * st + j] << ",";
      }
      std::cout << std::endl;
    }
#endif

    v_packed_weights_size_ = MlasGemmPackBSize(v_head_size, input_hidden_size);
    if (v_packed_weights_size_ == 0) {
      return Status::OK();
    }

    loop_len = static_cast<size_t>(1) * num_heads_;
    size_t v_packed_weights_data_size = v_packed_weights_size_ * loop_len;  // The same size would be computed by AllocArray() below
    auto* packed_weights_data_2 = static_cast<uint8_t*>(alloc->AllocArray(v_packed_weights_size_, loop_len));
#ifdef PRINT_PREPACK
    auto* packed_weights_data_2_float = reinterpret_cast<float*>(packed_weights_data_2);
#endif
    memset(packed_weights_data_2, 0, v_packed_weights_data_size);
    v_packed_weights_ = BufferUniquePtr(packed_weights_data_2, BufferDeleter(alloc));

    for (size_t i = 0; i < loop_len; i++) {
      MlasGemmPackB(CblasNoTrans, v_head_size, input_hidden_size, weights_data, size_t(2) * qk_hidden_size + v_hidden_size, packed_weights_data_2);
      packed_weights_data_2 += v_packed_weights_size_;
      weights_data += v_head_size;
    }

#ifdef PRINT_PREPACK
    std::cout<<"Printing from 'v packed data' packed_weights_data_float:"<<std::endl;
    for (size_t i = 0; i < loop_len; i++) {
      size_t st = v_packed_weights_size_/(sizeof(float)/sizeof(uint8_t));
      for (size_t j = 0; j < st; j++) {
        if (j % 16 == 0) {
          std::cout<< std::endl;
        }
        std::cout << packed_weights_data_2_float[i * st + j] << ",";
      }
      std::cout << std::endl;
    }
#endif
    
    bool share_prepacked_weights = (prepacked_weights != nullptr);
    if (share_prepacked_weights) {
      prepacked_weights->buffers_.push_back(std::move(qk_packed_weights_));
      prepacked_weights->buffer_sizes_.push_back(qk_packed_weights_data_size);
      prepacked_weights->buffers_.push_back(std::move(v_packed_weights_));
      prepacked_weights->buffer_sizes_.push_back(v_packed_weights_data_size);
    }
  } else {
    const size_t hidden_size = hidden_size_x3 / 3;
    const size_t head_size = hidden_size / num_heads_;

    // Bail out if the weights shape has an expected shape.
    if ((hidden_size == 0) || ((hidden_size % num_heads_) != 0) || (hidden_size_x3 != 3 * hidden_size)) {
      return Status::OK();
    }

    // const auto* weights_data = weights.Data<T>();

    packed_weights_size_ = MlasGemmPackBSize(head_size, input_hidden_size);
    if (packed_weights_size_ == 0) {
      return Status::OK();
    }

#ifdef PRINT_PREPACK
    std::cout<<"Packed weight size:"<<packed_weights_size_<<std::endl;
#endif

    const size_t loop_len = static_cast<size_t>(3) * num_heads_;
    size_t packed_weights_data_size = packed_weights_size_ * loop_len;  // The same size would be computed by AllocArray() below
    auto* packed_weights_data = static_cast<uint8_t*>(alloc->AllocArray(packed_weights_size_, loop_len));
#ifdef PRINT_PREPACK
    auto* packed_weights_data_float = reinterpret_cast<float*> (packed_weights_data);
#endif
    // Initialize memory to 0 as there could be some padding associated with pre-packed
    // buffer memory and we don not want it uninitialized and generate different hashes
    // if and when we try to cache this pre-packed buffer for sharing between sessions.
    memset(packed_weights_data, 0, packed_weights_data_size);
    packed_weights_ = BufferUniquePtr(packed_weights_data, BufferDeleter(alloc));

    for (size_t i = 0; i < loop_len; i++) {
      MlasGemmPackB(CblasNoTrans, head_size, input_hidden_size, weights_data, hidden_size_x3, packed_weights_data);

      //auto* packed_weights_data_temp = reinterpret_cast<float*> (packed_weights_data);
      //for (size_t j = 0; j < packed_weights_size_; j++) {
      //    if (j % hidden_size == 0) {
      //      std::cout<< std::endl;
      //    }
      //    std::cout << packed_weights_data_temp[j] << ",";
      //}
      //std::cout<<std::endl;

      packed_weights_data += packed_weights_size_;
      weights_data += head_size;
    }

#ifdef PRINT_PREPACK
    std::cout<<"Printing from packed_weights_data_float:"<<std::endl;
    for (size_t i = 0; i < loop_len; i++) {
      size_t st = packed_weights_size_/(sizeof(float)/sizeof(uint8_t));
      for (size_t j = 0; j < st; j++) {
        if (j % hidden_size == 0) {
          std::cout<< std::endl;
        }
        std::cout << packed_weights_data_float[i * st + j] << ",";
      }
      std::cout << std::endl;
    }
#endif

    bool share_prepacked_weights = (prepacked_weights != nullptr);
    if (share_prepacked_weights) {
      prepacked_weights->buffers_.push_back(std::move(packed_weights_));
      prepacked_weights->buffer_sizes_.push_back(packed_weights_data_size);
    }
  }

  is_packed = true;
  return Status::OK();
}

template <typename T>
Status Attention<T>::UseSharedPrePackedBuffers(std::vector<BufferUniquePtr>& prepacked_buffers,
                                               int input_idx,
                                               /*out*/ bool& used_shared_buffers) {
  if (1 != input_idx) {
    return Status::OK();
  }

  used_shared_buffers = true;
  packed_weights_ = std::move(prepacked_buffers[0]);

  return Status::OK();
}

template <typename T>
Status Attention<T>::Compute(OpKernelContext* context) const {
  const Tensor* input = context->Input<Tensor>(0);
  const Tensor* weights = (packed_weights_ || qk_packed_weights_) ? nullptr : context->Input<Tensor>(1);
  const Tensor* bias = context->Input<Tensor>(2);

  const Tensor* mask_index = context->Input<Tensor>(3);
  const Tensor* past = context->Input<Tensor>(4);
  const Tensor* extra_add_qk = context->Input<Tensor>(5);

  const TensorShape& weights_shape = (weights ? weights->Shape() : weight_shape_);
  ORT_RETURN_IF_ERROR(CheckInputs(input->Shape(),
                                  weights_shape,
                                  bias->Shape(),
                                  mask_index,
                                  past));

  const auto& shape = input->Shape().GetDims();
  const int batch_size = static_cast<int>(shape[0]);
  const int sequence_length = static_cast<int>(shape[1]);
  const int input_hidden_size = static_cast<int>(shape[2]);

  //TODO move this into CheckInputs
  const T* extra_add_qk_data = extra_add_qk != nullptr ? extra_add_qk->template Data<T>(): nullptr;
  const std::vector<int64_t>* extra_add_qk_dims = extra_add_qk_data != nullptr ? &(extra_add_qk->Shape().GetDims()) : nullptr;

  // LHS is uint64_t, RHS are ints, how is C++ allowing this.
  if (extra_add_qk_dims != nullptr) {
      if (extra_add_qk_dims->size() != 4) {
          return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Input 'extra_add_qk' is expected to have 4 dimensions, got ",
                           extra_add_qk_dims->size());
      }

      if (extra_add_qk_dims->at(1) != num_heads_) {
          return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Input 'extra_add_qk' dimension 1 should be same as number of heads, got ",
                           extra_add_qk_dims->at(1));
      }
      if (extra_add_qk_dims->at(2) != sequence_length) {
          return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Input 'extra_add_qk' dimension 2 should be same as sequence_length, got ",
                           extra_add_qk_dims->at(2));
      }
      if (extra_add_qk_dims->at(3) != sequence_length) {
          return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Input 'extra_add_qk' dimension 3 should be same as sequence_length, got ",
                           extra_add_qk_dims->at(3));
      }
  }

  int hidden_size;

  if (qkv_hidden_sizes_.size() == 0) {
    const auto& weights_dims = weights_shape.GetDims();
    hidden_size = static_cast<int>(weights_dims[1]) / 3;
  } else {
    hidden_size = static_cast<int>(qkv_hidden_sizes_[2]);
  }

  const int head_size = hidden_size / num_heads_;

  std::vector<int64_t> output_shape(3);
  output_shape[0] = shape[0];
  output_shape[1] = shape[1];
  output_shape[2] = static_cast<int64_t>(hidden_size);
  Tensor* output = context->Output(0, output_shape);

  constexpr size_t element_size = sizeof(T);

  int q_hidden_size = 0;
  int k_hidden_size = 0;
  int v_hidden_size = 0;
  if (qkv_hidden_sizes_.size() == 0) {
    q_hidden_size = hidden_size;
    k_hidden_size = hidden_size;
    v_hidden_size = hidden_size;
  } else {
    q_hidden_size = static_cast<int>(qkv_hidden_sizes_[0]);
    k_hidden_size = static_cast<int>(qkv_hidden_sizes_[1]);
    v_hidden_size = static_cast<int>(qkv_hidden_sizes_[2]);
  }

  int qk_head_size = q_hidden_size / num_heads_;
  int v_head_size = v_hidden_size / num_heads_;

  AllocatorPtr allocator;
  ORT_RETURN_IF_ERROR(context->GetTempSpaceAllocator(&allocator));

  auto* tp = context->GetOperatorThreadPool();
  // Compute Q, K, V
  // gemm_data(BS, 3NH) = input(BS, D) x weights(D, 3NH) + bias(3NH)
  // D (input_hidden_size) is hidden dimension of input, where D could be larger than hidden_size (NH) when model is pruned.
  auto gemm_data = allocator->Alloc(SafeInt<size_t>(batch_size) * sequence_length * (q_hidden_size + k_hidden_size + v_hidden_size) * element_size);
  BufferUniquePtr gemm_buffer(gemm_data, BufferDeleter(allocator));

  auto Q = reinterpret_cast<T*>(gemm_data);
  //auto K = Q + static_cast<size_t>(batch_size) * sequence_length * hidden_size;
  auto K = Q + static_cast<size_t>(batch_size) * sequence_length * q_hidden_size;

  //auto V = K + static_cast<size_t>(batch_size) * sequence_length * hidden_size;
  auto V = K + static_cast<size_t>(batch_size) * sequence_length * k_hidden_size;

  T* QKV[3] = {Q, K, V};

  {
    const int loop_len = 3 * batch_size * num_heads_;
    const auto* input_data = input->template Data<T>();
    const auto* weights_data = weights ? weights->template Data<T>() : nullptr;
    const auto* bias_data = bias->template Data<T>();

    // TODO
    // This cost is not exactly correct as for qk because the varied head size.
    const double cost =
        static_cast<double>(sequence_length) * static_cast<double>(head_size) * static_cast<double>(input_hidden_size);
    ThreadPool::TryParallelFor(tp, loop_len, cost, [&](std::ptrdiff_t begin, std::ptrdiff_t end) {
      for (std::ptrdiff_t i = begin; i != end; ++i) {
        const int batch_index = static_cast<int>((i / 3) / num_heads_);
        const int head_index = static_cast<int>((i / 3) % num_heads_);
        const int qkv_index = static_cast<int>(i % 3);

        int input_offset = 0;
        int weights_offset = 0;
        T *qkv_dest = nullptr;
        int qkv_offset = 0;

        if (qkv_hidden_sizes_.size() == 0) {
          input_offset = batch_index * sequence_length * input_hidden_size;
          weights_offset = qkv_index * hidden_size + head_index * head_size;
          qkv_dest = QKV[qkv_index];
          qkv_offset = (batch_index * num_heads_ + head_index) * (sequence_length * head_size);
          
          // TODO!! memcpy here makes it not worthwhile to use Gemm batch. Possible to post process?
          // broadcast 3NH -> (3.B.N.S.H)
          const T* broadcast_data_src = bias_data + weights_offset;
          T* broadcast_data_dest = QKV[qkv_index] + qkv_offset;
#ifdef PRINT_COMPUTE
          if (qkv_index == 2) {
            std::cout<<"\nadd for v_hidden_size:"<<std::endl;
            for (int i100 = 0; i100 < v_head_size; i100++) {             
              std::cout<<broadcast_data_src[i100]<<",";
            }
            std::cout<<std::endl;
          }  
#endif
          for (int seq_index = 0; seq_index < sequence_length; seq_index++) {
            memcpy(broadcast_data_dest, broadcast_data_src, head_size * sizeof(T));
            broadcast_data_dest += head_size;
          }
        }
        else {
          input_offset = batch_index * sequence_length * input_hidden_size;
          weights_offset = 0; 
          int bias_offset = 0;
          if (qkv_index == 1) {
            weights_offset += q_hidden_size;
            bias_offset += q_hidden_size;
          }
          
          if (qkv_index == 2) {
            bias_offset += q_hidden_size + k_hidden_size;
          }
          
          if (qkv_index == 0 || qkv_index == 1) {
            weights_offset += head_index * qk_head_size;
          } else {
            if (qk_packed_weights_ == nullptr){
              weights_offset += q_hidden_size + k_hidden_size;
            }
            weights_offset += head_index * v_head_size;
          }
          
          qkv_dest = QKV[qkv_index];
          qkv_offset = 0;
          if (qkv_index <= 1) {
            qkv_offset += (batch_index * num_heads_ + head_index) * (sequence_length * qk_head_size);
            bias_offset += head_index * qk_head_size;
          } else if (qkv_index == 2) {
            qkv_offset += (batch_index * num_heads_ + head_index) * (sequence_length * v_head_size);
            bias_offset += head_index * v_head_size;
          }
          
          // TODO!! memcpy here makes it not worthwhile to use Gemm batch. Possible to post process?
          // broadcast 3NH -> (3.B.N.S.H)
          const T* broadcast_data_src = bias_data + bias_offset;
          T* broadcast_data_dest = QKV[qkv_index] + qkv_offset;
#ifdef PRINT_COMPUTE
          if (qkv_index == 2) {
              std::cout<<"\nadd for v_hidden_size:"<<std::endl;
              for (int i100 = 0; i100 < v_head_size; i100++) {             
                std::cout<<broadcast_data_src[i100]<<",";
              }
              std::cout<<std::endl;
          }
          else {
              std::cout<<"\nadd for qk_hidden_size:"<<std::endl;
              for (int i100 = 0; i100 < qk_head_size; i100++) {             
                std::cout<<broadcast_data_src[i100]<<",";
              }
              std::cout<<std::endl;
          }
#endif
          for (int seq_index = 0; seq_index < sequence_length; seq_index++) {
            if (qkv_index <= 1) {
              memcpy(broadcast_data_dest, broadcast_data_src, qk_head_size * sizeof(T));
              broadcast_data_dest += qk_head_size;
            } else {
              memcpy(broadcast_data_dest, broadcast_data_src, v_head_size * sizeof(T));
              broadcast_data_dest += v_head_size;
            }
          }
        }

        //                   original           transposed            iteration
        // A: input          (BxSxD)            (B.)S x D             S x D
        // B: weights        (Dx3xNxH)          D x (3.N.)H           D x H
        // C: QKV[qkv_index] (3xBxNxSxH)        (3.B.N.)S x H         S x H
        if (packed_weights_ || qk_packed_weights_) {
          uint8_t* packed_weight;
          int head_size_passed_in = head_size;
          if (qkv_hidden_sizes_.size() == 0) {
            packed_weight = static_cast<uint8_t*>(packed_weights_.get()) + packed_weights_size_ * (weights_offset / head_size);
          } else {
            if (qkv_index <= 1) {
              packed_weight = static_cast<uint8_t*>(qk_packed_weights_.get()) + qk_packed_weights_size_ * (weights_offset / qk_head_size);
              head_size_passed_in = qk_head_size;
            } else {
              packed_weight = static_cast<uint8_t*>(v_packed_weights_.get()) + v_packed_weights_size_ * (weights_offset / v_head_size);
              head_size_passed_in = v_head_size;
            }
          }
#ifdef PRINT_COMPUTE
          std::cout<<"QKV index:"<< qkv_index << ",head_index:" << head_index << ",weights_offset:" << weights_offset<< ",head_size_passed_in:"<< head_size_passed_in<< std::endl;
          std::cout<<"v:qk"<<v_packed_weights_size_ <<":"<<qk_packed_weights_size_<<std::endl;
          std::cout<<reinterpret_cast<float*>(packed_weight)[0]<<std::endl;
#endif
          MlasGemm(
                CblasNoTrans,               // TransA = no
                sequence_length,            // M      = S
                head_size_passed_in,                  // N      = H
                input_hidden_size,          // K      = D
                1.0f,                       // alpha
                input_data + input_offset,  // A
                input_hidden_size,          // lda    = D
                packed_weight,              // B
                1.0f,                       // beta
                qkv_dest + qkv_offset,      // C
                head_size_passed_in,                  // ldc
                nullptr);                   // use single-thread
        } else {
          int head_size_passed_in = 0;
          if (qkv_index <= 1) {
            head_size_passed_in = qk_head_size;
          } else {
            head_size_passed_in = v_head_size;
          }

          math::GemmEx<float, ThreadPool>(
              CblasNoTrans,                   // TransA = no
              CblasNoTrans,                   // TransB = no
              sequence_length,                // M      = S
              //head_size                     // N      = H  
              head_size_passed_in,            // N      = H
              input_hidden_size,              // K      = D
              1.0f,                           // alpha
              input_data + input_offset,      // A
              input_hidden_size,              // lda    = D
              weights_data + weights_offset,  // B
              //3 * hidden_size,                // ldb    = 3NH
              q_hidden_size + k_hidden_size + v_hidden_size,
              1.0f,                           // beta
              qkv_dest + qkv_offset,          // C
              //head_size,                    // ldc
              head_size_passed_in,            // ldc
              nullptr                         // use single-thread
          );
        }
      }
    });
  }

  // Compute the attention score and apply the score to V
  return ApplyAttention(Q, K, V, mask_index, past, output,
                        batch_size, sequence_length,
                        qk_head_size, v_head_size, v_hidden_size,
                        extra_add_qk, context);
}
}  // namespace contrib
}  // namespace onnxruntime
