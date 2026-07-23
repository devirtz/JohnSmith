#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

enum class BenchmarkModule : unsigned {
    SoftwareTick = 1,
    TscExit = 2,
    TscCpuid = 4,
    All = 7
};

struct BenchmarkOptions {
    unsigned sampleCount = 200000;
    BenchmarkModule modules = BenchmarkModule::All;
    bool vmcall = false;
    bool plain = false;
};

struct PanelRow { std::string name; std::string value; };

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

[[maybe_unused]] static bool ParseOptions(const int argc, char** argv, BenchmarkOptions& options)
{
    options = {};
    bool moduleSpecified = false;
    bool sampleSpecified = false;
    unsigned selected = 0;
    for (int index = 1; index < argc; ++index) {
        const char* argument = argv[index];
        if (argument[0] != '-') {
            if (sampleSpecified) return false;
            char* end = nullptr;
            errno = 0;
            const unsigned long value = std::strtoul(argument, &end, 10);
            if (errno == ERANGE || end == argument || *end != '\0' ||
                value < 10000 || value > 10000000) return false;
            options.sampleCount = static_cast<unsigned>(value);
            sampleSpecified = true;
        } else if (std::strcmp(argument, "--software-tick") == 0) {
            selected |= static_cast<unsigned>(BenchmarkModule::SoftwareTick);
            moduleSpecified = true;
        } else if (std::strcmp(argument, "--tsc-exit") == 0) {
            selected |= static_cast<unsigned>(BenchmarkModule::TscExit);
            moduleSpecified = true;
        } else if (std::strcmp(argument, "--tsc-cpuid") == 0) {
            selected |= static_cast<unsigned>(BenchmarkModule::TscCpuid);
            moduleSpecified = true;
        } else if (std::strcmp(argument, "--all") == 0) {
            selected |= static_cast<unsigned>(BenchmarkModule::All);
            moduleSpecified = true;
        } else if (std::strcmp(argument, "--vmcall") == 0) {
            options.vmcall = true;
        } else if (std::strcmp(argument, "--plain") == 0) {
            options.plain = true;
        } else {
            return false;
        }
    }
    options.modules = static_cast<BenchmarkModule>(moduleSpecified ? selected :
                                                     static_cast<unsigned>(BenchmarkModule::All));
    return true;
}

[[maybe_unused]] static bool SoftwareTickPasses(const double ratio) { return ratio < 2.5; }
[[maybe_unused]] static bool TscExitPasses(const std::uint64_t average) { return average > 0 && average < 1000; }

[[maybe_unused]] static SoftwareTickTripwire DetectSoftwareTickTripwire(
    const double serializeTrimmedMean,
    const double leaf0TrimmedMean)
{
    return {
        serializeTrimmedMean == 1.0 || leaf0TrimmedMean == 1.0,
        serializeTrimmedMean > 2000.0 || leaf0TrimmedMean > 2000.0
    };
}

[[maybe_unused]] static int CombineOutcome(
    const int currentExitCode,
    const ModuleOutcome& outcome)
{
    if (currentExitCode >= 2) return currentExitCode;
    if (outcome.setupError >= 2) return outcome.setupError;
    if (currentExitCode == 1) return currentExitCode;
    return outcome.gated && !outcome.passed ? 1 : 0;
}

