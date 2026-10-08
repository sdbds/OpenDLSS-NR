# NR Steady-State Memory and Buffer Lifetimes

Date: 2026-10-03

Status: Spec and implementation plan approved by the user on 2026-10-03.
Implementation and targeted verification completed on 2026-10-03. NrPass reuse
is enabled by default after exact-output and measured-LOCAL gates. Results and
the newly selected community-DLL target are in `docs/optimization-validation.md`.

## Intent and Baseline

Reduce the complete Vulkan `NrPass` steady-state LOCAL memory usage toward the
original DLL, without changing the network arithmetic or its input/output
contracts. Keep the 256 MiB staging window unchanged. Do not use subagents.

The comparison in `build/vram-compare/results.json` measures commit `9370065`,
not the current uncommitted code. It subtracts each host's pre-NR baseline from
DXGI LOCAL CurrentUsage. The three-round medians are:

| Size | Original DLL MiB | NrPass 9370065 MiB |
| --- | ---: | ---: |
| 1920x1080 | 750.5000 | 1083.5625 |
| 2560x1440 | 885.9375 | 1474.0625 |
| 3840x2160 | 1322.4375 | 2623.9375 |

The existing uncommitted unused-FFN removal, FP16 head, and compact aux changes
remain part of the candidate. Their combined logical capacity reduction is
about 320.94 MiB at 4K, not a measured LOCAL usage reduction.

The user has authorized targeted builds, output regression, and the existing
three-resolution memory comparison for this work. This supersedes the earlier
static-only restriction for this work, but does not authorize unrelated tests,
repository cleanup, publishing, or changes to other ongoing NGX work.

## Findings That Drive the Design

The current Graph cache allocates one buffer per logical activation and keeps
every allocation until Graph destruction. Re-recording avoids new allocations,
but different stages do not reuse storage.

At 4K, the `9370065` default graph has 1840.36 MiB of buffers. State pairs use
494.13 MiB; expert and split-512 scratch use 350.75 MiB; raw pool intermediates
use 119.56 MiB. The retained full-resolution block-0 output is a separate
255 MiB allocation that must survive until the final block. These are not all
disposable at the same time.

The selected change is buffer-lifetime reuse, rather than another kernel
fusion. This addresses both scratch and already-consumed states and transition
buffers while keeping existing numerical operations intact.

## Alternatives

1. Share only matching encoder/decoder scratch. This is the smallest change,
   but its roughly 175.38 MiB 4K opportunity is too small for the observed gap.
2. Reuse whole buffer slots according to conservative graph lifetimes. This is
   selected. It can cover scratch, state, and transition buffers without adding
   byte offsets to every kernel or aliasing multiple Vulkan objects onto the
   same device-memory range.
3. Build a general graph IR and packed suballocation arena. This could reduce
   fragmentation further, but requires substantially more changes to command
   generation, descriptors, ownership, and the in-progress cross-API trace.
   It is outside this iteration.

## Selected Storage Model

Build a CPU-only allocation plan from the fixed graph topology, Geometry, and
selected routes before recording commands that reference workspace addresses.
Each planned activation has a stable identity, shape, format, byte capacity,
first-use domain, and last-use domain. The production recording must reject a
missing entry or shape mismatch rather than silently using the wrong plan.

A deterministic best-fit planner assigns non-overlapping lifetimes to whole
buffer slots. Slots are sized once to cover their assigned views. One slot owns
one VkBuffer and one VkDeviceMemory allocation; logical Activation objects are
non-owning views of the slot, with their own shape and label. No slot moves or
grows after command recording starts. Owners are destroyed exactly once.

Using the same buffer object, rather than separate overlapping buffer objects,
keeps descriptor and BDA consumers unchanged. The existing CommandTrace sees
one physical buffer identity and one address range for each slot. Logical
activation identities belong in the allocation-plan report, not in a second
set of GPU allocations.

Use a Graph option to select reuse. Keep a dedicated-allocation route for
comparison and compatibility. Enable reuse in NrPass only after the targeted
validation passes. Unsupported route combinations, intermediate capture, and
boundary capture use dedicated storage and report that choice explicitly.
The initial supported route is the default fused graph, including its existing
PTX families and both head storage formats. Do not force a new kernel route.

## Lifetime and Synchronization Rules

- Borrowed input features are never reused or modified. The same rule applies
  when a caller obtained its input with Graph::allocate.
