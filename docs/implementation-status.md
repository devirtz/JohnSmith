# Implementation status audit

Audit date: **2026-07-13**.

This is a static source audit of the first-party code, project files, and
documentation. It does not claim bare-metal validation. The current repository
has no first-party unit-test project for EPT hooks, event injection, or the AMD
exit dispatcher. `src/introspection.c` is a startup permission-change exercise,
not a replacement for those tests.

## Status meanings

- **Implemented (code):** reachable code is present and included in
  `JohnSmith.vcxproj`. This does not imply hardware validation.
- **Partial:** some required code exists, but the documented contract is not
  complete.
- **Not implemented:** no end-to-end implementation was found.
- **Blocked by defect:** code is present, but a source-level defect prevents the
  documented behavior from being claimed.
- **Not verified:** the required build, analysis, or bare-metal evidence is not
  committed with enough provenance to reproduce the claim.

## Current capability matrix

| Area | Audited status | Source evidence and boundary |
| --- | --- | --- |
| Common all-CPU lifecycle | Implemented (code), not verified on hardware | `src/hv.c` owns prepare/start/rollback/stop rendezvous. |
| Intel baseline VMX/EPT/VPID | Implemented (code), not verified on hardware | VMCS setup, assembly entry/exit, identity EPT, and generation invalidation are compiled into the driver. |
| Intel runtime 4 KiB EPT permission changes | Implemented (code), tests incomplete | Query/set and permission-split merge paths exist. The startup introspection exercise covers one allocation, not the roadmap matrix. |
| Intel EPT remapping | Partial | `IntelSetOwnedPageMapping` exists only as an Intel-internal, currently unused helper and has no focused tests. |
| Intel EPT exit classification | Partial | Qualification decoding and fail-stop paths exist. Synthetic `#PF` policy/PFEC construction is absent, and misconfiguration diagnostics do not dump EPTP or walk entries. |
| Intel dual-EPT hooks | Implemented (code), not verified on hardware | Mixed-region permissions, duplicate-GPA rejection, versioned lookup, all-core primary-view retirement, transactional removal, and teardown cleanup are present. Focused runtime tests remain absent. |
| Intel event collision handling | Partial | A basic exception classifier and `#DF` promotion exist; serial event preservation and hardware tests remain open. |
| Intel interrupt/NMI window injection | Not implemented; dead scaffold remains | Request/handler functions and one-slot pending fields exist, but there is no producer. The NMI request path would enable NMI-window exiting without the required pin/virtual-NMI controls if called. |
| AMD baseline SVM/NPT | Implemented (code), not verified on AMD hardware | VMCB/NPT/assembly code and CPUID emulation are compiled into the driver. |
| AMD CPUID policy | Implemented (code), not verified on AMD hardware | Native results are returned with hypervisor, VMX, SVM, and SVM capability exposure removed. |
| Control device and status IOCTL | Implemented (code) | `DriverEntry` creates the secure device, publishes a successful hypervisor, and tears it down on failure/unload. |
| Hook IOCTL ABI | Implemented (code), not verified on hardware | Intel install/remove/query routing exists; AMD reports unsupported. Rundown protection serializes in-flight IOCTLs against unpublish/stop. |
| HPA/GPA/GVA memory IOCTLs and guest page walker | Not implemented | No command definitions or walker implementation were found. |
| Console (`johnsmithctl`) | Not implemented | No `tools/johnsmithctl` project exists. |
| Private host page tables | Not implemented | VMCS host CR3 remains the current Windows CR3. |

## Remaining source-level gaps

- AMD exception collision handling still lacks the complete benign,
  contributory, page-fault, double-fault, and shutdown combination policy.
- Intel interrupt/NMI-window functions remain dead scaffold without the
  prerequisite controls, producer, or queueing model.
- Hook-created 4 KiB splits remain allocated until backend teardown; reference
  counting and last-hook merge are not implemented.
- Guest page walking, physical-memory IOCTLs, guest XCR0 virtualization, and
  private host page tables remain unimplemented.

## Evidence boundary

- `src/driver.c` reaches the device lifecycle, and the device uses rundown
  protection before dereferencing the published hypervisor.
- The repository contains build output, but generated artifacts alone do not
  identify a current source commit, CPU, Windows build, firmware state, or
  runtime test procedure. They are not bare-metal proof.
- No committed test harness or bare-metal record exercises mixed-region hook
  transitions, concurrent removal, teardown with live hooks, all-core view
  retirement, AMD CPUID, or the EPT-roadmap acceptance gates.
- The detailed per-stage status is maintained in
  [`ept-roadmap.md`](ept-roadmap.md).
