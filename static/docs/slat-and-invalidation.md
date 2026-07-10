# SLAT and invalidation invariants

JohnSmith uses Intel EPT or AMD NPT to build an identity second-level mapping.
The common ceiling is `HV_SLAT_MAXIMUM_ADDRESS` = 512 GiB. That is a deliberate
project limit and must not be described as the CPU's maximum guest-physical
address.

## Mapping shape

The current layout uses one top-level entry:

```text
PML4[0] -> 512 PDPT entries -> 512 page directories
                                -> 2 MiB identity leaves
                                -> optional 4 KiB split page table
```

One PML4 entry spans 512 GiB. The initial map uses 2 MiB leaves. A page is split
to 4 KiB entries only when the backend changes permissions for a page it owns.

## Common invariants

- Reject negative, non-page-aligned, overflowed, unsupported, or
  `>= 512 GiB` addresses before indexing a paging structure.
- Clamp construction to both the platform physical-address width and the
  project ceiling.
- Map RAM with the documented normal memory type. Treat non-RAM/MMIO ranges
  conservatively; never infer cacheability from address alone.
- Never accept an arbitrary external address in the introspection API. The
  single contiguous nonpaged page is allocated, tracked, queried, modified and
  freed internally.
- Query and set callbacks apply only to that owned page and return explicit
  range/status failures otherwise.
- Publish a split only after the 4 KiB table is complete. Retain its allocation
  until virtualization has stopped on all CPUs.
- Serialize SLAT mutation and make the new mapping visible before invalidation.
- Do not resume any CPU with a stale translation after a permission reduction
  or mapping change.

## Intel checks

Use the Intel SDM Volume 3C section "Extended Page Tables" plus Appendix A's
`IA32_VMX_EPT_VPID_CAP` definitions. Verify:

- supported EPT page-walk length and EPTP memory type;
- 2 MiB EPT leaf support before building large leaves;
- legal read/write/execute combinations and reserved bits;
- `MAXPHYADDR` handling;
- INVEPT availability and supported single-context/all-context type; and
- EPT-violation versus EPT-misconfiguration handling.

The [official Intel SDM page](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
provides the current Volume 3C.

## AMD checks

Use AMD APM Volume 2 Sections 15.16 and 15.25 and Appendix B. Verify:

- CPUID `Fn8000_000A_EDX[NP]` before enabling NPT;
- NPT entry reserved bits, NX behavior, address width and memory type rules;
- `NP_ENABLE` and `N_CR3` VMCB fields;
- ASID is nonzero and within the enumerated count;
- `TLB_CONTROL` uses a defined encoding; and
- `EXITINFO1/EXITINFO2` interpretation on NPF.

The [official AMD Volume 2 page](https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2)
provides the pinned manual.

## Invalidation sequence

For a live mapping change:

1. acquire the backend SLAT lock;
2. validate ownership, alignment, range, and requested access;
3. allocate and fully initialize a split table if required;
4. atomically publish the leaf/table change;
5. issue the vendor-defined invalidation on every active logical processor;
6. fail closed if any processor cannot complete invalidation; and
7. release the lock only after the rendezvous completes.

Intel INVEPT and AMD `TLB_CONTROL` have different scope and capability rules.
They are not interchangeable abstractions merely because both flush cached
translations.
