# Modular hv-benchmark Timers Design

## Goal

Refactor `hv-benchmark.exe` into three independently selectable timing modules
with one shared CLI, consistent panel output, exact detector-aligned gates, and
continued execution after module-specific setup failures.

The three modules are:

| Module | Panel title | Measurement |
|---|---|---|
| `software-tick` | `Software-tick timer` | Cross-core live counter around `SERIALIZE`, `CPUID(0)`, optional `CPUID(16h)`, and optional Benchmark VMCALL |
| `tsc-exit` | `TSC-exit timer` | Ten exact Pafish-style `RDTSC; CPUID(0); RDTSC` samples with `Sleep(500)` after every sample |
| `tsc-cpuid` | `TSC-CPUID timer` | One hundred leaf-1 CPUID sandwiches and one hundred RDTSC pairs |

## Non-goals

- Do not change driver, VMCS, rendezvous, TSC-offset, or assembly-probe behavior.
- Do not add Hyper-V-host-specific software-tick thresholds.
- Do not gate `CPUID(16h)`, VMCALL, or TSC-CPUID results.
- Do not add interactive scrolling, JSON, CSV, or dynamic module loading.
- Do not keep or replace the current `--selfcheck` command; remove it.
- Do not claim that any TSC measurement fixes VMAware `VM::TIMER`.

## Architecture

Keep the implementation in `tools/hv-benchmark/benchmark.cpp`. The utility is
small enough that separate translation units or a generic callback registry
would add build changes without improving the three fixed modules.

Use focused functions inside the existing translation unit:

- `ParseOptions` parses the complete CLI into a module bitmask and formatting
  options.
- `RunSoftwareTickTimer` owns the second clock thread and existing assembly
  probes.
- `RunTscExitTimer` implements the exact Pafish force-exit loop.
- `RunTscCpuidTimer` owns the existing leaf-1 timing calibration.
- `PrintPanel` is the only output-framing helper.
- `CombineOutcome` preserves deterministic exit-code precedence while allowing
  every runnable selected module to execute.

No new dependency is required. Add the compiler UTF-8 option to
`tools/hv-benchmark/hv-benchmark.vcxproj` so source literals and redirected
console output use stable UTF-8 bytes.

## CLI

```text
hv-benchmark.exe [samples] [flags]

  samples              default 200000; software-tick only

  --all                select all modules
  --software-tick
  --tsc-exit
  --tsc-cpuid
  --vmcall             include VMCALL in software-tick
  --plain              use text headers instead of box borders
```

Rules:

- Accept at most one numeric positional argument, in any argument position.
- Accept sample counts from `10000` through `10000000`, inclusive.
- Module flags combine with OR semantics.
- If no module flag is present, select all modules.
- `--all` selects all modules even when combined with individual module flags.
- `--vmcall` does not select a module by itself. With no module flags, the
  default all-module selection therefore includes software-tick plus VMCALL.
- Duplicate idempotent flags are accepted.
- Unknown flags, duplicate numeric arguments, malformed numbers, and
  out-of-range sample counts return exit code `2` after printing usage.
- Remove `--selfcheck`; it is an unknown flag after this change.

## Shared Execution Flow

Run selected modules in this fixed order:

1. Software-tick.
2. TSC-exit.
3. TSC-CPUID.

Discover the first logical CPU from each physical core once. Every timer uses
the first discovered CPU as the test CPU. Pin and raise the main test thread to
`THREAD_PRIORITY_TIME_CRITICAL` before running timers. Software-tick alone
requires a second physical core and creates the clock thread on it.

A module-specific setup failure records an error outcome and does not stop
other selected modules whose prerequisites remain available. A common failure
that makes every remaining measurement impossible still produces an outcome
for each affected module rather than silently omitting it.

## Software-tick Timer

Retain the existing two-thread design, warmup, sample collection, one-percent
trim, exception wrapper, and four probes:

- `SERIALIZE` is required.
- `CPUID leaf 0` is required.
- `CPUID leaf 16h` runs only when the maximum basic leaf is at least `0x16`.
- `VMCALL floor` runs only with `--vmcall`.

Print the full diagnostic columns for every executed probe:

```text
probe | mean | trim-mean | p10 | median | p90 | ratio(trim)
```

The only v1 gate is:

