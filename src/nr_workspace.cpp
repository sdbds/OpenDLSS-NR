#include "nr_workspace.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace nr {
namespace {
uint64_t addBytes(uint64_t a, uint64_t b) {
  if (b > std::numeric_limits<uint64_t>::max() - a) throw std::runtime_error("workspace byte count overflow");
  return a + b;
}

struct Totals { uint64_t logical = 0, peak = 0; };
Totals requestTotals(const std::vector<WorkspaceRequest>& requests) {
  std::set<std::string> keys;
  struct Change { uint64_t add = 0, remove = 0; };
  std::map<uint64_t, Change> changes;
  Totals totals;
  for (const auto& request : requests) {
    if (request.key.empty() || !request.bytes || request.firstDomain > request.lastDomain ||
        !keys.insert(request.key).second) throw std::runtime_error("invalid or duplicate workspace request");
    totals.logical = addBytes(totals.logical, request.bytes);
    auto& begin = changes[request.firstDomain];
    begin.add = addBytes(begin.add, request.bytes);
    auto& end = changes[uint64_t(request.lastDomain) + 1];
    end.remove = addBytes(end.remove, request.bytes);
  }
  uint64_t live = 0;
  for (const auto& [domain, change] : changes) {
    live -= change.remove;
    live = addBytes(live, change.add);
    totals.peak = std::max(totals.peak, live);
  }
  return totals;
}
}  // namespace

void validateWorkspacePlan(const std::vector<WorkspaceRequest>& requests, const WorkspacePlan& plan) {
  const auto totals = requestTotals(requests);
  uint64_t storage = 0;
  for (uint64_t capacity : plan.capacities) {
    if (!capacity) throw std::runtime_error("empty workspace slot");
    storage = addBytes(storage, capacity);
  }
  if (plan.slotFor.size() != requests.size() || plan.logicalBytes != totals.logical ||
      plan.peakLiveBytes != totals.peak || plan.storageBytes != storage || storage < totals.peak)
    throw std::runtime_error("inconsistent workspace plan totals");
  for (size_t i = 0; i < requests.size(); ++i) {
    const auto& a = requests[i];
    auto slot = plan.slotFor.find(a.key);
    if (slot == plan.slotFor.end() || slot->second >= plan.capacities.size() ||
        plan.capacities[slot->second] < a.bytes) throw std::runtime_error("missing or undersized workspace slot");
    for (size_t j = 0; j < i; ++j) {
      const auto& b = requests[j];
      if (slot->second != plan.slotFor.at(b.key)) continue;
      if (a.dedicated || b.dedicated || (a.firstDomain <= b.lastDomain && b.firstDomain <= a.lastDomain))
        throw std::runtime_error("overlapping workspace lifetimes: " + a.key + " and " + b.key);
    }
  }
}

WorkspacePlan planWorkspace(const std::vector<WorkspaceRequest>& requests) {
  WorkspacePlan plan;
  const auto totals = requestTotals(requests);
  plan.logicalBytes = totals.logical;
  plan.peakLiveBytes = totals.peak;
  std::vector<const WorkspaceRequest*> ordered;
  for (const auto& request : requests) ordered.push_back(&request);
  std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
    if (a->firstDomain != b->firstDomain) return a->firstDomain < b->firstDomain;
    if (a->bytes != b->bytes) return a->bytes > b->bytes;
    return a->key < b->key;
  });
  std::vector<uint32_t> lastDomains;
  std::vector<bool> dedicated;
  for (const auto* request : ordered) {
    size_t selected = plan.capacities.size();
    if (!request->dedicated) for (size_t slot = 0; slot < plan.capacities.size(); ++slot) {
      if (dedicated[slot] || lastDomains[slot] >= request->firstDomain || plan.capacities[slot] < request->bytes) continue;
      if (selected == plan.capacities.size() || plan.capacities[slot] < plan.capacities[selected]) selected = slot;
    }
    if (selected == plan.capacities.size()) {
      plan.capacities.push_back(request->bytes);
      lastDomains.push_back(request->lastDomain);
      dedicated.push_back(request->dedicated);
      plan.storageBytes = addBytes(plan.storageBytes, request->bytes);
    } else {
      lastDomains[selected] = request->lastDomain;
    }
    plan.slotFor.emplace(request->key, selected);
  }
  validateWorkspacePlan(requests, plan);
  return plan;
}

}  // namespace nr
