# Intel VMX Global Rendezvous Design

## Goal

Add a bounded-latency Intel VMX-root rendezvous that pauses every running
logical processor around timing-sensitive VM exits, compensates the interval
during which the complete topology is frozen, and resumes processors with a
shared TSC deadline.

The implementation supports both legacy xAPIC MMIO and x2APIC MSR delivery.
It does not claim cycle-identical `VMRESUME` execution: APIC delivery, cache
coherence, and pipeline state make mathematical simultaneity unavailable on
x86. The protocol instead provides a measured common release point and a
bounded final-path skew.

## Scope

This change applies only to the Intel VMX backend. It adds:

- physical LAPIC NMI broadcast through xAPIC or x2APIC;
- an epoch-counted VMX-root barrier;
- mandatory, conditional, and excluded exit classification;
- an eight-exit per-CPU hook-proximity budget;
- common VMCS TSC-offset compensation;
- bounded acquisition and release deadlines;
- a registered Windows NMI callback for NMIs delivered while already in
  VMX-root mode;
- a pre-`VMRESUME` join check for processors interrupted while already in
  VMX-root mode.

The change does not add new VM-exit intercepts, descriptor-instruction
emulation, MTF-based hook execution, CPU hot-add support, or AMD behavior.
Existing controls determine whether RDTSC, RDTSCP, MSR, and descriptor-table
instructions exit. The rendezvous policy applies if those exits occur.

## Architectural constraints

### NMI delivery has no software tag

An NMI broadcast cannot carry a private vector or source identifier. During an
epoch, the owner publishes an expected epoch to each target before broadcast.
The first NMI on that CPU consumes the marker even if delivery occurs after the
CPU joined through another exit or the pre-resume check. An outstanding
marker from a real broadcast therefore intentionally consumes the first later
NMI. Any unrelated physical NMI may satisfy or coalesce with the marker. This
approved first-NMI assumption is an architectural limitation of using NMIs as
the forced-exit mechanism.

After changing the phase to `Claimed`, the owner first drains per-CPU join
guards left by the prior epoch. In xAPIC mode it also waits for ICR delivery
status to become idle while still `Claimed`. It then advances the epoch and
resets the counters, makes `Acquiring` visible, publishes the new expected
epoch to each target, and broadcasts. An xAPIC readiness timeout aborts before
marker arming and before the ICR write, so a no-send failure cannot leave stale
expected markers. NMIs without an outstanding expected marker retain the
existing virtual-NMI reinjection behavior.

### A processor may already be in VMX-root mode

NMI exiting only forces processors that are in VMX non-root mode to exit.
When a target is already executing root-mode code, the host IDT processes the
NMI. The backend registers a nonpageable `KeRegisterNmiCallback` callback that
consumes the expected marker and joins an active epoch when needed, using only
per-CPU lookup, interlocked operations, `VMWRITE`, and `_mm_pause()`. It reports
the internal rendezvous NMI as handled. Every fast and slow path also checks
the active epoch immediately before `VMRESUME` as a defensive fallback for a
coalesced or late observation.

The owner waits for an exact participant count with a bounded deadline. It
never assumes that ICR readiness or the ICR write proves hardware completion
or means the targets are frozen.

### Safe TSC compensation starts after complete arrival

The protocol compensates only the interval beginning after every participant
has joined. Subtracting time beginning at the initial broadcast could move a
late target's guest-visible TSC backward because that target may have executed
guest instructions after the broadcast but before accepting the NMI.

All participants subtract the same delta. Per-CPU deltas are prohibited
because they would create a cross-core TSC discontinuity.

## State ownership

`INTEL_BACKEND_CONTEXT` owns one cache-line-aligned rendezvous record shared by
all Intel processors. The record contains:

- an atomic phase: `Idle`, `Claimed`, `Acquiring`, `Frozen`, `Preparing`,
  `Applying`, `Releasing`, or `Aborting`;
- a monotonically increasing 64-bit epoch;
- the owner processor index;
- the captured participant count;
- atomic arrived, prepared, and applied counts;
- the fully-frozen start TSC;
- the common compensation delta;
- the shared resume TSC;
- acquisition and release deadlines;
- timeout and aborted-epoch diagnostic counters.

The backend also retains the active `HV_STATE` pointer, xAPIC mapping when
needed, APIC mode, NMI-callback handle, calibrated timeout ticks, and
resume-lead ticks. The xAPIC mapping and NMI callback are established at
passive level before VMX launch and released during backend teardown. No
mapping, allocation, registration, logging, dispatcher call, or pageable
operation occurs in the VM-exit path.

Each `INTEL_CPU_CONTEXT` contains:

- the last joined epoch;
- the last prepared epoch;
- the last applied epoch;
- the cumulative guest TSC offset written to that CPU's VMCS;
- the eight-exit hook-proximity budget;
- an internal-NMI marker for the current exit;
- an optional carry-through epoch for future EPT/MTF replay support.

The host stack frame exposes the shared rendezvous record to the assembly
pre-resume check. Assembly-visible offsets receive `C_ASSERT` checks.

## LAPIC broadcast