```text
FAIL iff leaf0.trimmedMean / serialize.trimmedMean >= 2.5
```

Print the gate as:

```text
software-tick leaf0_ratio=<value> threshold=2.5 result=PASS|FAIL
```

Also print report-only tripwire data:

```text
trim_serialize=<value> trim_leaf0=<value>
tripwire_eq1=yes|no tripwire_gt2000=yes|no
```

`tripwire_eq1` is `yes` when either required trim mean equals `1`.
`tripwire_gt2000` is `yes` when either required trim mean exceeds `2000`.
Neither tripwire affects the v1 gate or process exit code.

Leaf `16h` and VMCALL ratios are diagnostic only. An unsupported leaf `16h` is
printed as unavailable and is not a setup error. A requested VMCALL or an
enumerated probe that raises an exception records exit code `7`.

## TSC-exit Timer

Run on the pinned, high-priority test thread. Match `pafish/cpu.c` rather than a
serialized calibration:

```text
repeat 10 times:
    before = RDTSC
    CPUID leaf 0
    after = RDTSC
    sum += after - before
    Sleep(500)
average = sum / 10
```

Do not add `LFENCE`, `MFENCE`, `RDTSCP`, `SERIALIZE`, baseline subtraction, trim,
or outlier removal. Use unsigned 64-bit accumulation and integer division.

The gate is:

```text
PASS iff 0 < average < 1000
```

The panel reports sample count, sleep duration, leaf, average, threshold, and a
`PASS` or `FAIL` result row.

## TSC-CPUID Timer

Move the current `MeasureCpuidRdtscTiming` behavior into its own module without
changing its measurement:

- One hundred `RDTSC; CPUID(1); RDTSC` samples.
- One hundred back-to-back RDTSC pairs.
- Integer `cpuid_avg` and `rdtsc_avg`.
- Signed `adjusted = cpuid_avg - rdtsc_avg`.

Print leaf, iteration counts, both averages, and adjusted timing. This module
has no result row and never contributes exit code `1`.

## Output

`PrintPanel(title, rows, plain)` receives preformatted name/value rows.

Default output uses static UTF-8 box borders. Call
`SetConsoleOutputCP(CP_UTF8)` before printing. Panel width is computed from the
ASCII title and row content, so the three panels remain aligned without fixed
terminal assumptions.

`--plain` prints the same titles, rows, values, unavailable messages, and result
lines. Only the surrounding frame changes to a simple text header and unboxed
`name | value` rows.

## Outcomes and Exit Codes

Each module returns an outcome containing its gate state, setup error code, and
rows. Execute every selected module before choosing the process exit code.

| Code | Meaning |
|---:|---|
| `0` | Every gated module that ran passed; no setup errors |
| `1` | At least one gated module failed; no setup errors |
| `2` | Invalid CLI |
| `3` | Insufficient CPU topology |
| `4` | Test-thread affinity or priority setup failed |
| `5` | Software-tick clock-thread affinity or priority setup failed |
| `6` | Required CPU capability unavailable |
| `7` | Requested or required probe raised an exception |

Setup errors override gated failures. If multiple setup errors occur, return
the first setup error encountered in the fixed module order.

## Verification

Do not add a replacement self-check command. Verification uses build and smoke
runs:

1. Build `tools/hv-benchmark/hv-benchmark.vcxproj` in Release with warnings as
   errors.
2. Run `hv-benchmark.exe --tsc-cpuid` and confirm no clock thread requirement.
3. Run `hv-benchmark.exe --tsc-exit` and confirm ten samples with ten 500 ms
   sleeps and the exact `0 < average < 1000` result.
4. Run `hv-benchmark.exe 10000 --software-tick --plain` and confirm full
   statistics, ratio gate, and report-only tripwires.
5. Run a combined selection and confirm all panels print even when a gated
   module fails.
6. Run invalid CLI cases and verify code `2`.
7. On the target host with the Benchmark driver loaded, run software-tick with
   `--vmcall` and record the VMCALL floor without gating it.
8. Run the exact Pafish force-exit check separately; the benchmark output is a
   matching diagnostic, not proof that Pafish passes.

Update `docs/build-and-test.md` for the new CLI and remove documentation that
instructs users to run `hv-benchmark.exe --selfcheck`.
