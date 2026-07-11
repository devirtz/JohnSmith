# Intel VMX/EPT map

Authority: [Intel 64 and IA-32 SDM version
092](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
mainly Volume 3C.

| Code concern | SDM location |
| --- | --- |
| VMX/firmware support | CPUID VMX; `IA32_FEATURE_CONTROL` |
| VMXON/VMCS pages | VMCS region; Section 27.11.5, VMXON Region |
| Control masks/CR rules | Appendix A capability MSRs; CR fixed MSRs |
| VMCS fields | Chapter 27 VMCS tables |
| VM entry/exit | Chapters 29-30 |
| Event reinjection | VM-entry injection and IDT-vectoring fields |
| EPT/VPID | Section 31.3 and `IA32_VMX_EPT_VPID_CAP` |
| INVEPT/INVVPID/VMCALL | VMX instruction reference |

Code: `include/intel.h`, `src/intel.c`, `src/intel/`, `asm/intel.asm`.

Always derive revision ID, allowed controls, CR masks, EPT capabilities,
physical width, and invalidation types at runtime. Treat EPT violation and EPT
misconfiguration separately. After a live EPT permission/mapping change,
invalidate every active CPU before resuming it. The project 512 GiB identity map
is an implementation ceiling. VMCS has an XSS-exiting bitmap, but no
guest-XSS or host-XSS state fields; do not invent such encodings.

`CR4.CET=1` does not by itself mean supervisor CET is active. JohnSmith permits
that state only when `IA32_S_CET=0`, which preserves user-mode CET without
requiring an alternate root shadow stack. Any nonzero `IA32_S_CET` is rejected
on every logical processor until guest/host CET VMCS state, CET entry/exit
controls, root SSP storage, and `#CP` handling are implemented. While active,
the MSR bitmap permits writes of zero to `IA32_S_CET` but injects `#GP` for a
nonzero write so the supervisor-CET-disabled invariant cannot change after
launch. User-mode CET MSRs are not intercepted.
