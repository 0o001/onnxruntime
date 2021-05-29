// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once
#include "core/providers/shared_library/provider_api.h"
#include "core/providers/cuda/cuda_kernel.h"

namespace onnxruntime {
namespace cuda {

static int64_t GetSeqIdx(const Tensor& idx_tensor) {
  int64_t seq_idx = INT_MAX;
  auto idx_tensor_dtype = idx_tensor.GetElementType();
  switch (idx_tensor_dtype) {
    case ONNX_NAMESPACE::TensorProto_DataType_INT32: {
      int32_t idx_data = 0;
      if (CUDA_CALL(cudaMemcpy(&idx_data, idx_tensor.Data<int32_t>(),
                               sizeof(int32_t), cudaMemcpyDeviceToHost))) {
        seq_idx = static_cast<int64_t>(idx_data);
      }
      break;
    }
    case ONNX_NAMESPACE::TensorProto_DataType_INT64: {
      int64_t idx_data = 0;
      if (CUDA_CALL(cudaMemcpy(&idx_data, idx_tensor.Data<int64_t>(),
                               sizeof(int64_t), cudaMemcpyDeviceToHost))) {
        seq_idx = idx_data;
      }
      break;
    }
    default:
      ORT_THROW("Sequence Ops GPU: Unsupported data type: ", idx_tensor_dtype);
  }
  return seq_idx;
}

class SequenceAt final: public CudaKernel {
 public:
  SequenceAt(const OpKernelInfo& info): CudaKernel(info) {}

  Status ComputeInternal(OpKernelContext* context) const override {
    const auto* X = context->Input<TensorSeq>(0);
    ORT_ENFORCE(X != nullptr, "SequenceAt GPU: Got nullptr for sequence input.");
    const auto* I = context->Input<Tensor>(1);
    ORT_ENFORCE(I != nullptr, "SequenceAt GPU: Got nullptr input for index tensor.");

    int64_t idx = GetSeqIdx(*I);
    int64_t sequence_size = static_cast<int64_t>(X->Size());
    if (idx < 0) {
      idx = sequence_size + idx;
    }
    ORT_ENFORCE(idx >= 0 && idx < sequence_size, "SequenceAt GPU: Invalid sequence index");

    const Tensor& source_tensor = X->Get(idx);
    auto source_type = source_tensor.DataType();
    const void* source_addr = source_tensor.DataRaw(source_type);
 
    Tensor* target_tensor = context->Output(0, source_tensor.Shape());
    ORT_ENFORCE(target_tensor != nullptr, "SequenceAt GPU: Got nullptr for output tensor");
    void* target_addr = target_tensor->MutableDataRaw(source_type);
 
    if (source_addr != target_addr) {
      CUDA_RETURN_IF_ERROR(cudaMemcpyAsync(target_addr,
                                           source_addr,
                                           source_tensor.SizeInBytes(),
                                           cudaMemcpyDeviceToDevice, Stream()));
    }
    return Status::OK();
  }
};

class SequenceConstruct final: public CudaKernel {
 public:
  SequenceConstruct(const OpKernelInfo& info): CudaKernel(info) {}

  Status ComputeInternal(OpKernelContext* context) const override {
    TensorSeq* Y = context->Output<TensorSeq>(0);
    ORT_ENFORCE(Y != nullptr, "SequenceConstruct: Got nullptr for output sequence");

    AllocatorPtr alloc;
    auto status = context->GetTempSpaceAllocator(&alloc);
    if (!status.IsOK()) {
      ORT_THROW("SequenceConstruct: Unable to get an allocator");
    }

    int32_t at = 0;
    const Tensor* source_tensor = nullptr;
    MLDataType data_type = nullptr;
    while (nullptr != (source_tensor = context->Input<Tensor>(at++))) {
      if (nullptr == data_type) {
        data_type = source_tensor->DataType();
        Y->SetType(data_type);
      } else {
        ORT_ENFORCE(data_type == source_tensor->DataType(),
                    "SequenceConstruct: inconsistent input type");
      }
      std::unique_ptr<Tensor> target_tensor = Tensor::Create(data_type,
                                                             source_tensor->Shape(), alloc);
      ORT_ENFORCE(target_tensor, "SequenceConstruct: Failed to allocate new tensor");
      CUDA_RETURN_IF_ERROR(cudaMemcpyAsync(target_tensor->MutableDataRaw(),
                                           source_tensor->DataRaw(),
                                           source_tensor->SizeInBytes(),
                                           cudaMemcpyDeviceToDevice, Stream()));
      Y->Add(std::move(*target_tensor));
      target_tensor.release();
    }
    ORT_ENFORCE(Y->Size() > 0, "SequenceConstruct: zero inputs");
    return Status::OK();
  }
};

class SequenceEmpty final: public CudaKernel {
 public:
  SequenceEmpty(const OpKernelInfo& info): CudaKernel(info) {
    if (!info.GetAttr("dtype", &dtype_).IsOK()) {
      dtype_ = ONNX_NAMESPACE::TensorProto_DataType_FLOAT;
    }
  }

