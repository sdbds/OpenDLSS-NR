#include "d3d12_test_device.h"
#include "d3d12_nvapi.h"

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>

int main() try {
  dx_test::D3D gpu;
  ngx::NvApi api;
  std::ifstream file("tests/d3d12_ordered_add.ptx", std::ios::binary);
  if (!file) throw std::runtime_error("cannot read ordered_add PTX");
  const std::string ptx{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  auto module = api.createModule(gpu.device.Get(), ptx);
  auto function = api.createFunction(gpu.device.Get(), module, "ordered_add");
  auto data = gpu.buffer(128, D3D12_HEAP_TYPE_DEFAULT);
  auto other = gpu.buffer(128, D3D12_HEAP_TYPE_DEFAULT);
  auto upload = gpu.buffer(128, D3D12_HEAP_TYPE_UPLOAD);
  auto readback = gpu.buffer(128, D3D12_HEAP_TYPE_READBACK);
  uint32_t* mapped = nullptr;
  D3D12_RANGE empty{};
  dx_test::check(upload->Map(0, &empty, reinterpret_cast<void**>(&mapped)), "Map upload");
  for (uint32_t i = 0; i < 32; ++i) mapped[i] = 0x1000 + i;
  upload->Unmap(0, nullptr);
  gpu.list->CopyBufferRegion(data.Get(), 0, upload.Get(), 0, 128);
  gpu.list->CopyBufferRegion(other.Get(), 0, upload.Get(), 0, 128);
  gpu.transition(data.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.transition(other.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  uint64_t pointer = data->GetGPUVirtualAddress();
  void* arguments[] = {&pointer};
  NVAPI_CU_KERNEL_LAUNCH_PARAMS_EX chain[2]{};
  for (auto& kernel : chain) {
    kernel.hFunction = function; kernel.gridDim = {1, 1, 1}; kernel.blockDim = {32, 1, 1}; kernel.kernelParams = arguments;
  }
  api.launchChain(gpu.list.Get(), chain, 2);
  pointer = other->GetGPUVirtualAddress();
  gpu.transition(data.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  gpu.list->CopyBufferRegion(readback.Get(), 0, data.Get(), 0, 128);
  gpu.submit();
  D3D12_RANGE range{0, 128};
  dx_test::check(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "Map readback");
  for (uint32_t i = 0; i < 32; ++i) {
    if (mapped[i] != ((0x1000 + i) * 3 + 7) * 3 + 7) throw std::runtime_error("dependent PTX kernels did not execute in chain order");
  }
  readback->Unmap(0, &empty);
  api.destroyFunction(gpu.device.Get(), function);
  api.destroyModule(gpu.device.Get(), module);
  puts("PASS PTX in D3D12: upload -> dependent kernel chain -> readback on the caller command list");
  return 0;
} catch (const std::exception& error) {
  fprintf(stderr, "FAIL: %s\n", error.what());
  return 1;
}
