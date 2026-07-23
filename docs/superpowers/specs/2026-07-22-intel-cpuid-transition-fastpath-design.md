# Intel CPUID Transition Fast Path Design

## Goal

Reduce the Intel leaf-0 `CPUID` VM-exit path enough for Pafish
`cpu_rdtsc_force_vmexit` to report an average strictly between 0 and 1000 TSC
ticks, without changing guest TSC values on the CPUID hot path or weakening the
existing multi-CPU rendezvous freeze compensation.

## Constraints

Intel VMX makes `CPUID` an unconditional VM exit. Transparent emulation must
return the cached leaf-0 registers and advance guest RIP before `VMRESUME`.
There is no VM-execution control that permits native leaf-0 execution.

The design does not rewrite `VMCS_TSC_OFFSET` for CPUID. Prior full and capped
TSC-hide experiments caused time rewind, freezes, and monotonic-clock failures
on the target Windows host. `VMCS_TSC_OFFSET` remains owned by VMCS setup and
the existing rendezvous freeze compensation.

## Transition-State Policy

Keep hardware save/load enabled for DR7 and `IA32_DEBUGCTL`. VM exit resets the
live debug state, so removing those controls would lose guest breakpoints and
debug tracing.

Treat PAT and EFER independently. Each state uses one all-or-none transition
triplet:

- PAT: VM-exit save PAT, VM-exit load PAT, and VM-entry load PAT.
- EFER: VM-exit save EFER, VM-exit load EFER, and VM-entry load EFER.

Read the selected true or legacy VM-exit and VM-entry capability MSRs once. If
all three controls for a state permit zero, leave the entire triplet disabled.
The live MSR then persists across the root/non-root transition. This is valid
for this same-OS VMM because the Windows root handler and Windows guest use the
same PAT and EFER state.

If any control in a triplet is forced to one, request all three controls. This
prevents partial hardware management from loading a stale VMCS value. Existing
control validation still rejects a processor that cannot support the required
fallback triplet.

Keep the guest and host PAT/EFER VMCS fields initialized in both modes. They are
used by the hardware-managed fallback and remain harmless in persistent mode.

## Shutdown Capture

`IntelCaptureStopState` must use the actual configured controls:

- Hardware-managed state is captured from the corresponding guest VMCS field.
- Persistent state is captured with `RDMSR` from the live logical processor.

On a 64-bit Windows VM exit, the host-address-space-size control establishes
EFER.LMA and EFER.LME as one. Those bits match the 64-bit guest; the remaining
EFER bits and PAT persist when their load controls are disabled. The captured
values therefore restore the live Windows state after `VMXOFF`.

## Leaf-0 Assembly Micropath

Keep the existing decision sequence:

1. Require `FastPathEnabled`.
2. Read the basic VM-exit reason and require `CPUID`.
3. Require guest EAX leaf 0.
4. Require a valid rendezvous phase pointer and the idle phase.
5. Read guest RIP, add the fixed two-byte `CPUID` length, and write guest RIP.
6. Load the cached leaf-0 EAX, EBX, ECX, and EDX values.
7. Restore preserved guest registers and execute `VMRESUME`.

Reduce leaf-0 scratch preservation to `r8` and `r9`, and address the fixed host
frame directly from RSP. Do not clobber CPUID output registers until the guest
RIP `VMWRITE` succeeds. Any probe failure must restore the original guest GPRs
and enter the existing C slow path.

The Benchmark-only VMCALL path may save `r10` and `r11` after exit-reason
classification. Its extra preservation must not remain on the production
leaf-0 path.

## Rendezvous and TSC Invariants

The change does not modify rendezvous phases, ownership, NMI delivery, hook
budgets, TSC delta calculation, or `IntelRendezvousApplyOffset`. Leaf-0 CPUID
uses the micropath only while the rendezvous phase is idle; all other cases use
the existing C handler.

No new per-CPU TSC watermark, residual budget, offset pointer, hide flag, TSC
scaling, or CPUID-time compensation is added. Guest TSC monotonicity continues
to depend only on the existing hardware TSC behavior and rendezvous offset
compensation.

## Validation

Add a portable self-check for the transition policy before changing production
code. It must prove that:

- a triplet remains disabled when every control permits zero;
- any forced-one control selects the complete fallback triplet;
- PAT and EFER decisions are independent.

Then verify:

1. The new self-check fails before implementation and passes afterward.
2. Existing Intel rendezvous policy and benchmark self-checks pass.
3. Debug, Release, and Benchmark x64 configurations build without warnings.
4. `git diff --check` passes.
5. `HideRoot`, `TscOffsetPtr`, `LastGuestTsc`, and `TSC_HIDE` remain absent.
6. The same-host Benchmark build records `VMCALL floor` and leaf-0 CPUID before
   Pafish testing.
7. Pafish runs its exact ten-sample `RDTSC; CPUID(0); RDTSC; Sleep(500)` rule
   and reports `0 < average < 1000`.
8. Cross-core TSC monotonic and libuv workloads show no backward timestamp,
   freeze, or `new_time >= loop->time` assertion.

If the optimized Benchmark `VMCALL floor` remains at or above 1000 TSC ticks,
the target is below the host's measured transition floor. Stop there rather
than reintroducing dynamic TSC-offset concealment or guest-code patching.

## Scope

VMAware `VM::TIMER` uses a software-tick cross-core signal and is not a TSC
test. This work does not attempt to change or claim improvement for that probe.

No guest instruction patching, EPT-based CPUID replacement, broad CPUID cache,
new hypercall surface, or AMD behavior change is included.
