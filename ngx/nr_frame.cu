#include "frame_params.h"
#include <cuda_runtime.h>
#include <cuda_fp16.h>

__device__ float rounded(float x) { return __half2float(__float2half_rn(x)); }
__device__ float logApprox(float x) { float y; asm("lg2.approx.ftz.f32 %0, %1;" : "=f"(y) : "f"(x)); return y; }
__device__ float sqrtApprox(float x) { float y; asm("sqrt.approx.ftz.f32 %0, %1;" : "=f"(y) : "f"(x)); return y; }
__device__ float sinApprox(float x) { float y; asm("sin.approx.ftz.f32 %0, %1;" : "=f"(y) : "f"(x)); return y; }
__device__ float cosApprox(float x) { float y; asm("cos.approx.ftz.f32 %0, %1;" : "=f"(y) : "f"(x)); return y; }
__device__ float expApprox(float x) { float y; asm("ex2.approx.ftz.f32 %0, %1;" : "=f"(y) : "f"(x)); return y; }
__device__ float reciprocalApprox(float x) { float y; asm("rcp.approx.ftz.f32 %0, %1;" : "=f"(y) : "f"(x)); return y; }
__device__ float uniform(uint32_t x) {
  x = (x >> ((x >> 28) + 4)) ^ x;
  x *= 0x108ef2d9u;
  return __uint2float_rn(((x >> 30) ^ (x >> 8)) + 1) * __uint_as_float(0x33800000u);
}
__device__ float3 noise(uint32_t x, uint32_t y, uint32_t seed) {
  uint32_t base = (x * 0x8da6b343u) ^ (seed * 0x9e3779b9u) ^ (y * 0xd8163841u) ^ 0x243f6a88u;
  base = (base >> ((base >> 28) + 4)) ^ base;
  base *= 0x108ef2d9u;
  base = (base >> 22) ^ base;
  const float u0 = uniform(base * 0x2c9277b5u + 0xac564b05u), u1 = uniform(base * 0xfa6dc5f9u + 0x4712a88eu);
  const float u2 = uniform(base * 0xcaa5b80du + 0x21dd796bu), u3 = uniform(base * 0x83232c31u + 0x3463e0acu);
  const float r0 = sqrtApprox(logApprox(u0) * __uint_as_float(0x3f317218u) * -2.0f);
  const float r1 = sqrtApprox(logApprox(u2) * __uint_as_float(0x3f317218u) * -2.0f);
  const float a0 = u1 * __uint_as_float(0x40c90fdbu), a1 = u3 * __uint_as_float(0x40c90fdbu);
  return make_float3(rounded(r0 * cosApprox(a0)), rounded(r0 * sinApprox(a0)), rounded(r1 * cosApprox(a1)));
}
__device__ float centered(float x) { return rounded(rounded(rounded(x) - 0.5f) * 0.125f); }
__device__ float4 colorAt(cudaTextureObject_t color, uint32_t x, uint32_t y, const NrFrameParams& p) {
  return tex2D<float4>(color, (static_cast<float>(x + p.colorX) + 0.5f) / p.colorWidth,
                            (static_cast<float>(y + p.colorY) + 0.5f) / p.colorHeight);
}
struct HistoryAxis { float low, center, high, w0, middle, w3; };
__device__ HistoryAxis historyAxis(float coordinate, float size) {
  const float base = floorf(__fmaf_rn(size, coordinate, -0.5f)) + 0.5f;
  const float f = fminf(fmaxf(__fmaf_rn(size, coordinate, -base), 0.0f), 1.0f);
  const float square = f * f, cube = f * square;
  const float w0 = __fmaf_rn(f + cube, -0.5f, square);
  const float w1 = __fmaf_rn(cube, 1.5f, -(square * 2.5f)) + 1.0f;
  const float w3 = (cube - square) * 0.5f;
  const float w2 = ((1.0f - w0) - w1) - w3, middle = w1 + w2;
  const float inverse = reciprocalApprox(size);
  return {fminf(fmaxf(base - 1.0f, 0.5f), size - 0.5f) * inverse,
          fminf(fmaxf(__fmaf_rn(w2, reciprocalApprox(middle), base), 0.5f), size - 0.5f) * inverse,
          fminf(fmaxf(base + 2.0f, 0.5f), size - 0.5f) * inverse, w0, middle, w3};
}
__device__ float4 historySample(cudaTextureObject_t previous, float u, float v, const NrFrameParams& p,
                                 float& reciprocal) {
  auto x = historyAxis(u, static_cast<float>(p.width));
  auto y = historyAxis(v, static_cast<float>(p.height));
  // Preserve the native subrectangle-to-texture mapping even for an identity
  // rectangle: its two roundings affect Catmull-Rom taps at odd dimensions.
  const float inverseX = __frcp_rn(static_cast<float>(p.width));
  const float inverseY = __frcp_rn(static_cast<float>(p.height));
  x.low = (x.low * p.width) * inverseX; x.center = (x.center * p.width) * inverseX; x.high = (x.high * p.width) * inverseX;
  y.low = (y.low * p.height) * inverseY; y.center = (y.center * p.height) * inverseY; y.high = (y.high * p.height) * inverseY;
  const float4 left = tex2D<float4>(previous, x.low, y.center), top = tex2D<float4>(previous, x.center, y.low);
  const float4 mid = tex2D<float4>(previous, x.center, y.center), bottom = tex2D<float4>(previous, x.center, y.high);
  const float4 right = tex2D<float4>(previous, x.high, y.center);
  const float a = x.w0 * y.middle, b = y.w0 * x.middle, c = x.middle * y.middle;
  const float d = y.w3 * x.middle, e = x.w3 * y.middle;
  reciprocal = reciprocalApprox(e + (d + (c + (a + b))));
  float4 result;
  #define CHANNEL(ch) result.ch = __fmaf_rn(right.ch, e, __fmaf_rn(bottom.ch, d, __fmaf_rn(mid.ch, c, __fmaf_rn(left.ch, a, top.ch * b))))
  CHANNEL(x); CHANNEL(y); CHANNEL(z); CHANNEL(w);
  #undef CHANNEL
  return result;
}
__device__ bool historyAt(cudaTextureObject_t previous, cudaTextureObject_t motion, uint32_t x, uint32_t y,
                           const NrFrameParams& p, float4& result, float* normalization = nullptr) {
  if (!p.historyValid) return false;
  float2 mv = make_float2(0.0f, 0.0f);
  if (motion) mv = tex2D<float2>(motion, (static_cast<float>(x + p.motionX) + 0.5f) / p.motionWidth,
                                       (static_cast<float>(y + p.motionY) + 0.5f) / p.motionHeight);
  const float pixelX = __fmaf_rn(mv.x, p.motionScaleX,
                               (static_cast<float>(x) + 0.5f) * reciprocalApprox(static_cast<float>(p.width)));
  const float pixelY = __fmaf_rn(mv.y, p.motionScaleY,
                               (static_cast<float>(y) + 0.5f) * reciprocalApprox(static_cast<float>(p.height)));
  float reciprocal;
  result = historySample(previous, pixelX, pixelY, p, reciprocal);
  if (normalization) *normalization = reciprocal;
  else {
    result.x *= reciprocal; result.y *= reciprocal; result.z *= reciprocal; result.w *= reciprocal;
  }
  return true;
}

