#pragma once
#include "d3d12_graph.h"
#include "frame_params.h"
#include <nvsdk_ngx.h>
#include <filesystem>
#include <memory>

namespace ngx {

class Dx12Feature {
 public:
  Dx12Feature(ID3D12Device* device, uint32_t width, uint32_t height, const std::filesystem::path& dataRoot);
  ~Dx12Feature();
  void evaluate(ID3D12GraphicsCommandList* commands, const NVSDK_NGX_Parameter* parameters);
  ID3D12Device* device() const { return device_.Get(); }
  uint64_t frames() const { return frames_; }
  uint64_t allocatedBytes() const { return allocatedBytes_; }

 private:
  using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  std::unique_ptr<Dx12Graph> graph_;
  Resource history_[2];
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> views_, samplers_;
  struct View {
    Resource resource;
    bool writable = false, linear = false;
    uint64_t object = 0;
  };
  std::vector<View> cached_;
  uint32_t nextView_ = 0, viewStride_ = 0, samplerStride_ = 0;
  NVDX_ObjectHandle frameModule_ = nullptr, prepare_ = nullptr, compose_ = nullptr;
  uint32_t width_ = 0, height_ = 0, fullWidth_ = 0, fullHeight_ = 0;
  uint64_t frames_ = 0;
  uint64_t allocatedBytes_ = 0;
  uint32_t seed_ = 0;
  int features_ = -1, head_ = -1;
  float blendScale_ = 1.0f;
  bool poisoned_ = false;
  uint64_t view(ID3D12Resource* resource, bool writable, bool linear = false);
};

}  // namespace ngx
