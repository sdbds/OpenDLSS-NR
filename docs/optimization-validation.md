# First-pass optimization validation

Base revision: `9d08f4184bbcb9d858e2fb7a7834ec0837a9d2f1`.
Local verification: RTX 4090, driver 610.88, Windows, 2026-10-03.

## Changes and Contracts

- Block 0 / block 4 raw outputs, the decoder-32 raw skip merge, and split-512
  middle / QKV / normalized buffers are allocated only on paths that consume them.
  Unfused and intermediate-capture paths retain their buffers.
- `Graph::record` accepts F32 and F16 features. F32 callers keep their existing
  conversion; F16 skips that conversion. Both PTX and GLSL block-0 paths support
  the packed input. The demo writes the same sixteen features as binary16, not FP8.
- A renderer target with explicit `VK_IMAGE_USAGE_STORAGE_BIT` receives composite
  writes directly. Targets without storage usage, including legacy `usage == 0`
  callers, retain the copy path. Its internal image is created lazily.

The Filament patch adds an opt-in `TextureUsage::STORAGE` and reports the actual
Vulkan image usage through the interop bridge. Rebuild Filament and the demo
together after updating this patch. The demo target also has `BLIT_SRC` for
capture; direct-target `saveOutput` requires `TRANSFER_SRC` usage.

Targets must be RGBA8 UNORM and match the pass dimensions. As with the existing
color/velocity bindings, changing a directly bound target requires earlier
recorded work to be complete. This change does not add a frames-in-flight
resource-set cache. Do not rotate storage targets that are still in flight.

`NrTimings::outputMs` and `kOutputReady` cover the copy or direct-write layout
transition. `presentMs` now starts at target readiness, not composite completion.
History publication and compute-to-sampling synchronization remain in place.

## Allocation Results

At 3840x2160 (3840x2176 padded), the regression runner observes real
`VkAllocateMemory` / `vkFreeMemory` calls, forwarding them to the driver.
The following counts cover Graph-owned buffers, including input, but exclude
model/kernel caches, images and the driver's internal allocations.

| Version | Graph Allocation Bytes | Reduction From Base |
| --- | ---: | ---: |
| A: original F32 input | 3,108,356,096 | 0 |
| B: path-dependent allocation | 2,197,143,552 | 869 MiB |
| C: B plus F16 input | 1,929,756,672 | 1124 MiB |

B saves 765 MiB in the three raw tensors and 104 MiB in split-512 temporaries
(their rows are allocated in multiples of 64). C removes a further 255 MiB.
Direct composition additionally avoids one RGBA8 intermediate image, with a
31.64 MiB pixel payload at 4K; this is not a measurement of its driver allocation.
It also eliminates one full-image copy per frame on the direct path.

## Correctness Checks

The references are fixed-input captures of the original implementation using the
local model, not the original DLSS implementation's native fixtures. No model or
reference image data is committed.

- GPU graph tests cover default execution, odd-sized boundary captures,
  unfused execution, intermediate captures, disabled pre/pool/upres fusion,
  GLSL block-32, 1080p, 1440p and 4K. Heads from F16-input runs and captured boundaries compare
  byte-for-byte against the original F32 references. Re-recording must not add
  allocations; the allocation saving is an explicit regression gate.
- The headless renderer test runs the real `NrPass` with procedural HDR color
  and motion images. Twelve frames cover history initialization, off-screen
  motion, NR off/on, temporal off, reset, target rotation and resize. It compares
  final RGBA8 output, sampled binary16 history and input feature bits, and checks
  the image-copy count. Direct, copy, mixed and unknown-usage routes are covered;
  switching to copy also releases the previous direct-target image view.
- Output capture is checked against the renderer target, not just for file existence.
- The existing PTX `fast_divmod` test checks all numerators below 2^24 for divisors 1..32.
- The full patched Filament and demo build succeed. A 44-frame demo smoke run with
  an animated scene and HDR lighting exits successfully and produces a nonblank capture.

The first-pass base revision's model hash function prints uppercase digests,
while the local manifest uses lowercase. `test_graph.ps1` independently checks
every stage's length and SHA-256 with PowerShell before invoking the runner with
`--no-verify`, supporting reference recording against that revision without
changing its production hash checker or model files.

Khronos validation was requested but is not installed on this machine. Runtime
parity tests are not a substitute for a synchronization-validation run.

## Commands

From the repository root, after fetching the portable tools and providing the model:

```powershell
scripts/build.ps1
scripts/build_tests.ps1
scripts/test_graph.ps1 -FullSizes
scripts/test_graph.ps1 -Half -FullSizes
build/tests/pass_regression.exe --expect-direct
build/tests/pass_regression.exe --copy-target
build/tests/pass_regression.exe --expect-direct --mixed-target
build/tests/pass_regression.exe --unknown-usage
python scripts/ptx/test_fast_divmod.py
```

The comparison commands require the local references in
`tmp/optimization/baseline-a` and `tmp/optimization/pass-baseline`.
For a new optimization experiment, build the test runners against its starting
implementation and use `test_graph.ps1 -RecordBaseline -FullSizes` and
`pass_regression.exe --record` with new reference directories **before** editing
production code. The pass recorder deliberately requires an F32 producer for
the input-storage experiment; do not record an optimized F16 result as its own
F32 baseline. Native parity still uses the existing `dlss5vk parity` fixtures.

Performance is a separate mode without frame readback or boundary capture:

```powershell
build/tests/pass_regression.exe --benchmark --width 3840 --height 2160 --frames 300 --warmup 30 --expect-direct
build/tests/pass_regression.exe --benchmark --width 3840 --height 2160 --frames 300 --warmup 30 --copy-target
```

This reports median/P95 CPU preparation, motion plus preprocess, network,
composite, target readiness and total headless NR GPU time. It uses fixed scene
inputs and incrementing frame seeds. It excludes actual game rendering and uses
synchronous submissions, so it cannot establish a game's frame-rate improvement.
Short correctness-test timing samples are not speed claims.

Three rounds of 300 timed frames (30 warmup frames each) compared direct output
with the copy fallback using the same F16 pipeline. Execution order was reversed
in the middle round. These are ranges of per-round medians, not pooled samples:

| Valid Size | Direct Total NR (ms) | Copy Total NR (ms) | Copy/Readiness Interval (ms) |
| --- | ---: | ---: | ---: |
| 1920x1080 | 5.632-5.670 | 5.540-5.721 | 0.0078-0.0081 |
| 2560x1440 | 8.667-9.117 | 8.904-9.194 | 0.0132-0.0134 |
| 3840x2160 | 18.654-20.227 | 18.540-19.217 | 0.0317-0.0327 |
| 1919x1079 | 5.339-5.387 | 5.434-5.592 | 0.0078-0.0081 |

Direct readiness medians were at most 0.00025 ms (near timestamp resolution).
The copy interval is removed, but a consistent total-NR speedup was **not**
established; at 4K, one direct round was slower than every copy round. Do not
translate the allocation reduction into an FPS claim. Logs with stage medians
and P95s are retained under `tmp/optimization/bench-*-r*.txt` locally. These
measurements isolate output routing, not the speed difference between F32 and
F16 input generation.

Split-512 PTX, tile/grid tuning, motion-vector integration, history-sample caching
and rotating-resource caching remain outside this first batch.

## Follow-Up: Expert Allocation Routes

Starting revision: `93700651eb9c3b311227188c61107e2856e87b7d`.
Status: built and checked against all available original graph/pass fixtures on
2026-10-03. Missing extra-route fixtures remain a separate limitation below.
The current full-pipeline memory comparison is recorded at the end of this file.

- The fused PTX expert route does not allocate `ffnNarrow`. The allocator and
  encoder use the same route predicate. PTX MLP, GLSL MLP, unfused execution and
  intermediate capture keep their required narrow buffer.
- Ordinary expert W1 layouts are created only by their GLSL MLP / unfused
  consumers. PTX expert and PTX MLP paths retain only the permuted W1 layout.
- This allocation-only change leaves formats, arithmetic, image routing and
  resource ownership unchanged. FP16 head is a separate opt-in change below;
  compact aux data is described separately below. Workspace aliasing and VR
  remain deferred.

At 3840x2176 padded, the payload targets are 111.5625 MiB fewer Graph buffers
and 4.875 MiB fewer cached expert weights. These are separate categories, derived
from shapes, not new `VkDeviceMemory` measurements or speed claims.

The graph runner now records allocation/free call counts, live bytes and peak
bytes globally and per memory heap. Both warmup and measured re-recording must
leave the counters and bytes unchanged; allocating and freeing an equal-size
buffer no longer escapes the check. Output is marked `scope=post-context`:
the hooks start after Context construction, so staging and other Context-owned
initial allocations, renderer images and driver-internal allocations are not
included. This is not full-module VRAM accounting.

New recordings also write `resident-allocation-bytes.txt`, measured after Graph
destruction while model/kernel resources remain alive. The runner can enforce
`--minimum-resident-saved 5111808` against an appropriate pre-change PTX baseline.
The script enables this gate when that metadata exists; older references without
it still check Graph savings and output parity, but do not gate weight-cache
savings.

`test_graph.ps1` retains the original `9d08f418` F32 baseline convention and raises
the Graph saving targets to include this follow-up. `-ExpertFallbacks` adds
separate PTX MLP and GLSL MLP cases with `DLSS5VK_PTX_FFN=0`; those paths must keep
`ffnNarrow`. They require their own pre-optimization reference directories, which
have not been recorded in this follow-up. To produce them, use the updated test
harness with the original production sources; do not record the optimized code
as its own reference.

Deferred verification commands, once those references are available:

```powershell
scripts/build_tests.ps1
scripts/test_graph.ps1 -FullSizes -ExpertFallbacks
scripts/test_graph.ps1 -Half -FullSizes -ExpertFallbacks
```

## Follow-Up: Opt-In FP16 Head

Status: built and verified on 2026-10-03, including full-size F16-head graph checks
and direct/copy/mixed/unknown-output temporal pass regressions.

`Graph::Options::fp16Head` defaults to false. Existing Graph callers, the CLI's
native parity path and `head.f32` exports keep the F32 contract. `NrPass` opts in
explicitly and requires an F16 head for its updated composite shader.

Both fused implementations store the original half accumulator pairs: PTX uses
`F_HEAD_F16=512` (the post/head variant is `block32_e4m3_f560.ptx`), and GLSL packs
the half values without an intervening F32 quantization. The non-fused / no-post
route uses the existing F16 output mode of `gemmF16`. The old F32 branches remain.
Composite expands packed halves to F32 before its existing calculations, and
output-capture diagnostics decode according to the head's format. History
truncation, input ownership and image routing do not change.

At 3840x2176 padded, the head payload changes from 127.5 MiB to 63.75 MiB.
Together with `ffnNarrow`, that is a 175.3125 MiB Graph-capacity target relative
to `93700651`, plus the separate 4.875 MiB expert-weight target. None of these
follow-up targets is a new allocation or performance measurement.

The graph runner's `--fp16-head` mode checks the returned format and byte count,
expands the head to F32 and compares every byte with the original F32 reference.
Repeat-submission checks use the same decoding. References are never compared
with shortened buffers or silently skipped, and recording an F16-head result as
its own reference is rejected. `test_graph.ps1 -HalfHead` exercises this mode
independently of `-Half` (input storage) and raises the allocation-saving gate.
The existing GLSL block-32, unfused and intermediate-capture cases cover the
alternative producers; `-PostFallback` adds a dedicated `no-post` case, which
requires a separately captured original-production reference.

Rebuild both the core shaders/PTX and the demo shaders before running the new
code; old binaries and shader caches do not contain the new storage contract.
Deferred verification commands, after supplying the missing route references:

```powershell
scripts/build_shaders.ps1
scripts/build_tests.ps1
scripts/test_graph.ps1 -FullSizes -ExpertFallbacks -PostFallback
scripts/test_graph.ps1 -Half -FullSizes -ExpertFallbacks -PostFallback
scripts/test_graph.ps1 -HalfHead -FullSizes -ExpertFallbacks -PostFallback
scripts/test_graph.ps1 -Half -HalfHead -FullSizes -ExpertFallbacks -PostFallback
build/tests/pass_regression.exe --expect-direct
build/tests/pass_regression.exe --copy-target
build/tests/pass_regression.exe --expect-direct --mixed-target
build/tests/pass_regression.exe --unknown-usage
```

The pass checks compare output, temporal history and feature bits against the
existing F32-input/F32-head baseline and exercise output capture. All four output
routes were rerun successfully on 2026-10-03.

## Follow-Up: Compact Model Aux Data

Status: built and verified on 2026-10-03. Compact aux bytes, bounds and stable
addresses passed; available original-reference graph/pass cases passed.

Model loading retains the original packed bytes on the CPU but no longer
uploads every complete tensor as `tensor.raw`. Graph declares the complete aux
ranges for each tensor before its first use. `Model::prepareAux` packs those
unchanged bytes on 16-byte boundaries into one model-owned buffer per used
tensor. Weight-only records have no aux buffer, while the existing execution
matrix and attention-prior caches continue to use the original CPU bytes.

All kernel-facing source offsets still refer to the original model layout.
`Tensor::auxOffset` translates each requested range to its compact GPU location;
`auxBuffer` supplies the matching buffer. This includes GEMM residual scales,
deferred expert projections from the previous block, window/global attention,
upsampling and pre/post fused blocks. No GPU kernel signature or arithmetic was
changed for this compaction. Direct users of the removed `Tensor::raw` field
must prepare and use the aux interface rather than treat the buffer as a whole
packed model record; Graph callers do not need to prepare it themselves.

Plans must be nonempty, nonoverlapping and within the CPU tensor. Repeating the
same plan, even in a different range order, reuses the existing buffer. Changing
a prepared plan is rejected instead of relocating an address that recorded
commands might still reference. Reads outside a declared range also fail before
recording a dispatch.

For the current model and hard-coded graph, source/layout arithmetic gives:

| Logical Buffer Payload | Bytes |
| --- | ---: |
| Previous whole-record backing, including 4-byte padding / 16-byte minimum | 147,683,904 |
| Compact aux, 119 buffers including 16-byte packing | 96,544 |
| Reduction target | 147,587,360 (about 140.75 MiB) |

The compact total comprises 1,632 bytes for the 32-channel stages, 26,304 for
expert stages, 33,792 for split-512, 33,792 for ViT and 1,024 for the decoder-512
transition. It is per model, independent of image resolution. It does not count
execution weights, activations, images, staging or allocation granularity, and
is not a new `VkAllocateMemory` or VRAM measurement. Use the resident-allocation
baseline, not the Graph-only byte count, to measure this change.

`aux_regression` checks that loading the model does not allocate GPU memory,
that compact GPU bytes and padding match the original source, that cached
addresses stay stable, and that invalid / overflowing / overlapping ranges and
foreign tensors are rejected. It covers nonzero source offsets and the ViT
scale range beginning at byte zero. In comparison mode, `test_graph.ps1
-AuxFallbacks` runs this test after independently verifying model hashes, then
adds tall-tile PTX GEMM and all-GLSL cases alongside the existing fused, unfused,
intermediate, pre/post and attention routes. These new cases still need
independent original references. The new workspace A/B tests exercise these
routes against current dedicated execution, but that is not an old-revision
arithmetic reference and does not fill the missing original-fixture gap.

For original-reference recording, `build_tests.ps1 -GraphOnly` builds just the
graph runner, avoiding the newer aux test API. The trace source is included when
present, so this mode can also build against older production revisions without
it. Keep reference captures separate from optimized results.

Additional deferred verification, after building matching shaders and supplying
the missing route references:

```powershell
scripts/build_tests.ps1
scripts/test_graph.ps1 -FullSizes -ExpertFallbacks -PostFallback -AuxFallbacks
scripts/test_graph.ps1 -Half -HalfHead -FullSizes -ExpertFallbacks -PostFallback -AuxFallbacks
```

## Whole-Buffer Workspace Reuse

`Graph::Options::reuseWorkspace` selects conservative whole-buffer slots for
the fused graph. Public allocations and borrowed input stay dedicated. A slot
has one Vulkan buffer, allocation, trace identity and stable device address;
logical Activation views do not own or free it. Captures, unfused execution,
disabled fusions and unsupported PTX routes explicitly fall back to dedicated
storage. `workspaceReport()` reports the route, capacities, addresses and the
closed lifetime interval assigned to each view.

Stage handovers use GPU execution dependencies, including final reads. This is
not CPU-record-order aliasing. Intra-stage counter chains remain intact. The
last decoder-32 state stays live through post; the head may reuse the retired
encoder skip, but not that decoder input. Frame entry also orders counter
clearing after earlier compute/transfer consumers. Head consumers must finish
before the next graph execution, on the same ordered queue.

Padding audit for the supported routes:

- Spatial producers cover the padded geometry; window kernels mask spatial
  boundary loads, and GEMMs mask or clamp out-of-range row loads and mask stores.
- ViT PTX normalization explicitly writes zero codes for padded tokens. The
  GLSL attention alternatives mask tokens before reading QKV/normalized data.
- Unwritten aligned tails of row-major tensors are not consumed as valid rows.
  Head producers write every valid full-field output element.
- Production reuse does not clear the workspace each frame. The test-only
  poison mode orders a nonzero fill before every new view's lifetime, after the
  previous occupant's final reads. No additional padding clear was required by
  the audit and tested routes.

The GPU workspace regression compares current dedicated execution with reuse
and poisoned reuse, including signed zero, subnormal and half-rounding inputs.
It checks exact head and borrowed-input bytes, stable allocations/addresses,
public allocation isolation, overlap rejection before dispatch, trace pointer
mapping, destruction, capture fallbacks and kernel-route switches. The original
independent graph fixtures remain a separate gate; `-ReuseWorkspace` never
records replacement references.

```powershell
scripts/build_tests.ps1
build/tests/workspace_plan_tests.exe
build/tests/workspace_regression.exe --width 513 --height 377 --trace
build/tests/workspace_regression.exe --width 3840 --height 2160 --fp16-head
scripts/test_graph.ps1 -ReuseWorkspace -Half -HalfHead -FullSizes
```

At 3840x2176 padded, the real GPU allocation ledger measured internal Graph
storage decreasing from 1,478,541,312 to 630,915,072 bytes (12 slots). The borrowed
F16 input contributes a separate unchanged 267,386,880 bytes. Including that
input, the Graph capacity is 856.6875 MiB rather than 1665.046875 MiB. These are
owned allocation bytes, not DXGI LOCAL residency or a timing claim.

### Complete-Pipeline Measurement: 2026-10-03

The accepted candidate is `0913c76-dirty-F788617FA625`, on RTX 4090 driver
610.88, adapter LUID `0000000000018798`. Its source/shader fingerprints stayed
unchanged throughout the measurement. The source manifest, immutable probe,
per-frame samples and results are retained under
`tmp/memory-lifetimes/measured-candidate-20261003/`. The old report and executables
under `build/vram-compare/` were not replaced.

The metric below is steady current-process DXGI LOCAL usage **minus the host's
pre-NR baseline**, in MiB. Candidate dedicated/reuse ran three alternating rounds,
20 frames each, for each size and each reset policy: reset every frame, and reset
only on the first frame. All rounds agreed exactly on memory usage. Both reset
policies produced the same table.

| Size | Original DLL (Historical) | 9370065 (Historical) | Current Dedicated | Current Reuse |
| --- | ---: | ---: | ---: | ---: |
| 1920x1080 | 750.5000 | 1083.5625 | 891.0625 | 675.3750 |
| 2560x1440 | 885.9375 | 1474.0625 | 1248.9375 | 883.5000 |
| 3840x2160 | 1322.4375 | 2623.9375 | 2302.5625 | 1493.8750 |

Original/9370065 columns are the preserved reset-every results, not newly run
first-reset measurements. Native and NrPass have different external input/output
formats; the host baselines exclude those allocations. Candidate A/B uses the
same binary, input conversion, controls and shader assets. It includes real
motion unpack, preprocess, F16 features, history and direct RGBA8 composite.

At 4K, reuse alone reduces LOCAL by 808.6875 MiB. Relative to 9370065, the total
reduction is 1130.0625 MiB (43.07%). Current reuse remains 171.4375 MiB (12.96%)
above the historical DLL result. In this workspace comparison, NON_LOCAL NR usage
was 257 MiB in every run; the 256 MiB staging window was unchanged. The subsequent
staging comparison below measures that window separately.

Do not confuse the following 4K reuse quantities:

- Internal logical view payload: 1,478,541,312 bytes, spread over reused lifetimes.
- Internal slot capacity: 630,915,072 bytes; Graph including F16 input: 898,301,952 bytes.
- All NrPass-owned Vulkan allocation bytes: 1,588,114,224, including staging.
- Owned device-local heap bytes: 1,319,678,752 (1258.5437 MiB).
- Measured LOCAL NR increment: 1,566,441,472 (1493.875 MiB). The 235.3313 MiB
  residual is not attributed to individual non-ledger Vulkan/driver objects and
  must not all be called reclaimable waste.
- Sampled runtime LOCAL peak equaled steady usage; samples occur after GPU frame
  completion and do not establish an exhaustive transient peak.

All 36 probe processes passed the 64 MiB canary, no-timeout and steady-allocation
checks; final full-size output SHA-256 matched dedicated/reuse in every size/reset
group. Separate direct/copy regressions compare exact output and both history
parities for six-frame sequences, including two pre-recorded, serialized queue
submissions without a CPU wait between them. External images remain fixed.

GPU timings are retained per run (five warmup frames, 15 timestamp samples).
Reset-every median-of-run-medians for dedicated/reuse was 5.142/6.660 ms at 1080p,
9.323/8.434 ms at 1440p and 18.298/18.088 ms at 4K; first-reset results were
6.115/5.159, 8.084/8.414 and 18.198/18.364 ms. The reversals and observed background
GPU activity do not support a speedup claim.

`NrPass` now defaults to reuse after these gates; its final constructor argument
can select dedicated execution for diagnosis. `Graph::Options` still defaults
to dedicated storage for other callers. Capture/unsupported routes fall back.

```powershell
build/tests/pass_regression.exe --workspace-check
build/tests/pass_regression.exe --workspace-check --copy-target
build/tests/pass_regression.exe --expect-workspace-default --expect-direct
scripts/test_memory.ps1 -Rounds 3 -Frames 20 -ResultsDirectory tmp/memory-lifetimes/new-measurement
```

`test_memory.ps1` builds its own `build/nr-memory/nr_memory_probe.exe` and matching
shader assets. `-BuildOnly` builds without measuring; `-SkipBuild` uses the saved
fingerprinted build. Reusing an existing results directory is rejected. Source
changes during a build and shader changes during measurement invalidate the run.

### Next Comparison Target: Community RTX 4090+ DLL

The user selected this local community-modified DLL on 2026-10-03 as the primary
target for subsequent comparisons:

- Path: `DLSS5VK_MODEL/rtx4090+/nvngx_dlssnr.dll`
- Size: 165,840,496 bytes
- File/product version: `310,8,0,0`
- SHA-256: `5f464e8753d1e121b23e07a87d95f5ad6b5ac0f4a6e523399dcd9fc7af9c4439`
- Authenticode status: `HashMismatch`. Its embedded NVIDIA certificate does not
  validate the modified file; this alone does not establish whether it is safe.

This DLL was not included in the Vulkan measurements above. The subsequent
D3D12 comparison below uses separate logs and retains the previous DLL as a
control; historical original-DLL numbers must not be relabeled as this variant.

## Smaller Persistent Staging: 2026-10-03

Each `vk::Context` now defaults to a 16 MiB host-visible, coherent staging
buffer. `DLSS5VK_STAGING_MIB` accepts a whole integer from 1 through 256, read
before Vulkan initialization in both owned and adopted Context constructors.
An absent variable selects 16; an empty, malformed or out-of-range value fails
early. Set it to 256 to restore the previous transfer window.

The existing synchronous upload/download chunk loops, barriers, allocation
lifetime and logical upload trace are unchanged. Normal NrPass frame recording
does not use this staging buffer. Model initialization and CPU-facing bulk
transfers do use it; shrinking the window increases submissions for transfers
larger than 16 MiB. This is a fixed-capacity change, not lazy allocation or a
new streaming allocator.

The accepted same-binary comparison is `0913c76-dirty-471F1A4B9361`, RTX 4090,
driver 610.88, retained under `tmp/staging/measured-20261003/`. Its immutable
probe, source/shader manifest and 36 raw logs cover three alternating rounds,
20 frames, three sizes and both reset policies, always with workspace reuse.
No source or shader drift occurred during the campaign. All processes passed
the canary, stable-allocation and output checks; each size/reset group has one
identical final-output SHA-256 across both capacities and all rounds.

Steady DXGI usage below is the NrPass increment after subtracting the host
baseline. Both reset policies and all rounds agreed exactly:

| Size | LOCAL, Either Capacity (MiB) | NON_LOCAL, 256 MiB Window | NON_LOCAL, 16 MiB Window |
| --- | ---: | ---: | ---: |
| 1920x1080 | 675.375 | 257 MiB | 17 MiB |
| 2560x1440 | 883.500 | 257 MiB | 17 MiB |
| 3840x2160 | 1493.875 | 257 MiB | 17 MiB |

The reduction is **240 MiB of NON_LOCAL usage per NrPass Context**, with no
measured LOCAL reduction. At 4K, owned allocation bytes fall from 1,588,114,224
to 1,336,455,984; Graph remains 898,301,952 bytes and the steady allocation count
remains 585. The probe's separate host Context also shrinks, but that saving
is outside this baseline-subtracted NR metric. These results do not measure
the community-modified DLL.

Timing medians below are ordered **256 / 16 MiB**. Initialization is fresh-process
NrPass construction wall time, including model/pipeline preparation and warmup;
it is not a guaranteed cold driver-cache measurement. GPU time discards the
first five frames of each run. The mixed timing changes do not establish a
steady-state speedup or a consistent initialization improvement.

| Size | Reset | Init (ms) | GPU Frame (ms) |
| --- | --- | ---: | ---: |
| 1920x1080 | Every | 1405.1 / 1483.0 | 5.097 / 5.098 |
| 2560x1440 | Every | 1441.0 / 1494.6 | 7.970 / 8.657 |
| 3840x2160 | Every | 1679.3 / 1367.8 | 18.526 / 18.338 |
| 1920x1080 | First | 1287.1 / 1328.2 | 5.684 / 5.150 |
| 2560x1440 | First | 1292.1 / 1539.7 | 8.625 / 9.043 |
| 3840x2160 | First | 1655.6 / 1315.0 | 18.093 / 17.279 |

