#include "d3d12_feature.h"
#include "ngx_error.h"
#include "nr_graph.h"
#include "numeric.h"
#include "reference.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace ngx {
namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT result, const char* operation) {
  require(SUCCEEDED(result), NVSDK_NGX_Result_FAIL_PlatformError,
          std::string(operation) + ": HRESULT " + std::to_string(static_cast<unsigned>(result)));
}
void barrier(ID3D12GraphicsCommandList* commands) {
  D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  commands->ResourceBarrier(1, &barrier);
}
void transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                  D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
  commands->ResourceBarrier(1, &barrier);
}
bool workspaceReuseEnabled() {
  wchar_t value[2]{};
  SetLastError(ERROR_SUCCESS);
  const DWORD length = GetEnvironmentVariableW(L"OPEN_DLSS_NR_WORKSPACE", value, 2);
  if (!length && GetLastError() == ERROR_ENVVAR_NOT_FOUND) return true;
  require(length == 1 && (value[0] == L'0' || value[0] == L'1'),
          NVSDK_NGX_Result_FAIL_InvalidParameter, "OPEN_DLSS_NR_WORKSPACE must be 0 or 1");
  return value[0] == L'1';
}
class UploadBatch {
 public:
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  explicit UploadBatch(ID3D12Device* device) {
    D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "upload queue");
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "upload allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "upload list");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "upload fence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, NVSDK_NGX_Result_FAIL_PlatformError, "upload event");
  }
  ~UploadBatch() { if (event) CloseHandle(event); }
  void finish() {
    check(list->Close(), "upload close");
    ID3D12CommandList* lists[] = {list.Get()}; queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), 1), "upload signal");
    check(fence->SetEventOnCompletion(1, event), "upload completion");
    require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, NVSDK_NGX_Result_FAIL_PlatformError, "upload timed out");
  }
};
std::string read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  require(bool(file), NVSDK_NGX_Result_FAIL_UnableToInitializeFeature, "missing " + path.string());
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
ID3D12Resource* resource(const NVSDK_NGX_Parameter* parameters, const char* name, bool mandatory) {
  void* pointer = optional<void*>(parameters, name, nullptr);
  require(pointer || !mandatory, NVSDK_NGX_Result_FAIL_MissingInput, std::string("missing ") + name);
  return static_cast<ID3D12Resource*>(pointer);
}
void rectangle(const NVSDK_NGX_Parameter* parameters, const char* prefix, ID3D12Resource* resource,
                 uint32_t width, uint32_t height, uint32_t& x, uint32_t& y) {
  const std::string key = std::string("DLSSNR.") + prefix + "Subrect";
  const int baseX = optional<int>(parameters, (key + "BaseX").c_str(), 0);
  const int baseY = optional<int>(parameters, (key + "BaseY").c_str(), 0);
  const int w = optional<int>(parameters, (key + "Width").c_str(), static_cast<int>(width));
  const int h = optional<int>(parameters, (key + "Height").c_str(), static_cast<int>(height));
  const auto desc = resource->GetDesc();
  require(baseX >= 0 && baseY >= 0 && w == static_cast<int>(width) && h == static_cast<int>(height) &&
          static_cast<uint64_t>(baseX) + width <= desc.Width && static_cast<uint64_t>(baseY) + height <= desc.Height,
          NVSDK_NGX_Result_FAIL_UnsupportedParameter, "unsupported rectangle for " + std::string(prefix));
  x = static_cast<uint32_t>(baseX); y = static_cast<uint32_t>(baseY);
}
}

