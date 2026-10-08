#include "compute_trace.h"

#include <cstring>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace vk {

void CommandTrace::bufferCreated(const Buffer& buffer, VkDeviceAddress address) {
  if (!buffer.buffer || !address || buffer.size > UINT64_MAX - address || active_.count(buffer.buffer))
    throw std::runtime_error("invalid or duplicate trace buffer");
  auto next = addresses_.lower_bound(address);
  if ((next != addresses_.end() && next->first < address + buffer.size) ||
      (next != addresses_.begin() && std::prev(next)->second.end > address))
    throw std::runtime_error("overlapping trace buffer addresses");
  const int index = static_cast<int>(buffers.size());
  buffers.push_back({buffer.label ? buffer.label : "", buffer.size, buffer.hostVisible, {}});
  active_[buffer.buffer] = index;
  addresses_[address] = {address + buffer.size, index};
}

void CommandTrace::bufferDestroyed(const Buffer& buffer) {
  auto active = active_.find(buffer.buffer);
  if (active == active_.end()) return;
  for (auto it = addresses_.begin(); it != addresses_.end(); ++it) {
    if (it->second.buffer == active->second) { addresses_.erase(it); break; }
  }
  active_.erase(active);
}

int CommandTrace::bufferIndex(const Buffer& buffer) const {
  auto found = active_.find(buffer.buffer);
  if (found == active_.end()) throw std::runtime_error("buffer is outside the recorded graph");
  return found->second;
}

void CommandTrace::upload(const Buffer& buffer, const void* data, VkDeviceSize size, VkDeviceSize offset) {
  auto& target = buffers.at(bufferIndex(buffer));
  if (offset > target.bytes || size > target.bytes - offset || (size && !data))
    throw std::runtime_error("trace upload overflows buffer");
  if (!size) return;
  const auto* bytes = static_cast<const uint8_t*>(data);
  target.uploads.push_back({offset, std::vector<uint8_t>(bytes, bytes + size)});
}

void CommandTrace::zero(const Buffer& buffer) { buffers.at(bufferIndex(buffer)).uploads.clear(); }

void CommandTrace::moduleCreated(VkCudaModuleNV module, const std::string& ptx) {
  moduleIndices_[module] = static_cast<uint32_t>(modules.size());
  modules.push_back(ptx);
}

void CommandTrace::functionCreated(VkCudaFunctionNV function, VkCudaModuleNV module, const char* name) {
  TraceFunction description;
  description.module = moduleIndices_.at(module); description.name = name;
  if (!std::regex_match(description.name, std::regex("[A-Za-z_][A-Za-z0-9_]*")))
    throw std::runtime_error("unsupported PTX entry name");
  const std::regex signature(std::string(R"(\.entry\s+)") + name + R"(\s*\(([^)]*)\))");
  std::smatch match;
  if (!std::regex_search(modules.at(description.module), match, signature))
    throw std::runtime_error("PTX entry signature not found: " + description.name);
  std::istringstream parameters(match[1].str());
  const std::regex parameter(R"(\s*\.param\s+\.([ubsf])(32|64)\s+([A-Za-z_][A-Za-z0-9_]*)\s*)");
  std::string text;
  while (std::getline(parameters, text, ',')) {
    std::smatch item;
    if (!std::regex_match(text, item, parameter)) throw std::runtime_error("unsupported PTX parameter: " + text);
    const uint32_t bytes = static_cast<uint32_t>(std::stoul(item[2].str()) / 8);
    // The checked-in generators explicitly prefix every device-address argument with p.
    const bool address = bytes == 8 && item[1] == "u" && item[3].str().front() == 'p';
    description.parameters.push_back({bytes, address});
  }
  functionIndices_[function] = static_cast<uint32_t>(functions.size());
  functions.push_back(std::move(description));
}

void CommandTrace::pipelineCreated(VkPipeline pipeline, const char* shader, const SpecConstants& constants) {
  pipelines_[pipeline] = {shader, constants};
}

TraceArgument CommandTrace::pointer(uint64_t address) const {
  if (!address) return {0, 8, -1};
  auto next = addresses_.upper_bound(address);
  if (next == addresses_.begin()) throw std::runtime_error("unowned PTX device address");
  --next;
  if (address >= next->second.end) throw std::runtime_error("unowned PTX device address");
  return {address - next->first, 8, next->second.buffer};
}

void CommandTrace::launch(VkCommandBuffer commands, VkCudaFunctionNV function, uint32_t x, uint32_t y, uint32_t z,
                           uint32_t threads, uint32_t sharedBytes, const void* const* parameters, size_t count) {
  if (!commands_ || commands != commands_) return;
  TraceOperation operation;
  operation.kind = TraceOperation::Kind::Ptx;
  operation.function = functionIndices_.at(function);
  operation.grid = {x, y, z}; operation.threads = threads; operation.shared = sharedBytes;
  const auto& types = functions.at(operation.function).parameters;
  if (types.size() != count || (count && !parameters)) throw std::runtime_error("PTX argument count differs from signature");
  for (size_t i = 0; i < count; ++i) {
    if (!parameters[i]) throw std::runtime_error("null PTX argument address");
    TraceArgument argument;
    argument.bytes = types[i].bytes;
    memcpy(&argument.value, parameters[i], argument.bytes);
    if (types[i].pointer) argument = pointer(argument.value);
    operation.arguments.push_back(argument);
  }
  operations.push_back(std::move(operation));
}

void CommandTrace::dispatch(VkCommandBuffer commands, VkPipeline pipeline, const Buffer* const* bindings,
                             const void* push, uint32_t pushBytes, uint32_t x, uint32_t y, uint32_t z) {
  if (!commands_ || commands != commands_) return;
  TraceOperation operation;
  operation.kind = TraceOperation::Kind::Shader;
  const auto& shader = pipelines_.at(pipeline);
  operation.shader = shader.name; operation.constants = shader.constants; operation.grid = {x, y, z};
  for (uint32_t i = 0; i < kGenericBindings; ++i) if (bindings[i]) operation.bindings[i] = bufferIndex(*bindings[i]);
  if (pushBytes) {
    if (!push) throw std::runtime_error("null shader push data");
    const auto* bytes = static_cast<const uint8_t*>(push);
    operation.push.assign(bytes, bytes + pushBytes);
  }
  operations.push_back(std::move(operation));
}

void CommandTrace::clear(VkCommandBuffer commands, const Buffer& buffer, uint32_t value) {
  if (!commands_ || commands != commands_) return;
  TraceOperation operation;
  operation.kind = TraceOperation::Kind::Clear; operation.buffer = bufferIndex(buffer); operation.fill = value;
  operations.push_back(std::move(operation));
}

void CommandTrace::barrier(VkCommandBuffer commands) {
  if (!commands_ || commands != commands_) return;
  if (operations.empty() || operations.back().kind != TraceOperation::Kind::Barrier) operations.emplace_back();
}

void CommandTrace::copy(VkCommandBuffer commands, const Buffer& source, const Buffer& destination, uint64_t bytes) {
  if (!commands_ || commands != commands_) return;
  if (bytes > source.size || bytes > destination.size) throw std::runtime_error("trace copy exceeds buffer");
  TraceOperation operation;
  operation.kind = TraceOperation::Kind::Copy;
  operation.source = bufferIndex(source); operation.buffer = bufferIndex(destination); operation.copyBytes = bytes;
  operations.push_back(std::move(operation));
}

}  // namespace vk
