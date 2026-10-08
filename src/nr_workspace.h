#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nr {

struct WorkspaceRequest {
  std::string key;
  uint64_t bytes = 0;
  uint32_t firstDomain = 0, lastDomain = 0;  // closed interval of GPU handover domains
  bool dedicated = false;
};

struct WorkspacePlan {
  std::vector<uint64_t> capacities;
  std::map<std::string, size_t> slotFor;
  uint64_t logicalBytes = 0, storageBytes = 0, peakLiveBytes = 0;
};

WorkspacePlan planWorkspace(const std::vector<WorkspaceRequest>& requests);
void validateWorkspacePlan(const std::vector<WorkspaceRequest>& requests, const WorkspacePlan& plan);

}  // namespace nr