  Status ComputeInternal(OpKernelContext* context) const override {
    TensorSeq* Y = context->Output<TensorSeq>(0);
    if (nullptr == Y) {
      return Status(common::ONNXRUNTIME, common::FAIL,
                    "SequenceEmpty: Failed to allocate tensor sequence.");
    }
    auto status = Status::OK();
    MLDataType seq_dtype{};
    switch (dtype_) {
      case ONNX_NAMESPACE::TensorProto_DataType_FLOAT:
        seq_dtype = DataTypeImpl::GetType<float>();
        break;
      case ONNX_NAMESPACE::TensorProto_DataType_BOOL:
        seq_dtype = DataTypeImpl::GetType<bool>();
        break;
      case ONNX_NAMESPACE::TensorProto_DataType_INT32:
        seq_dtype = DataTypeImpl::GetType<int>();
        break;
      case ONNX_NAMESPACE::TensorProto_DataType_DOUBLE:
        seq_dtype = DataTypeImpl::GetType<double>();
        break;
      case ONNX_NAMESPACE::TensorProto_DataType_STRING:
        seq_dtype = DataTypeImpl::GetType<std::string>();
        break;
      case ONNX_NAMESPACE::TensorProto_DataType_INT8:
        seq_dtype = DataTypeImpl::GetType<int8_t>();
        break;
      case ONNX_NAMESPACE::TensorProto_DataType_UINT8:
        seq_dtype = DataTypeImpl::GetType<uint8_t>();
        break;
      case ONNX_NAMESPACE::TensorProto_DataType_UINT16:
        seq_dtype = DataTypeImpl::GetType<uint16_t>();
        break;
      case ONNX_NAMESPACE::TensorProto_DataType_INT16:
        seq_dtype = DataTypeImpl::GetType<int16_t>();
        break;
      case ONNX_NAMESPACE::TensorProto_DataType_INT64:
        seq_dtype = DataTypeImpl::GetType<int64_t>();
        break;
      case ONNX_NAMESPACE::TensorProto_DataType_UINT32:
        seq_dtype = DataTypeImpl::GetType<uint32_t>();
        break;
      case ONNX_NAMESPACE::TensorProto_DataType_UINT64:
        seq_dtype = DataTypeImpl::GetType<uint64_t>();
        break;
      case ONNX_NAMESPACE::TensorProto_DataType_FLOAT16:
        seq_dtype = DataTypeImpl::GetType<MLFloat16>();
        break;
      case ONNX_NAMESPACE::TensorProto_DataType_BFLOAT16:
        seq_dtype = DataTypeImpl::GetType<BFloat16>();
        break;
      default:
        status = Status(common::ONNXRUNTIME, common::FAIL,
                        "SequenceEmpty: invalid tensor type");
    }
    Y->SetType(seq_dtype);
    return status;
  }
 private:
  int64_t dtype_{};
};

class SequenceLength final: public CudaKernel {
 public:
  SequenceLength(const OpKernelInfo& info): CudaKernel(info) {}

  Status ComputeInternal(OpKernelContext* context) const override {
    const auto* X = context->Input<TensorSeq>(0);
    if (nullptr == X) {
      return Status(common::ONNXRUNTIME, common::FAIL,
                    "SequenceLength: failed to get input tensor sequence.");
    }
    auto* Y = context->Output(0, {});
    if (nullptr == Y) {
      return Status(common::ONNXRUNTIME, common::FAIL,
                    "SequenceLength: failed to allocate output tensor.");
    }
    size_t X_size = static_cast<int64_t>(X->Size());
    CUDA_RETURN_IF_ERROR(cudaMemcpyAsync(Y->MutableDataRaw(),
                                         &X_size,
                                         sizeof(int64_t),
                                         cudaMemcpyHostToDevice, Stream()));
    return Status::OK();
  }
};
 
} // namespace cuda
} // namespace onnxruntime


