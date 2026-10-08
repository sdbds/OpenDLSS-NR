#include "d3d12_test_device.h"
#include "d3d12_nvapi.h"

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>

int main(int argc, char** argv) try {
  dx_test::D3D gpu;
  ngx::NvApi api;
  const char* path = argc > 1 ? argv[1] : "build/ngx/global_normalize_e4m3_native.ptx";
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot read normalization PTX");
  const std::string ptx{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  auto module = api.createModule(gpu.device.Get(), ptx);
  auto function = api.createFunction(gpu.device.Get(), module, "global_normalize_e4m3");
  // Captured from synthetic frame 2 at 3840x2160, ViT block 37. The tiny
  // high-half square must not disappear through an intermediate FP32 rounding.
  constexpr uint16_t query[32] = {
    0x3f04, 0x385d, 0xbbec, 0xb36c, 0x3a0c, 0x3bfa, 0xb44f, 0xb588,
    0x3dc8, 0xbbe0, 0xc032, 0x3f0c, 0xb9d7, 0x41e0, 0xbc30, 0x3f3e,
    0xa7d8, 0xade0, 0x22a0, 0x3add, 0xc177, 0x40d6, 0x3d3b, 0xb9e4,
    0x4093, 0x4199, 0xae60, 0x34ab, 0x3cda, 0x0c00, 0xbb06, 0xc076
  };
  constexpr uint8_t expected[32] = {
    0x40, 0x32, 0xb9, 0xa9, 0x36, 0x39, 0xaa, 0xad, 0x3e, 0xb9, 0xc2, 0x40, 0xb6, 0x46, 0xba, 0x41,
    0x91, 0x9e, 0x08, 0x38, 0xc5, 0x43, 0x3c, 0xb6, 0x43, 0x45, 0x9f, 0x2b, 0x3b, 0x00, 0xb8, 0xc2
  };
  constexpr uint32_t inputBytes = 64 * 96 * 2, outputBytes = 64 * 96;
  std::array<uint8_t, inputBytes + 16> data{};
  memcpy(data.data(), query, sizeof(query));
  const uint32_t scaleBits = 0x3fd9e000;
  memcpy(data.data() + inputBytes, &scaleBits, sizeof(scaleBits));
  auto input = gpu.buffer(data.size(), D3D12_HEAP_TYPE_DEFAULT);
  auto output = gpu.buffer(outputBytes, D3D12_HEAP_TYPE_DEFAULT);
  auto upload = gpu.buffer(data.size(), D3D12_HEAP_TYPE_UPLOAD);
  auto readback = gpu.buffer(outputBytes, D3D12_HEAP_TYPE_READBACK);
  uint8_t* mapped = nullptr;
  D3D12_RANGE empty{};
  dx_test::check(upload->Map(0, &empty, reinterpret_cast<void**>(&mapped)), "Map normalization input");
  memcpy(mapped, data.data(), data.size());
  upload->Unmap(0, nullptr);
  gpu.list->CopyBufferRegion(input.Get(), 0, upload.Get(), 0, data.size());
  gpu.transition(input.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.transition(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  uint64_t qkv = input->GetGPUVirtualAddress(), aux = qkv + inputBytes, out = output->GetGPUVirtualAddress(), zero64 = 0;
  uint32_t tokens = 1, padded = 64, heads = 1, zero = 0;
  void* arguments[] = {&qkv, &aux, &out, &tokens, &padded, &heads, &zero, &zero64, &zero, &zero64, &zero64};
  api.launch(gpu.list.Get(), function, {1, 1, 1}, {64, 1, 1}, 0, arguments);
  gpu.transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  gpu.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, outputBytes);
  gpu.submit();
  D3D12_RANGE range{0, outputBytes};
  dx_test::check(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "Map normalized query");
  size_t different = 0;
  for (size_t i = 0; i < 32; ++i) if (mapped[i] != expected[i]) {
    printf("query[%zu] got=%02x expected=%02x\n", i, mapped[i], expected[i]); ++different;
  }
  readback->Unmap(0, &empty);
  api.destroyFunction(gpu.device.Get(), function);
  api.destroyModule(gpu.device.Get(), module);
  if (different) throw std::runtime_error("ViT normalization differs from the native FP16 fused norm");
  puts("PASS native ViT normalization including the FP16 double-rounding boundary");
  return 0;
} catch (const std::exception& error) {
  fprintf(stderr, "FAIL: %s\n", error.what());
  return 1;
}