__device__ __forceinline__ void storeFeatures16(__half* output, const __half* values) {
  uint32_t words[8];
  #pragma unroll
  for (uint32_t i = 0; i < 8; ++i) {
    words[i] = static_cast<uint32_t>(__half_as_ushort(values[i * 2])) |
               (static_cast<uint32_t>(__half_as_ushort(values[i * 2 + 1])) << 16);
  }
  // Feature buffers are GPU-allocation aligned; each pixel occupies 32 bytes.
  auto* packed = reinterpret_cast<uint4*>(output);
  packed[0] = make_uint4(words[0], words[1], words[2], words[3]);
  packed[1] = make_uint4(words[4], words[5], words[6], words[7]);
}

extern "C" __global__ void nr_prepare(cudaTextureObject_t color, cudaTextureObject_t previous,
    cudaTextureObject_t motion, __half* features, NrFrameParams p) {
  const uint32_t x = blockIdx.x * blockDim.x + threadIdx.x;
  const uint32_t y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= p.fullWidth || y >= p.fullHeight) return;
  const uint32_t sx = x < p.width ? x : 2 * p.width - x - 2;
  const uint32_t sy = y < p.height ? y : 2 * p.height - y - 2;
  const float4 rgba = colorAt(color, sx, sy, p);
  float4 previousColor = rgba;
  historyAt(previous, motion, sx, sy, p, previousColor);
  const float3 n = noise(x, y, p.seed);
  float values[16] = {n.x, n.y, n.z, 1.0f, centered(rgba.x), centered(rgba.y), centered(rgba.z),
    centered(previousColor.x), centered(previousColor.y), centered(previousColor.z), p.style / 128.0f,
    rounded(p.localTone), rounded(p.autoMask ? 1.0f : p.localStructure),
    rounded(p.autoMask ? (p.skinStructure < 0.0f ? p.localStructure : p.skinStructure) : -1.0f),
    rounded(p.autoMask ? p.localStructure : -1.0f), 0.0f};
  const uint32_t base = (y * p.fullWidth + x) * 16;
  __half halfValues[16];
  #pragma unroll
  for (uint32_t i = 0; i < 16; ++i) halfValues[i] = __float2half_rn(values[i]);
  storeFeatures16(features + base, halfValues);
}

