// Keep the input/image fixture identical to the pinned full-NrPass memory probe.
#define main included_pass_regression_main
#include "pass_regression.cpp"
#undef main
#include "gpu_memory_meter.h"
#include "sha256.h"
#include <cmath>

namespace {
void ownedMemory(NrPass& pass, const char* phase) {
  const auto snapshot = pass.context().memorySnapshot();
  std::cout << snapshot.report(phase);
  uint64_t local = 0, nonlocal = 0;
  for (uint32_t heap = 0; heap < snapshot.heapCount; ++heap) {
    if (snapshot.heapFlags[heap] & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) local += snapshot.heaps[heap].liveBytes;
    else nonlocal += snapshot.heaps[heap].liveBytes;
  }
  const auto owner = [&](vk::MemoryOwner category) { return snapshot.owners[static_cast<size_t>(category)].liveBytes; };
  const auto staging = std::find_if(snapshot.records.begin(), snapshot.records.end(),
                                   [](const auto& record) { return record.label == "staging"; });
  require(staging != snapshot.records.end(), "staging allocation is missing from probe accounting");
  printf("OWNED {\"phase\":\"%s\",\"allocated_bytes\":%llu,\"logical_storage_bytes\":%llu,\"peak_allocated_bytes\":%llu,"
         "\"local_heap_bytes\":%llu,\"nonlocal_heap_bytes\":%llu,\"graph_bytes\":%llu,\"model_bytes\":%llu,"
         "\"kernels_bytes\":%llu,\"pass_bytes\":%llu,\"context_bytes\":%llu,\"staging_bytes\":%llu,\"allocation_count\":%llu,\"free_count\":%llu}\n",
         phase, snapshot.liveBytes, snapshot.liveLogicalBytes, snapshot.peakBytes, local, nonlocal,
         owner(vk::MemoryOwner::Graph), owner(vk::MemoryOwner::Model), owner(vk::MemoryOwner::Kernels),
         owner(vk::MemoryOwner::Pass), owner(vk::MemoryOwner::Context), staging->logicalBytes, snapshot.allocationCount, snapshot.freeCount);
}
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  try {
    const std::string workspace = value(argc, argv, "--workspace", "reuse");
    const std::string reset = value(argc, argv, "--reset", "every");
    require(workspace == "dedicated" || workspace == "reuse", "--workspace must be dedicated or reuse");
    require(reset == "every" || reset == "first", "--reset must be every or first");
    const std::string stagingMiB = value(argc, argv, "--staging-mib");
    if (flag(argc, argv, "--staging-mib")) {
      require(!stagingMiB.empty(), "--staging-mib needs a value");
      require(_putenv_s("DLSS5VK_STAGING_MIB", stagingMiB.c_str()) == 0, "cannot select staging capacity");
    }
    const uint32_t width = std::stoul(value(argc, argv, "--width", "3840"));
    const uint32_t height = std::stoul(value(argc, argv, "--height", "2160"));
    const int frames = std::stoi(value(argc, argv, "--frames", "20"));
    require(width >= 64 && height >= 64 && frames > 5, "invalid memory-probe dimensions or frames");
    const std::string model = value(argc, argv, "--model", "models/nr");
    const std::string shaders = value(argc, argv, "--shaders", "build/shaders");
    const std::string demoShaders = value(argc, argv, "--demo-shaders", "build/nr-memory/demo-shaders");
    const auto input = read(value(argc, argv, "--input"));
    require(input.size() == size_t(width) * height * 16, "input size mismatch");
    const float* proxy = reinterpret_cast<const float*>(input.data());
    vk::Context context;
    GpuMemoryMeter memory(context.physical()); memory.canary(context);
    GpuDevice gpu{(void*)context.instance(), (void*)context.physical(), (void*)context.device(), context.queueFamily(), 0, 0};
    Image color(context, width, height, VK_FORMAT_R16G16B16A16_SFLOAT);
    Image velocity(context, width, height, VK_FORMAT_R32G32B32A32_UINT);
    Image output(context, width, height, VK_FORMAT_R8G8B8A8_UNORM, true);
    std::vector<uint16_t> scene(size_t(width) * height * 4);
    std::vector<uint32_t> motion(scene.size(), 0);
    for (size_t pixel = 0; pixel < size_t(width) * height; ++pixel) {
      for (size_t c = 0; c < 3; ++c) {
        const float code = proxy[pixel * 4 + c];
        float linear = code <= 0.04045f ? code / 12.92f : std::pow((code + 0.055f) / 1.055f, 2.4f);
        if (linear > 0.75f) linear = 0.75f - std::log(std::max(1e-6f, 1.0f - (linear - 0.75f) * 4.0f)) / 5.770780f;
        scene[pixel * 4 + c] = num::f16Bits(linear);
      }
      scene[pixel * 4 + 3] = num::f16Bits(1.0f);
      motion[pixel * 4] = 1; motion[pixel * 4 + 1] = num::f32Bits(0.5f);
    }
    color.upload(scene.data(), scene.size() * 2); velocity.upload(motion.data(), motion.size() * 4);
    VkQueryPool timestamps = context.createTimestampPool(2);
    context.waitIdle(); Sleep(100); memory.sample("baseline");
    printf("RUN candidate source=%s Vulkan %ux%u frames=%d auto_mask=1 reset=%s route=direct workspace=%s\n",
           value(argc, argv, "--source-id", "unrecorded").c_str(), width, height, frames, reset.c_str(), workspace.c_str());
    {
      const auto initStart = std::chrono::steady_clock::now();
      NrPass pass(gpu, width, height, model, shaders, demoShaders, workspace == "reuse");
      const double initMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - initStart).count();
      printf("INIT {\"nr_pass_ms\":%.6f}\n", initMs);
      std::cout << pass.workspaceReport();
      require(pass.workspaceReport().find("route=" + workspace) != std::string::npos, "unexpected workspace route");
      ownedMemory(pass, "after_create"); memory.sample("after_create");
      const float background[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
      NrControls controls; controls.autoMask = true; controls.localTone = controls.localStructure = 1.0f;
      controls.skinStructure = -1.0f; controls.style = 0; controls.intensity = 1.0f;
      std::vector<double> gpuMs;
      vk::MemorySnapshot stable;
      for (int frameIndex = 0; frameIndex < frames; ++frameIndex) {
        if (reset == "every" || frameIndex == 0) pass.resetHistory();
        const auto frame = pass.beginFrame(controls, background);
        auto commands = context.beginCommands();
        vkCmdResetQueryPool(commands, timestamps, 0, 2);
        vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 0);
        pass.recordStamp(commands, frame, NrPass::kFrameStart);
        pass.record(commands, frame, color.info, velocity.info, output.info);
        pass.recordStamp(commands, frame, NrPass::kPresentEnd);
        vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 1);
        context.endAndSubmit(commands, true); output.info.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; pass.endFrame();
        require(!pass.chainTimedOut(), "candidate chained wait timed out");
        const auto stamps = context.readTimestampsMs(timestamps, 2);
        if (frameIndex >= 5) gpuMs.push_back(stamps[1] - stamps[0]);
        const auto snapshot = pass.context().memorySnapshot();
        if (frameIndex == 0) stable = snapshot;
        else require(snapshot.liveBytes == stable.liveBytes && snapshot.allocationCount == stable.allocationCount &&
                     snapshot.freeCount == stable.freeCount, "steady frames changed owned allocations");
        const auto phase = frameIndex == 0 ? std::string("first_frame") : "frame_" + std::to_string(frameIndex + 1);
        memory.sample(phase.c_str());
      }
      ownedMemory(pass, "steady");
      for (int i = 0; i < 3; ++i) { Sleep(100); const auto phase = std::string("steady_") + std::to_string(i); memory.sample(phase.c_str()); }
      std::sort(gpuMs.begin(), gpuMs.end());
      printf("GPU {\"median_ms\":%.6f,\"p95_ms\":%.6f,\"samples\":%zu,\"warmup_frames\":5}\n",
             gpuMs[gpuMs.size() / 2], gpuMs[(gpuMs.size() * 95 + 99) / 100 - 1], gpuMs.size());
      const auto pixels = output.download(); bool nonzero = false;
      for (size_t i = 0; i < pixels.size(); i += 4) nonzero |= pixels[i] != 0 || pixels[i + 1] != 0 || pixels[i + 2] != 0;
      require(nonzero, "candidate output is blank"); puts("OUTPUT CHECK nonblank PASS");
      printf("OUTPUT {\"sha256\":\"%s\"}\n", sha256Hex(pixels.data(), pixels.size()).c_str());
      context.waitIdle();
    }
    Sleep(100); memory.sample("after_release");
    vkDestroyQueryPool(context.device(), timestamps, nullptr);
    require(vk::Context::validationErrors() == 0, "Vulkan validation reported errors");
    puts("MEMORY PROBE PASS"); return 0;
  } catch (const std::exception& e) { fprintf(stderr, "ERROR: %s\n", e.what()); return 1; }
}
