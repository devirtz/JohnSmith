# Intel CPUID Transition Fast Path Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce the real Intel leaf-0 CPUID VM-transition cost without dynamic TSC-offset concealment or changes to rendezvous freeze compensation.

**Architecture:** Keep the existing cached assembly micropath, remove redundant leaf-0 register saves, and allow PAT/EFER to remain live across root/non-root transitions only when every related control permits zero. Retain hardware debug-state management and fall back to complete PAT/EFER save-load triplets on processors that force any member of a triplet.

**Tech Stack:** C17 Windows kernel driver, MASM x64, Intel VMX, MSVC/WDK 10.0.26100, portable assert self-check.

---

## File Structure

- Modify `src/intel/intel_rendezvous_policy.h`: add the portable VMX capability predicate used by production code and the self-check.
- Modify `tools/intel-rendezvous-policy-selfcheck.c`: cover persistent and hardware-managed transition decisions.
- Modify `src/intel/intel_vmcs.c`: select all-or-none PAT/EFER controls from cached capability MSRs.
- Modify `src/intel/intel_exit.c`: capture live PAT/EFER during shutdown when transition controls are disabled.
- Modify `asm/intel.asm`: save only `r8/r9` on the leaf-0 micropath and isolate Benchmark VMCALL saves.

### Task 1: Add the failing transition-policy self-check

**Files:**
- Modify: `tools/intel-rendezvous-policy-selfcheck.c:3-105`

- [ ] **Step 1: Write the failing checks**

Add these declarations near the start of `main`:

```c
    const unsigned patExitControls = (1u << 18) | (1u << 19);
    const unsigned patEntryControls = 1u << 14;
    const unsigned eferExitControls = (1u << 20) | (1u << 21);
    const unsigned eferEntryControls = 1u << 15;
```

Add these assertions after the existing VMX toggleability assertions:

```c
    assert(IntelVmxTransitionStateCanPersist(
        0ull, 0ull, patExitControls, patEntryControls));
    assert(!IntelVmxTransitionStateCanPersist(
        1ull << 18, 0ull, patExitControls, patEntryControls));
    assert(!IntelVmxTransitionStateCanPersist(
        0ull, 1ull << 14, patExitControls, patEntryControls));
    assert(IntelVmxTransitionStateCanPersist(
        1ull << 20, 1ull << 15, patExitControls, patEntryControls));
    assert(!IntelVmxTransitionStateCanPersist(
        1ull << 20, 0ull, eferExitControls, eferEntryControls));
    assert(!IntelVmxTransitionStateCanPersist(
        0ull, 1ull << 15, eferExitControls, eferEntryControls));
    assert(IntelVmxTransitionStateCanPersist(
        ~0ull << 32, ~0ull << 32,
        eferExitControls, eferEntryControls));
```

- [ ] **Step 2: Run the self-check build and verify RED**

Run from a Visual Studio developer shell:

```powershell
cl /nologo /std:c17 /W4 /WX /TC /I .\src\intel `
  .\tools\intel-rendezvous-policy-selfcheck.c `
  /Fe:"$env:TEMP\johnsmith-rendezvous-policy-selfcheck.exe"
```

Expected: compilation fails because `IntelVmxTransitionStateCanPersist` is not defined.

### Task 2: Implement the portable transition predicate

**Files:**
- Modify: `src/intel/intel_rendezvous_policy.h:26-35`
- Test: `tools/intel-rendezvous-policy-selfcheck.c`

- [ ] **Step 1: Add the minimum implementation**

Add immediately after `IntelVmxControlsAreToggleable`:

```c
static inline int
IntelVmxTransitionStateCanPersist(
    unsigned long long exitCapability,
    unsigned long long entryCapability,
    unsigned exitControls,
    unsigned entryControls
    )
{
    return ((unsigned)exitCapability & exitControls) == 0 &&
           ((unsigned)entryCapability & entryControls) == 0;
}
```

- [ ] **Step 2: Run the self-check and verify GREEN**

Run:

```powershell
cl /nologo /std:c17 /W4 /WX /TC /I .\src\intel `
  .\tools\intel-rendezvous-policy-selfcheck.c `
  /Fe:"$env:TEMP\johnsmith-rendezvous-policy-selfcheck.exe"
& "$env:TEMP\johnsmith-rendezvous-policy-selfcheck.exe"
```

