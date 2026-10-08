// Real feature-DLL contract probe. A successful Init/Create alone is not parity.
#include "ngx_parameters.h"
#include "numeric.h"

#include <windows.h>
#include <cuda.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}
void cudaCheck(CUresult result, const char* operation) {
  if (result == CUDA_SUCCESS) return;
  const char* message = nullptr;
  cuGetErrorString(result, &message);
  throw std::runtime_error(std::string(operation) + ": " + (message ? message : "CUDA error"));
}
#define CU_CHECK(call) cudaCheck(call, #call)
void ngxCheck(NVSDK_NGX_Result result, const char* operation) {
  printf("%s: 0x%08x\n", operation, static_cast<unsigned>(result));
  fflush(stdout);
  require(result == NVSDK_NGX_Result_Success, std::string(operation) + " failed");
}
std::string argument(int argc, char** argv, const char* key, const char* fallback) {
  for (int i = 1; i + 1 < argc; ++i) if (!strcmp(argv[i], key)) return argv[i + 1];
  return fallback;
}
bool flag(int argc, char** argv, const char* name) {
  for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], name)) return true;
  return false;
}
std::filesystem::path utf8Path(const std::string& value) {
  return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
struct Module {
  HMODULE handle = nullptr;
  explicit Module(const std::filesystem::path& path) {
    require(path.is_absolute(), "DLL path must be absolute");
    handle = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    require(handle != nullptr, "cannot load DLL: " + path.string() + " (Win32 " + std::to_string(GetLastError()) + ")");
  }
  ~Module() { if (handle) FreeLibrary(handle); }
  template<class T> T get(const char* name) {
    auto address = GetProcAddress(handle, name);
    require(address != nullptr, std::string("missing export: ") + name);
    return reinterpret_cast<T>(address);
  }
};
struct Cuda {
  CUdevice device = 0;
  CUcontext context = nullptr;
  CUstream stream = nullptr;
  Cuda() {
    CU_CHECK(cuInit(0));
    CU_CHECK(cuDeviceGet(&device, 0));
    CU_CHECK(cuDevicePrimaryCtxRetain(&context, device));
    CU_CHECK(cuCtxPushCurrent(context));
    CU_CHECK(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING));
    char name[256]{};
    CU_CHECK(cuDeviceGetName(name, sizeof(name), device));
    printf("device=%s\n", name);
  }
  ~Cuda() {
    if (stream) { cuStreamSynchronize(stream); cuStreamDestroy(stream); }
    if (context) { CUcontext previous = nullptr; cuCtxPopCurrent(&previous); cuDevicePrimaryCtxRelease(device); }
  }
};
struct Array {
  CUarray value = nullptr;
  size_t rowBytes = 0, height = 0;
  Array(size_t width, size_t h, CUarray_format format, unsigned channels, size_t scalarBytes) : rowBytes(width * channels * scalarBytes), height(h) {
    CUDA_ARRAY3D_DESCRIPTOR descriptor{};
    descriptor.Width = width; descriptor.Height = h; descriptor.Format = format;
    descriptor.NumChannels = channels; descriptor.Flags = CUDA_ARRAY3D_SURFACE_LDST;
    CU_CHECK(cuArray3DCreate(&value, &descriptor));
  }
  ~Array() { if (value) cuArrayDestroy(value); }
  void upload(const void* data) {
    CUDA_MEMCPY2D copy{};
    copy.srcMemoryType = CU_MEMORYTYPE_HOST; copy.srcHost = data; copy.srcPitch = rowBytes;
    copy.dstMemoryType = CU_MEMORYTYPE_ARRAY; copy.dstArray = value;
    copy.WidthInBytes = rowBytes; copy.Height = height;
    CU_CHECK(cuMemcpy2D(&copy));
  }
  std::vector<uint16_t> read() const {
    std::vector<uint16_t> bytes(rowBytes * height / 2);
    CUDA_MEMCPY2D copy{};
    copy.srcMemoryType = CU_MEMORYTYPE_ARRAY; copy.srcArray = value;
    copy.dstMemoryType = CU_MEMORYTYPE_HOST; copy.dstHost = bytes.data(); copy.dstPitch = rowBytes;
    copy.WidthInBytes = rowBytes; copy.Height = height;
    CU_CHECK(cuMemcpy2D(&copy));
    return bytes;
  }
};
void subrect(ngx_test::Parameters& params, const char* name, unsigned width, unsigned height) {
  const std::string base = std::string("DLSSNR.") + name + "Subrect";
  params.Set((base + "BaseX").c_str(), 0u); params.Set((base + "BaseY").c_str(), 0u);
  params.Set((base + "Width").c_str(), width); params.Set((base + "Height").c_str(), height);
}
}  // namespace