Separate transfer tests at 16/64/256 MiB use two warmups and five timed samples
per payload in each of three alternating rounds. CPU wall time includes copying,
GPU submission/waiting and download vector allocation. Median-of-run-medians
from `tmp/staging/transfers-20261003/timings.json`:

| Payload | Upload, 256 / 64 / 16 MiB Window (ms) | Download, 256 / 64 / 16 MiB Window (ms) |
| --- | ---: | ---: |
| 4 MiB | 0.80 / 0.77 / 0.82 | 16.19 / 16.23 / 16.27 |
| 64 MiB | 17.17 / 10.28 / 18.61 | 271.09 / 262.75 / 269.07 |
| 256 MiB | 70.64 / 78.00 / 122.92 | 1055.23 / 1089.23 / 1100.00 |

Large uploads therefore have a real cost at 16 MiB in this measurement. Use
64/256 for bulk-transfer-heavy workloads instead of assuming the smaller
default is optimal for every Context caller.

The staging regression checks both Context constructors, actual allocation
capacity/heap ownership, nonrepeating data across chunk boundaries, offsets,
partial and zero-length transfers, trace bytes and stable allocation counters.
It runs the default plus 1/16/64/256 MiB overrides and ten invalid settings.

After switching the default, the rebuilt runners passed that matrix, planner,
allocation accounting, compact aux, 24 workspace cases, 38 original-reference
graph cases (dedicated/reuse), four temporal pass routes, both direct/copy queued
history A/B suites and the default-workspace gate. Compared with the frozen
measurement build, the only source change was the Context default constant;
shader assets remained identical. The full Filament demo, NGX integration, VR
and long-running game stability were not tested in this staging iteration.

```powershell
scripts/build_tests.ps1
scripts/test_staging.ps1 -Benchmark -LogDirectory tmp/staging/new-transfers
scripts/test_memory.ps1 -CompareStaging -Rounds 3 -Frames 20 -ResultsDirectory tmp/staging/new-pipeline
$env:DLSS5VK_STAGING_MIB = '256'  # Previous transfer capacity, for the launched process.
Remove-Item Env:DLSS5VK_STAGING_MIB # Return to the 16 MiB default.
```

## Community DLL D3D12 Comparison: 2026-10-03

The `rtx4090+` DLL loads in the existing Vulkan host but its `VULKAN_Init_Ext2`
returns `0xBAD00002` (`PlatformError`), before feature creation or inference.
The same Vulkan executable/input succeeds with the previous control DLL.
D3D12 initialization and evaluation succeed with both DLLs, so the requested
comparison uses D3D12. No DLL was patched or installed into a game.

Both files report version `310,8,0,0`, are 165,840,496 bytes and have
Authenticode `HashMismatch`; the previous control is also community-modified.
Their SHA-256 identities are:

- Previous control: `984bee0f775c277d5829b8fd6775d53a7b0f75396c852b3aaf06a18375f81014`
- RTX 4090+ variant: `5f464e8753d1e121b23e07a87d95f5ad6b5ac0f4a6e523399dcd9fc7af9c4439`

On RTX 4090 / driver 610.88, each batch ran three alternating-order rounds at
1080p, 1440p and 4K, with 120 frames per process and the first 20 excluded from
steady timing. Both DLLs receive the same generated RGBA16F gradient/checker
input, zero RG32F motion, automatic masking, style 0, intensity/tone/structure 1
and skin strength -1. History resets only on the first frame. This is a static
test workload, not a game capture or the photo input used in the Vulkan table.

The initial batch uses the existing D3D12 probe. A separate source copy adds
LOCAL/NON_LOCAL samples and load/init/create/first-frame timings, without
changing inference parameters or commands. Its endpoint matches the initial
probe. Each DLL pair within a batch uses the same executable; binaries stayed
unchanged during both batches. The 18 paired endpoint comparisons across the
36 processes are bit-exact over RGBA16F, including alpha, and each size has one
output hash across both DLLs, all rounds and both probe versions. This does not
establish equivalence for every input, intermediate frame or game feature.

These are **whole-process DXGI usage** values, including identical host textures,
upload/readback buffers and driver overhead. They are not baseline-subtracted
NR-only allocations. Three post-completion samples per diagnostic run agreed,
and LOCAL also agreed across both batches:

| Size | Control LOCAL (MiB) | RTX 4090+ LOCAL (MiB) | Control NON_LOCAL (MiB) | RTX 4090+ NON_LOCAL (MiB) |
| --- | ---: | ---: | ---: | ---: |
| 1920x1080 | 590.0625 | 590.6250 | 241.765625 | 241.890625 |
| 2560x1440 | 764.8750 | 765.4375 | 290.890625 | 291.015625 |
| 3840x2160 | 1302.8750 | 1303.4375 | 431.578125 | 431.703125 |

The new variant saves no measured process GPU memory here: it adds 0.5625 MiB
LOCAL and 0.125 MiB NON_LOCAL at every size. Neither these absolute values nor
their sum should be subtracted from the earlier Vulkan NrPass-only LOCAL value.

GPU times bracket `EvaluateFeature` and exclude caller uploads/readback. Values
are medians of the three run medians, ordered **control / RTX 4090+**:

| Size | Initial Batch (ms) | Diagnostic Batch (ms) |
| --- | ---: | ---: |
| 1920x1080 | 3.792 / 3.702 | 3.785 / 3.437 |
| 2560x1440 | 5.534 / 5.285 | 5.692 / 5.385 |
| 3840x2160 | 11.212 / 11.356 | 11.355 / 11.175 |

The lower-resolution medians favor the new variant, with a variable margin.
At 4K the direction reverses between batches, so these samples do not establish
a consistent 4K speedup. Background GPU activity was present; this is serialized
headless evaluation timing, not game FPS or an end-to-end frame budget.

Diagnostic-batch startup and CPU recording medians, again control / RTX 4090+:

| Size | NGX Init + Create/Submit (ms) | First GPU Evaluation (ms) | Steady CPU Recording (ms) |
| --- | ---: | ---: | ---: |
| 1920x1080 | 612.5 / 584.3 | 4.130 / 4.473 | 0.455 / 0.467 |
| 2560x1440 | 610.1 / 600.8 | 5.793 / 5.824 | 0.487 / 0.474 |
| 3840x2160 | 614.1 / 607.3 | 11.413 / 12.805 | 0.492 / 0.497 |

DLL loading itself was about 4 ms in both cases. Each run starts a new process,
but OS and driver caches were not cleared. The startup column excludes creating
the host D3D12 device and image resources. Memory samples are checkpoints, not
an exhaustive startup-peak trace. Neither DLL nor the inference implementation
was changed for this assessment.

Results and captures are retained under `tmp/community-dll/dx12-20261003/` and
`tmp/community-dll/dx12-diagnostics-20261003/`; `tmp/community-dll/summary.json`
combines them. The diagnostic source, build command, measurement-contract test
and executable hashes are under `tmp/community-dll/diagnostics/`. Vulkan failure
and working-control logs are under `tmp/community-dll/pilot-20261003/`.

## Current D3D12 Backend vs Community DLL: 2026-10-04

The current working tree was rebuilt without adding an optimization, then
packaged into an isolated candidate with its model, SPIR-V and PTX assets.
Source hashes agreed before/after the build and after measurement. The candidate
DLL SHA-256 is
`bdcdf93e20894dc7f6ba96ac978d6dc8b0b91019c9c072866a6a6fbc003922f4`.
Its source revision is `0913c76` plus the recorded working-tree changes. The
previous build was retained separately. The reference is the unchanged RTX
4090+ community DLL, SHA-256 `5f464e8753d1e121b23e07a87d95f5ad6b5ac0f4a6e523399dcd9fc7af9c4439`.

Both implementations use the same diagnostic D3D12 probe and static input
fixture described above, with three alternating rounds, 120 frames per process,
20 warmup frames and first-frame-only history reset. All nine endpoint pairs
across the 18 processes are RGBA16F bit-exact. Models, shaders, DLLs, probe and
captures retained their recorded hashes throughout the accepted run.

Results below are medians of the three per-run medians on RTX 4090 / 610.88.
LOCAL and NON_LOCAL remain whole-process DXGI usage including identical caller
resources, not NGX allocation statistics or baseline-subtracted Vulkan usage.

| Size | Community GPU (ms) | Ours GPU (ms) | GPU Time Increase | Community LOCAL (MiB) | Ours LOCAL (MiB) |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1920x1080 | 3.943 | 6.382 | 61.8% | 590.625 | 852.809 |
| 2560x1440 | 5.336 | 9.664 | 81.1% | 765.438 | 1248.809 |
| 3840x2160 | 11.973 | 21.287 | 77.8% | 1303.438 | 2404.934 |

| Size | Community / Ours NON_LOCAL (MiB) | Community / Ours Init + Create (ms) | Community / Ours First GPU Evaluation (ms) |
| --- | ---: | ---: | ---: |
| 1920x1080 | 241.891 / 95.262 | 608.8 / 3342.9 | 3.840 / 14.836 |
| 2560x1440 | 291.016 / 144.387 | 629.9 / 3316.2 | 5.062 / 22.934 |
| 3840x2160 | 431.703 / 285.074 | 631.9 / 3405.7 | 12.017 / 42.279 |

At 4K, ours uses 1101.496 MiB more LOCAL (84.5%) and 146.629 MiB less NON_LOCAL.
The lower shared-memory use does not offset the dedicated-memory requirement.
Steady CPU recording medians, community / ours, were 0.468 / 0.967 ms at 1080p,
0.467 / 0.952 ms at 1440p and 1.182 / 0.987 ms at 4K. DLL loading was about
3.8-4.3 / 0.5-0.6 ms. The startup column excludes host device/images and does
not imply cold OS or driver caches. Background activity and run-to-run timing
variation remain; these numbers are not game FPS or exhaustive peak-memory data.

Source inspection identifies a missing integration, not a measured fix:
`Dx12Feature` constructs `nr::Graph` with default options in
`ngx/d3d12_feature.cpp`; `Graph::Options::reuseWorkspace` defaults to false in
`src/nr_graph.h`. The earlier default enablement was in NrPass, not this D3D12
path. D3D12 therefore has not received that workspace-reuse saving. Reusing the
existing planner is a candidate next memory optimization, subject to native
D3D12 output/lifetime tests and remeasurement. It does not establish the cause
of the GPU-time gap or promise a corresponding speedup. No inference code was
changed during this benchmark.

The frozen candidate/reference, build/source/package manifests, raw logs,
endpoint captures and `summary.json` are under
`tmp/community-dll/our-dx12-20261004/`. Reproduce against those frozen assets with:

```powershell
tmp/community-dll/diagnostics/benchmark.ps1 `
  -ReferenceDll tmp/community-dll/our-dx12-20261004/reference/nvngx_dlssnr.dll `
  -ReplacementDll tmp/community-dll/our-dx12-20261004/candidate/nvngx_dlssnr.dll `
  -OutputDirectory tmp/community-dll/new-current-dx12-comparison `
  -Rounds 3 -Frames 120 -Warmup 20 -Cases 1080p,1440p,4k
```

Unset `DLSS5VK_*` and `OPEN_DLSS_NR_ROOT` overrides in the test process so the
candidate uses its adjacent frozen data directory and the measured defaults.

## D3D12 Workspace Reuse: 2026-10-04

`Dx12Feature` now passes the existing `reuseWorkspace` option into graph
construction. The command trace already identifies physical buffers, and the
D3D12 replay already preserves recorded dependencies as UAV barriers between
kernel batches. Neither the trace format nor the replay implementation needed
to change. PTX, arithmetic, F32 head format, input ownership and history resources
remain unchanged. `OPEN_DLSS_NR_WORKSPACE=0` selects the old dedicated route;
`1` explicitly enables reuse. Absence selects the new default. Invalid values,
including an empty value, fail feature creation with `InvalidParameter` before
the temporary Vulkan compiler or graph buffers are created.

The measurement candidate was built with the old default retained, then frozen
with its assets under `tmp/dx12-workspace/20261004/optin-candidate/`. Its SHA-256
is `f859abf8e34e4b12dc71c0dd574139599430ff23e8feea415d829e92513aa529`;
source manifest SHA-256 is `29aaebecd634e0b4315851dc2a6f0cc58c7260491d0bfba94e11da23883c864d`.
The first campaign selects `0` and `1` on that same DLL. A second pairs `1` with
the frozen RTX 4090+ community DLL. Both use the existing diagnostic probe,
three alternating rounds, 120 frames, 20 warmups, first-frame-only reset and the
same static input/controls as the preceding D3D12 measurements. No source or
asset drift was found during either campaign.

All 18 endpoint pairs across the two campaigns are bit-exact. Three steady
memory samples per process agree. These are whole-process DXGI figures,
including identical caller resources; LOCAL and NON_LOCAL are kept separate.

| Size | Dedicated LOCAL (MiB) | Reuse LOCAL (MiB) | Saved LOCAL (MiB) | NON_LOCAL, Either Route (MiB) |
| --- | ---: | ---: | ---: | ---: |
| 1920x1080 | 852.809 | 653.996 | 198.813 | 95.262 |
| 2560x1440 | 1248.809 | 912.121 | 336.688 | 144.387 |
| 3840x2160 | 2404.934 | 1659.996 | 744.938 | 285.074 |

The native D3D12 graph regression independently reports owned committed-buffer
capacity falling from 2,050,555,904 to 1,269,432,320 bytes at 4K, also a
781,123,584-byte (744.9375 MiB) reduction. Its 13 F32-head workspace slots have
764,608,512 bytes of capacity. This owned-buffer figure excludes history
textures, caller images and driver allocations; it is not the LOCAL total.

GPU/init values below are medians of run medians, ordered **dedicated / reuse**:

| Size | GPU Evaluation (ms) | NGX Init + Create/Submit (ms) |
| --- | ---: | ---: |
| 1920x1080 | 6.169 / 6.016 | 2950.4 / 2925.9 |
| 2560x1440 | 9.680 / 9.645 | 3011.9 / 2940.5 |
| 3840x2160 | 19.971 / 19.881 | 3138.2 / 3052.5 |

The timing differences are small and variable; the result supports a memory
reduction, not a material speedup. The separate community comparison measured:

| Size | Community / Reuse LOCAL (MiB) | Community / Reuse NON_LOCAL (MiB) | Community / Reuse GPU (ms) |
| --- | ---: | ---: | ---: |
| 1920x1080 | 590.625 / 653.996 | 241.891 / 95.262 | 3.666 / 6.134 |
| 2560x1440 | 765.438 / 912.121 | 291.016 / 144.387 | 5.187 / 9.266 |
| 3840x2160 | 1303.438 / 1659.996 | 431.703 / 285.074 | 11.119 / 19.746 |

Reuse leaves a 356.559 MiB LOCAL gap at 4K. Its lower NON_LOCAL usage does not
make that dedicated-memory requirement disappear. These are serialized static
workloads, not game FPS, a cold-cache startup test or an exhaustive peak trace.

Verification before changing the default included the real NGX allocation
policy/error tests, native D3D12 graph replay at 513x377/1080p/1440p/4K, and all
18 NGX comparison cases (75 captured frames per implementation). The graph
tests compare dedicated, reused and nonzero-poisoned storage with four distinct
input patterns, including signed zero and subnormal values. The last two graph
evaluations and their head readbacks share one command-list submission with no
CPU wait between them; this does not claim a new cross-queue lifetime contract.

The default was switched only after these output and residency gates. Raw
build/test logs, source and asset manifests, two measurement reports, captures
and summaries are retained under `tmp/dx12-workspace/20261004/`. The dedicated
fallback remains available for new feature instances; no live allocations are
relocated when the environment variable changes.

The rebuilt default-enabled DLL is
`8fff3995de95eb003712110889090461c28200a50a12f76e4a36521fe986d8f5`.
The final allocation/default/error checks, all 18 NGX cases, ABI/lifecycle and
multiple-instance checks, parameter/trace tests, dependent-kernel ordering,
native normalization, and legacy D3D12 replay at 512x512/513x377 passed. Reversing
only the absent-setting default constant reproduces the measured source hash;
model and shader assets remain identical. The tested final package is retained
as `default-candidate/`, with the final gate record in `final-verification.json`.

## D3D12 GPU Batch Profiling: 2026-10-04

This is a diagnostic experiment, not a new inference implementation. Production
source, the tested default-workspace DLL, its model/shader package, and the
community DLL remain unchanged. The throwaway probe and scripts are under
`tmp/dx12-profile/20261004/`; no profiler flag was added to the shipped DLL or
the normal build/test scripts.

The probe redirects only the loaded DLL's `GetProcAddress` import in the test
process. It records CUDA function names and places D3D12 timestamps around
existing `LaunchCuKernelChain` / `LaunchCuKernelChainEx` calls. It forwards the
original kernel array, arguments and count without splitting chains or adding
barriers. Therefore an interval enclosing multiple kernels measures that
entire batch, not each kernel independently. Timestamp results are UINT64
ticks; subtraction precedes conversion to milliseconds. See Microsoft's
[D3D12 timing description](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing)
for the bottom-of-pipe query semantics.

The accepted campaign uses RTX 4090 / driver 610.88, three alternating rounds,
120 frames per process and 20 warmup frames. Both implementations use the same
gradient/checker RGBA16F input, zero RG32F motion, normal controls and
first-frame-only reset. Each implementation is tested with profiling off and
on at 1080p, 1440p and 4K: 36 processes, 3,600 warm whole-frame samples, including
1,800 profiled warm frames. Profiling-off/on ordering and implementation
ordering alternate between rounds. No environment overrides are used.

The immutable DLL hashes are:

- Ours: `8fff3995de95eb003712110889090461c28200a50a12f76e4a36521fe986d8f5`.
- Community: `5f464e8753d1e121b23e07a87d95f5ad6b5ac0f4a6e523399dcd9fc7af9c4439`.
- Diagnostic probe: `da2d541f8f24516dac8ea7e0b1d695b1213b3fa4f15e09163e16a50edc332ea9`.

Whole-frame values below are medians of the three run medians. The final
column is the ratio of those medians, not a correction applied to phase times.

| Size | Implementation | Profiling Off (ms) | Profiling On (ms) | Observed Shift |
| --- | --- | ---: | ---: | ---: |
| 1920x1080 | Ours | 5.747 | 5.866 | +2.07% |
| 1920x1080 | Community | 3.442 | 3.507 | +1.86% |
| 2560x1440 | Ours | 9.180 | 9.217 | +0.41% |
| 2560x1440 | Community | 4.887 | 4.943 | +1.16% |
| 3840x2160 | Ours | 19.381 | 19.400 | +0.10% |
| 3840x2160 | Community | 10.302 | 10.529 | +2.20% |

Individual paired shifts range from -0.75% to +3.09%. The initial pilot's larger
shift is not substituted for the accepted alternating campaign. These are
fresh-cohort measurements of unchanged binaries, not a speedup over earlier
sections, and not game/VR FPS. CPU recording overhead is retained separately
in the raw report; it is not treated as GPU work.

For our 4K backend, the following shares use arithmetic means across all 300
profiled warm frames. This makes the decomposition additive; these means must
not be mixed with the whole-frame medians above.

| Recorded Region | Mean GPU Interval (ms) | Share |
| --- | ---: | ---: |
| Local Swin C64/C128/C256 batches, encoder + decoder | 8.218 | 42.44% |
| Frame prepare/compose + C32 neural batches | 6.126 | 31.64% |
| Global ViT main batch | 2.357 | 12.17% |
| C512/projection kernels, other calls and unattributed intervals | 2.663 | 13.75% |
| Total | 19.364 | 100.00% |

The local Swin aggregate comprises six existing batches: slots 5/108 (C64),
8/105 (C128) and 11/102 (C256). Their means are 2.570, 2.730 and 2.919 ms per
channel family. The largest single batch is slot 56, the 48-kernel ViT chain,
at 2.357 ms; `nr_prepare` is next at 2.119 ms. Those two and all six local Swin
batches remain the top eight individual intervals in every 4K round.

Community function families put roughly 3.970 ms in local Swin C64/C128/C256
and 2.842 ms in fused frame/C32 regions. This is only a coarse semantic
comparison: its fused input/output, view conversion and transition work do
not align one-to-one with our batches. The profile supports prioritizing local
Swin and frame/C32 regions; it does not establish the hardware cause of the
gap, such as occupancy, memory bandwidth or spills. Likewise the small
unattributed interval is not a standalone measurement of synchronization
cost: waits can be charged inside the surrounding timestamp interval.

All timestamp intervals are contained in their enclosing frame and
non-overlapping. The community trace retains 78 zero-length intervals out of
140,400 warm intervals; ours has none out of 101,700. Identical reported ticks
are retained as zero, not interpreted as proof that a kernel is free. No warm
frames are dropped. A validator accepts nonnegative intervals and rejects
reversed/out-of-frame timestamps, missing slots and incomplete function names.
The raw CSV statistics independently reproduce the probe's printed whole-frame
means/medians, and phase means sum to their corresponding complete frame.

All 18 baseline/instrumented pairs have bit-exact first and last frame captures,
as do the nine cross-implementation pairs. Initial three-frame profiled runs
match uninstrumented captures on both implementations. The new probe also
passes all 18 existing NGX output cases without instrumentation, covering 75
captured frames per implementation. Package, DLL, probe and production-source
hash guards pass. Early rejected runs are retained but excluded from these
figures.

The lowest-cost next experiment is a single-variable `nr_prepare` launch-layout
comparison, retaining every arithmetic instruction and output bit. Its current
8x8 launch and scalar half stores are candidates, not a proven bandwidth
diagnosis. Global ViT-only work cannot address most of the measured time, while
a broader Swin fusion rewrite has greater numerical and synchronization risk.
No such optimization was implemented during this experiment.

Accepted raw logs, CSVs and captures are in
`tmp/dx12-profile/20261004/campaign-accepted/`; `results.json` records the
unchanged identities and endpoint checks, and `analysis.json` records phase
means, medians, inventories and per-round rankings. Reproduce with:

```powershell
./tmp/dx12-profile/20261004/build.ps1
./tmp/dx12-profile/20261004/run.ps1 -OutputDirectory new-campaign
./tmp/dx12-profile/20261004/analyze.ps1 -Campaign new-campaign
```

## Prepare Launch Layout Experiment: 2026-10-04

The proposed `nr_prepare` 8x8 versus 32x4 experiment is complete. The candidate
is **not adopted**: it preserves output but provides no repeatable whole-frame
benefit, and its prepare interval is slower at all three measured sizes.

Only the throwaway probe under `tmp/dx12-prepare-layout/20261004/` changes. Its
NVAPI wrapper copies the single `nr_prepare` launch descriptor and changes its
grid/block dimensions. Exact covered pixel dimensions are required to remain
equal. Function handle, arguments, shared-memory request, barriers and every
other dispatch are unchanged. Both baseline and candidate install the same
hook and execute the same frozen DLL; the baseline explicitly selects 8x8.
PTX, model data, workspace selection and official source/build files do not
change. The frozen DLL remains
`8fff3995de95eb003712110889090461c28200a50a12f76e4a36521fe986d8f5`;
the experiment probe is
`e41e7f579bfbd8f58947708931231868bfaa3a0b0947b8000cbc51ad5a0799f8`.

The campaign repeats the previous three sizes, three alternating rounds,
120 frames with 20 warmups, fixed input/controls and first-frame-only reset.
Both layouts run with profiling off and on, for 36 processes. The primary
decision uses uninstrumented whole-frame GPU timing, not the sum of profiler
medians. Values are medians of the three per-run medians:

| Size | 8x8 Whole Frame (ms) | 32x4 Whole Frame (ms) | Candidate Change | 8x8 / 32x4 Prepare, Profiled (ms) |
| --- | ---: | ---: | ---: | ---: |
| 1920x1080 | 6.001 | 6.143 | +2.36% | 0.539 / 0.566 |
| 2560x1440 | 9.672 | 9.607 | -0.67% | 0.927 / 0.981 |
| 3840x2160 | 20.200 | 20.298 | +0.49% | 2.096 / 2.219 |

The small isolated 1440p whole-frame decrease is not sufficient evidence of a
useful optimization. It does not repeat across sizes and the changed region
itself gets slower. These are fresh-cohort timings; their difference from the
previous section is not a production regression or speedup.

Verification includes a failing selector test against the old probe, then
successful layout selection and unchanged dispatch/coverage checks. Unknown,
malformed and missing selector values fail before device creation. All 18
profiling-off/on endpoint pairs and all nine cross-layout endpoint pairs are
bit-exact at both first and final frames. The complete 18-case NGX output
matrix with 32x4 also matches the frozen community DLL, covering 75 captured
frames per implementation. Raw timestamp means/medians match the probe's
printed totals, phase means sum to the frame, and source, probe, DLL and
package hash guards pass. No claim is made about unmeasured intermediate
frames in the long timing runs, game FPS or VR performance.

As a separate read-only investigation, CUDA 13.0 `ptxas` assembled the unchanged
frame PTX for sm_89 into an inspection-only cubin. It reports 33 registers and
no stack, local memory or spills for `nr_prepare`; its disassembly still has
16 `STG.E.U16` stores. This cubin is not loaded by the benchmark, and offline
assembly is not proof of the NVAPI driver's exact runtime SASS. It supports
testing packed writes of already-rounded half values next, not claiming a
confirmed bandwidth bottleneck or an expected speedup.

Raw measurements and `analysis.json` are in
`tmp/dx12-prepare-layout/20261004/campaign/`; the 18-case output report is in
`output-matrix/results.json`. The original 8x8 production path remains intact.

## Prepare Packed Stores: 2026-10-04

After the negative launch-layout experiment, a separate temporary PTX variant
packs the sixteen already-rounded half values into two 128-bit stores. Its
floating-point instructions, all other PTX instructions, launch geometry,
arguments and DLL remain unchanged. This is a store-instruction experiment,
not a claim that a hardware bandwidth counter identified the bottleneck.

The temporary experiment repeats three alternating rounds at three sizes,
120 frames per process with 20 warmups, profiling off and on: 36 processes.
Uninstrumented whole-frame values below are medians of three run medians.
These are matched-cohort results, not comparisons with the previous section.

