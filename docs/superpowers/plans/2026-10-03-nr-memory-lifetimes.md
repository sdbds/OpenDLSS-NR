# NR Memory Lifetimes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. The user requires inline execution without subagents. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce complete NrPass steady LOCAL usage toward the original DLL through verified graph buffer reuse, with accurate memory accounting.

**Architecture:** Plan conservative closed lifetime intervals for the fixed default graph, assign them to stable whole-buffer slots, and expose non-owning Activation views. Keep dedicated allocation available and preserve all externally visible lifetimes. Compare the same candidate with reuse disabled and enabled before enabling the production route.

**Tech Stack:** C++20, Vulkan/volk, existing PTX and GLSL kernels, PowerShell, MSVC, existing DXGI memory probe.

**Spec:** `docs/superpowers/specs/2026-10-03-nr-memory-lifetimes-design.md`

## Global Constraints

- Keep the 256 MiB staging window unchanged. Do not use subagents.
- No network arithmetic, precision, quantization, or kernel-route changes in the reuse implementation.
- No caller-input reuse, including inputs allocated through Graph::allocate.
- Preserve block-0 output through block 70 and encoder skips through their decoder consumers.
- Slot addresses are fixed before recording and never relocated for re-recording.
- Capture, unsupported routes, temporal images, model/aux, and kernel scratch remain outside reuse.
- Preserve concurrent NGX/trace edits. Do not stage shared files wholesale for commits.
- Targeted builds, parity regression, and memory measurements are authorized; do not replace independent reference outputs.
- No push, VR work, staging resize, or projection/pooling fusion in this iteration.

## Review Focus

- Multiple pre-recorded command buffers and repeated frames must not resize slots or overwrite a previous composite's input early.
- Odd valid sizes and padded ViT rows must not read data left by an earlier slot occupant.
- Borrowed input and externally exposed head contents must retain their existing observable contracts.
- Environment-selected fallback and capture routes must not silently use a default-only lifetime plan.
- Shared Vulkan buffer identities must remain unambiguous to CommandTrace and cross-API replay.

## File Responsibilities

- `src/nr_workspace.h/.cpp`: pure CPU lifetime validation, deterministic slot assignment, and byte totals.
- `src/nr_graph_workspace.cpp`: graph-specific shapes, route gating, lifetime domains, and view-to-slot mapping.
- `src/nr_graph.h/.cpp`: physical ownership, logical views, stable recording, handover synchronization, diagnostics.
- `src/vk_context.h/.cpp`: per-Context allocation ledger and memory-heap accounting, preserving trace hooks.
- `src/nr_model.cpp`, `src/kernels.cpp`: owner tags for existing allocations; no numerical changes.
- `demo/nr_pass.h/.cpp`: owned-image accounting, candidate option, and frame-consumer ordering.
- `tests/workspace_plan_tests.cpp`, `tests/workspace_regression.cpp`: CPU planner and GPU aliasing tests.
- `tests/graph_regression.cpp`, `tests/pass_regression.cpp`: route switches, full-pipeline parity, and reporting.
- `tests/nr_memory_probe.cpp`, `scripts/test_memory.ps1`: reproducible candidate steady-memory comparison.
- `scripts/build_tests.ps1`, `scripts/build.ps1`, `demo/CMakeLists.txt`: add new implementation/test units without dropping trace dependencies.
- `scripts/test_graph.ps1`, `docs/optimization-validation.md`, `docs/frame.md`: coverage and documented contracts/results.

### Task 1: Establish the Current Dedicated Candidate

**Interfaces:** Preserve the existing Graph and NrPass behavior. Produce independent logs for the accumulated three optimizations before adding reuse.

- [x] Build existing shaders/PTX with `./scripts/build_shaders.ps1`; require exit 0.
- [x] Build existing graph/pass/aux runners with `./scripts/build_tests.ps1`; require all three executables to link.
- [x] Run `build/tests/aux_regression.exe` with normal model verification; require byte-exact storage and all rejection/address-stability checks to pass.
- [x] Run `./scripts/test_graph.ps1 -LogDirectory tmp/memory-lifetimes/before-f32` and `./scripts/test_graph.ps1 -Half -HalfHead -FullSizes -LogDirectory tmp/memory-lifetimes/before-f16` against the existing default `tmp/optimization/baseline-a`. Require every available case to report PASS. Add `-ExpertFallbacks`, `-PostFallback`, and `-AuxFallbacks` only when their independent fixtures exist; report missing fixtures instead of generating them from candidate output.
- [x] Read the pass runner's options and run its existing direct/copy/history parity cases against the retained independent pass baseline. Keep fresh logs separate from older results.
- [x] Diagnose any failure before continuing. No failures were found in this baseline run; no corrective source edits were needed.

Baseline run on 2026-10-03: eight F32-input graph cases and eleven F16-input/F16-head
cases passed bit-exact checks, including 1080p/1440p/4K in the latter suite. The
four pass invocations (`--expect-direct`, plus each of `--copy-target`,
`--mixed-target`, and `--unknown-usage`) each passed 12 temporal frames against
the retained pass baseline. Compact aux passed with normal model verification.
The 4K F16 graph allocation was 1,745,928,192 bytes (1665.046875 MiB), with stable
re-recording allocation counts. These are not complete-process LOCAL measurements.

