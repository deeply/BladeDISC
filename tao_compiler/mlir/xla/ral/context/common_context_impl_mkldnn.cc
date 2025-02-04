// Copyright 2022 The BladeDISC Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#if defined(TAO_CPU_ONLY) && defined(TAO_ENABLE_MKLDNN)

#include "dnnl_threadpool_iface.hpp"
#include "mkl.h"
#include "tensorflow/compiler/mlir/xla/ral/context/common_context_impl.h"
#include "tensorflow/compiler/mlir/xla/ral/context/context_util.h"
#include "tensorflow/compiler/mlir/xla/ral/context/mkldnn/ideep/ideep.hpp"
#include "tensorflow/compiler/mlir/xla/ral/context/mkldnn/ideep/ideep_pin_singletons.hpp"
#include "tensorflow/compiler/mlir/xla/ral/device/cpu/cpu_driver.h"
#include "tensorflow/compiler/mlir/xla/ral/ral_base.h"
#include "tensorflow/compiler/mlir/xla/ral/ral_helper.h"

namespace tao {
namespace ral {

namespace {

enum DiscCpuMathKernelMode {
  kDiscPreferOneDNN = 1,
  kDiscPreferMKL = 0,
  kDiscPreferTuningBasedSelection = 2
};

DiscCpuMathKernelMode initDiscCpuMathKernelMode() {
  const char* env = getenv("DISC_CPU_MATH_KERNEL_MODE");
  std::string str = (env ? env : "");
  std::transform(str.begin(), str.end(), str.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (str == "mkl") {
    TAO_VLOG(1) << "Use MKL as blas by default.";
    return kDiscPreferMKL;
  } else if (str == "onednn") {
    TAO_VLOG(1) << "Use onednn as blas by default.";
    return kDiscPreferOneDNN;
  } else if (str == "autotune") {
    TAO_VLOG(1) << "Use auto-tuning strategy for blas.";
    return kDiscPreferTuningBasedSelection;
  } else {
    // default use mkl
    TAO_VLOG(1) << "Use MKL as blas by default.";
    return kDiscPreferMKL;
  }
}

DiscCpuMathKernelMode GetDiscCpuMathKernelMode() {
  static DiscCpuMathKernelMode mode = initDiscCpuMathKernelMode();
  return mode;
}

}  // namespace

using ideep::data_type;
using ideep::dims;
using ideep::format_tag;
using ideep::tensor;

struct ConvParams {
  format_tag input_format;
  format_tag filter_format;
  format_tag output_format;

