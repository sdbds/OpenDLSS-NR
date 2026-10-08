#include <cuda_fp16.h>
#include <cuda_fp8.h>
#include <stdint.h>

struct OpsParams {
  uint32_t count, channels, inWidth, inHeight, outWidth, outHeight, auxOffsetA, auxOffsetB, dual;
};

__device__ float e4(uint8_t value) {
  return __half2float(__nv_cvt_fp8_to_halfraw(value, __NV_E4M3));
}
__device__ uint8_t quantize(__half value) {
  const uint16_t bits = __half_as_ushort(value);
  if ((bits & 0x7fff) > 0x7c00) return 0;
  return __nv_cvt_halfraw_to_fp8(value, __NV_SATFINITE, __NV_E4M3);
}

extern "C" __global__ void nr_fill(uint32_t* output, uint32_t words, uint32_t value) {
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < words) output[i] = value;
}

// The elementwise operations not covered by the existing PTX tensor kernels.
// All half publication points match shaders/ops.comp.
extern "C" __global__ void nr_ops(uint32_t mode, const float* in32, const __half* in16,
    const uint8_t* in8, const uint8_t* skip8, const __half* aux, uint8_t* out8, __half* out16,
    OpsParams p) {
  const uint32_t group = (blockIdx.x + blockIdx.y * 65535u) * 256u + threadIdx.x;
  if (group * 8u >= p.count) return;
  const uint32_t channel = group * 8u % p.channels;
  const uint32_t pixel = group * 8u / p.channels;
  const uint32_t ox = mode >= 2 ? pixel % p.outWidth : 0;
  const uint32_t oy = mode >= 2 ? pixel / p.outWidth : 0;
  #pragma unroll
  for (uint32_t i = 0; i < 8; ++i) {
    const uint32_t index = group * 8u + i;
    __half value = __float2half_rn(0.0f);
    if (mode == 0) {
      out16[index] = __float2half_rn(in32[index]);
      continue;
    }
    if (mode == 1) value = in16[index];
    else if (mode == 2) {
      const uint32_t sx = ox * 2, sy = oy * 2;
      if (sx + 1 < p.inWidth && sy + 1 < p.inHeight) {
        const uint32_t a = (sy * p.inWidth + sx) * p.channels + channel + i;
        const uint32_t b = a + p.channels, c = a + p.inWidth * p.channels, d = c + p.channels;
        value = __hmul(__hadd(__hadd(in16[a], in16[b]), __hadd(in16[c], in16[d])), __float2half_rn(0.25f));
      }
    } else if (mode == 3 || mode == 4) {
      const uint32_t source = ((oy / 2) * p.inWidth + ox / 2) * p.channels + channel + i;
      const float scale = __half2float(aux[p.auxOffsetA + channel + i]);
      if (mode == 3) {
        const float first = __half2float(__float2half_rn(e4(in8[source]) * scale));
        value = __float2half_rn(__fmaf_rn(e4(skip8[index]), __half2float(aux[p.auxOffsetB + channel + i]), first));
      } else {
        value = __float2half_rn(__fmaf_rn(e4(skip8[index]), scale, __half2float(in16[source])));
      }
    }
    out8[index] = quantize(value);
    if (mode == 3 || (mode == 4 && p.dual)) out16[index] = value;
  }
}
