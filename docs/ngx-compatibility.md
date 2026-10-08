# Experimental NGX D3D12 Replacement

This build implements the NR snippet interface in `nvngx_dlssnr.dll`, backed by
this repository's optimized network. It does not forward evaluation to the
reference DLL. This is an experimental, locally tested replacement, not a claim
of universal compatibility with games or NGX loader versions.

The library exports the reference's 55 names and ordinals. Implemented D3D12
entry points include `Init_Ext`, `CreateFeature`, `EvaluateFeature`,
`ReleaseFeature`, `Shutdown`, `Shutdown1`, scratch-size and feature-requirement
queries, and the statistics/scaling-ratio parameter callbacks. Unsupported
interfaces return NGX failure instead of pretending to render successfully.

## Supported Profile

- Windows x64, NVIDIA Ada/RTX 40-class PTX target; tested on RTX 4090 with driver
  610.88. Other GPUs and driver versions have not been certified.
- NR feature ID 18, default render preset, scaling ratio 1, style 0.
- RGBA16F or RGBA32F color input; RGBA16F UAV output; RG16F or RG32F motion.
  The comparison matrix currently exercises RGBA16F color and RG32F motion;
  the other accepted formats and nonzero subrectangle origins are not certified.
- Input is the display-space NR proxy, not a replacement for a game's HDR tone
  mapping. This is **not** the DLSS Super Resolution DLL `nvngx_dlss.dll`.
- Intensity, local tone/structure/skin controls, automatic masking, reset,
  enable/disable, and same-sized valid rectangles. Motion scale follows the
  reference's pixel-displacement convention.
- D3D12 direct or compute command lists on the initialized device. Input is in
  `NON_PIXEL_SHADER_RESOURCE`, output in `UNORDERED_ACCESS`; these states are
  preserved after evaluation. Inputs and output must not alias.

Depth, UI/UIAlpha, control-mask textures, backbuffer composition, distortion,
nonzero styles, UI correction, custom resource allocation callbacks, scaling,
and multi-node devices are not implemented. CUDA, D3D11, Vulkan NGX entry
points and the vendor runtime/telemetry callback registrations remain
unsupported. No particular game's full loader sequence has been validated.

The supplied community reference also rejects D3D11 initialization: after its
caller-module check, `NVSDK_NGX_D3D11_Init_Ext` returns
`NVSDK_NGX_Result_FAIL_FeatureNotSupported`. Export presence alone is therefore
not evidence of a functioning backend. D3D11 work was deferred by user choice.

## Lifetime and Execution

Per-frame preprocessing, network evaluation, and composition are recorded on
the caller's D3D12 command list through the installed NVIDIA driver. There is
no per-frame CPU image readback and no separately submitted inference queue.
Caller writes before `EvaluateFeature` and reads after it stay in queue order.

Feature creation currently uses a temporary Vulkan context to compile the
existing graph into a relocatable execution plan. Vulkan buffers and objects
are then destroyed. Consequently an NVIDIA Vulkan runtime with the extensions
required by the original project is still needed, and startup is more expensive
than a purpose-built CPU-only graph compiler. Shader/model uploads use a private
D3D12 initialization queue and complete before `CreateFeature` returns.
This also creates temporary GPU allocations during initialization; the steady
memory samples below are not a bound on startup peak memory.

The D3D12 graph now enables the existing whole-buffer workspace reuse by
default. Logical activations share stable physical buffers only across
nonoverlapping lifetimes; recorded stage dependencies are retained as D3D12 UAV
barriers. Input features, model data and the two history textures remain separate,
and the neural head remains F32. This does not change the native arithmetic profile.
`OPEN_DLSS_NR_WORKSPACE=0` selects the previous dedicated-buffer route for
diagnosis; `1` enables reuse explicitly. The setting is read at feature creation,
not applied to a live feature. Unset it to use the default; empty or other values
return `InvalidParameter` from feature creation.

The host owns submission and completion. Submit evaluations of a feature in
order, synchronize any cross-queue use, and finish all GPU use before release or
shutdown. Abandoned recordings need a reset before further evaluation. Call
`Shutdown1`/`Shutdown` before unloading the DLL. GPU destruction is deliberately
not run under the Windows DLL loader lock.

