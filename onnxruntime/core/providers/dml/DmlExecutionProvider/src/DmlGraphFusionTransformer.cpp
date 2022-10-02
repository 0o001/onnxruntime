#pragma once

#include "precomp.h"
#include "GraphDescBuilder.h"
#include "ExecutionProvider.h"
#include "DmlGraphFusionTransformer.h"
#include "GraphPartitioner.h"
#include "core/framework/kernel_type_str_resolver.h"
#include "core/framework/kernel_lookup.h"
#include "FusedGraphKernel.h"
#include "MLOperatorAuthorImpl.h"
#include "GraphKernelHelper.h"


namespace Dml
{
    DmlGraphFusionTransformer::DmlGraphFusionTransformer(
        const std::string& name,
        const onnxruntime::IExecutionProvider* provider
    )
        :onnxruntime::GraphTransformer(name),
         m_providerImpl(static_cast<const ExecutionProvider*>(provider)->GetImpl())
    {
    }
	
    std::unordered_map<const onnx::TensorProto*, std::vector<uint32_t>>
    GetInitializerToPartitionMap(
        const onnxruntime::GraphViewer& graph,
        gsl::span<std::unique_ptr<GraphPartition>> partitions
    )
    {
        std::unordered_map<const onnx::TensorProto*, std::vector<uint32_t>> initializerPartitionMap;
        for (uint32_t partitionIndex = 0; partitionIndex < gsl::narrow_cast<uint32_t>(partitions.size()); ++partitionIndex)
        {
            auto& partition = partitions[partitionIndex];

            // Skip partitions which have been merged into other partitions
            if (partition->GetRootMergedPartition() != partition.get())
            {
                continue;
            }

            for (const std::string& input : partition->GetInputs())
            {
                const onnx::TensorProto* tensor = nullptr;
                if (graph.GetInitializedTensor(input, tensor))
                {
                    initializerPartitionMap[tensor].push_back(partitionIndex);
                }
            }
        }

        return initializerPartitionMap;
    }

    void ConvertGraphDesc(
        const Dml::GraphDescBuilder::GraphDesc& graphDesc,
        _Out_ DML_GRAPH_DESC& dmlGraphDesc,
        const uint32_t inputCount,
        const uint32_t outputCount,
        _Inout_ std::vector<DML_OPERATOR_GRAPH_NODE_DESC>& dmlOperatorGraphNodes,
        _Inout_ std::vector<DML_GRAPH_NODE_DESC>& dmlGraphNodes,
        _Inout_ std::vector<DML_GRAPH_EDGE_DESC>& dmlInputEdges,
        _Inout_ std::vector<DML_GRAPH_EDGE_DESC>& dmlOutputEdges,
        _Inout_ std::vector<DML_GRAPH_EDGE_DESC>& dmlIntermediateEdges)
    {
        const uint32_t graphInputCount = inputCount;

        for (size_t i = 0; i < graphDesc.nodes.size(); ++i)
        {
            dmlOperatorGraphNodes[i] = DML_OPERATOR_GRAPH_NODE_DESC{graphDesc.nodes[i].op.Get()};
            dmlGraphNodes[i] = DML_GRAPH_NODE_DESC{DML_GRAPH_NODE_TYPE_OPERATOR, &dmlOperatorGraphNodes[i]};
        }

        for (size_t i = 0; i < graphDesc.inputEdges.size(); ++i)
        {
            dmlInputEdges[i] = DML_GRAPH_EDGE_DESC{DML_GRAPH_EDGE_TYPE_INPUT, &graphDesc.inputEdges[i]};
        }

        for (size_t i = 0; i < graphDesc.outputEdges.size(); ++i)
        {
            dmlOutputEdges[i] = DML_GRAPH_EDGE_DESC{DML_GRAPH_EDGE_TYPE_OUTPUT, &graphDesc.outputEdges[i]};
        }

        for (size_t i = 0; i < graphDesc.intermediateEdges.size(); ++i)
        {
            dmlIntermediateEdges[i] =
                DML_GRAPH_EDGE_DESC{DML_GRAPH_EDGE_TYPE_INTERMEDIATE, &graphDesc.intermediateEdges[i]};
        }

        dmlGraphDesc.InputCount = graphInputCount;
        dmlGraphDesc.OutputCount = outputCount;
        dmlGraphDesc.NodeCount = gsl::narrow_cast<uint32_t>(dmlGraphNodes.size());
        dmlGraphDesc.Nodes = dmlGraphNodes.data();
        dmlGraphDesc.InputEdgeCount = gsl::narrow_cast<uint32_t>(dmlInputEdges.size());
        dmlGraphDesc.InputEdges = dmlInputEdges.data();
        dmlGraphDesc.OutputEdgeCount = gsl::narrow_cast<uint32_t>(dmlOutputEdges.size());
        dmlGraphDesc.OutputEdges = dmlOutputEdges.data();
        dmlGraphDesc.IntermediateEdgeCount = gsl::narrow_cast<uint32_t>(dmlIntermediateEdges.size());
        dmlGraphDesc.IntermediateEdges = dmlIntermediateEdges.data();
    }