  tensor src;
  tensor weight;
  dims dst_dims;
  tensor dst;
  dims strides;
  dims dilates;
  dims padding_l;
  dims padding_r;
  int groups;
};

format_tag str2format(const std::string& fmt) {
  if (fmt == "abcd") {
    return format_tag::abcd;
  } else if (fmt == "acdb") {
    return format_tag::acdb;
  } else if (fmt == "cdba") {
    return format_tag::cdba;
  } else if (fmt == "abc") {
    return format_tag::abc;
  } else if (fmt == "acb") {
    return format_tag::acb;
  }
  return format_tag::undef;
}

template <typename T>
data_type toDataType() {
  return data_type::undef;
}

template <>
data_type toDataType<float>() {
  return data_type::f32;
}

template <typename Tinput, int N, typename Tfilter = Tinput,
          typename Toutput = Tinput>
bool parseConvParams(ExecutionContext* ctx, MemRefType<Tinput, N> input,
                     MemRefType<Tfilter, N> kernel,
                     MemRefType<int32_t, 1> padding,
                     MemRefType<Toutput, N> output,
                     MemRefType<int32_t, 1> metadata, ConvParams* params) {
  for (int i = 0; i < N - 2; ++i) {
    params->padding_l.push_back(padding.data[2 * i]);
    params->padding_r.push_back(padding.data[2 * i + 1]);
  }
  if (TAO_VLOG_IS_ON(1)) {
    TAO_VLOG(0) << "input: " << input.data << ": ";
    for (int i = 0; i < N; ++i) TAO_VLOG(0) << input.sizes[i];

    TAO_VLOG(0) << "kernel: " << kernel.data << ": ";
    for (int i = 0; i < N; ++i) TAO_VLOG(0) << kernel.sizes[i];

    TAO_VLOG(0) << "output: " << output.data << ": ";
    for (int i = 0; i < N; ++i) TAO_VLOG(0) << output.sizes[i];

    TAO_VLOG(0) << "padding_l: ";
    for (int i = 0; i < params->padding_l.size(); ++i) {
      TAO_VLOG(0) << " " << params->padding_l[i];
    }
    TAO_VLOG(0) << "padding_r: ";
    for (int i = 0; i < params->padding_r.size(); ++i) {
      TAO_VLOG(0) << " " << params->padding_r[i];
    }
  }

  std::vector<char> format_buffer(N + 1, 0);
  int idx = 0;
  int ic = 0;
  for (int i = 0; i < N; ++i) {
    if (i == 1) ic = input.sizes[metadata.data[idx]];
    format_buffer[metadata.data[idx++]] = 'a' + i;
  }
  params->input_format = str2format(format_buffer.data());
  if (params->input_format == format_tag::undef) {
    ctx->signalError(Context::FAILURE, "invalid input format for conv op");
    return false;
  }
  if (TAO_VLOG_IS_ON(1)) {
    TAO_VLOG(0) << "input format: " << format_buffer.data();
  }

  int kc = kernel.sizes[metadata.data[idx]];
  format_buffer[metadata.data[idx++]] = 'b';
  format_buffer[metadata.data[idx++]] = 'a';
  for (int i = 2; i < N; ++i) {
    format_buffer[metadata.data[idx++]] = 'a' + i;
  }
  params->filter_format = str2format(format_buffer.data());
  if (params->filter_format == format_tag::undef) {
    ctx->signalError(Context::FAILURE, "invalid filter format for conv op");
    return false;
  }
  if (TAO_VLOG_IS_ON(1)) {
    TAO_VLOG(0) << "filter format: " << format_buffer.data();
  }

  for (int i = 0; i < N; ++i) {
    format_buffer[metadata.data[idx++]] = 'a' + i;
  }
  params->output_format = str2format(format_buffer.data());
  if (params->output_format == format_tag::undef) {
    ctx->signalError(Context::FAILURE, "invalid output format for conv op");
    return false;
  }
  if (TAO_VLOG_IS_ON(1)) {
    TAO_VLOG(0) << "output format: " << format_buffer.data();
  }

  for (int i = 0; i < N - 2; ++i) {
    params->strides.push_back(metadata.data[idx++]);
  }
  for (int i = 0; i < N - 2; ++i) {
    params->dilates.push_back(metadata.data[idx++]);
  }
  params->groups = ic / kc;
  if (TAO_VLOG_IS_ON(1)) {
    TAO_VLOG(0) << "strides: ";
    for (int i = 0; i < N - 2; ++i) {
      TAO_VLOG(0) << " " << params->strides[i];
    }
    TAO_VLOG(0) << "dilations: ";
    for (int i = 0; i < N - 2; ++i) {
      TAO_VLOG(0) << " " << params->dilates[i];
    }
    TAO_VLOG(0) << "ic = " << ic << ", kc = " << kc
                << ", groups = " << params->groups;
  }

  data_type input_dtype = toDataType<Tinput>();
  if (input_dtype == data_type::undef) {
    ctx->signalError(Context::FAILURE, "invalid input dtype for conv op");
    return false;
  }
  params->src = tensor{dims{input.sizes, input.sizes + N}, input_dtype,
                       params->input_format, input.data};

  data_type filter_dtype = toDataType<Tfilter>();
  if (filter_dtype == data_type::undef) {
    ctx->signalError(Context::FAILURE, "invalid filter dtype for conv op");
    return false;
  }
  params->weight = tensor{dims{kernel.sizes, kernel.sizes + N}, filter_dtype,
                          params->filter_format, kernel.data};

  data_type output_dtype = toDataType<Toutput>();
  if (output_dtype == data_type::undef) {
    ctx->signalError(Context::FAILURE, "invalid output dtype for conv op");
    return false;
  }
  params->dst = tensor{dims{output.sizes, output.sizes + N}, output_dtype,
                       params->output_format, output.data};
  params->dst_dims = params->dst.get_public_format_dims();
  return true;
}

template <typename Tinput, int N, typename Tfilter = Tinput,
          typename Toutput = Tinput>
void ral_conv(ExecutionContext* ctx, opaque_t /*stream_handle*/,
              MemRefType<Tinput, N> input, MemRefType<Tfilter, N> kernel,
              MemRefType<int32_t, 1> padding, MemRefType<Toutput, N> output,
              MemRefType<int32_t, 1> metadata) {
  if (isEmptyMemref(input) || isEmptyMemref(kernel) || isEmptyMemref(output)) {
    TAO_VLOG(1) << "ral_conv: early return for empty tensor";
    return;
  }

  ConvParams params;
  if (!parseConvParams(ctx, input, kernel, padding, output, metadata,
                       &params)) {
    ctx->signalError(Context::FAILURE, "invalid conv params");
  }

  ideep::tensor y;
  ideep::convolution_forward::compute(
      params.src, params.weight, params.dst_dims, y, params.strides,
      params.dilates, params.padding_l, params.padding_r, params.groups);
  // get engine
  // get stream

  // reorder to dst format
  y.reorder_to(params.dst);
}

TAO_RAL_API("ral_conv", "cpu", ral_conv<float, 3>);
TAO_RAL_API("ral_conv", "cpu", ral_conv<float, 4>);

template <typename Tinput, int N = 2, typename Tweight = Tinput,
          typename Toutput = Tinput>
void mkl_ral_gemm(ExecutionContext* ctx, void* stream_handle,
                  MemRefType<Tinput, N> A, MemRefType<Tweight, N> B,
                  MemRefType<Toutput, N> C, bool tp_a, bool tp_b) {
  int m = tp_a ? A.sizes[1] : A.sizes[0];
  int k = tp_a ? A.sizes[0] : A.sizes[1];
  int n = tp_b ? B.sizes[0] : B.sizes[1];

  cblas_sgemm(CblasRowMajor, tp_a ? CblasTrans : CblasNoTrans,
              tp_b ? CblasTrans : CblasNoTrans, m, n, k, 1.0,
              reinterpret_cast<Tinput*>(A.data), A.strides[0],
              reinterpret_cast<Tweight*>(B.data), B.strides[0], 0.0,
              reinterpret_cast<Toutput*>(C.data), C.strides[0]);
}

template <typename Tinput, int N = 2, typename Tweight = Tinput,
          typename Toutput = Tinput>
void onednn_ral_gemm(ExecutionContext* ctx, void* stream_handle,
                     MemRefType<Tinput, N> A, MemRefType<Tweight, N> B,
                     MemRefType<Toutput, N> C, bool tp_a, bool tp_b) {
  int m = tp_a ? A.sizes[1] : A.sizes[0];
  int k = tp_a ? A.sizes[0] : A.sizes[1];
  int n = tp_b ? B.sizes[0] : B.sizes[1];

  dnnl::sgemm(tp_a ? 'T' : 'N', tp_b ? 'T' : 'N', m, n, k, 1.0,
              reinterpret_cast<const float*>(A.data), A.strides[0],
              reinterpret_cast<const float*>(B.data), B.strides[0], 0.0,
              reinterpret_cast<float*>(C.data), C.strides[0]);
}

template <typename Tinput, int N = 2, typename Tweight = Tinput,
          typename Toutput = Tinput>
void ral_gemm(ExecutionContext* ctx, void* stream_handle,
              MemRefType<Tinput, N> A, MemRefType<Tweight, N> B,
              MemRefType<Toutput, N> C, bool tp_a, bool tp_b) {
  CpuTimer timer("ral_cpu_gemm");
  if (isEmptyMemref(A) || isEmptyMemref(B) || isEmptyMemref(C)) {
    TAO_VLOG(1) << "ral_gemm: early return for empty tensor";
    return;
  }

  int64_t m = tp_a ? A.sizes[1] : A.sizes[0];
  int64_t k = tp_a ? A.sizes[0] : A.sizes[1];
  if (k != (tp_b ? B.sizes[1] : B.sizes[0])) {
    ctx->signalError(Context::FAILURE, "mismatch contraction dim for gemm");
    return;
  }
  int64_t n = (tp_b ? B.sizes[0] : B.sizes[1]);
  DiscCpuMathKernelMode mode = GetDiscCpuMathKernelMode();
  if (mode == kDiscPreferOneDNN) {
    onednn_ral_gemm(ctx, stream_handle, A, B, C, tp_a, tp_b);
  } else if (mode == kDiscPreferMKL) {
    mkl_ral_gemm(ctx, stream_handle, A, B, C, tp_a, tp_b);
  } else {
    assert(mode == kDiscPreferTuningBasedSelection);
    ctx->signalError(Context::FAILURE,
                     "auto tuning mode for cpu gemm is not supported yet.");
    return;
  }

  timer.Stop();

  if (TAO_VLOG_IS_ON(1)) {
    int64_t bytes = sizeof(Tinput) * m * k + sizeof(Tweight) * k * n +
                    sizeof(Toutput) * m * n;
    TAO_VLOG(0) << "ral_cpu_gemm:\n"
                << "\tpa = " << A.data << "\n"
                << "\tpb = " << B.data << "\n"
                << "\tpc = " << C.data << "\n"
                << "\tm = " << m << "\n"
                << "\tn = " << n << "\n"
                << "\tk = " << k << "\n"
                << "\ttp_a = " << tp_a << "\n"
                << "\ttp_b = " << tp_b << "\n"
                << "\tMath Ops = " << 2 * m * n * k << "\n"
                << "\tBytes = " << bytes << "\n"
                << "\tBandwidth = "
                << double(bytes) / double(timer.GetNanoSeconds()) << " GB\n"
                << "\tGFLOPS = "
                << double(2 * m * n * k) / double(timer.GetNanoSeconds())
                << "\n";
  }
}

TAO_RAL_API("ral_gemm", "cpu", ral_gemm<float>);

template <typename T, int N>
int64_t GetBatchSize(MemRefType<T, N> memref) {
  int64_t batch = 1;
  for (int64_t i = 0; i < N - 2; ++i) {
    batch *= memref.sizes[i];
  }
  return batch;
}

template <typename Tinput, int N, typename Tweight = Tinput,
          typename Toutput = Tinput>
void mkl_ral_batch_gemm(ExecutionContext* ctx, void* stream_handle,
                        MemRefType<Tinput, N> A, MemRefType<Tweight, N> B,
                        MemRefType<Toutput, N> C, bool tp_a, bool tp_b) {
  int b = GetBatchSize(A);
  int m = tp_a ? A.sizes[N - 1] : A.sizes[N - 2];
  int n = tp_b ? B.sizes[N - 2] : B.sizes[N - 1];
  int k = tp_a ? A.sizes[N - 2] : A.sizes[N - 1];

  int ldA = A.strides[N - 2];
  int ldB = B.strides[N - 2];
  int ldC = C.strides[N - 2];

  CBLAS_TRANSPOSE ta = tp_a ? CblasTrans : CblasNoTrans;
  CBLAS_TRANSPOSE tb = tp_b ? CblasTrans : CblasNoTrans;
  Tinput alpha = 1.0f;
  Tinput beta = 0.0f;

  cblas_sgemm_batch_strided(CblasRowMajor, ta, tb, m, n, k, alpha, A.data, ldA,
                            m * k, B.data, ldB, k * n, beta, C.data, ldC, m * n,
                            b);
}

template <typename Tinput, int N, typename Tweight = Tinput,
          typename Toutput = Tinput>
void onednn_ral_batch_gemm(ExecutionContext* ctx, void* stream_handle,
                           MemRefType<Tinput, N> A, MemRefType<Tweight, N> B,
                           MemRefType<Toutput, N> C, bool tp_a, bool tp_b) {
  int b = GetBatchSize(A);
  int m = tp_a ? A.sizes[N - 1] : A.sizes[N - 2];
  int n = tp_b ? B.sizes[N - 2] : B.sizes[N - 1];
  int k = tp_a ? A.sizes[N - 2] : A.sizes[N - 1];

  data_type input_dtype = toDataType<Tinput>();
  tensor src{dims{b, m, k}, input_dtype,
             tp_a ? format_tag::acb : format_tag::abc, A.data};
  data_type weight_dtype = toDataType<Tweight>();
  tensor weight{dims{b, k, n}, weight_dtype,
                tp_b ? format_tag::acb : format_tag::abc, B.data};
  data_type output_dtype = toDataType<Toutput>();
  tensor output{dims{b, m, n}, output_dtype, format_tag::abc, C.data};

  ideep::matmul_forward::compute<true>(src, weight, output);
}

template <typename Tinput, int N, typename Tweight = Tinput,
          typename Toutput = Tinput>
void ral_batch_gemm(ExecutionContext* ctx, void* stream_handle,
                    MemRefType<Tinput, N> A, MemRefType<Tweight, N> B,
                    MemRefType<Toutput, N> C, bool tp_a, bool tp_b) {
  static_assert(N > 2, "batch gemm requires operands with rank higher than 2");
  CpuTimer timer("ral_cpu_batch_gemm");
  if (isEmptyMemref(A) || isEmptyMemref(B) || isEmptyMemref(C)) {
    ctx->signalError(Context::FAILURE, "ral_batch_gemm input error");
    return;
  }

  int64_t batch_a = GetBatchSize(A);
  int64_t batch_b = GetBatchSize(B);
  int64_t batch_c = GetBatchSize(C);
  if (batch_a != batch_b || batch_a != batch_c) {
    ctx->signalError(Context::FAILURE, "mismatch batch size");
    return;
  }

  int64_t m = tp_a ? A.sizes[N - 1] : A.sizes[N - 2];
  int64_t n = tp_b ? B.sizes[N - 2] : B.sizes[N - 1];
  int64_t k = tp_a ? A.sizes[N - 2] : A.sizes[N - 1];
  int64_t kb = tp_b ? B.sizes[N - 1] : B.sizes[N - 2];
  if (C.sizes[N - 2] != m || C.sizes[N - 1] != n || kb != k) {
    ctx->signalError(Context::FAILURE, "mismatch batch gemm params");
    return;
  }

  DiscCpuMathKernelMode mode = GetDiscCpuMathKernelMode();
  if (mode == kDiscPreferOneDNN) {
    onednn_ral_batch_gemm(ctx, stream_handle, A, B, C, tp_a, tp_b);
  } else if (mode == kDiscPreferMKL) {
    mkl_ral_batch_gemm(ctx, stream_handle, A, B, C, tp_a, tp_b);
  } else {
    assert(mode == kDiscPreferTuningBasedSelection);
    ctx->signalError(
        Context::FAILURE,
        "auto tuning mode for cpu batch gemm is not supported yet.");
    return;
  }

  timer.Stop();
  if (TAO_VLOG_IS_ON(1)) {
    int64_t bytes =
        batch_a * (sizeof(Tinput) * m * k + sizeof(Tweight) * k * n +
                   sizeof(Toutput) * m * n);
    TAO_VLOG(0) << "ral_cpu_batch_gemm:\n"
                << "\tpa = " << A.data << "\n"
                << "\tpb = " << B.data << "\n"
                << "\tpc = " << C.data << "\n"
                << "\tbatch = " << batch_a << "\n"
                << "\tm = " << m << "\n"
                << "\tn = " << n << "\n"
                << "\tk = " << k << "\n"
                << "\ttp_a = " << tp_a << "\n"
                << "\ttp_b = " << tp_b << "\n"
                << "\tMath Ops = " << 2 * batch_a * m * n * k << "\n"
                << "\tBytes = " << bytes << "\n"
                << "\tBandwidth = "
                << double(bytes) / double(timer.GetNanoSeconds()) << " GB\n"
                << "\tGFLOPS = "
                << double(2 * batch_a * m * n * k) /
                       double(timer.GetNanoSeconds())
                << "\n";
  }
}

TAO_RAL_API("ral_gemm", "cpu", ral_batch_gemm<float, 3>);
TAO_RAL_API("ral_gemm", "cpu", ral_batch_gemm<float, 4>);

}  // namespace ral
}  // namespace tao
#endif
