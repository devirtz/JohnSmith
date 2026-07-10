# Windows kernel and x64 ABI source map

The installed WDK headers and Microsoft Learn are normative for Windows-facing
code. Check the documented IRQL, lifetime and processor-group rules for every
call; matching a function signature is not enough.

## API map

| Project concern | Official reference | Code |
| --- | --- | --- |
| All-CPU rendezvous | [`KeIpiGenericCall`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-keipigenericcall) | start, rollback, stop, EPT/NPT invalidation |
| Active CPU count | [`KeQueryActiveProcessorCountEx`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-kequeryactiveprocessorcountex) | global CPU array sizing |
| Stable CPU index | [`KeGetCurrentProcessorIndex`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kegetcurrentprocessorindex) | per-CPU status and context lookup |
| Nonpaged allocations | [`ExAllocatePool2`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-exallocatepool2) | global, backend and per-CPU state |
| Contiguous owned page | [`MmAllocateContiguousMemorySpecifyCache`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmallocatecontiguousmemoryspecifycache) | introspection, VMX/SVM structures and maps |
| Physical RAM ranges | [Kernel DDI index](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/_kernel/) and documented [`MmGetPhysicalMemoryRangesEx2`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-mmgetphysicalmemoryrangesex2) | conservative SLAT memory typing |
| x64 assembly ABI | [x64 calling convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170) | `asm/intel.asm`, `asm/amd.asm` |
| x64 architecture | [x64 architecture overview](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/x64-architecture) | register, paging and debugger assumptions |
| Driver signing | [Windows driver signing tutorial](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/windows-driver-signing-tutorial) | test and release packaging |

## Broadcast lifecycle rules

- Size the CPU table from all processor groups and use a system-wide processor
  index consistently.
- Each broadcast callback writes only its own `HV_CPU` status and context.
- Do not treat `KeIpiGenericCall`'s single return value as per-CPU success;
  inspect every recorded status after the rendezvous.
- On partial start failure, broadcast rollback and verify every CPU that entered
  virtualization has stopped before freeing vendor memory.
- Stop must be idempotent at the common layer and must not free state while a
  callback can still reference it.
- Recheck the documented IRQL of every routine called from the broadcast worker.

## Assembly boundary rules

Windows x64 passes the first four integer arguments in RCX, RDX, R8, and R9 and
requires caller-provided shadow space. For every assembly entry point:

- keep RSP 16-byte aligned at call sites;
- reserve shadow space before calling C;
- preserve all nonvolatile general-purpose and XMM registers that the path
  modifies;
- keep unwind assumptions explicit; do not let an exception cross a hand-written
  frame without valid unwind metadata;
- document the C prototype beside the assembly symbol; and
- recheck offsets consumed by assembly with `C_ASSERT`.

## Allocation rules

- Use nonpaged storage for data touched at elevated IRQL or during VM exit.
- Check every allocation and unwind in exact reverse ownership order.
- Match contiguous allocations with the documented contiguous-memory free
  routine and pool allocations with the pool free routine.
- Zero secrets/cookies and host stacks before release when practical.
- Allocate physically constrained memory early; contiguous allocation can fail
  even when total free memory appears sufficient.

## Known contract risk

`src/intel.c` and `src/amd.c` currently call `MmGetPhysicalMemoryRanges`.
Microsoft's kernel DDI index lists that routine as reserved for system use; its
standalone public contract is not documented. `MmGetPhysicalMemoryRangesEx2` is
documented for Windows 10 version 2004 and later, but changing APIs requires a
separate compatibility and IRQL audit. Until that work is complete, treat the
current RAM-range query as **unsupported/untested**, not as a verified WDK
contract.
