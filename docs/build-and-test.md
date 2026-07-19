# Build and test

## Toolchain

- Visual Studio 2022 with Desktop C++.
- Windows Driver Kit `10.0.26100`.
- x64 Developer PowerShell or Developer Command Prompt.
- Bare-metal test system with a kernel debugger strongly recommended.

## Build configurations


| Configuration | Diagnostics | CPUID fast path | VMCALL floor |
| --- | --- | --- | --- |
| Debug | Enabled | Disabled | Disabled |
| Release | Disabled | Enabled | Disabled |
| Benchmark | Disabled | Enabled | Enabled |

Build from a Visual Studio developer shell:

```powershell
msbuild .\JohnSmith.sln /m /p:Configuration=Release /p:Platform=x64
.\build\bin\tools\johnsmithctl.exe selftest
```


## Load workflow

JohnSmith is unsigned. `johnsmithctl` builds KDU when needed, temporarily
changes Driver Signature Enforcement, creates the service, starts the driver,
and restores DSE.

```powershell
.\build\bin\tools\johnsmithctl.exe start --cpu 0
```

Record the printed seed. A later client must use the same seed while that
driver instance is running. Stop and remove the service with:

```powershell
.\build\bin\tools\johnsmithctl.exe stop
```

Use only a disposable test system with Secure Boot, Hyper-V, VBS, and HVCI
disabled. Do not terminate the loader while DSE is disabled.

## Verify the running artifact

Build success does not prove that Windows loaded that build:

```powershell
sc.exe qc JohnSmith
Get-FileHash .\build\bin\Release\JohnSmith.sys -Algorithm SHA256
sc.exe query JohnSmith
```

Record the service path and SHA-256 with every runtime log. A stale service path
invalidates code-to-log conclusions.

## VM-exit benchmark

```powershell
.\build\bin\tools\vmexit-bench.exe 200000
```

For the transition floor:

```powershell
msbuild .\JohnSmith.sln /m /p:Configuration=Benchmark /p:Platform=x64
.\build\bin\tools\johnsmithctl.exe start --cpu 0
.\build\bin\tools\vmexit-bench.exe 200000 --vmcall
```

`0xC000001D` is expected on bare metal and with Debug/Release. If it occurs with
the intended Benchmark build, verify the service path and hash before changing
the handler.

## Verification matrix

| Check | Scope | Build-only acceptable? |
| --- | --- | --- |
| Support probe | Intel and AMD | No |
| All-CPU launch and rollback | Intel and AMD | No |
| Live SLAT permission change | Intel INVEPT and AMD VMMCALL/TLB_CONTROL | No |
| CPUID policy | Intel and AMD | No |
| Execute-hook install/remove/query | Intel | No |
| Teardown and state restoration | Intel and AMD | No |
| Debug/Release/Benchmark compile | Solution | Yes |
| WDK analysis | Driver | Yes |

AMD compilation on an Intel system is not AMD runtime proof. Keep that boundary
explicit in reviews and release notes.

## Intel rendezvous policy self-check

Run the portable policy check from the repository root:

```powershell
cl /nologo /std:c17 /W4 /WX /TC /I .\src\intel `
  .\tools\intel-rendezvous-policy-selfcheck.c `
  /Fe:"$env:TEMP\johnsmith-rendezvous-policy-selfcheck.exe"
& "$env:TEMP\johnsmith-rendezvous-policy-selfcheck.exe"
```

It covers policy classification, exact budget behavior, excluded-exit budget
preservation, ICR encoding, the required NMI-exiting and virtual-NMI pin-control
bits, and the required TSC-offset primary-control bit. The hardware-only
boundary is LAPIC delivery, NMI callback timing, VMCS writes, timeout release,
and resume skew.

## Pull-request checklist

- [ ] Debug, Release, and Benchmark build without warnings.
- [ ] Release WDK analysis passes.
- [ ] Project XML and PowerShell scripts parse.
- [ ] `git diff --check` passes.
- [ ] Enabled intercepts have a complete handler and exception policy.
- [ ] Assembly-visible layouts have compile-time assertions.
- [ ] Manual revision and section are cited.
- [ ] Hardware results identify artifact, platform, and configuration.
