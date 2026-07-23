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
    DWORD setupError;
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
[[maybe_unused]] static bool TscExitPasses(const double average) { return average > 0 && average < 1000; }

[[maybe_unused]] static SoftwareTickTripwire DetectSoftwareTickTripwire(const std::vector<double>& values)
{
    SoftwareTickTripwire result{false, false};
    for (const double value : values) {
        result.equalOne = result.equalOne || value == 1.0;
        result.greaterThan2000 = result.greaterThan2000 || value > 2000.0;
    }
    return result;
}

[[maybe_unused]] static ModuleOutcome CombineOutcome(const ModuleOutcome left, const ModuleOutcome right)
{
    return {
        left.gated || right.gated,
        left.passed && right.passed && left.setupError == ERROR_SUCCESS && right.setupError == ERROR_SUCCESS,
        left.setupError != ERROR_SUCCESS ? left.setupError : right.setupError
    };
}

[[maybe_unused]] static void PrintPanel(const ModuleResult& result, const bool plain)
{
    if (plain) {
        std::printf("%s:\n", result.title.c_str());
        for (const PanelRow& row : result.rows)
            std::printf("%s | %s\n", row.name.c_str(), row.value.c_str());
        std::putchar('\n');
        return;
    }
    std::size_t width = result.title.size() + 3;
    for (const PanelRow& row : result.rows)
        width = std::max(width, row.name.size() + row.value.size() + 5);
    std::printf("┌─ %s ", result.title.c_str());
    for (std::size_t i = result.title.size() + 3; i < width; ++i) std::printf("─");
    std::printf("┐\n");
    for (const PanelRow& row : result.rows)
    {
        const std::size_t rowWidth = row.name.size() + row.value.size() + 5;
        std::printf("│ %s | %s", row.name.c_str(), row.value.c_str());
        for (std::size_t i = rowWidth; i < width; ++i) std::putchar(' ');
        std::printf(" │\n");
    }
    std::printf("└");
    for (std::size_t i = 0; i < width; ++i) std::printf("─");
    std::printf("┘\n");
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
    const char* name,
    Probe probe,
    ClockLine& clock,
    const unsigned sampleCount,
    Statistics& statistics)
{
    std::vector<std::uint64_t> warmup(4096);
    std::vector<std::uint64_t> samples(sampleCount);
    DWORD exceptionCode = 0;
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
        std::printf("%-16s unavailable (exception 0x%08lX)\n", name, exceptionCode);
        return false;
    }
    statistics = Summarize(std::move(samples));
    return true;
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

static bool TimingSelfCheck()
{
    return AverageAdjustedTiming(1000, 200, 100) == 8 &&
           AverageAdjustedTiming(200, 1000, 100) == -8 &&
           AverageAdjustedTiming(0, 0, 0) == 0;
}