int main(int argc, char** argv) try {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  const std::string dll = argument(argc, argv, "--dll", "");
  require(!dll.empty(), "usage: nvngx.dll-probe.exe --dll <absolute path> [--trace] [--create-only] [--out <dir>]");
  const unsigned width = std::stoul(argument(argc, argv, "--width", "512"));
  const unsigned height = std::stoul(argument(argc, argv, "--height", "512"));
  const int frames = std::stoi(argument(argc, argv, "--frames", "3"));
  require(width >= 64 && height >= 64 && width <= 8192 && height <= 8192 && frames > 0, "invalid dimensions or frame count");
  Module module(utf8Path(dll));
  Cuda cuda;
  const auto init = module.get<decltype(&NVSDK_NGX_CUDA_Init_Ext1)>("NVSDK_NGX_CUDA_Init_Ext1");
  const auto create = module.get<decltype(&NVSDK_NGX_CUDA_CreateFeature1)>("NVSDK_NGX_CUDA_CreateFeature1");
  const auto evaluate = module.get<decltype(&NVSDK_NGX_CUDA_EvaluateFeature)>("NVSDK_NGX_CUDA_EvaluateFeature");
  const auto release = module.get<decltype(&NVSDK_NGX_CUDA_ReleaseFeature)>("NVSDK_NGX_CUDA_ReleaseFeature");
  const auto shutdown = module.get<decltype(&NVSDK_NGX_CUDA_Shutdown1)>("NVSDK_NGX_CUDA_Shutdown1");
  NVSDK_NGX_CUDADevice device{cuda.context, cuda.stream};
  ngx_test::Parameters params;
  params.trace = flag(argc, argv, "--trace");
  params.Set("Width", width); params.Set("Height", height);
  params.Set("DLSSNR.Width", width); params.Set("DLSSNR.Height", height);
  params.Set("DLSSNR.ScalingRatio", 1.0f);
  params.Set("DLSSNR.Hint.Render.Preset", 0u);
  ngxCheck(init(0, L".", &device, NVSDK_NGX_Version_API, &params), "CUDA Init_Ext1");
  NVSDK_NGX_Handle* feature = nullptr;
  ngxCheck(create(&device, NVSDK_NGX_Feature_Reserved18, &params, &feature), "CUDA CreateFeature1");
  require(feature != nullptr, "success without a feature handle");
  if (flag(argc, argv, "--create-only")) {
    ngxCheck(release(feature), "CUDA ReleaseFeature");
    ngxCheck(shutdown(&device), "CUDA Shutdown1");
    puts("PASS lifecycle only; no frame evaluated");
    return 0;
  }

  Array input(width, height, CU_AD_FORMAT_HALF, 4, 2);
  Array output(width, height, CU_AD_FORMAT_HALF, 4, 2);
  Array motion(width, height, CU_AD_FORMAT_FLOAT, 2, 4);
  std::vector<uint16_t> pixels(static_cast<size_t>(width) * height * 4);
  for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
    const size_t i = (static_cast<size_t>(y) * width + x) * 4;
    pixels[i] = num::f16Bits(0.125f + 0.5f * static_cast<float>(x) / static_cast<float>(width));
    pixels[i + 1] = num::f16Bits(0.25f + 0.375f * static_cast<float>(y) / static_cast<float>(height));
    pixels[i + 2] = num::f16Bits(((x / 16 + y / 16) & 1) ? 0.65f : 0.3f);
    pixels[i + 3] = num::f16Bits(1.0f);
  }
  input.upload(pixels.data());
  std::vector<uint16_t> sentinel(pixels.size(), 0x7e00);
  output.upload(sentinel.data());
  std::vector<float> mv(static_cast<size_t>(width) * height * 2, 0.0f);
  motion.upload(mv.data());
  params.Set("DLSSNR.Color", static_cast<void*>(input.value));
  params.Set("DLSSNR.Output", static_cast<void*>(output.value));
  params.Set("DLSSNR.MVec", static_cast<void*>(motion.value));
  for (const char* name : {"Color", "Output", "MVec"}) subrect(params, name, width, height);
  params.Set("DLSSNR.Enabled", flag(argc, argv, "--disabled") ? 0u : 1u);
  params.Set("DLSSNR.Intensity", 1.0f);
  params.Set("DLSSNR.LocalToneStrength", 1.0f);
  params.Set("DLSSNR.LocalStructureStrength", 1.0f);
  params.Set("DLSSNR.SkinStructureStrength", -1.0f);
  params.Set("DLSSNR.UseAutoMask", 1u);
  params.Set("DLSSNR.Style", 0u);
  params.Set("DLSSNR.UICorrection", 0u);
  params.Set("DLSSNR.MVecScaleX", 1.0f); params.Set("DLSSNR.MVecScaleY", 1.0f);
  const auto outDir = utf8Path(argument(argc, argv, "--out", "tmp/ngx/probe"));
  std::filesystem::create_directories(outDir);
  for (int frame = 0; frame < frames; ++frame) {
    params.Set("DLSSNR.Reset", frame == 0 ? 1u : 0u);
    ngxCheck(evaluate(feature, &params, nullptr), "CUDA EvaluateFeature");
    CU_CHECK(cuStreamSynchronize(cuda.stream));
    const auto result = output.read();
    size_t finite = 0, changed = 0;
    for (size_t i = 0; i < result.size(); ++i) {
      if ((i & 3) == 3) continue;
      finite += std::isfinite(num::f16ToF32(result[i]));
      changed += result[i] != pixels[i];
    }
    printf("frame=%d finite_rgb=%zu changed_rgb=%zu\n", frame, finite, changed);
    require(finite == static_cast<size_t>(width) * height * 3, "nonfinite or unwritten output");
    if (!flag(argc, argv, "--disabled")) require(changed > 0, "enabled NR returned the input unchanged");
    std::ofstream file(outDir / ("frame-" + std::to_string(frame) + ".rgba16f"), std::ios::binary);
    file.write(reinterpret_cast<const char*>(result.data()), static_cast<std::streamsize>(result.size() * 2));
    require(bool(file), "cannot write frame capture");
  }
  ngxCheck(release(feature), "CUDA ReleaseFeature");
  ngxCheck(shutdown(&device), "CUDA Shutdown1");
  puts("PASS CUDA frames evaluated and read back; no cross-implementation comparison yet");
  return 0;
} catch (const std::exception& error) {
  fprintf(stderr, "FAIL: %s\n", error.what());
  return 1;
}
