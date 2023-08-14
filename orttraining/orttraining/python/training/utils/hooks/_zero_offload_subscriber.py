# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------

import inspect
from collections import OrderedDict
from types import CodeType, FunctionType
from typing import Callable, Optional, Tuple, Union

import onnx
import torch

from onnxruntime.training.utils import ORTModelInputOutputType, extract_data_and_schema, unflatten_data_using_schema, pytorch_dtype_to_onnx

from ._subscriber_base import RuntimeStates, SubscriberBase


# Used to monkey patch the original function
# Adapted from https://github.com/microsoft/DeepSpeed/blob/e8318634b4313eaad89842cf4322e1762d34ced3/deepspeed/runtime/zero/parameter_offload.py#L333
def _setup_zero_stage3_ort_compatible_hooks(self):
    self.hierarchy = 0

    from onnxruntime.training.utils.hooks import SubscriberManager, ZeROOffloadSubscriber
    from onnxruntime.training.utils.hooks._zero_offload_subscriber import _zero_offload_one_time_initializer

    # Each DeepSpeed engine has a separate subscriber manager.
    self._offload_subscriber_manager = SubscriberManager()
    self._offload_subscriber_manager.subscribe(
        self.module, [ZeROOffloadSubscriber(self, _zero_offload_one_time_initializer)]
    )
    self.forward_hooks.extend(self._offload_subscriber_manager._pre_forward_hooks)
    self.forward_hooks.extend(self._offload_subscriber_manager._post_forward_hooks)

    # Add top module to stack trace
    global FWD_MODULE_STACK  # noqa: PLW0602
    FWD_MODULE_STACK.append(self.module)


class _ZeROOffloadOneTimeInitializer:
    """Store the hook functions from DeepSpeed ZeRO offload.

    Hook functions code collected from DeepSpeed.
    """

    def __init__(self):
        self._code_store: OrderedDict[str, CodeType] = {}

    def collect_code(self, function: Callable):
        """Collect the function `CodeType`, which is the code object of the function."""
        code_obj = function.__code__
        for c in code_obj.co_consts:
            if inspect.iscode(c):
                self._code_store[c.co_name] = c


_zero_offload_one_time_initializer = None

try:
    # Have to import below explicitly, otherwise it complains about _apply_to_tensors_only not found.
    # The hooks reference functions or classes in that file.
    from deepspeed.runtime.zero.parameter_offload import *  # noqa: F403
    from deepspeed.runtime.zero.parameter_offload import DeepSpeedZeRoOffload, _apply_to_tensors_only  # noqa: F401
    from deepspeed.utils import instrument_w_nvtx  # noqa: F401

    # Used to collect the hook functions's code object from DeepSpeed ZeRO offload, this should be initialized only once.
    if _zero_offload_one_time_initializer is None:
        _zero_offload_one_time_initializer = _ZeROOffloadOneTimeInitializer()
        _zero_offload_one_time_initializer.collect_code(DeepSpeedZeRoOffload._register_hooks_recursively)
        _zero_offload_one_time_initializer.collect_code(DeepSpeedZeRoOffload.setup_zero_stage3_hooks)

    # This is the function to enable ORT ZeRO offload.
    def configure_ort_compatible_zero_stage3():
        """Configure ZeRO stage3 to be ORT compatible.

        This function will overwrite the original DeepSpeed ZeRO stage3 hooks to make it ORT compatible.
        """

        # Only done once no matter how many times this function is called for different modules.
        DeepSpeedZeRoOffload.setup_zero_stage3_hooks = _setup_zero_stage3_ort_compatible_hooks

except ImportError:

    def configure_ort_compatible_zero_stage3():
        raise RuntimeError("DeepSpeed is not installed, cannot configure ORT compatible ZeRO stage3.")


