# AMD SVM/NPT architecture

Implementation map for `src/amd.c`, `src/amd/`, `include/amd.h`, and
`asm/amd.asm`.

Normative source: [AMD64 APM Volume 2, publication 24593 revision 3.44](../../static/docs/24593_3.44_APM_Vol2.pdf).

## Required platform state

| Requirement | Project policy | APM location |
| --- | --- | --- |
| SVM | Require `CPUID.80000001H:ECX.SVM` | SVM feature discovery |
| Firmware | Reject `VM_CR.SVMDIS=1` | `VM_CR` |
| SVM revision | Require nonzero `CPUID.8000000AH:EAX` | Section 15.4 |
| ASIDs | Require at least host ASID 0 and one guest ASID | `CPUID Fn8000_000A` |
| Nested paging | Require NPT | Section 15.25 |
| Next RIP | Require NRIPS | VMCB `nRIP` support |
| CET | Reject `CR4.CET=1` | Project safety policy |

Every assembly-visible VMCB field has a compile-time offset assertion. Optional
features are derived from CPUID; no VMCB field or clean bit is inferred from an
Intel encoding.

## VMCB policy

The guest VMCB captures segment, control, debug, syscall, PAT, EFER, and
descriptor state. The backend enables:

- nested paging;
- MSR permission-map interception;
- INVLPGA interception;
- SVM-instruction interception;
- XSETBV interception when XSAVE is present.

CPUID interception is deliberately clear. AMD CPUID therefore retires in guest
mode without a VM exit, and the backend does not advertise a CPUID masking
policy.

I/O interception is disabled until the project owns an explicit port policy.
An allocated all-zero IOPM would add state without changing behavior.

## Entry and exit pipeline

`AmdAsmLaunch` saves the host VMCB, establishes a dedicated host stack, loads
guest state, enables GIF, and executes VMRUN. On exit it saves the guest VMCB,
restores the host VMCB, preserves GPRs and ABI-volatile XMM state, then calls the
C dispatcher.

The handler:

1. Consumes any pending NPT generation into `TLB_CONTROL`.
2. Restores valid `EXITINTINFO` into `EVENTINJ`.
3. Dispatches the exact exit code.
4. Advances to `nRIP` only for successfully emulated instructions.
5. Clears relevant VMCB clean state after software changes cached fields.

Unknown exits and unexpected nested page faults fail-stop with exit information
and RIP preserved for diagnosis.

## ASIDs and TLB invalidation

`CPUID Fn8000_000A.EBX` is an ASID count including reserved host ASID zero.
Valid guest ASIDs are `1..AsidCount-1`. Because each VMCB remains pinned to one
logical processor, numeric ASIDs need not be globally unique.

Live NPT changes increment a shared generation and execute an IPI rendezvous.
CPUID cannot serve as that rendezvous because it is not intercepted. The
callback therefore issues a private signature-gated CPL0 VMMCALL. The exit
handler observes the generation before dispatch and selects:

- `TLB_CONTROL=3` when flush-by-ASID is enumerated;
- `TLB_CONTROL=1` otherwise.

The ASID clean bit is cleared before VMRUN, and the post-rendezvous generation
check proves that every active CPU consumed the update.

## NPT

The identity NPT covers at most 512 GiB. Physical RAM and non-RAM cache flags
are derived from the active PAT. Fully uniform 2 MiB ranges use large entries;
mixed ranges are split into 4 KiB entries.

An NPF represents a second-level translation or permission failure, not a guest
page fault. JohnSmith has no recoverable NPF policy/emulator, so unexpected NPF
exits fail-stop with `EXITINFO1`, `EXITINFO2`, and RIP.

## Events and instruction policy

The handler will not overwrite a valid pending injection. A collision between
an event already selected for `EVENTINJ` and a new exception is a fail-stop
condition.

MSRPM policy virtualizes EFER, VM_CR, and VM_HSAVE_PA while rejecting unsupported
intercepted accesses with `#GP`. Guest SVM instructions and INVLPGA receive
`#UD`; nested SVM is not implemented. XSETBV shares the architecture validation
used by Intel and advances only on success.

## Host state and teardown

VMRUN/VMEXIT automatic state is supplemented by VMSAVE/VMLOAD for state not
covered by the automatic transition. Teardown reconstructs current Windows
control/debug state from the guest VMCB, restores PAT, EFER, and VM_HSAVE_PA,
reenables GIF, and returns to the captured `nRIP`/RSP.

## Speculation-control boundary

[AMD publication 111006-B](../../static/docs/111006-amd-hypervisor-speculation.pdf)
describes separate host and guest `SPEC_CTRL` behavior on VMRUN/VMEXIT for CPUs
that enumerate the facility. JohnSmith does not yet model the corresponding
VMCB state and makes no claim that this older paper applies to every supported
AMD CPU. Any future implementation must first verify current APM fields, CPUID
enumeration, and processor errata.

## Deliberate exclusions

- Native AMD CPUID is not masked.
- I/O virtualization and a device model are absent.
- AMD CET/SSP state virtualization is absent.
- Recoverable NPF emulation and nested SVM are absent.
