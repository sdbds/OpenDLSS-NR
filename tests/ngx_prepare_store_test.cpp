#include "d3d12_test_device.h"
#include "d3d12_nvapi.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) try {
  if (argc != 2) throw std::runtime_error("provide the prepare store test PTX");
  dx_test::D3D gpu;
  ngx::NvApi api;
  std::ifstream file(argv[1], std::ios::binary);
  if (!file) throw std::runtime_error("cannot read test PTX");
  const std::string ptx{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  struct Handles {
    ngx::NvApi& api; ID3D12Device* device;
    NVDX_ObjectHandle module = nullptr, function = nullptr;
    ~Handles() { api.destroyFunction(device, function); api.destroyModule(device, module); }
  } handles{api, gpu.device.Get()};
  handles.module = api.createModule(gpu.device.Get(), ptx);
  handles.function = api.createFunction(gpu.device.Get(), handles.module, "nr_prepare_store_test");
  for (uint32_t rows : {1u, 65536u, 65537u}) {
    const size_t words = static_cast<size_t>(rows) * 16 + 16;
    const uint64_t bytes = words * sizeof(uint16_t);
    std::vector<uint16_t> expected(words, 0xa55a);
    // Each position spans all half encodings, with different adjacent words.
    for (uint32_t row = 0; row < rows; ++row) for (uint32_t channel = 0; channel < 16; ++channel)
      expected[8 + static_cast<size_t>(row) * 16 + channel] = static_cast<uint16_t>(row + channel * 4093u);
    auto input = gpu.buffer(bytes, D3D12_HEAP_TYPE_DEFAULT);
    auto output = gpu.buffer(bytes, D3D12_HEAP_TYPE_DEFAULT);
    auto upload = gpu.buffer(bytes * 2, D3D12_HEAP_TYPE_UPLOAD);
    auto readback = gpu.buffer(bytes, D3D12_HEAP_TYPE_READBACK);
    uint16_t* mapped = nullptr;
    D3D12_RANGE empty{};
    dx_test::check(upload->Map(0, &empty, reinterpret_cast<void**>(&mapped)), "map upload");
    memcpy(mapped, expected.data(), static_cast<size_t>(bytes));
    std::fill(mapped + words, mapped + words * 2, uint16_t(0xa55a));
    upload->Unmap(0, nullptr);
    gpu.list->CopyBufferRegion(input.Get(), 0, upload.Get(), 0, bytes);
    gpu.list->CopyBufferRegion(output.Get(), 0, upload.Get(), bytes, bytes);
    gpu.transition(input.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gpu.transition(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    uint64_t source = input->GetGPUVirtualAddress() + 16, destination = output->GetGPUVirtualAddress() + 16;
    void* args[] = {&source, &destination, &rows};
    api.launch(gpu.list.Get(), handles.function, {(rows + 127) / 128, 1, 1}, {128, 1, 1}, 0, args);
    gpu.transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, bytes);
    gpu.submit();
    D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    dx_test::check(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "map result");
    const bool equal = memcmp(mapped, expected.data(), static_cast<size_t>(bytes)) == 0;
    readback->Unmap(0, &empty);
    if (!equal) throw std::runtime_error("prepare stores changed half bits or guard words");
    printf("PASS rows=%u: exact half bits in all 16 positions, 16-byte alignment, tail bounds and guards\n", rows);
  }
  return 0;
} catch (const std::exception& error) {
  fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
}