class ORTZeROOffloadPreForwardFunction(torch.autograd.Function):
    """This function is a common bridge to call original PyTorch's
    pre_forward_function and post_backward_function.
    """

    @staticmethod
    def forward(
        ctx,
        module,
        pre_forward_with_kwargs_function,
        post_backward_function,
        args_schema,
        kwargs_schema,
        args_tensor_count,
        kwargs_tensor_count,
        *tensor_list,
    ):
        """
        Args:
            ctx: context object
            module: the module to be called
            pre_forward_with_kwargs_function: the function to be called before forward (PyTorch's pre_forward_function)
            post_backward_function: the function to be called after backward (PyTorch's post_backward_function)
            args_schema: the schema of the args, used to reconstruct the args in original form in
                PyTorch's pre_forward_function's inputs.
            kwargs_schema: the schema of the kwargs, used to reconstruct the kwargs in original form in
                PyTorch's pre_forward_function's inputs.
            args_tensor_count: the number of tensors in args.
            kwargs_tensor_count: the number of tensors in kwargs.
            tensor_list: the list of tensors, the first args_tensor_count tensors are args, the next
                kwargs_tensor_count tensors are kwargs, the rest are the parameters for offload.
        """
        args_tensors = tensor_list[:args_tensor_count]
        kwargs_tensors = tensor_list[args_tensor_count : args_tensor_count + kwargs_tensor_count]

        args = unflatten_data_using_schema(args_tensors, args_schema)
        kwargs = unflatten_data_using_schema(kwargs_tensors, kwargs_schema)

        # We will re-retrieve the partitioned parameter tensors other than use the one passed in input (of size 0).
        # This is required for ORT run because in ORT graph, the tensor of size 0 will always be size 0
        # (this step is not necessary for PyTorch run, because PyTorch will re-use the same tensor
        # while .data got updated to full-sized data after pre_forward_with_kwargs_function is called).
        from deepspeed.runtime.zero.partition_parameters import ZeroParamStatus
        from deepspeed.runtime.zero.partitioned_param_coordinator import iter_params

        # Retrieve the parameters that are not available for this module.
        params_to_fetch = frozenset(iter_params(module))
        partitioned_params = []
        for param in params_to_fetch:
            if param.ds_status == ZeroParamStatus.NOT_AVAILABLE:
                partitioned_params.append(param)

        ctx.partitioned_params = partitioned_params

        f_ret = pre_forward_with_kwargs_function(module, args, kwargs)

        if f_ret is None:
            updated_args, updated_kwargs = args, kwargs
        else:
            assert isinstance(f_ret, tuple)
            updated_args, updated_kwargs = f_ret

        ctx.module = module
        ctx.post_backward_function = post_backward_function

        updated_args_tensors, _ = extract_data_and_schema(updated_args)
        updated_kwargs_tensors, _ = extract_data_and_schema(updated_kwargs)

        rets = tuple(updated_args_tensors + updated_kwargs_tensors)

        rets += tuple([p.detach().requires_grad_(p.requires_grad) for p in partitioned_params])

        # PyTorch exporter does not support an empty list of tensors, so we have this check.
        assert len(rets) != 0
        return rets

    @staticmethod
    def backward(ctx, *grads):
        updated_grads = grads
        if ctx.post_backward_function is not None:
            ret = ctx.post_backward_function(ctx.module, grads)
            if ret is not None:
                updated_grads = ret

        # Update grad for partitioned parameters.
        # TODO(pengwa)

        input_count = len(updated_grads) - len(ctx.partitioned_params)

        zeros = [torch.zeros_like(p) for p in ctx.partitioned_params]

        pass_through_grads = updated_grads[:input_count] + tuple(zeros)

        return (None, None, None, None, None, None, None, *pass_through_grads)

    @staticmethod
    def infer_shape(
        node: onnx.NodeProto,
        tensor_input_shapes: List[Optional[List[Union[int, str]]]],
        tensor_input_dtypes: List[torch.onnx.TensorProtoDataType],
    ) -> Tuple[List[Optional[List[Union[int, str]]]], List[torch.onnx.TensorProtoDataType]]:
        import ctypes
        # input_pointer_scalar_positions_attr_name = "input_pointer_scalar_positions"
        input_pointer_scalars_attr_name = "input_pointer_scalars"
        # found = [attr for attr in node.attribute if attr.name == input_pointer_scalar_positions_attr_name]
        # assert len(found) == 1
        # input_pointer_scalar_positions = found[0].ints
        found = [attr for attr in node.attribute if attr.name == input_pointer_scalars_attr_name]
        assert len(found) == 1
        input_pointer_scalars = found[0].ints
        print("input_pointer_scalars[0]: ", input_pointer_scalars[0])
        module = ctypes.cast(input_pointer_scalars[0], ctypes.py_object).value

        print(f"infer_shape for module: {module.__class__.__name__}")


        from deepspeed.runtime.zero.partition_parameters import ZeroParamStatus
        from deepspeed.runtime.zero.partitioned_param_coordinator import iter_params

        # Retrieve the parameters that are not available for this module.
        params_to_fetch = frozenset(iter_params(module))
        partitioned_params = []
        for param in params_to_fetch:
            if param.ds_status == ZeroParamStatus.NOT_AVAILABLE:
                partitioned_params.append(param)

        tensor_output_shapes = tensor_input_shapes
        tensor_output_dtypes = tensor_input_dtypes

        start_offset = len(tensor_input_shapes) - len(partitioned_params)
        for i in range(len(partitioned_params)):
            tensor_output_shapes[start_offset + i] = list(partitioned_params[i].ds_shape)
            tensor_output_dtypes[start_offset + i] = pytorch_dtype_to_onnx(partitioned_params[i].dtype)

        assert len(tensor_output_shapes) == len(tensor_input_shapes)
        assert len(tensor_output_dtypes) == len(tensor_input_dtypes)

        return tensor_output_shapes, tensor_output_dtypes

