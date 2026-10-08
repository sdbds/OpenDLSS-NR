# NGX Compatibility Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan inline. The user has prohibited subagents and explicitly requested implementation.

**Goal:** Produce a real, testable NGX replacement with the pinned reference contract and D3D12 entry path. The user chose a D3D12-only first package after the reference rejected D3D11 initialization.

**Architecture:** Keep NGX ABI adaptation separate from the optimized network. The native DLL is a read-only oracle. Do not advertise D3D11/D3D12 until resource and command-order tests pass.

**Tech Stack:** Windows x64, C++20, MSVC, CUDA Driver API, Vulkan, pinned NGX headers.

**Spec:** `docs/superpowers/specs/2026-10-03-ngx-compatibility-design.md`

## Global Constraints

- Preserve `DLSS5VK_MODEL/nvngx_dlssnr.dll`; signature status is not a blocker.
- Do not commit vendor binaries, weights, or fixtures.
- Work in the existing user-selected `dev` checkout; no subagents or automatic push.
- Product artifacts: `build/ngx/`. Diagnostic artifacts: `tmp/ngx/`.
- Unsupported features return a failure, not success with unchanged pixels.

## Review Focus

- Same-named SDK and snippet exports can have different ABI signatures.
- Parameter values can be requested with different numeric types.
- Feature handles must never cross implementation ownership boundaries.
- CUDA context/stream ownership must survive success, failure, and release.
- D3D12 recording must not execute against data the caller has not produced yet.

### Task 1: Native contract and comparison harness

**Files:** `scripts/fetch_ngx.ps1`, `scripts/build_ngx.ps1`, `tests/ngx_parameters.h`, `tests/ngx_probe.cpp`, `tests/ngx_parameter_tests.cpp`.

**Interfaces:** Pinned public NGX parameter ABI; dynamically loaded snippet exports. The probe takes an absolute DLL path, backend, dimensions, controls, and output directory. It prints stage-specific results and returns nonzero when evaluation or output assertions fail.

- [x] Write parameter round-trip tests, including missing keys and numeric conversion, and observe the absent test helper fail.
- [x] Implement the test parameter map against the published interface, not a guessed vtable.
- [x] Fetch pinned headers and build the harness with installed MSVC/CUDA.
- [x] Test real reference initialization, creation, evaluation, output, and cleanup.
- [x] Record observed parameter and resource contracts before writing the optimized adapter.

### Task 2: Compatible exports and optimized D3D12 frame

**Files:** `ngx/` for exports, parameter parsing, feature lifetime, and backend adapter; `tests/ngx_probe.cpp` for native-versus-replacement comparison; `scripts/build_ngx.ps1` for the DLL.

**Interfaces:** Match the signatures proved in Task 1. Feature state owns the network and history, while the caller retains its command list and resources. Required unsupported options fail explicitly. Preserve caller command-list ordering.

- [x] Run the probe against the missing replacement and observe the expected load failure.
- [x] Implement a minimal lifecycle and verified frame path; record backend identity in diagnostics.
- [x] Compare against the native oracle and local graph baseline using the same inputs.
- [x] Test missing parameters, invalid handles, multiple instances, resets, and caller resource states.
- [x] Benchmark only configurations whose correctness results are reported.

### Task 3: D3D11 and subsequent CUDA adapters

Deferred by explicit user choice. D3D11's reference Init returned unsupported;
the current package returns explicit failures for these unimplemented backends.

**Files:** `ngx/d3d12_backend.*`, `ngx/d3d11_backend.*`, corresponding probe cases.

**Interfaces:** Preserve published snippet command-list/context contracts and the frame behavior proved in Tasks 1-2.

- [ ] Build failing execution-order tests with writes before and reads after Evaluate.
- [ ] Implement resource adapters without assuming arbitrary caller textures are shareable.
- [ ] Test changing resources, dimensions, history, invalid formats, and repeated create/release.
- [ ] If the required driver execution mechanism cannot be established, keep that backend explicitly unsupported and document the measured blocker rather than providing unsafe synchronization.

### Task 4: Package and review

**Files:** `scripts/package_ngx.ps1`, `docs/ngx-compatibility.md`, runtime configuration example if needed.

- [x] Package independently of the working directory; verify an extracted local package can run.
- [x] Run existing regressions for every changed shared path and the new ABI/frame tests.
- [x] Review the complete diff independently without subagents.
- [x] Report artifacts, backend support, actual comparisons, performance, and remaining gaps.

## D3D12 Delivery Result

The 0.1 package contains 122 integrity-checked files and passed an extracted
513x377 test from the system temporary directory with the asset-root override
unset. All 18 short comparison cases (75 frames) passed exact RGBA16F comparison;
all twelve 120-frame benchmark endpoints matched. Existing Vulkan regressions
also passed. The package is a compatibility baseline, not a speedup: median
4K GPU evaluation was 20.382 ms versus 11.578 ms for the supplied reference.
See `docs/ngx-compatibility.md` and the packaged validation/benchmark summaries.
