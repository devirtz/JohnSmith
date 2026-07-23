# Implementation status

Audit date: **2026-07-19**.

This document lists reachable first-party code included by the Visual Studio
projects. Runtime behavior still requires validation on the target CPU and
Windows build.

## Capability matrix

| Area | Source evidence |
| --- | --- |
| Lifecycle | `src/hv.c` prepares, launches, rolls back, and stops all processors. |
| Intel VMX | `src/intel.c`, `src/intel/intel_vmcs.c`, `src/intel/intel_exit.c`, and `asm/intel.asm`. |
| Intel EPT/VPID | `src/intel/intel_slat.c` builds identity mappings, manages per-CPU views, and performs generation-based invalidation. |
| Intel execute hooks | `src/intel/intel_hook.c` installs, removes, queries, and switches dual-EPT views. |
| Intel cross-core rendezvous | `src/intel/intel_rendezvous.c` implements xAPIC/x2APIC NMI broadcast, root/non-root joining, TSC compensation, and bounded release. |
| Hook observation | `src/hook_observe.c`, `src/hook_thunk.c`, `src/hook_trampoline.c`, and `asm/hook_dispatch.asm`. |
| Intel control transport | `src/intel/intel_hypercall.c` and `src/intel/intel_hypercall_worker.c` implement the seeded CPUID/shared-page protocol. |
| AMD SVM/NPT | `src/amd.c`, `src/amd/`, `include/amd.h`, and `asm/amd.asm`. |
| Control client and loader | `tools/johnsmithctl/` starts and stops the driver, resolves exports, accesses memory, and manages hooks. |
| Hypervisor measurement | `tools/hv-benchmark/` measures CPUID, RDTSC, SERIALIZE, and Benchmark VMCALL transitions. |

## Repository checks

```powershell
msbuild .\JohnSmith.sln /m /p:Configuration=Release /p:Platform=x64
.\build\bin\tools\johnsmithctl.exe selftest
cl /nologo /std:c17 /W4 /WX /TC /I .\src\intel `
  .\tools\intel-rendezvous-policy-selfcheck.c `
  /Fe:"$env:TEMP\johnsmith-rendezvous-policy-selfcheck.exe"
& "$env:TEMP\johnsmith-rendezvous-policy-selfcheck.exe"
```

The Debug and Benchmark configurations use the same solution with their
respective `Configuration` value.