The initial descriptor cache retains resources until feature release. It has
128 entries, including history views. Normal fixed/rotating render targets are
supported; unbounded resource churn eventually returns an explicit error and
requires recreating the feature. Resize also requires a new feature.

## Build

Use the project's Vulkan dependencies, extracted model, compiled shaders, MSVC
x64 tools, and an installed CUDA toolkit (`nvcc` generates PTX; no CUDA runtime
DLL is linked into the replacement):

```powershell
./scripts/fetch_ngx.ps1
./scripts/build_shaders.ps1
./scripts/build_ngx.ps1
./scripts/build_ngx.ps1 -DllOnly
```

ABI headers and NVAPI declarations are fetched from pinned upstream commits.
The build does not download or modify the reference DLL. Model hashes are
checked when loaded, including lowercase hexadecimal hashes in the manifest.

## Comparison

The test host owns real D3D12 resources, uploads input before evaluation, and
reads output after evaluation on the same command list. The reference and
replacement run in separate processes. Comparison is exact over every RGBA16F
bit, including alpha and signed zero; a mismatch returns nonzero and preserves
both captures. Earlier failed experiments are not overwritten.

```powershell
./scripts/test_ngx.ps1 -OutputDirectory tmp/ngx/my-comparison
./scripts/test_ngx.ps1 -Cases toggle,alpha,disabled-alpha,rotation,1080p,1440p,4k `
  -OutputDirectory tmp/ngx/my-extended-comparison
./build/ngx/nvngx.dll-d3d12-probe.exe `
  --dll D:/UGit/OpenDLSS-NR/build/ngx/nvngx_dlssnr.dll --abi-checks --capabilities
./scripts/build_ngx.ps1 -NvapiOnly
./build/ngx/ngx_normalization_test.exe
```

The reference is the user-supplied, community-modified 310.8.0.0 binary, SHA-256
`984bee0f775c277d5829b8fd6775d53a7b0f75396c852b3aaf06a18375f81014`.
It remains unchanged. Generated native captures and driver diagnostics stay
under ignored `tmp/ngx/`; no native kernels or reference binaries are packaged.

On 2026-10-04 the user clarified that this file is the original runtime with
only RTX 40-series compatibility adaptation, distinct from the later
`rtx4090+` optimized variant. That patch scope is user-supplied provenance,
not an independent binary audit. Its Authenticode status is `HashMismatch`,
which does not identify the changes or establish a precision change. The
617.14 comparison against this user-designated original is recorded in
`docs/optimization-validation.md` under "RTX 40-Adapted Original Baseline".

The probe also accepts `--benchmark --frames 120 --warmup 20`. GPU timestamps
bracket `EvaluateFeature`, not uploads or output readback. Benchmark mode reads
images only on the first and last frame. This is a serialized GPU evaluation
measurement, not game FPS or end-to-end CPU/GPU frame time. Reported NGX owned
allocation bytes and DXGI process-local usage are different measurements and
must not be conflated.

### Measured Result: 2026-10-03

On the RTX 4090 / driver 610.88 used here:

- All 18 matrix cases passed exact comparison, covering 75 captured frames per
  implementation. Cases include odd dimensions, reset, enable toggling, alpha,
  intensity, controls, constant/fractional/varying motion, 20 resource rotations,
  and 1080p/1440p/4K.
- Three independent 120-frame runs at each of four sizes also had identical
  final-frame SHA-256 values. Only the endpoints, not every intermediate frame
  in these long benchmark runs, were captured and compared.
- Timing excludes 20 warmup frames per run. The table reports the median of
  the three per-run medians (100 GPU samples each), with alternating process
  order. CPU command recording is reported separately in `benchmark.json`.
- Supplementary checks passed: ABI/lifecycle failures and multiple instances,
  ordered dependent D3D12 kernels, the focused FP16 normalization regression,
  D3D12 graph replay at 512x512 and 513x377, all 11 historical Vulkan cases with
  both F32 and F16 inputs, four Vulkan frame-output routes, and auxiliary/
  workspace accounting and trace tests.

| Valid Size | Reference GPU ms | Replacement GPU ms | GPU Time Increase | Reference / Replacement Local MiB |
| --- | ---: | ---: | ---: | ---: |
| 512x512 | 2.285 | 2.453 | 7.3% | 356.7 / 376.6 |
| 1920x1080 | 3.993 | 6.432 | 61.1% | 590.1 / 852.8 |
| 2560x1440 | 5.569 | 9.759 | 75.2% | 764.9 / 1248.8 |
| 3840x2160 | 11.578 | 20.382 | 76.0% | 1302.9 / 2404.9 |

