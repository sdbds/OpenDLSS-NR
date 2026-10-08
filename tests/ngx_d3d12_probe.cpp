#include "ngx_parameters.h"
#include "numeric.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
#include "ngx_nvapi_trace.h"

using Microsoft::WRL::ComPtr;

namespace {
void require(bool value, const std::string& text) {
  if (!value) { fprintf(stderr, "assertion: %s\n", text.c_str()); fflush(stdout); throw std::runtime_error(text); }
}
void check(HRESULT value, const char* operation) {
  if (FAILED(value)) throw std::runtime_error(std::string(operation) + " HRESULT=" + std::to_string(static_cast<unsigned>(value)));
}
void ngxCheck(NVSDK_NGX_Result result, const char* operation) {
  printf("%s: 0x%08x\n", operation, static_cast<unsigned>(result)); fflush(stdout);
  require(result == NVSDK_NGX_Result_Success, std::string(operation) + " failed");
}
void expectResult(NVSDK_NGX_Result value, NVSDK_NGX_Result expected, const char* operation) {
  printf("%s: 0x%08x expected=0x%08x\n", operation, static_cast<unsigned>(value), static_cast<unsigned>(expected));
  require(value == expected, std::string(operation) + " returned the wrong error");
}
std::string arg(int argc, char** argv, const char* key, const char* fallback = "") {
  for (int i = 1; i + 1 < argc; ++i) if (!strcmp(argv[i], key)) return argv[i + 1];
  return fallback;
}
bool flag(int argc, char** argv, const char* key) {
  for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], key)) return true;
  return false;
}
std::filesystem::path utf8Path(const std::string& path) { return std::filesystem::path(std::u8string(path.begin(), path.end())); }
struct Module {
  HMODULE value = nullptr;
  explicit Module(const std::filesystem::path& path) {
    require(path.is_absolute(), "--dll must be an absolute path");
    value = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    require(value != nullptr, "cannot load DLL (Win32 " + std::to_string(GetLastError()) + "): " + path.string());
  }
  ~Module() { if (value) FreeLibrary(value); }
  template<class T> T get(const char* name) const {
    FARPROC fn = GetProcAddress(value, name);
    require(fn != nullptr, std::string("missing export: ") + name);
    return reinterpret_cast<T>(fn);
  }
};
struct D3D {
  ComPtr<IDXGIAdapter3> adapter;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  UINT64 value = 0;
  D3D() {
    ComPtr<IDXGIFactory6> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    for (UINT i = 0;; ++i) {
      ComPtr<IDXGIAdapter1> candidate;
      if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 description{};
      check(candidate->GetDesc1(&description), "GetDesc1");
      if (description.VendorId != 0x10de || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
      if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
        check(candidate.As(&adapter), "adapter3");
        printf("adapter vendor=0x%04x device=0x%04x\n", description.VendorId, description.DeviceId);
        break;
      }
    }
    require(device != nullptr, "no NVIDIA D3D12 device");
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "CreateCommandList");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "CreateEventW failed");
  }
  ~D3D() { if (event) CloseHandle(event); }
  void submit() {
    check(list->Close(), "Close");
    ID3D12CommandList* commands[] = {list.Get()};
    queue->ExecuteCommandLists(1, commands);
    check(queue->Signal(fence.Get(), ++value), "Signal");
    check(fence->SetEventOnCompletion(value, event), "SetEventOnCompletion");
    require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "D3D12 GPU wait timed out");
    check(device->GetDeviceRemovedReason(), "device status");
    check(allocator->Reset(), "allocator Reset");
    check(list->Reset(allocator.Get(), nullptr), "list Reset");
  }
  ComPtr<ID3D12Resource> buffer(UINT64 bytes, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes; desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto state = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)), "Create buffer");
    return resource;
  }
  ComPtr<ID3D12Resource> texture(UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = width; desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1; desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)), "Create texture");
    return resource;
  }
  void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    list->ResourceBarrier(1, &barrier);
  }
};
void rect(ngx_test::Parameters& p, const char* name, unsigned w, unsigned h) {
  const std::string key = std::string("DLSSNR.") + name + "Subrect";
  p.Set((key + "BaseX").c_str(), 0u); p.Set((key + "BaseY").c_str(), 0u);
  p.Set((key + "Width").c_str(), w); p.Set((key + "Height").c_str(), h);
}
}  // namespace

