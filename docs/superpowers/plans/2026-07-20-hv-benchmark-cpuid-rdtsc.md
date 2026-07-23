# HV Benchmark CPUID/RDTSC Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Integrate the user-mode CPUID/RDTSC timing routine into the existing benchmark and fully rename that benchmark target to `hv-benchmark` without moving the other monorepo projects.

**Architecture:** Keep `johnsmithctl` and the renamed benchmark as separate applications under `tools/` in the existing `JohnSmith.sln`. Add one intrinsic-only timing helper and one deterministic arithmetic self-check to the benchmark C++ harness; preserve all existing MASM probes and driver benchmark controls.

**Tech Stack:** C++20, MSVC x64 intrinsics, MASM, Visual Studio/MSBuild, PowerShell.

---

## File Structure

- Modify then rename `tools/vmexit-bench/benchmark.cpp` to `tools/hv-benchmark/benchmark.cpp`: add the timing calculation, measurement helper, self-check command, and output.
- Rename `tools/vmexit-bench/ops.asm` to `tools/hv-benchmark/ops.asm`: path-only move; assembly behavior remains unchanged.
- Rename and modify `tools/vmexit-bench/vmexit-bench.vcxproj` to `tools/hv-benchmark/hv-benchmark.vcxproj`: update project identity strings and output name while retaining its GUID.
- Modify `JohnSmith.sln`: point the existing benchmark project GUID at the renamed project.
- Modify `docs/build-and-test.md`: update executable commands and hashes.
- Modify `docs/implementation-status.md`: update the benchmark path and name.
- Delete local ignored file `build/bin/tools/test.c`: remove the superseded experiment without adding a tracked deletion.

The driver property `JohnSmithVmexitBenchmark` and macro `JOHNSMITH_VMEXIT_BENCHMARK` remain unchanged because they describe the VM-exit-specific driver build behavior, not the application project name.

### Task 1: Define and test adjusted timing arithmetic

**Files:**
- Modify: `tools/vmexit-bench/benchmark.cpp`

- [ ] **Step 1: Add the failing self-check contract**

Add this function above `ParseSamples`:

```cpp
static bool TimingSelfCheck()
{
    return AverageAdjustedTiming(1000, 200, 100) == 8 &&
           AverageAdjustedTiming(200, 1000, 100) == -8 &&
           AverageAdjustedTiming(0, 0, 0) == 0;
}
```

Add this as the first branch in `main`:

```cpp
    if (argc == 2 && std::strcmp(argv[1], "--selfcheck") == 0) {
        if (!TimingSelfCheck()) {
            std::fputs("hv-benchmark selfcheck failed\n", stderr);
            return 1;
        }
        std::puts("hv-benchmark selfcheck passed");
        return 0;
    }
```

- [ ] **Step 2: Build to verify the contract fails**

Run:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' .\tools\vmexit-bench\vmexit-bench.vcxproj /m /p:Configuration=Release /p:Platform=x64
```

Expected: build fails with `C3861` because `AverageAdjustedTiming` is not defined.

- [ ] **Step 3: Add the minimal calculation helper**

Add this immediately above `TimingSelfCheck`:

```cpp
static std::int64_t AverageAdjustedTiming(
    const std::uint64_t cpuidTotal,
    const std::uint64_t rdtscTotal,
    const unsigned iterations)
{
    if (iterations == 0) return 0;
    const auto divisor = static_cast<std::uint64_t>(iterations);
    return static_cast<std::int64_t>(cpuidTotal / divisor) -
           static_cast<std::int64_t>(rdtscTotal / divisor);
}
```

- [ ] **Step 4: Build and run the self-check**

Run:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' .\tools\vmexit-bench\vmexit-bench.vcxproj /m /p:Configuration=Release /p:Platform=x64
.\build\bin\tools\vmexit-bench.exe --selfcheck
```

Expected: build succeeds with zero warnings and the executable prints `hv-benchmark selfcheck passed` with exit code `0`. The temporary mismatch between the old executable filename and new output text is removed in Task 3.

### Task 2: Add the functionally equivalent user-mode measurement

**Files:**
- Modify: `tools/vmexit-bench/benchmark.cpp`

- [ ] **Step 1: Verify the normal-output contract fails**

Run:

```powershell
$output = .\build\bin\tools\vmexit-bench.exe 10000 | Out-String
if ($LASTEXITCODE -ne 0) { throw "Benchmark failed with exit code $LASTEXITCODE." }
if ($output -notmatch '(?m)^cpuid-rdtsc leaf=1 iterations=100 ') {
    throw 'Missing CPUID/RDTSC timing output.'
}
```

