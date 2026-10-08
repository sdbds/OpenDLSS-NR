// Fixed-input implementation parity and allocation regression, using real GPU work.
// References are captured before an optimization, not shipped model/native fixtures.
#include "nr_graph.h"
#include "numeric.h"
#include "reference.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

namespace {
using Clock = std::chrono::steady_clock;

// Observe the actual Vulkan allocations without changing production interfaces.
PFN_vkAllocateMemory allocateMemory;
PFN_vkFreeMemory freeMemory;
struct Allocation { VkDeviceSize bytes; uint32_t heap; };
struct AllocationStats {
  uint64_t allocateCalls = 0, freeCalls = 0;
  VkDeviceSize liveBytes = 0, peakBytes = 0;
};
std::map<VkDeviceMemory, Allocation> liveMemory;
AllocationStats memoryStats, heapStats[VK_MAX_MEMORY_HEAPS];
VkPhysicalDeviceMemoryProperties memoryProperties{};
VkDeviceSize freedBytes = 0;
VKAPI_ATTR VkResult VKAPI_CALL trackAllocate(VkDevice device, const VkMemoryAllocateInfo* info,
                                             const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory) {
  const uint32_t heap = memoryProperties.memoryTypes[info->memoryTypeIndex].heapIndex;
  ++memoryStats.allocateCalls;
  ++heapStats[heap].allocateCalls;
  VkResult result = allocateMemory(device, info, callbacks, memory);
  if (result == VK_SUCCESS) {
    liveMemory[*memory] = {info->allocationSize, heap};
    for (AllocationStats* stats : {&memoryStats, &heapStats[heap]}) {
      stats->liveBytes += info->allocationSize;
      stats->peakBytes = std::max(stats->peakBytes, stats->liveBytes);
    }
  }
  return result;
}
VKAPI_ATTR void VKAPI_CALL trackFree(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* callbacks) {
  ++memoryStats.freeCalls;
  auto it = liveMemory.find(memory);
  if (it != liveMemory.end()) {
    const Allocation allocation = it->second;
    freedBytes += allocation.bytes;
    memoryStats.liveBytes -= allocation.bytes;
    heapStats[allocation.heap].liveBytes -= allocation.bytes;
    ++heapStats[allocation.heap].freeCalls;
    liveMemory.erase(it);
  }
  freeMemory(device, memory, callbacks);
}

std::string value(int argc, char** argv, const std::string& key, const std::string& fallback = "") {
  for (int i = 1; i + 1 < argc; ++i) if (key == argv[i]) return argv[i + 1];
  return fallback;
}
bool flag(int argc, char** argv, const std::string& key) {
  for (int i = 1; i < argc; ++i) if (key == argv[i]) return true;
  return false;
}
void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
// The same runner must still record F32 references against pre-option Graph revisions.
template<class Options>
void selectHeadFormat(Options& options, bool halfHead) {
  if constexpr (requires { options.fp16Head = halfHead; }) options.fp16Head = halfHead;
  else require(!halfHead, "FP16 head is unavailable on this baseline revision");
}
template<class Options>
void selectWorkspace(Options& options, bool reuse) {
  if constexpr (requires { options.reuseWorkspace = reuse; }) options.reuseWorkspace = reuse;
  else require(!reuse, "workspace reuse is unavailable on this baseline revision");
}
void requireStableMemory(const AllocationStats& before, const std::string& phase) {
  require(memoryStats.allocateCalls == before.allocateCalls, phase + " called vkAllocateMemory");
  require(memoryStats.freeCalls == before.freeCalls, phase + " called vkFreeMemory");
  require(memoryStats.liveBytes == before.liveBytes && memoryStats.peakBytes == before.peakBytes,
          phase + " changed tracked GPU memory bytes");
}
template<class Context>
void reportMemory(const char* phase, const Context& context) {
  if constexpr (requires { context.memorySnapshot(); }) std::cout << context.memorySnapshot().report(phase);
  std::cout << "tracked_memory phase=" << phase << " scope=post-context"
            << " allocate_calls=" << memoryStats.allocateCalls << " free_calls=" << memoryStats.freeCalls
            << " live_allocations=" << liveMemory.size() << " live_bytes=" << memoryStats.liveBytes
            << " peak_bytes=" << memoryStats.peakBytes << '\n';
  for (uint32_t heap = 0; heap < memoryProperties.memoryHeapCount; ++heap) {
    const AllocationStats& stats = heapStats[heap];
    if (!stats.allocateCalls) continue;
    std::cout << "tracked_heap phase=" << phase << " heap=" << heap
              << " flags=" << memoryProperties.memoryHeaps[heap].flags
              << " allocate_calls=" << stats.allocateCalls << " free_calls=" << stats.freeCalls
              << " live_bytes=" << stats.liveBytes << " peak_bytes=" << stats.peakBytes << '\n';
  }
}
double elapsed(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void checkChain(nr::Kernels& kernels) {
  require(kernels.chainTimeouts().waits == 0, "a chained GPU wait timed out");
}
std::vector<uint8_t> headAsF32(vk::Context& context, const nr::Activation& head) {
  auto bytes = context.download(head.buffer, head.validBytes());
  if (head.format == nr::Format::F32) return bytes;
  require(head.format == nr::Format::F16, "unsupported head storage format");
  const size_t count = (size_t)head.rows * head.channels;
  std::vector<uint8_t> expanded(count * sizeof(float));
  for (size_t i = 0; i < count; ++i) {
    uint16_t half; memcpy(&half, bytes.data() + i * sizeof(half), sizeof(half));
    const float value = num::f16ToF32(half);
    memcpy(expanded.data() + i * sizeof(value), &value, sizeof(value));
  }
  return expanded;
}
void compareOrWrite(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, bool record) {
  if (record) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    require(bool(out), "cannot write " + path.string());
    return;
  }
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  require(bool(in), "cannot read reference " + path.string());
  require(in.tellg() == std::streamoff(bytes.size()), "reference size mismatch: " + path.string());
  in.seekg(0);
  std::vector<uint8_t> reference(bytes.size());
  in.read(reinterpret_cast<char*>(reference.data()), reference.size());
  require(bool(in), "short read: " + path.string());
  auto difference = std::mismatch(bytes.begin(), bytes.end(), reference.begin());
  require(difference.first == bytes.end(), "output differs at byte " +
      std::to_string(difference.first - bytes.begin()) + ": " + path.string());
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string modelDir = value(argc, argv, "--model", "models/nr");
    const std::string shaderDir = value(argc, argv, "--shaders", "build/shaders");
    const std::filesystem::path referenceDir = value(argc, argv, "--reference");
    require(!referenceDir.empty(), "--reference <directory> is required");
    const bool record = flag(argc, argv, "--record");
    const bool half = flag(argc, argv, "--fp16");
    const bool halfHead = flag(argc, argv, "--fp16-head");
    require(!record || !halfHead, "head references must use the default F32 output");
    const uint32_t width = std::stoul(value(argc, argv, "--width", "512"));
    const uint32_t height = std::stoul(value(argc, argv, "--height", "512"));
    const int frames = std::stoi(value(argc, argv, "--frames", "3"));
    const int warmup = std::stoi(value(argc, argv, "--warmup", "3"));
    const uint64_t minimumSaved = std::stoull(value(argc, argv, "--minimum-saved", "0"));
    const uint64_t minimumResidentSaved = std::stoull(value(argc, argv, "--minimum-resident-saved", "0"));
    require(width && height && frames > 0 && warmup >= 0, "invalid size or frame count");
    if (record) std::filesystem::create_directories(referenceDir);

    vk::Context context;
    reportMemory("context-initialized", context);
    // Context initialization (including staging) precedes these hooks and is not counted.
    vkGetPhysicalDeviceMemoryProperties(context.physical(), &memoryProperties);
    allocateMemory = vkAllocateMemory; freeMemory = vkFreeMemory;
    vkAllocateMemory = trackAllocate; vkFreeMemory = trackFree;
    nr::Model model(context, modelDir, !flag(argc, argv, "--no-verify"));
    nr::Kernels kernels(context, shaderDir);
    kernels.setSiluTable(ref::siluTable());
    const nr::Geometry geometry = nr::Geometry::fromValid(width, height);
    const uint32_t rows = geometry.fullWidth * geometry.fullHeight;
    const bool intermediates = flag(argc, argv, "--intermediates");
    const bool capture = intermediates || flag(argc, argv, "--boundaries");
    uint64_t graphBytes = 0, residentBytes = 0;
    std::vector<double> times;
    const auto initStart = Clock::now();
    double initMs = 0;
    {
      nr::Graph::Options options{.captureBoundaries = capture, .captureIntermediates = intermediates,
                                 .fusedBlocks = !flag(argc, argv, "--unfused")};
      selectHeadFormat(options, halfHead);
      selectWorkspace(options, flag(argc, argv, "--reuse-workspace"));
      nr::Graph graph(context, model, kernels, geometry, options);
      nr::Activation* input = graph.allocate("input features", rows, 16, half ? nr::Format::F16 : nr::Format::F32);
      std::vector<float> features((size_t)rows * 16);
      // Deliberately not rounded: FP16 storage must perform the original consumer's conversion.
      for (size_t i = 0; i < features.size(); ++i) features[i] = std::sin(float(i) * 0.0017f) * 0.125f;
      features[0] = -0.0f;
      features[1] = std::ldexp(1.0f, -24);
      features[2] = 1.00048828125f;  // halfway between two binary16 values
      if (half) {
        std::vector<uint16_t> packed(features.size());
        std::transform(features.begin(), features.end(), packed.begin(), num::f16Bits);
        context.upload(input->buffer, packed.data(), packed.size() * 2);
      } else {
        context.upload(input->buffer, features.data(), features.size() * 4);
      }
      auto run = [&] {
        context.resetDescriptorPool();
        VkCommandBuffer commands = context.beginCommands();
        graph.record(commands, *input);
        context.endAndSubmit(commands, true);
        checkChain(kernels);
      };
      run();
      initMs = elapsed(initStart);
      require(graph.head().format == (halfHead ? nr::Format::F16 : nr::Format::F32), "incorrect head storage format");
      require(graph.head().channels == 4 && graph.head().rows == rows &&
                  graph.head().validBytes() == (VkDeviceSize)rows * 4 * (halfHead ? 2 : 4),
              "incorrect head storage shape");
      const AllocationStats beforeWarmup = memoryStats;
      reportMemory("before-warmup", context);
      const auto firstHead = headAsF32(context, graph.head());
      compareOrWrite(referenceDir / "head.bin", firstHead, record);
      for (const auto& [name, activation] : graph.boundaries()) {
        std::string filename = name;
        std::replace(filename.begin(), filename.end(), '/', '_');
        compareOrWrite(referenceDir / (filename + ".bin"),
                       context.download(activation->buffer, activation->validBytes()), record);
      }
      for (int i = 0; i < warmup; ++i) run();
      requireStableMemory(beforeWarmup, "warmup/re-recording");
      const AllocationStats beforeMeasured = memoryStats;
      reportMemory("after-warmup", context);
      VkQueryPool queries = context.createTimestampPool(2);
      for (int i = 0; i < frames; ++i) {
        context.resetDescriptorPool();
        VkCommandBuffer commands = context.beginCommands();
        vkCmdResetQueryPool(commands, queries, 0, 2);
        vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
        graph.record(commands, *input);
        vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
        context.endAndSubmit(commands, true);
        checkChain(kernels);
        const auto stamps = context.readTimestampsMs(queries, 2);
        times.push_back(stamps[1] - stamps[0]);
      }
      vkDestroyQueryPool(context.device(), queries, nullptr);
      require(headAsF32(context, graph.head()) == firstHead,
              "head changed on repeated submission");
      requireStableMemory(beforeMeasured, "measured re-recording");
      reportMemory("after-measured", context);
      // Only Graph-owned allocations are freed at this scope boundary. Model/kernel caches stay live.
      freedBytes = 0;
    }
    graphBytes = freedBytes;
    residentBytes = memoryStats.liveBytes;  // model/kernel resources, not Graph buffers
    reportMemory("after-graph", context);
    if (record) {
      std::ofstream out(referenceDir / "allocation-bytes.txt");
      out << graphBytes << '\n';
      require(bool(out), "cannot write allocation baseline");
      std::ofstream resident(referenceDir / "resident-allocation-bytes.txt");
      resident << residentBytes << '\n';
      require(bool(resident), "cannot write resident allocation baseline");
    } else {
      uint64_t baselineBytes = 0;
      std::ifstream in(referenceDir / "allocation-bytes.txt");
      require(bool(in >> baselineBytes), "cannot read allocation baseline");
      require(graphBytes <= baselineBytes && baselineBytes - graphBytes >= minimumSaved,
              "allocation saving below target: baseline=" + std::to_string(baselineBytes) +
              ", actual=" + std::to_string(graphBytes) + ", required saving=" + std::to_string(minimumSaved));
      std::cout << "saved_bytes=" << baselineBytes - graphBytes << '\n';
      const auto residentPath = referenceDir / "resident-allocation-bytes.txt";
      if (std::filesystem::exists(residentPath) || minimumResidentSaved) {
        uint64_t baselineResident = 0;
        std::ifstream resident(residentPath);
        require(bool(resident >> baselineResident), "cannot read resident allocation baseline");
        require(residentBytes <= baselineResident && baselineResident - residentBytes >= minimumResidentSaved,
                "resident allocation saving below target: baseline=" + std::to_string(baselineResident) +
                ", actual=" + std::to_string(residentBytes) + ", required saving=" + std::to_string(minimumResidentSaved));
        std::cout << "resident_saved_bytes=" << baselineResident - residentBytes << '\n';
      } else {
        std::cout << "resident_savings_check=SKIP (reference metadata missing)\n";
      }
    }
    std::sort(times.begin(), times.end());
    const size_t p95 = (times.size() * 95 + 99) / 100 - 1;
    std::cout << "device=" << context.deviceName() << " size=" << width << 'x' << height
              << " full=" << geometry.fullWidth << 'x' << geometry.fullHeight
              << " input=" << (half ? "f16" : "f32") << " graph_allocation_bytes=" << graphBytes
              << " head=" << (halfHead ? "f16" : "f32")
              << " resident_allocation_bytes=" << residentBytes
              << " dispatches=" << kernels.dispatchCount()
              << " init_ms=" << initMs << " median_ms=" << times[times.size() / 2]
              << " p95_ms=" << times[p95] << " frames=" << frames << '\n';
    require(vk::Context::validationErrors() == 0, "Vulkan validation reported errors");
    std::cout << (record ? "RECORDED" : "PASS (bit-exact)") << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