int main(int argc, char** argv) try {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  const std::string path = arg(argc, argv, "--dll");
  require(!path.empty(), "usage: nvngx.dll-d3d12-probe.exe --dll <absolute path> [--trace] [--frames N] [--out dir]");
  const unsigned width = std::stoul(arg(argc, argv, "--width", "512"));
  const unsigned height = std::stoul(arg(argc, argv, "--height", "512"));
  const int frames = std::stoi(arg(argc, argv, "--frames", "3"));
  const bool benchmark = flag(argc, argv, "--benchmark");
  const int warmup = std::stoi(arg(argc, argv, "--warmup", "0"));
  require(width >= 64 && height >= 64 && width <= 8192 && height <= 8192 && frames > 0, "invalid dimensions or frame count");
  require(warmup >= 0 && warmup < frames, "invalid warmup count");
  Module module(utf8Path(path));
  if (flag(argc, argv, "--driver-trace")) ngx_test::driver_trace::install(module.value, "tmp/ngx/driver-modules");
  D3D gpu;
  const auto init = module.get<decltype(&NVSDK_NGX_D3D12_Init_Ext)>("NVSDK_NGX_D3D12_Init_Ext");
  const auto create = module.get<decltype(&NVSDK_NGX_D3D12_CreateFeature)>("NVSDK_NGX_D3D12_CreateFeature");
  const auto evaluate = module.get<decltype(&NVSDK_NGX_D3D12_EvaluateFeature)>("NVSDK_NGX_D3D12_EvaluateFeature");
  const auto release = module.get<decltype(&NVSDK_NGX_D3D12_ReleaseFeature)>("NVSDK_NGX_D3D12_ReleaseFeature");
  const auto shutdown = module.get<decltype(&NVSDK_NGX_D3D12_Shutdown1)>("NVSDK_NGX_D3D12_Shutdown1");
  ngx_test::Parameters params;
  params.trace = flag(argc, argv, "--trace");
  params.Set("DLSSNR.Width", width); params.Set("DLSSNR.Height", height);
  params.Set("DLSSNR.ScalingRatio", 1.0f);
  params.Set("DLSSNR.Hint.Render.Preset", 0);
  const bool abiChecks = flag(argc, argv, "--abi-checks");
  if (abiChecks) {
    NVSDK_NGX_Handle* invalid = reinterpret_cast<NVSDK_NGX_Handle*>(1);
    expectResult(create(gpu.list.Get(), NVSDK_NGX_Feature_Reserved18, &params, &invalid),
                 NVSDK_NGX_Result_FAIL_NotInitialized, "Create before Init");
    require(invalid == nullptr, "failed Create did not clear the output handle");
  }
  ngxCheck(init(0, L".", gpu.device.Get(), NVSDK_NGX_Version_API, &params), "D3D12 Init_Ext");
  if (abiChecks) {
    ngx_test::Parameters missing;
    NVSDK_NGX_Handle* invalid = nullptr;
    expectResult(create(gpu.list.Get(), NVSDK_NGX_Feature_Reserved18, &missing, &invalid),
                 NVSDK_NGX_Result_FAIL_MissingInput, "Create without dimensions");
  }
  if (flag(argc, argv, "--capabilities")) {
    using Populate = NVSDK_NGX_Result (NVSDK_CONV*)(NVSDK_NGX_Parameter*);
    const auto populate = module.get<Populate>("NVSDK_NGX_D3D12_PopulateParameters_Impl");
    ngxCheck(populate(&params), "D3D12 PopulateParameters_Impl");
  }
  NVSDK_NGX_Handle* feature = nullptr;
  ngxCheck(create(gpu.list.Get(), NVSDK_NGX_Feature_Reserved18, &params, &feature), "D3D12 CreateFeature");
  require(feature != nullptr, "success without a feature handle");
  struct Cleanup {
    NVSDK_NGX_Handle*& feature;
    ID3D12Device* device;
    decltype(release) release;
    decltype(shutdown) shutdown;
    ~Cleanup() { if (feature) { release(feature); shutdown(device); } }
  } cleanup{feature, gpu.device.Get(), release, shutdown};
  gpu.submit();
  DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
  check(gpu.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory), "query memory after create");
  printf("process_local_bytes_after_create=%llu\n", static_cast<unsigned long long>(memory.CurrentUsage));
  if (abiChecks) {
    const auto* foreign = reinterpret_cast<const NVSDK_NGX_Handle*>(1);
    expectResult(evaluate(gpu.list.Get(), foreign, &params, nullptr), NVSDK_NGX_Result_FAIL_FeatureNotFound, "foreign feature handle");
    expectResult(evaluate(gpu.list.Get(), feature, nullptr, nullptr), NVSDK_NGX_Result_FAIL_InvalidParameter, "null parameters");
    ngx_test::Parameters secondParams;
    secondParams.Set("DLSSNR.Width", 320u); secondParams.Set("DLSSNR.Height", 320u);
    NVSDK_NGX_Handle* second = nullptr;
    ngxCheck(create(gpu.list.Get(), NVSDK_NGX_Feature_Reserved18, &secondParams, &second), "second independent feature");
    require(second && second != feature, "duplicate feature handle");
    ngxCheck(release(second), "release second feature");
  }
  if (flag(argc, argv, "--capabilities")) {
    using Callback = NVSDK_NGX_Result (NVSDK_CONV*)(NVSDK_NGX_Parameter*);
    for (const char* name : {"DLSSNRGetStatsCallback", "DLSSNRComputeScalingRatioCallback"}) {
      params.Set("PerfQualityValue", 2u);
      void* callback = nullptr;
      ngxCheck(params.Get(name, &callback), name);
      require(callback != nullptr, "null capability callback");
      printf("%s result=0x%08x\n", name, static_cast<unsigned>(reinterpret_cast<Callback>(callback)(&params)));
    }
    unsigned long long bytes = 0;
    if (params.Get("SizeInBytes", &bytes) == NVSDK_NGX_Result_Success) printf("NGX stats bytes=%llu\n", bytes);
    float ratio = 0;
    if (params.Get("DLSSNR.ScalingRatio", &ratio) == NVSDK_NGX_Result_Success) printf("NGX scaling ratio=%g\n", ratio);
  }
  if (flag(argc, argv, "--create-only")) {
    ngxCheck(release(feature), "D3D12 ReleaseFeature");
    feature = nullptr;
    ngxCheck(shutdown(gpu.device.Get()), "D3D12 Shutdown1");
    puts("PASS lifecycle only; no frame evaluated");
    return 0;
  }
  auto color = gpu.texture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  auto output = gpu.texture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto motion = gpu.texture(width, height, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
  UINT64 totalBytes = 0;
  auto desc = color->GetDesc();
  gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &totalBytes);
  const UINT64 uploadStride = (totalBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                              ~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
  auto upload = gpu.buffer(uploadStride * 3, D3D12_HEAP_TYPE_UPLOAD);
  auto readback = gpu.buffer(totalBytes, D3D12_HEAP_TYPE_READBACK);
  uint8_t* uploadBytes = nullptr;
  D3D12_RANGE empty{};
  check(upload->Map(0, &empty, reinterpret_cast<void**>(&uploadBytes)), "upload Map");
  memset(uploadBytes, 0, static_cast<size_t>(uploadStride * 3));
  std::vector<uint16_t> input(static_cast<size_t>(width) * height * 4);
  const float inputAlpha = std::stof(arg(argc, argv, "--input-alpha", "1"));
  for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
    const size_t i = (static_cast<size_t>(y) * width + x) * 4;
    input[i] = num::f16Bits(0.125f + 0.5f * static_cast<float>(x) / static_cast<float>(width));
    input[i + 1] = num::f16Bits(0.25f + 0.375f * static_cast<float>(y) / static_cast<float>(height));
    input[i + 2] = num::f16Bits(((x / 16 + y / 16) & 1) ? 0.65f : 0.3f);
    input[i + 3] = num::f16Bits(inputAlpha);
  }
  const float mvx = std::stof(arg(argc, argv, "--motion-x", "0")), mvy = std::stof(arg(argc, argv, "--motion-y", "0"));
  const bool motionField = flag(argc, argv, "--motion-field");
  for (unsigned y = 0; y < height; ++y) {
    memcpy(uploadBytes + y * layout.Footprint.RowPitch, input.data() + static_cast<size_t>(y) * width * 4, static_cast<size_t>(width) * 8);
    auto* sentinel = reinterpret_cast<uint16_t*>(uploadBytes + uploadStride + y * layout.Footprint.RowPitch);
    std::fill(sentinel, sentinel + static_cast<size_t>(width) * 4, uint16_t(0x7e00));
    auto* velocity = reinterpret_cast<float*>(uploadBytes + uploadStride * 2 + y * layout.Footprint.RowPitch);
    for (unsigned x = 0; x < width; ++x) {
      velocity[x * 2] = mvx;
      velocity[x * 2 + 1] = mvy;
      if (motionField) {
        velocity[x * 2] += static_cast<float>(static_cast<int>((x / 13 + y / 17) % 7) - 3) * 0.25f;
        velocity[x * 2 + 1] += static_cast<float>(static_cast<int>((x / 11 + y / 19) % 5) - 2) * 0.375f;
      }
    }
  }
  upload->Unmap(0, nullptr);
  params.Set("DLSSNR.Color", color.Get()); params.Set("DLSSNR.Output", output.Get()); params.Set("DLSSNR.MVec", motion.Get());
  for (const char* name : {"Color", "Output", "MVec"}) rect(params, name, width, height);
  params.Set("DLSSNR.Enabled", flag(argc, argv, "--disabled") ? 0u : 1u);
  params.Set("DLSSNR.Intensity", std::stof(arg(argc, argv, "--intensity", "1")));
  params.Set("DLSSNR.LocalToneStrength", std::stof(arg(argc, argv, "--tone", "1")));
  params.Set("DLSSNR.LocalStructureStrength", std::stof(arg(argc, argv, "--structure", "1")));
  params.Set("DLSSNR.SkinStructureStrength", std::stof(arg(argc, argv, "--skin", "-1")));
  params.Set("DLSSNR.UseAutoMask", flag(argc, argv, "--no-auto-mask") ? 0u : 1u);
  params.Set("DLSSNR.Style", static_cast<unsigned>(std::stoul(arg(argc, argv, "--style", "0"))));
  params.Set("DLSSNR.UICorrection", 0u);
  params.Set("DLSSNR.MVecScaleX", 1.0f); params.Set("DLSSNR.MVecScaleY", 1.0f);
  if (abiChecks) {
    params.Set("DLSSNR.Style", 1u);
    expectResult(evaluate(gpu.list.Get(), feature, &params, nullptr), NVSDK_NGX_Result_FAIL_UnsupportedParameter, "unsupported style");
    params.Set("DLSSNR.Style", 0u);
    params.Set("DLSSNR.Output", color.Get());
    expectResult(evaluate(gpu.list.Get(), feature, &params, nullptr), NVSDK_NGX_Result_FAIL_UnsupportedParameter, "aliased output");
    params.Set("DLSSNR.Output", output.Get());
    params.Set("DLSSNR.Color", static_cast<ID3D12Resource*>(nullptr));
    expectResult(evaluate(gpu.list.Get(), feature, &params, nullptr), NVSDK_NGX_Result_FAIL_MissingInput, "missing color");
    params.Set("DLSSNR.Color", color.Get());
  }
  const auto outDir = utf8Path(arg(argc, argv, "--out", "tmp/ngx/d3d12-probe"));
  const auto reference = utf8Path(arg(argc, argv, "--reference"));
  require(!benchmark || reference.empty(), "benchmark and reference comparison must be separate runs");
  require(reference.empty() || std::filesystem::weakly_canonical(outDir) != std::filesystem::weakly_canonical(reference),
          "output must not overwrite the reference");
  std::filesystem::create_directories(outDir);
  std::vector<float> floats(input.size());
  std::transform(input.begin(), input.end(), floats.begin(), num::f16ToF32);
  if (!benchmark && flag(argc, argv, "--float-captures")) {
    std::ofstream proxyFile(outDir / "proxy.f32", std::ios::binary);
    proxyFile.write(reinterpret_cast<const char*>(floats.data()), static_cast<std::streamsize>(floats.size() * 4));
    require(bool(proxyFile), "cannot write proxy capture");
  }
  bool allFramesMatch = true;
  ComPtr<ID3D12QueryHeap> timestamps;
  D3D12_QUERY_HEAP_DESC queryDesc{}; queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; queryDesc.Count = 2;
  check(gpu.device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&timestamps)), "timestamp heap");
  auto timeReadback = gpu.buffer(16, D3D12_HEAP_TYPE_READBACK);
  UINT64 frequency = 0;
  check(gpu.queue->GetTimestampFrequency(&frequency), "timestamp frequency");
  require(frequency > 0, "invalid timestamp frequency");
  std::vector<double> milliseconds;
  std::vector<double> recordingMilliseconds;
  for (int frame = 0; frame < frames; ++frame) {
    if (flag(argc, argv, "--rotate-resources") && frame > 0) {
      color = gpu.texture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      output = gpu.texture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      motion = gpu.texture(width, height, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      params.Set("DLSSNR.Color", color.Get()); params.Set("DLSSNR.Output", output.Get()); params.Set("DLSSNR.MVec", motion.Get());
    }
    const bool frameEnabled = !flag(argc, argv, "--disabled") && frame != std::stoi(arg(argc, argv, "--disable-frame", "-1"));
    const float intensity = frame == std::stoi(arg(argc, argv, "--zero-intensity-frame", "-1")) ? 0.0f :
                            std::stof(arg(argc, argv, "--intensity", "1"));
    params.Set("DLSSNR.Enabled", frameEnabled ? 1 : 0);
    params.Set("DLSSNR.Intensity", intensity);
    ID3D12Resource* images[] = {color.Get(), output.Get(), motion.Get()};
    for (unsigned slot = 0; slot < 3; ++slot) {
      const auto before = slot == 1 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      gpu.transition(images[slot], before, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = images[slot]; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint = layout; src.PlacedFootprint.Offset = uploadStride * slot;
      if (slot == 2) src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32_FLOAT;
      gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      gpu.transition(images[slot], D3D12_RESOURCE_STATE_COPY_DEST, before);
    }
    const int resetEvery = std::stoi(arg(argc, argv, "--reset-every", "0"));
    params.Set("DLSSNR.Reset", frame == 0 || (resetEvery > 0 && frame % resetEvery == 0) ? 1u : 0u);
    gpu.list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    const auto started = std::chrono::steady_clock::now();
    const auto evaluated = evaluate(gpu.list.Get(), feature, &params, nullptr);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (frame >= warmup) recordingMilliseconds.push_back(std::chrono::duration<double, std::milli>(elapsed).count());
    ngxCheck(evaluated, "D3D12 EvaluateFeature");
    gpu.list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    gpu.list->ResolveQueryData(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, timeReadback.Get(), 0);
    const bool captureFrame = !benchmark || frame == 0 || frame == frames - 1;
    if (captureFrame) {
      gpu.transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = layout;
      D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = output.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      gpu.transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    gpu.submit();
    UINT64* ticks = nullptr;
    D3D12_RANGE timeRange{0, 16};
    check(timeReadback->Map(0, &timeRange, reinterpret_cast<void**>(&ticks)), "read timestamps");
    if (frame >= warmup) milliseconds.push_back(static_cast<double>(ticks[1] - ticks[0]) * 1000.0 / static_cast<double>(frequency));
    timeReadback->Unmap(0, &empty);
    if (!captureFrame) continue;
    const uint8_t* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(totalBytes)};
    check(readback->Map(0, &range, reinterpret_cast<void**>(const_cast<uint8_t**>(&mapped))), "readback Map");
    std::vector<uint16_t> result(input.size());
    for (unsigned y = 0; y < height; ++y) memcpy(result.data() + static_cast<size_t>(y) * width * 4, mapped + y * layout.Footprint.RowPitch, static_cast<size_t>(width) * 8);
    readback->Unmap(0, &empty);
    size_t finite = 0, changed = 0;
    for (size_t i = 0; i < result.size(); ++i) if ((i & 3) != 3) {
      finite += std::isfinite(num::f16ToF32(result[i])); changed += result[i] != input[i];
    }
    printf("frame=%d finite_rgb=%zu changed_rgb=%zu\n", frame, finite, changed);
    require(finite == static_cast<size_t>(width) * height * 3, "nonfinite or unwritten output");
    if (!frameEnabled) require(result == input, "disabled path did not preserve the input RGBA bits");
    if (frameEnabled && intensity > 0)
      require(changed > 0, "enabled NR returned the input unchanged");
    if (!reference.empty()) {
      const auto referencePath = reference / ("frame-" + std::to_string(frame) + ".rgba16f");
      require(std::filesystem::file_size(referencePath) == result.size() * 2, "reference size mismatch");
      std::vector<uint16_t> expected(result.size());
      std::ifstream expectedFile(referencePath, std::ios::binary);
      expectedFile.read(reinterpret_cast<char*>(expected.data()), static_cast<std::streamsize>(expected.size() * 2));
      require(bool(expectedFile), "cannot read reference");
      size_t different = 0, first = result.size();
      float maxError = 0.0f;
      for (size_t i = 0; i < result.size(); ++i) if (result[i] != expected[i]) {
        ++different; first = std::min(first, i);
        maxError = std::max(maxError, std::abs(num::f16ToF32(result[i]) - num::f16ToF32(expected[i])));
      }
      printf("comparison different=%zu/%zu max_error=%.9g first=%zu\n", different, result.size(), maxError, first);
      allFramesMatch = allFramesMatch && different == 0;
    }
    if (benchmark && frame != frames - 1) continue;
    std::ofstream file(outDir / ("frame-" + std::to_string(frame) + ".rgba16f"), std::ios::binary);
    file.write(reinterpret_cast<const char*>(result.data()), static_cast<std::streamsize>(result.size() * 2));
    require(bool(file), "cannot write output capture");
    if (flag(argc, argv, "--float-captures")) {
      std::transform(result.begin(), result.end(), floats.begin(), num::f16ToF32);
      std::ofstream floatFile(outDir / ("frame-" + std::to_string(frame) + ".rgba32f"), std::ios::binary);
      floatFile.write(reinterpret_cast<const char*>(floats.data()), static_cast<std::streamsize>(floats.size() * 4));
      require(bool(floatFile), "cannot write float output capture");
    }
  }
  check(gpu.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory), "query memory after evaluation");
  printf("process_local_bytes_after_evaluation=%llu\n", static_cast<unsigned long long>(memory.CurrentUsage));
  auto* released = feature;
  ngxCheck(release(feature), "D3D12 ReleaseFeature");
  feature = nullptr;
  if (abiChecks) {
    expectResult(release(released), NVSDK_NGX_Result_FAIL_FeatureNotFound, "double release");
    expectResult(evaluate(gpu.list.Get(), released, &params, nullptr), NVSDK_NGX_Result_FAIL_FeatureNotFound, "evaluate released feature");
  }
  ngxCheck(shutdown(gpu.device.Get()), "D3D12 Shutdown1");
  std::sort(milliseconds.begin(), milliseconds.end());
  double total = 0;
  for (double ms : milliseconds) total += ms;
  printf("gpu_ms samples=%zu median=%.6f mean=%.6f min=%.6f max=%.6f\n", milliseconds.size(),
         milliseconds[milliseconds.size() / 2], total / static_cast<double>(milliseconds.size()), milliseconds.front(), milliseconds.back());
  std::sort(recordingMilliseconds.begin(), recordingMilliseconds.end());
  printf("cpu_record_ms samples=%zu median=%.6f\n", recordingMilliseconds.size(), recordingMilliseconds[recordingMilliseconds.size() / 2]);
  require(allFramesMatch, "one or more frames differ from reference (all captures retained)");
  puts(reference.empty() ? "PASS D3D12 ordered frame output (no reference comparison)" : "PASS D3D12 output bit-exact vs reference");
  return 0;
} catch (const std::exception& error) {
  fprintf(stderr, "FAIL: %s\n", error.what());
  return 1;
}
