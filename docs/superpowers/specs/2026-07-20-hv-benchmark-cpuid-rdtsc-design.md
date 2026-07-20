# HV Benchmark CPUID/RDTSC Design

## Goal

Fold the standalone user-mode CPUID timing experiment into the existing VM-exit benchmark and rename that benchmark target from `vmexit-bench` to `hv-benchmark`.

## Target Rename

Rename `tools/vmexit-bench` to `tools/hv-benchmark`, including the project file, Visual Studio solution entry, root namespace, intermediate directory, executable target, and documentation references. The output remains under `build\bin\tools`, with the executable named `hv-benchmark.exe`.

Delete the local experimental `build\bin\tools\test.c`. It is a generated build-directory file and is not part of the tracked source tree.

## Timing Measurement

Add a user-mode `MeasureCpuidRdtscTiming` helper to `benchmark.cpp`. It performs:

1. 100 iterations of `RDTSC`, `CPUID(1)`, then `RDTSC`.
2. 100 iterations of two back-to-back `RDTSC` reads.
3. Integer average calculation for both cumulative totals.
4. A signed adjusted result: CPUID average minus RDTSC average.

The helper uses MSVC x64 intrinsics. It does not execute `__writecr8`, raise IRQL, or use inline assembly because those operations are unavailable or invalid in a user-mode x64 program. The existing benchmark thread affinity and time-critical priority setup provide the available user-mode scheduling controls.

The unresolved decompiler function `sub_18008DA94` has no project equivalent. Its replacement is the signed adjusted timing result. The benchmark prints the raw CPUID average, raw RDTSC overhead average, and adjusted average before the existing probe table.

No `LFENCE`, extra serialization, artificial jitter, VM callback, driver IOCTL, or new command-line dependency is added. This preserves the requested measurement sequence and keeps the change isolated to the benchmark.

## Existing Probes

Keep the existing cross-core software-counter probes unchanged:

- `SERIALIZE`
- `CPUID leaf 0`
- `CPUID leaf 16h`
- Optional `VMCALL floor`

The new RDTSC measurement supplements these probes; it does not replace their statistics or acceptance criteria.

## Validation

Add a `--selfcheck` path that verifies the signed average-adjustment calculation without running the hardware benchmark. Then:

1. Build the renamed Release x64 project with warnings treated as errors.
2. Run `hv-benchmark.exe --selfcheck` and require exit code `0`.
3. Run the normal benchmark to confirm the new timing line and existing probe table are both present.
4. Run `git diff --check`.

## Scope

No driver, hypervisor, public API, IOCTL, VMCS control, rendezvous policy, or assembly probe behavior changes. The root `JohnSmith` solution and driver retain their names; only the benchmark project becomes `hv-benchmark`.
