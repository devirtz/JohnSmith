# Intel VMX/EPT architecture

Implementation map for `src/intel.c`, `src/intel/`, `include/intel.h`, and
`asm/intel.asm`.

Normative source: [Intel SDM combined volumes, version 092](../../static/docs/325462-092-sdm-vol-1-2abcd-3abcd-4.pdf),
primarily Volume 3C. Section numbers below refer to that revision.

## Required platform state

| Requirement | Project policy | SDM location |
| --- | --- | --- |
| VMX support | Require `CPUID.01H:ECX.VMX` | VMX capability discovery |
| Firmware enable | Require locked `IA32_FEATURE_CONTROL` with VMX enabled outside SMX | VMX enablement |
| VMX regions | Allocate below 4 GiB when required; write runtime revision ID | VMCS and VMXON regions |
| EPT | Require page-walk length 4, WB EPTP, 2 MiB pages, and INVEPT | Section 31.3; `IA32_VMX_EPT_VPID_CAP` |
| natively exposed features | Enable RDTSCP, INVPCID, XSAVES, and VPID only when controls permit | Appendix A capability MSRs |
| CET | Permit `CR4.CET=1` only while `IA32_S_CET=0` | CET state and VM-entry/exit controls |

All control words are adjusted with the low/high halves of the relevant
capability MSR. Capability-mandated one bits are valid even when they were not
part of the requested functional set. Required behaviors are checked after
adjustment.

## Active controls

| Class | Required behavior |
| --- | --- |
| Primary execution | Secondary controls and MSR bitmaps |
| Secondary execution | EPT; optional RDTSCP, VPID, INVPCID, and XSAVES |
| VM exit | 64-bit host, save debug controls, save/load PAT, save/load EFER |
| VM entry | IA-32e guest, load debug controls, load PAT, load EFER |

External-interrupt exiting and acknowledge-interrupt-on-exit are not requested.
The VM-exit/entry MSR-list counts are zero. PAT and EFER use their dedicated
controls; moving them to generic MSR lists would add list processing and would
not remove their required guest/host transition semantics.

## VMCS ownership

The guest/host masks own only VMX-fixed CR0/CR4 positions plus `CR4.VMXE`:

```text
CR0 mask = IA32_VMX_CR0_FIXED0 | ~IA32_VMX_CR0_FIXED1
CR4 mask = IA32_VMX_CR4_FIXED0 | ~IA32_VMX_CR4_FIXED1 | CR4.VMXE
```

Read shadows preserve the values visible to the guest. `FIXED0 ^ FIXED1` is
not an ownership mask; it selects flexible positions.

`VMCLEAR` precedes the initial VMCS population. Guest interruptibility and
pending-debug fields are initialized once before `VMLAUNCH`, then updated only
when exit semantics require it.

## Exit pipeline

The slow path performs these operations in order:

1. Preserve guest GPRs and ABI-volatile XMM0-XMM5.
2. Read and validate the raw exit reason, instruction length, and guest RIP.
3. Restore a valid IDT-vectoring event for the next entry.
4. Synchronize EPT generation before dispatch.
5. Emulate the classified exit or fail-stop for an unsupported reason.
6. Consume interruptibility/debug state and advance guest RIP when appropriate.
7. Restore registers and execute `VMRESUME`.

VM-entry failure bit 31 in the raw exit reason is checked before reason-specific
handling. Unknown reasons are not converted into a generic exception because
qualification, retry, and RIP semantics are exit-specific.

## CPUID fast path

CPUID unconditionally exits in VMX non-root operation (Section 28.1.2). There
is no execution control that disables that exit.

Release builds attempt an assembly fast path after saving four scratch
registers. Direct resume is allowed only when all of the following hold:

- EPT generation is current.
- No valid IDT-vectoring event exists.
- STI and MOV-SS blocking are clear.
- `RFLAGS.TF=0`.
- The guest is executing in a 64-bit code segment (`CS.L=1`).
- The leaf/subleaf does not require a project feature mask.

Leaf 0 is returned from a per-CPU cache. Other eligible leaves execute native
root-mode CPUID. Any failed guard reaches the C path before guest-visible state
is changed.

Feature masks are prepared during VMCS setup:

- leaf 1 hides VMX and other policy-controlled features;
- leaf 7 subleaf 0 reflects INVPCID availability;
- leaf `0xD` subleaf 1 reflects XSAVES availability;
- leaf `0x80000001` reflects RDTSCP availability.

Topology, APIC ID, OSXSAVE, XCR0, and XSS-dependent results are not globally
cached.

## Events, debug state, and RIP

VM exits clear the valid bit in the VM-entry interruption-information field
(Section 27.8.3). The handler therefore reconstructs only events reported by
the IDT-vectoring fields.

Successful instruction emulation consumes saved STI/MOV-SS blocking as required
by Section 29.7.1. If `RFLAGS.TF=1` and `IA32_DEBUGCTL.BTF=0`, it sets pending
debug `BS` as required by Section 29.7.3. Existing pending causes are preserved.

RIP advancement follows Section 29.3.1.4:

- use 64-bit arithmetic only when `CS.L=1`;
- otherwise truncate to EIP so the result wraps at 4 GiB;
- never leave guest RIP bits 63:32 set for compatibility or legacy code.

## Control-register emulation

CR0 and CR4 writes combine guest-visible values with VMX fixed-bit constraints.
MOV-to-CR3 normally does not exit, but the handler remains correct if a
capability-mandated control causes interception.

With `CR4.PCIDE=1`, source bit 63 suppresses invalidation and is stripped before
VMWRITE because architectural CR3 bit 63 remains zero. Without PCID, source bit
63 produces guest `#GP`. Ordinary emulated CR3 loads issue single-context
INVVPID when VPID is active.

## EPT and VPID

The identity EPT covers at most 512 GiB. RAM uses WB memory type; non-RAM uses
UC. Mixed 2 MiB regions are split into 4 KiB PTEs. EPT violations and EPT
misconfigurations are distinct fail-stop conditions; an EPT violation is not a
guest page fault.

Each pinned CPU receives a nonzero processor-local VPID when supported. Live
EPT changes increment a shared generation and rendezvous every active CPU.
Each CPU performs single-context INVEPT when supported, otherwise all-context
INVEPT, before accepting the new generation.

## MSR and nested-VMX policy

The bitmap intercepts `IA32_FEATURE_CONTROL`, VMX capability MSRs, and the
supervisor-CET policy MSR. VMX is hidden from CPUID; intercepted VMX capability
reads and unsupported accesses receive `#GP`. Guest VMX instructions receive
`#UD`. Nested VMX is not implemented.

## Teardown and diagnostics

Before VMXOFF, the stop path captures current guest control, debug, descriptor,
PAT, EFER, SYSENTER, FS, and GS state. VM exit sets GDTR/IDTR limits to `0xFFFF`
(Section 30.5.2), so both bases and limits are restored after VMXOFF.

Debug builds retain a bounded per-CPU exit history. Unexpected exit, event
collision, EPT violation, and invalidation failures use distinct bugcheck
signatures. Release builds remove history collection from the hot path.

## Deliberate exclusions

- TSC offset remains zero; RDTSC/RDTSCP execute natively.
- Post-boot INIT is dropped because processor reset/hotplug is unsupported.
- Root supervisor CET and shadow-stack state are not implemented.
- Recoverable EPT-policy emulation and nested VMX are not implemented.
