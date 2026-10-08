#include "nr_graph.h"

#include <limits>
#include <algorithm>
#include <sstream>

namespace nr {

std::string activationKey(const std::string& label, uint32_t rows, uint32_t channels, Format format) {
  return label + "/" + std::to_string(rows) + "x" + std::to_string(channels) + "/" + std::to_string((int)format);
}

std::vector<WorkspaceRequest> graphWorkspaceRequests(const Geometry& g, bool fp16Head, uint32_t deferMax) {
  using D = WorkspaceDomain;
  auto domain = [](D value) { return static_cast<uint32_t>(value); };
  auto pixels = [](uint32_t width, uint32_t height) {
    const uint64_t count = uint64_t(width) * height;
    if (!count || count > std::numeric_limits<uint32_t>::max() - 63u)
      throw std::runtime_error("unsupported workspace geometry");
    return static_cast<uint32_t>(count);
  };
  const uint32_t fullRows = pixels(g.fullWidth, g.fullHeight);
  uint32_t rows[6];
  for (int i = 0; i < 6; ++i) rows[i] = pixels(g.levels[i].width, g.levels[i].height);
  std::vector<WorkspaceRequest> requests;
  auto add = [&](const std::string& label, uint32_t count, uint32_t channels, Format format, D first, D last) {
    requests.push_back({activationKey(label, count, channels, format), uint64_t(alignRows(count)) * channels * formatBytes(format),
                         domain(first), domain(last)});
  };
  auto expert = [&](const std::string& label, uint32_t count, uint32_t channels, D stage) {
    if (channels == 32) return;
    add(label + " FFN quantized", count, channels, Format::E4, stage, stage);
    if (channels <= deferMax) add(label + " FFN quantized B", count, channels, Format::E4, stage, stage);
    add(label + " attended", count, channels, Format::E4, stage, stage);
  };
  auto split = [&](const std::string& label, D stage) {
    for (const char* suffix : {" split branch", " split layer0", " split residual", " split attended"})
      add(label + suffix, rows[4], 512, Format::E4, stage, stage);
  };

  add("retained full block0", fullRows, 32, Format::E4, D::Pre, D::Post);
  add("block0 downsample", rows[0], 32, Format::E4, D::Pre, D::Decoder32);
  add("encoder 32 state", rows[0], 32, Format::E4, D::Encoder32, D::Encoder32);
  add("encoder 32 downsample", rows[1], 32, Format::E4, D::Encoder32, D::ToEncoder64);
  add("encoder 64 input", rows[1], 64, Format::E4, D::ToEncoder64, D::Decoder64);
  for (uint32_t level = 1; level <= 3; ++level) {
    const uint32_t channels = 32u << level;
    const D stage = static_cast<D>(2 + 2 * level), transition = static_cast<D>(3 + 2 * level);
    const D nextConsumer = static_cast<D>(20 - 2 * level);
    const std::string label = "encoder " + std::to_string(channels);
    add(label + " state", rows[level], channels, Format::E4, stage, stage);
    add(label + " raw transition", rows[level], channels, Format::F16, stage, transition);
    expert(label, rows[level], channels, stage);
    add(label + " downsample", rows[level + 1], channels, Format::E4, transition, transition);
    add(label + " next stage", rows[level + 1], channels * 2, Format::E4, transition, nextConsumer);
  }
  add("encoder 512 state", rows[4], 512, Format::E4, D::Encoder512, D::Encoder512);
  add("encoder 512 raw transition", rows[4], 512, Format::F16, D::Encoder512, D::ToVit);
  split("encoder 512", D::Encoder512);
  add("encoder 512 pooled", rows[5], 512, Format::E4, D::ToVit, D::ToVit);
  add("ViT state", rows[5], 1024, Format::E4, D::ToVit, D::ToDecoder512);
  add("ViT FFN 4096", rows[5], 4096, Format::E4, D::Vit, D::Vit);
  add("ViT FFN residual", rows[5], 1024, Format::E4, D::Vit, D::Vit);
  add("ViT QKV", rows[5], 3072, Format::F16, D::Vit, D::Vit);
  add("ViT normalized QKV", alignRows(rows[5]), 3072, Format::E4, D::Vit, D::Vit);
  add("ViT attended", rows[5], 1024, Format::E4, D::Vit, D::Vit);
  add("decoder 512 projection", rows[5], 512, Format::F16, D::ToDecoder512, D::ToDecoder512);
  add("decoder 512 skip merge", rows[4], 512, Format::E4, D::ToDecoder512, D::ToDecoder256);
  add("decoder 512 state", rows[4], 512, Format::E4, D::Decoder512, D::Decoder512);
  split("decoder 512", D::Decoder512);
  for (int level = 3; level >= 0; --level) {
    const uint32_t channels = 32u << level;
    const D stage = static_cast<D>(22 - 2 * level), transition = static_cast<D>(21 - 2 * level);
    const D nextConsumer = level == 0 ? D::Post : static_cast<D>(23 - 2 * level);
    const std::string label = "decoder " + std::to_string(channels);
    add(label + " projection", rows[level + 1], channels, Format::F16, transition, level == 0 ? stage : transition);
    add(label + " skip merge", rows[level], channels, Format::E4, transition, nextConsumer);
    add(label + " state", rows[level], channels, Format::E4, stage, stage);
    expert(label, rows[level], channels, stage);
  }
  add("RGBA neural head", fullRows, 4, fp16Head ? Format::F16 : Format::F32, D::Post, D::FrameEnd);
  return requests;
}

std::vector<WorkspaceRequest> Graph::makeWorkspaceRequests() const {
  return graphWorkspaceRequests(geometry_, options_.fp16Head, routes_.deferMax);
}

bool Graph::workspaceRouteSupported() const {
  return options_.fusedBlocks && !options_.captureBoundaries && !options_.captureIntermediates &&
         routes_.fusePre && routes_.fusePool && routes_.fuseUpres && routes_.fusePost &&
         Kernels::ptxGemmEnabled() && Kernels::ptxFfnEnabled() && Kernels::ptxQkvEnabled() && Kernels::ptxBlock32Enabled();
}

void Graph::prepareWorkspace() {
  if (workspacePrepared_) return;
  if (!options_.reuseWorkspace || !workspaceRouteSupported()) {
    workspacePrepared_ = true;
    return;
  }
  auto requests = makeWorkspaceRequests();
  auto plan = planWorkspace(requests);
  validateWorkspacePlan(requests, plan);
  std::vector<std::unique_ptr<WorkspaceStorage>> slots;
  for (size_t index = 0; index < plan.capacities.size(); ++index) {
    auto slot = std::make_unique<WorkspaceStorage>(context_, "graph workspace " + std::to_string(index));
    slot->buffer = context_.createBuffer(plan.capacities[index], false, slot->label.c_str(), 0, vk::MemoryOwner::Graph);
    context_.fillZero(slot->buffer);
    slots.push_back(std::move(slot));
  }
  workspaceRequests_ = std::move(requests);
  workspacePlan_ = std::move(plan);
  workspaceSlots_ = std::move(slots);
  workspaceActive_ = workspacePrepared_ = true;
}

Activation* Graph::allocateInternal(const std::string& label, uint32_t rows, uint32_t channels, Format format) {
  if (!workspaceActive_) return allocate(label, rows, channels, format);
  const auto key = activationKey(label, rows, channels, format);
  const auto request = std::find_if(workspaceRequests_.begin(), workspaceRequests_.end(),
                                  [&](const auto& entry) { return entry.key == key; });
  if (request == workspaceRequests_.end()) throw std::runtime_error("unplanned workspace view " + key);
  if (workspaceDomain_ < int32_t(request->firstDomain) || workspaceDomain_ > int32_t(request->lastDomain))
    throw std::runtime_error("workspace view allocated outside its domain: " + key);
  if (!usedThisRecord_.insert(key).second) throw std::runtime_error("duplicate activation label " + label);
  auto existing = workspaceViews_.find(key);
  if (existing != workspaceViews_.end()) return existing->second.get();
  auto view = std::make_unique<Activation>();
  view->label = label; view->format = format; view->rows = rows; view->allocRows = alignRows(rows); view->channels = channels;
  view->buffer = workspaceSlots_.at(workspacePlan_.slotFor.at(key))->buffer;
  const uint64_t bytes = uint64_t(view->allocRows) * channels * formatBytes(format);
  if (bytes != request->bytes || bytes > view->buffer.size) throw std::runtime_error("workspace view capacity mismatch: " + key);
  view->buffer.size = bytes;   // Views expose their logical extent; only the slot owns memory.
  view->buffer.label = view->label.c_str();
  auto* result = view.get();
  workspaceViews_.emplace(key, std::move(view));
  return result;
}

void Graph::enterWorkspaceDomain(VkCommandBuffer commands, uint32_t domain) {
  if (!workspaceActive_) return;
  if (int32_t(domain) <= workspaceDomain_ || hasPendingProjection_)
    throw std::runtime_error("invalid workspace handover or unfinished deferred projection");
  if (options_.poisonWorkspace) {
    // The transfer must wait for the previous occupant's final reads, not just writes.
    context_.transferBarrier(commands);
    for (const auto& request : workspaceRequests_)
      if (request.firstDomain == domain)
        context_.clearBuffer(commands, workspaceSlots_[workspacePlan_.slotFor.at(request.key)]->buffer, 0x7fffffffu);
    context_.transferBarrier(commands);
  } else if (workspaceDomain_ >= 0) {
    context_.computeBarrier(commands);
  }
  workspaceDomain_ = int32_t(domain);
}

std::string Graph::workspaceReport() const {
  std::ostringstream out;
  out << "workspace route=" << (workspaceActive_ ? "reuse" : options_.reuseWorkspace ? "dedicated-fallback" : "dedicated")
      << " logical_bytes=" << workspacePlan_.logicalBytes << " storage_bytes=" << workspacePlan_.storageBytes
      << " peak_live_bytes=" << workspacePlan_.peakLiveBytes << " slots=" << workspaceSlots_.size();
  if (options_.reuseWorkspace && !workspaceActive_) {
    out << " reason=" << (!workspacePrepared_ ? "not-recorded" : options_.captureBoundaries || options_.captureIntermediates
                          ? "capture" : !options_.fusedBlocks ? "unfused" : "unsupported-kernel-route");
  }
  out << '\n';
  for (size_t index = 0; index < workspaceSlots_.size(); ++index)
    out << "slot=" << index << " capacity=" << workspacePlan_.capacities[index]
        << " address=" << context_.deviceAddress(workspaceSlots_[index]->buffer) << '\n';
  for (const auto& request : workspaceRequests_)
    out << "view=" << request.key << " slot=" << workspacePlan_.slotFor.at(request.key) << " bytes=" << request.bytes
        << " first_domain=" << request.firstDomain << " last_domain=" << request.lastDomain << '\n';
  return out.str();
}

}  // namespace nr
