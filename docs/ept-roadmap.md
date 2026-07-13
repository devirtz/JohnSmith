# EPT and runtime-control roadmap

Audited: **2026-07-13** against the current first-party source and
`JohnSmith.vcxproj`.

Status labels:

- **[IMPLEMENTED (code)]** — reachable, project-wired code exists. Hardware
  validation is a separate gate.
- **[PARTIAL]** — some code exists, but the full item is not implemented.
- **[NOT IMPLEMENTED]** — no end-to-end implementation was found.
- **[BLOCKED BY DEFECT]** — code exists but cannot safely provide the claim.
- **[NOT VERIFIED]** — the required test/hardware evidence is absent.

The concise cross-project audit and release blockers are in
[`implementation-status.md`](implementation-status.md). A stage is not complete
merely because one symbol or prototype exists.

Normative references:

- Intel 64 and IA-32 SDM rev. 092:
  [`static/docs/325462-092-sdm-vol-1-2abcd-3abcd-4.pdf`](../static/docs/325462-092-sdm-vol-1-2abcd-3abcd-4.pdf),
  especially Vol. 3A exception delivery and Vol. 3C VM exits, VM-entry event
  injection, EPT, INVEPT, and VMCS controls.
- Existing project interpretation:
  [`architecture/intel-vmx.md`](architecture/intel-vmx.md).
- AMD64 APM 3.44:
  [`static/docs/24593_3.44_APM_Vol2.pdf`](../static/docs/24593_3.44_APM_Vol2.pdf)
  and [`architecture/amd-svm.md`](architecture/amd-svm.md).

## Findings that constrain the design

1. An EPT violation is not a guest page fault. Only an explicit policy may
   synthesize `#PF`, with a policy-derived PFEC and valid GLA.
2. Guest CR2 is not a VMCS field. The Intel slow path now snapshots/restores a
   per-vCPU `GuestCr2`, but no synthetic-`#PF` policy uses it yet.
3. Execute-only EPT is capability-dependent. Write-without-read remains
   unsupported by the current API.
4. One EPT cannot map one GPA to two HPAs simultaneously; dual-view hooks need
   separate EPT roots or EPTP switching.
5. Runtime split/query/set/merge primitives substantially exist, but their
   requested test matrix and bare-metal evidence do not.
6. EPT misconfiguration and unknown exits remain fail-stop until an exact
   recovery policy exists.
7. External-interrupt exiting is not requested. The checked-in one-slot
   interrupt/NMI window scaffold has no producer and is not a complete queue or
   NMI virtualization design.
8. XSS is not XCR0. Guest XSETBV is safely rejected on both backends; guest
   XCR0 virtualization is absent.
9. AMD CPUID is intercepted and emulated from native results. The policy clears
   hypervisor, VMX, and SVM exposure and zeros the SVM capability leaf. AMD
   hardware validation remains required.
10. Private host page tables require a mapping-lifetime design; a shallow PML4
    copy would not provide the claimed isolation.
11. Physical-memory IOCTL terminology must distinguish GPA, current HPA, and
    guest VA once remapping exists.

## Stage 0 — contracts, capability gates, and tests

1. **[PARTIAL]** Strong types exist for permissions and EPT roots, but GPA, HPA,
   and hook IDs still use generic integer/`PHYSICAL_ADDRESS` types.
2. **[NOT IMPLEMENTED]** The backend interface contains hook operations even
   though only Intel implements them; the requested Intel-specific root/view
   separation is absent.
3. **[PARTIAL]** Hook installation follows hook-lock → root-lock order and
   rendezvous occurs after lock release. The contract is not encoded or tested.
4. **[PARTIAL]** Permission-split retirement waits for invalidation. Hook
   retirement does not force vCPUs off the secondary root and is unsafe.
5. **[PARTIAL]** Patch and SLAT address checks exist. Canonical GVA, page-walk,
   user-buffer, and physical-width coverage is incomplete.
6. **[PARTIAL]** EPT qualification decoding is factored into a helper. PFEC,
   DR qualification, event priority, and guest-walk helpers/tests are absent.
7. **[PARTIAL]** Architecture documents and pinned manuals exist, but this audit
   found source/document contradictions; citations and status must be reviewed
   with each policy change.

## Stage 1 — complete and harden the existing EPT primitives

Files: `src/intel/intel_slat.c`, `src/intel/intel_internal.h`,
`include/hv.h`, `src/introspection.c`.

1. **[PARTIAL]** `IntelSplit2MbLocked` exists, allocates before publication,
   clones PFNs, and preserves R/W/X plus memory type. Ignored/PAT,
   suppress-VE, and user-execute capability policy is not implemented.
