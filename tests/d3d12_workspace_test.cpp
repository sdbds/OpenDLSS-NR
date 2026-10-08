#include "d3d12_test_device.h"
#include "d3d12_graph.h"
#include "nr_graph.h"
#include "numeric.h"
#include "reference.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <vector>

namespace {
using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
constexpr size_t kFrames = 4;
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
struct CapturedGraph {
  vk::CommandTrace trace;
  int input = -1, head = -1;
  uint64_t inputBytes = 0, headBytes = 0;
};
CapturedGraph capture(ID3D12Device* device, uint32_t width, uint32_t height, bool reuse, bool poison) {
  CapturedGraph result;
  vk::Context context;
  VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  properties.pNext = &identity;
  vkGetPhysicalDeviceProperties2(context.physical(), &properties);
  const LUID luid = device->GetAdapterLuid();
  require(identity.deviceLUIDValid && !memcmp(identity.deviceLUID, &luid, sizeof(luid)), "capture and replay use different GPUs");
  context.setCommandTrace(&result.trace);
  const auto geometry = nr::Geometry::fromValid(width, height);
  nr::Model model(context, "models/nr");
  nr::Kernels kernels(context, "build/shaders");
  kernels.setSiluTable(ref::siluTable());
  nr::Graph::Options options;
  options.reuseWorkspace = reuse;
  options.poisonWorkspace = poison;
  nr::Graph graph(context, model, kernels, geometry, options);
  auto* input = graph.allocate("input features", geometry.fullWidth * geometry.fullHeight, 16, nr::Format::F16);
  auto commands = context.beginCommands();
  result.trace.record(commands);
  graph.record(commands, *input);
  result.input = result.trace.bufferIndex(input->buffer);
  result.head = result.trace.bufferIndex(graph.head().buffer);
  result.inputBytes = input->buffer.size;
  result.headBytes = graph.head().validBytes();
  const auto report = graph.workspaceReport();
  require(!reuse || report.find("route=reuse ") != std::string::npos, "requested workspace silently fell back");
  printf("%s\n", report.substr(0, report.find('\n')).c_str());
  VK_CHECK(vkEndCommandBuffer(commands));
  context.setCommandTrace(nullptr);
  return result;
}
struct Result {
  uint64_t allocatedBytes = 0;
  std::array<std::vector<uint8_t>, kFrames> heads;
};
Result replay(dx_test::D3D& gpu, uint32_t width, uint32_t height, bool reuse, bool poison) {
  auto captured = capture(gpu.device.Get(), width, height, reuse, poison);
  ngx::Dx12Graph graph(gpu.device.Get(), gpu.list.Get(), std::move(captured.trace), "build/ngx/nr_ops.ptx",
                       ngx::GraphNumerics::Native);
  gpu.submit();
  graph.releaseUploadAfterCompletion();
  Result result;
  result.allocatedBytes = graph.allocatedBytes();
  auto* input = graph.buffer(captured.input);
  auto* head = graph.buffer(captured.head);
  require(input != head, "workspace aliases the caller input");
  const auto inputAddress = input->GetGPUVirtualAddress(), headAddress = head->GetGPUVirtualAddress();
  std::array<Resource, kFrames> uploads, readbacks;
  const uint16_t boundaries[] = {0x0000, 0x8000, 0x0001, 0x8001, 0x03ff, 0x83ff, 0x0400, 0x8400,
                                0x3000, 0xb000, 0x3800, 0xb800, 0x3801, 0xb801, 0x3bff, 0xbbff};
  for (size_t frame = 0; frame < kFrames; ++frame) {
    uploads[frame] = gpu.buffer(captured.inputBytes, D3D12_HEAP_TYPE_UPLOAD);
    readbacks[frame] = gpu.buffer(captured.headBytes, D3D12_HEAP_TYPE_READBACK);
    uint16_t* mapped = nullptr;
    D3D12_RANGE empty{};
    dx_test::check(uploads[frame]->Map(0, &empty, reinterpret_cast<void**>(&mapped)), "input Map");
    for (size_t i = 0; i < captured.inputBytes / 2; ++i) {
      mapped[i] = frame == 1 ? boundaries[i % std::size(boundaries)] :
        num::f16Bits(static_cast<float>(static_cast<int>((i * 37 + frame * 47 + 19) % 2049) - 1024) / 4096.0f);
    }
    uploads[frame]->Unmap(0, nullptr);
    gpu.transition(input, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu.list->CopyBufferRegion(input, 0, uploads[frame].Get(), 0, captured.inputBytes);
    gpu.transition(input, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    graph.record(gpu.list.Get());
    gpu.transition(head, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.list->CopyBufferRegion(readbacks[frame].Get(), 0, head, 0, captured.headBytes);
    gpu.transition(head, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // The last two evaluations and their head consumers share one submission.
    // Separate immutable upload buffers keep the queued inputs distinct.
    if (frame < 2 || frame == kFrames - 1) gpu.submit();
    require(input->GetGPUVirtualAddress() == inputAddress && head->GetGPUVirtualAddress() == headAddress &&
            graph.allocatedBytes() == result.allocatedBytes, "replay relocated or reallocated graph storage");
  }
  for (size_t frame = 0; frame < kFrames; ++frame) {
    void* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(captured.headBytes)}, empty{};
    dx_test::check(readbacks[frame]->Map(0, &range, &mapped), "head Map");
    const auto* bytes = static_cast<const uint8_t*>(mapped);
    result.heads[frame].assign(bytes, bytes + captured.headBytes);
    readbacks[frame]->Unmap(0, &empty);
  }
  require(result.heads[0] != result.heads[1], "distinct uploaded inputs produced an unchanged reference head");
  printf("native replay reuse=%d poison=%d allocated_bytes=%llu frames=%zu submissions=3\n",
         reuse, poison, static_cast<unsigned long long>(result.allocatedBytes), kFrames);
  return result;
}
void compare(const Result& expected, const Result& actual, const char* route) {
  for (size_t frame = 0; frame < kFrames; ++frame) {
    require(expected.heads[frame].size() == actual.heads[frame].size(), "head size changed");
    size_t differing = 0, first = 0;
    for (size_t i = 0; i < expected.heads[frame].size(); ++i) if (expected.heads[frame][i] != actual.heads[frame][i]) {
      if (!differing) first = i;
      ++differing;
    }
    printf("%s frame=%zu differing_head_bytes=%zu first=%zu\n", route, frame, differing, first);
    require(!differing, "native D3D12 workspace head differs from dedicated execution");
  }
}
}

int main(int argc, char** argv) try {
  const uint32_t width = argc > 1 ? std::stoul(argv[1]) : 513;
  const uint32_t height = argc > 2 ? std::stoul(argv[2]) : 377;
  dx_test::D3D gpu;
  const auto dedicated = replay(gpu, width, height, false, false);
  const auto reuse = replay(gpu, width, height, true, false);
  const auto poison = replay(gpu, width, height, true, true);
  require(reuse.allocatedBytes < dedicated.allocatedBytes, "workspace did not reduce real D3D12 allocations");
  require(poison.allocatedBytes == reuse.allocatedBytes, "poisoning changed workspace capacity");
  compare(dedicated, reuse, "reuse");
  compare(dedicated, poison, "poison");
  puts("PASS native D3D12 workspace allocation reduction, exact changing-input heads, poison and queued consumers");
  return 0;
} catch (const std::exception& error) {
  fprintf(stderr, "FAIL: %s\n", error.what());
  return 1;
}
