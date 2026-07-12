# Reference catalog

Snapshot verified: **2026-07-12**.

## Authority levels

| Level | Use |
| --- | --- |
| Normative | Architectural behavior, field encodings, exceptions, and DDIs |
| Vendor guidance | Documented optimization and processor-family behavior |
| Platform interface | Guest-visible ABI or operating-system contract |
| Research context | Design history and experimental techniques; never normative |

## Normative architecture

| Source | Revision | Local copy | Project use |
| --- | --- | --- | --- |
| Intel 64 and IA-32 SDM, combined volumes | 092, current as of 2026-06-22 | [325462-092](../static/docs/325462-092-sdm-vol-1-2abcd-3abcd-4.pdf) | VMX, EPT, VPID, instructions, exceptions, MSRs |
| AMD64 APM Volume 2: System Programming | 24593 rev. 3.44, 2026-03-06 | [24593-3.44](../static/docs/24593_3.44_APM_Vol2.pdf) | SVM, VMCB, NPT, ASID, event injection |
| AMD64 APM Volumes 1-5 | 40332 rev. 4.09 | [40332-4.09](../static/docs/40332_4.09_AMD64_APM_Volumes_1-5.pdf) | Cross-volume instruction and architecture lookup |
| Intel VT-d Architecture Specification | D51397-019, rev. 5.20, 2026-04 | [VT-d 5.20](../static/docs/D51397-019_Intel_VT-d_Architecture_Spec_5.2.pdf) | Future IOMMU work; not implemented |

Official live indexes:

- [Intel software developer manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [AMD APM Volume 2](https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2)
- [AMD processor documentation](https://www.amd.com/en/search/documentation/hub.html)
- [Microsoft Windows driver DDI reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/)

The split Intel Volume 3C file
[326019-082](../static/docs/326019-sdm-vol-3c.pdf) is retained for historical
search convenience but is superseded by the combined version 092 manual. Do
not cite it when version 092 contains the same topic.

## Vendor optimization and security guidance

| Source | Revision | Local copy | Correct use |
| --- | --- | --- | --- |
| Intel Optimization Reference Manual, Volume 1 | 248966-050, 2024-04 | [Intel ORM v050](../static/docs/248966-050-intel-optimization-vol1.pdf) | Golden Cove, cache, TLB, branch, locality guidance |
| AMD Indirect Branch Control Extension white paper | 111006-B, 2018-07 | [AMD 111006-B](../static/docs/111006-amd-hypervisor-speculation.pdf) | Historical SPEC_CTRL host/guest model; verify current APM/errata before implementation |

Live source:

- [Intel architecture optimization manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel64-and-ia32-architectures-optimization.html)
- [Intel VM-workload exit discussion](https://www.intel.com/content/www/us/en/developer/articles/technical/improving-performance-vm-workloads-opt-poll-time.html)

Optimization manuals describe documented microarchitectures but do not convert
hidden implementation details into architectural guarantees. In particular,
they do not promise predictor, VMCS-cache, or TLB warmth across VM transitions.

## Platform interfaces

| Source | Revision | Local copy | Scope |
| --- | --- | --- | --- |
| Microsoft Hypervisor TLFS | 6.0b, 2020-02 | [TLFS 6.0b](../static/docs/Microsoft_Hypervisor_TLFS_v6.0b.pdf) | Historical Microsoft HV#1 guest ABI |
| Microsoft TLFS online | Continuously maintained | [Current TLFS](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/tlfs) | Primary reference for current HV#1 behavior |
| Microsoft x64 ABI | Current online | [x64 calling convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170) | Register volatility, stack alignment, shadow space |

JohnSmith does not advertise the Microsoft HV#1 interface. TLFS terminology is
useful context but does not define Intel VMX or AMD SVM behavior.

## Research context

| Paper | Local copy | Relevance and limit |
| --- | --- | --- |
| Agesen et al., “Software Techniques for Avoiding Hardware Virtualization Exits,” USENIX ATC 2012 | [PDF](../static/docs/atc12-avoiding-hardware-virtualization-exits.pdf) | Exit-frequency reduction and historical transition measurements; not an Alder Lake estimate |
| Kivity et al., “kvm: the Linux Virtual Machine Monitor,” OLS 2007 | [PDF](../static/docs/ols2007-kvm.pdf) | Small hardware-assisted VMM architecture and exit dispatch; not normative |

Additional reading not archived locally:

- [USENIX ATC 2012 paper page](https://www.usenix.org/conference/atc12/technical-sessions/presentation/agesen)
- [Adams and Agesen, “A Comparison of Software and Hardware Techniques for x86 Virtualization,” DOI 10.1145/1168857.1168860](https://doi.org/10.1145/1168857.1168860)
- [Ben-Yehuda et al., “The Turtles Project,” OSDI 2010](https://www.usenix.org/conference/osdi10/turtles-project-design-and-implementation-nested-virtualization)

Research measurements apply only to their processors, VMMs, and methodology.
They may motivate an experiment but cannot replace target-hardware data.

## Local-file integrity

| File | SHA-256 |
| --- | --- |
| `111006-amd-hypervisor-speculation.pdf` | `CC7EDFB1E29FCB435EAD6ED162E50E6EE82E3E6CCB79436B4A86A7A59FE45FE9` |
| `24593_3.44_APM_Vol2.pdf` | `465454E3E7761B126075C789B4BF8B73190F8284637BE604D053252DAB6C11FD` |
| `248966-050-intel-optimization-vol1.pdf` | `4DA42303A8D82A0B4E4FFEBFDB669D6B57D85A038887421339E25CA37B67D61B` |
| `325462-092-sdm-vol-1-2abcd-3abcd-4.pdf` | `16A9336104750613AE2F2BAB6EB7A1B21A7E1EF60CED35E9AB2E0D8C7EFCEC68` |
| `326019-sdm-vol-3c.pdf` | `961FA795D2E504082C475643813E05CC7F72B6A3741B179234C711B0BCC50A2B` |
| `40332_4.09_AMD64_APM_Volumes_1-5.pdf` | `AE18704F924BB80BC9BBFE9D3C33B97C16CDEAA9C7AFCCB033D136E86FD88A08` |
| `atc12-avoiding-hardware-virtualization-exits.pdf` | `3EC491172A00A25C8213AD9C04635F048B6641E0C6758F15F514955FEBF87433` |
| `D51397-019_Intel_VT-d_Architecture_Spec_5.2.pdf` | `44179074934B5D28F49E0302805D69E74BD4E7AD323525EF127D6E99E0C0A2FA` |
| `Microsoft_Hypervisor_TLFS_v6.0b.pdf` | `858642504C86ED80B141862048B6D1327C0FE5237B666E4CBE41FB351FAEE92C` |
| `ols2007-kvm.pdf` | `5BBD6226D5919972E6BE2816098C80DEC163CF99847C1ED85DAFFC3D4E8173D9` |

## Refresh procedure

1. Check official Intel, AMD, and Microsoft indexes.
2. Record document ID, revision, release date, and canonical URL.
3. Download the unmodified public original.
4. Verify that the PDF opens and its cover metadata matches the catalog.
5. Record SHA-256 and update architecture links if section numbering changed.
6. Re-audit affected code before adopting changed semantics.