    void CreateIDmlCompiledOperatorAndRegisterKernel(
        onnxruntime::Graph& graph, 
        const onnxruntime::IndexedSubGraph& indexedSubGraph,
        const onnxruntime::Node& fusedNode,
        const std::unordered_map<std::string, GraphNodeProperties>& partitionNodePropsMap,
        std::shared_ptr<std::unordered_map<std::string, onnx::TensorProto>> transferredInitializerMap,
        const ExecutionProviderImpl* providerImpl,
        onnxruntime::KernelRegistry* registryForPartitionKernels)
    {
        // These nodeArgNames will be used while creating DML Graph inside FusedGraphKernel.cpp
        // Ordering of input/output nodeArgs in below vector will be same as Node::Definitions::input_defs because
        // ORT is populating these args as it is while creating the FusedNode at Graph::CreateFusedSubGraphNode()
        // Why we need these names?
        //      After Partitioning and before reaching to FusedGraphKernel, ORT may modify the input/output nodeArg names
        //      present in FusedNode (Node::Definitions::input_defs) as part of some transformers like memcopy, or L1/L2/L3 transformers.
        std::vector<std::string> fusedNodeInputArgOriginalNames = indexedSubGraph.GetMetaDef()->inputs;
        std::vector<std::string> fusedNodeOutputArgOriginalNames = indexedSubGraph.GetMetaDef()->outputs;

        // convert partitionONNXGraph into DML EP GraphDesc
        const uint32_t graphInputCount = gsl::narrow_cast<uint32_t>(fusedNode.InputDefs().size());
        std::vector<uint8_t> inputsConstant(graphInputCount);
        for (uint32_t index = 0; index < graphInputCount; ++index)
        {
            inputsConstant[index] = GraphKernelHelper::GetGraphInputConstness(index, fusedNodeInputArgOriginalNames, *transferredInitializerMap);
        }

        ComPtr<IDMLDevice> device;
        ORT_THROW_IF_FAILED(providerImpl->GetDmlDevice(device.GetAddressOf()));
        GraphDescBuilder::GraphDesc graphDesc = GraphDescBuilder::BuildGraphDesc(
            inputsConstant.data(),
            inputsConstant.size(),
            *transferredInitializerMap,
            graph,
            indexedSubGraph,
            fusedNodeInputArgOriginalNames,
            fusedNodeOutputArgOriginalNames,
            partitionNodePropsMap,
            device.Get(),
            providerImpl);

        // convert DML EP GraphDesc into DML_GRAPH_DESC and create IDMLCompiledOperator
        DML_GRAPH_DESC dmlGraphDesc = {};
        std::vector<DML_OPERATOR_GRAPH_NODE_DESC> dmlOperatorGraphNodes(graphDesc.nodes.size());
        std::vector<DML_GRAPH_NODE_DESC> dmlGraphNodes(graphDesc.nodes.size());
        std::vector<DML_GRAPH_EDGE_DESC> dmlInputEdges(graphDesc.inputEdges.size());
        std::vector<DML_GRAPH_EDGE_DESC> dmlOutputEdges(graphDesc.outputEdges.size());
        std::vector<DML_GRAPH_EDGE_DESC> dmlIntermediateEdges(graphDesc.intermediateEdges.size());
        ConvertGraphDesc(
            graphDesc, 
            dmlGraphDesc, 
            graphInputCount,
            gsl::narrow_cast<uint32_t>(fusedNode.OutputDefs().size()),
            dmlOperatorGraphNodes,
            dmlGraphNodes,
            dmlInputEdges,
            dmlOutputEdges,
            dmlIntermediateEdges);

        DML_EXECUTION_FLAGS executionFlags = DML_EXECUTION_FLAG_NONE;
        if (graphDesc.reuseCommandList)
        {
            executionFlags |= DML_EXECUTION_FLAG_DESCRIPTORS_VOLATILE;
        }

        // Query DML execution provider to see if metacommands is enabled
        if (!providerImpl->MetacommandsEnabled())
        {
            executionFlags |= DML_EXECUTION_FLAG_DISABLE_META_COMMANDS;
        }

        ComPtr<IDMLDevice1> device1;
        ORT_THROW_IF_FAILED(device.As(&device1));
        ComPtr<IDMLCompiledOperator> compiledExecutionPlanOperator;
        ORT_THROW_IF_FAILED(device1->CompileGraph(
            &dmlGraphDesc,
            executionFlags,
            IID_PPV_ARGS(&compiledExecutionPlanOperator)));

        // lamda captures for the kernel registration
        Windows::AI::MachineLearning::Adapter::EdgeShapes outputShapes;
        ORT_THROW_HR_IF(E_UNEXPECTED, !TryGetStaticOutputShapes(fusedNode, outputShapes));
        std::vector<DML_INPUT_GRAPH_EDGE_DESC>& inputEdges = graphDesc.inputEdges;
        bool resuableCommandList = graphDesc.reuseCommandList;
        auto fused_kernel_func = [compiledExecutionPlanOperator, outputShapes, inputEdges, resuableCommandList, inputsConstant, transferredInitializerMap, fusedNodeInputArgOriginalNames]
                    (onnxruntime::FuncManager& func_mgr, const onnxruntime::OpKernelInfo& info, std::unique_ptr<onnxruntime::OpKernel>& out) mutable ->onnxruntime::Status
        {
            out.reset(CreateFusedGraphKernel(
                info, 
                compiledExecutionPlanOperator,
                outputShapes,
                inputEdges,
                resuableCommandList,
                inputsConstant,
                *transferredInitializerMap, 
                fusedNodeInputArgOriginalNames));
			return Status::OK();
        };

        // build the kernel definition on the fly, and register it to the fused_kernel_regisitry.
        onnxruntime::KernelDefBuilder builder;
        builder.SetName(indexedSubGraph.GetMetaDef()->name)
            .SetDomain(indexedSubGraph.GetMetaDef()->domain)
            .SinceVersion(indexedSubGraph.GetMetaDef()->since_version)
            .Provider(onnxruntime::kDmlExecutionProvider);
        ORT_THROW_IF_ERROR(registryForPartitionKernels->Register(builder, fused_kernel_func));
    }