static unsigned ParseSamples(const int argc, char** argv)
{
    if (argc < 2) return 200000;
    const unsigned long value = std::strtoul(argv[1], nullptr, 10);
    return value >= 10000 && value <= 10000000
        ? static_cast<unsigned>(value)
        : 200000;
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--selfcheck") == 0) {
        if (!TimingSelfCheck()) {
            std::fputs("hv-benchmark selfcheck failed\n", stderr);
            return 1;
        }
        std::puts("hv-benchmark selfcheck passed");
        return 0;
    }

    const unsigned sampleCount = ParseSamples(argc, argv);
    const bool requestVmcall = argc >= 3 && std::strcmp(argv[2], "--vmcall") == 0;
    const auto cores = FirstLogicalCpuPerCore();
    if (cores.size() < 2) {
        std::fprintf(stderr, "At least two physical cores are required.\n");
        return 2;
    }

    int cpuid[4]{};
    __cpuid(cpuid, 0);
    const unsigned maximumLeaf = static_cast<unsigned>(cpuid[0]);
    __cpuidex(cpuid, 7, 0);
    if ((static_cast<unsigned>(cpuid[3]) & (1u << 14)) == 0) {
        std::fprintf(stderr, "SERIALIZE is not enumerated by CPUID.7.0:EDX[14].\n");
        return 3;
    }

    const LogicalCpu testCpu = cores[0];
    const LogicalCpu clockCpu = cores[1];
    ClockLine clock{};
    ControlLine control{};

    std::thread clockThread([&] {
        const bool pinned = PinCurrentThread(clockCpu);
        DWORD setupError = pinned ? ERROR_SUCCESS : GetLastError();
        const bool prioritized = pinned && SetThreadPriority(
            GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != FALSE;
        if (pinned && !prioritized) {
            setupError = GetLastError();
        }
        control.setupError.store(setupError, std::memory_order_relaxed);
        control.setupSucceeded.store(
            pinned && prioritized, std::memory_order_relaxed);
        control.ready.store(true, std::memory_order_release);
        while (!control.stop.load(std::memory_order_relaxed)) {
            ++clock.value;
        }
    });

    const bool testPinned = PinCurrentThread(testCpu);
    DWORD testSetupError = testPinned ? ERROR_SUCCESS : GetLastError();
    const bool testPrioritized = testPinned && SetThreadPriority(
        GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != FALSE;
    if (testPinned && !testPrioritized) {
        testSetupError = GetLastError();
    }
    while (!control.ready.load(std::memory_order_acquire)) YieldProcessor();
    if (!testPinned || !testPrioritized ||
        !control.setupSucceeded.load(std::memory_order_relaxed)) {
        control.stop.store(true, std::memory_order_relaxed);
        clockThread.join();
        std::fprintf(
            stderr,
            "Failed to pin or prioritize benchmark threads "
            "(test error %lu, clock error %lu).\n",
            testSetupError,
            control.setupError.load(std::memory_order_relaxed));
        return 4;
    }
    Sleep(50);
    const CpuidRdtscTiming cpuidRdtsc = MeasureCpuidRdtscTiming();

    std::printf(
        "samples=%u test=group%u/cpu%u clock=group%u/cpu%u max_leaf=0x%X\n",
        sampleCount,
        testCpu.group,
        testCpu.number,
        clockCpu.group,
        clockCpu.number,
        maximumLeaf);
    std::printf(
        "cpuid-rdtsc leaf=1 iterations=100 cpuid_avg=%llu "
        "rdtsc_avg=%llu adjusted=%lld\n",
        static_cast<unsigned long long>(cpuidRdtsc.cpuidAverage),
        static_cast<unsigned long long>(cpuidRdtsc.rdtscAverage),
        static_cast<long long>(cpuidRdtsc.adjustedAverage));
    std::fflush(stdout);

    Statistics serialize{};
    Statistics leaf0{};
    Statistics leaf16{};
    Statistics vmcall{};
    const bool haveSerialize = RunProbe(
        "SERIALIZE", MeasureSerialize, clock, sampleCount, serialize);
    const bool haveLeaf0 = RunProbe(
        "CPUID leaf 0", MeasureCpuidLeaf0, clock, sampleCount, leaf0);
    const bool haveLeaf16 = maximumLeaf >= 0x16 && RunProbe(
        "CPUID leaf 16h", MeasureCpuidLeaf16, clock, sampleCount, leaf16);
    const bool haveVmcall = requestVmcall && RunProbe(
        "VMCALL floor", MeasureVmcall, clock, sampleCount, vmcall);

    control.stop.store(true, std::memory_order_relaxed);
    clockThread.join();

    std::puts("probe             mean    trim-mean   p10  median   p90   ratio(trim)");
    auto print = [&](const char* name, const Statistics& value, const bool valid) {
        if (!valid) return;
        const double ratio = haveSerialize && serialize.trimmedMean != 0
            ? value.trimmedMean / serialize.trimmedMean
            : 0;
        std::printf(
            "%-16s %8.2f %12.2f %5llu %7llu %5llu %12.3f\n",
            name,
            value.mean,
            value.trimmedMean,
            static_cast<unsigned long long>(value.p10),
            static_cast<unsigned long long>(value.median),
            static_cast<unsigned long long>(value.p90),
            ratio);
    };
    print("SERIALIZE", serialize, haveSerialize);
    print("CPUID leaf 0", leaf0, haveLeaf0);
    print("CPUID leaf 16h", leaf16, haveLeaf16);
    print("VMCALL floor", vmcall, haveVmcall);
    return 0;
}