- The full-resolution block-0 output remains live through block 70.
- Encoder skips remain live through their decoder consumer. A state buffer is
  not scratch merely because its encoder stage has completed.
- The final decoder-32 state remains live through block 70's low-resolution
  reads. It cannot share the head's slot during that block.
- Two buffers used by the same dispatch cannot occupy the same slot unless
  the existing kernel already has an explicit in-place contract; this change
  introduces no new in-place operations.
- Reuse domains must be separated by GPU execution dependencies that cover
  the final reads as well as writes. CPU recording order and device-counter
  publication alone do not prove safe reuse. Keep intra-stage chaining, but
  add an explicit handover dependency where existing stage boundaries do not
  establish the required ordering.
- Before a new graph execution can overwrite a slot used for the previous
  head, the previous consumer must have completed. Preserve the serialized
  Graph execution contract and audit NrPass's composite-to-next-frame ordering.
- Initialization and padding are part of each view's contract. Do not assume
  an aliased buffer is still zero from construction. Clear required padding
  with ordered commands, or retain dedicated storage where write coverage
  cannot be established. Do not clear the whole workspace every frame.
- Capture buffers, temporal images, model weights, aux, and kernel sync or
  split-K storage remain outside this Graph reuse pool.

An initial coarse CPU lifetime exercise, preserving input and block-0 storage,
gave the following planning estimates. These are not GPU-validated capacities
or predicted driver measurements:

| Size | Current logical Graph MiB | Whole-slot estimate MiB |
| --- | ---: | ---: |
| 1920x1080 | 441.90 | 226.78 |
| 2560x1440 | 751.64 | 386.56 |
| 3840x2160 | 1665.05 | 856.69 |

The estimate includes the prior FFN and head changes, retains raw pool buffers,
and uses 13 slots including the separately preserved input. Lifetimes must be
checked against real command dependencies before any of these assignments is
accepted. The final planner may conservatively use more storage.

## Accounting

Report three separate quantities instead of calling all of them VRAM:

- Logical activation bytes and the plan's conservative live-byte maximum.
- Unique physical buffer/image allocation bytes, current and peak, grouped by
  owner and Vulkan memory heap. Count a shared slot once, not once per view.
- Driver-reported process LOCAL and NON_LOCAL usage, with the existing host
  baseline subtraction. Driver and module overhead remains visible as a
  residual rather than being silently assigned to model or Graph buffers.

Include Context's existing staging and dummy allocations, model/aux caches,
kernel scratch, Graph storage, NrPass-owned images, and parameter buffers.
Externally owned renderer images are identified separately, not double-counted.
Record initialization and re-recording allocation/free counts, and expose the
logical-view-to-slot mapping and handover domains for review.

Keep the original measurement implementation pinned to `9370065`; build the
candidate as a different executable with recorded source/route identity.
Do not overwrite independent output references with candidate output.

## Verification and Acceptance

First establish that the accumulated uncommitted changes build and pass the
targeted existing regression. Then compare the same candidate with reuse off
and on, isolating reuse from the previous three optimizations.

Test the pure planner for overlapping intervals, deterministic assignments,
capacity/alignment, preserved resources, and invalid plan requests. Exercise
real GPU reuse with poisoned prior contents, repeated submissions, and the
supported geometry/head-format combinations. Compare outputs bit-for-bit with
the dedicated route and use independent pre-optimization references where
available. Confirm addresses and physical allocation counts remain stable
after initialization, and allocations are released exactly once.

Run the complete NrPass memory probe at 1080p, 1440p, and 4K with the same input,
adapter, controls, and measurement phases as the pinned baseline. Retain the
reset-every-frame comparison, and separately cover reset-on-first-frame-only
temporal operation. Record GPU timings to identify a memory-versus-latency
tradeoff; do not claim a speedup from fewer bytes alone.

Accept a reuse route only with output parity, safe handover ordering, stable
re-recording, and an actual reduction in steady LOCAL usage. Report the
remaining gap to the original DLL; approaching it is the target, not a promise
that the two implementations have identical internal allocation strategies.

## Exclusions

No staging resize, VR work, projection/pooling kernel fusion, input/head alias,
weight-format change, generic allocator replacement, or unrelated NGX/D3D
refactor in this iteration. Split-K right-sizing is a separate follow-up after
Graph reuse has been measured. Preserve all concurrent working-tree changes.
