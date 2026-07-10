# AMD SVM/NPT source map

Normative sources:

- [AMD64 Architecture Programmer's Manual Volume 2: System Programming,
  24593 rev. 3.44](https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2)
- [AMD64 Architecture Programmer's Manual Volume 3: Instructions, 24594 rev.
  3.37](https://docs.amd.com/v/u/en-US/24594_3.37)

Verify the revision printed inside a downloaded PDF. Do not rely on its filename.

## Implementation map

| Project concern | AMD APM location to verify | Code |
| --- | --- | --- |
| SVM and NPT discovery | CPUID `Fn8000_000A`; Chapter 15 SVM feature discovery | `AmdSupport` in `src/amd.c` |
| Enable/disable state | `EFER.SVME`, `VM_CR.SVMDIS`, `VM_HSAVE_PA` | `AmdStart`, stop path |
| VMCB byte layout | Volume 2, Appendix B, Tables B-1 and B-2 | `AMD_VMCB_*` in `include/amd.h` |
| Intercepts and exit codes | Sections 15.9-15.11 and Appendix C | intercept constants, `AmdVmExitHandler` |
| I/O permissions | Section 15.10, "IOIO Intercepts" | IOPM allocation and VMCB base |
| MSR permissions | Section 15.11, "MSR Intercepts" | `AmdInitializeMsrpm` |
| VMCB cache coherency | Section 15.15, "VMCB State Caching" | `VmcbClean` updates |
| ASIDs and invalidation | Section 15.16, "TLB Control" | ASID assignment and SLAT rendezvous |
| Event injection | Section 15.20, "Event Injection" | `AmdInjectException` |
| Nested paging | Section 15.25, "Nested Paging" | `AmdBuildNpt`, owned-page callbacks |
| VMRUN/VMMCALL semantics | Volume 3 instruction reference | `asm/amd.asm`, stop hypercall |

## Layout rules

`include/amd.h` intentionally uses `C_ASSERT` for the VMCB page size and for
fields consumed by assembly or VM-exit code. When Appendix B changes:

1. compare every represented field with Table B-1/B-2;
2. preserve reserved/SBZ ranges with explicit padding;
3. update compile-time offsets before changing any assembly; and
4. re-audit `EVENTINJ`, `N_CR3`, `nRIP`, `EFER`, `RIP`, `RSP`, and `RAX` first.

## Facts that must remain runtime-derived

- SVM availability, ASID count, NPT and nRIP support from CPUID
  `Fn8000_000A`.
- Whether firmware disabled and locked SVM through `VM_CR`.
- Physical address width and legal NPT address bits.
- Optional instruction and invalidation behavior; reserved `TLB_CONTROL`
  encodings must never be used.

## Review traps

1. IOPM is 12 KiB of contiguous physical memory. MSRPM addressing is sparse;
   derive the bit location from the architectural ranges rather than treating
   an MSR number as a byte offset.
2. Clear the relevant VMCB clean bit whenever software changes cached state.
   A new or relocated VMCB starts with all clean bits clear.
3. An NPF is a second-level translation failure. Interpret `EXITINFO1` and
   `EXITINFO2` before deciding whether and how the guest should see a fault.
4. Permission reductions or mapping changes require an architecturally valid
   TLB flush before the same ASID resumes.
5. Host state not automatically saved by SVM remains the assembly/C context's
   responsibility.
6. Reject an active upstream hypervisor instead of assuming nested SVM works.

The project limit of 512 GiB is an implementation ceiling, not an AMD
architectural limit. See [SLAT and invalidation](slat-and-invalidation.md).
