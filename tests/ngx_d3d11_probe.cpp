#include "ngx_parameters.h"
#include "numeric.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
void check(HRESULT result, const char* operation) {
  require(SUCCEEDED(result), std::string(operation) + " HRESULT=" + std::to_string(static_cast<unsigned>(result)));
}
void ngxCheck(NVSDK_NGX_Result result, const char* operation) {
  printf("%s: 0x%08x\n", operation, static_cast<unsigned>(result)); fflush(stdout);
  require(result == NVSDK_NGX_Result_Success, std::string(operation) + " failed");
}
std::string arg(int argc, char** argv, const char* key, const char* fallback = "") {
  for (int i = 1; i + 1 < argc; ++i) if (!strcmp(argv[i], key)) return argv[i + 1];
  return fallback;
}
bool flag(int argc, char** argv, const char* key) {
  for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], key)) return true;
  return false;
}
std::filesystem::path path(const std::string& text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }
struct Module {
  HMODULE value;
  explicit Module(const std::filesystem::path& dll) {
    require(dll.is_absolute(), "DLL path must be absolute");
    value = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    require(value != nullptr, "DLL load failed");
  }
  ~Module() { FreeLibrary(value); }
  template<class T> T get(const char* name) {
    auto address = GetProcAddress(value, name);
    require(address != nullptr, std::string("missing export ") + name);
    return reinterpret_cast<T>(address);
  }
};
}

