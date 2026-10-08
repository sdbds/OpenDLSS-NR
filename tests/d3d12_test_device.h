#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace dx_test {
using Microsoft::WRL::ComPtr;
inline void check(HRESULT value, const char* operation) {
  if (FAILED(value)) throw std::runtime_error(std::string(operation) + " HRESULT=" + std::to_string(static_cast<unsigned>(value)));
}
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
      const HRESULT result = factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
      if (result == DXGI_ERROR_NOT_FOUND) break;
      check(result, "EnumAdapterByGpuPreference");
      DXGI_ADAPTER_DESC1 description{};
      check(candidate->GetDesc1(&description), "GetDesc1");
      if (description.VendorId != 0x10de || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
      if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
        check(candidate.As(&adapter), "adapter3");
        printf("adapter vendor=0x%04x device=0x%04x\n", description.VendorId, description.DeviceId);
        break;
      }
    }
    if (!device) throw std::runtime_error("no NVIDIA D3D12 device");
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "CreateCommandList");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("CreateEventW failed");
  }
  ~D3D() { if (event) CloseHandle(event); }
  D3D(const D3D&) = delete;
  D3D& operator=(const D3D&) = delete;
  void submit() {
    check(list->Close(), "Close");
    ID3D12CommandList* commands[] = {list.Get()};
    queue->ExecuteCommandLists(1, commands);
    check(queue->Signal(fence.Get(), ++value), "Signal");
    check(fence->SetEventOnCompletion(value, event), "SetEventOnCompletion");
    if (WaitForSingleObject(event, 30000) != WAIT_OBJECT_0) throw std::runtime_error("D3D12 GPU wait timed out");
    check(device->GetDeviceRemovedReason(), "device status");
    check(allocator->Reset(), "allocator Reset");
    check(list->Reset(allocator.Get(), nullptr), "list Reset");
  }
  ComPtr<ID3D12Resource> buffer(UINT64 bytes, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes; desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type == D3D12_HEAP_TYPE_DEFAULT) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
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
}  // namespace dx_test