Dx12Feature::Dx12Feature(ID3D12Device* device, uint32_t width, uint32_t height, const std::filesystem::path& root)
    : device_(device), width_(width), height_(height) {
  const bool reuseWorkspace = workspaceReuseEnabled();
  const auto geometry = nr::Geometry::fromValid(width, height);
  fullWidth_ = geometry.fullWidth; fullHeight_ = geometry.fullHeight;
  require(fullWidth_ < 2 * width && fullHeight_ < 2 * height, NVSDK_NGX_Result_FAIL_UnsupportedParameter, "image too small for mirror padding");
  vk::CommandTrace plan;
  {
    vk::Context context;
    VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &identity; vkGetPhysicalDeviceProperties2(context.physical(), &properties);
    const LUID luid = device->GetAdapterLuid();
    require(identity.deviceLUIDValid && !memcmp(identity.deviceLUID, &luid, sizeof(luid)),
            NVSDK_NGX_Result_FAIL_FeatureNotSupported, "graph compiler selected a different GPU");
    context.setCommandTrace(&plan);
    nr::Model model(context, (root / "models/nr").string());
    nr::Kernels kernels(context, (root / "build/shaders").string());
    kernels.setSiluTable(ref::siluTable());
    const auto& blend = model.tensor(70, 0, "blend_scale");
    if (blend.byteLength >= 2) blendScale_ = num::f16ToF32(static_cast<uint16_t>(blend.bytes[0] | (blend.bytes[1] << 8)));
    nr::Graph::Options graphOptions;
    graphOptions.reuseWorkspace = reuseWorkspace;
    nr::Graph graph(context, model, kernels, geometry, graphOptions);
    auto* input = graph.allocate("input features", fullWidth_ * fullHeight_, 16, nr::Format::F16);
    VkCommandBuffer commands = context.beginCommands();
    plan.record(commands);
    graph.record(commands, *input);
    features_ = plan.bufferIndex(input->buffer); head_ = plan.bufferIndex(graph.head().buffer);
    VK_CHECK(vkEndCommandBuffer(commands));
    context.setCommandTrace(nullptr);
  }
  UploadBatch upload(device);
  graph_ = std::make_unique<Dx12Graph>(device, upload.list.Get(), std::move(plan),
                                     (root / "build/ngx/nr_ops.ptx").string(), GraphNumerics::Native);
  upload.finish();
  check(device->GetDeviceRemovedReason(), "device after initialization");
  graph_->releaseUploadAfterCompletion();
  allocatedBytes_ = graph_->allocatedBytes();
  D3D12_DESCRIPTOR_HEAP_DESC heap{};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = 128;
  heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&views_)), "texture descriptors");
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER; heap.NumDescriptors = 2;
  check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&samplers_)), "sampler descriptors");
  viewStride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  samplerStride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  for (uint32_t i = 0; i < 2; ++i) {
    D3D12_SAMPLER_DESC sampler{};
    sampler.Filter = i ? D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX; sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    auto descriptor = samplers_->GetCPUDescriptorHandleForHeapStart(); descriptor.ptr += i * samplerStride_;
    device->CreateSampler(&sampler, descriptor);
    D3D12_HEAP_PROPERTIES memory{}; memory.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = width; desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device->CreateCommittedResource(&memory, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                         nullptr, IID_PPV_ARGS(&history_[i])), "history texture");
    allocatedBytes_ += device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    view(history_[i].Get(), false, true); view(history_[i].Get(), true);
  }
  try {
    frameModule_ = graph_->api().createModule(device, read(root / "build/ngx/nr_frame.ptx"));
    prepare_ = graph_->api().createFunction(device, frameModule_, "nr_prepare");
    compose_ = graph_->api().createFunction(device, frameModule_, "nr_compose");
  } catch (...) {
    graph_->api().destroyFunction(device, prepare_);
    graph_->api().destroyModule(device, frameModule_);
    throw;
  }
  fprintf(stderr, "[OpenDLSS-NR] D3D12 optimized backend %ux%u full=%ux%u graph_bytes=%llu\n",
          width, height, fullWidth_, fullHeight_, static_cast<unsigned long long>(graph_->allocatedBytes()));
}

Dx12Feature::~Dx12Feature() {
  if (graph_) {
    graph_->api().destroyFunction(device_.Get(), prepare_); graph_->api().destroyFunction(device_.Get(), compose_);
    graph_->api().destroyModule(device_.Get(), frameModule_);
  }
}

