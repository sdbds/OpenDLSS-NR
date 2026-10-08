#include "d3d12_feature.h"
#include "ngx_error.h"

#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

struct NVSDK_NGX_Handle { unsigned Id; };

namespace {
HMODULE self = nullptr;
struct Runtime {
  std::mutex mutex;
  std::map<ID3D12Device*, Microsoft::WRL::ComPtr<ID3D12Device>> devices;
  std::map<const NVSDK_NGX_Handle*, std::unique_ptr<ngx::Dx12Feature>> features;
  std::vector<std::unique_ptr<NVSDK_NGX_Handle>> handles;
  unsigned nextId = 1;
};
Runtime& runtime() {
  // ReleaseFeature/Shutdown own GPU teardown. Static destruction would run it
  // inside the Windows loader lock when a caller unloads a live feature.
  static Runtime* value = new Runtime;
  return *value;
}

std::filesystem::path dataRoot() {
  wchar_t environment[32768]{};
  const DWORD length = GetEnvironmentVariableW(L"OPEN_DLSS_NR_ROOT", environment, 32768);
  if (length && length < 32768) {
    const std::filesystem::path path(environment);
    ngx::require(path.is_absolute(), NVSDK_NGX_Result_FAIL_InvalidParameter, "OPEN_DLSS_NR_ROOT must be absolute");
    return path;
  }
  wchar_t name[32768]{};
  const DWORD size = GetModuleFileNameW(self, name, 32768);
  ngx::require(size && size < 32768, NVSDK_NGX_Result_FAIL_PlatformError, "cannot locate this DLL");
  const auto directory = std::filesystem::path(name).parent_path();
  for (const auto& candidate : {directory / "opendlss-nr", directory, directory.parent_path().parent_path()})
    if (std::filesystem::is_regular_file(candidate / "models/nr/manifest.json")) return candidate;
  throw ngx::Error(NVSDK_NGX_Result_FAIL_UnableToInitializeFeature, "model/assets missing; set OPEN_DLSS_NR_ROOT or install the data directory");
}
template<class F> NVSDK_NGX_Result guarded(F fn) {
  try {
    const std::lock_guard lock(runtime().mutex);
    return fn();
  } catch (const ngx::Error& error) {
    fprintf(stderr, "[OpenDLSS-NR] %s\n", error.what());
    return error.code;
  } catch (const std::bad_alloc&) {
    return NVSDK_NGX_Result_FAIL_OutOfGPUMemory;
  } catch (const std::exception& error) {
    fprintf(stderr, "[OpenDLSS-NR] %s\n", error.what());
    return NVSDK_NGX_Result_FAIL_PlatformError;
  } catch (...) { return NVSDK_NGX_Result_Fail; }
}
}

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) { self = module; DisableThreadLibraryCalls(module); }
  return TRUE;
}

