#include "vk_context.h"
#include "compute_trace.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
bool flag(int argc, char** argv, const char* name) {
  return std::find(argv + 1, argv + argc, std::string(name)) != argv + argc;
}
uint32_t expectedMiB(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) if (std::string(argv[i]) == "--expect-mib") return std::stoul(argv[i + 1]);
  throw std::runtime_error("--expect-mib is required");
}
struct OwnedBuffer {
  vk::Context& context;
  vk::Buffer buffer;
  ~OwnedBuffer() { context.destroyBuffer(buffer); }
};
void checkCapacity(const vk::Context& context, uint32_t mib) {
  const auto memory = context.memorySnapshot();
  const auto staging = std::find_if(memory.records.begin(), memory.records.end(),
                                    [](const auto& record) { return record.label == "staging"; });
  require(staging != memory.records.end(), "staging allocation missing");
  require(staging->logicalBytes == (uint64_t(mib) << 20), "staging capacity differs from requested policy");
  require(staging->allocatedBytes >= staging->logicalBytes && staging->owner == vk::MemoryOwner::Context,
          "staging accounting or owner is incorrect");
  constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  require((staging->properties & flags) == flags, "staging lost coherent host visibility");
  std::cout << "staging_mib=" << mib << " allocation_bytes=" << staging->allocatedBytes << " heap=" << staging->heap << '\n';
}
void requireStable(const vk::MemorySnapshot& before, const vk::Context& context) {
  const auto after = context.memorySnapshot();
  require(before.liveBytes == after.liveBytes && before.allocationCount == after.allocationCount &&
          before.freeCount == after.freeCount, "transfers changed persistent GPU allocations");
}
void transfers(vk::Context& context) {
  constexpr size_t offset = 64, count = (32u << 20) + 4096, tail = 128;
  vk::CommandTrace trace;
  context.setCommandTrace(&trace);
  struct Detach { vk::Context& context; ~Detach() { context.setCommandTrace(nullptr); } } detach{context};
  OwnedBuffer target{context, context.createBuffer(offset + count + tail, false, "staging boundary test")};
  context.fillZero(target.buffer);
  std::vector<uint8_t> payload(count), expected(offset + count + tail, 0);
  for (size_t i = 0; i < count; ++i) payload[i] = uint8_t(i * 37u + (i >> 13) * 17u + (i >> 21) * 11u);
  std::copy(payload.begin(), payload.end(), expected.begin() + offset);
  const auto stable = context.memorySnapshot();
  context.upload(target.buffer, payload.data(), payload.size(), offset);
  require(context.download(target.buffer, target.buffer.size) == expected, "multi-chunk upload/download or offset corrupted bytes");
  const auto middle = context.download(target.buffer, (16u << 20) + 128, offset + (8u << 20));
  require(std::equal(middle.begin(), middle.end(), expected.begin() + offset + (8u << 20)),
          "partial multi-chunk download corrupted bytes");
  const auto& uploads = trace.buffers.at(trace.bufferIndex(target.buffer)).uploads;
  require(uploads.size() == 1 && uploads[0].offset == offset && uploads[0].bytes == payload,
          "trace no longer represents the original logical upload");
  context.setCommandTrace(nullptr);
  context.upload(target.buffer, payload.data(), 0, offset);
  require(context.download(target.buffer, 0, offset).empty(), "zero-length download returned data");
  for (int repeat = 0; repeat < 2; ++repeat) {
    std::fill(payload.begin(), payload.end(), uint8_t(0x5a + repeat));
    context.upload(target.buffer, payload.data(), payload.size(), offset);
    require(context.download(target.buffer, payload.size(), offset) == payload, "repeated transfer retained stale staging bytes");
  }
  requireStable(stable, context);
}
void benchmark(vk::Context& context, uint32_t mib) {
  using Clock = std::chrono::steady_clock;
  for (uint32_t sizeMiB : {4u, 64u, 256u}) {
    OwnedBuffer target{context, context.createBuffer(uint64_t(sizeMiB) << 20, false, "staging timing")};
    std::vector<uint8_t> bytes(target.buffer.size, 0x67);
    std::vector<double> uploadMs, downloadMs;
    const auto stable = context.memorySnapshot();
    for (int repeat = 0; repeat < 7; ++repeat) {
      const auto begin = Clock::now();
      context.upload(target.buffer, bytes.data(), bytes.size());
      const auto uploaded = Clock::now();
      const auto actual = context.download(target.buffer, bytes.size());
      const auto downloaded = Clock::now();
      require(actual == bytes, "timed transfer corrupted bytes");
      if (repeat >= 2) {
        uploadMs.push_back(std::chrono::duration<double, std::milli>(uploaded - begin).count());
        downloadMs.push_back(std::chrono::duration<double, std::milli>(downloaded - uploaded).count());
      }
    }
    requireStable(stable, context);
    std::sort(uploadMs.begin(), uploadMs.end()); std::sort(downloadMs.begin(), downloadMs.end());
    std::cout << "TRANSFER {\"staging_mib\":" << mib << ",\"payload_mib\":" << sizeMiB
              << ",\"upload_median_ms\":" << uploadMs[2] << ",\"download_median_ms\":" << downloadMs[2]
              << ",\"upload_p95_ms\":" << uploadMs.back() << ",\"download_p95_ms\":" << downloadMs.back() << "}\n";
  }
}
}

int main(int argc, char** argv) {
  try {
    if (flag(argc, argv, "--expect-config-error")) {
      bool rejected = false;
      try { vk::Context context; }
      catch (const std::runtime_error& error) { rejected = std::string(error.what()).find("DLSS5VK_STAGING_MIB") != std::string::npos; }
      require(rejected, "invalid staging configuration was not rejected");
      require(vkCreateInstance == nullptr, "invalid staging configuration initialized Vulkan");
      std::cout << "PASS invalid staging configuration rejected before Vulkan initialization\n";
      return 0;
    }
    const uint32_t mib = expectedMiB(argc, argv);
    vk::Context context;
    checkCapacity(context, mib);
    {
      vk::Context adopted(context.instance(), context.physical(), context.device(), context.queueFamily());
      checkCapacity(adopted, mib);
      transfers(adopted);
    }
    transfers(context);
    if (flag(argc, argv, "--benchmark")) benchmark(context, mib);
    require(vk::Context::validationErrors() == 0, "Vulkan validation reported errors");
    std::cout << "PASS configured staging, owned/adopted contexts, chunk boundaries, offsets, trace and stable allocations\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