The backend reads `IA32_APIC_BASE` (`0x1B`) and requires the global APIC-enable
bit. Bit 10 selects x2APIC mode; otherwise the backend uses xAPIC MMIO at the
physical base in bits 51:12.

The ICR low value is:

```text
Destination shorthand, all excluding self  bits 19:18 = 11b
Level, assert                             bit 14    = 1
Trigger mode, edge                        bit 15    = 0
Delivery mode, NMI                        bits 10:8 = 100b

ICR low = 0x000C4400
```

For xAPIC, the owner waits for ICR delivery-status bit 12 to clear and writes
`0x000C4400` to the mapped ICR-low register at offset `0x300`. The destination
register is ignored because destination shorthand is active.

The xAPIC readiness wait occurs while the phase is `Claimed`, before target
markers are armed. A readiness timeout aborts without an ICR write.

For x2APIC, the owner writes the 64-bit value `0x00000000000C4400` to
`IA32_X2APIC_ICR` (`0x830`). The destination field is zero because shorthand
selects all other logical processors.

The owner publishes the epoch, owner, counts, `Acquiring` phase, and target
expected-epoch markers before the ICR write. Arrival counting, rather than
delivery-status polling, proves that the topology has entered the barrier.

## VMCS controls

VMCS setup requires:

- NMI exiting in pin-based controls;
- virtual NMIs in pin-based controls;
- use-TSC-offsetting in primary processor controls.

Startup fails with the existing control-mismatch diagnostics if any required
control is unavailable. `VMCS_TSC_OFFSET` is initialized from the per-CPU
cumulative offset, initially zero.

The current assembly CPUID fast path cannot bypass rendezvous classification.
Mandatory CPUID exits therefore use the common handler. Benchmark-only VMCALL
handling remains independent but must execute the pre-`VMRESUME` join check.

## Exit classification

Classification occurs after basic VM-exit state and vectoring information are
captured, but before timing-sensitive emulation or EPT mutation begins.

### Mandatory rendezvous

These exits always request ownership of a rendezvous epoch:

- CPUID, basic reason 10;
- EPT violation, basic reason 48;
- RDTSC, basic reason 16, when intercepted;
- RDTSCP, basic reason 51, when intercepted;
- RDMSR, basic reason 31, when guest RCX is `IA32_TIME_STAMP_COUNTER` (`0x10`).

If another epoch is active, the processor first joins that epoch, waits for its
release, and then retries ownership for its own mandatory exit. Mandatory exits
are serialized rather than nested.

### Conditional rendezvous

These exits request rendezvous only when the processor's hook-proximity budget
was nonzero at exit entry:

- control-register access, basic reason 28;
- RDMSR or WRMSR, basic reasons 31 and 32, except the mandatory TSC read;
- descriptor-table access, basic reasons 46 and 47, if existing or future VMCS
  controls make those exits reachable.

A hook-related EPT violation that matches an installed hook policy reloads the
local budget to eight. Each subsequent non-excluded VM exit consumes one unit
after classification. Mandatory exits consume budget but remain mandatory.
Excluded exits neither trigger rendezvous nor consume budget. This defines
proximity in deterministic exit count rather than guest-visible time.

### Strictly excluded rendezvous

These exits never create a new epoch:

- external interrupt, basic reason 1;
- monitor trap flag, basic reason 37;
- VMX-preemption timer expiration, basic reason 52.

An MTF exit may finish an EPT epoch explicitly carried across one guest
instruction, but it cannot acquire a new epoch. The current dual-EPT-view hook
implementation does not arm MTF, so current EPT epochs finish within their
original EPT-violation exit.

All unlisted exits use no rendezvous unless a future design explicitly adds
them to one of these classes.

## Rendezvous sequence

### Owner acquisition

1. The triggering processor first joins any already-active epoch.
2. It atomically changes the shared phase from `Idle` to `Claimed`.
3. While `Claimed`, it waits for all prior per-CPU join guards to drain and, in
   xAPIC mode, verifies that the ICR is ready.
4. It increments the epoch and publishes its processor index and metadata.
5. It snapshots processors whose state is `HV_CPU_RUNNING`. The participant
   count includes the owner; arrived count starts at one.
6. It resets the counters, release-publishes `Acquiring`, and publishes the
   expected epoch to every target.
7. It broadcasts the NMI.
8. It spins with `_mm_pause()` until arrived count equals participant count.
9. It records the fully-frozen start TSC and changes phase to `Frozen`.
10. It performs the exit-specific emulation or EPT operation.

Only one owner exists per epoch. A target increments arrived count once by
atomically claiming its per-CPU joined epoch.

### Target join

A target joins from the NMI-exit path, the registered host NMI callback, or the
common pre-resume check:

1. Acquire-load the active phase and epoch.
2. Ignore `Idle` and an epoch already recorded by this CPU.
3. Record the epoch and increment arrived count once.
4. Spin while phase is `Acquiring` or `Frozen`.
5. When phase becomes `Preparing`, record the prepared epoch and increment
   prepared count once without changing the VMCS.
