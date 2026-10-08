#include "d3d12_graph.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <stdexcept>

namespace ngx {
namespace {
struct MlpPush { uint32_t rows, inputStride, inputColumnBase, outputStride, outputColumnOffset, batches; };
struct GemmPush {
  uint32_t rows, n, nmatrix, weightColumnOffset, inputStride, inputColumnBase, outputStride,
           outputColumnOffset, auxHalfOffset, batches, columnGroups, splitStride;
};
template<class T> T pushOf(const vk::TraceOperation& operation) {
  if (operation.push.size() != sizeof(T)) throw std::runtime_error("unexpected shader parameters: " + operation.shader);
  T result;
  memcpy(&result, operation.push.data(), sizeof(result));
  return result;
}
uint32_t constant(const vk::TraceOperation& op, uint32_t id, uint32_t fallback = 0) {
  for (const auto& entry : op.constants.entries) if (entry.constantID == id) return op.constants.data.at(entry.offset / 4);
  return fallback;
}
std::vector<uint8_t> immutableBytes(const vk::TraceBuffer& buffer) {
  std::vector<uint8_t> result(static_cast<size_t>(buffer.bytes), 0);
  if (buffer.uploads.empty()) throw std::runtime_error("missing CPU weights for " + buffer.label);
  for (const auto& upload : buffer.uploads) memcpy(result.data() + upload.offset, upload.bytes.data(), upload.bytes.size());
  return result;
}
int addWeights(vk::CommandTrace& plan, std::string label, std::vector<uint8_t> bytes) {
  const int index = static_cast<int>(plan.buffers.size());
  const uint64_t size = bytes.size();
  plan.buffers.push_back({std::move(label), size, false, {{0, std::move(bytes)}}});
  return index;
}
void translateWeights(vk::CommandTrace& plan) {
  for (auto& op : plan.operations) {
    if (op.kind != vk::TraceOperation::Kind::Shader) continue;
    if (op.shader == "gemm_mlp") {
      const auto p = pushOf<MlpPush>(op);
      if (constant(op, 0) != 64 || constant(op, 1) != 256 || constant(op, 2) != 64 || constant(op, 3) != 0)
        throw std::runtime_error("unsupported D3D12 MLP shape");
      const auto original1 = immutableBytes(plan.buffers.at(op.bindings[1]));
      const auto original2 = immutableBytes(plan.buffers.at(op.bindings[2]));
      const size_t bytes = static_cast<size_t>(p.batches) * 64 * 256;
      if (original1.size() < bytes || original2.size() < bytes) throw std::runtime_error("short MLP weights");
      std::vector<uint8_t> w1(bytes), w2(bytes);
      for (uint32_t batch = 0; batch < p.batches; ++batch) {
        for (uint32_t kt = 0; kt < 2; ++kt) for (uint32_t n = 0; n < 256; ++n) {
          const uint32_t block = n / 32, half = (n % 32) / 16, v = n % 16;
          const uint32_t source = block * 32 + half * 16 + (v % 8 / 2) * 4 + v % 2 + (v / 8) * 2;
          memcpy(w1.data() + ((batch * 2 + kt) * 256 + n) * 32,
                 original1.data() + ((batch * 2 + kt) * 256 + source) * 32, 32);
        }
        for (uint32_t kt = 0; kt < 8; ++kt) for (uint32_t n = 0; n < 64; ++n)
          memcpy(w2.data() + ((batch * 8 + kt) * 64 + n) * 32,
                 original2.data() + (batch * 64 + n) * 256 + kt * 32, 32);
      }
      op.bindings[1] = addWeights(plan, "PTX split MLP W1", std::move(w1));
      op.bindings[2] = addWeights(plan, "PTX split MLP W2", std::move(w2));
    } else if (op.shader == "gemm_fp8") {
      const auto p = pushOf<GemmPush>(op);
      if (constant(op, 0) != 64 || constant(op, 2) != 0 || constant(op, 3) != 0 || constant(op, 9, 1) != 1 ||
          p.n != 32 || p.batches != 1 || p.nmatrix < p.weightColumnOffset + 32)
        throw std::runtime_error("unsupported D3D12 narrow GEMM shape");
      const auto old = immutableBytes(plan.buffers.at(op.bindings[1]));
      if (old.size() < static_cast<size_t>(64) * p.nmatrix) throw std::runtime_error("short narrow GEMM weights");
      std::vector<uint8_t> padded(64 * 64, 0);
      for (uint32_t kt = 0; kt < 2; ++kt)
        memcpy(padded.data() + kt * 64 * 32, old.data() + (kt * p.nmatrix + p.weightColumnOffset) * 32, 32 * 32);
      op.bindings[1] = addWeights(plan, "PTX padded narrow GEMM", std::move(padded));
    } else if (op.shader != "ops") {
      throw std::runtime_error("D3D12 graph does not implement fallback shader: " + op.shader);
    }
  }
}
void check(HRESULT result, const char* operation) {
  if (FAILED(result)) throw std::runtime_error(std::string(operation) + ": HRESULT " + std::to_string(static_cast<unsigned>(result)));
}
void uavBarrier(ID3D12GraphicsCommandList* commands) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  commands->ResourceBarrier(1, &barrier);
}
void transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                  D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  commands->ResourceBarrier(1, &barrier);
}
Microsoft::WRL::ComPtr<ID3D12Resource> makeBuffer(ID3D12Device* device, uint64_t bytes, bool upload) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = std::max<uint64_t>(bytes, 16); desc.Height = 1;
  desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = upload ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const auto state = upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  Microsoft::WRL::ComPtr<ID3D12Resource> result;
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&result)), "Create graph buffer");
  return result;
}
}

