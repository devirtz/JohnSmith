# Documentation

Architecture documentation snapshot: **2026-07-13**.

## Start here

| Topic | Document |
| --- | --- |
| Audited implementation status | [Source-to-claim status](docs/implementation-status.md) |
| Intel virtualization | [VMX/EPT architecture map](docs/architecture/intel-vmx.md) |
| AMD virtualization | [SVM/NPT architecture map](docs/architecture/amd-svm.md) |
| VM-exit optimization | [Performance and measurement](docs/performance.md) |
| Build, load, and validation | [Build and test](docs/build-and-test.md) |
| Source provenance | [Reference catalog](docs/references.md) |
| Contribution rules | [CONTRIBUTING.md](CONTRIBUTING.md) |
| Security reporting | [SECURITY.md](SECURITY.md) |

## Evidence policy

Use sources in this order:

1. Intel SDM, AMD APM, and installed WDK headers for architectural or DDI
   requirements.
2. Vendor optimization manuals for documented microarchitectural guidance.
3. Hardware measurements from the exact binary, CPU, and Windows build under
   discussion.
4. Research papers for design context only.

Every architectural claim must identify the document revision and section or
table title. Page numbers are secondary because pagination changes between
combined and split manuals. A clean build proves neither successful VM entry
nor correct behavior on untested hardware.

## Repository map

```text
asm/                  VM-entry, VM-exit, and state-transition assembly
include/              Public and assembly-facing contracts
src/common/           Vendor-neutral x86 validation and range helpers
src/intel/            VMCS, VM-exit, and EPT implementation
src/amd/              VMCB, VM-exit, and NPT implementation
src/hv.c              Backend selection and all-CPU lifecycle
src/introspection.c   Owned-page permission exercise
tools/                Loader and VM-exit benchmark
docs/                 Maintained design and operating documentation
static/docs/          Pinned original manuals and research papers
```

## Review rules

- Validate optional controls against capability MSRs or CPUID before use.
- Keep C layout assertions for every assembly-visible VMCS/VMCB context field.
- Add an exit handler before enabling a new intercept.
- Preserve all-CPU rollback, SLAT invalidation, and fail-stop teardown.
- Separate architectural requirements from project policy and measured facts.
- Record CPU model, firmware state, Windows build, configuration, service path,
  driver hash, sample count, and statistic for every performance result.