int main(int argc, char** argv) try {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  const unsigned width = std::stoul(arg(argc, argv, "--width", "512"));
  const unsigned height = std::stoul(arg(argc, argv, "--height", "512"));
  const int frames = std::stoi(arg(argc, argv, "--frames", "3"));
  require(width >= 64 && height >= 64 && width <= 8192 && height <= 8192 && frames > 0, "invalid size/frames");
  Module module(path(arg(argc, argv, "--dll")));
  ComPtr<IDXGIFactory6> factory;
  check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, commands;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc{}; check(adapter->GetDesc1(&desc), "adapter description");
    if (desc.VendorId != 0x10de) continue;
    if (SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                    &device, nullptr, &immediate))) break;
  }
  require(device != nullptr, "no NVIDIA D3D11 device");
  const bool deferred = flag(argc, argv, "--deferred");
  if (deferred) check(device->CreateDeferredContext(0, &commands), "deferred context");
  else commands = immediate;
  auto submit = [&] {
    if (deferred) {
      ComPtr<ID3D11CommandList> list;
      check(commands->FinishCommandList(FALSE, &list), "FinishCommandList");
      immediate->ExecuteCommandList(list.Get(), TRUE);
    }
  };
  const auto init = module.get<decltype(&NVSDK_NGX_D3D11_Init_Ext)>("NVSDK_NGX_D3D11_Init_Ext");
  const auto create = module.get<decltype(&NVSDK_NGX_D3D11_CreateFeature)>("NVSDK_NGX_D3D11_CreateFeature");
  const auto evaluate = module.get<decltype(&NVSDK_NGX_D3D11_EvaluateFeature)>("NVSDK_NGX_D3D11_EvaluateFeature");
  const auto release = module.get<decltype(&NVSDK_NGX_D3D11_ReleaseFeature)>("NVSDK_NGX_D3D11_ReleaseFeature");
  const auto shutdown = module.get<decltype(&NVSDK_NGX_D3D11_Shutdown1)>("NVSDK_NGX_D3D11_Shutdown1");
  ngx_test::Parameters params;
  params.trace = flag(argc, argv, "--trace");
  params.Set("DLSSNR.Width", width); params.Set("DLSSNR.Height", height);
  params.Set("DLSSNR.ScalingRatio", 1.0f); params.Set("DLSSNR.Hint.Render.Preset", 0);
  ngxCheck(init(0, L".", device.Get(), NVSDK_NGX_Version_API, &params), "D3D11 Init_Ext");
  NVSDK_NGX_Handle* handle = nullptr;
  ngxCheck(create(commands.Get(), NVSDK_NGX_Feature_Reserved18, &params, &handle), "D3D11 CreateFeature");
  require(handle != nullptr, "null feature");
  submit();
  auto texture = [&](DXGI_FORMAT format, bool staging = false) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format; desc.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    desc.BindFlags = staging ? 0 : D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
    ComPtr<ID3D11Texture2D> result;
    check(device->CreateTexture2D(&desc, nullptr, &result), "CreateTexture2D");
    return result;
  };
  auto color = texture(DXGI_FORMAT_R16G16B16A16_FLOAT), output = texture(DXGI_FORMAT_R16G16B16A16_FLOAT);
  auto motion = texture(DXGI_FORMAT_R32G32_FLOAT), readback = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, true);
  std::vector<uint16_t> input(static_cast<size_t>(width) * height * 4), sentinel(input.size(), 0x7e00);
  std::vector<float> velocity(static_cast<size_t>(width) * height * 2);
  const float mvx = std::stof(arg(argc, argv, "--motion-x", "0")), mvy = std::stof(arg(argc, argv, "--motion-y", "0"));
  for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
    const size_t i = (static_cast<size_t>(y) * width + x) * 4;
    input[i] = num::f16Bits(0.125f + 0.5f * static_cast<float>(x) / static_cast<float>(width));
    input[i + 1] = num::f16Bits(0.25f + 0.375f * static_cast<float>(y) / static_cast<float>(height));
    input[i + 2] = num::f16Bits(((x / 16 + y / 16) & 1) ? 0.65f : 0.3f);
    input[i + 3] = num::f16Bits(1.0f);
    velocity[i / 2] = mvx; velocity[i / 2 + 1] = mvy;
  }
  params.Set("DLSSNR.Color", static_cast<ID3D11Resource*>(color.Get()));
  params.Set("DLSSNR.Output", static_cast<ID3D11Resource*>(output.Get()));
  params.Set("DLSSNR.MVec", static_cast<ID3D11Resource*>(motion.Get()));
  for (const char* name : {"Color", "Output", "MVec"}) {
    const std::string key = std::string("DLSSNR.") + name + "Subrect";
    params.Set((key + "BaseX").c_str(), 0u); params.Set((key + "BaseY").c_str(), 0u);
    params.Set((key + "Width").c_str(), width); params.Set((key + "Height").c_str(), height);
  }
  params.Set("DLSSNR.Enabled", flag(argc, argv, "--disabled") ? 0u : 1u);
  params.Set("DLSSNR.Intensity", std::stof(arg(argc, argv, "--intensity", "1")));
  params.Set("DLSSNR.LocalToneStrength", std::stof(arg(argc, argv, "--tone", "1")));
  params.Set("DLSSNR.LocalStructureStrength", std::stof(arg(argc, argv, "--structure", "1")));
  params.Set("DLSSNR.SkinStructureStrength", std::stof(arg(argc, argv, "--skin", "-1")));
  params.Set("DLSSNR.UseAutoMask", flag(argc, argv, "--no-auto-mask") ? 0u : 1u);
  params.Set("DLSSNR.Style", static_cast<unsigned>(std::stoul(arg(argc, argv, "--style", "0"))));
  params.Set("DLSSNR.UICorrection", 0u);
  params.Set("DLSSNR.MVecScaleX", 1.0f); params.Set("DLSSNR.MVecScaleY", 1.0f);
  const auto out = path(arg(argc, argv, "--out", "tmp/ngx/d3d11-probe"));
  const auto reference = path(arg(argc, argv, "--reference"));
  require(reference.empty() || std::filesystem::weakly_canonical(out) != std::filesystem::weakly_canonical(reference), "cannot overwrite reference");
  std::filesystem::create_directories(out);
  for (int frame = 0; frame < frames; ++frame) {
    commands->UpdateSubresource(color.Get(), 0, nullptr, input.data(), width * 8, 0);
    commands->UpdateSubresource(output.Get(), 0, nullptr, sentinel.data(), width * 8, 0);
    commands->UpdateSubresource(motion.Get(), 0, nullptr, velocity.data(), width * 8, 0);
    const int resetEvery = std::stoi(arg(argc, argv, "--reset-every", "0"));
    params.Set("DLSSNR.Reset", frame == 0 || (resetEvery > 0 && frame % resetEvery == 0) ? 1u : 0u);
    ngxCheck(evaluate(commands.Get(), handle, &params, nullptr), "D3D11 EvaluateFeature");
    commands->CopyResource(readback.Get(), output.Get());
    submit();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(immediate->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map output");
    std::vector<uint16_t> result(input.size());
    for (unsigned y = 0; y < height; ++y)
      memcpy(result.data() + static_cast<size_t>(y) * width * 4, static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch, width * 8);
    immediate->Unmap(readback.Get(), 0);
    size_t nonfinite = 0, changed = 0;
    for (size_t i = 0; i < result.size(); ++i) {
      nonfinite += !std::isfinite(num::f16ToF32(result[i])); changed += result[i] != input[i];
    }
    printf("frame=%d changed=%zu nonfinite=%zu\n", frame, changed, nonfinite);
    require(nonfinite == 0, "nonfinite or unwritten output");
    const auto filename = "frame-" + std::to_string(frame) + ".rgba16f";
    std::ofstream capture(out / filename, std::ios::binary);
    capture.write(reinterpret_cast<const char*>(result.data()), static_cast<std::streamsize>(result.size() * 2));
    require(bool(capture), "cannot write capture");
    if (!reference.empty()) {
      require(std::filesystem::file_size(reference / filename) == result.size() * 2, "reference size mismatch");
      std::vector<uint16_t> expected(result.size());
      std::ifstream file(reference / filename, std::ios::binary);
      file.read(reinterpret_cast<char*>(expected.data()), static_cast<std::streamsize>(expected.size() * 2));
      require(bool(file), "cannot read reference");
      size_t different = 0;
      for (size_t i = 0; i < result.size(); ++i) different += result[i] != expected[i];
      printf("comparison different=%zu/%zu\n", different, result.size());
      require(different == 0, "output differs from reference");
    }
  }
  ngxCheck(release(handle), "D3D11 ReleaseFeature");
  ngxCheck(shutdown(device.Get()), "D3D11 Shutdown1");
  puts(reference.empty() ? "PASS D3D11 ordered frame output (no reference)" : "PASS D3D11 output bit-exact vs reference");
  return 0;
} catch (const std::exception& e) { fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
