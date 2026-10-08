#include "vk_context.h"
#include "nr_graph.h"
#include "numeric.h"
#include "reference.h"
#include "compute_trace.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template<class Function>
void rejects(Function function, const char* message) {
  bool rejected = false;
  try { function(); } catch (const std::runtime_error&) { rejected = true; }
  require(rejected, message);
}

struct TestImage {
  vk::Context& context;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkMemoryRequirements requirements{};
  uint32_t memoryType = 0;

  explicit TestImage(vk::Context& c) : context(c) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D; info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {8, 8, 1}; info.mipLevels = 1; info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    VK_CHECK(vkCreateImage(context.device(), &info, nullptr, &image));
    vkGetImageMemoryRequirements(context.device(), image, &requirements);
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(context.physical(), &properties);
    while (memoryType < properties.memoryTypeCount &&
           (!(requirements.memoryTypeBits & (1u << memoryType)) ||
            !(properties.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) ++memoryType;
    require(memoryType < properties.memoryTypeCount, "no image memory type");
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = memoryType;
    VK_CHECK(vkAllocateMemory(context.device(), &allocation, nullptr, &memory));
    VK_CHECK(vkBindImageMemory(context.device(), image, memory, 0));
  }
  ~TestImage() {
    if (image) vkDestroyImage(context.device(), image, nullptr);
    if (memory) vkFreeMemory(context.device(), memory, nullptr);
  }
};

template<class Context>
void accounting(Context& context) {
  if constexpr (!requires { context.memorySnapshot(); }) {
    throw std::runtime_error("memory accounting interface is missing");
  } else {
    const auto initial = context.memorySnapshot();
    using Owner = decltype(initial.records.front().owner);
    const auto ownerIndex = [](Owner owner) { return static_cast<size_t>(owner); };
    require(initial.records.size() >= 2, "Context allocations missing from ledger");
    const auto staging = std::find_if(initial.records.begin(), initial.records.end(),
                                      [](const auto& record) { return record.label == "staging"; });
    const auto dummy = std::find_if(initial.records.begin(), initial.records.end(),
                                    [](const auto& record) { return record.label == "dummy binding"; });
    require(staging != initial.records.end() && dummy != initial.records.end(), "staging/dummy records missing");
    require(staging->logicalBytes > 0 && staging->allocatedBytes >= staging->logicalBytes &&
            initial.liveBytes >= staging->allocatedBytes + dummy->allocatedBytes, "staging/dummy capacity missing");
    require(initial.owners[ownerIndex(Owner::Context)].liveBytes == initial.liveBytes,
            "Context-owned allocations have the wrong owner");

    char label[] = "accounting buffer";
    auto buffer = context.createBuffer(17, false, label, 0, Owner::Graph);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(context.device(), buffer.buffer, &requirements);
    const auto added = context.memorySnapshot();
    const auto entry = std::find_if(added.records.begin(), added.records.end(),
                                   [&](const auto& record) { return record.memory == buffer.memory; });
    require(entry != added.records.end(), "buffer missing from ledger");
    require(entry->logicalBytes == 17 && entry->allocatedBytes == requirements.size,
            "logical bytes confused with allocation size");
    require(added.liveBytes == initial.liveBytes + requirements.size, "incorrect live byte delta");
    require(added.allocationCount == initial.allocationCount + 1, "allocation count not updated");
    require(added.owners[ownerIndex(Owner::Graph)].liveBytes == requirements.size, "owner bytes incorrect");
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(context.physical(), &properties);
    require(entry->heap == properties.memoryTypes[entry->memoryType].heapIndex, "wrong memory heap");
    require(added.heaps[entry->heap].liveBytes == initial.heaps[entry->heap].liveBytes + requirements.size,
            "heap bytes incorrect");
    label[0] = 'X';
    const auto copied = context.memorySnapshot();
    const auto copy = std::find_if(copied.records.begin(), copied.records.end(),
                                  [&](const auto& record) { return record.memory == buffer.memory; });
    require(copy->label == "accounting buffer", "ledger borrowed a temporary label");
    rejects([&] { context.untrackImageAllocation(buffer.memory); }, "image removal accepted a buffer");
    context.destroyBuffer(buffer);
    const auto removed = context.memorySnapshot();
    require(removed.liveBytes == initial.liveBytes && removed.records.size() == initial.records.size(),
            "buffer release did not restore live bytes");
    require(removed.peakBytes == added.peakBytes && removed.freeCount == initial.freeCount + 1,
            "release corrupted peak/free counters");

    TestImage image(context);
    context.trackImageAllocation(image.memory, 256, image.requirements.size, image.memoryType, "test image");
    const auto imageAdded = context.memorySnapshot();
    require(imageAdded.liveBytes == initial.liveBytes + image.requirements.size, "image not tracked");
    require(imageAdded.owners[ownerIndex(Owner::Pass)].liveBytes == image.requirements.size,
            "image owner bytes incorrect");
    rejects([&] { context.trackImageAllocation(image.memory, 256, image.requirements.size,
                                               image.memoryType, "duplicate"); }, "duplicate image accepted");
    require(context.memorySnapshot().allocationCount == imageAdded.allocationCount,
            "rejected image changed counters");
    context.untrackImageAllocation(image.memory);
    rejects([&] { context.untrackImageAllocation(image.memory); }, "duplicate image removal accepted");
    require(context.memorySnapshot().liveBytes == initial.liveBytes, "image removal corrupted live bytes");
  }
}

bool flag(int argc, char** argv, const char* name) {
  return std::find(argv + 1, argv + argc, std::string(name)) != argv + argc;
}
uint32_t number(int argc, char** argv, const char* name, uint32_t fallback) {
  for (int i = 1; i + 1 < argc; ++i) if (std::string(name) == argv[i]) return std::stoul(argv[i + 1]);
  return fallback;
}
struct OwnedBuffer {
  vk::Context& context;
  vk::Buffer buffer;
  ~OwnedBuffer() { context.destroyBuffer(buffer); }
};
uint64_t graphBytes(const vk::Context& context) {
  return context.memorySnapshot().owners[static_cast<size_t>(vk::MemoryOwner::Graph)].liveBytes;
}
void stableMemory(const vk::MemorySnapshot& before, const vk::MemorySnapshot& after) {
  require(before.allocationCount == after.allocationCount && before.freeCount == after.freeCount &&
          before.liveBytes == after.liveBytes, "re-record changed physical allocations");
  require(before.records.size() == after.records.size(), "re-record changed buffer ownership");
  for (size_t i = 0; i < before.records.size(); ++i)
    require(before.records[i].memory == after.records[i].memory, "re-record relocated an allocation");
}
void checkTrace(const vk::CommandTrace& trace, const nr::Activation& head) {
  const int headIndex = trace.bufferIndex(head.buffer);
  require(headIndex >= 0, "head view did not resolve to its physical buffer");
  bool headReferenced = false;
  for (const auto& operation : trace.operations) {
    for (const auto& argument : operation.arguments) {
      if (argument.buffer < 0) continue;
      require(static_cast<size_t>(argument.buffer) < trace.buffers.size(), "trace pointer has invalid buffer");
      require(argument.value < trace.buffers[argument.buffer].bytes, "trace pointer lies outside physical slot");
      headReferenced |= argument.buffer == headIndex;
    }
    for (int binding : operation.bindings) {
      require(binding < 0 || static_cast<size_t>(binding) < trace.buffers.size(), "trace descriptor has invalid buffer");
      headReferenced |= binding == headIndex;
    }
  }
  require(headReferenced, "head's physical slot was not used by recorded commands");
}

template<class Graph>
void workspace(vk::Context& context, int argc, char** argv) {
  using Options = typename Graph::Options;
  if constexpr (!requires(Options options, const Graph& graph) {
    options.reuseWorkspace = true; options.poisonWorkspace = true; graph.workspaceReport();
  }) {
    throw std::runtime_error("workspace integration interface is missing");
  } else {
    vk::CommandTrace trace;
    const bool traceEnabled = flag(argc, argv, "--trace");
    if (traceEnabled) context.setCommandTrace(&trace);
    // Detach last, after resource destructors have removed their trace entries.
    struct Detach { vk::Context& context; ~Detach() { context.setCommandTrace(nullptr); } } detach{context};
    nr::Model model(context, "models/nr", true);
    nr::Kernels kernels(context, "build/shaders");
    kernels.setSiluTable(ref::siluTable());
    const auto geometry = nr::Geometry::fromValid(number(argc, argv, "--width", 513), number(argc, argv, "--height", 377));
    const uint32_t rows = geometry.fullWidth * geometry.fullHeight;
    const bool floatInput = flag(argc, argv, "--fp32-input");
    nr::Activation input;
    input.label = "borrowed input"; input.rows = rows; input.allocRows = nr::alignRows(rows); input.channels = 16;
    input.format = floatInput ? nr::Format::F32 : nr::Format::F16;
    OwnedBuffer owner{context, context.createBuffer(input.validBytes(), false, "borrowed input")};
    input.buffer = owner.buffer;
    std::vector<uint8_t> inputBytes(input.validBytes());
    for (size_t i = 0; i < size_t(rows) * 16; ++i) {
      float value = std::sin(float(i) * 0.0017f) * 0.125f;
      if (i % 2048 == 0) value = -0.0f;
      if (i % 2048 == 1) value = std::ldexp(1.0f, -24);
      if (i % 2048 == 2) value = 1.00048828125f;
      if (floatInput) memcpy(inputBytes.data() + i * 4, &value, 4);
      else { const uint16_t half = num::f16Bits(value); memcpy(inputBytes.data() + i * 2, &half, 2); }
    }
    context.upload(input.buffer, inputBytes.data(), inputBytes.size());
    Options options;
    options.fp16Head = flag(argc, argv, "--fp16-head");
    options.captureBoundaries = flag(argc, argv, "--boundaries") || flag(argc, argv, "--intermediates");
    options.captureIntermediates = flag(argc, argv, "--intermediates");
    const bool fallback = options.captureBoundaries || options.captureIntermediates || flag(argc, argv, "--expect-fallback");
    std::vector<uint8_t> reference;
    uint64_t dedicatedBytes = 0;
    for (int mode = 0; mode < 3; ++mode) {
      options.reuseWorkspace = mode != 0;
      options.poisonWorkspace = mode == 2;
      require(graphBytes(context) == 0, "previous Graph leaked physical storage");
      {
        Graph graph(context, model, kernels, geometry, options);
        auto run = [&] {
          context.resetDescriptorPool();
          auto commands = context.beginCommands();
          if (traceEnabled) trace.record(commands);
          graph.record(commands, input);
          context.endAndSubmit(commands, true);
          require(kernels.chainTimeouts().waits == 0, "workspace caused a chain timeout");
        };
        run();
        const auto bytes = context.download(graph.head().buffer, graph.head().validBytes());
        if (mode == 0) { reference = bytes; dedicatedBytes = graphBytes(context); }
        else {
          require(bytes == reference, "reuse_head_matches_dedicated failed");
          if (fallback) {
            require(graph.workspaceReport().find("route=dedicated-fallback") != std::string::npos,
                    "unsupported route did not select explicit dedicated fallback");
            require(graphBytes(context) == dedicatedBytes, "fallback partially reused storage");
          } else {
            require(graph.workspaceReport().find("route=reuse") != std::string::npos, "workspace route is inactive");
            require(graphBytes(context) < dedicatedBytes * 3 / 4, "physical workspace capacity did not decrease");
          }
        }
        const auto before = context.memorySnapshot();
        const auto address = context.deviceAddress(graph.head().buffer);
        const auto report = graph.workspaceReport();
        for (int repeat = 0; repeat < 3; ++repeat) {
          run();
          require(context.deviceAddress(graph.head().buffer) == address, "re-record relocated the head");
          require(graph.workspaceReport() == report, "re-record changed the slot mapping");
          stableMemory(before, context.memorySnapshot());
          require(context.download(graph.head().buffer, graph.head().validBytes()) == reference,
                  "repeated submission changed the head");
        }
        require(context.download(input.buffer, input.validBytes()) == inputBytes, "borrowed_input_is_preserved failed");
        if (traceEnabled) checkTrace(trace, graph.head());
        if (mode != 0 && !fallback) {
          // Reusing a Graph-owned slot as borrowed input would corrupt a live view.
          auto overlapping = input;
          overlapping.buffer = graph.head().buffer;
          overlapping.buffer.size = input.validBytes();
          auto commands = context.beginCommands();
          if (traceEnabled) trace.record(commands);
          rejects([&] { graph.record(commands, overlapping); }, "overlapping borrowed input was accepted");
          require(kernels.dispatchCount() == 0, "invalid overlap was rejected after dispatch");
          if (traceEnabled) require(trace.operations.empty(), "invalid overlap emitted GPU commands");
          context.endAndSubmit(commands, true);
        }
        auto* publicBuffer = graph.allocate("public allocation", 64, 4, nr::Format::F32);
        require(publicBuffer->buffer.buffer != graph.head().buffer.buffer, "public allocation reused internal storage");
        std::cout << "mode=" << mode << " dedicated_bytes=" << dedicatedBytes
                  << " graph_bytes=" << before.owners[static_cast<size_t>(vk::MemoryOwner::Graph)].liveBytes << '\n'
                  << report.substr(0, report.find('\n')) << '\n';
      }
      require(graphBytes(context) == 0, "Graph destruction did not release all physical storage");
    }
    require(vk::Context::validationErrors() == 0, "Vulkan validation reported errors");
    std::cout << "PASS borrowed input, exact head, poison, stable addresses, ownership and route\n";
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    vk::Context context;
    accounting(context);
    std::cout << "PASS Context, buffer and image accounting\n";
    if (!flag(argc, argv, "--accounting-only")) workspace<nr::Graph>(context, argc, argv);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
