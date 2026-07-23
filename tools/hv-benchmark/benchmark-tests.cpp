#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <io.h>
#include <string>

#define main HvBenchmarkProgramMain
#include "benchmark.cpp"
#undef main

extern "C" void MeasureSerialize(volatile std::uint64_t*, std::uint64_t*, unsigned) {}
extern "C" void MeasureCpuidLeaf0(volatile std::uint64_t*, std::uint64_t*, unsigned) {}
extern "C" void MeasureCpuidLeaf16(volatile std::uint64_t*, std::uint64_t*, unsigned) {}
extern "C" void MeasureVmcall(volatile std::uint64_t*, std::uint64_t*, unsigned) {}

static unsigned ModuleBits(const BenchmarkModule modules)
{
    return static_cast<unsigned>(modules);
}

static bool Parse(const std::initializer_list<const char*> arguments, BenchmarkOptions& options)
{
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (const char* argument : arguments) {
        argv.push_back(const_cast<char*>(argument));
    }
    return ParseOptions(static_cast<int>(argv.size()), argv.data(), options);
}

static std::string CapturePanel(
    const std::string& title,
    const std::vector<PanelRow>& rows,
    const bool plain)
{
    FILE* capture = nullptr;
    assert(tmpfile_s(&capture) == 0);
    assert(capture != nullptr);
    const int saved = _dup(_fileno(stdout));
    assert(saved >= 0);
    assert(_dup2(_fileno(capture), _fileno(stdout)) == 0);

    PrintPanel(title, rows, plain);
    std::fflush(stdout);
    assert(_dup2(saved, _fileno(stdout)) == 0);
    _close(saved);

    assert(std::fseek(capture, 0, SEEK_END) == 0);
    const long length = std::ftell(capture);
    assert(length >= 0);
    assert(std::fseek(capture, 0, SEEK_SET) == 0);
    std::string output(static_cast<std::size_t>(length), '\0');
    assert(std::fread(output.data(), 1, output.size(), capture) == output.size());
    std::fclose(capture);
    return output;
}