Additional expert/post/aux fallback reference directories and the old resident
allocation metadata do not exist in `baseline-a`; their corresponding baseline
savings checks were not claimed. The baseline includes current concurrent trace
and PTX edits; later A/B comparisons must use one identical candidate build.
Source/executable fingerprints are in `tmp/memory-lifetimes/before-source-manifest.json`.

### Task 2: Account for Physical Allocations

**Interfaces:**
- Add `vk::MemoryOwner { Context, Model, Kernels, Graph, Pass, Unspecified }`.
- Add `vk::MemorySnapshot Context::memorySnapshot() const`, containing `liveBytes`, `peakBytes`, `allocationCount`, `freeCount`, per-heap and per-owner totals, and live records with logical bytes, allocated bytes, heap, owner, kind, and an owned label.
- Add an optional final `MemoryOwner` argument to `Context::createBuffer`, preserving existing argument order and defaulting to Unspecified.
- Add `Context::trackImageAllocation(VkDeviceMemory, VkDeviceSize logicalBytes, VkDeviceSize allocatedBytes, uint32_t memoryType, const char* label)` and `Context::untrackImageAllocation(VkDeviceMemory)` for NrPass-owned image memory only.

- [x] Add `context_allocations_are_included`, `buffer_lifetime_updates_heap_and_owner`, and `duplicate_image_removal_is_rejected` to `tests/workspace_regression.cpp`: constructor-owned staging/dummy appear, one buffer increases the matching heap/owner and live bytes, freeing restores live bytes without reducing the recorded peak, and duplicate removal is rejected without underflow. Run these as `build/tests/workspace_regression.exe --accounting-only`.
- [x] Run the new assertions before implementing the ledger; require failure at the missing interface or expected accounting assertion.
- [x] Track actual successful memory allocations by VkDeviceMemory, recording requirements.size and memory-type heap, and copying labels into owned strings. Do not change allocation policy. Keep failures and cleanup accounting consistent.
- [x] Tag existing Model, Kernels, Graph, and NrPass buffers at their allocation call sites. Register/unregister owned images using their existing VkMemoryRequirements and memory type; do not count imported image views as owned images.
- [x] Report snapshots after Context initialization, graph preparation, warmup, measured frames, and Graph release. Keep legacy post-context regression totals distinct so old references remain meaningful.
- [x] Re-run ledger and dedicated graph/pass tests; require stable GPU allocation/free counts during repeated recording and no accounting duplication.
- [x] Review the focused diff. Commit only independently separable changes; defer commits for shared hunks that depend on uncommitted trace work.

### Task 3: Pure Planner and Default-Graph Lifetimes

**Interfaces:**
- `nr::WorkspaceRequest { std::string key; uint64_t bytes; uint32_t firstDomain, lastDomain; bool dedicated; }`.
- `nr::WorkspacePlan` contains slot capacities, a request-key-to-slot mapping, logical bytes, slot bytes, and a conservative live-byte maximum.
- `nr::WorkspacePlan nr::planWorkspace(const std::vector<WorkspaceRequest>& requests)`.
- `std::vector<WorkspaceRequest> Graph::makeWorkspaceRequests() const` supplies graph requests; `bool Graph::workspaceRouteSupported() const` gates the exact supported route.

- [x] Write CPU tests `touching_intervals_do_not_share`, `disjoint_intervals_share`, `capacity_is_sufficient`, and `repeat_is_deterministic`: [0,2] and [2,3] have different slots; [0,1] and [2,3] of equal capacity have the same slot. Add rejection cases for duplicate keys, zero/overflowing sizes and inverted intervals, plus empty-plan and dedicated/simultaneous input-output cases.
- [x] Run `workspace_plan_tests.exe` after adding its build target; require the missing implementation or assertions to fail before implementing the planner.
- [x] Implement a stable first-domain/descending-size ordering and best-fit reuse only when a slot's last domain is strictly earlier than the request's first domain. Use checked 64-bit size arithmetic. Validate the completed assignment for every overlapping interval pair sharing a slot.
- [x] Define conservative integer domains for pre, every encoder/decoder stage, each transition, ViT, and post. Add the actual default graph allocation names, shapes, formats, and terminal consumers. The input is separate dedicated storage. Do not shorten skip or block-0 lifetimes to their producing stage.
- [x] Derive dimensions from Geometry and bytes from alignRows/formatBytes. Use existing route predicates for optional publications. Reject unsupported route combinations before any workspace command is recorded.
- [x] Add topology tests at 1080p, 1440p, 4K, and an existing odd-size case. Check complete coverage of planned internal allocations and all skip/transition consumers. Add `final_decoder_state_is_live_through_post`: decoder-32's final state overlaps block 70 and cannot share with its head output. Treat the spec's 13-slot estimate as a hypothesis, not an assertion that overrides safer lifetimes.
- [x] Run all CPU planner tests; require PASS and independently inspect the exported view-to-slot table. Review the diff before integrating GPU ownership.