| Size | Scalar PTX (ms) | Temporary Packed PTX (ms) | Reduction |
| --- | ---: | ---: | ---: |
| 1920x1080 | 6.270 | 5.693 | 9.20% |
| 2560x1440 | 9.789 | 8.926 | 8.82% |
| 3840x2160 | 20.493 | 18.241 | 10.99% |

All nine matched uninstrumented comparisons improve, by 4.98% to 11.24%.
The first/final frame captures remain exact across variants and profiling
modes; kernel inventories and launch metadata match. The full 18-case native
NGX comparison also passes, covering 75 captured frames per implementation.
An independent D3D12/NVAPI test covers every 16-bit encoding in each of sixteen
component positions, including nonfinite encodings and signed zero. Tests
include 16-byte alignment, guard regions and partial final blocks. Evidence
is frozen in `tmp/dx12-packed-prepare/20261004/experiment-verification.json`.

### Production Source Validation

The user approved source adoption only after the experimental output matrix
passed. The only production source change is in `ngx/nr_frame.cu`: preserve
the original float expressions and `__float2half_rn` conversions, then pass
the converted values to `storeFeatures16`. This helper packs raw half bits
without additional floating-point arithmetic. Each feature pixel occupies
32 bytes in a GPU-allocation-aligned buffer; two `uint4` stores therefore
remain aligned even at odd image dimensions. The 8x8 dispatch is unchanged.

The original source fails the code-generation requirement with sixteen
scalar stores and no wide stores. Independently compiling the changed source
with the existing flags produces two `STG.E.128` stores, 33 registers, and no
stack or spills in offline sm_89 assembly. This is not a disassembly of the
NVAPI driver's runtime binary. The ordinary kernel build reproduces that
inspected PTX exactly. No other build asset changes, and `nr_compose` PTX is
byte-identical to the independently compiled original.

`scripts/test_ngx_prepare.ps1` compiles a wrapper around the actual production
helper and runs the exhaustive raw-bit tests on D3D12/NVAPI, with row counts
1, 65536 and 65537. All pass. A negative-control copy of its compiled PTX,
with the second store deliberately removed, fails the same test; the real
PTX passes again. This negative control is not shipped.

The normal production probe, without the diagnostic wrapper, passes all 18
native NGX output cases with the newly generated PTX, again covering 75
captured frames per implementation. The DLL remains
`8fff3995de95eb003712110889090461c28200a50a12f76e4a36521fe986d8f5`;
the production frame PTX is
`d284a197f12c67c6dd5fc61338bc352f055c452cbdfb7db95b7ce2484e33ab92`.
Detailed source-generation, unit-test and output evidence is under
`tmp/dx12-packed-prepare/20261004/production/`.

A fresh campaign measures the **production CUDA build**, not the temporary
hand-edited PTX. It uses the same 36-process design and passes all source,
DLL, probe and asset immutability guards. All nine matched uninstrumented
comparisons improve, by 5.68% to 9.37%. Acceptance timings are:

| Size | Original Source (ms) | Packed Source (ms) | Reduction | Original / Packed Prepare, Profiled (ms) |
| --- | ---: | ---: | ---: | ---: |
| 1920x1080 | 5.762 | 5.350 | 7.14% | 0.542 / 0.112 |
| 2560x1440 | 9.106 | 8.283 | 9.04% | 0.929 / 0.177 |
| 3840x2160 | 19.486 | 17.973 | 7.77% | 2.103 / 0.560 |

Whole-frame and prepare columns are medians of the three per-run medians.
Phase shares in `production/campaign/analysis.json` instead use arithmetic
means of all 300 warm samples. Raw timestamp statistics match the probe's
printed frame statistics and phase means sum to the whole frame. All 18
profiling-off/on endpoint pairs and all nine cross-version endpoint pairs
are exact, and all 252 named kernel launches in 113 chains retain their
order and geometry. Intermediate frames of the long timing runs are not
captured; those runs do not constitute exhaustive image coverage.

Only matched original/packed values within a campaign establish a reduction.
Absolute timings differ between cohorts, so the temporary-versus-production
tables are not evidence of a compiler regression or an additional speedup.
These are RTX 4090 D3D12 inference measurements, not game FPS or VR results.
No full Filament-demo or in-game end-to-end validation is claimed.

The optimization is adopted after the production gates pass. The ordinary
headless build succeeds, and thirteen supplementary suites pass: workspace
planning, NGX parameters/core, NVAPI dispatch, normalization, D3D12 graph
boundaries/workspace/policy, the eight-case graph matrix, pass regression,
workspace regression, compact auxiliaries and staging-policy tests. The first
temporary aggregate run falsely rejected the passing workspace-policy script
because its last native command was an expected negative test. A focused
RED/GREEN harness test fixes that classification; the complete aggregate run
then passes in `production/headless-verified/`. Existing policy tests and
production code are unchanged by this harness repair.

A separate source and call-site self-review finds no blocking issue: unsigned
bit packing preserves all half encodings, pixel stride preserves vector
alignment, writes remain within one pixel, and no arithmetic, dispatch or ABI
changes are introduced. No independent reviewer was used, per the user's
request not to start subagents. The scope audit confirms exactly one existing
production source changed, plus three new test/build-helper files.

The complete deliverable is
`build/packages/opendlss-nr-d3d12-0.1-packed-prepare-20261004.zip`, with SHA-256
`a07b35c1051e7b9f04fdb005e588f9ab9dec7cbcf8bbd4db194aacc1a91f1a7e`.
All 123 archive files match the unpacked validated package. Its packaged probe
runs from the package directory with asset-root and workspace overrides
cleared, passes ABI/capability checks, and produces three bit-exact odd-size
frames against the native reference. The package contains validation and
uninstrumented A/B reports.

The previous frozen package remains unchanged at
`tmp/dx12-workspace/20261004/default-candidate/`; its complete manifest is
rechecked for rollback. Deploy or roll back the **entire package**, not just
the unchanged DLL: this optimization resides in `opendlss-nr/build/ngx/nr_frame.ptx`.
No files were installed into a game, committed, pushed or published.

Reproduce the maintained store test and the frozen campaign with:

```powershell
./scripts/test_ngx_prepare.ps1
./tmp/dx12-packed-prepare/20261004/production/run.ps1 -OutputDirectory another-campaign
./tmp/dx12-packed-prepare/20261004/analyze.ps1 -Campaign production/another-campaign
```

After this change, prepare represents about 3.09% of the profiled 4K mean;
C32 kernels represent 19.85%, and the C64/C128/C256 local Swin groups together
represent about 46.17%. Further work should follow those newly measured
costs rather than assume prepare remains the primary target.

## C32 Index Division Experiment: 2026-10-04

This temporary candidate is **not adopted**. Correctness passes, but the
apparent gains in the short campaign do not reproduce as a stable whole-frame
improvement in a longer uninstrumented confirmation. The validated packed-
prepare package from the previous section remains unchanged.

The baseline is the complete
`build/packages/opendlss-nr-d3d12-0.1-packed-prepare-20261004/` package. Both
roles execute its same frozen DLL; only the candidate's asset root differs.
The overlay changes `block32_e4m3_f2/f48/f66/f130/f330.ptx`, replacing three
window-index quotient/remainder sites in each kernel. It reuses the existing
`swin.fast_divmod` correction sequence, computing the reciprocal once per
kernel invocation. Indices at or above `2^24` use the original integer
division/remainder path. Network arithmetic, rounding, prefetch order, launch
geometry, barriers and dependency-counter synchronization remain unchanged.

The transformation records reversible replacements. Removing those index
replacements and reciprocal setup reproduces each original PTX exactly;
all other model/shader assets retain their hashes. The actual candidate
blocks and the GPU index-test kernel use the same checked instruction
template. No production generator or runtime source is edited.

Correctness evidence includes:

- A real D3D12/NVAPI GPU test compared against independent CPU unsigned
  division and remainder. Its final run checks 504,201,319 pairs, including
  every `n < 2^24` for thirty divisors, the fast/fallback boundary, high
  unsigned indices, six large divisors, partial final blocks and guard words.
- The divisors cover all full- and half-resolution window widths observed
  in the output matrix, including the padded 512-pixel case's 36/37 and
  72/73 divisors, shifted windows, and the 4K 240/241 and 480/481 cases.
- The initial unwritten-output test fails as expected. A separate negative
  control removing the quotient correction also fails: `n=12582914, d=3`
  produces quotient 4194305 instead of 4194304. Restoring the tested template
  passes the full test again.
- All eighteen native NGX output cases remain bit-exact, covering 75 captured
  frames per implementation. This uses the existing diagnostic probe with
  profiling disabled, not a newly modified production probe.

Offline sm_89 assembly reports no stack or spills, but adds one or two
registers per kernel: 117/118, 127/128, 118/120, 125/127 and 118/120 for
baseline/candidate flags 2, 48, 66, 130 and 330 respectively. This is not
evidence of the exact NVAPI runtime SASS or a hardware bottleneck diagnosis.

The first campaign has three alternating rounds, three sizes, 120 frames and
20 warmups per process, profiling off and on: 36 processes. Values below are
medians of the three **uninstrumented per-run medians**. Negative change means
less GPU time; positive change means more GPU time.

| Size | Baseline (ms) | Candidate (ms) | Candidate Change |
| --- | ---: | ---: | ---: |
| 1920x1080 | 6.082 | 5.892 | -3.13% |
| 2560x1440 | 9.271 | 9.209 | -0.68% |
| 3840x2160 | 19.529 | 19.283 | -1.26% |

Those aggregate decreases are insufficient evidence. Individual 1080p and
1440p rounds change sign; the profiled C32 median at 4K increases from 3.650
to 3.804 ms. Profiled family means and per-round medians also vary. A longer
confirmation therefore retains the same binaries, assets and inputs but
runs 1200 frames with 200 warmups, three alternating rounds per size, and
profiling disabled throughout: 18 processes and 1000 warm samples per run.

| Size | Baseline (ms) | Candidate (ms) | Candidate Change |
| --- | ---: | ---: | ---: |
| 1920x1080 | 5.466 | 5.507 | +0.75% |
| 2560x1440 | 8.832 | 8.648 | -2.09% |
| 3840x2160 | 17.985 | 17.987 | +0.01% |

The longer candidate is slower in all three 1080p pairs and does not win any
4K pair. At 1440p the paired changes are +0.665%, -0.002% and -2.089%: only
the third round shows a material decrease. The aggregate 1440p number must
not be promoted as a repeatable improvement. The 4K +0.01% aggregate is
effectively unchanged, not evidence of a meaningful regression. No system
clock or power settings were changed, and no further kernel changes were
made between these cohorts.

Both cohorts retain exact first/final captures and pass source, DLL, probe
and asset immutability guards. The profiled campaign retains identical
252-kernel/113-chain launch inventories, raw timestamp statistics match
printed totals, and phase means sum to each frame. Intermediate frames in
the long timing runs are not captured. These are synthetic RTX 4090 D3D12
measurements, not game, VR, or other-GPU validation.

All candidate files remain throwaway artifacts under
`tmp/dx12-c32-index/20261004/`. `summary.json` retains both cohorts;
`campaign/analysis.json` retains phase data; `long-confirmation/results.json`
retains the longer confirmation; `output-matrix/results.json` and
`index-expanded-green.log` retain the correctness evidence. The production
source, built runtime assets and previous package are rechecked unchanged.
Nothing is committed, published or installed into a game.

Reproduce without overwriting previous runs:

```powershell
python ./tmp/dx12-c32-index/20261004/generate_variant.py --verify
./tmp/dx12-c32-index/20261004/build_index_test.ps1
./tmp/dx12-c32-index/20261004/index_test.exe ./tmp/dx12-c32-index/20261004/index-test.ptx
./tmp/dx12-c32-index/20261004/run.ps1 -OutputDirectory another-campaign
./tmp/dx12-c32-index/20261004/analyze.ps1 -Campaign another-campaign
./tmp/dx12-c32-index/20261004/run_long.ps1 -OutputDirectory another-long-confirmation
```

## FFN Initial Prefetch Wait Experiment: 2026-10-04

This temporary candidate is **not adopted**. All compared outputs are
bit-exact, but the long campaign does not establish a repeatable improvement
across resolutions. The validated packed-prepare package remains unchanged.

Both roles use the frozen DLL from
`build/packages/opendlss-nr-d3d12-0.1-packed-prepare-20261004/`. Only the
candidate asset root differs. In `ffn_e4m3_C64_R4_proj.ptx` and
`ffn_e4m3_C128_R4_proj.ptx`, the first `cp.async.wait_group 0` becomes
`cp.async.wait_group 1`. Every later wait and CTA barrier is unchanged, as
are arithmetic, register declarations, launch geometry and shared-memory
layout. Each candidate file differs from its baseline by exactly one byte;
all other asset files are identical. No production generator is edited.

