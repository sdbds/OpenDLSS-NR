# NGX Prepare Packed Stores Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans inline. The user requested no subagents. Preserve the current shared worktree and unrelated changes; do not commit or move the work.

**Goal:** Reduce prepare output store instructions without changing output bits.

**Architecture:** Keep all existing float calculations and round-to-nearest half conversions in `nr_prepare`. Pack the resulting half bits into eight unsigned words and issue two aligned `uint4` stores. Keep dispatch geometry, the DLL, and all other kernels unchanged.

**Tech Stack:** CUDA 13, PTX sm_89, D3D12/NVAPI, MSVC, PowerShell.

**Spec:** User-approved scope on 2026-10-04: modify only the output packing in `ngx/nr_frame.cu`, retain arithmetic, rounding, and 8x8 layout; regenerate PTX; require bit-exact regression and repeated A/B gains; retain the previous package for rollback.

## Global Constraints

- Production change is confined to `ngx/nr_frame.cu`; new tests and validation records are in scope.
- Never regenerate or overwrite the frozen baseline package.
- Use the existing compiler flags `--ptx -arch=sm_89 --std=c++17 --fmad=false`.
- Compare matched runs on the same GPU with profiling off for the acceptance timing.
- Offline SASS is code-generation evidence, not proof of the runtime driver's exact SASS.

## Review Focus

- Every half bit pattern, including signed zero, infinities, NaNs, and subnormals: test all 65536 patterns in each of 16 positions.
- Only 16-byte alignment and a partial last block: test offset buffers, guard words, and row counts 1/65536/65537.
- Changed float rounding or channel order: preserve source expressions and compare the 18-case end-to-end output matrix.
- Wider writes crossing pixel boundaries: guard checks and odd image dimensions must remain exact.
- Compiler scalarization or spills: inspect the separately compiled production kernel and compare repeated full-frame timings.

## Task 1: Implement and Validate Packed Stores

**Files:**
- Modify: `ngx/nr_frame.cu` output storage only.
- Create: `tests/ngx_prepare_store_test.cu`, `tests/ngx_prepare_store_test.cpp`, `scripts/test_ngx_prepare.ps1`.
- Modify: `docs/optimization-validation.md` with experiment and production results.

**Interfaces:**
- Consumes: sixteen already-rounded `__half` values; destination aligned to 16 bytes, with capacity for 32 bytes.
- Produces: identical consecutive 16-bit feature values, through two vector stores.
- Test wrapper calls the actual production `storeFeatures16` implementation, not a reimplementation.

- [x] Run the wide-store code-generation test against the original source. Expected: 16 scalar stores, zero wide stores, failing the two-wide-store requirement.
- [x] Add the real-GPU half-bit test and its build/run script. Add the minimal production packing helper and replace only the old final store loop.
- [x] Compile the production source independently. Expected: exactly two 128-bit stores in prepare, no local spills. Run the bit-pattern test and its negative control.
- [x] Regenerate production PTX with the existing build script. Preserve the frozen original PTX and DLL, and record all source/asset hashes.
- [x] Run the full 18-case NGX output matrix and three alternating A/B rounds at 1080p, 1440p, and 4K, 120 frames with 20 warmup frames, profiling on and off.
- [x] Run the repository's applicable headless suites and inspect results. Record any unavailable full-demo/game coverage explicitly.
- [x] Create a fresh validated package only after the output and performance gates pass. Verify its checksums and package-local execution.
- [x] Perform a separate self-review, record evidence and limitations, and report measured results. Do not commit or publish.

## Execution Record

- Pre-flight: one task; test wrapper consumes the packing helper, while runtime ABI and layout remain unchanged.
- Temporary PTX experiment is frozen at `tmp/dx12-packed-prepare/20261004/experiment-verification.json`: 36 processes, 18 output cases, and exhaustive raw-half checks passed before production edits.
- Existing code-generation test compiled the original source into `production/scalar-red.ptx`; its offline SASS contains 16 scalar stores and no 128-bit stores.
- RED confirmed again in `production/scalar-red-confirm.log`; changed-source code generation has two 128-bit stores, 33 registers and no spills. Production PTX matches the separately inspected compile.
- The exhaustive actual-helper test passes. Its compiled-PTX negative control (missing the second store) fails on output bits; the real kernel passes again.
- Ordinary production probe output matrix: 18/18 cases, 75 captures per implementation, bit-exact against the frozen native reference. Only `nr_frame.ptx` changes among built runtime assets, and compose PTX remains identical.
- Production campaign: 36 processes, 9/9 matched uninstrumented improvements, unchanged launch inventories and exact first/final frames. Median-of-run-medians reductions are 7.14% / 9.04% / 7.77% at 1080p / 1440p / 4K. Source/asset/DLL/probe guards and raw timestamp accounting pass.
- A temporary suite aggregator misclassified the workspace-policy script's expected negative-command exit as a failed script. The real policy assertions passed. A focused harness test reproduced the false failure, the aggregator was corrected, and the same test passed. No production or existing test script changed; the full aggregate suite is rerun into a fresh directory.
- Full headless build passed; `production/headless-verified/results.json` records 13/13 passing suites, including the eight-case graph matrix and existing pass/workspace/staging tests.
- Final review: self-review, honoring the user's no-subagent request. No blocking findings. Checked bit order, unsigned shifts, alignment/stride, bounds, unchanged float conversions/compose PTX/dispatch, actual-helper tests, and evidence provenance. Full game/VR and other GPU/driver combinations remain unverified, not claimed as passed.
- Packaging passed with both reports included. All 123 ZIP entries match the verified package, and a package-local 513x377 run with ABI/capability checks matches three native frames bit-for-bit. The original package's complete manifest is unchanged.
- Final scope/identity audit passed; only existing production source `ngx/nr_frame.cu` changed, with three new validation files and documentation. No commit, push, deployment or worktree change.