### Task 4: Stable Graph Slots and GPU Handover

**Interfaces:**
- Add `Graph::Options::reuseWorkspace = false` and `Graph::Options::poisonWorkspace = false` (test-only).
- Keep public `Graph::allocate` dedicated. Add private `Activation* allocateInternal(const std::string& label, uint32_t rows, uint32_t channels, Format format)` for Graph-owned planned views; captures continue using dedicated allocation.
- Add private `void prepareWorkspace()` and `void enterWorkspaceDomain(VkCommandBuffer commands, uint32_t domain)`.
- Add `std::string Graph::workspaceReport() const` for selected route, view mapping, physical capacity, and handover domains.

- [x] Write `borrowed_input_is_preserved`, `reuse_head_matches_dedicated`, `rerecord_keeps_addresses_and_allocations`, and `invalid_overlap_fails_before_dispatch` in `tests/workspace_regression.cpp`. Assert exact input/head bytes, identical saved addresses/counters after repeated records, and no dispatch on rejected overlap. Run with `build/tests/workspace_regression.exe --width 513 --height 377`, then each full-resolution case and `--fp16-head`.
- [x] Run these tests with the production integration absent; require the route or capacity assertions to fail.
- [x] Separate Graph's physical buffer owners from logical Activation objects. Dedicated buffers and workspace slots each have one owner and a stable owned label. Destroy physical buffers exactly once; never free memory through a logical view.
- [x] Prepare all slot buffers before recording address-bearing launches. Change internal allocation call sites to validated logical views; preserve shape/format checks and the ordinary public allocation path.
- [x] Add domain handovers based on GPU execution dependencies, preserving intra-stage chaining. Audit final reads, deferred projections, c32 decoder-to-post chaining, and composite-to-next-frame execution. Do not replace a GPU dependency with a CPU bookkeeping transition.
- [x] Determine each reused view's write coverage and required initialized padding. Add ordered, bounded padding initialization where needed. In poison mode, fill recycled storage before initialization/production without touching any still-live view.
- [x] Test poison mode, signed/subnormal/rounding-sensitive inputs, odd dimensions, both head formats, repeated submissions, and borrowed input. Add a trace round-trip check that all pointers and descriptors resolve to the same intended physical slot.
- [x] Exercise unsupported PTX/environment routes and both capture modes; require explicit dedicated fallback with correct output, not partial reuse of an invalid plan.
- [x] Run planner, workspace, and graph regressions. Require exact output parity, stable physical allocation counts, valid ownership cleanup, and observed capacity reduction. Keep NrPass reuse disabled until this passes.

### Task 5: Complete NrPass Comparison and Enablement

**Interfaces:**
- Append `bool reuseWorkspace = false` to the existing NrPass constructor for explicit probe selection, retaining all earlier parameters and calls. Change that default to true only at the enablement step after validation; unsupported routes still fall back explicitly.
- `nr_memory_probe.exe` accepts `--workspace dedicated|reuse`, `--reset every|first`, existing size/model/shader/input paths, and prints source/route identity plus memory snapshots and driver samples.
- `scripts/test_memory.ps1` builds/runs a separately named candidate and writes a new results directory. It reads the pinned report without modifying it.

- [x] Add full-pipeline A/B checks with reset only on the first frame as well as every frame. Compare output and both history parities and exercise direct/copy routes. Add `two_pre_recorded_frames_preserve_composite_reads`: prepare both parities with fixed bound images, submit two serialized frames without a CPU wait between them, capture each completed output in command order, and wait once at the end. Do not rotate/rebind external images while commands are pending. Require the dedicated and reused outputs to match, proving the earlier composite finishes its head reads before slot reuse.
- [x] Run those checks before enabling production reuse; require exact dedicated/reuse parity and no chained timeouts.
- [x] Adapt the existing memory-probe methodology into a candidate runner using current production NrPass, without retaining an extra F32 feature tensor. Build it separately from `build/vram-compare/optimized-memory-probe.exe` and record the dirty source manifest, shader identity, options, and adapter LUID.
- [x] Run three alternating rounds at 1920x1080, 2560x1440, and 3840x2160, 20 frames each, with the existing controls and baseline/steady phases. Sample LOCAL and NON_LOCAL separately; preserve raw logs and the independent `9370065` and original DLL results.
- [x] Report dedicated candidate versus reused candidate versus pinned implementation versus original DLL. Separate allocated bytes, logical live bytes, steady driver usage, sampled peaks, and GPU timing. Do not count one slot repeatedly through its views.
- [x] Enable reuse for supported NrPass routes only if parity and measured steady LOCAL reduction pass. If capacity savings do not appear in LOCAL usage, investigate the ledger residual before enabling or adding another optimization.
- [x] Update validation and frame-contract docs with exact commands, route identity, measured results, gaps, and unsupported cases. Perform a final inline ownership/synchronization review. Commit only safely separated work; do not publish or bundle unrelated NGX changes.
