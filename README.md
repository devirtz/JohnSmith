# JohnSmith

Minimal late-launch Windows x64 research hypervisor with Intel VT-x/EPT and
AMD-V/SVM/NPT backends.

![JohnSmith banner](static/img/main.png)

> [!WARNING]
> Experimental kernel code. Use only on disposable, kernel-debugger-enabled
> systems. It is not a production security boundary.

## Capabilities

| Area | Implementation |
| --- | --- |
| Lifecycle | Synchronized all-CPU launch, rollback, and teardown |
| Intel | VMX, EPT, VPID, VMCS control validation, CPUID policy |
| AMD | SVM, NPT, ASIDs, VMCB clean-state and TLB control |
| Memory | 512 GiB identity-SLAT ceiling; runtime 4 KiB access changes |
| State | CR0/CR3/CR4, debug state, PAT/EFER, MSR bitmaps |
| Diagnostics | Fail-stop bugchecks and Debug-only VM-exit history |
| Measurement | Cross-core software-clock VM-exit benchmark |

Intel CPUID exits are emulated and VMX-related features are hidden. AMD CPUID
executes natively. Nested virtualization is not implemented.

## Requirements

- Windows 10 version 2004 or newer, or Windows 11 x64.
- Bare-metal Intel VT-x/EPT or AMD-V/NPT enabled in firmware.
- Hyper-V, VBS, HVCI, and other active hypervisors disabled.
- Visual Studio 2022, Desktop C++, and WDK `10.0.26100`.
- Intel supervisor CET inactive (`IA32_S_CET=0`); AMD CET disabled.

## Build

From a Visual Studio Developer PowerShell:

```powershell
msbuild JohnSmith.sln /m /p:Configuration=Release /p:Platform=x64
```

| Configuration | Purpose | Driver |
| --- | --- | --- |
| `Debug` | Diagnostics and VM-exit history | `build/bin/Debug/JohnSmith.sys` |
| `Release` | Optimized hot path | `build/bin/Release/JohnSmith.sys` |
| `Benchmark` | Measurement-only VMCALL floor | `build/bin/Benchmark/JohnSmith.sys` |

## Load and measure

The included KDU workflow temporarily changes DSE, creates a normal kernel
service, restores DSE, and then starts virtualization:

```powershell
.\tools\load-kdu.ps1 -Config Release
.\tools\unload-kdu.ps1
```

Measure the transition floor only with the Benchmark driver:

```powershell
.\tools\load-kdu.ps1 -Config Benchmark
.\build\bin\tools\vmexit-bench.exe 200000 --vmcall
```

Results are software-clock ticks, not CPU cycles. Verify the installed service
path and driver hash before interpreting runtime data.

## Documentation

| Document | Purpose |
| --- | --- |
| [Documentation index](DOCUMENTATION.md) | Scope, policy, and repository map |
| [Intel VMX/EPT](docs/architecture/intel-vmx.md) | VMCS, exits, EPT, VPID, CET |
| [AMD SVM/NPT](docs/architecture/amd-svm.md) | VMCB, exits, NPT, ASIDs |
| [Performance](docs/performance.md) | Benchmark design and evidence rules |
| [Build and test](docs/build-and-test.md) | Reproducible build/load workflow |
| [References](docs/references.md) | Manuals, papers, revisions, and hashes |

## Deliberate limits

- No nested VMX or nested SVM.
- No processor reset, hot-add, or memory hot-add while active.
- No recoverable EPT/NPT violation emulator.
- No root supervisor-CET or shadow-stack implementation.
- No I/O virtualization or device model.
- No timing-invisibility claim.
- AMD runtime validation remains required on supported bare-metal SVM hardware.

## License

[MIT](LICENSE)