    void FusePartitionAndRegisterKernel(
        GraphPartition* partition,
        uint32_t partitionIndex,
        onnxruntime::Graph& graph,
        std::unordered_map<const onnxruntime::Node*, GraphNodeProperties>& graphNodePropertyMap,
        onnxruntime::KernelRegistry* registryForPartitionKernels,
        const std::string& partitionKernelPrefix,
        std::shared_ptr<std::unordered_map<std::string, onnx::TensorProto>> transferredInitializerMap,
        const ExecutionProviderImpl* providerImpl)
    {
        assert(partition->IsDmlGraphPartition());

        onnxruntime::IndexedSubGraph subGraph;
        // Create a definition for the node.  The name must be unique.
        auto def = std::make_unique<onnxruntime::IndexedSubGraph::MetaDef>();
        def->name = DmlGraphFusionTransformer::DML_GRAPH_FUSION_NODE_NAME_PREFIX + partitionKernelPrefix + std::to_string(partitionIndex);
        def->domain = DmlGraphFusionTransformer::DML_GRAPH_FUSION_NODE_DOMAIN;
        def->since_version = 1;
        def->inputs.insert(def->inputs.begin(), partition->GetInputs().begin(), partition->GetInputs().end());
        def->outputs.insert(def->outputs.begin(), partition->GetOutputs().begin(), partition->GetOutputs().end());

        subGraph.SetMetaDef(std::move(def));
        subGraph.nodes = std::move(partition->GetNodeIndices());
        auto& fusedNode = graph.BeginFuseSubGraph(subGraph, subGraph.GetMetaDef()->name);
        fusedNode.SetExecutionProviderType(onnxruntime::kDmlExecutionProvider);
        
        // Populate properties which will be passed to OpKernel for this graph via the function below
        std::unordered_map<std::string, GraphNodeProperties> partitionNodePropsMap;
        for (auto nodeIndex : subGraph.nodes)
        {
            const onnxruntime::Node* node = graph.GetNode(nodeIndex);

#ifdef PRINT_PARTITON_INFO
            printf("Partition %u\t%s\n", partitionIndex, GraphDescBuilder::GetUniqueNodeName(*node).c_str());
#endif
            partitionNodePropsMap.insert(std::make_pair(
                GraphDescBuilder::GetUniqueNodeName(*node), std::move(graphNodePropertyMap[node])));
        }

#ifdef PRINT_PARTITON_INFO
        printf("\n");
#endif
        CreateIDmlCompiledOperatorAndRegisterKernel(
            graph, 
            subGraph,
            fusedNode,
            partitionNodePropsMap, 
            transferredInitializerMap, 
            providerImpl,
            registryForPartitionKernels);
        graph.FinalizeFuseSubGraph(subGraph, fusedNode);
    }

