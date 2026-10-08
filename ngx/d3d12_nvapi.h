#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <nvapi.h>
#include <cstdint>
#include <string>

namespace ngx {

// NVIDIA's command-list kernel interface keeps GPU work in the caller's queue.
class NvApi {
 public:
  NvApi();
  ~NvApi();
  NvApi(const NvApi&) = delete;
  NvApi& operator=(const NvApi&) = delete;

  NVDX_ObjectHandle createModule(ID3D12Device* device, const std::string& ptx) const;
  NVDX_ObjectHandle createFunction(ID3D12Device* device, NVDX_ObjectHandle module, const char* name) const;
  void destroyFunction(ID3D12Device* device, NVDX_ObjectHandle function) const;
  void destroyModule(ID3D12Device* device, NVDX_ObjectHandle module) const;
  void launch(ID3D12GraphicsCommandList* commands, NVDX_ObjectHandle function, NVAPI_DIM3 grid,
              NVAPI_DIM3 block, uint32_t sharedBytes, void** arguments) const;
  void launchChain(ID3D12GraphicsCommandList* commands, const NVAPI_CU_KERNEL_LAUNCH_PARAMS_EX* kernels, uint32_t count) const;
  uint64_t texture(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE view, D3D12_CPU_DESCRIPTOR_HANDLE sampler) const;
  uint64_t surface(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE view) const;

 private:
  HMODULE library_ = nullptr;
  using Query = void* (__cdecl*)(unsigned int);
  Query query_ = nullptr;
  decltype(&NvAPI_GetErrorMessage) error_ = nullptr;
  decltype(&NvAPI_Unload) unload_ = nullptr;
  decltype(&NvAPI_D3D12_CreateCuModule) module_ = nullptr;
  decltype(&NvAPI_D3D12_CreateCuFunction) function_ = nullptr;
  decltype(&NvAPI_D3D12_DestroyCuModule) destroyModule_ = nullptr;
  decltype(&NvAPI_D3D12_DestroyCuFunction) destroyFunction_ = nullptr;
  decltype(&NvAPI_D3D12_LaunchCuKernelChainEx) launch_ = nullptr;
  decltype(&NvAPI_D3D12_GetCudaMergedTextureSamplerObject) texture_ = nullptr;
  decltype(&NvAPI_D3D12_GetCudaIndependentDescriptorObject) surface_ = nullptr;
  bool initialized_ = false;
  template<class T> T get(const char* name);
  void check(NvAPI_Status status, const char* operation) const;
};

}  // namespace ngx