Expected: compiler exit code `0`, executable exit code `0`, no output.

### Task 3: Select persistent PAT/EFER transition state

**Files:**
- Modify: `src/intel/intel_vmcs.c:62-70, 101-128, 149-158, 224-237`
- Modify: `src/intel/intel_exit.c:865-900`

- [ ] **Step 1: Make control adjustment consume a cached capability**

Replace `IntelAdjustControls` with:

```c
static ULONG
IntelAdjustControls(
    _In_ ULONG Desired,
    _In_ ULONG64 Capability
    )
{
    return (Desired | (ULONG)Capability) & (ULONG)(Capability >> 32);
}
```

Add `ULONG64 exitCapability;` and `ULONG64 entryCapability;` beside
`primaryCapability`. Read them after choosing `exitMsr` and `entryMsr`:

```c
    primaryCapability = __readmsr(primaryMsr);
    exitCapability = __readmsr(exitMsr);
    entryCapability = __readmsr(entryMsr);
```

Pass capabilities rather than MSR indexes:

```c
    pinControls = IntelAdjustControls(desiredPin, __readmsr(pinMsr));
    primaryControls = IntelAdjustControls(
        desiredPrimary, primaryCapability);
    secondaryControls = IntelAdjustControls(
        desiredSecondary, __readmsr(IA32_VMX_PROCBASED_CTLS2));
```

- [ ] **Step 2: Build all-or-none PAT and EFER triplets**

Replace the fixed exit/entry requirements with:

```c
    requiredExit = VMX_EXIT_SAVE_DEBUG_CONTROLS |
                   VMX_EXIT_HOST_ADDRESS_SPACE_SIZE;
    requiredEntry = VMX_ENTRY_LOAD_DEBUG_CONTROLS |
                    VMX_ENTRY_IA32E_MODE;

    if (!IntelVmxTransitionStateCanPersist(
            exitCapability,
            entryCapability,
            VMX_EXIT_SAVE_PAT | VMX_EXIT_LOAD_PAT,
            VMX_ENTRY_LOAD_PAT)) {
        requiredExit |= VMX_EXIT_SAVE_PAT | VMX_EXIT_LOAD_PAT;
        requiredEntry |= VMX_ENTRY_LOAD_PAT;
    }
    if (!IntelVmxTransitionStateCanPersist(
            exitCapability,
            entryCapability,
            VMX_EXIT_SAVE_EFER | VMX_EXIT_LOAD_EFER,
            VMX_ENTRY_LOAD_EFER)) {
        requiredExit |= VMX_EXIT_SAVE_EFER | VMX_EXIT_LOAD_EFER;
        requiredEntry |= VMX_ENTRY_LOAD_EFER;
    }

    exitControls = IntelAdjustControls(requiredExit, exitCapability);
    entryControls = IntelAdjustControls(requiredEntry, entryCapability);
```

Do not change the existing required-control validation or PAT/EFER VMCS field initialization.

- [ ] **Step 3: Capture live MSRs in persistent mode**

At the start of `IntelCaptureStopState`, before its final return chain, add:

```c
    if ((Context->ExitControls & VMX_EXIT_SAVE_PAT) != 0) {
        if (!IntelVmReadValue(VMCS_GUEST_PAT, &Context->ResumePat)) {
            return FALSE;
        }
    } else {
        Context->ResumePat = __readmsr(IA32_PAT);
    }

    if ((Context->ExitControls & VMX_EXIT_SAVE_EFER) != 0) {
        if (!IntelVmReadValue(VMCS_GUEST_EFER, &Context->ResumeEfer)) {
            return FALSE;
        }
    } else {
        Context->ResumeEfer = __readmsr(IA32_EFER);
    }
```

Remove the two PAT/EFER `IntelVmReadValue` terms from the final return chain.

- [ ] **Step 4: Build Release to verify the policy integration**

Run:

```powershell
msbuild .\JohnSmith.vcxproj /m /p:Configuration=Release /p:Platform=x64
```

Expected: exit code `0`, no compiler or linker errors.

### Task 4: Add the failing assembly structural check

**Files:**
- Test: one-off PowerShell assertion against `asm/intel.asm`

- [ ] **Step 1: Run the failing assertion**

Run:

