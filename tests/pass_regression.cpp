// Headless renderer interop: fixed scene/motion inputs, history, toggles and resize.
#include "nr_pass.h"
#include "numeric.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

namespace {
void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
std::string value(int argc, char** argv, const std::string& key, const std::string& fallback = "") {
  for (int i = 1; i + 1 < argc; ++i) if (key == argv[i]) return argv[i + 1];
  return fallback;
}
bool flag(int argc, char** argv, const std::string& key) {
  for (int i = 1; i < argc; ++i) if (key == argv[i]) return true;
  return false;
}
std::vector<uint8_t> read(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  require(bool(in), "cannot read " + path.string());
  std::vector<uint8_t> bytes((size_t)in.tellg());
  in.seekg(0);
  in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  require(bool(in), "short read: " + path.string());
  return bytes;
}
void compareOrWrite(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, bool record) {
  if (record) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    require(bool(out), "cannot write " + path.string());
  } else {
    require(read(path) == bytes, "bit mismatch: " + path.string());
  }
}

void barrier(VkCommandBuffer commands, VkImage image, VkImageLayout before, VkImageLayout after) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = before == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.oldLayout = before; b.newLayout = after;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &b);
}

struct Image {
  vk::Context& context;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  GpuImage info;
  Image(vk::Context& ctx, uint32_t width, uint32_t height, VkFormat format, bool storage = false) : context(ctx) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format; ci.extent = {width, height, 1};
    ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (storage) ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VK_CHECK(vkCreateImage(ctx.device(), &ci, nullptr, &image));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(ctx.device(), image, &req);
    VkPhysicalDeviceMemoryProperties props; vkGetPhysicalDeviceMemoryProperties(ctx.physical(), &props);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
      if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
    require(type != UINT32_MAX, "no memory type for test image");
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size; alloc.memoryTypeIndex = type;
    VK_CHECK(vkAllocateMemory(ctx.device(), &alloc, nullptr, &memory));
    VK_CHECK(vkBindImageMemory(ctx.device(), image, memory, 0));
    info = {(uint64_t)(uintptr_t)image, (uint32_t)format, VK_IMAGE_LAYOUT_UNDEFINED, width, height};
    info.usage = ci.usage;
  }
  ~Image() {
    vkDestroyImage(context.device(), image, nullptr);
    vkFreeMemory(context.device(), memory, nullptr);
  }
  void upload(const void* data, size_t size) {
    auto staging = context.createBuffer(size, true, "test image upload");
    memcpy(staging.mapped, data, size);
    auto commands = context.beginCommands();
    barrier(commands, image, (VkImageLayout)info.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {info.width, info.height, 1};
    vkCmdCopyBufferToImage(commands, staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    context.endAndSubmit(commands, true);
    info.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    context.destroyBuffer(staging);
  }
  std::vector<uint8_t> download() {
    auto staging = context.createBuffer((VkDeviceSize)info.width * info.height * 4, true, "test output readback");
    auto commands = context.beginCommands();
    barrier(commands, image, (VkImageLayout)info.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {info.width, info.height, 1};
    vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &region);
    barrier(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, (VkImageLayout)info.layout);
    context.endAndSubmit(commands, true);
    const auto* start = static_cast<const uint8_t*>(staging.mapped);
    std::vector<uint8_t> bytes(start, start + staging.size);
    context.destroyBuffer(staging);
    return bytes;
  }
};

PFN_vkUpdateDescriptorSets updateSets;
PFN_vkCmdCopyImage copyImage;
PFN_vkCreateImageView createView;
PFN_vkDestroyImageView destroyView;
std::map<VkImageView, VkImage> liveViews;
VkBuffer features = VK_NULL_HANDLE;
VkImageView histories[2]{};
uint32_t setUpdates = 0, copies = 0;
VKAPI_ATTR VkResult VKAPI_CALL observeCreateView(VkDevice device, const VkImageViewCreateInfo* info,
                                                 const VkAllocationCallbacks* callbacks, VkImageView* view) {
  VkResult result = createView(device, info, callbacks, view);
  if (result == VK_SUCCESS) liveViews[*view] = info->image;
  return result;
}
VKAPI_ATTR void VKAPI_CALL observeDestroyView(VkDevice device, VkImageView view, const VkAllocationCallbacks* callbacks) {
  liveViews.erase(view);
  destroyView(device, view, callbacks);
}
VKAPI_ATTR void VKAPI_CALL observeSets(VkDevice device, uint32_t count, const VkWriteDescriptorSet* writes,
                                       uint32_t copyCount, const VkCopyDescriptorSet* descriptorCopies) {
  if (count == 7 && writes[0].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && writes[3].dstBinding == 3) {
    if (setUpdates % 2 == 0) features = writes[3].pBufferInfo->buffer;
    histories[(setUpdates / 2) % 2] = writes[6].pImageInfo->imageView;
    ++setUpdates;
  }
  updateSets(device, count, writes, copyCount, descriptorCopies);
}
VKAPI_ATTR void VKAPI_CALL observeCopy(VkCommandBuffer commands, VkImage src, VkImageLayout srcLayout,
                                       VkImage dst, VkImageLayout dstLayout, uint32_t count, const VkImageCopy* regions) {
  ++copies;
  copyImage(commands, src, srcLayout, dst, dstLayout, count, regions);
}

// Sample the actual history image into losslessly packed binary16 words for comparison.
class HistoryReader {
 public:
  explicit HistoryReader(vk::Context& context) : context_(context) {
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 2; sl.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(context.device(), &sl, nullptr, &setLayout_));
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1; pl.pSetLayouts = &setLayout_; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &push;
    VK_CHECK(vkCreatePipelineLayout(context.device(), &pl, nullptr, &layout_));
    auto shader = context.loadShaderModule("build/tests/history_copy.spv");
    VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr};
    pi.layout = layout_;
    VK_CHECK(vkCreateComputePipelines(context.device(), VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline_));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1; dp.poolSizeCount = 2; dp.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(context.device(), &dp, nullptr, &pool_));
    VkDescriptorSetAllocateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ds.descriptorPool = pool_; ds.descriptorSetCount = 1; ds.pSetLayouts = &setLayout_;
    VK_CHECK(vkAllocateDescriptorSets(context.device(), &ds, &set_));
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VK_CHECK(vkCreateSampler(context.device(), &si, nullptr, &sampler_));
  }
  ~HistoryReader() {
    vkDestroySampler(context_.device(), sampler_, nullptr);
    vkDestroyPipeline(context_.device(), pipeline_, nullptr);
    vkDestroyPipelineLayout(context_.device(), layout_, nullptr);
    vkDestroyDescriptorPool(context_.device(), pool_, nullptr);
    vkDestroyDescriptorSetLayout(context_.device(), setLayout_, nullptr);
  }
  std::vector<uint8_t> read(VkImageView view, uint32_t width, uint32_t height) {
    auto result = context_.createBuffer((VkDeviceSize)width * height * 8, false, "test history readback");
    VkDescriptorImageInfo image{sampler_, view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo buffer{result.buffer, 0, result.size};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = set_;
      writes[i].dstBinding = i; writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo = &image;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo = &buffer;
    vkUpdateDescriptorSets(context_.device(), 2, writes, 0, nullptr);
    auto commands = context_.beginCommands();
    VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &memory, 0, nullptr, 0, nullptr);
    uint32_t size[] = {width, height};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_, 0, nullptr);
    vkCmdPushConstants(commands, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(size), size);
    vkCmdDispatch(commands, (width + 7) / 8, (height + 7) / 8, 1);
    context_.endAndSubmit(commands, true);
    auto bytes = context_.download(result, result.size);
    context_.destroyBuffer(result);
    return bytes;
  }
 private:
  vk::Context& context_;
  VkDescriptorSetLayout setLayout_{};
  VkPipelineLayout layout_{};
  VkPipeline pipeline_{};
  VkDescriptorPool pool_{};
  VkDescriptorSet set_{};
  VkSampler sampler_{};
};
}  // namespace

