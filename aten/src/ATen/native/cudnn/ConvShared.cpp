#include <ATen/core/Tensor.h>
#include <ATen/cuda/CUDAConfig.h>  // for the definition of AT_CUDNN_ENABLED
#include <ATen/native/ConvUtils.h>

#if AT_CUDNN_ENABLED()

#include <ATen/native/cudnn/ConvShared.h>

// NOTE [cuDNN API version]
//
// ConvPlaceholders.cpp contains placeholder implementation of cudnn
// convolution when cudnn is not enabled. These operators only raises
// errors, and do no real computation. These operators are implemented
// using currnet operators.
//
// cuDNN v7 and v8 have different API. ConvShared.{cpp, h} contains
// code shared by v7 and v8. Conv_v7.cpp contains implementation of
// convolution using cuDNN v7 API. Conv_v8.cpp contains implementation
// with v8 API.
//
// NOTE [ Convolution design ]
//
// cuDNN convolutions does not handle bias. Bias is handled outside.
//
// The general strategy:
//
//    - cudnn_convolution (Tensor)
//      Entry points for clients
//
//    - cudnn_convolution_forward (TensorArg)
//      Entry point, which may be reused between regular
//      convolution and transposed convolution.
//
//    - raw_cudnn_convolution_forward_out (Tensor)
//      Function that has different implementation on Conv_v7.cpp
//      and Conv_v8.cpp
//
// The raw API directly invokes CuDNN and are implemeted differently
// on cuDNN v7 and cuDNN v8
//
// There are a few reasons this should never be directly exposed
// via ATen:
//
//    - It takes output as a parameter (this should be computed!)
//    - It doesn't do input checking
//    - It doesn't resize output (it is assumed to be correctly sized)
//
// Where does argument checking happen?  Here's the division of
// responsibility:
//  - Things that happen in at::Tensor
//    - TensorArg allocation
//  - Things that happen in TensorArg
//    - Check arguments (type, GPU, shape)