    onnxruntime::common::Status DmlGraphFusionTransformer::ApplyImpl(
        onnxruntime::Graph& graph,
        bool& modified,
        int graph_level,
        const onnxruntime::logging::Logger& logger) const
    {
        onnxruntime::ProviderType provider_type = onnxruntime::kDmlExecutionProvider;
        const gsl::not_null<const onnxruntime::KernelRegistry*> registry = m_providerImpl->GetKernelRegistry().get();
        const auto kernel_type_str_resolver = onnxruntime::OpSchemaKernelTypeStrResolver{};
        const auto kernel_lookup = onnxruntime::KernelLookup{provider_type,
                                                             gsl::make_span(&registry, 1),
                                                             kernel_type_str_resolver};

        // Initializers needed by any graph partition
        std::unordered_set<std::string> requiredInitializerMap;
        std::unordered_map<const onnxruntime::Node*, GraphNodeProperties> graphNodePropertyMap;
        onnxruntime::GraphViewer graphViewer(graph);
        std::vector<std::unique_ptr<GraphPartition>> partitions = BuildPartitions(
            graphViewer,
            *m_providerImpl->GetInternalRegistrationInfoMap(), 
            kernel_lookup,
            m_providerImpl->GetSupportedDeviceDataTypeMask(),
            graphNodePropertyMap, 
            requiredInitializerMap);

        // Create a map between each initialized tensor and the partition(s) it is part of.
        auto initializerPartitionMap = GetInitializerToPartitionMap(graphViewer, partitions);

        for (uint32_t partitionIndex = 0; partitionIndex < partitions.size(); ++partitionIndex)
        {
            auto& partition = partitions[partitionIndex];

            if (partition->GetRootMergedPartition() != partition.get() ||
                !partition->IsDmlPartition())
            {
                continue;
            }

            // Create a map which will store by name each initializer which should be transferred to the 
            // partition.  This prevents OnnxRuntime from allocating GPU resources and uploading those initializers,
            // so the partiton's kernel can do so.  In the process, it will pre-process weights while consuming a CPU
            // backed resource, avoiding an extra set of GPU resources in memory.
            // A shared pointer is used so the functor and contained initializer captures can be cheaply copied within ORT.
            auto transferredInitializerMap = std::make_shared<std::unordered_map<std::string, onnx::TensorProto>>();
            
            if (partition->IsDmlGraphPartition())
            {
                // populate transferredInitializerMap
                for (const auto& input : partition->GetInputs())
                {
                    const onnx::TensorProto* tensor = nullptr;
                    if (graph.GetInitializedTensor(input, tensor))
                    {
                        // It's only safe to transfer tensors which are used by this partition alone.
                        auto iter = initializerPartitionMap.find(tensor);
                        assert(iter != initializerPartitionMap.end());
                        if (iter->second.size() > 1)
                        {
                            if (requiredInitializerMap.find(input) != requiredInitializerMap.end())
                            {
                                // The kernel relies on this input to be initialized, and it should be small enough to copy
                                // cheaply. FusedGraphKernel only handles constant CPU inputs through transferred initializers,
                                // rather than ORT, to avoid mismatches in policy or implementation causing failures.
                                (*transferredInitializerMap)[input] = *tensor;
                            }

                            continue;
                        }
                        ORT_RETURN_IF_ERROR(graph.ExtractInitializedTensor(tensor->name(), (*transferredInitializerMap)[input]));
                    }
                }

                std::string partitionKernelPrefix = std::to_string(m_providerImpl->GetPartitionKernelPrefixVal()) + "_";
                m_providerImpl->IncreasePartitionKernelPrefixVal();

                FusePartitionAndRegisterKernel(
                    partition.get(), 
                    partitionIndex, 
                    graph, 
                    graphNodePropertyMap,
                    m_providerImpl->GetKernelRegistry().get(),
                    partitionKernelPrefix,
                    transferredInitializerMap,
                    m_providerImpl
                );
            }
        }

        return onnxruntime::common::Status::OK();
    }
}