extern "C" NVSDK_NGX_Result NVSDK_CONV OpenDlssUnsupported() {
  return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Init_Ext(unsigned long long, const wchar_t*, ID3D12Device* device,
                                                    NVSDK_NGX_Version version, const NVSDK_NGX_Parameter*) {
  return guarded([&] {
    ngx::require(device != nullptr, NVSDK_NGX_Result_FAIL_InvalidParameter, "missing D3D12 device");
    ngx::require(static_cast<unsigned>(version) >= 0x13, NVSDK_NGX_Result_FAIL_OutOfDate, "NGX ABI older than 1.3");
    runtime().devices[device] = device;
    return NVSDK_NGX_Result_Success;
  });
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList* commands,
    NVSDK_NGX_Feature feature, const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output) {
  if (output) *output = nullptr;
  return guarded([&] {
    ngx::require(commands && parameters && output, NVSDK_NGX_Result_FAIL_InvalidParameter, "invalid CreateFeature arguments");
    ngx::require(feature == NVSDK_NGX_Feature_Reserved18, NVSDK_NGX_Result_FAIL_FeatureNotSupported, "only NR feature 18 is supported");
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    ngx::require(SUCCEEDED(commands->GetDevice(IID_PPV_ARGS(&device))), NVSDK_NGX_Result_FAIL_PlatformError, "command-list device unavailable");
    ngx::require(runtime().devices.count(device.Get()) != 0, NVSDK_NGX_Result_FAIL_NotInitialized, "D3D12 device was not initialized");
    const unsigned width = ngx::required<unsigned>(parameters, "DLSSNR.Width");
    const unsigned height = ngx::required<unsigned>(parameters, "DLSSNR.Height");
    ngx::require(width >= 64 && height >= 64 && width <= 8192 && height <= 8192 && uint64_t(width) * height <= 16777216,
                 NVSDK_NGX_Result_FAIL_UnsupportedParameter, "unsupported dimensions");
    ngx::require(ngx::optional<float>(parameters, "DLSSNR.ScalingRatio", 1.0f) == 1.0f,
                 NVSDK_NGX_Result_FAIL_UnsupportedParameter, "scaling ratio must be 1");
    ngx::require(ngx::optional<int>(parameters, "DLSSNR.Hint.Render.Preset", 0) == 0,
                 NVSDK_NGX_Result_FAIL_UnsupportedParameter, "only the default render preset is implemented");
    ngx::require(device->GetNodeCount() == 1 && ngx::optional<unsigned>(parameters, "CreationNodeMask", 1) <= 1 &&
                 ngx::optional<unsigned>(parameters, "VisibilityNodeMask", 1) <= 1,
                 NVSDK_NGX_Result_FAIL_UnsupportedParameter, "multi-node devices are not implemented");
    ngx::require(!ngx::optional<void*>(parameters, "ResourceAllocCallback", nullptr) &&
                 !ngx::optional<void*>(parameters, "ResourceReleaseCallback", nullptr),
                 NVSDK_NGX_Result_FAIL_UnsupportedParameter, "custom resource allocators are not implemented");
    ngx::require(runtime().nextId != 0, NVSDK_NGX_Result_FAIL_OutOfGPUMemory, "feature identifiers exhausted");
    auto implementation = std::make_unique<ngx::Dx12Feature>(device.Get(), width, height, dataRoot());
    auto handle = std::make_unique<NVSDK_NGX_Handle>();
    handle->Id = runtime().nextId++;
    NVSDK_NGX_Handle* address = handle.get();
    runtime().handles.push_back(std::move(handle));
    runtime().features.emplace(address, std::move(implementation));
    *output = address;
    return NVSDK_NGX_Result_Success;
  });
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList* commands,
    const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback) {
  return guarded([&] {
    ngx::require(handle != nullptr, NVSDK_NGX_Result_FAIL_InvalidParameter, "missing feature handle");
    auto entry = runtime().features.find(handle);
    ngx::require(entry != runtime().features.end(), NVSDK_NGX_Result_FAIL_FeatureNotFound, "unknown or released feature");
    entry->second->evaluate(commands, parameters);
    return NVSDK_NGX_Result_Success;
  });
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle* handle) {
  return guarded([&] {
    ngx::require(handle != nullptr, NVSDK_NGX_Result_FAIL_InvalidParameter, "missing feature handle");
    ngx::require(runtime().features.erase(handle) != 0, NVSDK_NGX_Result_FAIL_FeatureNotFound, "unknown or released feature");
    return NVSDK_NGX_Result_Success;
  });
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Shutdown1(ID3D12Device* device) {
  return guarded([&] {
    auto& state = runtime();
    for (auto it = state.features.begin(); it != state.features.end();) {
      if (!device || it->second->device() == device) it = state.features.erase(it); else ++it;
    }
    if (device) state.devices.erase(device); else state.devices.clear();
    return NVSDK_NGX_Result_Success;
  });
}
extern "C" NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Shutdown() { return NVSDK_NGX_D3D12_Shutdown1(nullptr); }

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_GetScratchBufferSize(NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter*, size_t* bytes) {
  if (!bytes) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  *bytes = 0;
  return feature == NVSDK_NGX_Feature_Reserved18 ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

namespace {
NVSDK_NGX_Result NVSDK_CONV stats(NVSDK_NGX_Parameter* parameters) {
  return guarded([&] {
    ngx::require(parameters != nullptr, NVSDK_NGX_Result_FAIL_InvalidParameter, "missing stats parameters");
    ngx::require(!runtime().features.empty(), NVSDK_NGX_Result_FAIL_FeatureNotFound, "no live feature for stats");
    unsigned long long bytes = 0;
    for (const auto& feature : runtime().features) bytes += feature.second->allocatedBytes();
    parameters->Set("SizeInBytes", bytes);
    parameters->Set("#D", 40); parameters->Set("#E", 0);
    return NVSDK_NGX_Result_Success;
  });
}
NVSDK_NGX_Result NVSDK_CONV scalingRatio(NVSDK_NGX_Parameter* parameters) {
  return guarded([&] {
    unsigned quality = 0;
    ngx::require(parameters && parameters->Get("PerfQualityValue", &quality) == NVSDK_NGX_Result_Success,
                 NVSDK_NGX_Result_FAIL_InvalidParameter, "missing PerfQualityValue");
    ngx::require(quality <= 5 && quality != 3, NVSDK_NGX_Result_FAIL_UnsupportedParameter, "unsupported quality mode");
    parameters->Set("DLSSNR.ScalingRatio", 1.0f);
    return NVSDK_NGX_Result_Success;
  });
}
}
extern "C" NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_PopulateParameters_Impl(NVSDK_NGX_Parameter* parameters) {
  return guarded([&] {
    ngx::require(parameters != nullptr, NVSDK_NGX_Result_FAIL_InvalidParameter, "missing capability parameters");
    parameters->Set("DLSSNRGetStatsCallback", reinterpret_cast<void*>(&stats));
    parameters->Set("DLSSNRComputeScalingRatioCallback", reinterpret_cast<void*>(&scalingRatio));
    return NVSDK_NGX_Result_Success;
  });
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_GetFeatureRequirements(IDXGIAdapter* adapter,
    const NVSDK_NGX_FeatureDiscoveryInfo* discovery, NVSDK_NGX_FeatureRequirement* result) {
  if (!adapter || !discovery || !result) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (discovery->FeatureID != NVSDK_NGX_Feature_Reserved18) return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
  DXGI_ADAPTER_DESC desc{};
  if (FAILED(adapter->GetDesc(&desc))) return NVSDK_NGX_Result_FAIL_PlatformError;
  if (desc.VendorId != 0x10de) return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
  *result = {};
  result->MinHWArchitecture = 0x190;
  strcpy_s(result->MinOSVersion, "10.0.19041.0");
  return NVSDK_NGX_Result_Success;
}

extern "C" unsigned NVSDK_CONV NVSDK_NGX_GetAPIVersion() { return 0x13; }
extern "C" unsigned NVSDK_CONV NVSDK_NGX_GetApplicationId() { return 0x0e658703; }
extern "C" unsigned NVSDK_CONV NVSDK_NGX_GetGPUArchitecture() { return 0x190; }
extern "C" unsigned NVSDK_CONV NVSDK_NGX_GetSnippetVersion() { return 0x01360800; }
extern "C" NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_GetDriverVersionEx(unsigned* components, unsigned capacity, unsigned* count) {
  if ((!components || !capacity) && !count) return NVSDK_NGX_Result_FAIL_InvalidParameter;
  if (count) *count = 2;
  if (components && capacity) components[0] = 610;
  if (components && capacity > 1) components[1] = 88;
  return NVSDK_NGX_Result_Success;
}
