#include "compute_trace.h"
#include "sha256.h"

#include <cstdio>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F fn, const char* text) {
  bool rejected = false;
  try { fn(); } catch (const std::exception&) { rejected = true; }
  require(rejected, text);
}
}

int main() try {
  const uint8_t abc[] = {'a', 'b', 'c'};
  require(sha256Matches(abc, 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "lowercase manifest hash");
  require(sha256Matches(abc, 3, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"), "uppercase manifest hash");
  require(!sha256Matches(abc, 3, "ca7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "changed model is rejected");
  require(!sha256Matches(abc, 3, "ba78"), "short hash is rejected");

  vk::CommandTrace trace;
  vk::Buffer input;
  input.buffer = reinterpret_cast<VkBuffer>(uintptr_t(1)); input.size = 256; input.label = "input";
  trace.bufferCreated(input, 0x1000);
  const uint32_t value = 0x12345678;
  trace.upload(input, &value, sizeof(value), 12);
  const auto commands = reinterpret_cast<VkCommandBuffer>(uintptr_t(2));
  trace.record(commands);
  const auto module = reinterpret_cast<VkCudaModuleNV>(uintptr_t(3));
  const auto function = reinterpret_cast<VkCudaFunctionNV>(uintptr_t(4));
  trace.moduleCreated(module, ".visible .entry run(.param .u64 pInput, .param .u32 count, .param .f32 scale) { ret; }");
  trace.functionCreated(function, module, "run");
  uint64_t pointer = 0x1010;
  uint32_t count = 7;
  float scale = 1.5f;
  const void* args[] = {&pointer, &count, &scale};
  trace.launch(commands, function, 2, 3, 4, 128, 512, args, 3);
  require(trace.operations.size() == 1, "one captured launch");
  const auto& launch = trace.operations[0];
  require(launch.kind == vk::TraceOperation::Kind::Ptx, "PTX operation kind");
  require(launch.arguments[0].buffer == 0 && launch.arguments[0].value == 16, "device pointer becomes buffer plus offset");
  require(launch.arguments[1].buffer == -1 && launch.arguments[1].value == 7 && launch.arguments[1].bytes == 4, "integer argument value");
  require(launch.arguments[2].value == 0x3fc00000, "float argument bits");
  require(launch.grid[0] == 2 && launch.grid[1] == 3 && launch.grid[2] == 4 && launch.threads == 128 && launch.shared == 512, "launch geometry");
  require(trace.buffers[0].uploads.size() == 1 && trace.buffers[0].uploads[0].offset == 12 && trace.buffers[0].uploads[0].bytes[0] == 0x78, "weight bytes and offset");
  count = 9;
  require(launch.arguments[1].value == 7, "capture owns parameter bytes");
  rejects([&] { trace.launch(commands, function, 1, 1, 1, 32, 0, args, 2); }, "argument count mismatch rejected");
  pointer = 0x2000;
  rejects([&] { trace.launch(commands, function, 1, 1, 1, 32, 0, args, 3); }, "unknown device pointer rejected");
  trace.bufferDestroyed(input);
  pointer = 0x1010;
  rejects([&] { trace.launch(commands, function, 1, 1, 1, 32, 0, args, 3); }, "destroyed buffer rejected");
  trace.bufferCreated(input, 0x1000);
  trace.launch(commands, function, 1, 1, 1, 32, 0, args, 3);
  require(trace.operations.back().arguments[0].buffer == 1, "reused address has a new allocation identity");
  trace.clear(commands, input, 0);
  trace.barrier(commands);
  require(trace.operations[trace.operations.size() - 2].kind == vk::TraceOperation::Kind::Clear, "counter clear captured");
  require(trace.operations.back().kind == vk::TraceOperation::Kind::Barrier, "ordering barrier captured");
  vk::Buffer output;
  output.buffer = reinterpret_cast<VkBuffer>(uintptr_t(5)); output.size = 128;
  trace.bufferCreated(output, 0x2000);
  trace.copy(commands, input, output, 64);
  const auto& copy = trace.operations.back();
  require(copy.kind == vk::TraceOperation::Kind::Copy && copy.source == 1 && copy.buffer == 2 && copy.copyBytes == 64,
          "copy retains source, destination, and byte count");
  rejects([&] { trace.copy(commands, input, output, 129); }, "oversized copy rejected");
  const auto size = trace.operations.size();
  trace.copy(reinterpret_cast<VkCommandBuffer>(uintptr_t(9)), input, output, 64);
  require(trace.operations.size() == size, "unrelated command list ignored");
  puts("PASS graph trace relocation, argument ownership, buffer lifetime, and model hash validation");
  return 0;
} catch (const std::exception& error) {
  fprintf(stderr, "FAIL: %s\n", error.what());
  return 1;
}
