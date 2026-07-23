# Modular hv-benchmark Timers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn `hv-benchmark.exe` into three independently selectable timer modules with detector-aligned gates, modular output, continued execution after module-specific setup errors, and no `--selfcheck` command.

**Architecture:** Keep production code in `tools/hv-benchmark/benchmark.cpp`. Add one standalone C++ test translation unit that includes the production source with `main` renamed, allowing direct tests of static CLI, gate, outcome, and panel helpers without adding a test framework or restoring a runtime self-check mode. Preserve `ops.asm` unchanged.

**Tech Stack:** C++20, Windows API, MSVC v143, MASM x64 probes, PowerShell smoke checks, assert-based standalone tests.

---

## File Structure

- Modify `tools/hv-benchmark/benchmark.cpp`: shared models, CLI parser, gates, panel formatter, three timer modules, execution orchestration, and exit aggregation.
- Create `tools/hv-benchmark/benchmark-tests.cpp`: runnable assert-based tests for deterministic logic and the two short/non-destructive timer result shapes.
- Modify `tools/hv-benchmark/hv-benchmark.vcxproj`: compile source literals as UTF-8.
- Modify `docs/build-and-test.md`: document modular commands, the `2.5` software-tick gate, exact TSC-exit semantics, and VMCALL usage.
- Verify only `tools/hv-benchmark/ops.asm`: assembly probes remain byte-for-byte unchanged.

## Task 1: Add Tested Shared CLI, Gate, Outcome, and Panel Primitives

**Files:**
- Create: `tools/hv-benchmark/benchmark-tests.cpp`
- Modify: `tools/hv-benchmark/benchmark.cpp:1-55`

- [ ] **Step 1: Write the failing standalone tests**

Create `tools/hv-benchmark/benchmark-tests.cpp` with:

```cpp
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <io.h>

#define main HvBenchmarkProgramMain
#include "benchmark.cpp"
#undef main

extern "C" void MeasureSerialize(
    volatile std::uint64_t*, std::uint64_t*, unsigned) {}
extern "C" void MeasureCpuidLeaf0(
    volatile std::uint64_t*, std::uint64_t*, unsigned) {}
extern "C" void MeasureCpuidLeaf16(
    volatile std::uint64_t*, std::uint64_t*, unsigned) {}
extern "C" void MeasureVmcall(
    volatile std::uint64_t*, std::uint64_t*, unsigned) {}

static bool Parse(
    const std::initializer_list<const char*> arguments,
    BenchmarkOptions& options)
{
    std::vector<std::string> storage(arguments);
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& argument : storage) argv.push_back(argument.data());
    return ParseOptions(static_cast<int>(argv.size()), argv.data(), options);
}

static std::string CapturePanel(const bool plain)
{
    FILE* temporary = nullptr;
    assert(tmpfile_s(&temporary) == 0);
    assert(temporary != nullptr);

    std::fflush(stdout);
    const int saved = _dup(_fileno(stdout));
    assert(saved >= 0);
    assert(_dup2(_fileno(temporary), _fileno(stdout)) == 0);

    PrintPanel("Timer", {{"name", "value"}}, plain);

    std::fflush(stdout);
    assert(_dup2(saved, _fileno(stdout)) == 0);
    _close(saved);
    std::rewind(temporary);

    std::string output;
    char buffer[256];
    while (const std::size_t count =
               std::fread(buffer, 1, sizeof(buffer), temporary)) {
        output.append(buffer, count);
    }
    std::fclose(temporary);
    output.erase(
        std::remove(output.begin(), output.end(), '\r'), output.end());
    return output;
}

int main()
{
    BenchmarkOptions options{};

    assert(Parse({"hv-benchmark.exe"}, options));
    assert(options.sampleCount == 200000);
    assert(options.modules == BenchmarkModuleAll);
    assert(!options.vmcall);
    assert(!options.plain);

    assert(Parse(
        {"hv-benchmark.exe", "--tsc-exit", "--plain", "10000"},
        options));
    assert(options.sampleCount == 10000);
    assert(options.modules == BenchmarkModuleTscExit);
    assert(options.plain);

    assert(Parse(
        {"hv-benchmark.exe", "--software-tick", "--tsc-exit", "--vmcall"},
        options));
    assert(options.modules ==
           (BenchmarkModuleSoftwareTick | BenchmarkModuleTscExit));
    assert(options.vmcall);

    assert(Parse(
        {"hv-benchmark.exe", "--all", "--tsc-cpuid", "--all"},
        options));
    assert(options.modules == BenchmarkModuleAll);

    assert(!Parse({"hv-benchmark.exe", "--selfcheck"}, options));
    assert(!Parse({"hv-benchmark.exe", "9999"}, options));
    assert(!Parse({"hv-benchmark.exe", "10000", "20000"}, options));
    assert(!Parse({"hv-benchmark.exe", "--unknown"}, options));

    assert(SoftwareTickPasses(2.499));
    assert(!SoftwareTickPasses(2.5));
    assert(!SoftwareTickPasses(2.501));

    assert(TscExitPasses(999));
    assert(!TscExitPasses(0));
    assert(!TscExitPasses(1000));

    const SoftwareTickTripwire quiet = DetectSoftwareTickTripwire(50.0, 400.0);
    assert(!quiet.equalOne);
    assert(!quiet.greaterThan2000);
    const SoftwareTickTripwire noisy = DetectSoftwareTickTripwire(1.0, 2001.0);
    assert(noisy.equalOne);
    assert(noisy.greaterThan2000);

    assert(CombineOutcome(0, {true, true, 0}) == 0);
    assert(CombineOutcome(0, {true, false, 0}) == 1);
    assert(CombineOutcome(1, {false, true, 7}) == 7);
    assert(CombineOutcome(7, {false, true, 5}) == 7);
    assert(CombineOutcome(0, {false, true, 0}) == 0);

    assert(AverageAdjustedTiming(1000, 200, 100) == 8);
    assert(AverageAdjustedTiming(200, 1000, 100) == -8);
    assert(AverageAdjustedTiming(0, 0, 0) == 0);

    const std::string plain = CapturePanel(true);
    assert(plain == "Timer:\nname | value\n\n");
    const std::string boxed = CapturePanel(false);
    assert(boxed.find("\xE2\x94\x8C") != std::string::npos);
    assert(boxed.find("Timer") != std::string::npos);
    assert(boxed.find("name | value") != std::string::npos);
    assert(boxed.find("\xE2\x94\x98") != std::string::npos);

    std::puts("hv-benchmark tests passed");
    return 0;
}
```

