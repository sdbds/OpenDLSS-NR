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
  GLSL block-32, 1080p, 1440p and 4K. F16 heads and captured boundaries compare
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

The repository's model hash function prints uppercase digests, while the local
manifest uses lowercase. `test_graph.ps1` independently checks every stage's
length and SHA-256 with PowerShell before invoking the runner with `--no-verify`.
This avoids changing the unrelated production hash checker or the model files.

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