6. When phase becomes `Applying`, subtract the common delta from the local
   cumulative TSC offset and write `VMCS_TSC_OFFSET`.
7. Record the applied epoch and increment applied count once.
8. Wait for `Releasing`, then spin until the shared resume TSC.
9. Return through the interrupted root path or the normal register restoration
   and `VMRESUME` path.

The rendezvous NMI is consumed by the host. It is not added to `PendingNmi` and
is not injected into the guest.

### Compensation and release

After emulation, the owner:

1. Computes `delta = current_tsc - fully_frozen_start_tsc`.
2. Publishes the common delta and changes phase to `Preparing`.
3. Records its prepared epoch, increments prepared count, and waits for every
   participant to reach the pre-apply point.
4. If all participants are prepared, changes phase to `Applying`.
5. Applies the same delta to its own cumulative VMCS TSC offset, records its
   applied epoch, and increments applied count.
6. Waits for every participant to report the offset applied.
7. Publishes a resume TSC at least the calibrated lead interval in the future.
8. Changes phase to `Releasing` with release semantics.
9. Spins until the resume TSC and follows the same final restoration path as
   the targets.
10. The owner returns the phase to `Idle` only after release publication is
   globally visible. The next owner uses a new epoch, so late readers cannot be
   mistaken for participants in the next rendezvous.

The final TSC spin is placed as close as practical to the common `VMRESUME`
sequence. Guest register values are preserved across the wait.

## Timeout and failure behavior

Acquisition and prepared waits use TSC deadlines calibrated during backend
preparation. The initial ceiling is two milliseconds; the release lead is two
microseconds. These are fixed internal constants, not registry configuration.

On acquisition or prepared timeout, before any VMCS offset changes, the owner:

- changes phase to `Aborting`;
- publishes zero compensation;
- releases every processor that did arrive;
- increments the appropriate diagnostic counter;
- continues the triggering exit without claiming synchronized timing.

Targets treat `Aborting` as an immediate release and do not modify their TSC
offset. Timeout handling is fail-open to prevent a permanent VMX-root deadlock
or DPC-watchdog cascade. It performs no VM-exit logging.

Once phase reaches `Applying`, a VMCS-write failure or applied-count timeout
uses the existing fail-stop policy. Fail-open is no longer safe after any
participant changes its offset because continuing with divergent per-CPU TSC
offsets would corrupt guest time.

## Memory ordering

All phase transitions use interlocked operations or explicit barriers:

- `Claimed` excludes another owner while metadata is constructed;
- prior per-CPU join guards drain while `Claimed`, before epoch advancement
  and counter reset;
- owner metadata is released before `Acquiring` becomes observable;
- target expected-epoch markers are armed after `Acquiring` is visible and
  before the ICR write;
- targets acquire phase before reading epoch metadata;
- compensation delta is released before `Preparing`;
- all prepared increments complete before `Applying`;
- each VMCS offset write completes before applied count increments;
- resume TSC is released before `Releasing`;
- epoch changes occur only while ownership excludes another owner.

Aligned 32-bit interlocked phase/count fields and aligned 64-bit epoch/TSC
fields avoid torn accesses. The shared record is isolated from unrelated hot
fields to reduce false sharing.

## Lifecycle

Backend preparation maps xAPIC MMIO when required, registers the NMI callback,
calibrates deadline ticks, initializes the rendezvous record, and stores the
`HV_STATE` pointer. Per-CPU preparation initializes local epochs, offsets, and
hook budgets.

Teardown begins only after common lifecycle rendezvous has stopped every VMX
processor. It verifies that the rendezvous phase is `Idle`, deregisters the NMI
callback, clears state, and unmaps xAPIC MMIO. A non-idle phase during teardown
is an invariant violation and follows the existing fail-stop teardown policy.

CPU hot-add after launch remains unsupported. The participant snapshot uses the
startup CPU array and `HV_CPU_RUNNING` states.

## Verification

Build-time verification covers:

- ICR bit-value assertions;
- assembly-visible structure offsets;
- mandatory VMCS control masks;
- Debug, Release, and Benchmark solution builds;
- WDK static analysis;
- project XML parsing and `git diff --check`.

The portable self-check in `tools/intel-rendezvous-policy-selfcheck.c` covers:

- mandatory, conditional, and excluded classification;
- exact eight-exit budget consumption;
- budget preservation across excluded exits;
- ICR-low encoding;
- required NMI-exiting and virtual-NMI pin-control bits;
- the required TSC-offset primary-control bit.

Bare-metal verification must record the processor model, APIC mode, active
logical-processor count, build hash, and configuration. It must cover:

- xAPIC and x2APIC broadcasts;
- CPUID and EPT mandatory rendezvous;
- conditional CR/MSR behavior inside and outside the eight-exit window;
- excluded external-interrupt, MTF, and preemption-timer behavior;
- concurrent mandatory exits on multiple processors;
- timeout release with an intentionally withheld participant;
- guest TSC monotonicity and cross-core skew before and after compensation;
- repeated start/stop teardown.

Build success alone is not runtime proof for LAPIC delivery, VMCS timing, or
cross-core resume skew.
