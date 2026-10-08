#include "../ngx/nr_frame.cu"

extern "C" __global__ void nr_prepare_store_test(const __half* input, __half* output, uint32_t rows) {
  const uint32_t row = blockIdx.x * blockDim.x + threadIdx.x;
  if (row >= rows) return;
  __half values[16];
  #pragma unroll
  for (uint32_t i = 0; i < 16; ++i) values[i] = input[row * 16 + i];
  storeFeatures16(output + row * 16, values);
}