__device__ ushort4 halfPixel(float4 value) {
  return make_ushort4(__half_as_ushort(__float2half_rz(value.x)), __half_as_ushort(__float2half_rz(value.y)),
                     __half_as_ushort(__float2half_rz(value.z)), __half_as_ushort(__float2half_rz(value.w)));
}

extern "C" __global__ void nr_compose(cudaTextureObject_t color, cudaTextureObject_t previous,
    cudaTextureObject_t motion, const float4* head, cudaSurfaceObject_t next, cudaSurfaceObject_t output,
    NrFrameParams p) {
  const uint32_t x = blockIdx.x * blockDim.x + threadIdx.x;
  const uint32_t y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= p.width || y >= p.height) return;
  const float4 proxy = colorAt(color, x, y, p);
  if (!p.enabled) {
    surf2Dwrite(halfPixel(proxy), output, (x + p.outputX) * 8, y + p.outputY);
    return;
  }
  float4 result = proxy;
  if (p.enabled) {
    const float4 h = head[y * p.fullWidth + x];
    result.x = fminf(fmaxf(__fmaf_rn(h.x, 0.03125f, __fmaf_rn(proxy.x, 0.125f, -0.0625f)) * 8.0f + 0.5f, 0.0f), 1.0f);
    result.y = fminf(fmaxf(__fmaf_rn(h.y, 0.03125f, __fmaf_rn(proxy.y, 0.125f, -0.0625f)) * 8.0f + 0.5f, 0.0f), 1.0f);
    result.z = fminf(fmaxf(__fmaf_rn(h.z, 0.03125f, __fmaf_rn(proxy.z, 0.125f, -0.0625f)) * 8.0f + 0.5f, 0.0f), 1.0f);
    float4 history;
    float normalization;
    if (historyAt(previous, motion, x, y, p, history, &normalization)) {
      const float weight = fminf(fmaxf(reciprocalApprox(1.0f + expApprox(h.w * __uint_as_float(0xbfb8aa3bu))) * p.blendScale, 0.0f), 1.0f);
      // The native post block fuses reconstruction normalization with this
      // subtraction. Rounding the normalized history first changes half bits.
      result.x = __fmaf_rn(weight, __fmaf_rn(normalization, history.x, -result.x), result.x);
      result.y = __fmaf_rn(weight, __fmaf_rn(normalization, history.y, -result.y), result.y);
      result.z = __fmaf_rn(weight, __fmaf_rn(normalization, history.z, -result.z), result.z);
    }
  }
  result.w = 1.0f;
  surf2Dwrite(halfPixel(result), next, x * 8, y);
  if (p.intensity != 1.0f) {
    result.x = fminf(fmaxf(__fmaf_rn(p.intensity, result.x - proxy.x, proxy.x), 0.0f), 1.0f);
    result.y = fminf(fmaxf(__fmaf_rn(p.intensity, result.y - proxy.y, proxy.y), 0.0f), 1.0f);
    result.z = fminf(fmaxf(__fmaf_rn(p.intensity, result.z - proxy.z, proxy.z), 0.0f), 1.0f);
  }
  surf2Dwrite(halfPixel(result), output, (x + p.outputX) * 8, y + p.outputY);
}
