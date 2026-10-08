#include "d3d12_test_device.h"
#include "d3d12_graph.h"
#include "nr_graph.h"
#include "numeric.h"
#include "reference.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <map>

namespace {
PFN_vkCmdDispatch originalDispatch;
PFN_vkCmdBindDescriptorSets originalBind;
PFN_vkUpdateDescriptorSets originalUpdate;
vk::Context* captureContext = nullptr;
vk::CommandTrace* captureTrace = nullptr;
VkCommandBuffer captureCommands = VK_NULL_HANDLE;
VkDescriptorSet boundSet = VK_NULL_HANDLE;
std::map<VkDescriptorSet, std::array<VkBuffer, vk::kGenericBindings>> sets;
std::vector<vk::Buffer> shaderCaptures;
VKAPI_ATTR void VKAPI_CALL captureUpdate(VkDevice device, uint32_t writes, const VkWriteDescriptorSet* data,
                                         uint32_t copies, const VkCopyDescriptorSet* copyData) {
  for (uint32_t i = 0; i < writes; ++i)
    if (data[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && data[i].dstBinding < vk::kGenericBindings)
      sets[data[i].dstSet][data[i].dstBinding] = data[i].pBufferInfo->buffer;
  originalUpdate(device, writes, data, copies, copyData);
}
VKAPI_ATTR void VKAPI_CALL captureBind(VkCommandBuffer commands, VkPipelineBindPoint point, VkPipelineLayout layout,
                                      uint32_t first, uint32_t count, const VkDescriptorSet* descriptors,
                                      uint32_t offsets, const uint32_t* dynamicOffsets) {
  if (commands == captureCommands && first == 0 && count) boundSet = descriptors[0];
  originalBind(commands, point, layout, first, count, descriptors, offsets, dynamicOffsets);
}
VKAPI_ATTR void VKAPI_CALL captureDispatch(VkCommandBuffer commands, uint32_t x, uint32_t y, uint32_t z) {
  originalDispatch(commands, x, y, z);
  if (commands != captureCommands || captureTrace->operations.empty()) return;
  const auto& last = captureTrace->operations.back();
  if (last.kind != vk::TraceOperation::Kind::Shader || last.shader != "gemm_mlp") return;
  vk::Buffer source;
  source.buffer = sets.at(boundSet)[5]; source.size = captureTrace->buffers.at(last.bindings[5]).bytes;
  vk::Buffer copy = captureContext->createBuffer(source.size, false, "MLP capture");
  captureContext->transferBarrier(commands);
  captureContext->copyBuffer(commands, source, copy, source.size);
  captureContext->transferBarrier(commands);
  shaderCaptures.push_back(copy);
}
}

int main(int argc, char** argv) try {
  const uint32_t width = argc > 1 ? static_cast<uint32_t>(std::stoul(argv[1])) : 512;
  const uint32_t height = argc > 2 ? static_cast<uint32_t>(std::stoul(argv[2])) : 512;
  const auto geometry = nr::Geometry::fromValid(width, height);
  vk::CommandTrace trace;
  std::vector<uint8_t> expected;
  int headBuffer = -1;
  struct Boundary { int buffer; std::vector<uint8_t> expected; };
  std::map<std::string, Boundary> boundaries;
  {
    vk::Context context;
    context.setCommandTrace(&trace);
    nr::Model model(context, "models/nr");
    nr::Kernels kernels(context, "build/shaders");
    kernels.setSiluTable(ref::siluTable());
    nr::Graph::Options options;
    options.captureBoundaries = argc > 3 && !strcmp(argv[3], "--boundaries");
    nr::Graph graph(context, model, kernels, geometry, options);
    auto* features = graph.allocate("input features", geometry.fullWidth * geometry.fullHeight, 16, nr::Format::F16);
    std::vector<uint16_t> values(static_cast<size_t>(features->rows) * 16);
    for (size_t i = 0; i < values.size(); ++i) values[i] = num::f16Bits(static_cast<float>(static_cast<int>((i * 37 + 19) % 129) - 64) / 512.0f);
    context.upload(features->buffer, values.data(), values.size() * 2);
    VkCommandBuffer commands = context.beginCommands();
    trace.record(commands);
    const bool captureShaders = argc > 3 && !strcmp(argv[3], "--shaders");
    if (captureShaders) {
      captureCommands = commands; captureContext = &context; captureTrace = &trace;
      originalDispatch = vkCmdDispatch; originalBind = vkCmdBindDescriptorSets; originalUpdate = vkUpdateDescriptorSets;
      vkCmdDispatch = captureDispatch; vkCmdBindDescriptorSets = captureBind; vkUpdateDescriptorSets = captureUpdate;
    }
    graph.record(commands, *features);
    if (captureShaders) { vkCmdDispatch = originalDispatch; vkCmdBindDescriptorSets = originalBind; vkUpdateDescriptorSets = originalUpdate; }
    headBuffer = trace.bufferIndex(graph.head().buffer);
    context.endAndSubmit(commands);
    expected = context.download(graph.head().buffer, graph.head().validBytes());
    for (const auto& [name, activation] : graph.boundaries())
      boundaries[name] = {trace.bufferIndex(activation->buffer), context.download(activation->buffer, activation->validBytes())};
    for (size_t i = 0; i < shaderCaptures.size(); ++i) {
      auto& buffer = shaderCaptures[i];
      boundaries["MLP-" + std::to_string(i)] = {trace.bufferIndex(buffer), context.download(buffer, buffer.size)};
      context.destroyBuffer(buffer);
    }
    context.setCommandTrace(nullptr);
  }
  size_t ptx = 0, shaders = 0;
  for (const auto& operation : trace.operations) {
    ptx += operation.kind == vk::TraceOperation::Kind::Ptx;
    if (operation.kind == vk::TraceOperation::Kind::Shader) { ++shaders; printf("shader=%s\n", operation.shader.c_str()); }
  }
  printf("trace buffers=%zu functions=%zu PTX=%zu shaders=%zu\n", trace.buffers.size(), trace.functions.size(), ptx, shaders);
  dx_test::D3D gpu;
  ngx::Dx12Graph graph(gpu.device.Get(), gpu.list.Get(), std::move(trace), "build/ngx/nr_ops.ptx",
                       ngx::GraphNumerics::Vulkan);
  gpu.submit();
  auto readback = gpu.buffer(expected.size(), D3D12_HEAP_TYPE_READBACK);
  for (int frame = 0; frame < 3; ++frame) {
    graph.record(gpu.list.Get());
    ID3D12Resource* head = graph.buffer(headBuffer);
    gpu.transition(head, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.list->CopyBufferRegion(readback.Get(), 0, head, 0, expected.size());
    gpu.transition(head, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gpu.submit();
    void* mapped = nullptr;
    D3D12_RANGE range{0, expected.size()};
    dx_test::check(readback->Map(0, &range, &mapped), "readback Map");
    const auto* actual = static_cast<const uint8_t*>(mapped);
    size_t differing = 0;
    for (size_t i = 0; i < expected.size(); ++i) differing += actual[i] != expected[i];
    printf("frame=%d differing_head_bytes=%zu/%zu\n", frame, differing, expected.size());
    D3D12_RANGE empty{};
    readback->Unmap(0, &empty);
    if (differing && boundaries.empty()) throw std::runtime_error("D3D12 head differs from Vulkan graph");
    if (!boundaries.empty()) {
      size_t failed = 0;
      auto names = nr::Graph::referenceBoundaryNames();
      for (size_t i = 0; i < shaderCaptures.size(); ++i) names.push_back("MLP-" + std::to_string(i));
      for (const auto& name : names) {
        auto found = boundaries.find(name);
        if (found == boundaries.end()) continue;
        const auto& boundary = found->second;
        auto capture = gpu.buffer(boundary.expected.size(), D3D12_HEAP_TYPE_READBACK);
        auto* source = graph.buffer(boundary.buffer);
        gpu.transition(source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.list->CopyBufferRegion(capture.Get(), 0, source, 0, boundary.expected.size());
        gpu.transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu.submit();
        D3D12_RANGE bytes{0, boundary.expected.size()};
        dx_test::check(capture->Map(0, &bytes, &mapped), "boundary Map");
        const auto* got = static_cast<const uint8_t*>(mapped);
        size_t mismatches = 0, first = 0;
        for (size_t i = 0; i < boundary.expected.size(); ++i) if (got[i] != boundary.expected[i]) { if (!mismatches) first = i; ++mismatches; }
        if (mismatches) {
          printf("boundary %s differs=%zu/%zu first=%zu actual=%02x expected=%02x\n", name.c_str(), mismatches,
                  boundary.expected.size(), first, got[first], boundary.expected[first]);
          ++failed;
        }
        capture->Unmap(0, &empty);
      }
      if (failed || differing) throw std::runtime_error("D3D12 boundary comparison failed");
    }
  }
  puts("PASS full optimized network in D3D12, bit-exact head over three recordings");
  return 0;
} catch (const std::exception& error) {
  fprintf(stderr, "FAIL: %s\n", error.what());
  return 1;
}