- [ ] **Step 2: Compile the tests and verify RED**

Run from a Visual Studio developer PowerShell:

```powershell
cl /nologo /std:c++20 /EHsc /W4 /WX `
  .\tools\hv-benchmark\benchmark-tests.cpp `
  /Fe:"$env:TEMP\hv-benchmark-tests.exe"
```

Expected: compilation fails because `BenchmarkOptions`, module constants,
`ParseOptions`, gate helpers, outcome types, and `PrintPanel` do not exist.

- [ ] **Step 3: Add the shared types**

Add `#include <cerrno>` to `benchmark.cpp`. After `CpuidRdtscTiming`, add:

```cpp
enum BenchmarkModule : unsigned {
    BenchmarkModuleSoftwareTick = 1u << 0,
    BenchmarkModuleTscExit = 1u << 1,
    BenchmarkModuleTscCpuid = 1u << 2,
    BenchmarkModuleAll = BenchmarkModuleSoftwareTick |
                         BenchmarkModuleTscExit |
                         BenchmarkModuleTscCpuid
};

struct BenchmarkOptions {
    unsigned sampleCount = 200000;
    unsigned modules = 0;
    bool vmcall = false;
    bool plain = false;
};

struct PanelRow {
    std::string name;
    std::string value;
};

struct ModuleOutcome {
    bool gated;
    bool passed;
    int setupError;
};

struct ModuleResult {
    std::string title;
    std::vector<PanelRow> rows;
    ModuleOutcome outcome;
};

struct SoftwareTickTripwire {
    bool equalOne;
    bool greaterThan2000;
};
```

- [ ] **Step 4: Add the parser and deterministic policies**

Add before `PinCurrentThread`:

```cpp
static bool ParseOptions(
    const int argc,
    char** const argv,
    BenchmarkOptions& options)
{
    options = {};
    bool sampleSeen = false;
    bool moduleSeen = false;

    for (int index = 1; index < argc; ++index) {
        const char* const argument = argv[index];
        if (std::strcmp(argument, "--all") == 0) {
            options.modules |= BenchmarkModuleAll;
            moduleSeen = true;
        } else if (std::strcmp(argument, "--software-tick") == 0) {
            options.modules |= BenchmarkModuleSoftwareTick;
            moduleSeen = true;
        } else if (std::strcmp(argument, "--tsc-exit") == 0) {
            options.modules |= BenchmarkModuleTscExit;
            moduleSeen = true;
        } else if (std::strcmp(argument, "--tsc-cpuid") == 0) {
            options.modules |= BenchmarkModuleTscCpuid;
            moduleSeen = true;
        } else if (std::strcmp(argument, "--vmcall") == 0) {
            options.vmcall = true;
        } else if (std::strcmp(argument, "--plain") == 0) {
            options.plain = true;
        } else {
            if (argument[0] == '-' || sampleSeen) return false;
            errno = 0;
            char* end = nullptr;
            const unsigned long value = std::strtoul(argument, &end, 10);
            if (errno != 0 || end == argument || *end != '\0' ||
                value < 10000 || value > 10000000) {
                return false;
            }
            options.sampleCount = static_cast<unsigned>(value);
            sampleSeen = true;
        }
    }

    if (!moduleSeen) options.modules = BenchmarkModuleAll;
    return true;
}

static bool SoftwareTickPasses(const double leaf0Ratio)
{
    return leaf0Ratio < 2.5;
}

static bool TscExitPasses(const std::uint64_t average)
{
    return average > 0 && average < 1000;
}

static SoftwareTickTripwire DetectSoftwareTickTripwire(
    const double serializeTrimmedMean,
    const double leaf0TrimmedMean)
{
    return {
        serializeTrimmedMean == 1.0 || leaf0TrimmedMean == 1.0,
        serializeTrimmedMean > 2000.0 || leaf0TrimmedMean > 2000.0
    };
}

static int CombineOutcome(
    const int currentExitCode,
    const ModuleOutcome& outcome)
{
    if (currentExitCode >= 2) return currentExitCode;
    if (outcome.setupError >= 2) return outcome.setupError;
    if (currentExitCode == 1) return 1;
    return outcome.gated && !outcome.passed ? 1 : 0;
}
```