int main()
{
    BenchmarkOptions options{};
    assert(Parse({"benchmark"}, options));
    assert(options.sampleCount == 200000);
    assert(ModuleBits(options.modules) == ModuleBits(BenchmarkModule::All));
    assert(!options.vmcall);
    assert(!options.plain);

    assert(Parse({"benchmark", "--software-tick", "--tsc-exit"}, options));
    assert(ModuleBits(options.modules) ==
           (ModuleBits(BenchmarkModule::SoftwareTick) |
            ModuleBits(BenchmarkModule::TscExit)));
    assert(Parse({"benchmark", "--software-tick", "--software-tick", "--tsc-exit", "--tsc-exit"}, options));
    assert(ModuleBits(options.modules) ==
           (ModuleBits(BenchmarkModule::SoftwareTick) |
            ModuleBits(BenchmarkModule::TscExit)));
    assert(Parse({"benchmark", "--all", "--plain", "--vmcall"}, options));
    assert(ModuleBits(options.modules) == ModuleBits(BenchmarkModule::All));
    assert(options.plain && options.vmcall);
    assert(Parse({"benchmark", "--tsc-cpuid"}, options));
    assert(ModuleBits(options.modules) == ModuleBits(BenchmarkModule::TscCpuid));
    assert(!options.plain && !options.vmcall);
    assert(Parse({"benchmark"}, options));
    assert(ModuleBits(options.modules) == ModuleBits(BenchmarkModule::All));
    assert(!options.plain && !options.vmcall);
    assert(Parse({"benchmark", "10000"}, options));
    assert(options.sampleCount == 10000);
    assert(Parse({"benchmark", "--plain", "10000000", "--vmcall"}, options));
    assert(options.sampleCount == 10000000);
    assert(!Parse({"benchmark", "10000001"}, options));
    assert(!Parse({"benchmark", "--plain", "10000", "--vmcall", "10000001"}, options));

    assert(!Parse({"benchmark", "--selfcheck"}, options));
    assert(!Parse({"benchmark", "9999"}, options));
    assert(!Parse({"benchmark", "10000001"}, options));
    assert(!Parse({"benchmark", "10000", "20000"}, options));
    assert(!Parse({"benchmark", "--unknown"}, options));

    assert(SoftwareTickPasses(2.499));
    assert(!SoftwareTickPasses(2.5));
    assert(!SoftwareTickPasses(2.501));
    assert(TscExitPasses(999));
    assert(!TscExitPasses(0));
    assert(!TscExitPasses(1000));

    const SoftwareTickTripwire tripwire = DetectSoftwareTickTripwire(1, 2001);
    assert(tripwire.equalOne);
    assert(tripwire.greaterThan2000);
    const SoftwareTickTripwire clear = DetectSoftwareTickTripwire(2, 2000);
    assert(!clear.equalOne && !clear.greaterThan2000);

    const ModuleOutcome setupFailure{false, true, ERROR_ACCESS_DENIED};
    assert(CombineOutcome(0, setupFailure) == ERROR_ACCESS_DENIED);
    assert(CombineOutcome(ERROR_FILE_NOT_FOUND, setupFailure) == ERROR_FILE_NOT_FOUND);
    assert(CombineOutcome(1, setupFailure) == ERROR_ACCESS_DENIED);
    const ModuleOutcome gatedFailure{true, false, ERROR_SUCCESS};
    const ModuleOutcome nonSetupCode{false, true, 1};
    assert(CombineOutcome(0, gatedFailure) == 1);
    assert(CombineOutcome(1, nonSetupCode) == 1);
    assert(CombineOutcome(0, nonSetupCode) == 0);
    const ModuleOutcome nonGatedFailure{false, false, ERROR_SUCCESS};
    assert(CombineOutcome(0, nonGatedFailure) == 0);

    assert(AverageAdjustedTiming(1000, 200, 100) == 8);
    assert(AverageAdjustedTiming(200, 1000, 100) == -8);
    assert(AverageAdjustedTiming(0, 0, 0) == 0);

    const ModuleResult panel{
        "Timer",
        {{"short", "12345"}, {"long-name", "1"}},
        {false, true, ERROR_SUCCESS}};
    assert(CapturePanel(panel.title, panel.rows, true) ==
           "Timer:\nshort | 12345\nlong-name | 1\n\n");
    const std::string boxed = CapturePanel(panel.title, panel.rows, false);
    assert(boxed.find("\xE2\x94\x8C") != std::string::npos);
    assert(boxed.find("\xE2\x94\x80") != std::string::npos);
    assert(boxed.find("\xE2\x94\x82") != std::string::npos);
    assert(boxed.find("\xE2\x94\x90") != std::string::npos);
    assert(boxed.find("\xE2\x94\x94") != std::string::npos);
    assert(boxed.find("\xE2\x94\x98") != std::string::npos);
    assert(boxed.find("Timer") != std::string::npos);
    assert(boxed.find("short     | 12345") != std::string::npos);
    assert(boxed.find("long-name | 1    ") != std::string::npos);
    assert(boxed ==
           "┌─ Timer ───────────┐\n"
           "│ short     | 12345 │\n"
           "│ long-name | 1     │\n"
           "└───────────────────┘\n\n");

    const ModuleResult setup = MakeSetupErrorResult(
        "Setup timer", 7, "probe unavailable");
    assert(setup.title == "Setup timer");
    assert(setup.rows.size() == 2);
    assert(setup.rows[0].name == "setup");
    assert(setup.rows[0].value == "probe unavailable");
    assert(setup.rows[1].name == "result");
    assert(setup.rows[1].value == "SETUP_ERROR code=7");
    assert(!setup.outcome.gated);
    assert(setup.outcome.passed);
    assert(setup.outcome.setupError == 7);

    const ModuleResult cpuidTimer = RunTscCpuidTimer();
    assert(cpuidTimer.title == "TSC-CPUID timer");
    assert(!cpuidTimer.outcome.gated);
    assert(cpuidTimer.outcome.setupError == ERROR_SUCCESS);
    assert(cpuidTimer.rows.size() == 3);
    assert(cpuidTimer.rows[0].name == "leaf / samples");
    assert(cpuidTimer.rows[1].name == "cpuid_avg / rdtsc_avg");
    assert(cpuidTimer.rows[2].name == "adjusted");

    const ModuleResult exitTimer = RunTscExitTimer();
    assert(exitTimer.title == "TSC-exit timer");
    assert(exitTimer.outcome.gated);
    assert(exitTimer.outcome.setupError == ERROR_SUCCESS);
    assert(exitTimer.rows.size() == 3);
    assert(exitTimer.rows[0].name == "samples / sleep / leaf");
    assert(exitTimer.rows[1].name == "average / threshold");
    assert(exitTimer.rows[2].name == "result");
    return 0;
}
