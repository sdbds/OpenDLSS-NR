// Compact auxiliary storage: exact source bytes, bounds and stable GPU addresses.
#include "nr_model.h"

#include <algorithm>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template<class F>
void requireThrows(F operation, const char* message) {
  bool threw = false;
  try { operation(); } catch (const std::runtime_error&) { threw = true; }
  require(threw, message);
}
std::string value(int argc, char** argv, const std::string& key, const std::string& fallback) {
  for (int i = 1; i + 1 < argc; ++i) if (key == argv[i]) return argv[i + 1];
  return fallback;
}
bool flag(int argc, char** argv, const std::string& key) {
  for (int i = 1; i < argc; ++i) if (key == argv[i]) return true;
  return false;
}

PFN_vkAllocateMemory allocateMemory;
uint64_t allocationCalls = 0;
VKAPI_ATTR VkResult VKAPI_CALL countAllocate(VkDevice device, const VkMemoryAllocateInfo* info,
                                             const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory) {
  ++allocationCalls;
  return allocateMemory(device, info, callbacks, memory);
}
struct ObserveAllocations {
  ObserveAllocations() { allocateMemory = vkAllocateMemory; vkAllocateMemory = countAllocate; }
  ~ObserveAllocations() { vkAllocateMemory = allocateMemory; }
};
}  // namespace

int main(int argc, char** argv) {
  try {
    vk::Context context;
    ObserveAllocations observer;
    nr::Model model(context, value(argc, argv, "--model", "models/nr"), !flag(argc, argv, "--no-verify"));
    require(allocationCalls == 0, "loading CPU model records uploaded unused GPU data");

    const nr::Tensor& pre = model.tensor(0);
    const std::vector<uint8_t> original(pre.bytes, pre.bytes + pre.byteLength);
    requireThrows([&] { pre.auxBuffer(); }, "unprepared aux buffer was exposed");
    model.prepareAux(pre, {{9232, 64}, {20576, 4}, {21616, 64}});
    require(allocationCalls == 1, "one aux tensor must use one allocation");
    require(pre.auxBuffer().size == 144, "block 0 aux storage retained packed matrix gaps");
    require(pre.auxOffset(9232, 64) == 0 && pre.auxOffset(20576, 4) == 64 &&
                pre.auxOffset(21616, 64) == 80 && pre.auxOffset(21632, 16) == 96,
            "incorrect compact or interior aux offset");
    std::vector<uint8_t> expected(144, 0);
    std::copy_n(original.data() + 9232, 64, expected.data());
    std::copy_n(original.data() + 20576, 4, expected.data() + 64);
    std::copy_n(original.data() + 21616, 64, expected.data() + 80);
    require(context.download(pre.auxBuffer(), pre.auxBuffer().size) == expected,
            "GPU aux bytes or alignment padding changed");
    require(std::equal(original.begin(), original.end(), pre.bytes), "CPU packed model data changed");

    const VkBuffer buffer = pre.auxBuffer().buffer;
    const VkDeviceAddress address = context.deviceAddress(pre.auxBuffer());
    model.prepareAux(pre, {{21616, 64}, {9232, 64}, {20576, 4}});
    require(allocationCalls == 1 && pre.auxBuffer().buffer == buffer &&
                context.deviceAddress(pre.auxBuffer()) == address,
            "re-preparing aux data relocated recorded GPU addresses");
    requireThrows([&] { model.prepareAux(pre, {{9232, 64}}); }, "cached aux layout was replaced");
    requireThrows([&] { pre.auxOffset(9295, 2); }, "aux read crossed a declared range");
    requireThrows([&] { pre.auxOffset(20580, 4); }, "aux read entered an undeclared packed gap");
    requireThrows([&] { pre.auxOffset(9232, 0); }, "empty aux read was accepted");
    nr::Tensor foreign;
    foreign.name = pre.name; foreign.bytes = pre.bytes; foreign.byteLength = pre.byteLength;
    requireThrows([&] { model.prepareAux(foreign, {{9232, 64}}); }, "foreign tensor was accepted by the cache");

    const nr::Tensor& weightsOnly = model.tensor(31, 0);
    requireThrows([&] { model.prepareAux(weightsOnly, {}); }, "empty aux plan was accepted");
    requireThrows([&] { model.prepareAux(weightsOnly, {{0, 0}}); }, "zero-length aux range was accepted");
    requireThrows([&] { model.prepareAux(weightsOnly, {{0, 32}, {16, 32}}); }, "overlapping aux ranges were accepted");
    requireThrows([&] { model.prepareAux(weightsOnly, {{UINT32_MAX, 16}}); }, "overflowing aux range was accepted");
    requireThrows([&] { model.prepareAux(weightsOnly, {{weightsOnly.byteLength - 2, 4}}); },
                  "aux plan exceeded the CPU tensor");
    requireThrows([&] { weightsOnly.auxBuffer(); }, "invalid aux plans allocated GPU storage");
    require(allocationCalls == 1, "rejected aux requests allocated GPU memory");

    const nr::Tensor& qkv = model.tensor(31, 2);
    model.prepareAux(qkv, {{0, 128}});
    require(qkv.auxBuffer().size == 128 && qkv.auxOffset(0, 128) == 0,
            "leading ViT scale range was not preserved");
    require(context.download(qkv.auxBuffer(), 128) == std::vector<uint8_t>(qkv.bytes, qkv.bytes + 128),
            "leading ViT scales differ from CPU bytes");
    require(allocationCalls == 2, "unexpected auxiliary allocation count");
    require(vk::Context::validationErrors() == 0, "Vulkan validation reported errors");
    std::cout << "PASS compact aux bytes, bounds and immutable addresses\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