Local MiB is DXGI process-local usage sampled after evaluation, including the
same test harness resources and driver overhead in each process. It is not
peak VRAM or the NGX-owned allocation statistic. These synthetic, serialized
measurements do **not** establish a game FPS improvement. This version is
slower and uses more resident memory than the supplied reference at the tested
sizes; it is a compatibility baseline, not a performance-upgrade release.

The 4K multi-frame test exposed an FP16 double-rounding boundary in ViT query
normalization. Native uses a fused half multiply-add; an intermediate FP32 add
lost a tiny positive square and changed one FP8 query value, which later spread
through attention. The native D3D12 arithmetic profile now uses the fused
operation and has a focused GPU regression test. The legacy Vulkan replay
profile and its generated arithmetic remain unchanged.

```powershell
./scripts/benchmark_ngx.ps1 -OutputDirectory tmp/ngx/my-benchmark
```

Full measurements are in `tmp/ngx/release-comparison/results.json` and
`tmp/ngx/release-benchmark/results.json`. The packaged DLL SHA-256 is
`ee30d3421ea4c5cc07bfa780c40e3af7e133ab9a926ac7457fbd5b14633f2e29`.

## Package

```powershell
./scripts/package_ngx.ps1 `
  -ValidationResults tmp/ngx/release-comparison/results.json `
  -BenchmarkResults tmp/ngx/release-benchmark/results.json
```

The package contains the DLL, a D3D12 probe, licenses, hashes, and the required
`opendlss-nr` data directory with model and generated shader assets. The DLL and
that directory must remain adjacent. A lone DLL is not a complete installation.
The optional absolute `OPEN_DLSS_NR_ROOT` environment variable overrides asset
lookup. Runtime operation does not depend on the current working directory or
on having the reference DLL available.

The packaging script verifies the model and refuses to overwrite an existing
package. It does not replace any game file or the supplied baseline.
When result files are supplied, packaging also verifies that the tested DLL
and shader assets have not changed and includes the result summaries.
Both report types use schema version 1 and record the requested cases, a
successful completion flag, and hashes of the model manifest, model stages,
and shader inventory. The flag is set only after every requested comparison
and final DLL/resource integrity check succeeds. An interrupted run keeps a
diagnostic report with `completed: false`, not release evidence.

Packaging rejects incomplete, duplicate, failed, or unexpected case/round
records, missing resource fingerprints, and changed or added resources.
Benchmark-only packaging enforces the same resource checks. Reports generated
before this schema must be regenerated; adding a completion flag to an old
report is not a substitute for rerunning it. Omitting both report arguments
still creates a package without test evidence, not a validated release.

The CPU-only packaging regression suite uses disposable file fixtures:

```powershell
./tests/ngx_evidence_tests.ps1
```

## Workspace Measurement: 2026-10-04

A same-DLL, three-round comparison reduced whole-process DXGI LOCAL usage from
852.809 to 653.996 MiB at 1080p, 1248.809 to 912.121 MiB at 1440p, and 2404.934
to 1659.996 MiB at 4K. NON_LOCAL usage was unchanged. The 4K saving is 744.938 MiB
(31.0%); GPU evaluation time was essentially unchanged in that A/B test.

The reused route still uses 356.559 MiB more LOCAL than the RTX 4090+ community
DLL at 4K and remains slower. See [the validation record](optimization-validation.md#d3d12-workspace-reuse-2026-10-04)
for the distinct same-DLL and community comparisons, output tests and limits.
The earlier release table above remains a historical dedicated-buffer result.

```powershell
./scripts/build_ngx.ps1 -WorkspaceOnly
./build/ngx/d3d12_workspace_test.exe 513 377
./build/ngx/d3d12_workspace_test.exe 3840 2160
./scripts/test_ngx_workspace.ps1 -OutputDirectory tmp/ngx/new-workspace-check
$env:OPEN_DLSS_NR_WORKSPACE = '0' # Legacy allocation route for a new feature.
Remove-Item Env:OPEN_DLSS_NR_WORKSPACE # Default reuse for a new feature.
```