class ORTZeROOffloadPostForwardFunction(torch.autograd.Function):
    @staticmethod
    def forward(
        ctx,
        module,
        post_forward_function,
        pre_backward_function,
        output_schema,
        *output_tensors,
    ):
        """
        Args:
            ctx: context object
            module: the module to be called
            post_forward_function: the function to be called after forward (PyTorch's post_forward_function)
            pre_backward_function: the function to be called before backward (PyTorch's pre_backward_function)
            output_schema: the schema of the output, used to reconstruct the output in original form in
                PyTorch's post_forward_function's inputs.
            output_tensors: the list of tensors.

        """
        outputs = unflatten_data_using_schema(output_tensors, output_schema)

        # STAGE3WARN: _post_forward_module_hook's second argument `input is not used, so we just pass a None here.
        updated_outputs = post_forward_function(module, None, outputs)

        if updated_outputs is None:
            updated_output_tensors = output_tensors
        else:
            updated_output_tensors, _ = extract_data_and_schema(updated_outputs)

        ctx.module = module
        ctx.pre_backward_function = pre_backward_function
        rets = [o.detach().requires_grad_(o.requires_grad) for o in updated_output_tensors]
        return tuple(rets)

    @staticmethod
    def backward(ctx, *grads):
        updated_args = grads
        if ctx.pre_backward_function is not None:
            ret = ctx.pre_backward_function(ctx.module, grads)
            if ret is not None:
                updated_args = ret
        return (None, None, None, None, *updated_args)

    @staticmethod
    def infer_shape(
        node: onnx.NodeProto,
        tensor_input_shapes: List[Optional[List[Union[int, str]]]],
        tensor_input_dtypes: List[torch.onnx.TensorProtoDataType],
    ) -> Tuple[List[Optional[List[Union[int, str]]]], List[torch.onnx.TensorProtoDataType]]:
        return tensor_input_shapes, tensor_input_dtypes

class _ZeROOffloadFunctions:
    def __init__(self, one_time_init: _ZeROOffloadOneTimeInitializer, offloader) -> None:
        self._function_store: OrderedDict[str, FunctionType] = {}
        self._one_time_init = one_time_init
        for name, code in self._one_time_init._code_store.items():
            cell = self._create_closure_for_ds_hook_function(offloader)
            self._function_store[name] = FunctionType(code, globals(), code.co_name, None, (cell,))

    def get(self, name: str) -> FunctionType:
        return self._function_store[name]

    def _create_closure_for_ds_hook_function(self, offloader):
        # https://stackoverflow.com/questions/17395338/how-to-access-a-function-inside-a-function
        def make_closure_cell(_self):
            def nested():
                return _self

            return nested.__closure__[0]

        cell = make_closure_cell(offloader)
        return cell