[[maybe_unused]] static void PrintPanel(
    const std::string& title,
    const std::vector<PanelRow>& rows,
    const bool plain)
{
    if (plain) {
        std::printf("%s:\n", title.c_str());
        for (const PanelRow& row : rows)
            std::printf("%s | %s\n", row.name.c_str(), row.value.c_str());
        std::putchar('\n');
        return;
    }
    std::size_t nameWidth = 0;
    std::size_t valueWidth = 0;
    for (const PanelRow& row : rows) {
        nameWidth = (std::max)(nameWidth, row.name.size());
        valueWidth = (std::max)(valueWidth, row.value.size());
    }
    const std::size_t width = (std::max)(
        title.size() + 3, nameWidth + valueWidth + 5);
    std::printf("┌─ %s ", title.c_str());
    for (std::size_t i = title.size() + 3; i < width; ++i) std::printf("─");
    std::printf("┐\n");
    for (const PanelRow& row : rows)
    {
        std::printf("│ %s", row.name.c_str());
        for (std::size_t i = row.name.size(); i < nameWidth; ++i) std::putchar(' ');
        std::printf(" | %s", row.value.c_str());
        for (std::size_t i = row.value.size(); i < valueWidth; ++i) std::putchar(' ');
        for (std::size_t i = nameWidth + valueWidth + 5; i < width; ++i) std::putchar(' ');
        std::printf(" │\n");
    }
    std::printf("└");
    for (std::size_t i = 0; i < width; ++i) std::printf("─");
    std::printf("┘\n\n");
}

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

extern "C" void MeasureSerialize(volatile std::uint64_t*, std::uint64_t*, unsigned);
extern "C" void MeasureCpuidLeaf0(volatile std::uint64_t*, std::uint64_t*, unsigned);
extern "C" void MeasureCpuidLeaf16(volatile std::uint64_t*, std::uint64_t*, unsigned);
extern "C" void MeasureVmcall(volatile std::uint64_t*, std::uint64_t*, unsigned);

struct LogicalCpu {
    WORD group;
    BYTE number;
};

#pragma warning(push)
#pragma warning(disable: 4324)
struct alignas(64) ClockLine {
    volatile std::uint64_t value;
};

struct alignas(64) ControlLine {
    std::atomic<bool> ready;
    std::atomic<bool> setupSucceeded;
    std::atomic<DWORD> setupError;
    std::atomic<bool> stop;
};
#pragma warning(pop)

struct Statistics {
    double mean;
    double trimmedMean;
    std::uint64_t p10;
    std::uint64_t median;
    std::uint64_t p90;
};

struct CpuidRdtscTiming {
    std::uint64_t cpuidAverage;
    std::uint64_t rdtscAverage;
    std::int64_t adjustedAverage;
};

using Probe = void (*)(volatile std::uint64_t*, std::uint64_t*, unsigned);

static __declspec(noinline) bool InvokeProbeSeh(
    const Probe probe,
    volatile std::uint64_t* counter,
    std::uint64_t* samples,
    const unsigned sampleCount,
    DWORD* const exceptionCode)
{
    __try {
        probe(counter, samples, sampleCount);
        return true;
    }
    __except ((*exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool PinCurrentThread(const LogicalCpu cpu)
{
    GROUP_AFFINITY affinity{};
    affinity.Group = cpu.group;
    affinity.Mask = KAFFINITY{1} << cpu.number;
    return SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr) != FALSE;
}

static std::vector<LogicalCpu> FirstLogicalCpuPerCore()
{
    DWORD bytes = 0;
    if (GetLogicalProcessorInformationEx(
            RelationProcessorCore, nullptr, &bytes) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
        return {};
    }
    std::vector<BYTE> storage(bytes);
    if (!GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data()),
            &bytes)) {
        return {};
    }

    std::vector<LogicalCpu> result;
    for (DWORD offset = 0; offset < bytes;) {
        auto* entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
            storage.data() + offset);
        if (entry->Size == 0 || entry->Size > bytes - offset) {
            return {};
        }
        if (entry->Relationship == RelationProcessorCore) {
            const auto& relationship = entry->Processor;
            for (WORD groupIndex = 0; groupIndex < relationship.GroupCount; ++groupIndex) {
                const GROUP_AFFINITY& group = relationship.GroupMask[groupIndex];
                unsigned long number = 0;
                if (_BitScanForward64(&number, group.Mask)) {
                    result.push_back({group.Group, static_cast<BYTE>(number)});
                    break;
                }
            }
        }
        offset += entry->Size;
    }
    return result;
}

