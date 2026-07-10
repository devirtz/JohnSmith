# Intel VMX/EPT source map

Normative source: [Intel 64 and IA-32 Architectures Software Developer's
Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
especially Volume 3C. This map targets SDM version 092. Recheck headings when
Intel publishes a new revision.

## Implementation map

| Project concern | Intel SDM location to verify | Code |
| --- | --- | --- |
| VMX availability and lock state | VMX capability reporting; `IA32_FEATURE_CONTROL`; CPUID VMX bit | `IntelSupport` in `src/intel.c` |
| VMXON and VMCS regions | "VMCS Region" and Section 27.11.5, "VMXON Region" | `IntelPrepareCpu`, `IntelStart` |
| Allowed-0/allowed-1 controls | Appendix A, "VMX Capability Reporting Facility" | `IntelAdjustControls`, `IntelSetupVmcs` |
| VMCS field encodings | Chapter 27 VMCS field tables | `VMCS_*` constants in `src/intel.c` |
| Guest/host state checks | Chapter 29, "VM Entries" | `IntelSetupVmcs` |
| Exit reason and qualification | Chapter 30, especially "Recording VM-Exit Information" | `IntelVmExitHandler` |
| Event delivery and reinjection | VM-entry event-injection fields; IDT-vectoring information | `IntelPreserveVectoringEvent`, `IntelInjectException` |
| EPTP and EPT entries | Section 31.3, "Extended Page Tables" | `IntelBuildEpt`, owned-page callbacks |
| EPT/VPID capabilities | `IA32_VMX_EPT_VPID_CAP`, Appendix A | `IntelSupport`, `IntelStart` |
| EPT invalidation | `INVEPT` instruction reference and capability bits | `IntelAsmInvept`, SLAT rendezvous |
| VPID invalidation | `INVVPID` instruction reference and capability bits | `IntelAsmInvvpid` |
| VMX instruction hiding | VM-exit reason definitions and VMX instruction reference | `IntelIsVmxInstructionExit` |
| CPL0 stop hypercall | `VMCALL`, exit qualification, guest CPL derivation | `IntelVmExitHandler`, `IntelAsmStop` |

## Facts that must remain runtime-derived

- VMCS revision ID and VMX region size from `IA32_VMX_BASIC`.
- Required/allowed control bits from the basic or true control MSRs.
- CR0/CR4 fixed masks from `IA32_VMX_CR{0,4}_FIXED{0,1}`.
- EPT page-walk length, memory type, 2 MiB leaf, INVEPT type and VPID
  capabilities from `IA32_VMX_EPT_VPID_CAP`.
- Guest instruction exposure from CPUID must agree with enabled VM-execution
  controls. A transparent CPUID result is not permission to enable unsupported
  controls.
- Physical address fields must reject bits beyond the enumerated physical
  address width.

## Review traps

1. A control bit being desirable does not mean it can be set. Apply the
   allowed-0/allowed-1 masks and explicitly test every required bit.
2. VM-entry failure is not a normal VM exit. Read the VM-instruction error field
   only when the instruction status permits it.
3. EPT violations and EPT misconfigurations are different exits; do not inject a
   guest page fault for an invalid EPT entry.
4. Preserve in-flight IDT-vectoring information before installing a new injected
   event.
5. After reducing EPT permissions or changing a mapping, invalidate the EPT
   context on every logical processor that can use it.
6. VMXON, VMCS, bitmap and host-stack memory has per-CPU ownership. Do not free
   it until that CPU has completed VMXOFF or start rollback.

The project limit of 512 GiB is an implementation ceiling, not an Intel
architectural limit. See [SLAT and invalidation](slat-and-invalidation.md).
