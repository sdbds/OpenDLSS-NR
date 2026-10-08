#include <algorithm>
#include <iostream>
#include <iomanip>
#include <limits>
#include <stdexcept>

#if __has_include("nr_workspace.h")
#include "nr_workspace.h"
#include "nr_graph.h"
#define HAVE_WORKSPACE_PLANNER 1
#endif

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template<class Function>
void rejects(Function function, const char* message) {
  bool rejected = false;
  try { function(); } catch (const std::runtime_error&) { rejected = true; }
  require(rejected, message);
}

#ifdef HAVE_WORKSPACE_PLANNER
void plannerTests() {
  using nr::WorkspaceRequest;
  const auto empty = nr::planWorkspace({});
  require(empty.capacities.empty() && empty.logicalBytes == 0 && empty.storageBytes == 0, "empty plan not empty");
  const std::vector<WorkspaceRequest> disjoint{{"a", 64, 0, 1}, {"b", 64, 2, 3}};
  const auto shared = nr::planWorkspace(disjoint);
  require(shared.slotFor.at("a") == shared.slotFor.at("b"), "disjoint intervals did not share");
  require(shared.logicalBytes == 128 && shared.storageBytes == 64 && shared.peakLiveBytes == 64,
          "disjoint plan byte counts incorrect");
  const std::vector<WorkspaceRequest> touching{{"a", 64, 0, 2}, {"b", 64, 2, 3}};
  const auto separate = nr::planWorkspace(touching);
  require(separate.slotFor.at("a") != separate.slotFor.at("b"), "touching intervals overlap in one slot");
  require(separate.storageBytes == 128 && separate.peakLiveBytes == 128, "closed-interval live peak incorrect");
  std::vector<WorkspaceRequest> requests{{"large", 128, 0, 1}, {"small", 64, 0, 1}, {"late", 48, 2, 2}};
  const auto best = nr::planWorkspace(requests);
  require(best.slotFor.at("late") == best.slotFor.at("small"), "planner did not choose the smallest fitting slot");
  require(best.storageBytes == 192 && best.logicalBytes == 240, "best-fit capacity incorrect");
  std::reverse(requests.begin(), requests.end());
  const auto repeated = nr::planWorkspace(requests);
  require(repeated.slotFor == best.slotFor && repeated.capacities == best.capacities, "planning is not deterministic");
  const auto pinned = nr::planWorkspace({{"input", 64, 0, 1, true}, {"head", 64, 2, 3}});
  require(pinned.slotFor.at("input") != pinned.slotFor.at("head"), "dedicated input was reused");
  const auto finalDomain = nr::planWorkspace({{"last", 1, UINT32_MAX, UINT32_MAX}});
  require(finalDomain.peakLiveBytes == 1, "inclusive end overflowed the domain range");
  auto bad = separate;
  bad.slotFor["b"] = bad.slotFor.at("a");
  rejects([&] { nr::validateWorkspacePlan(touching, bad); }, "validator accepted overlapping views");
  bad = separate;
  bad.capacities[bad.slotFor.at("a")] = 32;
  bad.capacities[bad.slotFor.at("b")] = 96;
  rejects([&] { nr::validateWorkspacePlan(touching, bad); }, "validator accepted undersized storage");
  rejects([] { nr::planWorkspace({{"a", 64, 0, 1}, {"a", 64, 2, 3}}); }, "duplicate key accepted");
  rejects([] { nr::planWorkspace({{"", 64, 0, 1}}); }, "empty key accepted");
  rejects([] { nr::planWorkspace({{"a", 0, 0, 1}}); }, "zero size accepted");
  rejects([] { nr::planWorkspace({{"a", 64, 2, 1}}); }, "reversed interval accepted");
  rejects([] { nr::planWorkspace({{"a", std::numeric_limits<uint64_t>::max(), 0, 1}, {"b", 1, 0, 1}}); },
          "byte counter overflow accepted");
}

void topologyTests(bool dump) {
  struct Case { uint32_t width, height; uint64_t internalBytes; };
  const Case cases[] = {{1920, 1080, 392585216}, {2560, 1440, 667566080}, {3840, 2160, 1478541312}, {513, 377, 0}};
  for (const auto& test : cases) {
    const auto geometry = nr::Geometry::fromValid(test.width, test.height);
    const auto requests = nr::graphWorkspaceRequests(geometry, true, 128);
    const auto plan = nr::planWorkspace(requests);
    require(requests.size() == 66, "default internal activation coverage changed");
    if (test.internalBytes) require(plan.logicalBytes == test.internalBytes, "graph allocation shapes do not match independent totals");
    require(plan.storageBytes < plan.logicalBytes, "graph plan did not reduce storage");
    for (const auto& request : requests) {
      require(request.key.find("input features/") != 0, "caller input included in workspace");
      require(plan.capacities.at(plan.slotFor.at(request.key)) >= request.bytes, "graph slot capacity too small");
    }
    nr::validateWorkspacePlan(requests, plan);
    std::cout << "plan " << test.width << 'x' << test.height << " logical=" << plan.logicalBytes
              << " storage=" << plan.storageBytes << " live_peak=" << plan.peakLiveBytes
              << " slots=" << plan.capacities.size() << '\n';
    if (test.width == 3840) {
      if (dump) for (const auto& request : requests)
        std::cout << "view slot=" << plan.slotFor.at(request.key) << " bytes=" << request.bytes
                  << " first=" << request.firstDomain << " last=" << request.lastDomain
                  << " key=" << std::quoted(request.key) << '\n';
      const std::string head = "RGBA neural head/8355840x4/1";
      const std::string decoder = "decoder 32 skip merge/2088960x32/0";
      const std::string retained = "retained full block0/8355840x32/0";
      require(plan.slotFor.at(head) != plan.slotFor.at(decoder), "final decoder state overlaps head output");
      require(plan.slotFor.at(head) != plan.slotFor.at(retained), "block0 was recycled before post");
      const auto f32 = nr::planWorkspace(nr::graphWorkspaceRequests(geometry, false, 128));
      require(f32.logicalBytes == plan.logicalBytes + 66846720, "F32 head capacity incorrect");
    }
  }
  rejects([] { nr::graphWorkspaceRequests(nr::Geometry{}, true, 128); }, "empty geometry accepted");
}
#endif
}  // namespace

int main(int argc, char** argv) {
  try {
#ifdef HAVE_WORKSPACE_PLANNER
    plannerTests();
    topologyTests(argc > 1 && std::string(argv[1]) == "--dump");
    std::cout << "PASS workspace intervals, capacities and graph lifetimes\n";
#else
    throw std::runtime_error("workspace planner interface is missing");
#endif
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