namespace at { namespace native {

// ---------------------------------------------------------------------
//
// ConvolutionParams
//
// ---------------------------------------------------------------------

std::ostream& operator<<(std::ostream & out, const ConvolutionParams& params) {
  out << "ConvolutionParams \n"
    << "    data_type = " << cudnnTypeToString(params.dataType) << "\n"
    << "    padding = " << ArrayRef<int>{params.padding} << "\n"
    << "    stride = " << ArrayRef<int>{params.stride} << "\n"
    << "    dilation = " << ArrayRef<int>{params.dilation} << "\n"
    << "    groups = " << params.groups << "\n"
    << "    deterministic = " << (params.deterministic ? "true" : "false") << "\n"
    << "    allow_tf32 = " << (params.allow_tf32 ? "true" : "false") << "\n";

  return out;
}

// NB: This can't be a constructor, because then ConvolutionParams
// would not be a POD anymore.
// TODO: Use TensorGeometry here instead of the entire Tensor, which we
// don't actually need.  (OTOH: We can always pass in
// grad_input/grad_output, so this is not very pressing)
void setConvolutionParams(
    ConvolutionParams* params,
    const at::Tensor& input, const at::Tensor& weight,
    IntArrayRef padding, IntArrayRef stride, IntArrayRef dilation,
    int64_t groups, bool deterministic, bool allow_tf32) {

  cudnnDataType_t dataType = getCudnnDataType(input);
  memset(params, 0, sizeof(ConvolutionParams));
  params->device_id = at::cuda::current_device();
  params->dataType = dataType;
  // ASSERT(weight.dim() == input.dim())
  params->input_dim = input.dim();
  params->memory_format = input.suggest_memory_format();
  for (int i = 0; i != params->input_dim; ++i) {
    params->input_size[i] = (int) input.sizes()[i];
    params->weight_size[i] = (int) weight.sizes()[i];
  }
  // ASSERT(padding.size() == stride.size())
  // ASSERT(padding.size() == dilation.size())
  for (size_t i = 0; i != padding.size(); ++i) {
    params->padding[i] = padding[i];
    params->stride[i] = stride[i];
    params->dilation[i] = dilation[i];
  }
  // In principle, we shouldn't parametrize by groups for legacy
  // CuDNN, but it doesn't seem worth the effort to actually do this.
  params->groups = groups;
  params->deterministic = deterministic;
  params->allow_tf32 = allow_tf32;
}

std::string repro_from_args(const ConvolutionParams& params) {
  auto pybool = [](bool b) -> const char* { return b ? "True" : "False"; };
  std::string partial_dtype;
  switch (params.dataType) {
    case CUDNN_DATA_FLOAT: partial_dtype = "float"; break;
    case CUDNN_DATA_DOUBLE: partial_dtype = "double"; break;
    case CUDNN_DATA_HALF: partial_dtype = "half"; break;
    default: partial_dtype = "unsupported";
  }
  const std::string full_dtype = "torch." + partial_dtype;
  const int out_channels = params.weight_size[0];
  const int in_channels = params.weight_size[1] * params.groups;
  const size_t dim = params.input_dim;
  const std::string channels_last_xd = dim == 4 ? "channels_last" : "channels_last_3d";
  const std::string to_channels_last =
    ((params.memory_format == at::MemoryFormat::ChannelsLast) || (params.memory_format == at::MemoryFormat::ChannelsLast3d)) \
    ? ".to(memory_format=torch." + channels_last_xd + ")" : "";

  std::ostringstream ss;
  ss << "You can try to repro this exception using the following code snippet. ";
  ss << "If that doesn't trigger the error, please include your original repro script when reporting this issue.\n\n";
  ss << "import torch\n";
  ss << "torch.backends.cuda.matmul.allow_tf32 = " << pybool(at::globalContext().allowTF32CuBLAS()) << "\n";
  ss << "torch.backends.cudnn.benchmark = " << pybool(at::globalContext().benchmarkCuDNN()) << "\n";
  ss << "torch.backends.cudnn.deterministic = " << pybool(params.deterministic) << "\n";
  ss << "torch.backends.cudnn.allow_tf32 = " << pybool(params.allow_tf32) << "\n";
  ss << "data = torch.randn(" << ArrayRef<int>(params.input_size, dim) << ", dtype=" << full_dtype << ", ";
  ss <<   "device='cuda', requires_grad=True)" << to_channels_last << "\n";
  ss << "net = torch.nn.Conv" << dim-2 << "d(" << in_channels << ", " << out_channels << ", ";
  ss <<   "kernel_size=" << ArrayRef<int>(&params.weight_size[2], dim - 2) << ", ";
  ss <<   "padding=" << ArrayRef<int>(params.padding, dim-2) << ", ";
  ss <<   "stride=" << ArrayRef<int>(params.stride, dim-2) << ", ";
  ss <<   "dilation=" << ArrayRef<int>(params.dilation, dim-2) << ", ";
  ss <<   "groups=" << params.groups << ")\n";
  ss << "net = net.cuda()." << partial_dtype << "()" << to_channels_last << "\n";
  ss << "out = net(data)\n";
  ss << "out.backward(torch.randn_like(out))\n";
  ss << "torch.cuda.synchronize()\n\n";

  return ss.str();
}

// ---------------------------------------------------------------------
//
// Checking
//
// ---------------------------------------------------------------------

// Used on pad, stride and dilation
static void check_args(CheckedFrom c, IntArrayRef args, size_t expected_size, const char* arg_name)
{
  TORCH_CHECK(args.size() <= expected_size,
           "Too many ", arg_name, " values (", args.size(), ") supplied, expecting ",
           expected_size, " (while checking arguments for ", c, ")");
  TORCH_CHECK(args.size() >= expected_size,
           "Not enough ", arg_name, " values (", args.size(), ") supplied, expecting ",
           expected_size, " (while checking arguments for ", c, ")");

  auto num_negative_values = std::count_if(args.begin(), args.end(), [](int x){return x < 0;});
  if (num_negative_values > 0){
    std::stringstream ss;
    ss << arg_name << " should be greater than zero but got (";
    std::copy(args.begin(), args.end() - 1, std::ostream_iterator<int>(ss,", "));
    ss << args.back() <<  ")" << " (while checking arguments for " << c << ")";
    AT_ERROR(ss.str());
  }
}


// NOTE [ Convolution checks ]
//
// NB: For many call sites, it is not strictly necessary to check all of
// these relationships (for example, for forward convolution, we compute
// the size of output ourselves, so we don't actually need to check
// output.  However, writing a single function that does everything
// means we get to reuse it for both forwards and all backwards
// variants, even when the set of "real" inputs varies.  The magic of
// relational computing!
//
// (There is one downside, which is that it is slightly harder to write
// error messages which are able to distinguish between real inputs
// (which the user can change) and computed inputs (which the user can
// only indirectly affect).  It would be an interesting exercise to
// come up with a general framework to handle such situations.)
static void convolution_shape_check(
    CheckedFrom c,
    const TensorGeometryArg& input, const TensorGeometryArg& weight, const TensorGeometryArg& output,
    IntArrayRef padding, IntArrayRef stride, IntArrayRef dilation, int64_t groups)
{
  check_args(c, padding, input->dim() - 2, "padding");
  check_args(c, stride, padding.size(), "stride");
  check_args(c, dilation, padding.size(), "dilation");

  // Input
  checkDimRange(c, input, 3, 6 /* exclusive */);
  checkSize(c, input, input_channels_dim, weight->size(1) * groups);

  // Weight
  checkSameDim(c, input, weight);

  // TODO: check that output->size() matches output_sizes
  // TODO: check that weight matches output->sizes()
  checkSameDim(c, input, output);
}

// ---------------------------------------------------------------------
//
// Convolution forward / Transposed convolution backward
//
// ---------------------------------------------------------------------

Tensor cudnn_convolution_forward(
    CheckedFrom c,
    const TensorArg& input, const TensorArg& weight,
    IntArrayRef padding, IntArrayRef stride, IntArrayRef dilation, int64_t groups,
    bool benchmark, bool deterministic, bool allow_tf32)
{
  checkAllSameType(c, {input, weight});
  checkAllSameGPU(c, {input, weight});

  auto memory_format = cudnn_conv_suggest_memory_format(*input, *weight);
  auto output_t = at::native::empty_cuda(
                    conv_output_size(input->sizes(), weight->sizes(),
                                     padding, stride, dilation),
                    /*dtype=*/input->scalar_type(),
                    /*layout=*/c10::nullopt,
                    /*device=*/kCUDA,
                    /*pin_memory=*/c10::nullopt,
                    /*memory_format=*/memory_format);

  if (output_t.numel() == 0) {
    return output_t;
  }

  // Avoid ambiguity of "output" when this is being used as backwards
  TensorArg output{ output_t, "result", 0 };
  convolution_shape_check(c, input, weight, output, padding, stride, dilation, groups);

  Tensor weight_contig = weight->contiguous(memory_format);
  Tensor input_contig = input->contiguous(memory_format);

  raw_cudnn_convolution_forward_out(
      *output, input_contig, weight_contig,
      padding, stride, dilation, groups, benchmark, deterministic, allow_tf32);

  return *output;
}

Tensor cudnn_convolution(
    const Tensor& input_t, const Tensor& weight_t,
    IntArrayRef padding, IntArrayRef stride, IntArrayRef dilation,
    int64_t groups, bool benchmark, bool deterministic, bool allow_tf32)
{
  TensorArg input  { input_t,  "input",  1 },
            weight { weight_t, "weight", 2 };
  CheckedFrom c = "cudnn_convolution";
  auto output_t = cudnn_convolution_forward(
    c, input, weight, padding, stride, dilation, groups, benchmark, deterministic, allow_tf32);
  return output_t;
}

// NB: output_padding not needed here, as there is no ambiguity to
// resolve
Tensor cudnn_convolution_transpose_backward_input(
    const Tensor& grad_output_t, const Tensor& weight_t,
    IntArrayRef padding, IntArrayRef stride, IntArrayRef dilation,
    int64_t groups, bool benchmark, bool deterministic, bool allow_tf32)
{
  TensorArg grad_output { grad_output_t,  "grad_output", 1 },
            weight      { weight_t, "weight", 2 };
  return cudnn_convolution_forward(
    "cudnn_convolution_transpose_backward_input",
    grad_output, weight, padding, stride, dilation, groups, benchmark, deterministic, allow_tf32);
}

// ---------------------------------------------------------------------
//
// Convolution backward / Transposed convolution forward
//
// ---------------------------------------------------------------------

// NOTE [ Backward vs transpose convolutions ]
//
// Backward and transpose are algorithmically equivalent, but they
// compute their geometry differently.  In a backwards, you knew what
// the original size of the input tensor was, so you can cache that
// geometry and fill it directly.  In transposed convolution, it is
// more conventional to not explicitly specify the output (previously
// input) size, and compute it.  This, however, leaves a degree of
// freedom; this degree of freedom is resolved using the
// output_padding parameter.  Both of these interfaces are equivalent,
// but they are differently convenient depending on the use case.

Tensor cudnn_convolution_backward_input(
    CheckedFrom c,
    IntArrayRef input_size, const TensorArg& grad_output, const TensorArg& weight,
    IntArrayRef padding, IntArrayRef stride, IntArrayRef dilation, int64_t groups,
    bool benchmark, bool deterministic, bool allow_tf32)
{
  checkAllSameType(c, {grad_output, weight});
  checkAllSameGPU(c, {grad_output, weight});

  auto memory_format = cudnn_conv_suggest_memory_format(*grad_output, *weight);
  auto grad_input_t = at::native::empty_cuda(
                    input_size,
                    /*dtype=*/grad_output->scalar_type(),
                    /*layout=*/c10::nullopt,
                    /*device=*/kCUDA,
                    /*pin_memory=*/c10::nullopt,
                    /*memory_format=*/memory_format);

  // Avoid "grad_input" when this is being used as transposed convolution
  TensorArg grad_input{ grad_input_t, "result", 0 };
  convolution_shape_check(c, grad_input, weight, grad_output, padding, stride, dilation, groups);

  Tensor weight_contig = weight->contiguous(memory_format);
  Tensor grad_output_contig = grad_output->contiguous(memory_format);

  raw_cudnn_convolution_backward_input_out(
      *grad_input, grad_output_contig, weight_contig,
      padding, stride, dilation, groups, benchmark, deterministic, allow_tf32);

  return *grad_input;
}

Tensor cudnn_convolution_transpose_forward(
    CheckedFrom c,
    const TensorArg& grad_output, const TensorArg& weight,
    IntArrayRef padding, IntArrayRef output_padding, IntArrayRef stride, IntArrayRef dilation, int64_t groups,
    bool benchmark, bool deterministic, bool allow_tf32)
{
  auto input_size = conv_input_size(grad_output->sizes(), weight->sizes(),
                                    padding, output_padding, stride, dilation, groups);
  return cudnn_convolution_backward_input(c, input_size, grad_output, weight,
                                    padding, stride, dilation, groups, benchmark, deterministic, allow_tf32);
}

Tensor cudnn_convolution_backward_input(
    IntArrayRef input_size, const Tensor& grad_output_t, const Tensor& weight_t,
    IntArrayRef padding, IntArrayRef stride, IntArrayRef dilation, int64_t groups,
    bool benchmark, bool deterministic, bool allow_tf32)
{
  TensorArg grad_output{ grad_output_t, "grad_output", 1 },
            weight{ weight_t, "weight", 2 };
  return cudnn_convolution_backward_input(
      "cudnn_convolution_backward_input",
      input_size, grad_output, weight,
      padding, stride, dilation, groups, benchmark, deterministic, allow_tf32);
}

Tensor cudnn_convolution_transpose(
    const Tensor& input_t, const Tensor& weight_t,
    IntArrayRef padding, IntArrayRef output_padding, IntArrayRef stride, IntArrayRef dilation,
    int64_t groups, bool benchmark, bool deterministic, bool allow_tf32)
{
  TensorArg input  { input_t,  "input",  1 },
            weight { weight_t, "weight", 2 };
  CheckedFrom c = "cudnn_convolution_transpose";
  auto output_t = cudnn_convolution_transpose_forward(
    c, input, weight, padding, output_padding, stride, dilation, groups, benchmark, deterministic, allow_tf32);
  return output_t;
}

// ---------------------------------------------------------------------
//
// Convolution backward (weight)
//
// ---------------------------------------------------------------------

Tensor cudnn_convolution_backward_weight(
    CheckedFrom c,
    IntArrayRef weight_size, const Tensor& grad_output_t, const Tensor& input_t,
    IntArrayRef padding, IntArrayRef stride, IntArrayRef dilation, int64_t groups,
    bool benchmark, bool deterministic, bool allow_tf32)
{
  auto layout = cudnn_conv_suggest_memory_format(input_t, grad_output_t);

  Tensor grad_output_contig_t = grad_output_t.contiguous(layout);
  TensorArg grad_output_contig{ grad_output_contig_t, "grad_output", 1 };

  Tensor input_contig_t = input_t.contiguous(layout);
  TensorArg input{ input_contig_t, "input", 2};

  checkAllSameType(c, {grad_output_contig, input});
  checkAllSameGPU(c, {grad_output_contig, input});

  auto grad_weight_t = at::empty(weight_size, grad_output_contig->options(), layout);

  // For uniformity with everything else, although it seems grad_weight
  // would be unambiguous too.
  TensorArg grad_weight{ grad_weight_t, "result", 0 };
  convolution_shape_check(c, input, grad_weight, grad_output_contig, padding, stride, dilation, groups);

  raw_cudnn_convolution_backward_weight_out(
      *grad_weight, *grad_output_contig, *input,
      padding, stride, dilation, groups, benchmark, deterministic, allow_tf32);

  return grad_weight_t;
}

Tensor cudnn_convolution_backward_weight(
    IntArrayRef weight_size,
    const Tensor& grad_output_t,
    const Tensor& input_t,
    IntArrayRef padding, IntArrayRef stride, IntArrayRef dilation, int64_t groups,
    bool benchmark, bool deterministic, bool allow_tf32)
{
  return cudnn_convolution_backward_weight(
      "cudnn_convolution_backward_weight",
      weight_size, grad_output_t, input_t,
      padding, stride, dilation, groups, benchmark, deterministic, allow_tf32);
}

std::tuple<at::Tensor,at::Tensor> cudnn_convolution_backward(
    const at::Tensor& input, const at::Tensor& grad_output_t, const at::Tensor& weight,
    IntArrayRef padding, IntArrayRef stride, IntArrayRef dilation, int64_t groups,
    bool benchmark, bool deterministic, bool allow_tf32, std::array<bool,2> output_mask) {

  Tensor grad_output = grad_output_t.contiguous(input.suggest_memory_format());

  Tensor grad_input, grad_weight;
  if (input.numel() == 0) {
    if (output_mask[0]) {
      grad_input = at::empty_like(input, LEGACY_CONTIGUOUS_MEMORY_FORMAT);
    }
    if (output_mask[1]) {
      grad_weight = at::zeros_like(weight, LEGACY_CONTIGUOUS_MEMORY_FORMAT);
    }
  } else {
    if (output_mask[0]) {
      grad_input = cudnn_convolution_backward_input(input.sizes(), grad_output, weight, padding, stride, dilation, groups, benchmark, deterministic, allow_tf32);
    }
    if (output_mask[1]) {
      grad_weight = cudnn_convolution_backward_weight(weight.sizes(), grad_output, input, padding, stride, dilation, groups, benchmark, deterministic, allow_tf32);
    }
  }

  return std::tuple<Tensor,Tensor>{grad_input, grad_weight};
}

Tensor cudnn_convolution_transpose_backward_weight(
    IntArrayRef weight_size,
    const Tensor& grad_output_t,
    const Tensor& input_t,
    IntArrayRef padding, IntArrayRef stride, IntArrayRef dilation, int64_t groups,
    bool benchmark, bool deterministic, bool allow_tf32)
{
  return cudnn_convolution_backward_weight(
      "cudnn_convolution_backward_weight",
      weight_size, input_t, grad_output_t,
      padding, stride, dilation, groups, benchmark, deterministic, allow_tf32);
}

std::tuple<at::Tensor,at::Tensor> cudnn_convolution_transpose_backward(
    const at::Tensor& input, const at::Tensor& grad_output_t, const at::Tensor& weight,
    IntArrayRef padding, IntArrayRef output_padding, IntArrayRef stride, IntArrayRef dilation, int64_t groups,
    bool benchmark, bool deterministic, bool allow_tf32, std::array<bool,2> output_mask) {

  Tensor grad_output = grad_output_t.contiguous(input.suggest_memory_format());

  Tensor grad_input, grad_weight;
  if (output_mask[0]) {
    grad_input = cudnn_convolution_transpose_backward_input(grad_output, weight, padding, stride, dilation, groups, benchmark, deterministic, allow_tf32);
  }
  if (output_mask[1]) {
    grad_weight = cudnn_convolution_transpose_backward_weight(weight.sizes(), grad_output, input, padding, stride, dilation, groups, benchmark, deterministic, allow_tf32);
  }

  return std::tuple<Tensor,Tensor>{grad_input, grad_weight};
}

Tensor cudnn_convolution_relu(
    const Tensor& input_t,
    const Tensor& weight_t,
    const c10::optional<Tensor>& bias_t,
    IntArrayRef stride,
    IntArrayRef padding,
    IntArrayRef dilation,
    int64_t groups) {
  auto memory_format = cudnn_conv_suggest_memory_format(input_t, weight_t);
  const Tensor input = input_t.contiguous(memory_format);
  const Tensor weight = weight_t.contiguous(memory_format);

  // FuseFrozenConvAddRelu performs some tensor shape checking
  auto output_t = at::native::empty_cuda(
      conv_output_size(
          input.sizes(), weight.sizes(), padding, stride, dilation),
      /*dtype=*/input.scalar_type(),
      /*layout=*/c10::nullopt,
      /*device=*/kCUDA,
      /*pin_memory=*/c10::nullopt,
      /*memory_format=*/memory_format);
  if (output_t.numel() == 0) {
    return output_t;
  }

  auto& ctx = at::globalContext();
  bool allow_tf32 = ctx.allowTF32CuDNN();
  auto _bias = bias_t.has_value()
          ? bias_t.value()
          : at::native::zeros(
                {output_t.size(1)},
                optTypeMetaToScalarType(output_t.options().dtype_opt()),
                output_t.options().layout_opt(),
                output_t.options().device_opt(),
                output_t.options().pinned_memory_opt());

#ifdef AT_CUDNN_CONV_BIAS_RELU_FALLBACK
  raw_cudnn_convolution_add_relu_fallback_out(
      output_t,
      input,
      weight,
      output_t, // use output_t as z to satisfy CUDNN API
      0, // alpha
      _bias,
      stride,
      padding,
      dilation,
      groups,
      false, // benchmark
      false, // deterministic
      allow_tf32  // allow_tf32
  );
#else  // AT_CUDNN_CONV_BIAS_RELU_FALLBACK
  raw_cudnn_convolution_add_relu_out(
      output_t,
      input,
      weight,
      output_t, // use output_t as z to satisfy CUDNN API
      0, // alpha
      _bias,
      stride,
      padding,
      dilation,
      groups,
      false, // benchmark
      false, // deterministic
      allow_tf32  // allow_tf32
  );
#endif

  return output_t;
}

Tensor cudnn_convolution_add_relu(
    const Tensor& input_t,
    const Tensor& weight_t,
    const Tensor& z_t,
    const c10::optional<Scalar>& alpha,
    const c10::optional<Tensor>& bias_t,
    IntArrayRef stride,
    IntArrayRef padding,
    IntArrayRef dilation,
    int64_t groups) {
  auto memory_format = cudnn_conv_suggest_memory_format(input_t, weight_t);
  const Tensor input = input_t.contiguous(memory_format);
  const Tensor weight = weight_t.contiguous(memory_format);

  // FuseFrozenConvAddRelu performs some tensor shape checking
  auto output_t = at::native::empty_cuda(
      conv_output_size(
          input.sizes(), weight.sizes(), padding, stride, dilation),
      /*dtype=*/input.scalar_type(),
      /*layout=*/c10::nullopt,
      /*device=*/kCUDA,
      /*pin_memory=*/c10::nullopt,
      /*memory_format=*/memory_format);
  if (output_t.numel() == 0) {
    return output_t;
  }

  auto& ctx = at::globalContext();
  bool allow_tf32 = ctx.allowTF32CuDNN();
  auto _alpha = alpha.has_value() ? alpha.value().to<float>() : 1.0;
  auto _bias = bias_t.has_value()
          ? bias_t.value()
          : at::native::zeros(
                {output_t.size(1)},
                optTypeMetaToScalarType(output_t.options().dtype_opt()),
                output_t.options().layout_opt(),
                output_t.options().device_opt(),
                output_t.options().pinned_memory_opt());

#ifdef AT_CUDNN_CONV_BIAS_RELU_FALLBACK
  raw_cudnn_convolution_add_relu_fallback_out(
      output_t,
      input,
      weight,
      z_t,
      _alpha,
      _bias,
      stride,
      padding,
      dilation,
      groups,
      false, // benchmark
      false, // deterministic
      allow_tf32  // allow_tf32
  );
#else  // AT_CUDNN_CONV_BIAS_RELU_FALLBACK
  raw_cudnn_convolution_add_relu_out(
      output_t,
      input,
      weight,
      z_t,
      _alpha,
      _bias,
      stride,
      padding,
      dilation,
      groups,
      false, // benchmark
      false, // deterministic
      allow_tf32  // allow_tf32
  );
#endif  // AT_CUDNN_CONV_BIAS_RELU_FALLBACK

  return output_t;
}

REGISTER_CUDA_DISPATCH(cudnn_convolution_backward_stub, &cudnn_convolution_backward);
REGISTER_CUDA_DISPATCH(cudnn_convolution_transpose_backward_stub, &cudnn_convolution_transpose_backward);

}}

#endif  // AT_CUDNN_ENABLED
