#pragma once
#include "compute_trace.h"
#include "d3d12_nvapi.h"
#include <wrl/client.h>

namespace ngx {

enum class GraphNumerics { Vulkan, Native };

class Dx12Graph {
 public:
  Dx12Graph(ID3D12Device* device, ID3D12GraphicsCommandList* initialization,
            vk::CommandTrace plan, const std::string& opsPtx, GraphNumerics numerics = GraphNumerics::Vulkan);
  ~Dx12Graph();
  Dx12Graph(const Dx12Graph&) = delete;
  Dx12Graph& operator=(const Dx12Graph&) = delete;
  void record(ID3D12GraphicsCommandList* commands);
  ID3D12Resource* buffer(int index) const;
  uint64_t allocatedBytes() const { return allocatedBytes_; }
  void releaseUploadAfterCompletion() { upload_.Reset(); }
  NvApi& api() { return api_; }

 private:
  using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
  NvApi api_;
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  std::vector<Resource> buffers_;
  std::vector<uint64_t> bytes_;
  Resource upload_;
  std::vector<NVDX_ObjectHandle> modules_, functions_;
  struct Batch {
    std::vector<NVAPI_CU_KERNEL_LAUNCH_PARAMS_EX> kernels;
    std::vector<std::vector<uint64_t>> values;
    std::vector<std::vector<void*>> pointers;
    int source = -1, destination = -1;
    uint64_t copyBytes = 0;
  };
  std::vector<Batch> batches_;
  NVDX_ObjectHandle opsModule_ = nullptr, ops_ = nullptr, fill_ = nullptr;
  NVDX_ObjectHandle mlpModule_ = nullptr, mlp_ = nullptr, narrowModule_ = nullptr, narrow_ = nullptr;
  uint64_t allocatedBytes_ = 0;
  uint64_t address(int index) const;
  void clear(ID3D12GraphicsCommandList* commands, int index, uint32_t value);
  void prepare(const std::vector<vk::TraceOperation>& operations);
  void destroyKernels();
};

}  // namespace ngx
