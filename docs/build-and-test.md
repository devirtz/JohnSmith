# Build and test

## Toolchain

- Visual Studio 2022 with Desktop C++.
- Windows Driver Kit `10.0.26100`.
- x64 Developer PowerShell or Developer Command Prompt.
- Bare-metal test system with a kernel debugger strongly recommended.

## Build configurations

```powershell
msbuild JohnSmith.sln /m /t:Rebuild /p:Configuration=Debug /p:Platform=x64
msbuild JohnSmith.sln /m /t:Rebuild /p:Configuration=Release /p:Platform=x64
msbuild JohnSmith.sln /m /t:Rebuild /p:Configuration=Benchmark /p:Platform=x64
```

| Configuration | Diagnostics | CPUID fast path | VMCALL floor |
| --- | --- | --- | --- |
| Debug | Enabled | Disabled | Disabled |
| Release | Disabled | Enabled | Disabled |
| Benchmark | Disabled | Enabled | Enabled |

Run WDK C/C++ analysis on Release:

```powershell
msbuild JohnSmith.vcxproj /m /t:Rebuild `
  /p:Configuration=Release /p:Platform=x64 /p:RunCodeAnalysis=true
```

## Load workflow

JohnSmith is unsigned. The KDU helper is intended only for disposable test
systems with Secure Boot, Hyper-V, VBS, and HVCI disabled.

```powershell
.\tools\load-kdu.ps1 -Config Release
.\tools\unload-kdu.ps1
```

The loader removes any existing service before invoking KDU, temporarily
changes DSE, creates a normal demand-start kernel service, restores DSE, and
then requests hypervisor startup through the service registry state.

Do not terminate the loader between DSE disable and its `finally` restoration.

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
.\tools\load-kdu.ps1 -Config Benchmark
.\build\bin\tools\vmexit-bench.exe 200000 --vmcall
```

`0xC000001D` is expected on bare metal and with Debug/Release. If it occurs with
the intended Benchmark build, verify the service path and hash before changing
the handler.

## Verification matrix

| Check | Intel | AMD | Build-only acceptable? |
| --- | --- | --- | --- |
| Support probe | Required | Required | No |
| All-CPU launch | Required | Required | No |
| Rollback after injected failure | Required | Required | No |
| Live SLAT permission change | INVEPT generation | VMMCALL/TLB_CONTROL generation | No |
| CPUID policy | Masked/emulated | Native | No |
| Teardown/state restoration | Required | Required | No |
| Debug/Release/Benchmark compile | Required | Required | Yes |
| WDK analysis | Required | Required | Yes |

AMD compilation on an Intel system is not AMD runtime proof. Keep that boundary
explicit in reviews and release notes.

## Pull-request checklist

- [ ] Debug, Release, and Benchmark build without warnings.
- [ ] Release WDK analysis passes.
- [ ] Project XML and PowerShell scripts parse.
- [ ] `git diff --check` passes.
- [ ] New intercepts have a complete handler and exception policy.
- [ ] Assembly-visible layouts have compile-time assertions.
- [ ] Manual revision and section are cited.
- [ ] Hardware results identify artifact, platform, and configuration.
