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

static std::string CapturePanel(const ModuleResult& result, const bool plain)
{
    FILE* capture = std::tmpfile();
    assert(capture != nullptr);
    const int saved = _dup(_fileno(stdout));
    assert(saved >= 0);
    assert(_dup2(_fileno(capture), _fileno(stdout)) == 0);

    PrintPanel(result, plain);
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
    assert(Parse({"benchmark", "--all", "--plain", "--vmcall"}, options));
    assert(ModuleBits(options.modules) == ModuleBits(BenchmarkModule::All));
    assert(options.plain && options.vmcall);
    assert(Parse({"benchmark", "10000"}, options));
    assert(options.sampleCount == 10000);

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

    const SoftwareTickTripwire tripwire = DetectSoftwareTickTripwire({1, 1, 2001, 2});
    assert(tripwire.equalOne);
    assert(tripwire.greaterThan2000);
    const SoftwareTickTripwire clear = DetectSoftwareTickTripwire({2, 3, 2000});
    assert(!clear.equalOne && !clear.greaterThan2000);

    const ModuleOutcome failure{false, false, ERROR_SUCCESS};
    const ModuleOutcome setupFailure{false, true, ERROR_ACCESS_DENIED};
    const ModuleOutcome combined = CombineOutcome(failure, setupFailure);
    assert(!combined.passed);
    assert(combined.setupError == ERROR_ACCESS_DENIED);

    assert(AverageAdjustedTiming(1000, 200, 100) == 8);
    assert(AverageAdjustedTiming(200, 1000, 100) == -8);
    assert(AverageAdjustedTiming(0, 0, 0) == 0);

    const ModuleResult panel{"Timer", {{"mean", "12"}}, {false, true, ERROR_SUCCESS}};
    assert(CapturePanel(panel, true) == "Timer:\nmean | 12\n\n");
    const std::string boxed = CapturePanel(panel, false);
    assert(boxed.find("\xE2\x94\x8C") != std::string::npos);
    assert(boxed.find("\xE2\x94\x80") != std::string::npos);
    assert(boxed.find("\xE2\x94\x82") != std::string::npos);
    assert(boxed.find("\xE2\x94\x90") != std::string::npos);
    assert(boxed.find("\xE2\x94\x94") != std::string::npos);
    assert(boxed.find("\xE2\x94\x98") != std::string::npos);
    assert(boxed.find("Timer") != std::string::npos);
    assert(boxed.find("mean | 12") != std::string::npos);
    assert(boxed == "┌─ Timer ───┐\n│ mean | 12 │\n└───────────┘\n");
    return 0;
}