int main(int argc, char** argv) {
  try {
    const std::filesystem::path reference = value(argc, argv, "--reference", "tmp/optimization/pass-baseline");
    const bool record = flag(argc, argv, "--record");
    const bool direct = flag(argc, argv, "--expect-direct");
    const bool storage = !flag(argc, argv, "--copy-target");
    const bool mixed = flag(argc, argv, "--mixed-target");
    const bool unknown = flag(argc, argv, "--unknown-usage");
    const bool benchmark = flag(argc, argv, "--benchmark");
    const int frames = benchmark ? std::stoi(value(argc, argv, "--frames", "300")) : 12;
    const int warmup = std::stoi(value(argc, argv, "--warmup", "30"));
    require(frames > 0 && warmup >= 0 && !(benchmark && record), "invalid benchmark options");
    require(!record || !std::filesystem::exists(reference / "output-0.bin"), "reference already exists");
    if (record) std::filesystem::create_directories(reference);
    uint32_t width = std::stoul(value(argc, argv, "--width", "193"));
    uint32_t height = std::stoul(value(argc, argv, "--height", "129"));
    vk::Context context;
    GpuDevice gpu{(void*)context.instance(), (void*)context.physical(), (void*)context.device(), context.queueFamily(), 0, 0};
    auto color = std::make_unique<Image>(context, width, height, VK_FORMAT_R16G16B16A16_SFLOAT);
    auto velocity = std::make_unique<Image>(context, width, height, VK_FORMAT_R32G32B32A32_UINT);
    auto output = std::make_unique<Image>(context, width, height, VK_FORMAT_R8G8B8A8_UNORM, storage);
    auto alternate = std::make_unique<Image>(context, width, height, VK_FORMAT_R8G8B8A8_UNORM, storage && !mixed);
    NrPass pass(gpu, width, height, "models/nr", "build/shaders", "build/tests/demo-shaders");
    updateSets = vkUpdateDescriptorSets; vkUpdateDescriptorSets = observeSets;
    copyImage = vkCmdCopyImage; vkCmdCopyImage = observeCopy;
    createView = vkCreateImageView; vkCreateImageView = observeCreateView;
    destroyView = vkDestroyImageView; vkDestroyImageView = observeDestroyView;
    std::unique_ptr<HistoryReader> historyReader;
    if (!benchmark) historyReader = std::make_unique<HistoryReader>(context);
    std::vector<NrTimings> samples;
    std::vector<double> cpuSamples;
    for (int frameIndex = 0; frameIndex < (benchmark ? warmup + frames + 2 : frames); ++frameIndex) {
      if (!benchmark && frameIndex == 10) {
        width += 8; height += 16;
        pass.resize(width, height);
        color = std::make_unique<Image>(context, width, height, VK_FORMAT_R16G16B16A16_SFLOAT);
        velocity = std::make_unique<Image>(context, width, height, VK_FORMAT_R32G32B32A32_UINT);
        output = std::make_unique<Image>(context, width, height, VK_FORMAT_R8G8B8A8_UNORM, storage);
        alternate = std::make_unique<Image>(context, width, height, VK_FORMAT_R8G8B8A8_UNORM, storage && !mixed);
      }
      if (!benchmark || frameIndex == 0) {
      std::vector<uint16_t> scene((size_t)width * height * 4);
      std::vector<uint32_t> motion(scene.size());
      for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
        const size_t p = ((size_t)y * width + x) * 4;
        scene[p] = num::f16Bits(float((x + frameIndex * 3) % width) / width * 3.0f);
        scene[p + 1] = num::f16Bits(float(y) / height);
        scene[p + 2] = num::f16Bits(((x / 13 + y / 7) % 2) ? 0.125f : 1.125f);
        scene[p + 3] = num::f16Bits(1.0f);
        motion[p] = y < height / 4 ? 0 : 1;
        motion[p + 1] = y < height / 4 ? 0 : num::f32Bits(0.5f);
        motion[p + 2] = num::f32Bits(frameIndex == 2 && x < width / 3 ? 3.0f : (frameIndex ? 2.0f / width : 0.0f));
        motion[p + 3] = num::f32Bits(frameIndex % 2 ? -1.0f / height : 0.0f);
      }
      color->upload(scene.data(), scene.size() * 2);
      velocity->upload(motion.data(), motion.size() * 4);
      }
      NrControls controls;
      controls.enabled = benchmark || (frameIndex != 3 && frameIndex != 4);
      controls.temporal = benchmark || frameIndex != 6;
      controls.localTone = 0.75013f; controls.localStructure = 0.3517f;
      controls.style = benchmark ? 0 : frameIndex % 3;
      controls.autoMask = benchmark || frameIndex % 2 == 0;
      if (!benchmark && frameIndex == 7) pass.resetHistory();
      float background[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.003f, 0, 0, 1};
      const auto prepareStart = std::chrono::steady_clock::now();
      auto frame = pass.beginFrame(controls, background);
      Image& target = !benchmark && (frameIndex == 8 || frameIndex == 9) ? *alternate : *output;
      if (unknown) target.info.usage = 0;
      copies = 0;
      auto commands = context.beginCommands();
      pass.recordStamp(commands, frame, NrPass::kFrameStart);
      pass.record(commands, frame, color->info, velocity->info, target.info);
      pass.recordStamp(commands, frame, NrPass::kPresentEnd);
      const double prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepareStart).count();
      context.endAndSubmit(commands, true);
      target.info.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      pass.endFrame();
      require(!pass.chainTimedOut(), "chained GPU wait timed out");
      if (direct) require(copies == ((target.info.usage & VK_IMAGE_USAGE_STORAGE_BIT) ? 0u : 1u), "incorrect output copy route");
      if (!storage || unknown) require(copies == 1, "non-storage or unknown target must retain the copy fallback");
      if (mixed && frameIndex == 8) {
        for (const auto& [view, image] : liveViews)
          require(image != output->image, "direct output view retained after switching to copy fallback");
      }
      if (benchmark) {
        if (frameIndex >= warmup + 2) { samples.push_back(pass.timings()); cpuSamples.push_back(prepareMs); }
        continue;
      }
      const std::string suffix = std::to_string(frameIndex) + ".bin";
      compareOrWrite(reference / ("output-" + suffix), target.download(), record);
      compareOrWrite(reference / ("history-" + suffix), historyReader->read(histories[frame.parity], width, height), record);
      if (controls.enabled) {
        const auto geometry = nr::Geometry::fromValid(width, height);
        const size_t count = (size_t)geometry.fullWidth * geometry.fullHeight * 16;
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(context.device(), features, &req);
        const bool half = req.size < count * 4;
        vk::Buffer buffer; buffer.buffer = features; buffer.size = req.size;
        auto bytes = context.download(buffer, count * (half ? 2 : 4));
        const auto path = reference / ("features-" + suffix);
        if (half && !record) {
          const auto old = read(path);
          require(old.size() == count * 4, "expected original F32 features");
          std::vector<uint8_t> rounded(count * 2);
          for (size_t i = 0; i < count; ++i) {
            float f; memcpy(&f, old.data() + i * 4, 4);
            uint16_t h = num::f16Bits(f); memcpy(rounded.data() + i * 2, &h, 2);
          }
          require(bytes == rounded, "preprocess FP16 bit pattern mismatch at frame " + std::to_string(frameIndex));
        } else {
          require(!record || !half, "baseline must use the original F32 preprocess");
          compareOrWrite(path, bytes, record);
        }
      }
      std::cout << "frame=" << frameIndex << (record ? " captured, copies=" : " output/history/features exact, copies=") << copies << '\n';
    }
    require(vk::Context::validationErrors() == 0, "Vulkan validation reported errors");
    if (benchmark) {
      auto report = [](const char* label, std::vector<double> times) {
        std::sort(times.begin(), times.end());
        std::cout << label << ",median_ms=" << times[times.size() / 2]
                  << ",p95_ms=" << times[(times.size() * 95 + 99) / 100 - 1] << '\n';
      };
      auto metric = [&](const char* label, double NrTimings::*member) {
        std::vector<double> times;
        for (const auto& t : samples) times.push_back(t.*member);
        report(label, std::move(times));
      };
      std::cout << "BENCH " << width << 'x' << height << " frames=" << samples.size()
                << " warmup=" << warmup << " route=" << (storage && !unknown ? "direct" : "copy") << '\n';
      report("cpu_prepare", cpuSamples);
      metric("motion_and_preprocess", &NrTimings::preprocessMs);
      metric("network", &NrTimings::networkMs);
      metric("composite", &NrTimings::compositeMs);
      metric("output_ready", &NrTimings::outputMs);
      metric("total_nr", &NrTimings::frameMs);
      return 0;
    }
    pass.saveOutput("tmp/optimization/pass-capture.ppm");
    const auto ppm = read("tmp/optimization/pass-capture.ppm");
    const auto rgba = read(reference / "output-11.bin");
    const std::string header = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    require(ppm.size() == header.size() + (size_t)width * height * 3 &&
            std::equal(header.begin(), header.end(), ppm.begin()), "invalid output capture");
    for (size_t i = 0; i < (size_t)width * height; ++i)
      for (size_t c = 0; c < 3; ++c)
        require(ppm[header.size() + i * 3 + c] == rgba[i * 4 + c], "output capture differs from renderer target");
    std::cout << (record ? "RECORDED" : "PASS") << " 12 temporal frames including off/on, reset, output rotation and resize\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