Expected: the command throws `Missing CPUID/RDTSC timing output.` because Task 1 does not yet produce the new measurement line.

- [ ] **Step 2: Add the result type and measurement helper**

Add the result type near `Statistics`:

```cpp
struct CpuidRdtscTiming {
    std::uint64_t cpuidAverage;
    std::uint64_t rdtscAverage;
    std::int64_t adjustedAverage;
};
```

Add this helper below `AverageAdjustedTiming`:

```cpp
static __declspec(noinline) CpuidRdtscTiming MeasureCpuidRdtscTiming()
{
    constexpr unsigned iterationCount = 100;
    std::uint64_t cpuidTotal = 0;
    std::uint64_t rdtscTotal = 0;
    int registers[4]{};
    volatile int cpuidResult[4]{};

    for (unsigned index = 0; index < iterationCount; ++index) {
        const std::uint64_t before = __rdtsc();
        __cpuid(registers, 1);
        cpuidResult[0] = registers[0];
        cpuidResult[1] = registers[1];
        cpuidResult[2] = registers[2];
        cpuidResult[3] = registers[3];
        const std::uint64_t after = __rdtsc();
        cpuidTotal += after - before;
    }

    for (unsigned index = 0; index < iterationCount; ++index) {
        const std::uint64_t before = __rdtsc();
        const std::uint64_t after = __rdtsc();
        rdtscTotal += after - before;
    }

    const auto divisor = static_cast<std::uint64_t>(iterationCount);
    const CpuidRdtscTiming result{
        cpuidTotal / divisor,
        rdtscTotal / divisor,
        AverageAdjustedTiming(cpuidTotal, rdtscTotal, iterationCount)
    };
    static_cast<void>(cpuidResult[0]);
    return result;
}
```

This retains the requested 100-iteration `RDTSC`, `CPUID(1)`, `RDTSC` sequence and the separate back-to-back RDTSC loop. The volatile CPUID result stores preserve the decompiled function's observable result writes. Do not add CR8 access, IRQL calls, `LFENCE`, or inline assembly.

- [ ] **Step 3: Print the measurement after benchmark thread setup**

Immediately after the existing `Sleep(50);`, calculate the result:

```cpp
    const CpuidRdtscTiming cpuidRdtsc = MeasureCpuidRdtscTiming();
```

Immediately after the existing `samples=...` header, print it:

```cpp
    std::printf(
        "cpuid-rdtsc leaf=1 iterations=100 cpuid_avg=%llu "
        "rdtsc_avg=%llu adjusted=%lld\n",
        static_cast<unsigned long long>(cpuidRdtsc.cpuidAverage),
        static_cast<unsigned long long>(cpuidRdtsc.rdtscAverage),
        static_cast<long long>(cpuidRdtsc.adjustedAverage));
```

- [ ] **Step 4: Build and smoke-test both paths**