2. **[IMPLEMENTED (code)]** Intel query, access mutation, and remap select the
   containing 4 KiB page from an arbitrary byte GPA. HPA remap targets remain
   page-aligned.
3. **[PARTIAL]** `IntelQueryOwnedPageAccess` handles large and split mappings.
   The requested boundary/out-of-range/runtime-split unit tests do not exist.
4. **[PARTIAL]** `IntelSetOwnedPageMapping` atomically updates PFN and access
   under the root lock, but it is Intel-internal, unused by a caller, and
   untested.
5. **[IMPLEMENTED (code)]** Mutation publishes under the root lock, releases
   it, increments the generation, performs the all-CPU rendezvous, and runs
   INVEPT. **[NOT VERIFIED]** on hardware.
6. **[IMPLEMENTED (code)]** `PERMISSION` splits merge only when all 512 PTEs
   are contiguous and identical in the attributes the implementation tracks;
   retirement occurs after invalidation. **[NOT VERIFIED]** by focused tests.
7. **[PARTIAL]** `MIXED_MEMORY_MAP`, `PERMISSION`, and `HOOK` reasons exist.
   There are no hook reference counts, and a region retagged `HOOK` is never
   retagged or merged after its last hook is removed.

Acceptance gate: **[NOT VERIFIED]**. The startup introspection exercise covers a
single permission change. There is no committed evidence for the complete
split/remap/merge, memory-type, X-only, or all-core mutation matrix.

## Stage 2 — classify EPT exits instead of globally injecting `#PF`

Files: `src/intel/intel_exit.c`, `src/intel/intel_internal.h`,
`include/intel.h`, `asm/intel.asm`.

1. **[PARTIAL]** `IntelDecodeEptViolation` decodes access/EPT/GLA flags and the
   handler reads GLA only when valid. Diagnostics do not provide the full
   requested structured log.
2. **[IMPLEMENTED (code)]** `IntelHookLookup` uses an even/odd sequence around
   slot mutation and accepts only stable snapshots. Removal keeps slot contents
   alive until every vCPU retires the secondary view.
3. **[PARTIAL]** Known hook transitions switch EPTP and retry without advancing
   RIP. Stage 4 safety defects prevent completion.
4. **[PARTIAL]** The slow assembly path snapshots/restores per-vCPU guest CR2,
   and injection constants exist. No intentional page-fault policy, PFEC
   builder, call to `IntelSetGuestCr2`, or `#PF` injection path exists.
5. **[IMPLEMENTED (code)]** Unknown EPT violations use the dedicated decoded
   fail-stop path.
6. **[PARTIAL]** EPT misconfiguration has a dedicated fail-stop signature with
   CPU/GPA/RIP. It does not dump EPTP or relevant walk entries as requested.

Acceptance gate: **[NOT VERIFIED]**. No focused tests demonstrate instruction
versus data classification, invalid-GLA handling, CR2 isolation, or retry RIP.

## Stage 3 — event and exit robustness, one reason at a time

1. **[PARTIAL]** Intel classifies benign/contributory/page-fault exceptions and
   promotes the architectural double-fault pairs. Serial event preservation,
   shutdown semantics, instruction-length cases, and hardware tests remain
   open.
2. **[NOT IMPLEMENTED]** Interrupt/NMI window injection has dead scaffold code,
   not a complete feature. No producer calls it. If the NMI request function
   were called, it could enable NMI-window exiting without NMI exiting and
   virtual NMIs.
3. **[IMPLEMENTED (code)]** Unknown exits remain fail-stop.
4. **[PARTIAL]** MOV-DR decode/emulation code exists for forced exits, but the
   intercept is not requested and host debug-register ownership is unproven.
5. **[PARTIAL]** RDTSC/RDTSCP fallback emulation exists; RDTSCP exposes host
   `TSC_AUX`. Normal controls do not request RDTSC exiting.
6. **[NOT IMPLEMENTED]** No defined or tested cross-core TSC synchronization
   policy exists.
7. **[PARTIAL]** Guest XSETBV is rejected with `#GP`, preventing host XCR0
   corruption. Guest XCR0/IA32_XSS virtualization is not implemented.

## Stage 4 — dual-EPT code hooks

Files: `src/intel/intel_hook.c`, `src/intel/intel_internal.h`,
`src/intel/intel_slat.c`, `src/intel/intel_exit.c`, `src/intel.c`.

1. **[PARTIAL]** `INTEL_EPT_ROOT` owns PML4/PDPT/PD/split-list/lock/EPTP
   metadata, and primary/secondary roots are separate. The requested reference
   counts are absent.
