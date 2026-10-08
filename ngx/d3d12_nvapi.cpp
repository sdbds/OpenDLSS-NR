#include "d3d12_nvapi.h"
#include <nvapi_interface.h>

#include <cstring>
#include <limits>
#include <stdexcept>

namespace ngx {

template<class T> T NvApi::get(const char* name) {
  for (const auto& entry : nvapi_interface_table) {
    if (strcmp(entry.func, name)) continue;
    void* address = query_(entry.id);
    if (address) return reinterpret_cast<T>(address);
    break;
  }
  throw std::runtime_error(std::string("NVIDIA driver does not expose ") + name);
}

void NvApi::check(NvAPI_Status status, const char* operation) const {
  if (status == NVAPI_OK) return;
  NvAPI_ShortString message{};
  if (error_) error_(status, message);
  throw std::runtime_error(std::string(operation) + ": " + std::to_string(status) + " " + message);
}

NvApi::NvApi() {
  library_ = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!library_) throw std::runtime_error("cannot load the installed nvapi64.dll");
  try {
    query_ = reinterpret_cast<Query>(GetProcAddress(library_, "nvapi_QueryInterface"));
    if (!query_) throw std::runtime_error("nvapi_QueryInterface is unavailable");
    error_ = get<decltype(error_)>("NvAPI_GetErrorMessage");
    unload_ = get<decltype(unload_)>("NvAPI_Unload");
    check(get<decltype(&NvAPI_Initialize)>("NvAPI_Initialize")(), "NvAPI_Initialize");
    initialized_ = true;
    module_ = get<decltype(module_)>("NvAPI_D3D12_CreateCuModule");
    function_ = get<decltype(function_)>("NvAPI_D3D12_CreateCuFunction");
    destroyModule_ = get<decltype(destroyModule_)>("NvAPI_D3D12_DestroyCuModule");
    destroyFunction_ = get<decltype(destroyFunction_)>("NvAPI_D3D12_DestroyCuFunction");
    launch_ = get<decltype(launch_)>("NvAPI_D3D12_LaunchCuKernelChainEx");
    texture_ = get<decltype(texture_)>("NvAPI_D3D12_GetCudaMergedTextureSamplerObject");
    surface_ = get<decltype(surface_)>("NvAPI_D3D12_GetCudaIndependentDescriptorObject");
  } catch (...) {
    if (initialized_) unload_();
    FreeLibrary(library_);
    throw;
  }
}

NvApi::~NvApi() {
  if (initialized_) unload_();
  if (library_) FreeLibrary(library_);
}

NVDX_ObjectHandle NvApi::createModule(ID3D12Device* device, const std::string& ptx) const {
  if (ptx.size() >= std::numeric_limits<NvU32>::max()) throw std::runtime_error("PTX module is too large");
  NVDX_ObjectHandle result = nullptr;
  check(module_(device, ptx.c_str(), static_cast<NvU32>(ptx.size() + 1), &result), "NvAPI_D3D12_CreateCuModule");
  return result;
}

NVDX_ObjectHandle NvApi::createFunction(ID3D12Device* device, NVDX_ObjectHandle module, const char* name) const {
  NVDX_ObjectHandle result = nullptr;
  check(function_(device, module, name, &result), "NvAPI_D3D12_CreateCuFunction");
  return result;
}

void NvApi::destroyFunction(ID3D12Device* device, NVDX_ObjectHandle function) const {
  if (function) destroyFunction_(device, function);
}
void NvApi::destroyModule(ID3D12Device* device, NVDX_ObjectHandle module) const {
  if (module) destroyModule_(device, module);
}

void NvApi::launch(ID3D12GraphicsCommandList* commands, NVDX_ObjectHandle function, NVAPI_DIM3 grid,
                   NVAPI_DIM3 block, uint32_t sharedBytes, void** arguments) const {
  NVAPI_CU_KERNEL_LAUNCH_PARAMS_EX kernel{};
  kernel.hFunction = function; kernel.gridDim = grid; kernel.blockDim = block;
  kernel.dynSharedMemBytes = sharedBytes; kernel.kernelParams = arguments;
  check(launch_(commands, &kernel, 1), "NvAPI_D3D12_LaunchCuKernelChainEx");
}

void NvApi::launchChain(ID3D12GraphicsCommandList* commands, const NVAPI_CU_KERNEL_LAUNCH_PARAMS_EX* kernels, uint32_t count) const {
  check(launch_(commands, kernels, count), "NvAPI_D3D12_LaunchCuKernelChainEx");
}

uint64_t NvApi::texture(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE view, D3D12_CPU_DESCRIPTOR_HANDLE sampler) const {
  NVAPI_D3D12_GET_CUDA_MERGED_TEXTURE_SAMPLER_OBJECT_PARAMS params{};
  params.structSizeIn = params.structSizeOut = sizeof(params);
  params.pDevice = device; params.texDesc = view; params.smpDesc = sampler;
  check(texture_(&params), "NvAPI_D3D12_GetCudaMergedTextureSamplerObject");
  return params.textureHandle;
}

uint64_t NvApi::surface(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE view) const {
  NVAPI_D3D12_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_PARAMS params{};
  params.structSizeIn = params.structSizeOut = sizeof(params);
  params.pDevice = device; params.desc = view; params.type = NVAPI_D3D12_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_SURFACE;
  check(surface_(&params), "NvAPI_D3D12_GetCudaIndependentDescriptorObject");
  return params.handle;
}

}  // namespace ngx