```powershell
$source = Get-Content -Raw .\asm\intel.asm
$leaf0 = $source.Split('IntelVmExitCheckBenchmarkVmcall:')[0]
if ($leaf0 -match '(?m)^\s+push r10\s*$' -or
    $leaf0 -match '(?m)^\s+push r11\s*$' -or
    $leaf0 -match '(?m)^\s+lea r10,') {
    throw 'Leaf-0 prefix still saves r10/r11.'
}
```

Expected: throws `Leaf-0 prefix still saves r10/r11.`.

### Task 5: Trim the leaf-0 assembly path

**Files:**
- Modify: `asm/intel.asm:87-185`

- [ ] **Step 1: Use only `r8/r9` for the common probe**

Replace the fast-probe block from `IntelAsmVmExit PROC` through the jump to
`IntelVmExitSlowPath` with:

```asm
IntelAsmVmExit PROC
    ; Leaf-0 CPUID micropath: no C, no EPT flush, no rendezvous join.
    ; Guest GPRs still live in host registers on exit. Host RSP is the
    ; INTEL_HOST_STACK_FRAME (VMCS HOST_RSP). Fast path only when enabled
    ; and rendezvous is idle so SLAT/epoch work stays on the C path.
    push r8
    push r9

    cmp qword ptr [rsp + 16 + HOST_FRAME_FAST_PATH_ENABLED], 1
    jne IntelVmExitAfterFastProbe

    mov r8d, VMCS_EXIT_REASON
    vmread r9, r8
    jbe IntelVmExitAfterFastProbe
    and r9d, 0FFFFh
    cmp r9d, VMX_EXIT_CPUID
    jne IntelVmExitCheckBenchmarkVmcall

    ; Only leaf 0. Hypercall uses leaf 1 + magic subleaf → C path.
    test eax, eax
    jnz IntelVmExitAfterFastProbe

    mov r8, qword ptr [rsp + 16 + HOST_FRAME_RENDEZVOUS_PHASE]
    test r8, r8
    jz IntelVmExitAfterFastProbe
    cmp dword ptr [r8], INTEL_RENDEZVOUS_IDLE
    jne IntelVmExitAfterFastProbe

    ; CPUID is always 0F A2 (length 2). Skip EXIT_INSTRUCTION_LENGTH VMREAD.
    ; Advance RIP before clobbering guest GPRs so a failed VMWRITE can fall
    ; through to the C path with the original guest register state.
    mov r8d, VMCS_GUEST_RIP
    vmread r9, r8
    jbe IntelVmExitAfterFastProbe
    add r9, 2
    vmwrite r9, r8
    jbe IntelVmExitAfterFastProbe

    mov eax, dword ptr [rsp + 16 + HOST_FRAME_CPUID_LEAF0_EAX]
    mov ebx, dword ptr [rsp + 16 + HOST_FRAME_CPUID_LEAF0_EBX]
    mov ecx, dword ptr [rsp + 16 + HOST_FRAME_CPUID_LEAF0_ECX]
    mov edx, dword ptr [rsp + 16 + HOST_FRAME_CPUID_LEAF0_EDX]

    pop r9
    pop r8
    vmresume
    ud2

IntelVmExitCheckBenchmarkVmcall:
IF JOHNSMITH_VMEXIT_BENCHMARK
    push r10
    push r11

    cmp r9d, VMX_EXIT_VMCALL
    jne IntelVmExitBenchmarkProbeMiss
    mov r8, 04A534D5642454E43h
    cmp rax, r8
    jne IntelVmExitBenchmarkProbeMiss
    mov r8, 0484D41524B464C52h
    cmp rcx, r8
    jne IntelVmExitBenchmarkProbeMiss
    mov r8, 0564D43414C4C3031h
    cmp rdx, r8
    jne IntelVmExitBenchmarkProbeMiss
    mov r8, 0B16B00B5DEADC0DEh
    cmp qword ptr [rsp + 24], r8
    jne IntelVmExitBenchmarkProbeMiss
    mov r8, qword ptr [rsp + 32 + HOST_FRAME_RENDEZVOUS_PHASE]
    test r8, r8
    jz IntelVmExitBenchmarkProbeMiss
    cmp dword ptr [r8], INTEL_RENDEZVOUS_IDLE
    jne IntelVmExitBenchmarkProbeMiss
    ; Benchmark-only VMCALL: exit + RIP advance + VMRESUME, no C.
    mov r8d, VMCS_EXIT_INSTRUCTION_LENGTH
    vmread r11, r8
    jbe IntelVmExitBenchmarkProbeMiss
    mov r8d, VMCS_GUEST_RIP
    vmread r9, r8
    jbe IntelVmExitBenchmarkProbeMiss
    add r9, r11
    vmwrite r9, r8
    jbe IntelVmExitBenchmarkProbeMiss

    pop r11
    pop r10
    pop r9
    pop r8
    vmresume
    ud2

IntelVmExitBenchmarkProbeMiss:
    pop r11
    pop r10
ENDIF

IntelVmExitAfterFastProbe:
    pop r9
    pop r8
    jmp IntelVmExitSlowPath

IntelVmExitSlowPath:
```

