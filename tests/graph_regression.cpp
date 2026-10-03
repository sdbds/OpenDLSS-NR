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
std::map<VkDeviceMemory, VkDeviceSize> liveMemory;
VkDeviceSize freedBytes = 0;
VKAPI_ATTR VkResult VKAPI_CALL trackAllocate(VkDevice device, const VkMemoryAllocateInfo* info,
                                             const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory) {
  VkResult result = allocateMemory(device, info, callbacks, memory);
  if (result == VK_SUCCESS) liveMemory[*memory] = info->allocationSize;
  return result;
}
VKAPI_ATTR void VKAPI_CALL trackFree(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* callbacks) {
  auto it = liveMemory.find(memory);
  if (it != liveMemory.end()) { freedBytes += it->second; liveMemory.erase(it); }
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
double elapsed(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void checkChain(nr::Kernels& kernels) {
  require(kernels.chainTimeouts().waits == 0, "a chained GPU wait timed out");
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
    const uint32_t width = std::stoul(value(argc, argv, "--width", "512"));
    const uint32_t height = std::stoul(value(argc, argv, "--height", "512"));
    const int frames = std::stoi(value(argc, argv, "--frames", "3"));
    const int warmup = std::stoi(value(argc, argv, "--warmup", "3"));
    const uint64_t minimumSaved = std::stoull(value(argc, argv, "--minimum-saved", "0"));
    require(width && height && frames > 0 && warmup >= 0, "invalid size or frame count");
    if (record) std::filesystem::create_directories(referenceDir);

    vk::Context context;
    allocateMemory = vkAllocateMemory; freeMemory = vkFreeMemory;
    vkAllocateMemory = trackAllocate; vkFreeMemory = trackFree;
    nr::Model model(context, modelDir, !flag(argc, argv, "--no-verify"));
    nr::Kernels kernels(context, shaderDir);
    kernels.setSiluTable(ref::siluTable());
    const nr::Geometry geometry = nr::Geometry::fromValid(width, height);
    const uint32_t rows = geometry.fullWidth * geometry.fullHeight;
    const bool intermediates = flag(argc, argv, "--intermediates");
    const bool capture = intermediates || flag(argc, argv, "--boundaries");
    uint64_t graphBytes = 0;
    std::vector<double> times;
    const auto initStart = Clock::now();
    double initMs = 0;
    {
      nr::Graph graph(context, model, kernels, geometry,
                      {.captureBoundaries = capture, .captureIntermediates = intermediates,
                       .fusedBlocks = !flag(argc, argv, "--unfused")});
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
      const size_t allocationCount = liveMemory.size();
      const auto firstHead = context.download(graph.head().buffer, graph.head().validBytes());
      compareOrWrite(referenceDir / "head.bin", firstHead, record);
      for (const auto& [name, activation] : graph.boundaries()) {
        std::string filename = name;
        std::replace(filename.begin(), filename.end(), '/', '_');
        compareOrWrite(referenceDir / (filename + ".bin"),
                       context.download(activation->buffer, activation->validBytes()), record);
      }
      for (int i = 0; i < warmup; ++i) run();
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
      require(context.download(graph.head().buffer, graph.head().validBytes()) == firstHead,
              "head changed on repeated submission");
      require(liveMemory.size() == allocationCount, "re-recording allocated new GPU memory");
      // Only Graph-owned allocations are freed at this scope boundary. Model/kernel caches stay live.
      freedBytes = 0;
    }
    graphBytes = freedBytes;
    if (record) {
      std::ofstream out(referenceDir / "allocation-bytes.txt");
      out << graphBytes << '\n';
      require(bool(out), "cannot write allocation baseline");
    } else {
      uint64_t baselineBytes = 0;
      std::ifstream in(referenceDir / "allocation-bytes.txt");
      require(bool(in >> baselineBytes), "cannot read allocation baseline");
      require(graphBytes <= baselineBytes && baselineBytes - graphBytes >= minimumSaved,
              "allocation saving below target: baseline=" + std::to_string(baselineBytes) +
              ", actual=" + std::to_string(graphBytes) + ", required saving=" + std::to_string(minimumSaved));
      std::cout << "saved_bytes=" << baselineBytes - graphBytes << '\n';
    }
    std::sort(times.begin(), times.end());
    const size_t p95 = (times.size() * 95 + 99) / 100 - 1;
    std::cout << "device=" << context.deviceName() << " size=" << width << 'x' << height
              << " full=" << geometry.fullWidth << 'x' << geometry.fullHeight
              << " input=" << (half ? "f16" : "f32") << " graph_allocation_bytes=" << graphBytes
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