- [ ] **Step 5: Add the only panel-framing helper**

Add after the deterministic policies:

```cpp
static void PrintPanel(
    const std::string& title,
    const std::vector<PanelRow>& rows,
    const bool plain)
{
    std::size_t nameWidth = 0;
    std::size_t valueWidth = 0;
    for (const auto& row : rows) {
        nameWidth = (std::max)(nameWidth, row.name.size());
        valueWidth = (std::max)(valueWidth, row.value.size());
    }

    if (plain) {
        std::printf("%s:\n", title.c_str());
        for (const auto& row : rows) {
            std::printf("%s | %s\n", row.name.c_str(), row.value.c_str());
        }
        std::putchar('\n');
        return;
    }

    std::size_t innerWidth = nameWidth + valueWidth + 5;
    if (innerWidth < title.size() + 3) {
        valueWidth += title.size() + 3 - innerWidth;
        innerWidth = title.size() + 3;
    }

    std::fputs("\xE2\x94\x8C\xE2\x94\x80 ", stdout);
    std::fputs(title.c_str(), stdout);
    std::putchar(' ');
    for (std::size_t index = title.size() + 3;
         index < innerWidth;
         ++index) {
        std::fputs("\xE2\x94\x80", stdout);
    }
    std::fputs("\xE2\x94\x90\n", stdout);

    for (const auto& row : rows) {
        std::printf(
            "\xE2\x94\x82 %-*s | %-*s \xE2\x94\x82\n",
            static_cast<int>(nameWidth),
            row.name.c_str(),
            static_cast<int>(valueWidth),
            row.value.c_str());
    }

    std::fputs("\xE2\x94\x94", stdout);
    for (std::size_t index = 0; index < innerWidth; ++index) {
        std::fputs("\xE2\x94\x80", stdout);
    }
    std::fputs("\xE2\x94\x98\n\n", stdout);
}
```

- [ ] **Step 6: Compile and run the tests to verify GREEN**

```powershell
cl /nologo /std:c++20 /EHsc /W4 /WX `
  .\tools\hv-benchmark\benchmark-tests.cpp `
  /Fe:"$env:TEMP\hv-benchmark-tests.exe"
& "$env:TEMP\hv-benchmark-tests.exe"
```

Expected: compilation exits `0` and prints `hv-benchmark tests passed`.

- [ ] **Step 7: Commit the shared foundation**

```powershell
git add -- tools/hv-benchmark/benchmark.cpp `
  tools/hv-benchmark/benchmark-tests.cpp
git commit -m "refactor(bench): add modular timer core"
```

## Task 2: Isolate TSC-CPUID and Add Exact TSC-exit Measurement

**Files:**
- Modify: `tools/hv-benchmark/benchmark-tests.cpp`
- Modify: `tools/hv-benchmark/benchmark.cpp:167-237`

- [ ] **Step 1: Add failing module-shape tests**

Add before the final success message in `benchmark-tests.cpp`:

```cpp
    const ModuleResult tscCpuid = RunTscCpuidTimer();
    assert(tscCpuid.title == "TSC-CPUID timer");
    assert(!tscCpuid.outcome.gated);
    assert(tscCpuid.outcome.setupError == 0);
    assert(tscCpuid.rows.size() == 3);
    assert(tscCpuid.rows[0].name == "leaf / samples");
    assert(tscCpuid.rows[1].name == "cpuid_avg / rdtsc_avg");
    assert(tscCpuid.rows[2].name == "adjusted");

    const ModuleResult tscExit = RunTscExitTimer();
    assert(tscExit.title == "TSC-exit timer");
    assert(tscExit.outcome.gated);
    assert(tscExit.outcome.setupError == 0);
    assert(tscExit.rows.size() == 3);
    assert(tscExit.rows[0].name == "samples / sleep / leaf");
    assert(tscExit.rows[1].name == "average / threshold");
    assert(tscExit.rows[2].name == "result");