uint64_t Dx12Feature::view(ID3D12Resource* resource, bool writable, bool linear) {
  if (!resource) return 0;
  for (const auto& cached : cached_)
    if (cached.resource.Get() == resource && cached.writable == writable && cached.linear == linear) return cached.object;
  require(nextView_ < 128, NVSDK_NGX_Result_FAIL_OutOfGPUMemory, "descriptor cache full; recreate the feature");
  ComPtr<ID3D12Device> owner;
  check(resource->GetDevice(IID_PPV_ARGS(&owner)), "texture device");
  require(owner.Get() == device_.Get(), NVSDK_NGX_Result_FAIL_InvalidParameter, "texture belongs to another device");
  const auto desc = resource->GetDesc();
  require(desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count == 1 && desc.DepthOrArraySize == 1,
          NVSDK_NGX_Result_FAIL_UnsupportedFormat, "only non-MSAA 2D textures are supported");
  auto descriptor = views_->GetCPUDescriptorHandleForHeapStart(); descriptor.ptr += nextView_ * viewStride_;
  uint64_t object = 0;
  if (writable) {
    require((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0, NVSDK_NGX_Result_FAIL_RWFlagMissing, "output needs UAV usage");
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = desc.Format; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(resource, nullptr, &uav, descriptor);
    object = graph_->api().surface(device_.Get(), descriptor);
  } else {
    require((desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0,
            NVSDK_NGX_Result_FAIL_UnsupportedFormat, "input does not allow shader-resource views");
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = desc.Format; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(resource, &srv, descriptor);
    auto sampler = samplers_->GetCPUDescriptorHandleForHeapStart(); if (linear) sampler.ptr += samplerStride_;
    object = graph_->api().texture(device_.Get(), descriptor, sampler);
  }
  ++nextView_;
  cached_.push_back({resource, writable, linear, object});
  return object;
}

void Dx12Feature::evaluate(ID3D12GraphicsCommandList* commands, const NVSDK_NGX_Parameter* parameters) {
  require(commands && parameters && !poisoned_, NVSDK_NGX_Result_FAIL_InvalidParameter, "invalid or failed feature");
  ComPtr<ID3D12Device> device;
  check(commands->GetDevice(IID_PPV_ARGS(&device)), "command-list device");
  require(device.Get() == device_.Get(), NVSDK_NGX_Result_FAIL_InvalidParameter, "command list belongs to another device");
  require(commands->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT || commands->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE,
          NVSDK_NGX_Result_FAIL_InvalidParameter, "NR requires a direct or compute command list");
  auto* color = resource(parameters, "DLSSNR.Color", true);
  auto* output = resource(parameters, "DLSSNR.Output", true);
  auto* motion = resource(parameters, "DLSSNR.MVec", false);
  require(color != output, NVSDK_NGX_Result_FAIL_UnsupportedParameter, "in-place color/output is not supported");
  for (const char* key : {"DLSSNR.Depth", "DLSSNR.ControlMask", "DLSSNR.UI", "DLSSNR.UIAlpha", "DLSSNR.Backbuffer", "DLSSNR.BidirectionalDistortionField"})
    require(!resource(parameters, key, false), NVSDK_NGX_Result_FAIL_UnsupportedParameter, std::string("not implemented: ") + key);
  const auto colorDesc = color->GetDesc(), outDesc = output->GetDesc();
  require((colorDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || colorDesc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT) &&
          outDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT, NVSDK_NGX_Result_FAIL_UnsupportedFormat, "color needs RGBA16F/32F; output needs RGBA16F");
  if (motion) require(motion->GetDesc().Format == DXGI_FORMAT_R16G16_FLOAT || motion->GetDesc().Format == DXGI_FORMAT_R32G32_FLOAT,
                       NVSDK_NGX_Result_FAIL_UnsupportedFormat, "motion needs RG16F/32F");
  NrFrameParams p{};
  p.width = width_; p.height = height_; p.fullWidth = fullWidth_; p.fullHeight = fullHeight_;
  p.enabled = optional<int>(parameters, "DLSSNR.Enabled", 1) != 0;
  p.autoMask = optional<int>(parameters, "DLSSNR.UseAutoMask", 1) != 0;
  p.style = optional<unsigned>(parameters, "DLSSNR.Style", 0);
  require(p.style == 0 && optional<int>(parameters, "DLSSNR.UICorrection", 0) == 0,
          NVSDK_NGX_Result_FAIL_UnsupportedParameter, "style and UI correction are not implemented yet");
  p.intensity = optional<float>(parameters, "DLSSNR.Intensity", 1.0f);
  p.localTone = optional<float>(parameters, "DLSSNR.LocalToneStrength", 1.0f);
  p.localStructure = optional<float>(parameters, "DLSSNR.LocalStructureStrength", 1.0f);
  p.skinStructure = optional<float>(parameters, "DLSSNR.SkinStructureStrength", -1.0f);
  p.motionScaleX = optional<float>(parameters, "DLSSNR.MVecScaleX", 1.0f);
  p.motionScaleY = optional<float>(parameters, "DLSSNR.MVecScaleY", 1.0f);
  p.blendScale = blendScale_;
  for (float value : {p.intensity, p.localTone, p.localStructure, p.skinStructure, p.motionScaleX, p.motionScaleY})
    require(std::isfinite(value), NVSDK_NGX_Result_FAIL_InvalidParameter, "nonfinite NR parameter");
  require(p.intensity >= 0.0f && p.intensity <= 1.0f, NVSDK_NGX_Result_FAIL_UnsupportedParameter, "intensity outside [0,1]");
  p.motionScaleX /= static_cast<float>(width_); p.motionScaleY /= static_cast<float>(height_);
  rectangle(parameters, "Color", color, width_, height_, p.colorX, p.colorY);
  rectangle(parameters, "Output", output, width_, height_, p.outputX, p.outputY);
  p.colorWidth = static_cast<uint32_t>(colorDesc.Width); p.colorHeight = colorDesc.Height;
  if (motion) {
    rectangle(parameters, "MVec", motion, width_, height_, p.motionX, p.motionY);
    p.motionWidth = static_cast<uint32_t>(motion->GetDesc().Width); p.motionHeight = motion->GetDesc().Height;
  }
  const bool reset = optional<int>(parameters, "DLSSNR.Reset", 0) != 0;
  p.seed = reset ? 0 : seed_;
  p.historyValid = !reset && frames_ != 0;
  const uint32_t previous = static_cast<uint32_t>(frames_ & 1), next = 1 - previous;
  uint64_t colorView = view(color, false), motionView = view(motion, false);
  uint64_t historyView = view(history_[previous].Get(), false, true);
  uint64_t nextView = view(history_[next].Get(), true), outputView = view(output, true);
  uint64_t features = graph_->buffer(features_)->GetGPUVirtualAddress(), head = graph_->buffer(head_)->GetGPUVirtualAddress();
  try {
    transition(commands, history_[previous].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (p.enabled) {
      void* args[] = {&colorView, &historyView, &motionView, &features, &p};
      graph_->api().launch(commands, prepare_, {(fullWidth_ + 7) / 8, (fullHeight_ + 7) / 8, 1}, {8, 8, 1}, 0, args);
      barrier(commands);
      graph_->record(commands);
    }
    void* args[] = {&colorView, &historyView, &motionView, &head, &nextView, &outputView, &p};
    graph_->api().launch(commands, compose_, {(width_ + 7) / 8, (height_ + 7) / 8, 1}, {8, 8, 1}, 0, args);
    barrier(commands);
    transition(commands, history_[previous].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (p.enabled) { ++frames_; seed_ = p.seed + 1; }
    else { frames_ = 0; seed_ = 0; }
  } catch (...) { poisoned_ = true; throw; }
}

}  // namespace ngx
