#pragma once
#include "vk_context.h"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace vk {

struct TraceUpload { uint64_t offset = 0; std::vector<uint8_t> bytes; };
struct TraceBuffer {
  std::string label;
  uint64_t bytes = 0;
  bool hostVisible = false;
  std::vector<TraceUpload> uploads;
};
struct TraceParameter { uint32_t bytes = 0; bool pointer = false; };
struct TraceFunction { uint32_t module = 0; std::string name; std::vector<TraceParameter> parameters; };
struct TraceArgument {
  uint64_t value = 0;  // scalar bits, or byte offset within buffer
  uint32_t bytes = 0;
  int buffer = -1;
};
struct TraceOperation {
  enum class Kind { Ptx, Shader, Clear, Copy, Barrier } kind = Kind::Barrier;
  uint32_t function = 0;
  std::vector<TraceArgument> arguments;
  std::array<uint32_t, 3> grid{1, 1, 1};
  uint32_t threads = 0, shared = 0;
  int buffer = -1;
  uint32_t fill = 0;
  int source = -1;
  uint64_t copyBytes = 0;
  std::string shader;
  SpecConstants constants;
  std::array<int, kGenericBindings> bindings;
  std::vector<uint8_t> push;
  TraceOperation() { bindings.fill(-1); }
};

// Captures one graph recording as relocatable buffer references and kernel
// arguments. The Vulkan objects can be destroyed before a different API replays it.
class CommandTrace {
 public:
  std::vector<TraceBuffer> buffers;
  std::vector<std::string> modules;
  std::vector<TraceFunction> functions;
  std::vector<TraceOperation> operations;

  void record(VkCommandBuffer commands) { commands_ = commands; operations.clear(); }
  void bufferCreated(const Buffer& buffer, VkDeviceAddress address);
  void bufferDestroyed(const Buffer& buffer);
  int bufferIndex(const Buffer& buffer) const;
  void upload(const Buffer& buffer, const void* bytes, VkDeviceSize size, VkDeviceSize offset);
  void zero(const Buffer& buffer);
  void moduleCreated(VkCudaModuleNV module, const std::string& ptx);
  void functionCreated(VkCudaFunctionNV function, VkCudaModuleNV module, const char* name);
  void pipelineCreated(VkPipeline pipeline, const char* shader, const SpecConstants& constants);
  void launch(VkCommandBuffer commands, VkCudaFunctionNV function, uint32_t x, uint32_t y, uint32_t z,
              uint32_t threads, uint32_t sharedBytes, const void* const* parameters, size_t count);
  void dispatch(VkCommandBuffer commands, VkPipeline pipeline, const Buffer* const* bindings,
                const void* push, uint32_t pushBytes, uint32_t x, uint32_t y, uint32_t z);
  void clear(VkCommandBuffer commands, const Buffer& buffer, uint32_t value);
  void copy(VkCommandBuffer commands, const Buffer& source, const Buffer& destination, uint64_t bytes);
  void barrier(VkCommandBuffer commands);

 private:
  VkCommandBuffer commands_ = VK_NULL_HANDLE;
  std::map<VkBuffer, int> active_;
  struct Range { uint64_t end = 0; int buffer = -1; };
  std::map<uint64_t, Range> addresses_;
  std::map<VkCudaModuleNV, uint32_t> moduleIndices_;
  std::map<VkCudaFunctionNV, uint32_t> functionIndices_;
  struct Shader { std::string name; SpecConstants constants; };
  std::map<VkPipeline, Shader> pipelines_;
  TraceArgument pointer(uint64_t address) const;
};

}  // namespace vk
