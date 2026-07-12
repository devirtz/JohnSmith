#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

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

    std::printf(
        "samples=%u test=group%u/cpu%u clock=group%u/cpu%u max_leaf=0x%X\n",
        sampleCount,
        testCpu.group,
        testCpu.number,
        clockCpu.group,
        clockCpu.number,
        maximumLeaf);
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