Dx12Graph::Dx12Graph(ID3D12Device* device, ID3D12GraphicsCommandList* initialization,
                     vk::CommandTrace plan, const std::string& opsPtx, GraphNumerics numerics) : device_(device) {
  if (!device || !initialization) throw std::runtime_error("D3D12 graph needs device and open command list");
  std::ifstream file(opsPtx, std::ios::binary);
  if (!file) throw std::runtime_error("missing operations PTX: " + opsPtx);
  const std::string ptx{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  bool uploadMapped = false;
  try {
    opsModule_ = api_.createModule(device, ptx);
    ops_ = api_.createFunction(device, opsModule_, "nr_ops");
    fill_ = api_.createFunction(device, opsModule_, "nr_fill");
    auto variant = [&](const char* name, NVDX_ObjectHandle& module, NVDX_ObjectHandle& function) {
      const auto path = std::filesystem::path(opsPtx).parent_path() / (std::string(name) + ".ptx");
      std::ifstream in(path, std::ios::binary);
      if (!in) throw std::runtime_error("missing NGX PTX: " + path.string());
      const std::string code{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
      module = api_.createModule(device, code);
      function = api_.createFunction(device, module, name);
    };
    variant(numerics == GraphNumerics::Native ? "mlp_e4m3_K64_H256_N64_split_native_hidden" :
            "mlp_e4m3_K64_H256_N64_split_raw_hidden", mlpModule_, mlp_);
    variant("gemm2_e4m3_K64_f8_n32", narrowModule_, narrow_);
    translateWeights(plan);
    if (numerics == GraphNumerics::Native) for (const auto& function : plan.functions) {
      if (function.name != "global_normalize_e4m3" && function.name.find("global_attention_e4m3_p") != 0) continue;
      const auto path = std::filesystem::path(opsPtx).parent_path() / (function.name + "_native.ptx");
      std::ifstream in(path, std::ios::binary);
      if (!in) throw std::runtime_error("missing native normalization PTX: " + path.string());
      plan.modules.at(function.module) = {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    modules_.reserve(plan.modules.size()); functions_.reserve(plan.functions.size());
    for (const auto& module : plan.modules) modules_.push_back(api_.createModule(device, module));
    for (const auto& function : plan.functions)
      functions_.push_back(api_.createFunction(device, modules_.at(function.module), function.name.c_str()));
    std::vector<bool> used(plan.buffers.size(), false);
    auto use = [&](int index) { if (index >= 0) used.at(index) = true; };
    for (const auto& operation : plan.operations) {
      for (const auto& arg : operation.arguments) use(arg.buffer);
      for (int index : operation.bindings) use(index);
      use(operation.buffer);
      use(operation.source);
    }
    buffers_.resize(plan.buffers.size()); bytes_.resize(plan.buffers.size());
    uint64_t uploadBytes = 0;
    for (size_t i = 0; i < plan.buffers.size(); ++i) {
      if (!used[i]) continue;
      bytes_[i] = plan.buffers[i].bytes;
      buffers_[i] = makeBuffer(device, bytes_[i], false);
      const auto desc = buffers_[i]->GetDesc();
      allocatedBytes_ += device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
      for (const auto& part : plan.buffers[i].uploads) uploadBytes += (part.bytes.size() + 15) & ~uint64_t(15);
    }
    uint8_t* mapped = nullptr;
    if (uploadBytes) {
      upload_ = makeBuffer(device, uploadBytes, true);
      D3D12_RANGE empty{};
      check(upload_->Map(0, &empty, reinterpret_cast<void**>(&mapped)), "Map weights upload");
      uploadMapped = true;
    }
    uint64_t uploadOffset = 0;
    for (size_t i = 0; i < plan.buffers.size(); ++i) {
      if (!used[i]) continue;
      clear(initialization, static_cast<int>(i), 0);
      const auto& parts = plan.buffers[i].uploads;
      if (!parts.empty()) {
        transition(initialization, buffers_[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        for (const auto& part : parts) {
          memcpy(mapped + uploadOffset, part.bytes.data(), part.bytes.size());
          initialization->CopyBufferRegion(buffers_[i].Get(), part.offset, upload_.Get(), uploadOffset, part.bytes.size());
          uploadOffset += (part.bytes.size() + 15) & ~uint64_t(15);
        }
        transition(initialization, buffers_[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      }
    }
    if (uploadMapped) { upload_->Unmap(0, nullptr); uploadMapped = false; }
    prepare(plan.operations);
  } catch (...) {
    if (uploadMapped) upload_->Unmap(0, nullptr);
    destroyKernels();
    throw;
  }
}

void Dx12Graph::destroyKernels() {
  for (auto function : functions_) api_.destroyFunction(device_.Get(), function);
  functions_.clear();
  for (auto module : modules_) api_.destroyModule(device_.Get(), module);
  modules_.clear();
  api_.destroyFunction(device_.Get(), ops_); api_.destroyFunction(device_.Get(), fill_);
  api_.destroyFunction(device_.Get(), mlp_); api_.destroyFunction(device_.Get(), narrow_);
  api_.destroyModule(device_.Get(), mlpModule_); api_.destroyModule(device_.Get(), narrowModule_);
  api_.destroyModule(device_.Get(), opsModule_);
  ops_ = fill_ = opsModule_ = nullptr;
  mlp_ = narrow_ = mlpModule_ = narrowModule_ = nullptr;
}
Dx12Graph::~Dx12Graph() { destroyKernels(); }

ID3D12Resource* Dx12Graph::buffer(int index) const {
  if (index < 0 || static_cast<size_t>(index) >= buffers_.size() || !buffers_[index])
    throw std::runtime_error("unavailable D3D12 graph buffer");
  return buffers_[index].Get();
}
uint64_t Dx12Graph::address(int index) const { return index < 0 ? 0 : buffer(index)->GetGPUVirtualAddress(); }

void Dx12Graph::clear(ID3D12GraphicsCommandList* commands, int index, uint32_t value) {
  uint64_t pointer = address(index);
  if (bytes_.at(index) / 4 > UINT32_MAX || bytes_.at(index) % 4) throw std::runtime_error("unsupported clear size");
  uint32_t words = static_cast<uint32_t>(bytes_[index] / 4);
  void* arguments[] = {&pointer, &words, &value};
  api_.launch(commands, fill_, {(words + 255) / 256, 1, 1}, {256, 1, 1}, 0, arguments);
  uavBarrier(commands);
}

void Dx12Graph::prepare(const std::vector<vk::TraceOperation>& operations) {
  Batch batch;
  batch.kernels.reserve(operations.size()); batch.values.reserve(operations.size()); batch.pointers.reserve(operations.size());
  auto enqueue = [&](NVDX_ObjectHandle function, NVAPI_DIM3 grid, NVAPI_DIM3 block, uint32_t shared,
                      void* const* arguments, const std::vector<uint32_t>& sizes) {
    batch.values.emplace_back(sizes.size() * 8, 0);
    batch.pointers.emplace_back(sizes.size());
    for (size_t i = 0; i < sizes.size(); ++i) {
      if (sizes[i] > 64) throw std::runtime_error("kernel argument exceeds the owned argument slot");
      batch.pointers.back()[i] = batch.values.back().data() + i * 8;
      memcpy(batch.pointers.back()[i], arguments[i], sizes[i]);
    }
    NVAPI_CU_KERNEL_LAUNCH_PARAMS_EX kernel{};
    kernel.hFunction = function; kernel.gridDim = grid; kernel.blockDim = block;
    kernel.dynSharedMemBytes = shared; kernel.kernelParams = batch.pointers.back().data();
    batch.kernels.push_back(kernel);
  };
  auto flush = [&] {
    if (batch.kernels.empty()) return;
    batches_.push_back(std::move(batch));
    batch = {};
  };
  for (const auto& operation : operations) {
    if (operation.kind == vk::TraceOperation::Kind::Barrier) {
      flush();
    } else if (operation.kind == vk::TraceOperation::Kind::Clear) {
      const uint64_t bytes = bytes_.at(operation.buffer);
      if (bytes / 4 > UINT32_MAX || bytes % 4) throw std::runtime_error("unsupported clear size");
      uint64_t pointer = address(operation.buffer);
      uint32_t words = static_cast<uint32_t>(bytes / 4), value = operation.fill;
      void* arguments[] = {&pointer, &words, &value};
      enqueue(fill_, {(words + 255) / 256, 1, 1}, {256, 1, 1}, 0, arguments, {8, 4, 4});
    } else if (operation.kind == vk::TraceOperation::Kind::Copy) {
      flush();
      Batch copy;
      copy.source = operation.source; copy.destination = operation.buffer; copy.copyBytes = operation.copyBytes;
      batches_.push_back(std::move(copy));
    } else if (operation.kind == vk::TraceOperation::Kind::Ptx) {
      std::vector<uint64_t> storage(operation.arguments.size());
      std::vector<void*> arguments(storage.size());
      std::vector<uint32_t> sizes(storage.size());
      for (size_t i = 0; i < arguments.size(); ++i) {
        const auto& arg = operation.arguments[i];
        storage[i] = arg.value + (arg.buffer >= 0 ? address(arg.buffer) : 0);
        arguments[i] = &storage[i];
        sizes[i] = arg.bytes;
      }
      enqueue(functions_.at(operation.function), {operation.grid[0], operation.grid[1], operation.grid[2]},
               {operation.threads, 1, 1}, operation.shared, arguments.data(), sizes);
    } else if (operation.kind == vk::TraceOperation::Kind::Shader) {
      if (operation.shader == "gemm_mlp") {
        auto p = pushOf<MlpPush>(operation);
        uint64_t input = address(operation.bindings[0]), w1 = address(operation.bindings[1]),
                 w2 = address(operation.bindings[2]), output = address(operation.bindings[5]);
        void* arguments[] = {&input, &w1, &w2, &output, &p.rows, &p.inputStride, &p.inputColumnBase, &p.outputStride, &p.outputColumnOffset};
        enqueue(mlp_, {p.batches, (p.rows + 63) / 64, 1}, {128, 1, 1}, 0, arguments, {8, 8, 8, 8, 4, 4, 4, 4, 4});
        continue;
      }
      if (operation.shader == "gemm_fp8") {
        auto p = pushOf<GemmPush>(operation);
        uint64_t input = address(operation.bindings[0]), weights = address(operation.bindings[1]),
                 output = address(operation.bindings[2]), zero64 = 0;
        uint32_t zero = 0, nmatrix = 64, width = 1;
        void* arguments[] = {&input, &weights, &input, &input, &input, &output, &p.rows, &p.inputStride, &p.inputColumnBase,
                             &nmatrix, &zero, &p.outputStride, &p.outputColumnOffset, &zero,
                             &zero64, &zero, &zero, &zero64, &width, &zero64, &zero, &zero, &input};
        enqueue(narrow_, {1, (p.rows + 63) / 64, 1}, {128, 1, 1}, 0, arguments,
                 {8, 8, 8, 8, 8, 8, 4, 4, 4, 4, 4, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 8});
        continue;
      }
      if (operation.shader != "ops" || operation.push.size() != 36 || operation.constants.data.size() != 1)
        throw std::runtime_error("unsupported D3D12 shader operation");
      uint32_t mode = operation.constants.data[0];
      if (mode > 4) throw std::runtime_error("unsupported D3D12 elementwise mode");
      uint64_t pointers[7];
      void* arguments[9] = {&mode};
      for (uint32_t i = 0; i < 7; ++i) { pointers[i] = address(operation.bindings[i]); arguments[i + 1] = &pointers[i]; }
      arguments[8] = const_cast<uint8_t*>(operation.push.data());
      enqueue(ops_, {operation.grid[0], operation.grid[1], operation.grid[2]}, {256, 1, 1}, 0, arguments,
               {4, 8, 8, 8, 8, 8, 8, 8, 36});
    }
    // Only launches already chained by the graph share a driver batch. Preserve
    // recorded barriers, including the counter reset before each evaluation.
  }
  flush();
}

void Dx12Graph::record(ID3D12GraphicsCommandList* commands) {
  for (const auto& batch : batches_) {
    if (batch.source >= 0) {
      auto* source = buffer(batch.source);
      auto* destination = buffer(batch.destination);
      transition(commands, source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      transition(commands, destination, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      commands->CopyBufferRegion(destination, 0, source, 0, batch.copyBytes);
      transition(commands, destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      transition(commands, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    } else {
      api_.launchChain(commands, batch.kernels.data(), static_cast<uint32_t>(batch.kernels.size()));
      uavBarrier(commands);
    }
  }
}

}  // namespace ngx
