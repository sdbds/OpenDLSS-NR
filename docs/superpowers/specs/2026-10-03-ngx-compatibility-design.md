# NGX compatible neural rendering DLL

## Goal and approval

The user wants an x64 Windows `nvngx_dlssnr.dll` that existing NR callers can load
without changing their NGX calls. Its inference must use this repository's
optimized implementation, not silently delegate all work to the reference DLL.
CUDA, D3D12, and D3D11 are the requested entry paths. The existing Vulkan tool
and demo must continue to work. The user approved implementation on 2026-10-03,
with signature status explicitly not a blocker for this local DIY build.
After the supplied reference rejected D3D11 initialization, the user explicitly
chose a D3D12-only first package. D3D11 and CUDA adapters are deferred, not part
of this package's completion claim.

## Reference

Use the user-provided, community-modified RTX 40 compatible 310.8.0.0 DLL at
`DLSS5VK_MODEL/nvngx_dlssnr.dll` as the behavioral reference. Its SHA-256 is
`984bee0f775c277d5829b8fd6775d53a7b0f75396c852b3aaf06a18375f81014`.
Keep that file unchanged. Its output is a community-build baseline, not evidence
about an unmodified NVIDIA release. Do not commit or download vendor binaries,
model weights, or captured image fixtures.

## Interface boundary

Match the feature DLL (NGX snippet) ABI, not the same-named public SDK loader
functions. Use the published parameter interface and verify the real binary's
parameter reads, types, return values, and lifecycle with an isolated harness.
An export name alone does not establish a usable backend. In particular, the
legacy Init exports in this build return an unsupported result; extended Init
must be tested before choosing the integration route.

Own every optimized feature from Create through Release. Do not pass a native
handle to the optimized implementation or vice versa. Unsupported parameter or
resource combinations must return a clear failure without corrupting output.
Do not report an API as optimized when it is only forwarded or stubbed.

## Architecture

Separate the NGX exports/parameter decoding, feature lifetime, frame preparation,
network execution, and graphics resource adapters. Reuse the current PTX and
graph arithmetic. Establish the real CUDA resource contract before committing
to a CUDA-only port or a GPU-only interop adapter to the existing Vulkan engine.
Choose between them based on compatibility, synchronization cost, and measured
output, not the name of the host API.

The user subsequently prioritized game-facing D3D12 and D3D11. D3D12 runs first;
CUDA remains an internal kernel and diagnostic tool. D3D12 must preserve caller
command-list execution order; an immediate
CUDA launch at record time is not an implementation of that contract. D3D11 must
preserve immediate-context ordering and resource ownership. GPU-only adapters
may need internal shareable resources when caller textures cannot be shared.
CPU readbacks are permitted in the comparison harness, not as an undisclosed
production fast path.

## Milestones

1. Pin the ABI and obtain actual native frame outputs in a repeatable harness.
2. Build the same-named replacement and prove an optimized D3D12 entry path with
   the same inputs, settings, history resets, and output format.
3. Package D3D12 locally, with truthful support limits and comparison results.
4. Deferred: D3D11 and independent CUDA adapters with their own execution tests.

Each milestone is independently testable. Failure of a reference backend must
be reported, not relabeled as parity. A loadable DLL or successful Create call
alone is not a completed NR implementation.

## Verification

- Verify the pinned reference hash before and after tests, without requiring a
  valid signature. Load libraries by absolute path.
- Compare nontrivial output pixels, not only return codes. Include a disabled
  control and an enabled control that changes the image.
- Check constant and varying images, odd sizes, control changes, repeat frames,
  reset, independent feature instances, invalid inputs, and orderly release.
- Separate network parity from preprocessing, history, and composition parity.
  Record exact mismatches; do not invent an error tolerance to turn a failure
  into a pass.
- Benchmark only after correctness, with warmup and no frame readback in the
  timed interval. Report host submission cost, GPU work, and memory separately.
- Run existing graph regressions when shared code changes.

## Build and deployment

Use the installed MSVC and CUDA toolkit plus the existing portable Vulkan tools.
Fetch pinned SDK headers into ignored `tools/`; preserve their upstream license
notices. Put binaries under ignored `build/ngx/` and observations under ignored
`tmp/ngx/`. Do not install into any game or overwrite the reference automatically.
Do not push changes without a new request.

## Sources

- NVIDIA NGX headers, revision `374959484e79a640feaba44c93ac8cfb0a03f5b5`:
  <https://github.com/NVIDIA/DLSS/tree/374959484e79a640feaba44c93ac8cfb0a03f5b5/include>
- CUDA graphics interoperability:
  <https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/graphics-interop.html>
- Local PE export table and disassembly of the pinned reference DLL.
