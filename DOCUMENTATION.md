# JohnSmith documentation policy

This file is the entry point for architecture work in JohnSmith. Its purpose is
to keep implementation claims traceable and to prevent constants or behavior
from being accepted only because they look familiar.

Source snapshot: **2026-07-10**.

## Evidence rules

1. Use a primary source: the Intel SDM, AMD APM, Microsoft Learn/WDK headers,
   or an applicable published specification.
2. Record the document ID, revision, section or table title, and the code that
   depends on it. Page numbers alone are not stable across revisions.
3. Mark evidence as **verified**, **inferred**, or **untested**. Never silently
   promote an inference into a verified fact.
4. Architecture constants need a nearby source comment, a compile-time
   assertion where layout is involved, and a runtime capability check where the
   architecture makes support optional.
5. A successful build proves compilation and linking only. It does not prove
   legal VM-entry state, correct VM-exit behavior, cross-CPU teardown, or safe
   operation on hardware.
6. Example projects, blog posts, and the local `Reff/VMXHypervisorToolbox` tree
   may help discovery, but they are never normative sources.

If sources conflict, stop and record the conflict. Prefer the newest applicable
primary architecture manual, then verify against real hardware before changing
behavior. Do not guess.

## Pinned primary sources

| Area | Primary source | Pinned metadata |
| --- | --- | --- |
| Intel VMX, EPT, VMCS, exits | [Intel 64 and IA-32 SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html) | SDM PDFs version 092; Intel page updated 2026-06-22 |
| AMD SVM, NPT, VMCB | [AMD64 APM Volume 2, System Programming](https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2) | Document 24593, revision 3.44, released 2026-03-06 |
| AMD virtualization instructions | [AMD64 APM Volume 3](https://docs.amd.com/v/u/en-US/24594_3.37) | Document 24594, revision 3.37, released 2025-07-02 |
| AMD complete architecture set | [AMD64 APM Volumes 1-5](https://docs.amd.com/v/u/en-US/40332_4.09_APM_PUB) | Document 40332, revision 4.09, released 2026-03-09 |
| Windows kernel APIs | [Windows driver API reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/) | Recheck the page requirements and installed WDK headers before each change |
| Windows x64 ABI | [x64 calling convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170) | MSVC/WDK toolset used by the project |

The vendor landing pages are authoritative for the current download. A cached
PDF filename is not evidence of its embedded revision; check its cover/revision
record and checksum.

## Current evidence status

- The source has been built for Debug x64 and Release x64 with the configured
  WDK toolchain. This is **build evidence only**.
- No reproducible Intel or AMD bare-metal test record is checked into this
  repository. Both runtime backends therefore remain **untested** for release
  claims.
- The current use of `MmGetPhysicalMemoryRanges` is **unsupported/untested** as
  a public driver contract because Microsoft lists it as reserved for system
  use. See the [Windows source map](static/docs/windows-kernel.md#known-contract-risk).
- These source maps are navigation aids, not proof that every implementation
  detail is correct. Complete the verification checklist for each release.

## Topic guides

- [Documentation hub](static/docs/README.md)
- [Intel VMX/EPT source map](static/docs/intel-vmx.md)
- [AMD SVM/NPT source map](static/docs/amd-svm.md)
- [SLAT and invalidation invariants](static/docs/slat-and-invalidation.md)
- [Windows kernel and x64 ABI references](static/docs/windows-kernel.md)
- [Extended reading list](static/docs/reading-list.md)
- [Verification checklist](static/docs/verification-checklist.md)

## Code ownership map

| Concern | Code | Required evidence |
| --- | --- | --- |
| Backend contract and per-CPU state | `include/hv.h`, `src/hv.c` | WDK processor topology, broadcast, IRQL and allocation contracts |
| Intel VMX/EPT | `include/intel.h`, `src/intel.c`, `asm/intel.asm` | Intel SDM VMX chapters, VMCS tables, capability MSRs and instruction reference |
| AMD SVM/NPT | `include/amd.h`, `src/amd.c`, `asm/amd.asm` | AMD APM Chapter 15, Appendix B/C and instruction reference |
| Owned introspection page | `include/introspection.h`, `src/introspection.c` | WDK contiguous nonpaged allocation/free contract and SLAT rules |
| Driver lifecycle | `src/driver.c` | WDM unload, signing, pool, processor group and IRQL rules |

## Required change record

Any change to VMCS/VMCB layouts, control bits, exit codes, event injection,
MSRPM/I/O maps, paging entries, invalidation, CR masks, or assembly ABI must put
this in its pull-request description:

```text
Source: <vendor>, <document ID/revision>, <section/table title>
Code: <file and symbol>
Evidence: verified | inferred | untested
Checks: build | static analysis | debugger | Intel hardware | AMD hardware
```

Unverified values must not be merged as architecture facts.
