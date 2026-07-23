# Intel VMX/EPT architecture

Implementation map for `src/intel.c`, `src/intel/`, `include/intel.h`,
`include/hook_observe.h`, `src/hook_*.c`, `asm/intel.asm`, and
`asm/hook_dispatch.asm`.

Normative source: [Intel SDM combined volumes, version 092](../../static/docs/325462-092-sdm-vol-1-2abcd-3abcd-4.pdf),
primarily Volume 3C.

## Platform gates

The backend requires VMX, firmware-enabled `IA32_FEATURE_CONTROL`, valid VMX
fixed bits, EPT, a four-level WB EPT pointer, 2 MiB EPT pages, and INVEPT.
VMXON and VMCS regions receive the runtime revision identifier and are placed
below 4 GiB when `IA32_VMX_BASIC` requires it.

Control words are adjusted through their capability MSRs. VM entry and exit
save or restore debug controls, PAT, EFER, and 64-bit host state. Secondary
controls enable EPT and conditionally enable RDTSCP, VPID, INVPCID, and XSAVES.

## VMCS and exit pipeline

Each processor owns a VMXON region, VMCS, MSR bitmap, host stack, cached CPUID
policy, VPID, EPT views, exit history, and hypercall-page registration.

The VM-exit path:

1. Preserves guest GPRs and ABI-volatile XMM registers.
2. Reads the exit reason, qualification, instruction length, and guest state.
3. Restores a valid IDT-vectoring event.
4. Synchronizes the active EPT view generation.
5. Handles CPUID, control-register, MSR, VMX-instruction, EPT, and stop exits.
6. Advances RIP only for completed instruction emulation.
7. Restores state and executes `VMRESUME`.

Unknown exits and invalid transition states use distinct fail-stop signatures.

## NMI rendezvous and TSC compensation

Timing-sensitive exits use an epoch-counted VMX-root barrier. The owner sends
ICR-low `0x000C4400` through xAPIC MMIO offset `0x300` or x2APIC MSR `0x830`.
NMI exiting handles targets in VMX non-root mode; a registered Windows NMI
callback handles targets already in VMX-root mode.

EPT violations, intercepted RDTSC/RDTSCP, and intercepted `RDMSR(0x10)` are
mandatory rendezvous exits. A matched hook EPT violation reloads the per-CPU
budget to exactly eight, and each subsequent non-excluded exit, including
CPUID, decrements it. Mandatory exits still rendezvous and consume budget.
External interrupts, MTF, and the VMX-preemption timer neither start a
rendezvous nor consume budget. CPUID, control-register, other MSR, GDTR/IDTR,
and LDTR/TR exits rendezvous only when the budget was nonzero at exit entry.

The compensated interval starts when every participant has arrived and ends
when owner Finish captures the delta immediately before publishing `Preparing`.
Every participant then prepares before any `VMCS_TSC_OFFSET` write, applies the
same delta, and waits for one future resume TSC. Preparation, VMCS apply coordination, the release lead, and final resume overhead remain guest-visible.
Acquisition and prepared-count timeouts fail open without compensation;
failures after offset application begins fail stop.

While the phase is `Claimed`, the owner drains old per-CPU join guards and
outstanding expected-NMI markers, verifies xAPIC ICR readiness when needed,
and rechecks that lifecycle is still `RUNNING` before advancing the epoch. It
then publishes `Acquiring`, defensively verifies the prior markers remain
drained, arms new expected-epoch markers, and broadcasts. Because NMI carries
no software tag, an unrelated physical NMI can satisfy or coalesce with an
armed marker. This approved first-NMI assumption still requires hardware
validation. The xAPIC preflight occurs before marker arming, preventing a
no-send failure from leaving stale markers.

## CPUID policy

CPUID exits are emulated through the C handler in Debug, Release, and
Benchmark. The policy hides VMX exposure, applies the enabled INVPCID, XSAVES,
and RDTSCP masks, and preserves native topology and OS-dependent results.
Outside hook proximity, benign CPUID still VM-exits through this path but does
not acquire a global rendezvous or broadcast NMIs. Only Benchmark enables the
guarded assembly VMCALL fast path.

This policy change adds no artificial jitter, broad CPUID cache, forced
`CPUID.80000007H:EDX[8]`, assembly CPUID fast path, new `Draining` phase, or
change to NMI, APIC, or timeout behavior.

## EPT and VPID

The identity EPT maps up to 512 GiB. RAM uses WB memory type and non-RAM uses
UC. Uniform ranges use 2 MiB leaves; mixed memory maps, per-page permissions,
and hook pages use 4 KiB page tables.

Each processor owns primary and secondary EPT views with independent
generation counters. Mapping changes increment the backend generation and
rendezvous active processors. The callback uses single-context INVEPT when the
CPU supports it and all-context INVEPT otherwise.

Per-CPU VPIDs start at one. INVVPID is used for emulated CR3 updates when VPID
is active.

## Execute hooks

`IntelHookInstall` validates the target GPA and patch range, prepares the
target page in every processor's primary and secondary views, copies the
original page into contiguous shadow memory, and applies the patch bytes.

The primary view keeps the original page readable and writable without execute
permission. The secondary view maps the shadow page execute-only. EPT
violations switch the active view and retry the guest instruction without RIP
advancement.

Hook policies are stored in a fixed slot table and GPA hash. Installation and
removal serialize mutations, rendezvous all active processors, restore original
PTEs on removal, retire secondary views, and reclaim per-CPU page-table pages
after lock-free readers have crossed an invalidation boundary.

`ObserveHookInstall` builds a trampoline for the displaced instructions and a
fixed-size thunk that enters `AsmHookDispatcher`. `HookObserveDispatch`
increments the hit counter, calls the registered callback, and returns the
trampoline address. Query and list operations expose the hook ID, GPA, cookie,
active state, and hit count.

## Control transport

`DriverEntry` reads `Parameters\HypercallSeed`, restricts the service and
parameters registry keys to SYSTEM and Administrators, and starts the
hypervisor.

The seed derives CPUID subleaves and command identifiers with FNV-1a. A client
registers one page on its selected processor. The worker locks that user page
with an MDL and services these commands:

- register shared page;
- install, remove, query, and list hooks;
- read and write memory;
- invoke the hook probe.

Process-exit notification releases registered pages. Hypervisor teardown stops
the worker, drains rundown protection, removes hooks, resets thunk storage, and
frees processor EPT views.

## Teardown and diagnostics

After lifecycle changes to `STOPPING`, common calls the backend quiesce hook.
Intel waits for rendezvous phase `Idle` and until all outstanding expected-NMI markers are consumed before VMXOFF or per-CPU state changes. The NMI callback remains registered through CPU stop and is deregistered only during later backend teardown. A quiesce timeout follows the fail-stop policy.

Before VMXOFF, the stop path captures current guest control, debug, descriptor,
PAT, EFER, SYSENTER, FS, and GS state. The assembly return path restores that
state after leaving VMX operation.

Debug builds retain bounded per-CPU VM-exit history. EPT violations, EPT
misconfigurations, event collisions, invalidations, and unexpected exits use
separate bugcheck signatures.
