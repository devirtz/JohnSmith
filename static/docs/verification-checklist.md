# Correctness verification checklist

Use this checklist for changes to virtualization behavior. Record failures and
unsupported cases; absence of a crash is not a passing result.

## Source audit

- [ ] Vendor, document ID, revision, and section/table title are recorded.
- [ ] The cited text was read in the primary document, not copied from a sample.
- [ ] Constants and structure offsets match the cited revision.
- [ ] Optional capabilities are detected at runtime before use.
- [ ] Reserved, MBZ/SBZ, alignment and physical-width rules are enforced.
- [ ] CPUID exposure is consistent with guest instruction/control behavior.
- [ ] The evidence label is honest: verified, inferred, or untested.

## Build/static checks

- [ ] Debug x64 builds warning-free.
- [ ] Release x64 builds warning-free.
- [ ] MASM/C prototypes and assembly-consumed offsets agree.
- [ ] Code analysis reports no new warnings.
- [ ] All failure paths release only resources they own, in reverse order.
- [ ] No binaries, manuals, signing keys, dumps, or build output are staged.

## Lifecycle checks

- [ ] Unsupported vendor and active-hypervisor paths fail before allocation.
- [ ] Every active processor gets a unique, stable `HV_CPU` and vendor context.
- [ ] No Windows routine listed as reserved for system use is treated as a
      supported driver contract; the current `MmGetPhysicalMemoryRanges` use is
      tracked as unresolved until replaced or otherwise justified.
- [ ] A forced failure on each CPU position rolls back all CPUs cleanly.
- [ ] Repeated start/stop and unload cycles complete without leaked state.
- [ ] Stop cookie/signature/CPL checks reject every non-shutdown hypercall.
- [ ] No per-CPU memory is freed before its CPU leaves VMX/SVM operation.

## Guest transparency checks

- [ ] CPUID leaves/subleaves match native results except intentional hiding.
- [ ] Normal exceptions, NMIs and in-flight vectoring events survive VM exits.
- [ ] CR0/CR4 reads expose guest-visible values while hardware-required bits stay
      active.
- [ ] MSR and I/O intercepts either emulate correctly or preserve native fault
      behavior.
- [ ] Unsupported VMX/SVM instructions produce the intended guest exception.
- [ ] XCR/XSS and optional instruction state is preserved on capable processors.

## SLAT checks

- [ ] Lowest, highest, unaligned, overflowed, non-owned and `>= 512 GiB`
      addresses are tested.
- [ ] Initial 2 MiB mappings and 4 KiB splits translate to the same physical page.
- [ ] Read/write/execute query exactly matches the installed leaf.
- [ ] Permission reductions invalidate every active CPU before resumption.
- [ ] Intel exercises supported INVEPT fallback; AMD exercises the selected
      ASID/TLB-control path.
- [ ] MMIO is never accidentally assigned normal-RAM cache attributes.

## Hardware evidence

Run Intel and AMD separately on disposable, kernel-debugger-enabled hosts. For
each result record:

```text
CPU model/stepping:
Microcode/BIOS:
Windows build:
VBS/Hyper-V state:
WDK/toolset:
Signing mode:
Backend:
Test duration and workload:
Debugger/bugcheck/VM-instruction-error evidence:
Result: pass | fail | untested
```

Do not label the project hardware-verified until both backend sections have real
evidence. A virtual machine is useful for negative tests but does not replace
bare-metal validation when nested virtualization is rejected.