Run:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' .\tools\vmexit-bench\vmexit-bench.vcxproj /m /p:Configuration=Release /p:Platform=x64
.\build\bin\tools\vmexit-bench.exe --selfcheck
.\build\bin\tools\vmexit-bench.exe 10000
```

Expected: zero compiler warnings, self-check passes, and the normal run prints one `cpuid-rdtsc leaf=1` line followed by the unchanged probe table.

- [ ] **Step 5: Commit the working timing feature**

```powershell
git add -- tools/vmexit-bench/benchmark.cpp
git commit -m "feat(bench): add CPUID RDTSC timing"
```

### Task 3: Rename the benchmark target in place

**Files:**
- Rename: `tools/vmexit-bench` to `tools/hv-benchmark`
- Rename: `tools/hv-benchmark/vmexit-bench.vcxproj` to `tools/hv-benchmark/hv-benchmark.vcxproj`
- Modify: `tools/hv-benchmark/hv-benchmark.vcxproj`
- Modify: `JohnSmith.sln`

- [ ] **Step 1: Verify the directory move stays inside the worktree**

Run:

```powershell
$root = (Resolve-Path .).Path
$source = (Resolve-Path .\tools\vmexit-bench).Path
$destination = [System.IO.Path]::GetFullPath((Join-Path $root 'tools\hv-benchmark'))
if (-not $source.StartsWith("$root\", [System.StringComparison]::OrdinalIgnoreCase) -or
    -not $destination.StartsWith("$root\", [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Benchmark move escapes the worktree.'
}
```

Expected: no output and no exception.

- [ ] **Step 2: Move the tracked directory and project file**

Run as separate commands:

```powershell
git mv tools/vmexit-bench tools/hv-benchmark
git mv tools/hv-benchmark/vmexit-bench.vcxproj tools/hv-benchmark/hv-benchmark.vcxproj
```

- [ ] **Step 3: Update the project identity**

In `tools/hv-benchmark/hv-benchmark.vcxproj`, preserve project GUID `{4EB727DA-8184-4E79-8301-4323C89DB217}` and replace only these values:

```xml
    <RootNamespace>HvBenchmark</RootNamespace>
```

```xml
    <IntDir>$(MSBuildThisFileDirectory)..\..\build\obj\hv-benchmark\</IntDir>
    <TargetName>hv-benchmark</TargetName>
```

- [ ] **Step 4: Update the solution project entry**

Replace the benchmark entry in `JohnSmith.sln` with:

```text
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "hv-benchmark", "tools\hv-benchmark\hv-benchmark.vcxproj", "{4EB727DA-8184-4E79-8301-4323C89DB217}"
```

- [ ] **Step 5: Build the renamed project and run its self-check**

Run:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' .\tools\hv-benchmark\hv-benchmark.vcxproj /m /p:Configuration=Release /p:Platform=x64
.\build\bin\tools\hv-benchmark.exe --selfcheck
```

Expected: build succeeds with zero warnings and self-check passes.

### Task 4: Update operational documentation and remove the experiment

**Files:**
- Modify: `docs/build-and-test.md`
- Modify: `docs/implementation-status.md`
- Delete locally: `build/bin/tools/test.c`

- [ ] **Step 1: Update active benchmark references**

In `docs/build-and-test.md`, replace every executable reference:

```text
vmexit-bench.exe
```

with:

```text
hv-benchmark.exe
```

In `docs/implementation-status.md`, replace:

```markdown
| VM-exit measurement | `tools/vmexit-bench/` measures CPUID and Benchmark VMCALL transitions. |
```

with:

```markdown
| Hypervisor measurement | `tools/hv-benchmark/` measures CPUID, RDTSC, SERIALIZE, and Benchmark VMCALL transitions. |
```

Do not rewrite the design specification's historical statement that the target is renamed from `vmexit-bench`.

- [ ] **Step 2: Remove the ignored standalone source if present**

Run:

```powershell
$testSource = Join-Path (Resolve-Path .).Path 'build\bin\tools\test.c'
if (Test-Path -LiteralPath $testSource) {
    Remove-Item -LiteralPath $testSource
}
```

Expected: `build\bin\tools\test.c` no longer exists. No Git deletion appears because `build/` is ignored.

- [ ] **Step 3: Verify no active old target references remain**

Run:

```powershell
rg -n "tools[\\/]vmexit-bench|vmexit-bench\.exe|VmexitBench" `
    .\JohnSmith.sln `
    .\tools `
    .\docs\build-and-test.md `
    .\docs\implementation-status.md
```

Expected: no matches. Descriptive `vmexit-bench` occurrences in the design and implementation-plan documents are allowed because they name the source target being renamed.

- [ ] **Step 4: Build the complete solution**

Run each configuration separately:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' .\JohnSmith.sln /m /p:Configuration=Debug /p:Platform=x64
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' .\JohnSmith.sln /m /p:Configuration=Release /p:Platform=x64
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' .\JohnSmith.sln /m /p:Configuration=Benchmark /p:Platform=x64
```

Expected: all three builds finish with `0 Warning(s)` and `0 Error(s)`.

- [ ] **Step 5: Run final checks**

Run:

```powershell
.\build\bin\tools\hv-benchmark.exe --selfcheck
.\build\bin\tools\hv-benchmark.exe 10000
.\build\bin\tools\johnsmithctl.exe selftest
git diff --check
git status --short
```

Expected:

- `hv-benchmark selfcheck passed`.
- Normal benchmark prints the CPUID/RDTSC line and existing probe table.
- `johnsmithctl.exe selftest` passes.
- `git diff --check` prints nothing.
- Git status contains the intended benchmark rename, source changes, documentation changes, this plan, and the pre-existing rendezvous work only.

- [ ] **Step 6: Commit the rename and documentation**

Stage only the benchmark rename, solution entry, operational documentation, and this plan:

```powershell
git add -- JohnSmith.sln tools/hv-benchmark docs/build-and-test.md docs/implementation-status.md docs/superpowers/plans/2026-07-20-hv-benchmark-cpuid-rdtsc.md
git commit -m "refactor(bench): rename hv benchmark target"
```

Do not stage the pre-existing rendezvous policy and documentation changes.
