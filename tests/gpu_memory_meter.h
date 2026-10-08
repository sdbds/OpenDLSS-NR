#pragma once
#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "vk_context.h"

struct GpuMemorySample {
  uint64_t local = 0, shared = 0, budget = 0;
};

class GpuMemoryMeter {
 public:
  explicit GpuMemoryMeter(VkPhysicalDevice physical) {
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; properties.pNext = &id;
    vkGetPhysicalDeviceProperties2(physical, &properties);
    if (!id.deviceLUIDValid) throw std::runtime_error("Vulkan device LUID unavailable");
    LUID luid; memcpy(&luid, id.deviceLUID, sizeof(luid));
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    check(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter_)), "EnumAdapterByLuid");
    printf("ADAPTER luid=%08lX%08lX name=%s vendor=%u device=%u driver=%u\n",
           static_cast<unsigned long>(luid.HighPart), luid.LowPart, properties.properties.deviceName,
           properties.properties.vendorID, properties.properties.deviceID, properties.properties.driverVersion);
  }

  GpuMemorySample sample(const char* phase) const {
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
    check(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local), "Query local memory");
    check(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared), "Query nonlocal memory");
    printf("MEM {\"phase\":\"%s\",\"pid\":%lu,\"local_bytes\":%llu,\"shared_bytes\":%llu,\"budget_bytes\":%llu}\n",
           phase, GetCurrentProcessId(), local.CurrentUsage, shared.CurrentUsage, local.Budget);
    return {local.CurrentUsage, shared.CurrentUsage, local.Budget};
  }

  void canary(vk::Context& context) const {
    constexpr uint64_t size = 64ull << 20;
    const auto before = sample("canary_before");
    auto buffer = context.createBuffer(size, false, "memory-meter 64MiB canary");
    context.fillZero(buffer);
    Sleep(50);
    const auto allocated = sample("canary_allocated");
    context.destroyBuffer(buffer); context.waitIdle(); Sleep(50);
    const auto freed = sample("canary_freed");
    if (allocated.local < before.local + size - (1ull << 20) || allocated.local < freed.local + size - (1ull << 20))
      throw std::runtime_error("DXGI metric did not observe the known 64MiB Vulkan allocation and release");
    puts("CANARY PASS: current-process local memory observes Vulkan allocations");
  }

 private:
  static void check(HRESULT result, const char* operation) {
    if (FAILED(result)) { char message[192]; sprintf_s(message, "%s failed: 0x%08X", operation, unsigned(result)); throw std::runtime_error(message); }
  }
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_;
};