- [ ] **Step 2: Confirm the slow-path body remains byte-for-byte unchanged**

Do not edit the existing CR2 snapshot, guest register frame, XMM preservation,
C handler call, restore sequence, shutdown sequence, or final `VMRESUME` below
`IntelVmExitSlowPath`.

- [ ] **Step 3: Run the structural check and verify GREEN**

Run the Task 4 PowerShell assertion again.

Expected: exit code `0`, no output.

### Task 6: Verify source invariants and builds

**Files:**
- Verify only

- [ ] **Step 1: Run portable checks**

```powershell
cl /nologo /std:c17 /W4 /WX /TC /I .\src\intel `
  .\tools\intel-rendezvous-policy-selfcheck.c `
  /Fe:"$env:TEMP\johnsmith-rendezvous-policy-selfcheck.exe"
& "$env:TEMP\johnsmith-rendezvous-policy-selfcheck.exe"
.\build\bin\tools\hv-benchmark.exe --selfcheck
```

Expected: both executables return `0`; benchmark prints `hv-benchmark selfcheck passed`.

- [ ] **Step 2: Build every driver configuration**

```powershell
msbuild .\JohnSmith.sln /m /p:Configuration=Debug /p:Platform=x64
msbuild .\JohnSmith.sln /m /p:Configuration=Release /p:Platform=x64
msbuild .\JohnSmith.sln /m /p:Configuration=Benchmark /p:Platform=x64
```

Expected: three exit codes `0`.

- [ ] **Step 3: Verify forbidden code remains absent**

```powershell
rg -n "HideRoot|TscOffsetPtr|LastGuestTsc|TSC_HIDE" asm src
rg -n "VMCS_TSC_OFFSET" src/intel asm/intel.asm
git diff --check
```

Expected: the forbidden-symbol search returns no matches. No new
`VMCS_TSC_OFFSET` read or write appears in the CPUID path; existing references
remain limited to VMCS setup and rendezvous compensation. `git diff --check`
returns `0`.

- [ ] **Step 4: Inspect the final diff without committing pre-existing edits**

```powershell
git diff -- asm/intel.asm src/intel/intel_vmcs.c src/intel/intel_exit.c `
  src/intel/intel_rendezvous_policy.h `
  tools/intel-rendezvous-policy-selfcheck.c
git status --short
```

Expected: only requested source/test changes plus the pre-existing leaf-0
micropath baseline. Do not stage or commit the combined production diff without
explicit user direction because `asm/intel.asm` and `src/intel/intel_vmcs.c`
were already modified before this implementation.

### Task 7: Hardware acceptance on the target host

**Files:**
- Verify external behavior only

- [ ] **Step 1: Measure the transition floor**

With the intended Benchmark driver loaded on the disposable target system, run:

```powershell
.\build\bin\tools\hv-benchmark.exe 200000 --vmcall
```

Record the driver and benchmark SHA-256 hashes, `VMCALL floor`, and leaf-0 CPUID
statistics. If `VMCALL floor` is at or above 1000 TSC ticks, stop: the Pafish
threshold is below the measured transition floor.

- [ ] **Step 2: Run the exact Pafish rule**

Run Pafish and require `cpu_rdtsc_force_vmexit` to compute
`0 < average(rdtsc; cpuid(0); rdtsc) < 1000` across ten samples separated by
`Sleep(500)`.

- [ ] **Step 3: Run stability workloads**

Run the cross-core TSC monotonic stress and the prior libuv reproduction. Require
no backward TSC, freeze, or `new_time >= loop->time` assertion. Do not report
VMAware `VM::TIMER` as fixed; it is outside this TSC optimization.