```

- [ ] **Step 2: Compile and verify RED**

Run the Task 1 test compile command.

Expected: compilation fails because `RunTscCpuidTimer` and
`RunTscExitTimer` do not exist.

- [ ] **Step 3: Wrap the existing leaf-1 calibration as a module**

Keep `AverageAdjustedTiming` and `MeasureCpuidRdtscTiming` unchanged. Add after
`MeasureCpuidRdtscTiming`:

```cpp
static ModuleResult RunTscCpuidTimer()
{
    const CpuidRdtscTiming timing = MeasureCpuidRdtscTiming();
    char averages[96];
    char adjusted[48];
    std::snprintf(
        averages,
        sizeof(averages),
        "%llu / %llu",
        static_cast<unsigned long long>(timing.cpuidAverage),
        static_cast<unsigned long long>(timing.rdtscAverage));
    std::snprintf(
        adjusted,
        sizeof(adjusted),
        "%lld",
        static_cast<long long>(timing.adjustedAverage));

    return {
        "TSC-CPUID timer",
        {
            {"leaf / samples", "1 / 100 CPUID + 100 RDTSC"},
            {"cpuid_avg / rdtsc_avg", averages},
            {"adjusted", adjusted}
        },
        {false, true, 0}
    };
}
```

- [ ] **Step 4: Add the exact Pafish-style TSC-exit module**

Add immediately after `RunTscCpuidTimer`:

```cpp
static __declspec(noinline) ModuleResult RunTscExitTimer()
{
    constexpr unsigned iterationCount = 10;
    constexpr DWORD sleepMilliseconds = 500;
    std::uint64_t total = 0;
    int registers[4]{};
    volatile int cpuidSink = 0;

    for (unsigned index = 0; index < iterationCount; ++index) {
        const std::uint64_t before = __rdtsc();
        __cpuid(registers, 0);
        cpuidSink ^= registers[0] ^ registers[1] ^
                     registers[2] ^ registers[3];
        const std::uint64_t after = __rdtsc();
        total += after - before;
        Sleep(sleepMilliseconds);
    }

    static_cast<void>(cpuidSink);
    const std::uint64_t average = total / iterationCount;
    const bool passed = TscExitPasses(average);
    char averageLine[64];
    std::snprintf(
        averageLine,
        sizeof(averageLine),
        "%llu / 1000",
        static_cast<unsigned long long>(average));

    return {
        "TSC-exit timer",
        {
            {"samples / sleep / leaf", "10 / 500ms / 0"},
            {"average / threshold", averageLine},
            {"result", passed ? "PASS" : "FAIL"}
        },
        {true, passed, 0}
    };
}
```

Do not insert any fence, baseline subtraction, trimming, or outlier handling.
The test executable now takes about five seconds because it executes the exact
ten sleeps.

- [ ] **Step 5: Compile and run the tests to verify GREEN**

Run the Task 1 compile and test commands.

Expected: `hv-benchmark tests passed` after approximately five seconds.

- [ ] **Step 6: Commit both TSC modules**

```powershell
git add -- tools/hv-benchmark/benchmark.cpp `
  tools/hv-benchmark/benchmark-tests.cpp
git commit -m "feat(bench): add modular TSC timers"
```

## Task 3: Modularize Software-tick and Replace Main Orchestration

**Files:**
- Modify: `tools/hv-benchmark/benchmark.cpp:121-353`
- Test: `tools/hv-benchmark/benchmark-tests.cpp`

- [ ] **Step 1: Run a failing black-box CLI assertion**

Build the current benchmark, then run:

```powershell
$exe = '.\build\bin\tools\hv-benchmark.exe'
$output = & $exe --tsc-cpuid --plain 2>&1
if ($LASTEXITCODE -ne 0 -or
    ($output -join "`n") -notmatch 'TSC-CPUID timer:') {
    throw 'modular CLI not implemented'
}
```

Expected: throws `modular CLI not implemented` because the old `main` does not
parse module flags or print a modular panel.

- [ ] **Step 2: Make software probe collection return data only**

Replace the existing `RunProbe` with:

```cpp
static bool RunProbe(
    const Probe probe,
    ClockLine& clock,
    const unsigned sampleCount,
    Statistics& statistics,
    DWORD& exceptionCode)
{
    std::vector<std::uint64_t> warmup(4096);
    std::vector<std::uint64_t> samples(sampleCount);
    exceptionCode = 0;
    if (!InvokeProbeSeh(
            probe,
            &clock.value,
            warmup.data(),
            static_cast<unsigned>(warmup.size()),
            &exceptionCode) ||
        !InvokeProbeSeh(
            probe,
            &clock.value,
            samples.data(),
            sampleCount,
            &exceptionCode)) {
        return false;
    }
    statistics = Summarize(std::move(samples));
    return true;
}
```

- [ ] **Step 3: Add setup-result construction**

Add after `PrintPanel`:

```cpp
static ModuleResult MakeSetupErrorResult(
    const char* const title,
    const int code,
    const std::string& message)
{
    char result[48];
    std::snprintf(result, sizeof(result), "SETUP_ERROR code=%d", code);
    return {
        title,
        {{"setup", message}, {"result", result}},
        {false, true, code}
    };
}
```

- [ ] **Step 4: Add the software-tick module**

Add after `RunProbe`:

```cpp
static ModuleResult RunSoftwareTickTimer(
    const BenchmarkOptions& options,
    const LogicalCpu testCpu,
    const LogicalCpu clockCpu,
    const unsigned maximumLeaf,
    const bool serializeSupported)
{
    if (!serializeSupported) {
        return MakeSetupErrorResult(
            "Software-tick timer",
            6,
            "SERIALIZE is not enumerated by CPUID.7.0:EDX[14]");
    }

    ClockLine clock{};
    ControlLine control{};
    std::thread clockThread([&] {
        const bool pinned = PinCurrentThread(clockCpu);
        DWORD setupError = pinned ? ERROR_SUCCESS : GetLastError();
        const bool prioritized = pinned && SetThreadPriority(
            GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != FALSE;
        if (pinned && !prioritized) setupError = GetLastError();
        control.setupError.store(setupError, std::memory_order_relaxed);
        control.setupSucceeded.store(
            pinned && prioritized, std::memory_order_relaxed);
        control.ready.store(true, std::memory_order_release);
        while (!control.stop.load(std::memory_order_relaxed)) ++clock.value;
    });

    while (!control.ready.load(std::memory_order_acquire)) YieldProcessor();
    if (!control.setupSucceeded.load(std::memory_order_relaxed)) {
        control.stop.store(true, std::memory_order_relaxed);
        clockThread.join();
        char message[96];
        std::snprintf(
            message,
            sizeof(message),
            "clock affinity/priority failed (error %lu)",
            control.setupError.load(std::memory_order_relaxed));
        return MakeSetupErrorResult("Software-tick timer", 5, message);
    }

    Sleep(50);
    Statistics serialize{};
    Statistics leaf0{};
    Statistics leaf16{};
    Statistics vmcall{};
    DWORD serializeException = 0;
    DWORD leaf0Exception = 0;
    DWORD leaf16Exception = 0;
    DWORD vmcallException = 0;
    const bool haveSerialize = RunProbe(
        MeasureSerialize,
        clock,
        options.sampleCount,
        serialize,
        serializeException);
    const bool haveLeaf0 = RunProbe(
        MeasureCpuidLeaf0,
        clock,
        options.sampleCount,
        leaf0,
        leaf0Exception);
    const bool leaf16Supported = maximumLeaf >= 0x16;
    const bool haveLeaf16 = leaf16Supported && RunProbe(
        MeasureCpuidLeaf16,
        clock,
        options.sampleCount,
        leaf16,
        leaf16Exception);
    const bool haveVmcall = options.vmcall && RunProbe(
        MeasureVmcall,
        clock,
        options.sampleCount,
        vmcall,
        vmcallException);

    control.stop.store(true, std::memory_order_relaxed);
    clockThread.join();

    std::vector<PanelRow> rows;
    char topology[128];
    std::snprintf(
        topology,
        sizeof(topology),
        "%u / group%u/cpu%u / group%u/cpu%u",
        options.sampleCount,
        testCpu.group,
        testCpu.number,
        clockCpu.group,
        clockCpu.number);
    rows.push_back({"samples / test / clock", topology});
    rows.push_back({
        "probe",
        "mean | trim-mean | p10 | median | p90 | ratio(trim)"});

    const auto appendProbe = [&](const char* const name,
                                 const Statistics& value,
                                 const bool available,
                                 const double ratio,
                                 const char* const unavailable) {
        if (!available) {
            rows.push_back({name, unavailable});
            return;
        }
        char line[160];
        std::snprintf(
            line,
            sizeof(line),
            "%.2f | %.2f | %llu | %llu | %llu | %.3f",
            value.mean,
            value.trimmedMean,
            static_cast<unsigned long long>(value.p10),
            static_cast<unsigned long long>(value.median),
            static_cast<unsigned long long>(value.p90),
            ratio);
        rows.push_back({name, line});
    };

    const double leaf0Ratio = haveSerialize && serialize.trimmedMean != 0.0 &&
                              haveLeaf0
        ? leaf0.trimmedMean / serialize.trimmedMean
        : 0.0;
    const double leaf16Ratio = haveSerialize && serialize.trimmedMean != 0.0 &&
                               haveLeaf16
        ? leaf16.trimmedMean / serialize.trimmedMean
        : 0.0;
    const double vmcallRatio = haveSerialize && serialize.trimmedMean != 0.0 &&
                               haveVmcall
        ? vmcall.trimmedMean / serialize.trimmedMean
        : 0.0;

    char serializeUnavailable[64];
    char leaf0Unavailable[64];
    char leaf16Unavailable[64];
    char vmcallUnavailable[64];
    std::snprintf(
        serializeUnavailable,
        sizeof(serializeUnavailable),
        "unavailable (exception 0x%08lX)",
        serializeException);
    std::snprintf(
        leaf0Unavailable,
        sizeof(leaf0Unavailable),
        "unavailable (exception 0x%08lX)",
        leaf0Exception);
    if (leaf16Supported) {
        std::snprintf(
            leaf16Unavailable,
            sizeof(leaf16Unavailable),
            "unavailable (exception 0x%08lX)",
            leaf16Exception);
    } else {
        std::snprintf(
            leaf16Unavailable,
            sizeof(leaf16Unavailable),
            "unavailable (unsupported)");
    }
    std::snprintf(
        vmcallUnavailable,
        sizeof(vmcallUnavailable),
        "unavailable (exception 0x%08lX)",
        vmcallException);

    appendProbe(
        "SERIALIZE",
        serialize,
        haveSerialize,
        1.0,
        serializeUnavailable);
    appendProbe(
        "CPUID leaf 0",
        leaf0,
        haveLeaf0,
        leaf0Ratio,
        leaf0Unavailable);
    appendProbe(
        "CPUID leaf 16h",
        leaf16,
        haveLeaf16,
        leaf16Ratio,
        leaf16Unavailable);
    if (options.vmcall) {
        appendProbe(
            "VMCALL floor",
            vmcall,
            haveVmcall,
            vmcallRatio,
            vmcallUnavailable);
    }

    int setupError = 0;
    if (!haveSerialize || !haveLeaf0 ||
        serialize.trimmedMean <= 0.0 || leaf0.trimmedMean <= 0.0 ||
        (leaf16Supported && !haveLeaf16) ||
        (options.vmcall && !haveVmcall)) {
        setupError = 7;
    }

    const bool gateRan = haveSerialize && haveLeaf0 &&
                         serialize.trimmedMean > 0.0 &&
                         leaf0.trimmedMean > 0.0;
    const bool passed = gateRan && SoftwareTickPasses(leaf0Ratio);
    if (gateRan) {
        char gate[96];
        std::snprintf(
            gate,
            sizeof(gate),
            "leaf0_ratio=%.3f threshold=2.5 result=%s",
            leaf0Ratio,
            passed ? "PASS" : "FAIL");
        rows.push_back({"software-tick", gate});

        const SoftwareTickTripwire tripwire = DetectSoftwareTickTripwire(
            serialize.trimmedMean, leaf0.trimmedMean);
        char trimmed[96];
        char flags[96];
        std::snprintf(
            trimmed,
            sizeof(trimmed),
            "trim_serialize=%.2f trim_leaf0=%.2f",
            serialize.trimmedMean,
            leaf0.trimmedMean);
        std::snprintf(
            flags,
            sizeof(flags),
            "tripwire_eq1=%s tripwire_gt2000=%s",
            tripwire.equalOne ? "yes" : "no",
            tripwire.greaterThan2000 ? "yes" : "no");
        rows.push_back({"tripwire trim", trimmed});
        rows.push_back({"tripwire flags", flags});
    } else {
        rows.push_back({"software-tick", "result=SETUP_ERROR code=7"});
    }

    return {
        "Software-tick timer",
        std::move(rows),
        {gateRan, passed, setupError}
    };
}
```

- [ ] **Step 5: Add usage text**

Add before `main`:

```cpp
static void PrintUsage(FILE* const output)
{
    std::fputs(
        "hv-benchmark.exe [samples] [flags]\n\n"
        "  samples              default 200000; software-tick only\n\n"
        "  --all                all modules\n"
        "  --software-tick\n"
        "  --tsc-exit\n"
        "  --tsc-cpuid\n"
        "  --vmcall             include VMCALL in software-tick\n"
        "  --plain              text framing\n",
        output);
}
```

- [ ] **Step 6: Replace the old parser, self-check branch, and main**

Delete `TimingSelfCheck`, `ParseSamples`, and the existing `main`. Replace them
with:

```cpp
int main(int argc, char** argv)
{
    BenchmarkOptions options{};
    if (!ParseOptions(argc, argv, options)) {
        PrintUsage(stderr);
        return 2;
    }

    SetConsoleOutputCP(CP_UTF8);
    const auto cores = FirstLogicalCpuPerCore();
    const auto selected = [&](const unsigned module) {
        return (options.modules & module) != 0;
    };
    const auto printAffected = [&](const int code, const std::string& message) {
        if (selected(BenchmarkModuleSoftwareTick)) {
            const ModuleResult result = MakeSetupErrorResult(
                "Software-tick timer", code, message);
            PrintPanel(result.title, result.rows, options.plain);
        }
        if (selected(BenchmarkModuleTscExit)) {
            const ModuleResult result = MakeSetupErrorResult(
                "TSC-exit timer", code, message);
            PrintPanel(result.title, result.rows, options.plain);
        }
        if (selected(BenchmarkModuleTscCpuid)) {
            const ModuleResult result = MakeSetupErrorResult(
                "TSC-CPUID timer", code, message);
            PrintPanel(result.title, result.rows, options.plain);
        }
    };

    if (cores.empty()) {
        printAffected(3, "no physical-core logical CPU was discovered");
        return 3;
    }

    const LogicalCpu testCpu = cores[0];
    const bool testPinned = PinCurrentThread(testCpu);
    DWORD testSetupError = testPinned ? ERROR_SUCCESS : GetLastError();
    const bool testPrioritized = testPinned && SetThreadPriority(
        GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != FALSE;
    if (testPinned && !testPrioritized) testSetupError = GetLastError();
    if (!testPinned || !testPrioritized) {
        char message[96];
        std::snprintf(
            message,
            sizeof(message),
            "test affinity/priority failed (error %lu)",
            testSetupError);
        printAffected(4, message);
        return 4;
    }

    int cpuid[4]{};
    __cpuid(cpuid, 0);
    const unsigned maximumLeaf = static_cast<unsigned>(cpuid[0]);
    bool serializeSupported = false;
    if (maximumLeaf >= 7) {
        __cpuidex(cpuid, 7, 0);
        serializeSupported =
            (static_cast<unsigned>(cpuid[3]) & (1u << 14)) != 0;
    }

    int exitCode = 0;
    if (selected(BenchmarkModuleSoftwareTick)) {
        ModuleResult result = cores.size() >= 2
            ? RunSoftwareTickTimer(
                  options,
                  testCpu,
                  cores[1],
                  maximumLeaf,
                  serializeSupported)
            : MakeSetupErrorResult(
                  "Software-tick timer",
                  3,
                  "software-tick requires two physical cores");
        PrintPanel(result.title, result.rows, options.plain);
        exitCode = CombineOutcome(exitCode, result.outcome);
    }

    if (selected(BenchmarkModuleTscExit)) {
        const ModuleResult result = RunTscExitTimer();
        PrintPanel(result.title, result.rows, options.plain);
        exitCode = CombineOutcome(exitCode, result.outcome);
    }

    if (selected(BenchmarkModuleTscCpuid)) {
        const ModuleResult result = RunTscCpuidTimer();
        PrintPanel(result.title, result.rows, options.plain);
        exitCode = CombineOutcome(exitCode, result.outcome);
    }

    return exitCode;
}
```

This starts the software clock thread only inside `RunSoftwareTickTimer`.
TSC-only invocations require one physical core, never create `ClockLine`, and
continue after software-specific topology, capability, clock, or probe errors.

- [ ] **Step 7: Build and verify the modular CLI GREEN**

```powershell
msbuild .\tools\hv-benchmark\hv-benchmark.vcxproj /m `
  /p:Configuration=Release /p:Platform=x64
$exe = '.\build\bin\tools\hv-benchmark.exe'
$output = & $exe --tsc-cpuid --plain 2>&1
if ($LASTEXITCODE -ne 0 -or
    ($output -join "`n") -notmatch 'TSC-CPUID timer:') {
    throw 'TSC-CPUID module smoke check failed'
}
& $exe --selfcheck 2>$null
if ($LASTEXITCODE -ne 2) {
    throw '--selfcheck must be rejected with exit code 2'
}
```

Expected: build exits `0`; TSC-CPUID prints its plain panel; `--selfcheck`
returns `2`.

- [ ] **Step 8: Re-run the standalone tests**

Run the Task 1 compile and test commands.

Expected: `hv-benchmark tests passed` after approximately five seconds.

- [ ] **Step 9: Commit modular orchestration**

```powershell
git add -- tools/hv-benchmark/benchmark.cpp
git commit -m "feat(bench): modularize benchmark timers"
```

## Task 4: Enable UTF-8 Compilation and Update Operator Documentation

**Files:**
- Modify: `tools/hv-benchmark/hv-benchmark.vcxproj:27-38`
- Modify: `docs/build-and-test.md:60-115`

- [ ] **Step 1: Run the failing UTF-8 project assertion**

```powershell
$project = Get-Content -Raw .\tools\hv-benchmark\hv-benchmark.vcxproj
if ($project -notmatch '/utf-8') {
    throw 'hv-benchmark project does not force UTF-8 source and execution encoding'
}
```

Expected: throws because the project has no `/utf-8` option.

- [ ] **Step 2: Enable UTF-8 in the benchmark project**

Inside the Release `ClCompile` block, add:

```xml
      <AdditionalOptions>/utf-8 %(AdditionalOptions)</AdditionalOptions>
```

- [ ] **Step 3: Replace the benchmark command introduction**

At the start of the benchmark section in `docs/build-and-test.md`, replace the
single default command with:

````markdown
```powershell
.\build\bin\tools\hv-benchmark.exe 200000
.\build\bin\tools\hv-benchmark.exe --tsc-exit
.\build\bin\tools\hv-benchmark.exe --tsc-cpuid
.\build\bin\tools\hv-benchmark.exe 200000 --software-tick --tsc-exit
.\build\bin\tools\hv-benchmark.exe 200000 --software-tick --plain
```

No module flag means all modules. The positional sample count applies only to
software-tick. `--plain` preserves the same rows and result lines while
removing box borders.
````

- [ ] **Step 4: Keep five-run CPUID comparisons software-only**

Change both five-run commands at the current lines 83 and 94 to:

```powershell
1..5 | ForEach-Object {
    .\build\bin\tools\hv-benchmark.exe 200000 --software-tick
}
```

Replace the old ratio-10 paragraph with:

```markdown
Every software-tick run must report
`software-tick leaf0_ratio=<value> threshold=2.5 result=PASS`, where
`leaf0_ratio < 2.5`. The trim-mean equality and greater-than-2000 tripwires are
report-only. CPUID leaf 16h remains informational.
```

- [ ] **Step 5: Document exact TSC-exit and VMCALL commands**

Before the transition-floor paragraph, add:

````markdown
For the exact Pafish-style force-exit diagnostic:

```powershell
.\build\bin\tools\hv-benchmark.exe --tsc-exit
```

The timer performs ten `RDTSC; CPUID(0); RDTSC` samples, calls `Sleep(500)`
after every sample, computes the integer mean, and passes only when
`0 < average < 1000`.
````

Change the VMCALL command to:

```powershell
.\build\bin\tools\hv-benchmark.exe 200000 --software-tick --vmcall
```

State that leaf 16h and VMCALL are printed without gates, and TSC-CPUID is a
leaf-1 calibration with no result line.

- [ ] **Step 6: Verify UTF-8 configuration and documentation**

```powershell
$project = Get-Content -Raw .\tools\hv-benchmark\hv-benchmark.vcxproj
if ($project -notmatch '/utf-8') { throw 'missing /utf-8' }
rg -n -- '--selfcheck' tools\hv-benchmark docs\build-and-test.md
if ($LASTEXITCODE -eq 0) { throw 'active benchmark docs or source still expose --selfcheck' }
rg -n 'threshold=2\.5|--tsc-exit|--tsc-cpuid|--software-tick --vmcall' `
  docs\build-and-test.md
```

Expected: no `--selfcheck` match in active source/docs; all modular commands
and the `2.5` threshold are present.

- [ ] **Step 7: Commit project and documentation changes**

```powershell
git add -- tools/hv-benchmark/hv-benchmark.vcxproj docs/build-and-test.md
git commit -m "docs(bench): document modular timers"
```

## Task 5: Full Verification and Diff Audit

**Files:**
- Verify: `tools/hv-benchmark/benchmark.cpp`
- Verify: `tools/hv-benchmark/benchmark-tests.cpp`
- Verify: `tools/hv-benchmark/hv-benchmark.vcxproj`
- Verify unchanged: `tools/hv-benchmark/ops.asm`
- Verify: `docs/build-and-test.md`

- [ ] **Step 1: Compile and run the standalone tests fresh**

```powershell
cl /nologo /std:c++20 /EHsc /W4 /WX `
  .\tools\hv-benchmark\benchmark-tests.cpp `
  /Fe:"$env:TEMP\hv-benchmark-tests.exe"
& "$env:TEMP\hv-benchmark-tests.exe"
```

Expected: exit code `0`; `hv-benchmark tests passed` after approximately five
seconds.

- [ ] **Step 2: Rebuild the Release benchmark**

```powershell
msbuild .\tools\hv-benchmark\hv-benchmark.vcxproj /t:Rebuild /m `
  /p:Configuration=Release /p:Platform=x64
```

Expected: exit code `0`, zero compiler/linker warnings, and
`build\bin\tools\hv-benchmark.exe` produced.

- [ ] **Step 3: Verify CLI rejection and plain TSC-CPUID output**

```powershell
$exe = '.\build\bin\tools\hv-benchmark.exe'
& $exe --selfcheck 2>$null
if ($LASTEXITCODE -ne 2) { throw '--selfcheck exit code mismatch' }

$plain = & $exe --tsc-cpuid --plain 2>&1
if ($LASTEXITCODE -ne 0) { throw 'TSC-CPUID failed' }
$plainText = $plain -join "`n"
if ($plainText -notmatch 'TSC-CPUID timer:' -or
    $plainText -notmatch 'cpuid_avg / rdtsc_avg' -or
    $plainText -match '┌') {
    throw 'plain TSC-CPUID framing mismatch'
}
```

- [ ] **Step 4: Verify exact TSC-exit output and gate exit**

```powershell
$tscExit = & $exe --tsc-exit --plain 2>&1
$tscExitCode = $LASTEXITCODE
if ($tscExitCode -notin 0, 1) { throw "TSC-exit setup failed: $tscExitCode" }
$tscExitText = $tscExit -join "`n"
if ($tscExitText -notmatch '10 / 500ms / 0' -or
    $tscExitText -notmatch 'average / threshold' -or
    $tscExitText -notmatch 'result \| (PASS|FAIL)') {
    throw 'TSC-exit output mismatch'
}
```

Expected: about five seconds runtime; exit `0` for PASS or `1` for gated FAIL.

- [ ] **Step 5: Verify software-tick diagnostics and combined continuation**

```powershell
$combined = & $exe 10000 --software-tick --tsc-cpuid --plain 2>&1
$combinedCode = $LASTEXITCODE
if ($combinedCode -notin 0, 1, 3, 5, 6, 7) {
    throw "unexpected combined exit code: $combinedCode"
}
$combinedText = $combined -join "`n"
if ($combinedText -notmatch 'Software-tick timer:' -or
    $combinedText -notmatch 'mean \| trim-mean \| p10 \| median \| p90 \| ratio\(trim\)' -or
    $combinedText -notmatch 'threshold=2\.5' -or
    $combinedText -notmatch 'tripwire_eq1=' -or
    $combinedText -notmatch 'TSC-CPUID timer:') {
    throw 'combined module output mismatch'
}
```

This proves TSC-CPUID still runs after software-specific topology, capability,
clock-thread, probe, or gated-result outcomes. Exit code `4` is excluded because
a common test-thread setup failure makes both selected modules unrunnable.

- [ ] **Step 6: Verify default boxed output**

```powershell
$boxed = & $exe --tsc-cpuid 2>&1
if ($LASTEXITCODE -ne 0 -or ($boxed -join "`n") -notmatch '┌') {
    throw 'UTF-8 panel output missing'
}
```

- [ ] **Step 7: Audit source and documentation invariants**

```powershell
rg -n -- '--selfcheck' tools\hv-benchmark docs\build-and-test.md
if ($LASTEXITCODE -eq 0) { throw '--selfcheck remains active' }

git diff --exit-code -- tools/hv-benchmark/ops.asm
if ($LASTEXITCODE -ne 0) { throw 'ops.asm changed' }

git diff --check
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

git status --short
git diff -- tools/hv-benchmark docs/build-and-test.md
```

Expected: no active `--selfcheck`; `ops.asm` unchanged; no whitespace errors;
only the planned benchmark/test/project/docs changes plus the pre-existing
uncommitted VMX work remain.

- [ ] **Step 8: Hardware-only acceptance**

With the intended Benchmark driver loaded on the disposable target host:

```powershell
.\build\bin\tools\hv-benchmark.exe 200000 --software-tick --vmcall
.\build\bin\tools\hv-benchmark.exe --tsc-exit
```

Record artifact SHA-256 hashes, VMCALL floor, leaf-0 ratio, TSC-exit average,
and process exit codes. Then run Pafish separately and require its exact ten
sample mean to satisfy `0 < average < 1000`. Run the existing cross-core TSC
monotonic and libuv stability workloads separately. Do not report VMAware
`VM::TIMER` as fixed by TSC behavior.
