// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <string>
#include <vector>

#include "core/providers/qnn/builder/qnn_model_wrapper.h"
#include "core/providers/qnn/builder/op_builder_factory.h"
#include "core/providers/qnn/builder/qnn_utils.h"

#include "base_op_builder.h"

namespace onnxruntime {
namespace qnn {

class QdqOpBuilder : public BaseOpBuilder {
 public:
  QdqOpBuilder() : BaseOpBuilder("QdqOpBuilder") {}
  ORT_DISALLOW_COPY_ASSIGNMENT_AND_MOVE(QdqOpBuilder);

 protected:
  Status ProcessInputs(QnnModelWrapper& qnn_model_wrapper,
                       const NodeUnit& node_unit,
                       const logging::Logger& logger,
                       bool is_quantized_node,
                       std::vector<std::string>& input_names,
                       bool do_op_validation = false) const override ORT_MUST_USE_RESULT;

  Status ProcessAttributesAndOutputs(QnnModelWrapper& qnn_model_wrapper,
                                     const NodeUnit& node_unit,
                                     std::vector<std::string>&& input_names,
                                     const logging::Logger& logger,
                                     bool is_quantized_node,
                                     bool do_op_validation) const override ORT_MUST_USE_RESULT;
};

Status QdqOpBuilder::ProcessInputs(QnnModelWrapper& qnn_model_wrapper,
                                    const NodeUnit& node_unit,
                                    const logging::Logger& logger,
                                    bool is_quantized_node,
                                    std::vector<std::string>& input_names,
                                    bool do_op_validation) const {
  ORT_UNUSED_PARAMETER(do_op_validation);
  ORT_UNUSED_PARAMETER(is_quantized_node);

  // DequantizeLinear input is quantized tensor
  // QuantizeLinear input is non-quantized tensor
  bool is_quantized_tensor = node_unit.OpType() == "DequantizeLinear";

  const auto& inputs = node_unit.Inputs();
  ORT_RETURN_IF_ERROR(ProcessInput(qnn_model_wrapper, inputs[0], logger, is_quantized_tensor, input_names));

  return Status::OK();
}

Status QdqOpBuilder::ProcessAttributesAndOutputs(QnnModelWrapper& qnn_model_wrapper,
                                                  const NodeUnit& node_unit,
                                                  std::vector<std::string>&& input_names,
                                                  const logging::Logger& logger,
                                                  bool is_quantized_node,
                                                 bool do_op_validation) const {
  ORT_UNUSED_PARAMETER(is_quantized_node);
  if (input_names.size() < 1) {
    return Status::OK();
  }

  // QuantizeLinear output is quantized tensor
  // DequantizeLinear output is non-quantized tensor
  bool is_quantized_tensor = node_unit.OpType() == "QuantizeLinear";

  ORT_RETURN_IF_ERROR(ProcessOutputs(qnn_model_wrapper, node_unit, std::move(input_names), {},
                                     logger, is_quantized_tensor, do_op_validation,
                                     GetQnnOpType(node_unit.OpType())));
  return Status::OK();
}


void CreateQdqOpBuilder(const std::string& op_type, OpBuilderRegistrations& op_registrations) {
  op_registrations.AddOpBuilder(op_type, std::make_unique<QdqOpBuilder>());
}

}  // namespace qnn
}  // namespace onnxruntime