2. **[IMPLEMENTED (code)]** A lazily allocated deep-copy EPT root is built at
   PASSIVE_LEVEL. Uniform, mixed RAM/MMIO, and non-RAM leaves all honor the
   requested R/W-only secondary-root permissions.
3. **[IMPLEMENTED (code)]** A cached contiguous shadow is allocated, patched
   with checked bounds, and copied from the HPA currently mapped by the primary
   root. Non-identity runtime tests remain absent.
4. **[IMPLEMENTED (code)]** Install capability-gates execute-only EPT, changes
   the primary PTE to R/W, and maps the shadow X-only in the secondary PTE.
   This local PTE operation does not make the hook subsystem complete.
5. **[IMPLEMENTED (code)]** The three transition branches retry without
   advancing RIP, and the secondary root remains non-executable outside shadow
   pages.
6. **[IMPLEMENTED (code)]** Per-vCPU active EPTP tracking and generation-based
   INVEPT on view switches exist. **[NOT VERIFIED]** under concurrent all-core
   transitions.
7. **[IMPLEMENTED (code)]** Removal unpublishes a versioned slot, forces and
   verifies every vCPU on the primary root, reacquires both PTEs before changing
   either, restores and invalidates both views, then frees the shadow. Failure
   republishes the unchanged policy. Duplicate GPAs are rejected, live shadows
   are freed at teardown, and the secondary-root pool tag is consistent.

Acceptance gate: **[NOT VERIFIED]**. There is no committed hook test harness or
bare-metal evidence for original-read/shadow-execute behavior, page crossing,
concurrent install/remove, memory type, or safe retirement.

## Stage 5 — AMD CPUID policy

1. **[IMPLEMENTED (code)]** CPUID interception has a matching dispatcher and
   native-result masking policy.
2. **[IMPLEMENTED (code)]** The handler clears hypervisor and VMX exposure from
   leaf 1, clears SVM from `0x80000001`, zeros `0x8000000A`, advances `NextRip`,
   and marks the VMCB dirty.
3. **[NOT VERIFIED]** Topology/OSXSAVE behavior and AMD hardware evidence remain
   absent.

## Stage 6 — device/API surface

1. **[IMPLEMENTED (code)]** The secure device, DOS link, create/close/cleanup
   and device-control dispatch, and failure/unload teardown exist.
2. **[PARTIAL]** The ABI uses fixed-width versioned headers, `METHOD_BUFFERED`,
   length checks, and a SYSTEM/Administrators SDDL. There is no explicit
   debug-privilege check; access is ACL-based.
3. **[PARTIAL]** Status and Intel hook add/remove/query commands exist. HPA, GPA,
   GVA, and translate commands do not.
4. **[NOT IMPLEMENTED]** No 4/5-level guest page-table walker exists.
5. **[NOT IMPLEMENTED]** No executable-write coherency or MMIO range policy
   exists because physical-memory commands are absent.
6. **[IMPLEMENTED (code)]** IOCTL handlers acquire rundown protection before
   reading the published `HV_STATE*`. Unpublish blocks new requests and waits
   for in-flight handlers before hypervisor stop.
7. **[NOT IMPLEMENTED]** No user-facing VMCALL command ABI exists.

## Stage 7 — console

**[NOT IMPLEMENTED]** There is no `tools/johnsmithctl/` project. The existing
tool is only the VM-exit benchmark.

## Stage 8 — host page-table isolation research gate

**[NOT IMPLEMENTED]** The current VMCS host CR3 is `__readcr3()`. No private
root address space or mapping-lifetime implementation exists, and no private
host-address-space claim should be made.

## Recommended implementation order

1. Complete Stage 0 contracts and add unit-testable helpers/tests.
2. Harden Stage 1 remap/split/merge with focused tests.
3. Complete Stage 2 classified EPT policy only where hook policy needs it.
4. Add Stage 4 focused and concurrent runtime tests before claiming hardware
   support.
5. Add Stage 6 memory APIs/walker only when a concrete consumer requires them.
6. Implement Stage 3 event features individually, enabling controls only with
   complete producers and handlers.
7. Treat Stage 8 as a separate security project.

## Hardware verification gates

All gates are currently **[NOT VERIFIED]** by committed, reproducible evidence.
For each completed stage, build Debug/Release/Benchmark and run WDK analysis.
Runtime records must include CPU model/microcode, Windows build, VMX/SVM
capabilities, artifact SHA-256, and debugger logs. Required bare-metal tests
include all-core launch/stop, repeated split/remap/merge, concurrent hook
install/remove, instruction/data behavior, invalidation completion on every
CPU, sleep/hotplug policy, and failure injection at each
allocation/publication boundary.
