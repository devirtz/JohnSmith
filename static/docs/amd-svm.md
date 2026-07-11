# AMD SVM/NPT map

Authority: [AMD APM Volume 2, 24593 rev.
3.44](https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2) and [Volume 3,
24594 rev. 3.37](https://docs.amd.com/v/u/en-US/24594_3.37).

| Code concern | APM location |
| --- | --- |
| SVM/NPT/ASID support | CPUID `Fn8000_000A`; Chapter 15 |
| Enable/disable | `EFER.SVME`, `VM_CR.SVMDIS`, `VM_HSAVE_PA` |
| VMCB layout | Volume 2 Appendix B, Tables B-1/B-2 |
| Exit codes | Appendix C |
| IOPM/MSRPM | Sections 15.10-15.11 |
| VMCB clean bits | Section 15.15 |
| ASID/TLB flush | Section 15.16 |
| Event injection | Section 15.20 |
| Nested paging/NPF | Section 15.25 |
| VMRUN/VMMCALL | Volume 3 instruction reference |

Code: `include/amd.h`, `src/amd.c`, `src/amd/`, `asm/amd.asm`.

Keep `C_ASSERT` checks for every assembly-visible VMCB offset. Derive SVM, NPT,
nRIP, ASID count, physical width, and flush behavior at runtime. Clear relevant
VMCB clean bits after software changes cached state. Interpret NPF
`EXITINFO1/EXITINFO2` before guest injection, and flush every active CPU after a
live NPT permission/mapping change. The project 512 GiB identity map is an
implementation ceiling.