At that first wait, group 0 loads the current projection and group 1
prefetches future W1 weights. NVIDIA's
[PTX wait-group specification](https://docs.nvidia.com/cuda/archive/12.8.0/parallel-thread-execution/index.html#data-movement-and-conversion-instructions-cp-async-wait-group-cp-async-wait-all)
allows the newest group to remain pending with `wait_group 1`, while older
groups must complete. The unchanged CTA barrier then precedes shared reads.
This ordering argument does not imply a measured stall reduction.

The temporary verifier evaluates actual emitted PTX address and predicate
dependencies for every thread, rejecting unknown or reassigned inputs.
All three first-step `ldmatrix` instructions read only group 0. Group 1
does not overlap either projection staging region, and the original second
`wait_group 0` drains all groups before subsequent consumers. Shared byte
ranges below are half-open offsets from `smem`:

| Kernel | Threads | Group 0 | Group 1, Future W1 | Group 2, Next Projection |
| --- | ---: | --- | --- | --- |
| C64 R4 projected | 256 | [0, 4096) | [10240, 18432) | [4096, 8192) |
| C128 R4 projected | 512 | [0, 6144) | [18432, 34816) | [6144, 12288) |

The candidate-identity guard rejects the unmodified copies before the
experiment. It also catches newline changes introduced by the text patch;
restoring the baseline newline convention leaves only the two intended
operand bytes. This guard establishes a controlled experiment, not a
functional defect in the baseline. Offline sm_89 assembly succeeds with
74/80 registers and no stack or spills, unchanged from baseline. That is
not evidence of the NVAPI runtime's exact SASS.

Correctness checks include 64 native D3D12 graph evaluations at 513x377,
1920x1080, 2560x1440 and 3840x2160. Four changing inputs include negative
values, signed zero and subnormals. Candidate reuse, dedicated allocation
and poisoned reuse are compared against an independently replayed frozen
baseline: all 48 complete head-buffer comparisons are exact. The last two
evaluations and their readback consumers share a single command-list
submission without an intervening CPU wait. Buffer addresses and reuse
capacity remain stable.

All eighteen native NGX cases also pass, with 75 captured frames per
implementation. A separate 512x512 profiled smoke test retains identical
244-kernel/113-chain launch inventories and exact first/final captures.
The two modified kernels are exercised six and ten times per frame. These
short profiled runs verify execution routes; their timings are not used to
accept the optimization.

The initial aggregate checker stops after the passing output matrix because
its copied diagnostic validator recognizes only the benchmark completion
message, not the reference-comparison message. A focused test reproduces
the false rejection. The temporary validator is repaired and passes five
positive/negative fixtures, including a mismatched comparison and missing
metadata. All eighteen existing matrix logs are then revalidated and the
remaining smoke checks completed, without repeating the captured GPU work.
The probe executable and production code are unchanged.

The performance campaign uses three alternating baseline/candidate rounds
per size, 1200 frames with 200 warmups per process, and profiling disabled
throughout: 18 processes and 18,000 warm samples. The baseline and candidate
columns are medians of three per-run GPU medians; negative change means
less GPU time.

| Size | Baseline (ms) | Candidate (ms) | Candidate Change | Paired Changes, Rounds 1 / 2 / 3 |
| --- | ---: | ---: | ---: | --- |
| 1920x1080 | 5.801 | 5.792 | -0.17% | -1.714% / -2.496% / -0.165% |
| 2560x1440 | 9.028 | 9.021 | -0.08% | +2.827% / +0.093% / -0.292% |
| 3840x2160 | 19.001 | 19.077 | +0.40% | -0.804% / -2.055% / +3.310% |

The candidate is faster in all three 1080p pairs, but the third difference
is only 0.165%. At 1440p it loses two pairs; at 4K the third round reverses
the first two gains. The small aggregate differences do not establish a
general speedup or a definite regression. No clock or power settings were
changed. These are synthetic RTX 4090 D3D12 results, not game FPS, VR or
other-GPU validation.

Every long-run first/final output pair is exact. Intermediate timing-run
frames are not captured. Source, DLL, probe and complete asset guards pass;
all 102 workspace built assets checked also match the frozen package.
The existing package ZIP retains SHA-256
`a07b35c1051e7b9f04fdb005e588f9ab9dec7cbcf8bbd4db194aacc1a91f1a7e`.
No files are installed into a game, committed, pushed or published.

Evidence remains under `tmp/dx12-ffn-prefetch/20261004/`:
`variant-manifest.json` contains the address proof and asset hashes;
`correctness/` contains captures and queued-execution logs;
`long-confirmation/results.json` contains all run and pair statistics;
`summary.json` and `final-audit.json` record the final checks. Recheck or
repeat the uninstrumented campaign without overwriting earlier runs:

```powershell
python ./tmp/dx12-ffn-prefetch/20261004/verify_variant.py
./tmp/dx12-ffn-prefetch/20261004/test_diagnostics.ps1
./tmp/dx12-ffn-prefetch/20261004/run_long.ps1 -OutputDirectory another-long-confirmation
./tmp/dx12-ffn-prefetch/20261004/summarize.ps1 -Campaign another-long-confirmation
```

## Driver 617.14 Rebaseline: 2026-10-04

The user updated the RTX 4090 driver from 610.88 to 617.14. The loaded
version is checked with `nvidia-smi`, including before every measured process.
This campaign compares the existing community reference DLL with the
unchanged current packed-prepare package. Neither implementation is rebuilt;
the model, PTX, shaders, controls and diagnostic probe retain their hashes.
No previously rejected C32 or FFN candidate is used.

The campaign runs three alternating rounds at each of three sizes, with
1200 frames and 200 warmups per process. All 18 processes run without batch
profiling or a hardware profiler, yielding 18,000 warm samples. Input remains
the generated RGBA16F gradient/checker image with zero RG32F motion and reset
on the first frame only. GPU timestamps measure serialized D3D12 evaluation,
excluding caller input upload and output readback. These are not game FPS.

All eighteen output cases pass on 617.14, covering 75 captures per
implementation. The nine long-run first/final pairs are also bit-exact.
For our unchanged package, all nine first/final pairs match the historical
610.88 captures as well. Intermediate frames of timing runs are not saved.

The following values are medians of three per-run medians. Positive GPU
change means our implementation takes more GPU time than the reference.
Memory values are current-process DXGI usage, including identical caller
resources; they are not an NR-only increment or an exhaustive peak.

| Size | Reference GPU (ms) | Our GPU (ms) | Extra GPU Time | Reference LOCAL (MiB) | Our LOCAL (MiB) |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1920x1080 | 3.553 | 5.727 | +61.17% | 590.625 | 653.996 |
| 2560x1440 | 5.238 | 8.941 | +70.69% | 765.438 | 912.121 |
| 3840x2160 | 11.037 | 18.496 | +67.57% | 1303.438 | 1659.996 |

Our paired GPU-time increases are 53.95%/55.19%/68.91% at 1080p,
70.69%/68.03%/68.23% at 1440p, and 66.84%/70.45%/68.52% at 4K. The remaining
gap is present in every pair, despite variation in absolute timings.

Startup and recording costs are measured independently:

| Size | CPU Record, Reference / Ours (ms) | Init + Create, Reference / Ours (ms) | First GPU Frame, Reference / Ours (ms) | NON_LOCAL, Reference / Ours (MiB) |
| --- | --- | --- | --- | --- |
| 1920x1080 | 0.485 / 0.465 | 575.83 / 2945.81 | 4.357 / 15.286 | 242.016 / 95.137 |
| 2560x1440 | 0.497 / 0.405 | 573.26 / 2877.36 | 6.407 / 18.696 | 150.262 / 144.262 |
| 3840x2160 | 0.592 / 0.409 | 587.83 / 2948.38 | 11.062 / 34.958 | 290.949 / 284.949 |

Init + Create is summed within each run before taking the median. It excludes
DLL loading and caller D3D setup. DLL-load medians range from 3.69 to 3.99 ms
for the reference and 0.70 to 0.74 ms for ours. Each process is new, but OS
and driver caches are retained: these are not fully cold-machine startup
measurements. First-frame values have only three observations per role and
size and should not be treated as stable latency guarantees. All three
post-completion memory samples agree within each run. LOCAL and NON_LOCAL
must remain separate; shared-system-memory reductions do not offset LOCAL
GPU-memory pressure.

For historical context, the earlier FFN experiment's **baseline-only** runs
used this same packed-prepare package, probe, input and 1200/200 frame counts:

| Size | Our GPU, Driver 610.88 (ms) | Our GPU, Driver 617.14 (ms) | Observed Change |
| --- | ---: | ---: | ---: |
| 1920x1080 | 5.801 | 5.727 | -1.28% |
| 2560x1440 | 9.028 | 8.941 | -0.96% |
| 3840x2160 | 19.001 | 18.496 | -2.66% |

Our LOCAL usage is unchanged at all three sizes. Init + Create medians were
3186.36/2969.82/3139.79 ms in that earlier cohort, compared with
2945.81/2877.36/2948.38 ms here. The raw history also preserves first-frame
and CPU-record values. These comparisons are observations across cohorts,
not a controlled causal driver experiment: collection time, comparison
partner, thermal state, caches and background activity differ. They do not
establish a DLSS 5-specific speedup. Earlier optimization A/B percentages
remain specific to their original driver and campaign; this rebaseline does
not rerun every historical candidate.

Source fingerprints, both DLLs, the probe, the complete frozen package and
all 102 checked workspace built assets remain unchanged. All 150 matrix
captures and 36 timing captures are rehashed. The formal ZIP still has
SHA-256 `a07b35c1051e7b9f04fdb005e588f9ab9dec7cbcf8bbd4db194aacc1a91f1a7e`.
No files are installed into a game, committed or published.

Evidence is under `tmp/dx12-driver-baseline/617.14-20261004/`:
`output-matrix/`, `long/results.json`, `long/runs.json`, `summary.json`,
`cross-driver-endpoints.json` and `final-verification.json`. Reproduce on
driver 617.14 without overwriting the accepted run:

```powershell
./tmp/dx12-driver-baseline/617.14-20261004/benchmark.ps1 -OutputDirectory tmp/dx12-driver-baseline/617.14-20261004/another
./tmp/dx12-driver-baseline/617.14-20261004/analyze.ps1 -Campaign another
```

### Hardware Capture Status

Nsight Graphics 2026.2.0 is installed from an NVIDIA-signed official MSI,
with only the main host/target components. Version 2026.2 was selected while
610.88 was still installed because its
[documented minimum](https://archive.docs.nvidia.com/nsight-graphics/2026.2/ReleaseNotes/index.html)
is 591.86, whereas
[2026.3 requires R615](https://docs.nvidia.com/nsight-graphics/ReleaseNotes/index.html).
The driver upgrade was performed by the user. The
installer does not change the formal package, profiling-access policy or
TDR configuration.

The capture setup exposes three pre-launch issues: Qt consumes a separated
`--platform` argument, `Windows` is not this CLI's complete platform name,
and the output directory must already exist. A full negative-control launch
with a nonexistent executable verifies the final syntax without GPU work.
The working platform argument is the single token
`--platform=Windows (x86_64)`. Metric set 0 disables the requested real-time
shader profiler; offline validation selects Ada Throughput Metrics, set 1,
which supports it. No inference or kernel changes are made for these fixes.

One actual elevated GPU Trace collection then succeeds, with clocks
unaltered, screenshots and HES disabled, and a limit of five submits or
500 ms. However, its submit-count trigger fires **before CreateFeature
returns**, during initialization. The log contains no EvaluateFeature call.
The exported data has 221 device-wide metrics, but no D3DPERF event rows or
regime rows, and CUDA/NGX active-warp and Tensor-pipe metrics are zero in this
interval. These values must not be interpreted as FFN utilization or proof
of a forward memory bottleneck.

The CLI terminates its target after exporting the trace, so the wrapper's
final-inference-image gate fails. This capture is **rejected for forward
diagnosis** and is not mixed into the accepted uninstrumented benchmark.
No additional hardware capture is attempted. A future attempt needs a
verified post-initialization trigger or explicit forward-region marker;
global submit counts are insufficient for this application.

The trace and coverage assessment are retained under
`tmp/dx12-ffn-profile/20261004/driver-617.14-verified/`. The tracked profiling
and TDR registry settings still match the pre-install snapshot; a final
non-elevated CUPTI query still reports insufficient counter privileges.
No persistent global counter access is enabled, and no probe or CLI process
is left running.

## RTX 40-Adapted Original Baseline: 2026-10-04

The user identified `DLSS5VK_MODEL/nvngx_dlssnr.dll` as the original runtime
with only RTX 40-series compatibility adaptation. This campaign uses that
exact file, SHA-256
`984bee0f775c277d5829b8fd6775d53a7b0f75396c852b3aaf06a18375f81014`,
instead of the separate `rtx4090+` variant used in the preceding campaign.
The stated patch scope comes from the user; it has not been independently
audited against an unmodified NVIDIA binary. Authenticode reports
`HashMismatch`, which establishes neither the patch scope nor a precision
change. The source file and the frozen test copy retain the same hash.

Tests run on the RTX 4090 with driver 617.14. The replacement is the same
packed-prepare package, DLL SHA-256
`8fff3995de95eb003712110889090461c28200a50a12f76e4a36521fe986d8f5`.
The diagnostic probe, inputs and controls are unchanged. Process-local
experimental overrides are cleared for testing and restored afterward.
No hardware profiler, clock change, kernel edit or rebuild is used.

All 18 output cases pass, with 75 RGBA16F captures per implementation.
The performance campaign has three alternating reference/replacement rounds
per size, 1200 frames and 200 warmups per process: 18 processes and 18,000
warm samples. Every first/final capture pair is bit-exact. The table reports
the median of three per-run GPU medians, with process-local DXGI memory
measured after GPU completion.

| Size | Original GPU (ms) | Our GPU (ms) | Extra GPU Time | Original LOCAL (MiB) | Our LOCAL (MiB) |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1920x1080 | 3.749 | 5.527 | +47.43% | 590.063 | 653.996 |
| 2560x1440 | 5.232 | 8.855 | +69.25% | 764.875 | 912.121 |
| 3840x2160 | 11.139 | 18.237 | +63.73% | 1302.875 | 1659.996 |

Our GPU-time increases in paired rounds 1/2/3 are 50.28%/58.39%/42.97%
at 1080p, 69.25%/66.99%/68.56% at 1440p, and 70.73%/60.63%/63.67% at 4K.
Our implementation is slower in every pair. LOCAL usage is higher by
63.934/147.246/357.121 MiB. Both roles include identical caller resources;
these values are neither NR-only increments nor exhaustive memory peaks.
GPU timestamps cover serialized D3D12 evaluation, excluding input upload
and output readback. They do not measure game FPS.

| Size | CPU Record, Original / Ours (ms) | Init + Create, Original / Ours (ms) | First GPU Frame, Original / Ours (ms) | NON_LOCAL, Original / Ours (MiB) |
| --- | --- | --- | --- | --- |
| 1920x1080 | 0.475 / 0.387 | 583.25 / 2943.39 | 4.510 / 11.883 | 241.891 / 95.137 |
| 2560x1440 | 0.478 / 0.409 | 585.65 / 2879.90 | 5.265 / 19.352 | 150.137 / 144.262 |
| 3840x2160 | 0.532 / 0.513 | 593.11 / 2973.25 | 11.323 / 34.327 | 290.824 / 284.949 |

Init + Create is summed within each run before taking the median. It
excludes DLL loading and caller D3D setup. DLL-load medians range from
3.70 to 3.92 ms for the original and 0.57 to 0.74 ms for ours. Processes
are fresh but OS and driver caches remain populated. Each first-frame
median has only three observations. Three steady memory samples agree
within every run; NON_LOCAL remains distinct from LOCAL GPU-memory usage.

The earlier 617.14 community campaign measured GPU medians of
3.553/5.238/11.037 ms at these sizes. It was collected at a different time,
so comparison with this original is not a directly interleaved A/B test.
Those timing differences cannot establish an optimization benefit or a
precision/performance tradeoff. Physical rehashing does establish that all
75 matrix captures and all 18 long-run endpoint captures from this original
match both our current outputs and the earlier community reference outputs.
The saved fixtures show no output difference. They do not prove identical
internal precision, kernels, or results on untested inputs; intermediate
frames of the timing runs are not saved.

All 93 recorded source-file hashes, the helper/probe hashes, both reference
copies, and the complete package manifest pass verification. The formal ZIP
remains `a07b35c1051e7b9f04fdb005e588f9ab9dec7cbcf8bbd4db194aacc1a91f1a7e`.
No files are installed into a game, committed or published.

Evidence is in `tmp/dx12-original-baseline/617.14-20261004/`:
`setup.json`, `output-matrix/`, `long/results.json`, `long/runs.json`,
`summary.json`, `matrix-capture-audit.json`, `long-capture-audit.json` and
`final-verification.json`.
To repeat the timing campaign without overwriting these captures, explicitly
select the frozen original rather than the copied runner's old default:

```powershell
$lab = 'tmp/dx12-original-baseline/617.14-20261004'
$package = 'build/packages/opendlss-nr-d3d12-0.1-packed-prepare-20261004'
& "$lab/benchmark.ps1" -ReferenceDll "$lab/reference/nvngx_dlssnr.dll" `
  -ReplacementDll "$package/nvngx_dlssnr.dll" -OutputDirectory "$lab/another-long" `
  -Rounds 3 -Frames 1200 -Warmup 200 -Cases 1080p,1440p,4k -ExpectedDriverVersion 617.14
& "$lab/analyze.ps1" -Campaign another-long
```

## Workspace Capacity Growth Candidate: 2026-10-04

This experiment keeps production source and the current formal package
unchanged. The only implementation change is in the copied
`tmp/dx12-workspace-growth/20261004/src/nr_workspace.cpp`, with added tests
in that directory's `tests/workspace_plan_tests.cpp`. The candidate DLL is
`50ffebb69ddc14dc2929aa172bf68862f3aa610b3b5e15b1f791de5bc0b78856`;
the current-package baseline remains
`8fff3995de95eb003712110889090461c28200a50a12f76e4a36521fe986d8f5`.
Model data, all runtime PTX/shaders, numerical formats, lifetime domains and
barriers are unchanged. Testing uses the RTX 4090 and driver 617.14.

The original planner reuses only expired slots that are already large
enough. A later, larger tensor therefore adds another allocation even when
an earlier slot could have been sized for both uses before resource creation.
The candidate builds two plans: the original best-fit placement and a
placement that may increase an expired slot's planned capacity. It chooses
the least additional capacity, validates both plans, and selects growth only
when total planned storage is strictly smaller. Ties retain the original
placement. No live GPU resource is resized or relocated.

The first new test fails against the original planner: disjoint 64-byte and
128-byte requests consume 192 bytes instead of sharing 128 bytes. It passes
after the change. Further cases check closed interval boundaries, dedicated
allocations, deterministic placement and least-growth selection. A separate
counterexample would make unconditional growth consume 256 bytes instead
of the original 192; the fallback keeps the smaller original plan.

For the D3D12 F32-head graph, workspace slots decrease from 13 to 12. At 4K,
workspace capacity falls from 764,608,512 to 697,761,792 bytes, while the
568,197,120-byte live-request peak is unchanged. Real graph-owned committed
D3D12 buffers fall from 1,269,432,320 to 1,202,585,600 bytes. F16-head plans
at the tested sizes keep their original capacity. The borrowed input,
history textures and persistent model storage are not included in this reuse.

### Measured Memory

The main A/B campaign runs three alternating rounds at each size, 1200
frames with 200 warmups per process. All 18 processes run without profiling.
Every pair reproduces the following LOCAL reduction, with no NON_LOCAL
increase. These are whole-process DXGI values, including caller resources,
not model-only usage or an exhaustive peak.

| Size | Current Package LOCAL (MiB) | Candidate LOCAL (MiB) | Saved LOCAL (MiB) | Reduction | NON_LOCAL, Either (MiB) |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1920x1080 | 653.996 | 637.121 | 16.875 | 2.58% | 95.137 |
| 2560x1440 | 912.121 | 883.371 | 28.750 | 3.15% | 144.262 |
| 3840x2160 | 1659.996 | 1596.246 | 63.750 | 3.84% | 284.949 |

The original-reference cohort measured 1302.875 MiB LOCAL at 4K, so this
candidate still uses about 293.371 MiB more in the same test configuration.
That reference was measured earlier; this campaign directly interleaves
the current package and the candidate, not the NVIDIA reference.

### Timing Limits

GPU values below are medians of three per-run medians, each based on 1000
warm samples. Negative change means less time. The measurements cover
serialized D3D12 evaluation and exclude input uploads and output readback.

| Size | Current Package GPU (ms) | Candidate GPU (ms) | Change | Paired Changes, Rounds 1 / 2 / 3 |
| --- | ---: | ---: | ---: | --- |
| 1920x1080 | 5.764 | 5.962 | +3.44% | -2.292% / +4.547% / +0.098% |
| 2560x1440 | 9.063 | 9.044 | -0.21% | +0.364% / -1.382% / -0.147% |
| 3840x2160 | 18.941 | 18.722 | -1.16% | -0.011% / -1.833% / +0.252% |

The noisy 1080p result prompted one targeted confirmation, with the same
1200/200 protocol and the opposite pair order. Its three baseline/candidate
medians are 5.570/5.561 ms overall (-0.15%), with paired changes of
-0.152%/-0.543%/+1.710%. All three pairs again save 16.875 MiB LOCAL. Across
the two cohorts, baseline-first and candidate-first orders each occur three
times; the median of all six paired changes is -0.027%. Both cohorts remain
reported rather than replacing the unfavorable first aggregate. These
observations do not establish a general speedup or a stable warm-evaluation
regression.

| Size | CPU Record, Baseline / Candidate (ms) | Init + Create, Baseline / Candidate (ms) | First GPU Frame, Baseline / Candidate (ms) |
| --- | --- | --- | --- |
| 1920x1080 | 0.566 / 0.572 | 2779.34 / 2777.65 | 12.495 / 12.875 |
| 2560x1440 | 0.659 / 0.577 | 2791.39 / 2809.63 | 9.618 / 19.785 |
| 3840x2160 | 0.648 / 0.598 | 2832.74 / 2819.75 | 32.175 / 44.126 |

Startup remains approximately 2.8 seconds. Init + Create is summed per run
before taking the median and excludes DLL loading and caller D3D setup.
Processes are fresh, but OS/driver caches and GPU clocks are not reset or
controlled. First-frame timing is a concern rather than an improvement:
the candidate's first evaluation is slower in all three 1440p and 4K pairs,
including one 119.854 ms 4K observation. There are only three first-frame
samples per role and size, so no startup or first-frame latency guarantee
is claimed. The candidate stays unpromoted; steady memory savings alone
do not establish an across-the-board reduction in overhead.

### Validation

Accepted coverage includes 34 distinct supplementary tests, 22 comparisons
with independent original Vulkan graph captures, and all 18 original-DLL
NGX cases (75 captured frames per implementation). Tests cover nonzero-poisoned
workspace reuse, borrowed-input preservation, fixed addresses, F16/F32
inputs and heads, ownership/destruction, route fallbacks, output/history
composition, ABI/lifecycle and workspace-policy errors. Native D3D12 replay
checks four changing inputs per size, including two evaluations and their
head consumers queued in one submission.

All twelve long-run first/final pairs, including the 1080p confirmation,
are bit-exact. Their candidate endpoints also match the earlier original
reference's saved endpoints. Intermediate timing-run frames are not saved.
There are 24 measured processes and 24,000 warm samples in total. These are
synthetic tests, not validation of a game's complete integration or other GPUs.

Two temporary test-setup failures are retained with their logs. PowerShell
7.6.5 restores a null value through `SetEnvironmentVariable` as a present
empty variable, inadvertently disabling PTX routes after fallback tests.
A failing environment-restoration test reproduces this; explicit removal
fixes it while preserving intentional empty/nonempty values. A later legacy
D3D12 replay test needs two PTX files that are absent from the native release
package. Matching copies are added only to the isolated test assets. Affected
and remaining tests then pass without changing the candidate implementation.
The existing `nr_model.cpp:131` C4458 warning is recorded and left unchanged.

All 93 production-source hashes, 114 model/runtime asset hashes, reference
identity, candidate sources, measurement helpers, and package manifests are
checked. The formal ZIP remains
`a07b35c1051e7b9f04fdb005e588f9ab9dec7cbcf8bbd4db194aacc1a91f1a7e`.
No production source, game installation or formal package is replaced.

Evidence is under `tmp/dx12-workspace-growth/20261004/`: `variant.json`,
`red-verification.json`, `logs/`, the three `correctness*` directories,
`accepted-tests.json`, `long/`, `confirmation-1080p/`, `summary.json`,
and the capture-audit JSON files. The runtime candidate is in `candidate/`.
Its copied `benchmark.json` belongs to the baseline release; use the parent
directory's `summary.json` for this experiment. Recheck saved evidence with:

```powershell
./tmp/dx12-workspace-growth/20261004/test_environment.ps1
./tmp/dx12-workspace-growth/20261004/build/tests/workspace_plan_tests.exe
./tmp/dx12-workspace-growth/20261004/analyze_confirmation.ps1
```

## Workspace Candidate First-Frame Diagnosis: 2026-10-04

The approved follow-up is a short diagnostic experiment, not another runtime
optimization or a release promotion. It asks whether the preceding candidate's
4K first-evaluation regression repeats consistently, and how many early frames
are affected. The formal DLL (`8fff3995...`) and workspace-growth candidate
(`50ffebb6...`) are unchanged, including their precision, kernels and assets.

Four fresh-process pairs run at 3840x2160, 30 frames each, in the fixed order
baseline/candidate, candidate/baseline, baseline/candidate, candidate/baseline.
Frame 0 resets history. There is no GPU preconditioning, clock lock, profiler
or driver trace. All 240 per-frame samples are retained; the existing warm
summary excludes frame 0 only. Each process still captures frames 0 and 29.
The new throwaway probe is
`2b58712ecd1927f4beedcb2c34b2b8dda13eab4b3eedde0890ceb9feeef1389f`.

The probe adds an ordered CPU/GPU timing vector and two CPU timestamps around
the existing submission/wait function. It prints the vector after the final
frame. The GPU query positions and recorded GPU commands are unchanged.
`submit_wait_ms` includes CPU submission, fence waiting and command-list reset;
`cpu_gap_ms` measures from the previous completed submission wrapper to the
next submission, including capture handling and the next frame's recording.
The zero value for frame 0 means that no preceding submission is measured.

### Observations

All entries below are milliseconds. Frame numbers are zero-based. The late
column is the median of frames 10-29, a descriptive short-run window rather
than a replacement for the preceding 1200-frame benchmarks.

| Pair | Implementation | Frame 0 GPU | Frame 1 GPU | Frame 2 GPU | Late GPU median | CPU gap before frame 1 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | Baseline | 123.792 | 103.004 | 22.381 | 18.185 | 373.573 |
| 1 | Candidate | 44.570 | 230.196 | 20.124 | 17.957 | 1742.083 |
| 2 | Candidate | 41.775 | 131.095 | 44.304 | 18.095 | 1339.253 |
| 2 | Baseline | 118.738 | 120.475 | 19.613 | 18.578 | 365.913 |
| 3 | Baseline | 35.918 | 43.850 | 41.886 | 18.683 | 351.836 |
| 3 | Candidate | 110.884 | 97.837 | 25.518 | 18.086 | 398.652 |
| 4 | Candidate | 123.875 | 114.950 | 117.287 | 17.971 | 433.952 |
| 4 | Baseline | 35.977 | 117.493 | 109.053 | 18.025 | 506.188 |

The candidate's first frame is faster in two pairs and slower in two pairs.
Across four runs per implementation, first-frame medians are 77.357 ms
baseline and 77.727 ms candidate,
with ranges of 35.918-123.792 ms and 41.775-123.875 ms respectively. An even
sample count uses the average of the two central values. These wide ranges
and four observations per implementation do not establish equivalence.
The preceding three-pair result, in which the candidate's 4K first frame was
always slower and reached 119.854 ms, remains part of the evidence.

The slowdown is not restricted to frame 0 or to the candidate. Pair 4 reaches
117.287 ms on candidate frame 2 and 109.053 ms on baseline frame 2. Across
all eight processes, every sample from frame 8 through frame 29 is within
16.926-20.357 ms. The candidate's late median is lower in all four pairs
(0.30%-3.19%), but this short, uncontrolled experiment does not establish a
general speedup or supersede the earlier long-run measurements.

The probe performs CPU readback processing, validation and a 66,355,200-byte
file write after frame 0. The observed gap before frame 1 is 351.836-1742.083
ms, compared with per-run late-gap medians of 1.077-1.518 ms. This gap is
outside the GPU timestamp interval, but changes the conditions preceding
the next GPU submission. It cannot explain frame 0 itself. No control run
yet isolates this gap's effect on subsequent GPU timings. Before/after
`nvidia-smi` snapshots show varying clock states; they do not measure clocks
inside the evaluated frames or identify a cause. Initialization plus feature
creation remains roughly three seconds in this cohort, with medians of
3076.726 ms baseline and 3156.412 ms candidate.

### Validation And Disposition

All four first/final capture pairs match byte-for-byte, and all 16 files are
physically rehashed during analysis. LOCAL usage is 1659.996 MiB baseline and
1596.246 MiB candidate in every pair: the same 63.750 MiB reduction. NON_LOCAL
usage remains 284.949 MiB for both. This validates these endpoints, not every
intermediate frame or every possible input.

The timing-contract test first detects the missing ordered samples in the old
probe, then passes with the instrumented probe. Both 512x512 smoke captures
also match the old probe. An initial 64x64 smoke attempt fails because the
model rejects the image as too small for mirror padding; its log is retained
and it is not counted as the expected missing-instrumentation failure.
All 93 production-source files, both package manifests, the formal ZIP,
original NVIDIA reference and measurement helpers pass the before/after
immutability checks. No production test suite is rerun for this probe-only
experiment; no product code changes.

The candidate remains unpromoted. The follow-up does not reproduce a
consistent candidate-specific first-frame penalty, but it also does not
clear the earlier regression risk. A useful next control would defer CPU
capture validation and file writes until after timing, while retaining both
endpoint readbacks. That would isolate inter-frame host work; a separate
cold-versus-GPU-active control is still needed to investigate frame 0.
Neither change should be presented as a runtime optimization.

Evidence is under `tmp/dx12-first-frame/20261004/`: `protocol.json`,
`records.json`, `frames.csv`, `run-summary.csv`, `summary.json`,
`completion.json`, `manifest.json`, the `runs/` logs/captures, and the
`red/`, `red-512/`, `green-512/` smoke records. Recheck the saved samples and
captures without running another GPU campaign:

```powershell
./tmp/dx12-first-frame/20261004/analyze.ps1
```

## Inter-Frame Capture Processing Control: 2026-10-04

This follow-up isolates CPU capture handling without changing the formal
runtime or the workspace-growth candidate. A throwaway probe supports inline
and deferred CPU processing of the same frame-0/frame-29 GPU readbacks.
Deferred mode maps, copies, validates and writes both captures only after all
30 evaluations have finished. Both modes allocate two identical readback
buffers before the loop, copy the same two output frames on the GPU, and use
unchanged GPU timestamp boundaries. Neither mode prewarms the GPU or NR.
The probe SHA256 is
`238bfe9ae34661b9925bf6cf4f076f3dec91a665fdf7f2e2d5dc85c849f74dd8`.

The four conditions are baseline-inline (A), baseline-deferred (B),
candidate-inline (C), and candidate-deferred (D). Four fixed blocks use
orders A-B-D-C, B-C-A-D, C-D-B-A and D-A-C-B. Each condition occupies each
position once; each runtime's inline/deferred order is reversed in two
blocks. The campaign contains 16 fresh processes and 480 measured frames
at 3840x2160 on the RTX 4090, driver 617.14. Profiling and driver tracing
are off. All samples are retained.

### Results

The following are medians across four runs per condition, in milliseconds.
GPU frame numbers are zero-based. The late statistic is the median of each
run's frame-10-through-frame-29 median, not a new long-run benchmark.

| Condition | CPU gap before frame 1 | GPU frame 0 | GPU frame 1 | GPU sum, frames 1-7 | Late GPU median |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline inline | 243.699 | 33.589 | 39.708 | 224.319 | 17.903 |
| Baseline deferred | 0.834 | 38.305 | 38.151 | 187.737 | 18.366 |
| Candidate inline | 209.517 | 35.968 | 42.678 | 245.772 | 17.994 |
| Candidate deferred | 0.724 | 32.914 | 36.260 | 186.437 | 18.379 |

CPU deferral removes the long inter-frame gap: all inline observations are
195.045-282.584 ms; all deferred observations are 0.492-1.118 ms. The GPU
transient remains. Deferred frame 1 still takes 32.599-50.153 ms, compared
with late medians around 18 ms. Frame 1 improves in five of eight same-runtime
pairs and worsens in three. The total GPU time for frames 1-7 decreases in
seven pairs, but this is not a uniform speedup: the late median increases in
seven pairs by 0.80%-3.44%; the remaining pair decreases by 0.32%.

The candidate-inline process in block 3 records 111.801 ms on frame 1 and
112.54 ms on frame 2; neither sample is discarded. First-frame timings also
vary even though the changed CPU capture work only happens after frame 0.
This experiment therefore does not establish a first-frame fix or show that
the long capture gap is the sole cause of the early GPU slowdown. It also
does not identify clock ramping, cache state, resource residency or another
process as the cause. The earlier cohorts, including their larger outliers,
remain separate rather than being replaced by these numbers.

### Validation And Next Control

All 32 endpoint files are physically rehashed and match the preceding probe's
frame-0/frame-29 captures. The readback resources are distinct, so deferring
the first capture does not overwrite it with the final frame. CPU processing
order is checked from each run's evaluation/inspection trace, as are all 480
raw timing records and the requested capture policy.

Both capture modes have identical process-memory usage for a given runtime.
LOCAL remains 1659.996 MiB baseline and 1596.246 MiB candidate, preserving
the 63.750 MiB difference in all eight runtime comparisons. NON_LOCAL is
348.262 MiB in every condition. This is 63.3125 MiB above the preceding
single-readback probe because this control allocates a second endpoint
buffer in both modes. It is a probe allocation, not a new runtime cost.

The old probe fails the deferred-order contract, while both modes of the
new probe pass and reproduce the old 512x512 smoke outputs. One initial
deferred smoke exposes a PowerShell argument construction error: a single
string is splatted as individual characters instead of passing the flag.
Explicit array construction fixes it. The failure log is retained, the old
probe's expected failure is replayed with corrected arguments, and the
main campaign begins only after the corrected deferred smoke passes.

All 93 production-source files, both package manifests, the formal ZIP and
original NVIDIA reference remain unchanged. No production suite is rerun
for this probe-only control. The candidate is not promoted. A next diagnostic
would compare the current no-preconditioning path with an independent GPU
workload immediately before NR's first evaluation, without executing NR or
advancing its history. Preconditioning cost must be reported separately;
moving work before the measured interval is not a runtime speedup.

Evidence is under `tmp/dx12-capture-defer/20261004/`: `protocol.json`,
`records.json`, `frames.csv`, `run-summary.csv`, `summary.json`,
`completion.json`, `manifest.json`, the `runs/` directory, and the named
smoke-test directories. Recheck without rerunning the GPU campaign:

```powershell
./tmp/dx12-capture-defer/20261004/analyze.ps1
```

## Independent GPU Preconditioning Control: 2026-10-04

This diagnostic compares no independent GPU work with an independent compute
load immediately before the first NR evaluation. It does not execute NR in
advance, alter its history, change precision, or modify either runtime DLL.
All conditions retain deferred CPU capture processing from the preceding
control. "No preconditioning" does not guarantee that the device is cold.

The throwaway probe builds the same ordinary D3D12 compute pipeline, 1 MiB
scratch buffer and inspection readback in both conditions. An integer HLSL
loop dispatches 1024 groups of 256 threads, with 8192 mixing iterations per
thread. Its raw-buffer root UAV follows Microsoft's
[root-descriptor rules](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-descriptors-directly-in-the-root-signature).
Active runs accumulate at least 200 ms of timestamp-measured GPU work;
no-preconditioning runs dispatch none of it. The workload never receives
NR resources or its feature handle. The first and last scratch elements are
checked against a CPU reference only after NR timing finishes. Logging is
also deferred. The probe SHA256 is
`a3c137b60e8111187a9b98d689344aa12368a4b78666e6e5073c0ef223d3135f`.

There are four fixed-order blocks and 16 fresh processes, each evaluating
30 frames at 3840x2160 on the RTX 4090 with driver 617.14. Conditions are
baseline-none (A), baseline-active (B), candidate-none (C), candidate-active
(D), ordered A-B-D-C, B-C-A-D, C-D-B-A, D-A-C-B. The original balanced order
is retained. No profiler, driver trace or global clock setting is used.

### Results And Cost

All values below are medians across four runs, in milliseconds. GPU frame
numbers are zero-based. Late GPU is the median of per-run frame-10-through-
frame-29 medians and does not replace the earlier long-run benchmark.

| Condition | GPU frame 0 | GPU frame 1 | Late GPU | Preconditioning wall time | Time to first completion |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline none | 45.804 | 47.961 | 18.085 | 0.000 | 60.982 |
| Baseline active | 19.246 | 19.774 | 18.368 | 240.665 | 282.932 |
| Candidate none | 40.330 | 40.285 | 18.195 | 0.000 | 54.484 |
| Candidate active | 21.880 | 19.140 | 17.930 | 243.438 | 279.664 |

Every one of the eight same-runtime pairs has a lower first-frame GPU time
after independent work, with reductions of 10.80%-73.88%. The preconditioning
actually executes 200.134-201.504 GPU ms in 102-166 batches. Submission,
waiting and timestamp collection bring its wall cost to 237.839-247.993 ms.
The gap from its last completed submission wrapper to the first NR submission
is 0.973-2.468 ms. These are measured gaps, not in-frame clock observations.

Time to first completion starts immediately before the preconditioning/no-op
function and ends when the first NR submission wrapper returns. It includes
preconditioning, frame recording, input uploads, NR, endpoint GPU readback
and fence waiting. It excludes DLL initialization, feature creation and the
common independent-pipeline setup, whose separate setup times are retained.
Including the extra work delays first completion in every pair by
185.929-261.647 ms. Adding artificial preconditioning to the runtime would
therefore increase this measured latency despite reducing the NR-only number.
NGX initialization plus feature creation still has condition medians of
roughly 2.88-2.93 seconds; this experiment does not optimize it.

Independent work does not eliminate all first-frame variation. Active
baseline frame 0 ranges from 17.936 to 32.318 ms; active candidate frame 0
ranges from 19.010 to 24.865 ms. The active candidate is slower than the
active baseline in three of four blocks, by 0.322, 1.092 and 4.928 ms, and
faster by 7.454 ms in the remaining block. These four observations do not
clear a possible candidate-specific residual cost. The no-preconditioning
baseline's 71.562 ms first frame and 115.823 ms second frame are retained,
as are all other samples and the previous cohorts' outliers.

The intervention supports a substantial dependence on execution-before-NR
conditions in this probe. It does not distinguish clock behavior from cache,
residency, scheduling or other shared driver/device effects. It is neither
a game-FPS result nor evidence of a net runtime speedup.

### Validation And Disposition

The old probe fails the missing-precondition-record contract. The new probe
passes both no-work and 200-GPU-ms smoke tests. All main runs record exactly
30 NR evaluations, with zero before preconditioning. Active scratch checks,
all 480 frame timings, batch counts/totals, capture processing order and
all 32 physical NR endpoint files pass validation. NR captures match the
preceding unmodified capture path byte-for-byte.

LOCAL usage is 1660.996 MiB baseline and 1597.246 MiB candidate in either
preconditioning condition. The 63.750 MiB runtime difference persists in
all eight comparisons. NON_LOCAL is 348.265625 MiB in every condition. The
independent test workload adds 1 MiB LOCAL and 4 KiB NON_LOCAL relative to
the preceding probe, equally for both runtime DLLs and both conditions.
These are common probe allocations, not new runtime allocations.

The first block stops after its four completed measurements because the
post-block comparison uses the obsolete `baseline-inline` key instead of
`baseline-none`. Physical rehashing confirms that the actual outputs match.
The original helper, failure completion and first four records are preserved.
`run_remaining.ps1` fixes that lookup and runs only blocks 2-4 using the
unchanged original protocol. Analysis verifies that the first four records
remain identical; none are discarded or rerun. `resume-protocol.json`
records the correction and both helper identities.

All 93 production-source files, both runtime packages, the formal ZIP and
original NVIDIA reference remain unchanged. No production test suite is
rerun for this diagnostic-only probe. The candidate remains unpromoted and
no artificial workload is added to the product. The remaining question is
which stage accounts for the residual first-evaluation cost after independent
GPU activity, rather than how to hide that cost before the timer starts.

Evidence is under `tmp/dx12-precondition/20261004/`: `protocol.json`,
`resume-protocol.json`, `initial-completion.json`, `initial-records.json`,
`records.json`, `frames.csv`, `run-summary.csv`, `summary.json`, the `runs/`
logs/captures, and smoke records. `run.ps1` intentionally preserves the
original failed validation helper. Recheck saved evidence without a GPU run:

```powershell
./tmp/dx12-precondition/20261004/analyze.ps1
```

## Preserved Launch-Chain Profiling: 2026-10-05

This diagnostic keeps the 200-GPU-ms independent workload and deferred
endpoint processing, then compares profiling off/on for each unchanged DLL.
The existing NVAPI wrapper forwards each original launch chain, its arguments
and kernel count intact. It does not split compound chains. Consequently,
these are launch-chain intervals; only single-kernel chains have individual
kernel granularity. There is no new runtime optimization in this experiment.

The profiler previously wrote CSV rows after every measured frame. The new
throwaway version retains timestamp copies in memory and flushes CSV data
after all NR evaluations and endpoint processing. Stream positions must still
match their constructor/header positions immediately before that flush.
The probe SHA256 is
`57dbdf2114d2bd011e6ae83fc935830e171e538b7c14636a77ec4844324f371c`.

Four balanced blocks use baseline-off (A), baseline-on (B), candidate-off
(C), candidate-on (D), ordered A-B-D-C, B-C-A-D, C-D-B-A, D-A-C-B. All 16
processes run 30 frames at 3840x2160 on the RTX 4090, driver 617.14. There
are 480 outer-frame samples, 240 profiled frames and 27,120 measured chain
intervals. Each profiled frame has the same 113 chains and 252 kernel entries;
nine chains contain multiple kernels. The inventory, launch dimensions and
chain composition match across both runtimes and every profiled frame.
The 512x512 smoke inventory is smaller and is not substituted for the 4K one.

### Where Time Goes

The following averages pool frames 10-29 from the eight profiled processes.
They describe this short diagnostic cohort, not game FPS or an update to the
earlier long-run benchmark. Gaps before, between and after launch intervals
are accounted separately rather than assigned to an arbitrary kernel.

| Recorded region | Mean GPU time, ms | Share of outer interval |
| --- | ---: | ---: |
| Prepare | 0.698 | 3.46% |
| Network launch chains | 19.038 | 94.25% |
| Compose | 0.442 | 2.19% |
| Unattributed gaps | 0.022 | 0.11% |
| Outer interval | 20.200 | 100% |

The largest steady chain is slot 56, from
`gemmv_e4m3_K1024_f6_s1_m96` to `gemmv_e4m3_K1024_f5_s4_m96`.
Its 48 kernels together average 2.745 ms, about 13.59% of the outer interval.
Other expensive compound chains include slot 102 at 1.703 ms, slot 5 at
1.643 ms and slot 11 at 1.608 ms. These numbers do not identify which kernel
inside a compound chain dominates; the chain was deliberately not split.

The slowest profiled first frame is baseline-on, block 3: 32.630 ms versus
20.107 ms averaged over that run's frames 10-29. Its additive excess is:

| Region | First-frame excess over that run's late mean, ms |
| --- | ---: |
| Prepare | -0.138 |
| Network launch chains | +12.340 |
| Compose | +0.017 |
| Unattributed gaps | +0.304 |
| Total | +12.523 |

The network excess is spread across multiple chains. For example, slots 56,
108, 8 and 105 add 1.489, 1.435, 1.269 and 1.261 ms respectively. There is no
additional first-frame chain or kernel entry in the observed public launch
topology. This does not rule out hidden driver work or establish the cause
of the longer intervals. In particular, the candidate-off 43.875 ms first
frame has no chain trace; it cannot be assigned the decomposition of this
different, profiled baseline process.

### Observer Effects

Profiling-on versus profiling-off changes the late GPU median by -1.99% to
+1.04% across the eight matched pairs. It adds 0.232-0.369 ms to the late CPU
recording median. First-frame paired changes range from -23.807 to +12.437
ms, with both signs. Separate processes and these variable first frames do
not support a constant observer-overhead correction or a zero-overhead claim.

The profiled candidate's late GPU median is 0.55%-1.98% above the profiled
baseline in all four blocks. Without profiling, three blocks are 0.60%-1.96%
higher and one is 1.05% lower. These small, short-cohort differences do not
replace the earlier long-run results, but they are retained rather than
used to claim a candidate speedup or performance clearance.

The measurements locate the stable cost in the network chains, not in the
prepare/compose stages or a large amount of unattributed inter-chain time.
Further optimization should inspect the data flow and implementation of
these expensive chains while preserving numerical results. Blindly removing
synchronization or adding more artificial preconditioning is not supported
by this evidence.

### Validation And Limits

All 32 endpoint files match the previous unprofiled captures byte-for-byte.
Every chain interval is ordered, non-overlapping and contained in its outer
frame interval, which also matches the normal frame timer. Per-frame and
aggregate stage sums cover the complete outer interval. The old profiler
fails the deferred-output contract; the new off/on smoke runs pass. Four
negative validator fixtures reject a missing launch, an interval outside its
frame, a mismatched kernel inventory and a prematurely flushed profile.

The candidate saves 63.750 MiB LOCAL in all eight same-mode runtime pairs.
Profiling adds 0.125 MiB LOCAL to either implementation. NON_LOCAL normally
increases by 0.0625 MiB with profiling. One candidate-off process, block 3,
uses 0.250 MiB less NON_LOCAL than baseline-off; that difference is already
present at `after_create`, and remains stable through all three steady
samples. Its cause is not established and it is not claimed as a repeatable
memory improvement. The initial analyzer's exact-NON_LOCAL-equality assertion
rejects that observation. `analyze.initial.ps1` is preserved; the revised
analysis reports the actual delta and still rejects any candidate increase.
No measurement is discarded or rerun because of this analysis assumption.

All 93 production-source files, both packages, the formal ZIP, original NVIDIA
reference and previous probes remain unchanged. No production suite is rerun
for this probe-only instrumentation. The candidate remains unpromoted; the
profiling code is not added to either runtime package.

Evidence is under `tmp/dx12-first-stage-profile/20261005/`: `protocol.json`,
`records.json`, `run-summary.csv`, `stage-frames.csv`, `stage-summary.csv`,
`chain-frames.csv`, `chain-summary.csv`, `hotspots.json`, `summary.json`,
the `runs/` logs/captures/profile CSVs, and smoke/validator records. Recheck
without another GPU campaign:

```powershell
./tmp/dx12-first-stage-profile/20261005/analyze.ps1
```

## Full-Graph Split-K Capacity Candidate: 2026-10-05

Status: independently built and validated as a memory-saving candidate. It is
not promoted. The 1080p timing results contain a regression signal, so this
experiment does not establish performance non-regression or a speedup.

### Dataflow Audit

The measured 4K chain 56 is the eight ViT blocks, 31 through 38. Each block
expands the state with SiLU, contracts it with a residual, produces FP16 QKV,
normalizes/reorders QKV, runs streaming attention, and projects with another
residual. The 48 entries are not 48 copies of the same computation. Intermediate
formats, split counts and reduction order are part of the numerical contract.

A throwaway record-only program uses the frozen source and package assets to
capture the graph's buffer references and scalar arguments. It ends, but never
submits, the recorded NR command buffer. Model initialization still performs its
normal uploads. At 4K, all 48 ViT names, grids and thread counts match the earlier
chain-56 inventory. The graph has 2160 ViT tokens and 2176 padded tokens.

The split-K buffer is allocated at four times its first GEMMV requirement, with
a 16 MiB floor. Its actual users include 24 ViT GEMMV launches **and** the
decoder-entry GEMMT/reduction pair. Checking only chain 56 would miss the latter.
The auditor's initial GEMMV-only check rejects that additional use; its original
script and raw recording are retained. The completed whole-graph audit gives:

| Input | Existing Scratch MiB | Largest Required Range MiB | Unused Capacity MiB |
| --- | ---: | ---: | ---: |
| 512x512 | 16 | 2.25 | 13.75 |
| 1920x1080 | 21 | 7.875 | 13.125 |
| 2560x1440 | 30 | 11.25 | 18.75 |
| 3840x2160 | 69 | 25.875 | 43.125 |

These recordings are under `tmp/dx12-chain56-audit/20261005/`. They establish
allocation bounds, not runtime savings by themselves.

### Candidate Scope

The candidate is based on the previous workspace-growth candidate, retaining
that experiment's planner unchanged. The new code is confined to copies under
`tmp/dx12-scratch-capacity/20261005/ngx/`: a CPU-only scratch-capacity planner
and its call in the D3D12 graph constructor before buffer allocation.

The planner checks every recorded reference to `split-K partials`. It recognizes
the six tested GEMMV variants and the decoder GEMMT/reduction pair, validates
their argument schemas and launch geometry, and bounds the complete partial
storage range. Products and the PTX's 32-bit address intermediates are checked.
Unknown functions, other argument slots, nonzero subviews, shader bindings,
copies, clears, uploaded contents or unsupported shapes retain the old capacity.
Only a proven unused tail is removed. The argument values, counters, operations,
barriers and arithmetic remain unchanged.

This reduces the D3D12 allocation, not the temporary Vulkan graph compiler's
allocation. It does not establish a lower initialization peak. The planner's
contracts refer to the checked-in kernel layouts; changed PTX layouts require
revalidation, even if their entry names remain the same.

- Previous candidate / paired baseline: `50FFEBB69DDC14DC2929AA172BF68862F3AA610B3B5E15B1F791DE5BC0B78856`.
- New scratch-capacity candidate: `77114D070369D5E030558C04B49911C6D858D41D1A053844B7B70FD13EBA7867`.
- Formal package DLL remains `8FFF3995DE95EB003712110889090461C28200A50A12F76E4A36521FE986D8F5`.
- Original NVIDIA reference remains `984BEE0F775C277D5829B8FD6775D53A7B0F75396C852B3AAF06A18375F81014`; it is used for correctness, not the new timing comparison.

The package differs from the previous candidate in its DLL only, apart from the
updated manifest. All 114 model/shader assets match the formal package. Copied
package `benchmark.json` and experiment metadata describe earlier experiments;
the authoritative new results are in this candidate's parent experiment folder.

### Verification

The planner test first fails with the unchanged 4K capacity, 72351744 bytes,
instead of the required 27131904 bytes. All 33 cases then pass. Coverage includes
both tile heights, a decoder range larger than the ViT range, unchanged graph
contents, idempotence, malformed signatures, unexpected users, and overflow.

The D3D12 checks include four workspace sizes with poison, changing inputs and
queued consumers; two complete Vulkan/D3D12 graph comparisons; parameter, trace,
NVAPI ordering, native normalization and ABI tests; and default, dedicated,
reuse and invalid workspace policies. The 18-case NVIDIA comparison produces
75 frame pairs, all byte-identical and independently rehashed. The unrelated
22-case Vulkan graph matrix is not rerun for this D3D12-only change.

The first ABI attempt uses the restricted timing probe and is rejected before
loading the candidate. It is preserved as a test-harness failure. The resumed
correctness run uses the existing general-purpose probe; the timing probe is
unchanged. `accepted-tests.json` records the 12 completed standalone checks,
and the original failed log remains under `correctness/`. Builds retain the
existing `nr_model.cpp:131` C4458 warning; no new compiler warning is introduced.

### Measured Memory

Four paired rounds at each of 1080p, 1440p and 4K alternate the old/new candidate
order. Three additional formal-package runs check total memory savings and
outputs. All 27 processes run 240 evaluations with 60 warmup frames, identical
independent 200 ms GPU preconditioning, deferred endpoint processing, and no
profiling. This yields 6480 frame timings, including 4320 paired warm samples
and 540 formal-control warm samples. The 54 physical first/final captures match
within each size across all implementations and rounds; intermediate benchmark
frames are timed, not all image-compared.

On the RTX 4090 with driver 617.14, current-process DXGI LOCAL usage is:

| Input | Formal MiB | Previous Candidate MiB | New Candidate MiB | Incremental Saving MiB | Total Saving MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1920x1080 | 654.996 | 638.121 | 624.996 | 13.125 | 30 |
| 2560x1440 | 913.121 | 884.371 | 865.621 | 18.75 | 47.5 |
| 3840x2160 | 1660.996 | 1597.246 | 1554.121 | 43.125 | 106.875 |

Every paired round has the stated incremental saving, with three stable memory
samples per process. NON_LOCAL is unchanged between implementations: 111.015625,
172.390625 and 348.265625 MiB respectively. The numbers include identical host
resources; they are not host-subtracted NR-only usage. The new 4K graph test
reports 1157365760 owned bytes, compared with 1202585600 in the previous frozen
workspace-growth test, a difference of exactly 43.125 MiB.

### Timing Limitation

Changes in paired warm GPU medians relative to the previous candidate are below.
Positive values mean the new candidate is slower.

| Input | Round 1 | Round 2 | Round 3 | Round 4 |
| --- | ---: | ---: | ---: | ---: |
| 1920x1080 | +0.21% | +5.54% | -0.37% | +3.50% |
| 2560x1440 | -3.40% | -3.26% | +1.05% | +0.17% |
| 3840x2160 | +0.30% | -0.18% | +1.29% | -1.90% |

1080p is slower in three of four rounds; the arithmetic mean of its paired
percentage changes is +2.22%. This is a regression signal, not a proven cause
or a universal penalty estimate. The other two sizes have mixed signs. CPU
recording deltas also have mixed signs. First-frame peaks remain present, and
the single formal controls do not support a paired formal-version timing claim.
No sample is discarded or rerun to obtain a better timing result.

The candidate delivers the expected memory reduction but remains experimental.
It is not a release replacement or a demonstrated FPS improvement. The formal
package, ZIP, all 93 production-source files and the prior candidate remain
unchanged. A performance non-regression claim would need to resolve the 1080p
signal rather than selecting only the favorable rounds or sizes.

Evidence under `tmp/dx12-scratch-capacity/20261005/` includes `variant.json`,
`planner-red.log`, `planner-green.log`, `accepted-tests.json`, the correctness
directories, `native-capture-audit.json`, `benchmark/`, `run-summary.csv`,
`pair-summary.csv`, `benchmark-capture-audit.json` and `summary.json`. Recheck
without another GPU campaign:

```powershell
./tmp/dx12-scratch-capacity/20261005/check_native.ps1
./tmp/dx12-scratch-capacity/20261005/analyze.ps1
```

## Same-Binary 1080p Capacity Control: 2026-10-05

Status: the capacity-only switch does not reproduce a consistent warm-median
slowdown in this diagnostic experiment. This is not proof that the previous
results were erroneous, a general performance-equivalence claim, or approval
to replace the formal package.

Before running another experiment, the prior 1080p data was divided into fixed
30-frame windows within the measured frame-60-through-239 interval. In one
previous-candidate process, window medians range from 5.275680 to 6.048704 ms
(14.65%); one new-candidate process ranges from 5.361600 to 6.091264 ms (13.61%).
Those within-process changes motivate a self-control experiment. They do not
by themselves establish the cause of the earlier between-version difference.

### Diagnostic Design

One throwaway DLL contains both modes. During graph creation only,
`OPEN_DLSS_NR_DIAG_SPLIT_SCRATCH=keep` retains the original capacity, while
`compact` invokes the unchanged scratch planner. Missing or invalid values are
rejected. A single creation-time record reports the selected mode and before/
after capacities; its output is flushed before the first NR evaluation. There
is no diagnostic logging or capacity decision in the per-frame path.

The diagnostic DLL SHA-256 is
`199FBD18A698693AEAC0369A6551C269D8F606C3DDAD3E9821170B20A72B4513`.
It is not a replacement candidate. The existing `50FF...` and `77114...`
candidates, formal `8FFF...` DLL and ZIP, original NVIDIA reference, production
source inventory and all runtime assets remain frozen.

The fixed protocol uses four balanced blocks, each with two separate processes
per mode. Labels A/B are `keep-a`/`keep-b`; C/D are `compact-a`/`compact-b`.
Orders are ABDC, BCAD, CDBA and DACB. Each of the 16 processes uses 1920x1080,
600 evaluations, 200 warmup frames, the same independent 200 ms GPU load,
deferred endpoint inspection and profiling off. Driver and global GPU settings
are not changed. Before/after telemetry is retained, but does not measure clocks
during the timed frames.

The primary comparison, defined before the runs, is the ratio of each block's
average of two compact process medians to its average of two keep process
medians. The a/b comparison within each mode estimates repeat variation. The
6400 warm frame timings are not treated as 6400 independent experiments.

### Results

Positive changes below mean slower execution. The repeat columns compare b to
a within that mode, not one capacity to the other.

| Block | Keep Mean Median ms | Compact Mean Median ms | Capacity Change | Keep Repeat Change | Compact Repeat Change |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 5.301248 | 5.284992 | -0.307% | +0.147% | +0.944% |
| 2 | 5.299360 | 5.291024 | -0.157% | -0.174% | +0.544% |
| 3 | 5.288096 | 5.293472 | +0.102% | +0.116% | +0.407% |
| 4 | 5.300992 | 5.309472 | +0.160% | +0.114% | -0.176% |

The mode changes have mixed signs and average -0.051%. Their observed range
is -0.307% to +0.160%; the largest absolute same-mode repeat difference is
0.944%. These descriptive comparisons do not establish statistical equivalence
or a small speedup.

Post-hoc checks are retained separately from the primary comparison. Applying
the former frame-60-through-239 window to these same recordings gives capacity
changes of -0.352%, -0.138%, +0.192% and +0.219%. The warm per-process P95
comparisons have mixed signs: +1.274%, -0.870%, +1.067% and -1.947%.
The prior 5.54% result is therefore not reproduced as a consistent median
penalty here, including in the older window. However, this experiment uses a
new diagnostic binary and a different cohort; it does not identify the cause
of the original result. First-frame peaks still occur in both modes.

The allocation switch is independently verified in every run: logical scratch
capacity changes from 21 to 7.875 MiB. Current-process DXGI LOCAL usage changes
from 638.12109375 to 624.99609375 MiB, exactly 13.125 MiB, with three stable
samples per process. This reproduces the previous memory saving, not an
additional saving. NON_LOCAL is normally 111.015625 MiB. Block 1 `compact-b`
uses 111.031250 MiB, an additional 16 KiB already present after creation and
stable through release. Its cause is not established; it is not discarded or
reported as identical shared-memory usage.

### Verification And Limits

The mode test first demonstrates that the previous always-compact behavior
cannot retain the original capacity. The 13 diagnostic cases and unchanged
33-case capacity-planner suite then pass. An initial test-source compile error
in a mixed-type initializer list is fixed before the expected failing assertion
is observed; both logs are retained. The DLL build has only the existing
`nr_model.cpp:131` C4458 warning.

Both modes produce three consecutive 1080p smoke frames byte-identical to the
frozen original NVIDIA captures. All 32 physical first/final benchmark files
also match across all modes and blocks, and each first frame matches the
original reference. All 9600 ordered frame timings, creation-time mode records,
capacity differences and helper hashes are rechecked. Intermediate benchmark
frames are timed, not all image-compared. No run or sample is omitted or rerun
based on its timing. The broader product suites are not rerun for this
throwaway creation-time switch.

The evidence supports retaining the memory-saving candidate for further
validation without treating the earlier 1080p penalty as an established cost
of compaction. It does not promote the diagnostic DLL, establish game FPS, or
remove the need for representative application testing before release.

Evidence is under `tmp/dx12-scratch-control/20261005/`: `setup.json`,
`build-manifest.json`, CPU test logs, `smoke/`, the preregistered `protocol.json`,
`records.json`, `runs/`, `run-summary.csv`, `block-summary.csv`,
`window-summary.csv`, `capture-audit.json` and `summary.json`. Recheck without
another GPU campaign:

```powershell
./tmp/dx12-scratch-control/20261005/analyze.ps1
```

## Static Portrait D3D12 Replay: 2026-10-05

The user chose image testing because no target application was available. This
experiment compares the unchanged RTX 40-adapted original (`984BEE0F...`), formal
packed-prepare package (`8FFF3995...`), and combined workspace-growth/split-K
capacity candidate (`77114D07...`). No runtime DLL is rebuilt or promoted.
Full identities, probe sources, input hashes and asset locations are recorded
in `tmp/dx12-image-replay/20261005/frozen.json`.

### Input And Coverage

The source is `docs/images/cowboy-gramps-nr-off.png`, a 2048x1152 RGB8 rendered
portrait. Four resized views use 1920x1080, 2560x1440, 3840x2160 and 513x377;
a fifth uses a 512x512 face crop. These are five views of one image, not five
independent scenes. The 1440p and 4K inputs are upsampled. Pillow Lanczos
resizing precedes division by float32 255 and conversion to little-endian
RGBA16F, with alpha 1. No inverse transfer function, tone mapping or sharpening
is applied. The input is a display-code proxy, not an HDR render capture.

Private copies of the existing correctness and timing probes add
`--input-rgba16f`. They validate dimensions, exact byte length and finite RGBA
values before loading the runtime or creating a GPU device, and log the hash
of the actual loaded pixel buffer. The remaining upload, evaluation and timing
paths are unchanged. Disabled evaluations reproduce the input file's bits,
checking that the uploaded resource contains the requested image rather than
the previous generated pattern.

All three runtimes pass nine image cases: three consecutive frames of each
view, plus reset, disabled, enable-toggle and manual-control cases on the face
crop. The 27 processes produce 87 captures. All 58 formal/candidate comparisons
against the original are exact over RGBA16F bits, including alpha and signed
zero. The new probe also passes the existing 18-case synthetic matrix: 75
original/candidate frame pairs are physically rehashed. Synthetic results are
kept separate from the image results.

The loader's 18 CPU checks, five encoding checks, ten invalid-input CLI checks,
nine image-record validation checks and seven numeric-comparison checks pass.
The loader, encoding and comparator tests have retained failing-before/fixed
logs; the CLI check also demonstrates that the old probe does not validate
the new image argument. No Vulkan suite is rerun for this probe-only change.

### Timing And Memory

On the RTX 4090 with driver 617.14, each resolution has three process runs per
runtime. Every process evaluates 600 frames and excludes the first 200 from
the warm summary. Runtime and resolution order rotate across the three rounds.
An independent 200-GPU-ms preconditioner does not evaluate NR or advance its
history. Profiling is off. Only frames 0 and 599 are captured during timing,
with CPU capture processing deferred until after the evaluation loop.

All 16,200 raw timings and 54 physical endpoints are retained. Each first frame
matches the fresh original correctness capture, and both endpoints match
across all runtimes and rounds. Intermediate timing frames are not image-compared.
No run is discarded or repeated because of its timing. GPU timestamps enclose
serialized `EvaluateFeature`, excluding input loading, upload and output
readback. They do not measure game FPS or whole-frame rendering cost.

The table reports the median of three per-process warm GPU medians. Each
process uses the upper median of its 400 ordered warm samples. Positive
percentages mean increased GPU time.

| Size | Original GPU ms | Formal GPU ms | Candidate GPU ms | Candidate vs Formal | Candidate vs Original |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1920x1080 | 3.545952 | 5.304960 | 5.296672 | -0.156% | +49.372% |
| 2560x1440 | 5.019744 | 8.202240 | 8.226720 | +0.298% | +63.887% |
| 3840x2160 | 10.370464 | 17.798368 | 17.896896 | +0.554% | +72.576% |

The three paired candidate/formal changes are -0.023%, -0.156%, +0.034% at
1080p; -0.052%, +0.836%, +0.298% at 1440p; and -0.691%, +0.730%, +0.015% at
4K. These descriptive repetitions show no consistent speed advantage. A ratio
of aggregate medians is not the same statistic as the paired-round changes;
neither establishes performance equivalence. Warm mean, p95, maximum, CPU
recording and first-evaluation times remain in `run-summary.csv`.

| Size | Original LOCAL MiB | Formal LOCAL MiB | Candidate LOCAL MiB | Saved vs Formal MiB |
| --- | ---: | ---: | ---: | ---: |
| 1920x1080 | 591.062500 | 654.996094 | 624.996094 | 30.000 |
| 2560x1440 | 765.875000 | 913.121094 | 865.621094 | 47.500 |
| 3840x2160 | 1303.875000 | 1660.996094 | 1554.121094 | 106.875 |

These are DXGI current-process LOCAL samples including common probe resources
and driver overhead, without host subtraction. They are not initialization
peaks or NGX-owned allocation counts. All three steady samples within each
process agree, and all three rounds reproduce the savings. The reductions
repeat the existing combined candidate's gains; image testing adds no new
memory optimization. Candidate LOCAL usage still exceeds the original by
33.934, 99.746 and 250.246 MiB respectively.

NON_LOCAL is recorded separately. Original/formal/candidate usage is
257.769531/111.015625/111.015625 MiB at 1080p,
319.144531/172.390625/172.390625 MiB at 1440p, and
354.140625/348.265625/348.265625 MiB at 4K. These shared-memory measurements
must not be presented as dedicated VRAM or omitted when discussing total
memory use.

### Visuals And Limits

`comparison-1080p.png` shows input, original and candidate. The
`comparison-4k-detail.png` face crop uses 1:1 output pixels alongside an absolute
RGB difference multiplied by 16, computed from the raw half-float values.
The black difference panel follows exact raw equality. All previews use the
same display-code clamp and nearest-integer RGB8 conversion; the full-size
outputs are `reference-4k.png` and `candidate-4k.png`. Visualization settings
are recorded in `visualization.json`.

This is a static-image test with zero motion and sequential temporal feedback.
It does not validate real motion, occlusion, an application's NGX loader,
concurrent render work, or arbitrary scene content. The candidate preserves
the tested output and saves LOCAL memory relative to the formal build, while
remaining slower and using more LOCAL memory than the original here. The
formal package, ZIP, both existing candidates, original runtime, model and
shader assets, and 93 production source files remain unchanged.

The experiment directory contains the frozen inputs and probes, all logs and
captures, correctness and synthetic-regression records, the benchmark protocol,
`run-summary.csv`, `pair-summary.csv`, `image-comparisons.json`,
`benchmark-capture-audit.json` and `summary.json`. Recheck and regenerate the
derived figures without another GPU campaign:

```powershell
python ./tmp/dx12-image-replay/20261005/analyze.py
./tmp/dx12-image-replay/20261005/check_frozen.ps1
```

## C128 FFN R3 Tiling Candidate: 2026-10-06

The approved next experiment changes only C128 FFN row tiling from R4 to R3
in a private copy of `src/kernels.cpp`. The baseline is the existing combined
memory candidate (`77114D07...`), not the original NVIDIA DLL. The R3 candidate
is `3ABA9647A69E69E0CF02DDF7B4925704B35B4E6557E8C09267BCB3B3285DD1C0`.
Both roles use the same expanded asset directory, containing unchanged common
assets plus the two generated C128 R3 PTX files. C64 remains R4 and C256 remains
R3. Weights, arithmetic order, barrier algorithms, workspace growth and split-K
capacity compaction are unchanged. No register limit is introduced.

Build and correctness work started on October 5; the four-round timing campaign
completed on October 6. Evidence remains under
`tmp/dx12-ffn-rowtiles/20261005/`, with full identities in `variant.json`.

### Resource And Correctness Gates

The unchanged generator emits both normal and projected variants. Regenerated
R4 PTX matches the frozen assets. CUDA 13.0 `ptxas -arch=sm_89 -v` reports:

| Variant | Rows per Group | Threads | Dynamic Shared Bytes | Registers per Thread |
| --- | ---: | ---: | ---: | ---: |
| R4 normal | 64 | 512 | 46080 | 83 |
| R4 projected | 64 | 512 | 46080 | 80 |
| R3 normal | 48 | 384 | 43776 | 100 |
| R3 projected | 48 | 384 | 43776 | 100 |

All four have zero reported stack and spill bytes. The increased register
count weakens the proposed occupancy benefit; fewer threads and less shared
memory alone do not establish higher occupancy. These are offline assembly
results, not measurements of the NVAPI driver's JIT register allocation.

The host selector/coordinate test fails against the old selector and passes
42 cases against R3. Nine Python tests pass, including 1728 independently
enumerated publication-band checks against the generated integer PTX and
negative trace-validator cases for stale R4 launches, stale consumer group
sizes and unrelated kernel changes. The coordinate test is a host model, not
a proof of every emitted PTX address.

Record-only graph audits cover 513x377, 1080p, 1440p and 4K. Buffer layouts,
bindings and unrelated operations match. Exactly 12 C128 FFN launch records
change, along with the 12 QKV consumer arguments carrying producer rows per
group (64 to 48). Producer signal and consumer wait bindings remain aligned.
A separate three-frame D3D12 profiling smoke confirms the 12 intended R3
launches in the real feature and otherwise unchanged chain inventory.
Its timings are not used for performance acceptance.

Independent R4/R3 full-head tests execute 96 graph evaluations across four
sizes, four changing half-float feature inputs and dedicated/reuse/poison
routes. They include consecutive evaluations and readbacks in one submission.
All 48 candidate route/frame comparisons are exact; 16 independent R4/R3
full-head capture pairs are physically rehashed. The existing ABI/lifecycle,
parameter, core, NVAPI, normalization and workspace-policy gates also pass.
The 18-case synthetic NGX matrix produces 75 fresh original/R3 output pairs,
all bit-exact. Nine static-image cases produce 58 new R4/R3 captures, all
bit-exact against the frozen original NVIDIA image captures. These checks do
not substitute for a full Vulkan regression suite, which was not rerun.

### Paired Image Timing

The RTX 4090 remains on driver 617.14. The frozen image timing probe, image
inputs and common assets are identical between roles. Each process runs 600
evaluations, excludes the first 200 from warm statistics, uses an independent
200-GPU-ms preconditioner, and disables profiling. CPU processing of endpoint
captures is deferred until after evaluation. Role order alternates R4/R3,
R3/R4, R4/R3, R3/R4; resolution order also varies under the saved protocol.

All 24 processes, 14400 raw timings and 9600 warm samples are retained without
timing-driven exclusions or repeats. All 48 physical endpoint files (frames
0 and 599) match the frozen 600-frame original NVIDIA image campaign. Other
benchmark frames are timed but not image-compared. GPU timestamps enclose
serialized `EvaluateFeature`, not image loading, upload, readback or game FPS.

Each process uses the upper median of 400 warm GPU samples. The aggregate is
the median of four process medians, averaging the middle two. Positive changes
mean increased GPU time. Aggregate ratios and paired-round ratios are distinct
statistics; both are reported rather than selecting the favorable one.

| Size | R4 Aggregate ms | R3 Aggregate ms | R3 Change | Paired Changes, Rounds 1-4 |
| --- | ---: | ---: | ---: | --- |
| 1920x1080 | 5.492192 | 5.498848 | +0.121% | -1.394%, -2.461%, +7.232%, +2.550% |
| 2560x1440 | 8.734368 | 8.673920 | -0.692% | +0.669%, -0.431%, -0.950%, +0.971% |
| 3840x2160 | 18.069440 | 18.263216 | +1.072% | +0.676%, +0.799%, +1.345%, +1.475% |

At the lower resolutions, only two of four pairs favor R3. All four 4K pairs
have higher median and mean GPU time with R3. This does not establish a
population effect or causal explanation, but it fails the practical gate for
promoting an optimization. Raw warm means, p95, maxima, CPU recording times,
first evaluations, initialization and before/after GPU telemetry are retained.
Temperature and clock observations vary over the campaign; they are not
continuous in-frame measurements and cannot identify the cause of a slow run.

DXGI current-process LOCAL and NON_LOCAL usage are unchanged in every pair.
Both roles use LOCAL 624.996094, 865.621094 and 1554.121094 MiB, and NON_LOCAL
111.015625, 172.390625 and 348.265625 MiB at 1080p, 1440p and 4K respectively.
All three steady samples agree within every process. These include probe and
driver allocations, are not peak or NGX-owned figures, and do not turn reduced
per-block shared memory into a dedicated-VRAM saving.

### Disposition

Do not promote R3. Retain R4 and both earlier memory candidates unchanged;
keep R3 and its evidence for diagnosis only. The formal package and ZIP,
original runtime, existing shader/model assets and the frozen inventory of
93 production source files remain unchanged. This report is the only tracked
file updated by this experiment; no global GPU settings, commit or deployment
is changed.

The test still represents one static portrait, with upsampled 1440p/4K inputs
and zero image motion. It supports rejecting this candidate under the tested
conditions, not conclusions about arbitrary applications. A useful next step
is to identify an actual costly execution path before another tiling sweep,
rather than adding a register cap to rescue the original hypothesis.

Recheck CPU tests, stored launch/profile contracts, physical output hashes,
all raw timings and frozen artifacts without another GPU campaign:

```powershell
./tmp/dx12-ffn-rowtiles/20261005/verify_evidence.ps1
```

`final-verification.json`, `summary.json`, `run-summary.csv`, `pair-summary.csv`
and `capture-audit.json` record the final checks and interpretation. Original
build failures, successful red/green tests, resource logs, graph captures,
correctness records and every benchmark run remain in the experiment folder.

## Image Hotspots And Next Candidates: 2026-10-07

The optimization goal remains active. The user now requests joint evaluation
of GPU time, memory and initialization, permits small output differences, and
approves separate C64 weight-reuse and D3D12 half-head candidates plus
initialization phase timing. Permission for approximate output is not a
quantified quality threshold or permission to promote an unverified build.

This diagnostic compares unchanged R4 (`77114D07...`) and original NVIDIA
(`984BEE0F...`) runtimes using the existing frozen image timing probe. It does
not rebuild a runtime or modify kernel execution. Four balanced blocks run
profiling off/on for both roles at 1080p and 4K, 120 frames per process with
20 warmups and independent 200-GPU-ms preconditioning. The existing wrapper
retains whole launch chains, arguments and synchronization; all endpoint and
profile-file processing is deferred until after evaluation.

The 32 main processes retain 3840 frame timings, 3200 warm samples, 1920
profiled frames, 258240 chain intervals and 64 physical endpoint captures.
R4 has 113 chains/252 entries and the original has 156 single-kernel chains
at both sizes. Inventories stay identical across blocks within each role/size.
All first/final endpoints match across roles, profiling modes and blocks;
first frames also match the frozen original image reference. A separate
four-mode 512x512 smoke retains eight matching original-reference endpoints.
Intermediate main-campaign frames are not image-compared.

### Measured Costs

The following unprofiled figures are medians across four processes. GPU and
CPU columns aggregate each process's upper median of 100 warm samples;
initialization adds NGX init and feature creation within each run first.

| Size | Role | GPU ms | CPU Record ms | Init + Create ms | LOCAL MiB | NON_LOCAL MiB |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1080p | Original | 3.605040 | 0.409250 | 603.441 | 591.062500 | 257.769531 |
| 1080p | R4 | 5.507296 | 0.850650 | 2910.482 | 624.996094 | 111.015625 |
| 4K | Original | 10.811616 | 1.138650 | 592.789 | 1303.875000 | 495.019531 |
| 4K | R4 | 18.206192 | 0.885800 | 3198.307 | 1554.121094 | 348.265625 |

All steady memory samples agree within each process. These are current-process
DXGI figures, not peaks or exclusive NR allocations. The original's 4K
NON_LOCAL value differs from the earlier image cohort; this observation is
retained without assigning a cause or substituting the older number. Startup
processes are fresh, but OS/driver caches are retained, so these are not
cold-machine measurements. GPU evaluation excludes caller upload/readback and
does not measure game FPS. CPU and GPU measurements must not simply be added.

Additive 4K profile means place local Swin C64/C128/C256 chains at
2.623409/2.760914/3.036554 ms in R4, versus 0.970854/1.241167/1.787080 ms in the
original's corresponding broad families. Their sums are 8.420877 and 3.999101
ms. This is the largest coarse family gap, but fused transitions differ and
these are not one-to-one isolated operation comparisons. R4's C32 chains take
3.694758 ms, ViT chain 56 takes 2.447652 ms, prepare/compose take
0.498104/0.435704 ms, and unattributed gaps take 0.019508 ms, out of an
18.272622 ms mean outer interval. Waits may be inside the chain intervals.

On/off observer shifts are not uniformly small: R4 ranges from -0.508% to
+7.525% at 1080p and -6.500% to +2.449% at 4K; original ranges from -5.439% to
+0.035% and +0.008% to +4.486% respectively. All samples and runs remain,
without timing-driven repeats, outlier removal or an overhead correction.
These short diagnostic results select work; they do not qualify a small gain.

### Concrete Follow-Ups

C64 QKV has two attention heads and two K32 weight stages. The host grid is
`min(items, 12 * SM_count)`; both choices are even, so each persistent CTA
keeps the same head across iterations. Source/recorded-plan inspection finds
the same immutable weight tiles copied again for each item, although their
two existing 3 KiB shared-memory regions do not overlap A, K, V or output
staging. The current generator reproduces the frozen C64 PTX byte-for-byte.

Across eight C64 launches, per-frame source-level weight staging is
207.023438 MiB at 1080p, 350.929688 MiB at 1440p and 773.835938 MiB at 4K.
Once-per-CTA reuse would reduce each to 72 MiB on this RTX 4090. At 513x377,
each CTA handles one item, so this opportunity is zero. These are instruction
request counts, not measured DRAM traffic, VRAM allocation savings or a
predicted speedup. The candidate must preserve activation reloads, arithmetic,
asynchronous-copy group ordering and all publication/wait semantics.

The existing Vulkan graph already supports FP16 head storage, but D3D12
leaves that option disabled and its compose shader reads F32. Porting both
ends offers a separate logical head-size reduction of 63.75 MiB at padded
4K; actual process-memory savings and exact output still require validation.
Initialization source inspection also finds a real Vulkan context, module
creation and weight uploads used to record a plan, followed by D3D12 graph
creation. No NR evaluation is submitted on that temporary Vulkan graph, but
initialization is not CPU-only. Phase timing is needed before estimating how
much of the startup gap is attributable to that path.

Evidence is under `tmp/dx12-image-hotspots/20261007/`, including preregistered
protocols, raw profiles, `summary.json`, `run-summary.csv`, `observer-pairs.csv`,
all additive stage/chain tables and `weight-reuse-audit.json`. Nine analyzer
tests have retained red/green logs, covering large integer ticks, zero-length
intervals, missing/duplicate slots, overlap, bounds and inventory mismatches.
The final check revalidates 36 probe logs and all physical endpoints. No
production suite is rerun for this unchanged-probe diagnostic; the formal
package, ZIP, existing candidates, assets and 93 production-source fingerprints
remain unchanged. Recheck without another GPU campaign:

```powershell
./tmp/dx12-image-hotspots/20261007/verify_evidence.ps1
```

## C64 QKV Weight-Reuse Candidate: 2026-10-07

Status: correctness gates pass; timing shows a modest positive signal but
does not establish an across-resolution improvement or memory saving. Keep
the candidate isolated and unpromoted while the approved half-head and
initialization work proceeds. The joint optimization goal is not complete.

Both roles load the exact same R4 DLL, SHA256
`77114D070369D5E030558C04B49911C6D858D41D1A053844B7B70FD13EBA7867`.
Only `build/ptx/qkv_e4m3_K64.ptx` differs between the two asset roots:

- Baseline PTX: `752AB428D4B394BCE69B155B8CAA87FCD2F409660EC541D707DA0D1C603AE00E`.
- Candidate PTX: `14F82D6AA66B2FDCC11767E957FB7787D549F07144A95D3533E3626A392903F0`.

The private generator keeps C64's first weight tile loaded before its
persistent loop, loads the second tile only on the first item, and omits
subsequent first-tile weight prefetches. Activation copies and all async
commit/wait groups, barriers and publication counters remain. The host's even
C64 grid keeps each CTA on the same head. No tile-size, arithmetic, weight,
launch-parameter or register-cap change is included. Other asset files match
the frozen package, and generated C128/C256/C512 kernels remain byte-identical.

Offline CUDA 13.0 `ptxas -arch=sm_89` reports the same footprint for both C64
kernels: 80 registers, 16384 shared bytes, one barrier and zero stack/spills.
This is not a measurement of NVAPI JIT registers or occupancy.

### Correctness

Eight tests pass. A restricted interpreter evaluates the actual generated
copy predicates for one-, two-, three- and eleven-item CTAs: weight copies
occur once while all per-item activation copies remain. The three repetition
tests fail against the original generator before the change. Other tests
compare generated synchronization and floating/tensor instruction sequences,
allowing register renumbering. These structural checks are not a proof of all
PTX addresses or numerical behavior; the GPU comparisons supply separate evidence.

The same frozen R4 full-head executable runs against each asset root at
513x377, 1080p, 1440p and 4K. Dedicated/reuse/poison routes, four changing
feature inputs and queued consumers produce 96 evaluations, with all 48
candidate route/frame comparisons exact. All 16 independent baseline/candidate
full-head file pairs are physically rehashed. The 18-case native NGX matrix
passes 75 fresh original/candidate frame pairs, and nine image cases pass all
58 new baseline/candidate output comparisons against stored original captures.
A three-frame 1080p profile smoke confirms unchanged launch inventory and
matching endpoints. ABI implementation is unchanged; the full Vulkan suite
is not rerun for this private PTX candidate.

### Four-Round Timing

Every process runs 600 frames with 200 warmups, profiling off, independent
200-GPU-ms preconditioning and deferred CPU endpoint processing. Role order
alternates and resolution order varies under the saved protocol. All 24
processes, 14400 raw timings, 9600 warm samples and 48 physical endpoints are
retained. Both endpoints match the frozen original 600-frame image outputs.
No slow run or sample is removed or repeated because of its timing.

Values below are medians of four process warm GPU medians, averaging the
middle two. Each process uses the upper median of 400 warm samples. Positive
changes mean increased GPU time; paired ratios are reported separately.

| Size | Baseline ms | Candidate ms | Aggregate Change | Paired Changes, Rounds 1-4 |
| --- | ---: | ---: | ---: | --- |
| 1080p | 5.371168 | 5.293872 | -1.439% | -1.689%, -0.563%, -3.197%, -0.105% |
| 1440p | 8.424640 | 8.438048 | +0.159% | -1.441%, -1.034%, +3.142%, -0.262% |
| 4K | 17.905552 | 17.859440 | -0.258% | -0.431%, -0.151%, -0.724%, -0.208% |

All four 1080p and 4K median pairs favor the candidate; three of four 1440p
pairs do, but its aggregate median ratio is slightly worse. The 4K fourth
pair's warm mean and p95 are also worse despite its lower median. These are
small descriptive effects, not statistical equivalence, a universal speedup,
or proof that the eliminated staging requests were the limiting hardware cost.

LOCAL is unchanged in every pair: 624.996094, 865.621094 and 1554.121094 MiB
at 1080p, 1440p and 4K. NON_LOCAL normally matches at 111.015625, 172.390625
and 348.265625 MiB. In 1080p round 4 the candidate uses 0.250 MiB more
NON_LOCAL, already present after creation and stable in all three steady
samples. Its cause is not established; the observation is retained, so total
memory equality or memory improvement is not claimed. These are process
DXGI samples including probe/driver allocations, not peaks or exclusive NR use.

Baseline/candidate init-plus-create medians are 2661.594/2636.191 ms,
2742.642/2756.285 ms and 3060.176/3059.307 ms at the three sizes. CPU recording
medians are 1.025100/1.075350 ms, 1.099500/1.107350 ms and
1.122200/1.128300 ms. No host-path change was made; these observations do not
establish a startup or CPU improvement. Per-run means, p95, first evaluations,
memory phases and telemetry remain in the raw records and summary CSVs.

### Disposition And Evidence

The candidate remains inconclusive for general adoption, rather than being
promoted using the 1080p result alone. Output quality was not traded away in
any tested case. The larger structural opportunities remain FP16 head storage
and initialization; each will be measured independently before considering
a combined variant. The test still uses one static portrait, with upsampled
4K/1440p inputs, and does not measure application FPS or real image motion.

Evidence is under `tmp/dx12-qkv-weight-cache/20261007/`. The only private PTX
change, generator tests, assembly logs, full-head outputs, native/image checks,
protocol, every benchmark process and final verification are retained. A
PowerShell interpolation syntax error in the new qualification harness was
fixed at parser validation before any qualification GPU run. Formal DLL/ZIP,
previous candidates, original runtime and all 93 protected production source
files remain unchanged. Recheck without a new GPU campaign:

```powershell
./tmp/dx12-qkv-weight-cache/20261007/verify_evidence.ps1 -Disposition inconclusive -Reason 'Exact output; modest 1080p/4K signal, mixed 1440p result; no memory or startup improvement established.'
```

## D3D12 FP16 Head Candidate: 2026-10-07

Status: retain as an unpromoted memory-saving candidate. This experiment is
independent of C64 weight reuse: its baseline remains R4 `77114D07...`, with
the original C64 PTX. Candidate DLL SHA256 is
`200D9A7994B2A407D2D4C6FFDA4F2BE361D08F84DFE6BDE4E76A09BBD8A47031`.

Two private source changes enable the graph's existing FP16 head option and
make `nr_compose` load packed half values and widen them before its unchanged
F32 calculations. The existing post/head kernel becomes `block32_e4m3_f560`
instead of `block32_e4m3_f48`. This stores the already-half accumulators, rather
than introducing a new reduced-precision accumulation scheme. No model
weights, C64 optimization, workspace growth or split-K capacity logic changes.

Only `build/ngx/nr_frame.ptx` differs in the copied asset directory. Its
SHA256 changes from `D284A197...` to `AA078D0D...`; full identities are in
`tmp/dx12-fp16-head/20261007/variant.json`. Generated `nr_prepare` is unchanged
and the compose parameter ABI is unchanged. Offline `ptxas` reports 37 to 38
registers for compose, 33 for prepare in both versions, and no stack/spills.
These are not NVAPI JIT resource measurements.

### Allocation And Output Gates

Before the change, the real allocation test fails at 513x377 because both
DLLs report 195887104 graph-owned bytes. After the change, the same test passes:
graph and total feature-owned allocation sums fall by 1966080 bytes at 513x377
and 66846720 bytes at 4K. These sums use D3D12 resource allocation information,
not just tensor payload arithmetic. The failed-before DLL and logs are retained.
The only build warning is the pre-existing `nr_model.cpp` C4458 warning.

Four sizes run 48 new graph evaluations with dedicated/reuse/poison storage,
changing inputs and queued consumers. The test retains 16 packed F16 head
files and 16 expanded F32 files. Every expanded byte matches the independently
captured R4 F32 head; the 16 reused reference files are physically rehashed.
The analyzer independently expands the physical half files and verifies every
F32 byte, rather than comparing shortened buffers. Raw half bits also match
between the three workspace routes.

The rebuilt DLL passes ABI/lifecycle and workspace-policy gates, the 18-case
native matrix (75 fresh original/candidate pairs), and nine image cases
(58 baseline/candidate comparisons against original captures). A three-frame
1080p profile confirms exactly one intended post/head function-name change,
with every other inventory field preserved. The full Vulkan suite is not
rerun for this private D3D12 integration; production source remains unchanged.

### Paired Measurements

The same four-round, 600-frame/200-warmup protocol retains all 24 processes,
14400 raw timings, 9600 warm samples and 48 original-matching endpoint files.
Profiling is off, preconditioning is independent of NR history, and CPU
endpoint processing is deferred. No timing-based exclusions or repeats occur.
Intermediate benchmark frames are not image-compared. GPU values are the
median of four per-process upper medians, not game FPS or whole-frame cost.

| Size | R4 GPU ms | FP16 GPU ms | GPU Change | R4 LOCAL MiB | FP16 LOCAL MiB | Saved MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1080p | 5.498832 | 5.411248 | -1.593% | 624.996094 | 608.121094 | 16.875 |
| 1440p | 8.721088 | 8.653136 | -0.779% | 865.621094 | 836.871094 | 28.750 |
| 4K | 18.281584 | 18.116512 | -0.903% | 1554.121094 | 1490.371094 | 63.750 |

Paired GPU changes are -1.635%, -1.834%, -0.360%, -0.687% at 1080p;
+4.204%, -2.315%, -2.532%, +0.195% at 1440p; and
-0.576%, -1.827%, -1.227%, +0.068% at 4K. Therefore lower aggregate medians
do not establish an improvement in every pair. In 1080p, two warm means and
three p95 values are worse despite all four lower medians. The memory result
is the stronger reason to retain this candidate, not a universal speed claim.

All twelve memory pairs reproduce the LOCAL reductions, while NON_LOCAL is
unchanged at 111.015625, 172.390625 and 348.265625 MiB respectively. All three
steady samples agree within every process. These are DXGI current-process
measurements including probe and driver allocations, not peaks or exclusive
NR residency.

R4/FP16 init-plus-create medians are 2605.779/2601.803 ms,
2600.298/2607.297 ms and 2729.184/2690.591 ms. CPU-recording medians are
0.496050/0.572050 ms, 0.526400/0.517650 ms and 0.581250/0.599000 ms. No
initialization or CPU improvement is established by this storage change.
The shared static portrait, upsampled high-resolution views, and absence of
a real application remain limitations. No output-quality difference is found
in the tested cases, despite the user's permission to investigate small ones.

Full source/build identities, allocation red/green evidence, half/F32 heads,
ABI/policy checks, native/image captures and all timing/memory records are in
`tmp/dx12-fp16-head/20261007/`. Formal DLL/ZIP, prior candidates and all 93
protected production-source files remain unchanged. Recheck without another
GPU campaign:

```powershell
./tmp/dx12-fp16-head/20261007/verify_evidence.ps1 -Disposition keep-unpromoted -Reason 'Repeatable LOCAL savings with exact tested outputs; mixed timing/CPU observations, no general speedup claim.'
```

## Initialization Phase And Hash Isolation: 2026-10-07

This separate diagnostic starts from R4 F32, without FP16 head or C64 weight
reuse. A private DLL adds twelve constructor wall intervals and prints their
records only after successful construction. Removing the timing statements
and include reproduces the canonical feature source exactly. Constructor
intervals include CPU work and any GPU waits; they are not exclusive CPU time.

Two three-frame image smoke runs match all six stored original outputs. Four
balanced rounds at 1080p and 4K then compare unchanged R4 and timing-only R4
in 16 fresh processes. The main campaign performs creation/release only,
never `EvaluateFeature`. Input files are loaded consistently but feature
creation does not consume their pixels. OS/driver caches remain warm; these
are not cold-machine startup measurements. All runs are retained.

The eight instrumented constructors yield 96 validated additive intervals.
Each constructor fits inside the probe's outer create/submit interval. Mean
constructor decompositions are:

| Region | 1080p Mean ms | 4K Mean ms |
| --- | ---: | ---: |
| Vulkan context creation | 270.988 | 271.224 |
| Model file/JSON/hash loading | 1009.597 | 1015.879 |
| Graph recording/preparation | 888.086 | 918.456 |
| Vulkan-side destruction | 155.427 | 175.780 |
| D3D12 graph creation | 301.828 | 360.973 |
| Other constructor work | 40.422 | 50.613 |
| Complete constructor | 2666.348 | 2792.923 |

Uninstrumented/instrumented outer create medians are 2702.320/2644.998 ms at
1080p and 2779.335/2805.461 ms at 4K. Individual on/off changes span -2.713%
to +0.377% and -3.526% to +4.117% respectively; no constant observer correction
is applied. Owned D3D12 allocation sums match in every pair.

The largest region is model loading, about 36%-38% of constructor time.
Inspection of `Model::Model` shows file reading, JSON/tensor metadata work and
SHA-256 validation, with no GPU calls in that region. Graph recording is a
mixture of necessary CPU weight preparation and Vulkan allocation/module/upload
work. Its entire duration must not be treated as removable Vulkan overhead.

### Independent Hash Probe

A CPU-only executable compares the actual scalar `sha256Hex` implementation
with Windows CNG over the same preloaded model bytes. It uses Microsoft's
[single-shot BCryptHash API](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcrypthash)
with SHA-256 and no secret/key. Nothing is changed in a runtime's loader or
integrity policy. The test checks three known vectors, 48 length/alignment
cases, every timed model digest against the manifest, and single-byte
corruption rejection by both implementations for all 11 stages.

There are 147683778 model bytes and 88 timed stage digests across four
alternating scalar/CNG rounds. Summing all stages within each round gives:

| Round | Existing Scalar ms | CNG ms |
| --- | ---: | ---: |
| 1 | 647.0411 | 102.2450 |
| 2 | 648.1793 | 102.1363 |
| 3 | 646.1493 | 102.5389 |
| 4 | 978.0401 | 105.6286 |
| Median | 647.6102 | 102.3920 |

The slower fourth scalar observation is retained. Manifest parsing takes
1.4475 ms and stage reading/allocation 337.131 ms in one separate observation;
those single observations are not repeated I/O benchmarks. Hash timings
exclude provider/DLL setup, file reads and other constructor work. The roughly
545 ms median hash difference is not yet an actual DLL startup improvement.

The user has approved a separate CNG model-loader candidate with the current
software implementation retained as a fallback, full integrity checks and
independent A/B testing. That integration is the next work item, not an
accomplished result at this checkpoint. Do not bypass checks or combine it
with FP16 head before its independent result is known.

Evidence is under `tmp/dx12-init-phases/20261007/`: source-only timing audit,
missing-marker red test, six phase-validator tests, image smoke, startup
protocol/records, additive tables, and `hash-isolation/` source/build identities,
all observations and digest checks. Formal packages and production code are
unchanged; no driver/counter/clock setting is changed. Recheck retained evidence:

```powershell
./tmp/dx12-init-phases/20261007/verify_evidence.ps1
```

## Independent CNG Model Loader: 2026-10-07

The approved follow-up is now implemented and measured privately in
`tmp/dx12-cng-model/20261007/`. It starts from R4 F32 and uses exactly the
formal baseline's model/shader/PTX asset root. It includes neither the C64
cache nor FP16 head, and has no constructor phase instrumentation.

| Runtime | SHA-256 |
| --- | --- |
| R4 baseline | `77114D070369D5E030558C04B49911C6D858D41D1A053844B7B70FD13EBA7867` |
| CNG candidate | `AC1F2C14BA5EDEF062986C34FCF6ACC5EF73194B3DBA1BC180592AE58049D186` |
| Unavailable-provider diagnostic | `F3AF437E078814664C5294688BB2436E417F3EA0D06360CC9809AF05A234FD3A` |

### Change And Integrity

The private `nr_model.cpp` differs only by including the new verifier,
constructing its local provider owner, and replacing the stage-digest call.
Undoing these three substitutions reproduces the canonical model source.
R4 memory implementations are byte-identical to their frozen copies. No
public API, tensor layout, graph, shader, frame operation or integrity default
is changed. Existing stage-size validation and error messages are preserved.

The provider owner spans one Model constructor and reuses the algorithm handle
for all stages. It loads `bcrypt.dll` only from System32, dynamically resolves
the required entry points, and uses
[BCryptHash](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcrypthash)
with a 32-byte SHA-256 result and no HMAC key. The provider and module are
released when construction finishes or unwinds, not from a global destructor
or DllMain. Microsoft documents the algorithm handle's lifetime in
[BCryptOpenAlgorithmProvider](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptopenalgorithmprovider),
and the restricted DLL search flag in
[LoadLibraryExW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw).
Import inspection confirms no mandatory BCrypt import in the candidate DLL.

Missing entry points, provider/hash failure, or input too large for the
single-shot ULONG length return to the original, unchanged `sha256Matches`.
A failed native digest is discarded even if its bytes look valid. There is no
digest cache, skipped stage, `verifyHashes=false` call, or new runtime bypass.
The existing explicit Model API option remains unchanged; the actual D3D12
constructor continues using its default full verification.

Verification includes:

- A meaningful scalar-only RED gate, then 133 passing hash case groups:
  known vectors, padding/alignment, input preservation, uppercase/lowercase,
  malformed hashes, all 64 digest positions, misleading failed-provider output,
  empty inputs and the native ULONG boundary. The oversized software path is
  reviewed, not exercised with a multi-gigabyte allocation.
- Six startup-log validator tests. The incomplete parser produces 14 failing
  validation assertions and one missing-record error; the final validator passes
  all six tests and rejects duplicate/missing/invalid timing and lifecycle data.
- 48 actual DLL creation rejections: 16 inputs across R4, CNG and a separately
  linked unavailable-provider diagnostic. Each of the eleven stages is corrupted
  individually, followed by truncation, short/nonhex/wrong digest, and missing-file
  cases. Only private fixtures are modified and restored. No rejected model
  reaches inference. The diagnostic uses the real verifier and software fallback,
  not a production environment flag or a disabled integrity check.
- Three valid fallback frames, 75 fresh native-reference pairs, 58 image
  comparisons and four profiled endpoints all match exactly: 140 physical capture
  comparisons. ABI/lifecycle/capability and workspace-policy checks pass. Paired
  three-frame launch inventories are identical. Full-head captures are not rerun
  because this candidate changes neither graph execution nor any asset.

Builds emit only the already-existing `nr_model.cpp` C4458 shadow warning.
The author performed a separate self-review, not an independent-agent review.
No real application, full Vulkan suite, alternate Windows version or deliberately
disabled OS CNG provider was tested. The link-time failure diagnostic and native
known-vector tests cover the fallback boundary without changing system settings.

### Actual Creation A/B

Four balanced blocks alternate baseline/CNG order and 1080p/4K order, yielding
16 fresh processes. Input images are loaded consistently, but the main cohort
does not call EvaluateFeature. Provider setup, all model checks and graph
construction remain inside the measured CreateFeature/submit wall interval.
OS/driver caches are retained; no runs are repeated or excluded based on timing.
The GPU is the same RTX 4090 on driver 617.14.

| Size | R4 Create Median ms | CNG Create Median ms | Median Difference ms | Change |
| --- | ---: | ---: | ---: | ---: |
| 1080p | 2624.876 | 2025.692 | -599.185 | -22.827% |
| 4K | 3109.115 | 2124.767 | -984.348 | -31.660% |

All eight paired create intervals decrease. Per-block changes are
`[-22.377, -23.067, -22.397, -23.250]%` at 1080p and
`[-31.600, -33.964, -31.737, -29.584]%` at 4K. Medians including DLL load plus
NGX Init plus Create/submit are 2625.559/2026.339 ms and 3109.790/2125.412 ms,
respectively. These totals exclude process spawning, input-file loading and
other probe setup; they are not end-to-end application launch times.

Owned allocation sums match in every pair: 464257024 bytes at 1080p and
1291059200 bytes at 4K. After-create LOCAL residency matches in all eight pairs.
NON_LOCAL matches in seven pairs; block 2 at 1080p is 262144 bytes higher in
the candidate. This is a retained observation, not a memory-reduction claim.
No steady-state frame-time campaign is run for this startup-only change.

The 4K baseline in this cohort is slower than in the earlier phase-diagnostic
cohort. Its roughly 984 ms create difference also exceeds the earlier roughly
545 ms isolated-hash median difference. The observations are from different
cohorts and scopes: do not subtract the old microbenchmark from this result,
attribute every saved millisecond to hashing alone, or extrapolate a fixed
resolution-dependent hash benefit. The evidence supports whole-create speedup
in this balanced local cohort, not cold-machine, cross-platform or FPS claims.

### Joint Disposition

Keep CNG as an **unpromoted startup candidate**: a repeatable observed creation
benefit with full validation and exact tested outputs. Its costs are a small
Windows-specific provider wrapper and dependence on CNG availability for the
fast path; fallback keeps correctness, not the speed benefit. Do not weaken
integrity for additional speed.

Across the independent experiments, prioritize CNG for startup and FP16 head
for the repeated 16.875/28.750/63.750 MiB LOCAL reductions. Keep the C64 cache
unpromoted/inconclusive because its benefit is not consistent enough to justify
the added kernel complexity. FP16's mixed frame-time/tail results also do not
justify a universal speed claim. Combining CNG with FP16 would be a new
interaction-validation experiment, not an already-tested or published package.

Formal DLL/ZIP, prior candidates, all 93 protected production-source files,
assets and GPU/driver settings remain unchanged. This completes the approved
independent candidate experiments, not every possible optimization. Retained
evidence can be rechecked without a new GPU timing campaign:

```powershell
./tmp/dx12-cng-model/20261007/verify_evidence.ps1 -Disposition keep-unpromoted -Reason 'Eight paired creation improvements with full validation and exact tested outputs; warm-cache local startup evidence only, no FPS claim.'
```

## Feature-Owned Input Reuse: 2026-10-07

The continuing objective is to reduce VRAM and overhead, not to stop after
the independent startup improvement. The user approved a separate candidate
that recycles the D3D12 Feature's own input storage after its last graph use.
This experiment excludes CNG, FP16 head and C64 weight caching, and compares
against the same frozen R4 F32 baseline. Formal packages remain untouched.

The candidate is `tmp/dx12-input-reuse/20261007/build/candidate/nvngx_dlssnr.dll`,
SHA-256 `29E182FF5DAE4101C038876834C869C07F248DC090E57E03BF28E6A3617FAAAB`.
Baseline SHA-256 remains
`77114D070369D5E030558C04B49911C6D858D41D1A053844B7B70FD13EBA7867`.
Both load the same formal model/shader/PTX asset root; no asset differs.

### Lifetime Evidence

Before implementing reuse, a private diagnostic captured eight unmodified R4
command traces: 513x377, 1080p, 1440p and 4K, each with workspace poisoning off
and on. It records graph commands but does not submit graph inference. Buffer
first/last uses are reconstructed independently from PTX arguments, shader
bindings, copy endpoints and clears, rather than inferred from CPU allocation
order or just tensor names.

All four ordinary traces use the input last at operation 3, contain a global
barrier at 4, and first use the selected workspace at 5. Poisoned traces have
the corresponding sequence 6, 7, 8. Each trace has ten eligible later workspace
buffers; the candidate selects the largest one that fits without growing the
input resource, consistently `graph workspace 2` in this corpus. Microsoft's
[UAV barrier contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_uav_barrier)
orders prior UAV reads/writes before subsequent accesses; the existing global
barrier is retained. No extra per-frame barrier or kernel is inserted.

| Shape | Input Extent MiB | Retired Workspace Extent MiB |
| --- | ---: | ---: |
| 513x377 | 7.500 | 1.875 |
| 1080p | 67.500 | 16.875 |
| 1440p | 115.000 | 28.750 |
| 4K | 255.000 | 63.750 |

The 255 MiB input is not eliminated: one later workspace shares its storage,
so the saving is the retired workspace's size, not the entire input size.
After Vulkan planning has finished and its objects are destroyed, a private
Feature-only helper remaps that workspace's command references to the input.
The existing D3D12 constructor already skips buffers with no remaining uses.
Offsets, scalar values, command order and synchronization stay unchanged.
An external head index would also be remapped if it named the retired buffer.

The generic Graph/Dx12Graph implementations and borrowed-input contract stay
unchanged. The helper is called only for Feature-owned disposable input; its
label check is not a general proof of external ownership. Host-visible buffers,
uploaded/immutable data, insufficient capacity, overlapping lifetimes, missing
barriers, invalid references and unsupported operations retain the original
allocation. Dedicated-workspace mode also keeps its prior behavior.

### Correctness And Allocation

The CPU planner gate first fails against a no-op implementation, then passes
25 behavior cases. These cover overlap, late reads through every reference
field, missing synchronization, ineligible storage, invalid offsets/indices,
unknown operations, deterministic selection and complete reference remapping.
A separate actual-DLL RED test observes zero allocation reduction. GREEN
creation tests then measure 1966080 bytes saved at 513x377 and 66846720 at 4K
in both graph and total Feature-owned allocation sums.

Four full-head processes run five routes each: dedicated, ordinary reuse,
ordinary poison, owned-input reuse and owned-input poison. Across 80 graph
evaluations and 64 route comparisons, complete F32 heads match exactly, with
changing inputs, signed zero, negative values and subnormals. The final two
evaluations and their output consumers are queued in one submission. Sixteen
new optimized-head captures also match the frozen independent R4 captures.

Fresh ABI/lifecycle/capability and workspace-policy checks pass. All 75 native
reference pairs, 58 image comparisons and four profiled endpoints match. The
paired three-frame kernel inventories are identical. This is not just an
unchanged final image after an unchecked intermediate remap.

### Independent Timing

The predeclared campaign has four balanced rounds, three resolutions and two
roles: 24 processes, 600 frames each with 200 warmups, 14400 retained raw frame
timings and 9600 warm samples. Profiling is off; 200 GPU-ms preconditioning and
deferred CPU capture processing match the preceding protocols. All 48 endpoint
files match the original NVIDIA references. No timing-driven repeats or
exclusions are used.

| Size | R4 GPU Median ms | Reuse GPU Median ms | Change | LOCAL Saved MiB |
| --- | ---: | ---: | ---: | ---: |
| 1080p | 5.609936 | 5.650608 | +0.725% | 16.875 |
| 1440p | 8.857760 | 8.924352 | +0.752% | 28.750 |
| 4K | 18.542736 | 18.393904 | -0.803% | 63.750 |

These are medians of four process upper medians, not pooled-frame confidence
estimates. Paired changes are `[-0.080, -1.425, +5.799, -0.810]%` at 1080p,
`[+0.049, +2.942, +0.676, +1.471]%` at 1440p and
`[-2.456, +2.325, -1.716, +0.076]%` at 4K. All four 1440p medians and p95s
are slower; p95 changes range from +0.040% to +2.222%. The 1080p round-3
regression is retained. Fewer resources do not establish lower frame time,
and the cause of these timing differences has not been isolated.

All twelve LOCAL pairs reproduce the exact allocation saving. Baseline/reuse
LOCAL values are 624.996094/608.121094, 865.621094/836.871094 and
1554.121094/1490.371094 MiB. Every process has three agreeing steady samples.
NON_LOCAL is unchanged except for 1080p round 2 (-0.015625 MiB) and round 4
(+0.015625 MiB). These are current-process DXGI measurements including probe
and driver costs, not exclusive NR memory or initialization peaks.

R4/reuse CPU-recording medians are 0.326450/0.393550, 0.364050/0.364950 and
0.431800/0.385900 ms. Init-plus-create medians are 2633.776/2639.130,
2625.568/2639.916 and 2716.578/2727.430 ms. No general CPU or startup benefit
is established. Temporary Vulkan planner allocations are unchanged, so this
candidate does not establish a lower initialization peak either.

### Disposition And Scope

Retain as an **unpromoted memory-saving candidate with a measured frame-time
tradeoff**, not an unconditional default improvement. The footprint benefit is
repeatable and tested outputs are exact, but the consistent 1440p slowdown
must not be dismissed simply because instructions and launch counts match.
Do not add its saving to FP16's or claim a combined result without measuring
their interaction. Further optimization remains active.

The source change is a private helper plus a six-line Feature insertion; all
93 protected production-source files, earlier candidates and formal DLL/ZIP
are unchanged. Only the existing nr_model.cpp C4458 warning appears in builds.
The review is the author's self-review, not an independent review. Limits
remain one GPU/driver, static portrait views plus synthetic controls, no real
application and no full Vulkan suite rerun. Higher-resolution portrait views
are upsampled. The head, image and benchmark evidence is retained separately
from the prior FP16 and CNG experiments.

```powershell
./tmp/dx12-input-reuse/20261007/verify_evidence.ps1 -Disposition keep-unpromoted -Reason 'Repeatable LOCAL reduction and exact outputs; mixed timings including four slower 1440p pairs. Memory option only, not a default speedup or combined-candidate claim.'
```

## Combined Baseline And Marginal Input Reuse: 2026-10-07

The user approved a private CNG-plus-FP16 baseline followed by a marginal
input-reuse comparison. Arm A is CNG model validation plus FP16 head storage;
arm B is exactly A plus the Feature-owned input/workspace remap. Neither R4
nor the original NVIDIA DLL is a timing arm in this experiment. C64 weight
caching remains excluded, and neither combination is promoted.

All new work is under `tmp/dx12-combined-input/20261007`. Runtime identities:

| Role | DLL SHA-256 |
| --- | --- |
| A: CNG + FP16 | `EF838D0EDB58FE363B9134D78E338E8733381D8308EB3C3ADD8B7B1E9E6AF7D4` |
| B: A + owned-input reuse | `0565E972E07FEB0853B0A11BFFB8B99EDDEF40DBF5E0F702DA2E4738C66DB82B` |
| B with failed native-hash provider, diagnostic only | `D60438BA8E7F82AA0CE87AB05FB42ABF82D2ED6C8FC104CD855CF8EB5B6C6C58` |

Both timing arms use the same frozen FP16 asset directory, with no new PTX or
shader compilation. The CNG loader/wrapper, input-reuse helper and R4 graph
implementation are copied byte-for-byte from the earlier candidates. A's
Feature source equals the frozen FP16 Feature source. Removing B's private
include and five-line remap/log block recovers A exactly. No external-input
contract, graph operation order, barrier or kernel is added by B.

### Reused Gold And Composition Gates

`gold.json` freezes existing correctness references rather than rerunning the
original NVIDIA DLL. Its SHA-256 is
`A451268090FC28CDFA0726DF66D80DCDE8D8496D7891EA68A628E76DE234B56B`.
The provenance check verifies prior capture-audit hashes against their final
verification records, then rehashes the physical native, packed/expanded head,
image and timing-endpoint gold files. There are zero new original-reference
processes in this experiment; this is reference reuse, not new independent
reference coverage.

The actual-DLL allocation RED gate compares A with A and fails on zero saving
against the expected 1966080 bytes at 513x377. After adding B's remap, GREEN
records exactly 1966080 bytes saved at 513x377 and 66846720 at 4K, in both graph
and total Feature-owned allocation sums. At 4K the owned sums are
1224212480 bytes for A and 1157365760 for B. The input resource remains alive;
the eliminated allocation is one later workspace, not the whole input.

Four full-head processes exercise A reuse, A poison, B reuse and B poison
across 64 graph evaluations and 48 route comparisons. Inputs change each
frame and include signed zero, negative values and subnormals. The final two
evaluations and their consumers share a submission. Sixteen new packed F16
heads and sixteen expanded F32 heads match their frozen FP16 gold files
exactly; packed values also widen to the captured F32 bit patterns exactly.

Both DLLs pass fresh ABI/lifecycle/capability and workspace-policy checks.
Eighteen synthetic cases per DLL yield 150 native-output comparisons against
75 reused original references. Nine image cases per DLL add 58 comparisons.
All are exact. Two three-frame profile smokes have identical kernel inventories,
including the FP16 head kernel `block32_e4m3_f560`, and four exact endpoints.

Composition-specific integrity tests independently corrupt the first and last
model stages, truncate the first stage, and supply a nonhex expected digest.
All four cases are rejected before Evaluate by A, B and the failed-provider
diagnostic: twelve actual-DLL rejections. Three valid diagnostic frames match
the original gold through software SHA-256 fallback. This is a bounded new
composition matrix, not a repeat of the earlier 48-rejection CNG matrix.
The unchanged components retain that earlier evidence, and the final check
reruns their 133 hash case groups and 25 planner behavior cases. Validation is
never bypassed, and no production failure-injection flag is introduced.

### Marginal Timing And Memory

The predeclared balanced campaign runs four rounds, three resolutions and two
roles: 24 fresh processes, 600 frames each, 200 warmups, 14400 raw timings and
9600 warm samples. It retains 200 GPU-ms preconditioning, deferred CPU capture
and profiling-off main runs. All 48 timing endpoints match the frozen original
NVIDIA outputs. No timing-driven exclusions or repeats are used; driver 617.14
and GPU settings are unchanged.

The following are medians of four process upper medians, not a pooled-frame
estimate. Positive changes mean slower GPU execution.

| Size | A GPU Median ms | B GPU Median ms | B/A Change | Additional LOCAL Saved MiB |
| --- | ---: | ---: | ---: | ---: |
| 1080p | 5.662304 | 5.696688 | +0.607% | 16.875 |
| 1440p | 8.970896 | 8.940112 | -0.343% | 28.750 |
| 4K | 18.907136 | 18.748288 | -0.840% | 63.750 |

Paired changes by round are `[+0.939, -2.229, +1.635, +1.336]%` at 1080p,
`[-0.504, -0.743, +0.059, -2.879]%` at 1440p and
`[-0.634, -0.787, -1.744, +0.196]%` at 4K. B is slower in three of four
1080p pairs and faster in three of four pairs at each higher resolution.
Paired p95 changes range from -2.212% to +1.691%, -3.532% to +1.400%, and
-1.923% to +0.307%, respectively. The elevated 1440p round-4 A CPU/GPU
observation is retained; neither it nor any other observation is removed.

All twelve LOCAL pairs reproduce the exact incremental saving. A/B residency
is 608.121094/591.246094 MiB at 1080p, 836.871094/808.121094 at 1440p and
1490.371094/1426.621094 at 4K. Every process has three agreeing steady samples.
NON_LOCAL is unchanged in every pair: 111.015625, 172.390625 and 348.265625 MiB,
respectively. These are current-process DXGI measurements including probe and
driver allocations, not exclusive NR memory or initialization peaks. There is
no fresh R4 memory arm, so these results establish B's increment over A rather
than a newly measured combined saving against R4.

A/B CPU-recording medians are 0.322450/0.318150, 0.335800/0.332650 and
0.329000/0.337400 ms. Init-plus-create medians are 2080.466/2064.992,
2106.712/2076.002 and 2174.498/2126.564 ms. Both arms already use CNG; these
numbers cannot establish a new CNG-versus-software startup benefit. They also
do not establish a general CPU or application-launch improvement. No cold-OS
startup campaign or peak-allocation measurement is performed.

### Disposition And Limits

The prior F32/R4 cohort's four slower 1440p pairs do not repeat on this combined
baseline. That is an observation, not proof that FP16 fixed the cause: baseline
and cohort changed, and cache/placement effects have not been isolated. The
1080p regressions and mixed tails still preclude an unconditional speedup or
performance-equivalence claim. Four process pairs per size are descriptive;
9600 frame samples are not 9600 independent experiments.

Keep both combinations **unpromoted**. B is a tested memory-saving option on
the combined baseline, with exact tested outputs and a resolution-dependent
timing tradeoff. The strongest result is the repeated additional LOCAL saving,
not a universal frame-time gain. The continuous VRAM/overhead goal remains
active; completing this bounded A/B does not complete the broader objective.

All 93 protected production-source files, old candidates, original references,
formal DLL/ZIP and assets remain unchanged. Only private experiment files and
this appended report are new. Builds retain the known nr_model.cpp C4458
warning. Review is self-review only. Coverage remains one GPU/driver, one static
portrait with resized/cropped views plus synthetic controls; higher-resolution
portrait inputs are upsampled. There is no real-application test, full Vulkan
suite rerun, motion-scene quality claim or game-FPS claim.

The final evidence check revalidates retained artifacts without another timing
campaign and records report/artifact hashes in `final-verification.json`:

```powershell
./tmp/dx12-combined-input/20261007/verify_evidence.ps1 -Reason 'Combined A/B retains exact outputs and incremental LOCAL savings; timings mixed, 1080p slower in three pairs; no universal speedup or cross-cohort attribution.'
```

## Disjoint Input Slices: 2026-10-07

The user approved a private input-buffer slicing candidate after the read-only
audit. This experiment replaces single-workspace recycling with disjoint slices
for several later workspaces. It does not overwrite input inside the pre kernel,
change arithmetic or kernels, or combine a new hash/precision optimization.

Arm A is the preceding CNG+FP16+single-input-reuse DLL, SHA-256
`0565E972E07FEB0853B0A11BFFB8B99EDDEF40DBF5E0F702DA2E4738C66DB82B`.
Arm B is `tmp/dx12-input-slices/20261007/build/candidate/nvngx_dlssnr.dll`,
SHA-256 `B03AB0FCF6135CC5403526C2ACB54803997D71AA7C0AB4B8722825C3223ADD16`.
Both load the exact same frozen FP16 assets and byte-identical CNG loader/hash
components. R4 and the original NVIDIA DLL are not timing arms. Earlier DLLs,
raw measurements and gold captures are preserved rather than rebuilt.

### Fresh Traces And Storage Design

Before implementing the allocator, eight fresh CNG+FP16 command traces cover
513x377, 1080p, 1440p and 4K, each with poisoning off/on. They confirm the input's
last use at operation 3, the existing global barrier at 4, and eligible workspace
uses afterward; poisoned traces have input-last/barrier operations 6/7. The
trace executable records graph commands but does not submit graph inference.
Both pre `block32_e4m3_f330` and FP16 head `block32_e4m3_f560` are checked.

Six workspaces fit into nonoverlapping 64-KiB-aligned slices in every trace.
At 4K their extents are two 63.750 MiB regions and four 31.875 MiB regions,
exactly filling the existing 255 MiB input allocation. The old candidate already
retired one 63.750 MiB workspace, so the marginal prediction is 191.250 MiB,
not another 255 MiB. No input allocation is removed or enlarged.

The private D3D12 backend now keeps logical lengths separate from physical
backing resources and offsets. PTX arguments retain their inner offsets;
translated shader pointers receive the same slice base. Frame-time clears use
each logical length, never the entire input size. Initialization clears each
physical root once, and allocation accounting counts that root once. The raw
resource accessor rejects sliced logical buffers rather than returning an
offset-blind handle. Feature input and retained head remain ordinary resources.

Slicing requires explicit Feature-owned disposable-input opt-in, complete
reference/lifetime validation, sufficient capacity and an existing post-input
barrier. Host-visible/uploaded storage, retained outputs, early uses, unknown
operations and malformed references are excluded or fall back. Copy endpoints
are not sliced; a copied input disables the optimization, preserving the existing
copy/state-transition path. Logical trace operations, arguments, kernel assets
and synchronization order are not rewritten. The external borrowed-input and
NGX texture contracts remain unchanged; dedicated-workspace mode is preserved.

### Correctness Gates

The CPU layout test first fails against the no-op planner, then passes 34 cases
covering disjoint ranges, alignment gaps, capacity/overflow, late references,
poison clears, excluded storage, copied endpoints and default opt-out. An initial
MSVC local-struct default-initialization compile error was corrected with an
explicit aggregate initializer; its failed build log is retained separately.

A real GPU boundary test first fails against the backend without slicing, with
49156 differing words. With slicing enabled it verifies all 65536 words of a
256 KiB backing buffer: three logical regions, an untouched alignment gap,
nonzero PTX inner offset, translated shader input/output bindings, and three
queued graph records. There are zero differing words, and only one 256 KiB
physical resource is allocated. No mock GPU implementation is used.

The actual-DLL allocation RED gate compares A with itself and fails on zero
saving. GREEN then observes 5898240 bytes saved at 513x377 and 200540160 at 4K
in both graph and total Feature-owned allocation sums. At 4K graph bytes are
1023672320/823132160 and owned bytes are 1157365760/956825600 for A/B.

Four full-head processes compare single reuse, single reuse with poison, sliced
reuse and sliced reuse with poison. All 64 graph evaluations and 48 route
comparisons pass with changing inputs, signed zero, negative values, subnormals,
and queued final-two-frame consumers. Sixteen packed F16 and sixteen expanded
F32 captures match the existing FP16 gold exactly.

Fresh ABI/lifecycle/capability and workspace-policy checks pass for both DLLs.
The reused original gold corpus gives 150 native-output and 58 image comparisons,
all exact. Two profile smokes have identical launch inventories and four exact
endpoints. The normal `scripts/test_ngx.ps1` command also passes its eight
default cases against A, with 25 physically rehashed A/B output pairs. It uses
the project's existing D3D12 probe, not the private image probe. No fresh
original-NVIDIA reference process is needed.

CNG source and integrity behavior are not changed by this candidate. The earlier
rejection/fallback evidence is retained, and final verification reruns 133 hash
case groups and the old 25-case single-reuse planner suite alongside the new
34-case layout test. The actual-DLL corrupt-model matrix is not rerun in this
experiment; there is no new rejection-coverage claim or checksum bypass.

### Marginal Results

The frozen campaign again uses four balanced rounds, three resolutions, two
roles, 600 frames and 200 warmups: 24 processes, 14400 raw timings and 9600 warm
samples. Profiling is off, preconditioning is 200 GPU-ms, and CPU capture
processing is deferred. All 48 first/final endpoints match frozen original
outputs. Every run and frame timing is retained without performance-driven
exclusions or repeats. Results are descriptive, not equivalence/confidence tests.

| Size | Single-Reuse GPU Median ms | Sliced GPU Median ms | Change | Additional LOCAL Saved MiB |
| --- | ---: | ---: | ---: | ---: |
| 1080p | 5.669456 | 5.718304 | +0.862% | 50.625 |
| 1440p | 9.052576 | 9.000480 | -0.575% | 86.250 |
| 4K | 19.143744 | 19.485264 | +1.784% | 191.250 |

These are medians of four process upper medians. Positive changes mean slower.
Paired changes are `[-1.094, +3.430, +0.618, -2.175]%` at 1080p,
`[-0.625, +0.573, -0.916, -1.340]%` at 1440p and
`[+1.689, +1.501, -0.977, +3.789]%` at 4K. B is faster in 2/4, 3/4 and 1/4
pairs, respectively. The 4K p95 changes are `[+1.056, +2.087, -0.307, +2.247]%`;
three slower median/mean/p95 pairs must not be dismissed as an automatic
consequence of noise. The cause has not been isolated.

All twelve LOCAL pairs show the exact predicted marginal reduction. A/B LOCAL
is 591.246094/540.621094 MiB at 1080p, 808.121094/721.871094 at 1440p and
1426.621094/1235.371094 at 4K. Every process has three identical steady samples.
NON_LOCAL is unchanged in all pairs: 111.015625, 172.390625 and 348.265625 MiB.
These are current-process DXGI measurements including probe/driver costs, not
exclusive NR memory or initialization peaks. They are direct increments over
the single-reuse combination, not sums of historical independent experiments.

A/B CPU-recording medians are 0.873500/0.862500, 0.882300/0.873500 and
0.872300/0.875850 ms. Init-plus-create medians are 2479.156/2542.236,
2371.027/2348.243 and 2498.974/2449.198 ms. CPU recording is higher in both arms
than in the preceding cohort; do not use old absolute timings as this control
or attribute the cross-cohort change to slicing. No general startup or CPU
speedup follows from these mixed measurements.

First-evaluation latency is a separate adverse observation at 4K. A/B first-GPU
medians are 19.943504/42.507568 ms. B's four first evaluations are
59.666848, 83.283520, 25.348288 and 19.208928 ms, versus A's 20.731552,
18.075616, 19.884224 and 20.002784 ms. They remain in the raw data; the predefined
warm metric does not include them. This is not a cold-OS startup test, and the
reason for these first-use spikes is unknown. Lower steady memory must not hide
this latency tradeoff.

### Disposition

Keep the sliced candidate **unpromoted and memory-oriented**. The additional
50.625/86.250/191.250 MiB reduction is repeatable and tested outputs remain exact,
but the 4K warm slowdown and first-use spikes preclude recommending it as the
default or as a general overhead reduction. Preserve the frozen single-reuse
combination as the latency comparison point. A targeted investigation of 4K
first-use and warm kernel costs is more informative than blindly repeating the
same full A/B or declaring the regression fixed by allocation count alone.

All production changes remain private. The 93 protected production-source
files, old candidates, formal DLL/ZIP and shared assets are unchanged, as are
driver/GPU settings. Product builds retain only the known nr_model.cpp C4458
warning. Review is self-review, not an independent review. Limits remain one
GPU/driver, static portrait variants plus synthetic controls, upsampled 1440p/4K
inputs, no real application, no full Vulkan suite rerun, no initialization-peak
measurement and no FPS claim. The continuous optimization goal remains active.

```powershell
./tmp/dx12-input-slices/20261007/verify_evidence.ps1 -Reason 'Exact outputs and repeatable incremental LOCAL savings; mixed timings with three slower 4K warm pairs and first-use spikes. Retain as a private memory option, not a default speedup.'
```

## Slice Latency Diagnostics: 2026-10-07

This diagnostic reuses the frozen single-reuse and sliced DLLs without changing
either implementation, assets, driver settings or the original adverse cohort.
Artifacts are under `tmp/dx12-slice-diagnostics/20261007`. Its purpose is to
separate first-use variability from a reproducible warm-chain regression, not
to manufacture a better A/B result or promote the sliced candidate.

### Timing Scope And Protocol

Inspection of the frozen timing probe confirms that input texture copies are
recorded before the first Evaluate timestamp, while output readback and query
resolution follow the ending timestamp. CPU Evaluate recording time is measured
separately. D3D12 timestamps are sampled after preceding GPU work completes;
the two query points are in the same command list. See Microsoft's
[timing contract](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing)
and [query comparability rules](https://learn.microsoft.com/en-us/windows/win32/direct3d12/queries).
This establishes the intended interval, not an isolated active-cycle count or
proof that scheduling, power state or first-use work is irrelevant.

Eight processes were declared before sampling: 4K only, two balanced A/B pairs
with profiling off and two with profiling on. Every process retains 600 frames,
200 warmups, 200 GPU-ms independent preconditioning and deferred endpoint
processing. The existing profiler forwards each launch chain intact and brackets
it with timestamps; it does not split kernels or add explicit barriers. The
timestamps themselves can perturb execution, so profiled observations are not
substituted for unprofiled production timings. No NR warmup is hidden before
the recorded first evaluation.

### Controller Handoff

The old PowerShell profile validator filters the entire launch CSV once per
frame. At 600 frames and 113 chains this repeatedly scans 67800 rows. The
controller had accumulated 1144.094 CPU seconds when it was stopped, after
confirming that no GPU child process remained active. Five GPU processes had
completed; four had been recorded and the fifth was waiting on offline
validation. This was a validation bottleneck, not a hung GPU or a reason to
rerun measurements.

The original runner, protocol, partial records, logs and captures are retained.
`controller-handoff.json` records the process identity and completed-log hashes;
`amendment.json` records the replacement validation method. A linear validator
first fails against a no-op stub, then passes one valid fixture and 22 rejected
mutations covering coverage, dimensions, chain identities, API kinds, timestamp
ordering, interval bounds, outer-timing equality and deferred completion.
Validation of each real profiled CSV takes approximately 0.26 seconds on the
resume path. That is a diagnostic-tool improvement, not an NR speedup.

The five completed outputs were validated without another GPU launch, and only
the three unexecuted protocol entries were run afterward. All eight original
process positions are present exactly once. For round 2 / profiling on /
baseline, the stopped controller had not persisted the start time, exit status
or before/after GPU telemetry. Those fields remain unknown. Its complete log,
timestamps and physical captures were recovered and validated; an observed zero
exit code is not invented. The long validation gaps and this metadata loss limit
the experiment and are not concealed by the resumed completion record.

### Observations

All 4800 raw timings, 2400 profiled frames and 16 original-matching endpoints
are retained. Each profiled frame contains the same 113 chains and 252 kernel
entries, giving 271200 checked chain intervals across four profiled processes.
The 191.250 MiB LOCAL reduction repeats in all four mode-matched pairs;
NON_LOCAL is equal within every pair and all three steady samples agree.
Profiling adds 0.125 MiB LOCAL and 0.0625 MiB NON_LOCAL to both arms in this
cohort, which is instrument overhead rather than a candidate allocation change.

| Profiling | Pair | A Warm GPU ms | B Warm GPU ms | B/A Change | A First GPU ms | B First GPU ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Off | 1 | 18.753088 | 18.546016 | -1.104% | 66.603328 | 20.607232 |
| Off | 2 | 19.324128 | 19.555936 | +1.200% | 19.113696 | 17.634944 |
| On | 1 | 19.378816 | 18.728576 | -3.355% | 20.436448 | 19.011616 |
| On | 2 | 19.172576 | 19.380608 | +1.085% | 24.857760 | 19.535776 |

Warm values are each process's upper median of its predefined 400 warm samples.
The new 66.603328 ms first-use spike is in the unchanged single-reuse baseline,
not the sliced candidate. Its next-nine-frame median is 18.267456 ms. Thus the
large first-use phenomenon is not exclusive to slicing. This does not erase
the previous candidate's 59.666848/83.283520 ms spikes or prove that they have
been fixed. Neither profiled sliced run reproduces a similarly large spike.

The moderately slow profiled baseline first evaluation, 24.857760 ms, contains
3.603456 ms in `nr_prepare`, 19.093504 ms across graph chains, 0.158720 ms in
`nr_compose`, and 2.002080 ms outside the measured chain intervals. These are
chain-level elapsed intervals, not uniquely attributed hardware causes. The
large unprofiled 66.603328 ms event has no chain breakdown.

Warm graph-chain mean deltas are -0.544241 ms in profiled pair 1 and +0.167342 ms
in pair 2. Three short C512 sites have positive deltas above 5 microseconds in
both profiled pairs: QKV slots 37 and 78, and GEMMV slot 77. Their deltas are
10.317/18.071, 14.566/7.828 and 7.209/7.542 microseconds respectively. These are
post-hoc leads among 113 chains, not proof of a dominant bottleneck or isolated
kernel regression. The largest positive chains otherwise change between pairs.

There is also non-NR first-submission cost that the current probe cannot split:
the spiking unprofiled baseline has 112.1628 ms submit/wait versus 66.603328 ms
inside Evaluate. The two earlier large candidate spikes likewise have roughly
44 ms of submit/wait outside the NR interval. This remainder includes work and
waiting beyond NR, not specifically upload time or a measured scheduling delay.
Independent preconditioning batch duration and host recording time vary across
the cohort; before/after GPU snapshots do not establish clocks during the spike.
For example, profiled round-2 CPU recording medians are 1.3903/0.4108 ms across
the controller handoff. Do not attribute that cross-process shift to slicing.

### Decision

Do not patch slice placement on the assumption that cache conflicts are already
proven. The memory saving remains repeatable, while the warm effect changes sign
and a large first-use spike now appears in the control. The original regression
data remain valid observations, and the candidate stays unpromoted. No claim
of performance equivalence, resolved root cause or repaired latency is made.

The next useful discriminator is a coarse first-submission timeline separating
input copies, Evaluate, output copies and total submit/wait, checked against the
original probe. This is less invasive than rewriting the allocator or hiding NR
work in initialization. Any such new probe requires its own scoped design and
validation; no additional product change is included here.

All frozen runtime/source/asset guards and endpoint checks remain in force.
Formal DLL/ZIP, 93 protected production-source files and GPU settings are
unchanged. This is self-reviewed, single-machine diagnostic evidence with two
process pairs per mode, not real-application or FPS validation. The broader
optimization objective remains active.

```powershell
./tmp/dx12-slice-diagnostics/20261007/verify.ps1
```

## First-Submission Phase Diagnostic (2026-10-07)

### Scope And Frozen Inputs

User-approved throwaway diagnostic in `tmp/dx12-submit-phases/20261007`.
Only a private copy of the image timing probe was instrumented. No runtime DLL,
model, PTX, shader, production source or formal package was rebuilt or edited.
The two runtime inputs remain:

- A, CNG + FP16 + single-region input reuse:
  `0565E972E07FEB0853B0A11BFFB8B99EDDEF40DBF5E0F702DA2E4738C66DB82B`.
- B, CNG + FP16 + disjoint input slices:
  `B03AB0FCF6135CC5403526C2ACB54803997D71AA7C0AB4B8722825C3223ADD16`.
- Frozen original timing executable:
  `FF5D3A8D32EE0F743334BFAEC76DCAEAB1F0350C66FAC1C54F1A57A31B5D5460`.
- New private phase executable:
  `FEC732BECA2ECA2FBB90315948EB2C0F4E99BB948FFD46A93CDA989340C4DB91`.

The original report prefix is 208,363 bytes with SHA-256
`04F0F8DFE29FCC75210D68455CB2648F00920F9B373642C01B93141062FBC4CA`.
It is checked byte-for-byte as an unchanged prefix. Earlier adverse cohorts,
their conclusions, and the previous diagnostic's controller-handoff disclosure
are retained; none is replaced by this new cohort.

### Measurement Boundaries

The original NR timestamp indices 0/1 and their `ResolveQueryData` call stay in
the same places. Three markers are added: index 2 before the input/upload
region, index 3 after the original query resolve, and index 4 after the optional
output copy. A final resolve copies only indices 2..4 to bytes 16..39 of the
timestamp readback. The independent preconditioner still uses indices 0/1,
exactly its original workload, and no NR evaluation before recorded frame 0.

All five timestamps are in the same direct command list and use that queue's
timestamp frequency. Ordered boundaries are 2 <= 0 < 1 <= 3 <= 4. Raw UINT64
ticks are retained as JSON integers, subtracted before floating-point conversion,
and checked against reported millisecond values. D3D12 timestamps are
bottom-of-pipe elapsed observations, not isolated hardware active cycles.
See Microsoft's [timing documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing)
and [query documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/queries).
No stable-power, driver, clock, counter-permission or TDR setting was changed.

- Upload region, 2 to 0: three 4K texture copies, including output sentinel and
  zero-motion initialization, plus their existing transitions. This is about
  199 MB per frame in this headless image harness, not solely the color image.
- NR region, 0 to 1: the unchanged Evaluate interval.
- Query-resolve region, 1 to 3: kept separate from image readback.
- Readback region, 3 to 4: output copy and transitions at endpoints only. On
  non-capture frames it is an empty marker interval, not a readback workload.
- GPU span, 2 to 4: includes these regions but excludes the final auxiliary
  query resolve. CPU submit/wait includes Close, Execute, Signal, fence wait,
  device-status check and allocator/list reset. Its difference from GPU span
  is a residual, not uniquely queue delay, driver work or context switching.

Only phase records, frame timing records and capture processing are deferred.
The original per-Evaluate success print and flush remain; this is not a claim
that all host logging was removed from the frame loop. No extra synchronization,
NR warmup or chain profiler was added, and the measured CPU Evaluate interval
does not include the new post-submit phase bookkeeping.

### Protocol And Validation

`protocol.json` was written before any GPU sampling and pins both executables,
runtime inputs, private source and supporting checks. Two separate 3-frame
smoke processes precede exactly eight 4K measurement processes. Each measured
process runs 600 NR frames, retains all raw timings and endpoints, and uses
frames 200..599 for its 400-sample warm summary. The independent precondition
target is 200 GPU milliseconds, reset remains at frame 0, and captures remain
at 0/599. The fixed measured order is:

1. Round 1: original A, phases A, phases B, original B.
2. Round 2: original B, phases B, phases A, original A.

No sample was excluded, repeated or replaced because of its timing. Every
process has observed exit status, start/end time, before/after read-only GPU
telemetry and a pinned raw log. Unlike the previous diagnostic, no controller
handoff or recovered process metadata was needed. Driver remains 617.14.

The phase gate first failed against the frozen original log with zero phase
records. Its unit tests first failed against a no-op validator, then passed one
hand-derived valid fixture and rejected 34 invalid fixtures. They cover missing
or reordered records, incorrect capture flags, non-integer or out-of-range ticks,
integer precision above 2^53, frequency changes, reversed boundaries, arithmetic
and NR-interval disagreement, nonfinite values, and non-deferred output. The
private probe builds with the original MSVC flags without compiler warnings.

Observed validation totals:

- 4,800 measured raw frames, 3,200 warm frames and 2,400 measured phase records.
- Six additional smoke frames, with six validated phase records.
- 20 physical endpoint captures bit-exact against the previously frozen
  proprietary-reference gold, including all 16 measured endpoints.
- The ordinary `scripts/test_ngx.ps1` default eight-case suite also ran after
  the measured cohort using unchanged A/B DLLs. All 25 physical frame pairs
  match, covering default, odd, reset, disabled, intensity, controls, motion and
  fractional motion. This uses the existing project probe, not the phase probe.
- No new proprietary-reference process, new runtime candidate or hidden NR
  evaluation. Total GPU process count is 2 smoke + 8 measured + 16 suite.

### Observations

All times below are milliseconds. Warm medians use the existing upper-middle
convention; P95 and full per-frame data remain in `summary.json`,
`process-summary.csv` and `frame-phases.csv`.

| Round | Probe | Runtime | First NR | First submit/wait | Warm NR median |
|---|---|---|---:|---:|---:|
| 1 | Original | A | 16.785952 | 32.5449 | 18.742272 |
| 1 | Phases | A | 25.892416 | 43.1168 | 18.915328 |
| 1 | Phases | B | 17.574848 | 34.1018 | 19.241600 |
| 1 | Original | B | 31.220416 | 48.4322 | 18.796320 |
| 2 | Original | B | 20.316672 | 36.7529 | 18.772864 |
| 2 | Phases | B | 25.327200 | 41.2445 | 18.939712 |
| 2 | Phases | A | 26.146368 | 39.5035 | 18.875712 |
| 2 | Original | A | 20.278336 | 58.5584 | 18.851456 |

The four instrumented first-frame breakdowns are:

| Round/runtime | Upload | NR | Query resolve | Readback | GPU span | CPU submit minus span |
|---|---:|---:|---:|---:|---:|---:|
| 1/A | 9.357760 | 25.892416 | 0.006144 | 5.700416 | 40.956736 | 2.160064 |
| 1/B | 8.830016 | 17.574848 | 0.004096 | 5.555776 | 31.964736 | 2.137064 |
| 2/B | 8.846752 | 25.327200 | 0.004096 | 5.607936 | 39.785984 | 1.458516 |
| 2/A | 8.978880 | 26.146368 | 0.006144 | 2.662656 | 37.794048 | 1.709452 |

In these four observations, upload and image readback together account for
11.641536..15.058176 ms outside NR. Warm upload medians are
8.424352..8.971744 ms. Last-endpoint readback is 2.518656..5.811136 ms, and
last-endpoint submit-minus-span residual is 0.246888..0.489964 ms. All warm
frames remain in the summary, including the last endpoint's copy; a zero warm
readback median simply reflects the other 399 frames without a copy. First
and later NR frames have different history/reset semantics, so their timing
difference alone is not a measured initialization cost.

The predeclared descriptive flag, first NR > 1.5 times median NR at frames 1..9,
fires once: original B round 1 is 31.220416 ms versus 18.021312 ms. No segmented
process reproduces the earlier 59/66/83 ms NR events. More importantly, original
A round 2 has 58.5584 ms submit/wait with only 20.278336 ms inside NR, leaving
38.280064 ms outside that interval. That process has no phase markers. The
11.6..15.1 ms copy-region observations from other processes cannot be used to
attribute its extra waiting. Nor do outside-NR copies explain the historical
spikes already measured inside the NR interval.

### Instrumentation Tradeoff And Decision

Relative warm NR median changes for phases versus original are +0.923346% and
+0.128669% for A, and +2.368974% and +0.888772% for B. Corresponding submit/wait
median changes are +0.839168%/-1.348758% for A and +2.209862%/+1.496397% for B.
All four NR differences are positive, but two process pairs per runtime do not
separate instrumentation cost from execution-state variability. Treat this as
a diagnostic cohort with observed perturbation, not zero-cost profiling or a
correction factor for the original timings. Host recording medians and independent
precondition batch times also vary; before/after telemetry does not measure
clocks or competing GPU work during an event.

The B-minus-A warm NR effect is +0.288375%/-0.416901% with the original probe,
and +1.724908%/+0.339060% with phase markers. These small, mode-dependent values
do not establish equivalence or supersede the original adverse cohort.
Steady LOCAL remains 1,426.621094 MiB for A and 1,235.371094 MiB for B: exactly
191.25 MiB saved in every mode-matched pair. NON_LOCAL is 348.265625 MiB for both.
Three steady samples per process agree; the new probe adds no observed DXGI
LOCAL/NON_LOCAL allocation at this granularity, although it does add CPU-side
records and timestamp commands.

The useful result is a separation of ordinary image-harness transfer time from
NR time, not a repaired runtime regression. Removing those test copies would
change the harness workload and cannot be advertised as an NR kernel speedup
or real-application improvement. Keep B unpromoted and retain the original
probe as the latency control. The remaining first-use cause is unresolved.

A lower-intrusion next discriminator would split CPU time inside the original
submit helper (Close, Execute/Signal, fence wait, status/reset) while retaining
its original GPU command stream. That is a separate proposed diagnostic, not
implemented here; it should have a bounded sample count rather than repeatedly
adding GPU markers or rerunning until a favorable result appears.

This work was self-reviewed with no independent reviewer. The final guard
checks preserve 93 protected production-source files, formal DLL/ZIP, all frozen
runtime/assets and the previous 22 diagnostic artifacts and eight raw logs.
The broader optimization objective remains active; no candidate is promoted.

```powershell
./tmp/dx12-submit-phases/20261007/verify.ps1
```

## Cumulative Review And Release Evidence: 2026-10-08

The cumulative `dev` source was reviewed and tested without subagents. A clean
build in `tmp/review-push/20261008/source` covered the Vulkan shaders/PTX,
runtime, regression executables, and D3D12 NGX targets. The compiler reported
the existing `nr_model.cpp` local/member `byteLength` shadowing warning.

The expanded graph run initially stopped because five fallback gold cases
were missing, not because output differed. All 16 gold cases were then
captured from the unmodified production sources at
`9d08f4184bbcb9d858e2fb7a7834ec0837a9d2f1`, with only the current test harness
used to drive them. The 11 existing gold heads matched the fresh original-code
captures exactly. Optimized output was not used as its own reference.

Review found two release-evidence defects: an aborted comparison could leave
an empty/partial report that packaging accepted, and benchmark reports did
not bind the tested shader inventory. Both were reproduced before fixing
the scripts. Schema-version-1 reports now record requested cases and a
successful completion flag set only after all comparisons and final DLL/model/
shader integrity checks. Packaging independently checks complete case/round
coverage, probe results, sample counts, and the full resource fingerprint.
Old reports must be regenerated, not retroactively marked complete.

Verification of the fixes is under `tmp/review-push/20261008-fix`:

- The focused regression suite failed 22 of 29 cases against the old scripts
  and passed all 29 against the fixes, including interrupted report writers.
- All 32 full-regression invocations passed, including 64 graph configurations
  across F32, F16 input/head, workspace reuse, fallback routes, and full sizes.
- Workspace boundaries/poisoning, queued frames, four Vulkan output routes,
  staging policies, auxiliary storage, D3D12 ordering, normalization, packed
  prepare stores, allocation policies, and ABI/lifecycle checks passed.
- All 18 original-versus-replacement NGX cases passed exact comparison across
  75 frames. The three-round benchmark at 512, 1080p, 1440p, and 4K produced
  12 exact 120-frame endpoints with 100 timed samples per run.
- Temporary packages accepted complete validation plus benchmark evidence,
  and complete benchmark-only evidence. Actual aborted validation/benchmark
  reports and a post-benchmark PTX change were rejected before package creation.

Tests ran on an RTX 4090 with driver 617.14. The tested replacement DLL SHA-256
was `482f6e7692e6ee6994352f33eb9f96ef4638fc63c33512df0df5218ab31534a1`;
the original DLL was
`984bee0f775c277d5829b8fd6775d53a7b0f75396c852b3aaf06a18375f81014`.
The fix changes reporting and packaging only; native sources and compiled
runtime inputs matched the clean-build review snapshot. Temporary packages
are test artifacts, not a new formal release. No private optimization
candidate was promoted. These checks do not establish real-game compatibility,
cross-driver coverage, or a new performance improvement.