class ZeROOffloadSubscriber(SubscriberBase):
    """This subscriber is used to enable ZeRO Offload feature in a way compatible with ORTModule."""

    def __init__(self, offloader, one_time_init: _ZeROOffloadOneTimeInitializer, enable_debug_info: bool = False):
        super().__init__(None, None)
        self._offloader = offloader
        self._functions = _ZeROOffloadFunctions(one_time_init, self._offloader)
        self._enable_debug_info = enable_debug_info

    def pre_forward_module_apply_impl(
        self,
        run_rtx: RuntimeStates,
        module: torch.nn.Module,
        args: ORTModelInputOutputType,
        kwargs: ORTModelInputOutputType,
    ) -> Tuple[ORTModelInputOutputType, ORTModelInputOutputType]:
        args_tensors, args_schema = extract_data_and_schema(args)
        kwargs_tensors, kwargs_schema = extract_data_and_schema(kwargs)

        from deepspeed.runtime.zero.partition_parameters import ZeroParamStatus
        from deepspeed.runtime.zero.partitioned_param_coordinator import iter_params

        # Retrieve the parameters that are not available for this module.
        params_to_fetch = frozenset(iter_params(module))
        partitioned_params = []
        for param in params_to_fetch:
            if param.ds_status == ZeroParamStatus.NOT_AVAILABLE:
                partitioned_params.append(param)

        _pre_forward_module_hook = self._functions.get("_pre_forward_module_hook")

        args_tensor_count = len(args_tensors)
        kwargs_tensor_count = len(kwargs_tensors)

        def _wrap_pre_forward_module_hook(module, args, kwargs):
            rets = _pre_forward_module_hook(module, args)
            updated_args, updated_kwargs = args, kwargs
            if rets is not None:
                updated_args = rets

            # STAGE3WARN: Moved from _post_backward_module_hook to make sure ORT run will trigger every iteration.
            module.ds_grads_remaining = 0
            return updated_args, updated_kwargs

        all_tensors = args_tensors + kwargs_tensors + partitioned_params

        self._check_all_tensor(all_tensors, module, "pre_forward_module_apply_impl input check")

        rets = ORTZeROOffloadPreForwardFunction.apply(
            module,
            _wrap_pre_forward_module_hook,
            None,
            args_schema,
            kwargs_schema,
            args_tensor_count,
            kwargs_tensor_count,
            *all_tensors,
        )

        self._check_all_tensor(rets, module, "pre_forward_module_apply_impl output check")

        updated_args_tensors = rets[:args_tensor_count]
        updated_kwargs_tensors = rets[args_tensor_count : args_tensor_count + kwargs_tensor_count]

        updated_args = unflatten_data_using_schema(updated_args_tensors, args_schema)
        updated_kwargs = unflatten_data_using_schema(updated_kwargs_tensors, kwargs_schema)

        _post_backward_module_hook = self._functions.get("_post_backward_module_hook")
        # STAGE3WARN: Other part of _post_backward_module_hook can be traced correctly so we don't need to
        # wrap with PythonOp.
        updated_args = _post_backward_module_hook(module, updated_args)

        return updated_args, updated_kwargs

    def post_forward_module_apply_impl(
        self,
        run_rtx: RuntimeStates,
        module: torch.nn.Module,
        args: ORTModelInputOutputType,
        outputs: ORTModelInputOutputType,
    ) -> Tuple[ORTModelInputOutputType, ORTModelInputOutputType]:
        outputs_tensors, outputs_schema = extract_data_and_schema(outputs)

        _post_forward_module_hook = self._functions.get("_post_forward_module_hook")

        self._check_all_tensor(outputs_tensors, module, "post_forward_module_apply_impl input check")

        updated_outputs_tensors = ORTZeROOffloadPostForwardFunction.apply(
            module, _post_forward_module_hook, None, outputs_schema, *outputs_tensors
        )

        self._check_all_tensor(updated_outputs_tensors, module, "post_forward_module_apply_impl output check")

        assert len(updated_outputs_tensors) == len(outputs_tensors)

        # WARN: we assume updated_output_tensors can REUSE the outputs_schema.
        updated_outputs = unflatten_data_using_schema(updated_outputs_tensors, outputs_schema)

        _pre_backward_module_hook = self._functions.get("_pre_backward_module_hook")
        # STAGE3WARN: _pre_backward_module_hook's second argument `input is not used, so we just pass a None here.
        updated_outputs = _pre_backward_module_hook(module, None, updated_outputs)

        return args, updated_outputs

    def post_forward_outmost_module_apply_impl(
        self,
        run_rtx: RuntimeStates,
        module: torch.nn.Module,
        args: ORTModelInputOutputType,
        outputs: ORTModelInputOutputType,
    ) -> Tuple[ORTModelInputOutputType, ORTModelInputOutputType]:
        outputs_tensors, outputs_schema = extract_data_and_schema(outputs)

        def _pre_backward_function(module, grads):
            return

        _end_of_forward_hook = self._functions.get("_end_of_forward_hook")

        self._check_all_tensor(outputs_tensors, module, "post_forward_outmost_module_apply_impl input check")

        updated_outputs_tensors = ORTZeROOffloadPostForwardFunction.apply(
            module, _end_of_forward_hook, _pre_backward_function, outputs_schema, *outputs_tensors
        )

        self._check_all_tensor(updated_outputs_tensors, module, "post_forward_outmost_module_apply_impl output check")

        assert len(updated_outputs_tensors) == len(outputs_tensors)
        updated_outputs = unflatten_data_using_schema(updated_outputs_tensors, outputs_schema)
        return args, updated_outputs

    def _check_all_tensor(self, tensor_list: Tuple[torch.Tensor], module: torch.nn.Module, name: str):
        if not self._enable_debug_info:
            return

        for t in tensor_list:
            if not isinstance(t, torch.Tensor):
                raise RuntimeError(f"{name} fail: {module.__class__.__name__}, input type: {type(t)}")