static Statistics Summarize(std::vector<std::uint64_t> samples)
{
    std::sort(samples.begin(), samples.end());
    const std::size_t count = samples.size();
    long double sum = 0;
    for (const auto value : samples) sum += value;

    const std::size_t trim = count / 100;
    long double trimmed = 0;
    for (std::size_t index = trim; index < count - trim; ++index) {
        trimmed += samples[index];
    }
    return {
        static_cast<double>(sum / count),
        static_cast<double>(trimmed / (count - 2 * trim)),
        samples[count / 10],
        samples[count / 2],
        samples[(count * 9) / 10]
    };
}

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

    int setupError = ERROR_SUCCESS;
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

[[maybe_unused]] static ModuleResult RunTscCpuidTimer()
{
    const CpuidRdtscTiming timing = MeasureCpuidRdtscTiming();
    return {
        "TSC-CPUID timer",
        {
            {"leaf / samples", "1 / 100 CPUID + 100 RDTSC"},
            {"cpuid_avg / rdtsc_avg",
             std::to_string(timing.cpuidAverage) + " / " +
                 std::to_string(timing.rdtscAverage)},
            {"adjusted", std::to_string(timing.adjustedAverage)}
        },
        {false, true, ERROR_SUCCESS}
    };
}

[[maybe_unused]] static __declspec(noinline) ModuleResult RunTscExitTimer()
{
    constexpr unsigned iterationCount = 10;
    std::uint64_t total = 0;
    int registers[4]{};
    volatile int cpuidResult[4]{};

    for (unsigned index = 0; index < iterationCount; ++index) {
        const std::uint64_t before = __rdtsc();
        __cpuid(registers, 0);
        const std::uint64_t after = __rdtsc();
        total += after - before;
        Sleep(500);
        cpuidResult[0] = registers[0];
        cpuidResult[1] = registers[1];
        cpuidResult[2] = registers[2];
        cpuidResult[3] = registers[3];
    }

    const std::uint64_t average = total / iterationCount;
    const bool passed = TscExitPasses(average);
    static_cast<void>(cpuidResult[0]);
    return {
        "TSC-exit timer",
        {
            {"samples / sleep / leaf", "10 / 500ms / 0"},
            {"average / threshold", std::to_string(average) + " / 1000"},
            {"result", passed ? "PASS" : "FAIL"}
        },
        {true, passed, ERROR_SUCCESS}
    };
}

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

int main(int argc, char** argv)
{
    BenchmarkOptions options{};
    if (!ParseOptions(argc, argv, options)) {
        PrintUsage(stderr);
        return 2;
    }

    SetConsoleOutputCP(CP_UTF8);
    const auto cores = FirstLogicalCpuPerCore();
    const auto selected = [&](const BenchmarkModule module) {
        return (static_cast<unsigned>(options.modules) &
                static_cast<unsigned>(module)) != 0;
    };
    const auto printAffected = [&](const int code, const std::string& message) {
        if (selected(BenchmarkModule::SoftwareTick)) {
            const ModuleResult result = MakeSetupErrorResult(
                "Software-tick timer", code, message);
            PrintPanel(result.title, result.rows, options.plain);
        }
        if (selected(BenchmarkModule::TscExit)) {
            const ModuleResult result = MakeSetupErrorResult(
                "TSC-exit timer", code, message);
            PrintPanel(result.title, result.rows, options.plain);
        }
        if (selected(BenchmarkModule::TscCpuid)) {
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
    if (selected(BenchmarkModule::SoftwareTick)) {
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

    if (selected(BenchmarkModule::TscExit)) {
        const ModuleResult result = RunTscExitTimer();
        PrintPanel(result.title, result.rows, options.plain);
        exitCode = CombineOutcome(exitCode, result.outcome);
    }

    if (selected(BenchmarkModule::TscCpuid)) {
        const ModuleResult result = RunTscCpuidTimer();
        PrintPanel(result.title, result.rows, options.plain);
        exitCode = CombineOutcome(exitCode, result.outcome);
    }

    return exitCode;
}
