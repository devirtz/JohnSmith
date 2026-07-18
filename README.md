# JohnSmith

Windows x64 research hypervisor with Intel VT-x/EPT and AMD-V/SVM/NPT
backends for isolated red-team experiments.

![JohnSmith banner](static/img/main.png)

> [!WARNING]
> Experimental Hypervisor, Not Ready for daily use or production uses

## Capabilities

| Area | Implementation |
| --- | --- |
| Lifecycle | Synchronized all-CPU launch, rollback, and teardown |
| Intel | VMX, EPT, VPID, VMCS control validation, and CPUID policy |
| Intel hooks | Per-vCPU dual-EPT execute hooks and observation counters |
| AMD | SVM/NPT and masked CPUID policy |
| Control | Per-CPU CPUID/shared-page transport and `johnsmithctl` |
| Memory | 512 GiB identity-SLAT ceiling, runtime 4 KiB access changes |
| State | CR0/CR3/CR4, debug state, PAT/EFER, and MSR bitmaps modeled in code |
| Diagnostics | Fail-stop bugchecks and Debug-only VM-exit history |
| Measurement | Cross-core software-clock VM-exit benchmark |

Intel and AMD CPUID exits are emulated and vendor-virtualization features are
hidden. Execute hooks and `johnsmithctl` control are Intel-only.

## Build and start

```powershell
msbuild .\JohnSmith.sln /m /p:Configuration=Release /p:Platform=x64
.\build\bin\tools\johnsmithctl.exe selftest
.\build\bin\tools\johnsmithctl.exe start --cpu 0
```

`johnsmithctl start` temporarily modifies Driver Signature Enforcement through
KDU. Use it only on a disposable test system with Secure Boot, Hyper-V, VBS,
and HVCI disabled. Do not interrupt the loader before it restores DSE.

Stop and remove the service with:

```powershell
.\build\bin\tools\johnsmithctl.exe stop
```

## Documentation

| Document | Purpose |
| :--- | :--- |
| [Implementation Status](docs/implementation-status.md) | Audited code-to-claim matrix and known blockers |
| [Documentation Index](DOCUMENTATION.md) | Scope, policy, and repository map |
| [Intel VMX/EPT](docs/architecture/intel-vmx.md) | VMCS, exits, EPT, VPID, CET |
| [AMD SVM/NPT](docs/architecture/amd-svm.md) | VMCB, exits, NPT, ASIDs |
| [Build and Test](docs/build-and-test.md) | Reproducible build/load workflow instructions |
| [References](docs/references.md) | Manuals, papers, revisions, and hashes |

## License

[MIT](LICENSE)
