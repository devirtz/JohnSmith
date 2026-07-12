# Performance and measurement

This document defines what JohnSmith measures, what can be optimized, and what
must not be inferred from the result.

## Current target

| Item | Value |
| --- | --- |
| CPU | Intel Core i5-12400F, Alder Lake |
| Test core | First logical processor of physical core 0 |
| Clock core | First logical processor of a different physical core |
| Samples | 200,000 unless stated otherwise |
| Statistic | Sort, trim 1% from each tail, mean remaining 98% |
| Unit | Cross-core counter ticks, not TSC cycles |

The benchmark validates that both threads are pinned and promoted to
time-critical priority. Results are invalid if that setup fails.

## Software clock

The clock thread repeatedly increments one volatile, 64-byte-aligned `uint64`.
The test core reads that line immediately before and after one probe instruction.
The delta is a coherence-mediated progress counter independent of RDTSC.

This is not a constant-frequency clock. Each reader competes with the writer for
cache-line ownership. Intel's Optimization Reference Manual, Section 22.8.8.1,
describes modified-data sharing as a high-penalty coherence event. Scheduling,
interrupts, cache residency, and ownership timing therefore change the tick rate.
Ratios can move substantially when the SERIALIZE denominator changes even if
the absolute CPUID result is stable.

Adding `PAUSE` to the writer would define a different clock. It may reduce
contention, but its effect must be measured rather than assumed to improve
stability. Never combine paused and unpaused datasets.

## Recorded dataset

Measurements supplied from the i5-12400F:

| State | Probe | Trimmed mean | Ratio to SERIALIZE |
| --- | --- | ---: | ---: |
| JohnSmith loaded | SERIALIZE | 86 | 1.00 |
| JohnSmith loaded | CPUID leaf 0 | 492 | 5.72 |
| JohnSmith loaded | CPUID leaf `0x16` | 1,788 | 20.7 |
| Bare metal | SERIALIZE | 62 | 1.00 |
| Bare metal | CPUID leaf 0 | 71 | 1.15 |
| Bare metal | CPUID leaf `0x16` | 1,218 | 19.7 |

Leaf `0x16` is expensive without a hypervisor and is not a useful estimate of
VM-exit-handler overhead. The leaf-0 cache reduced the virtualized leaf-0 ratio
from approximately 10x to the recorded 5.72x.

No valid VMCALL floor has yet been recorded. Earlier `0xC000001D` results came
from loading the Release driver, where the benchmark-only VMCALL signature is
intentionally absent.

## Cost model

For leaf 0, the measured interval contains:

```text
VM exit
+ assembly dispatch and guard VMREADs
+ cached register writeback
+ guest-RIP VMWRITE
+ VMRESUME
+ software-clock observation noise
```

For uncached leaves, add native root-mode CPUID and feature masking. No handler
optimization can remove the architectural VM exit caused by CPUID in VMX
non-root operation.

The benchmark-only VMCALL path measures a narrower floor:

```text
VM exit + reason/signature dispatch + RIP read/write + VMRESUME
```

It intentionally omits production CPUID guard checks and emulation. It is a
lower-bound experiment, not a production feature.

## Reproducible floor test

```powershell
msbuild JohnSmith.sln /m /p:Configuration=Benchmark /p:Platform=x64
.\tools\load-kdu.ps1 -Config Benchmark

sc.exe qc JohnSmith
Get-FileHash .\build\bin\Benchmark\JohnSmith.sys -Algorithm SHA256
.\build\bin\tools\vmexit-bench.exe 200000 --vmcall
```

The installed service path must resolve to
`build\bin\Benchmark\JohnSmith.sys`. Production builds must continue reporting
VMCALL unavailable.

## Optimization decision rule

1. Measure the VMCALL floor and SERIALIZE in the same run.
2. If `VMCALL/SERIALIZE > 2.5`, the requested ratio is below this experiment's
   transition floor on that machine and configuration.
3. Otherwise, compare leaf 0 directly with VMCALL. That gap is the remaining
   production fast-path and emulation overhead.
4. Optimize only changes whose absolute leaf-0 ticks improve across repeated
   runs; do not select changes solely from a favorable denominator.

## Architectural controls

| Candidate | Decision |
| --- | --- |
| Disable CPUID exiting | Impossible; CPUID exits unconditionally |
| TSC offset | Remains zero |
| PAT/EFER dedicated controls | Required and retained |
| Debug save/load controls | Required and retained |
| Generic MSR lists for PAT/EFER | No benefit; would add list processing |
| ACK interrupt on exit | Not requested; external-interrupt exiting is inactive |
| EPT generation check | Mandatory on every resume path |
| STI/MOV-SS, TF/BTF, IDT vectoring | Mandatory correctness guards |

## Golden Cove considerations

Intel Optimization Reference Manual Volume 1, Section 2.4, documents Golden
Cove's wider pipeline, improved branch prediction, larger prediction structures,
larger instruction TLB, and larger cache/TLB resources. These facts justify
keeping the hot code small, aligned, predictable, and resident.

They do **not** specify:

- a stable branch-predictor state across VM entry or exit;
- a software-visible VMCS-cache residency guarantee;
- a fixed VM-exit/entry cycle count;
- a supported prefetch mechanism for processor-internal VMCS state.

Those behaviors are implementation-dependent. JohnSmith can keep per-CPU
control structures and handler code warm through ordinary locality, but it
cannot claim architectural control over hidden predictor or VMCS caches.
VPID and EPT invalidation controls are the supported mechanisms for translation
state; unnecessary invalidations should be avoided, required invalidations must
not be skipped.

## Research context

The 2012 USENIX paper
[Software Techniques for Avoiding Hardware Virtualization Exits](../static/docs/atc12-avoiding-hardware-virtualization-exits.pdf)
reports historical round-trip latencies from Prescott through Sandy Bridge and
shows that reducing exit frequency can dominate handler micro-optimization.
Those cycle counts are not Alder Lake estimates and must never be substituted
for measurements on the i5-12400F.

The KVM paper [kvm: the Linux Virtual Machine Monitor](../static/docs/ols2007-kvm.pdf)
is useful design context for exit-reason dispatch and host integration. It is
not an Intel or AMD architectural specification.

## Result record

Every retained dataset should include:

```text
date, CPU model/stepping, firmware, Windows build
Hyper-V/VBS/HVCI state, logical CPU mapping
driver configuration, service path, SHA-256
sample count, counter mode, raw summary
mean, trimmed mean, p10, median, p90
```
