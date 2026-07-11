#include "intel_internal.h"

VOID
IntelFlushEptIfNeeded(
    _Inout_ INTEL_CPU_CONTEXT* Context
    )
{
    INTEL_BACKEND_CONTEXT* backend =
        (INTEL_BACKEND_CONTEXT*)Context->BackendContext;
    LONG64 generation;
    INTEL_INVALIDATION_DESCRIPTOR descriptor;
    ULONG type;

    if (backend == NULL) return;
    generation = InterlockedCompareExchange64(
        &backend->SlatGeneration, 0, 0);
    if (Context->SlatGeneration == generation) return;

    descriptor.Context = Context->EptPointer;
    descriptor.Reserved = 0;
    type = (backend->EptVpidCapabilities & (1ull << 25)) != 0
        ? INVEPT_SINGLE_CONTEXT : INVEPT_ALL_CONTEXTS;
    if (type == INVEPT_ALL_CONTEXTS) descriptor.Context = 0;
    if (IntelAsmInvept(type, &descriptor) != 0) {
        KeBugCheckEx(HYPERVISOR_ERROR, INTEL_BUGCHECK_INVALIDATION,
            type, Context->EptPointer, generation);
    }
    InterlockedExchange64(&Context->SlatGeneration, generation);
}

_Function_class_(KIPI_BROADCAST_WORKER)
_IRQL_requires_(IPI_LEVEL)
static ULONG_PTR
IntelSlatRendezvous(
    _In_ ULONG_PTR Argument
    )
{
    int registers[4];
    UNREFERENCED_PARAMETER(Argument);
    __cpuid(registers, 0);
    return 0;
}

static VOID
IntelInvalidateRunningSlat(
    _Inout_ HV_STATE* State,
    _Inout_ INTEL_BACKEND_CONTEXT* Backend
    )
{
    LONG64 generation;
    ULONG index;

    generation = InterlockedIncrement64(&Backend->SlatGeneration);
    KeIpiGenericCall(IntelSlatRendezvous, 0);
    for (index = 0; index < State->CpuCount; ++index) {
        INTEL_CPU_CONTEXT* cpuContext =
            (INTEL_CPU_CONTEXT*)State->Cpus[index].VendorContext;
        if (cpuContext == NULL || cpuContext->SlatGeneration != generation) {
            KeBugCheckEx(HYPERVISOR_ERROR, INTEL_BUGCHECK_INVALIDATION,
                index, generation,
                cpuContext == NULL ? 0 : cpuContext->SlatGeneration);
        }
    }
}

static PVOID
IntelFindSplitPt(
    _In_ INTEL_BACKEND_CONTEXT* Context,
    _In_ ULONG PdptIndex,
    _In_ ULONG PdIndex
    )
{
    PLIST_ENTRY entry;

    for (entry = Context->SplitList.Flink;
         entry != &Context->SplitList;
         entry = entry->Flink) {
        INTEL_SLAT_SPLIT* split = CONTAINING_RECORD(
            entry, INTEL_SLAT_SPLIT, Link);
        if (split->PdptIndex == PdptIndex && split->PdIndex == PdIndex) {
            return split->Pt;
        }
    }
    return NULL;
}

PVOID
IntelAllocatePage(
    _In_ ULONG64 HighestAddress
    )
{
    PHYSICAL_ADDRESS low;
    PHYSICAL_ADDRESS high;
    PHYSICAL_ADDRESS boundary;
    PVOID page;

    low.QuadPart = 0;
    high.QuadPart = (LONGLONG)HighestAddress;
    boundary.QuadPart = 0;
    page = MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, low, high, boundary, MmCached);
    if (page != NULL) {
        RtlZeroMemory(page, PAGE_SIZE);
    }
    return page;
}

static BOOLEAN
IntelRangeIsRam(
    _In_opt_ PPHYSICAL_MEMORY_RANGE Ranges,
    _In_ ULONG64 Base,
    _In_ ULONG64 Size
    )
{
    ULONG index;

    if (Ranges == NULL || Base > MAXULONGLONG - Size) {
        return FALSE;
    }

    for (index = 0; Ranges[index].NumberOfBytes.QuadPart != 0; ++index) {
        ULONG64 rangeBase = (ULONG64)Ranges[index].BaseAddress.QuadPart;
        ULONG64 rangeSize = (ULONG64)Ranges[index].NumberOfBytes.QuadPart;

        if (Base >= rangeBase &&
            Base - rangeBase <= rangeSize &&
            Size <= rangeSize - (Base - rangeBase)) {
            return TRUE;
        }
    }

    return FALSE;
}

static ULONG64
IntelPhysicalLimit(
    VOID
    )
{
    int registers[4];
    ULONG bits;
    ULONG64 limit;

    __cpuid(registers, 0x80000000);
    if ((ULONG)registers[0] < 0x80000008u) {
        return 1ull << 36;
    }

    __cpuid(registers, 0x80000008);
    bits = (ULONG)registers[0] & 0xffu;
    if (bits >= 63) {
        limit = MAXLONGLONG;
    } else {
        limit = 1ull << bits;
    }

    return min(limit, HV_SLAT_MAXIMUM_ADDRESS);
}

NTSTATUS
IntelBuildEpt(
    _Inout_ INTEL_BACKEND_CONTEXT* Context
    )
{
    PPHYSICAL_MEMORY_RANGE ranges;
    ULONG pdptCount;
    ULONG pdptIndex;
    ULONG pdIndex;
    ULONG64* pml4;
    ULONG64* pdpt;

    Context->MapLimit = IntelPhysicalLimit();
    if (Context->MapLimit < (1ull << 32)) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }

    Context->Pml4 = IntelAllocatePage(MAXLONGLONG);
    Context->Pdpt = IntelAllocatePage(MAXLONGLONG);
    if (Context->Pml4 == NULL || Context->Pdpt == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pml4 = (ULONG64*)Context->Pml4;
    pdpt = (ULONG64*)Context->Pdpt;
    pml4[0] = ((ULONG64)MmGetPhysicalAddress(Context->Pdpt).QuadPart &
               EPT_ADDRESS_MASK) | EPT_ACCESS_MASK;

    ranges = MmGetPhysicalMemoryRanges();
    pdptCount = (ULONG)((Context->MapLimit + ((1ull << 30) - 1)) >> 30);
    for (pdptIndex = 0; pdptIndex < pdptCount; ++pdptIndex) {
        ULONG64* pd;

        Context->Pds[pdptIndex] = IntelAllocatePage(MAXLONGLONG);
        if (Context->Pds[pdptIndex] == NULL) {
            if (ranges != NULL) {
                ExFreePool(ranges);
            }
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        pd = (ULONG64*)Context->Pds[pdptIndex];
        pdpt[pdptIndex] =
            ((ULONG64)MmGetPhysicalAddress(pd).QuadPart & EPT_ADDRESS_MASK) |
            EPT_ACCESS_MASK;

        for (pdIndex = 0; pdIndex < 512; ++pdIndex) {
            ULONG64 physical = ((ULONG64)pdptIndex << 30) |
                               ((ULONG64)pdIndex << 21);
            ULONG64 memoryType;

            if (physical >= Context->MapLimit) {
                break;
            }
            memoryType = IntelRangeIsRam(ranges, physical, 1ull << 21)
                ? EPT_MEMORY_TYPE_WB
                : 0;
            pd[pdIndex] = (physical & EPT_2MB_ADDRESS_MASK) |
                          EPT_ACCESS_MASK |
                          EPT_LARGE_PAGE |
                          (memoryType << EPT_MEMORY_TYPE_SHIFT);
        }
    }

    if (ranges != NULL) {
        ExFreePool(ranges);
    }
    return STATUS_SUCCESS;
}

VOID
IntelFreeEpt(
    _Inout_ INTEL_BACKEND_CONTEXT* Context
    )
{
    while (!IsListEmpty(&Context->SplitList)) {
        PLIST_ENTRY entry = RemoveHeadList(&Context->SplitList);
        INTEL_SLAT_SPLIT* split = CONTAINING_RECORD(
            entry, INTEL_SLAT_SPLIT, Link);

        MmFreeContiguousMemory(split->Pt);
        ExFreePoolWithTag(split, HV_POOL_TAG_SLAT_SPLIT);
    }

    for (ULONG index = 0; index < RTL_NUMBER_OF(Context->Pds); ++index) {
        if (Context->Pds[index] != NULL) {
            MmFreeContiguousMemory(Context->Pds[index]);
            Context->Pds[index] = NULL;
        }
    }
    if (Context->Pdpt != NULL) {
        MmFreeContiguousMemory(Context->Pdpt);
        Context->Pdpt = NULL;
    }
    if (Context->Pml4 != NULL) {
        MmFreeContiguousMemory(Context->Pml4);
        Context->Pml4 = NULL;
    }
}

static NTSTATUS
IntelValidateOwnedAddress(
    _In_ INTEL_BACKEND_CONTEXT* Context,
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _Out_ ULONG* PdptIndex,
    _Out_ ULONG* PdIndex,
    _Out_ ULONG* PtIndex
    )
{
    ULONG64 address = (ULONG64)PhysicalAddress.QuadPart;

    if (PhysicalAddress.QuadPart < 0 ||
        (address & (PAGE_SIZE - 1)) != 0) {
        return STATUS_DATATYPE_MISALIGNMENT;
    }
    if (address >= Context->MapLimit ||
        address >= HV_SLAT_MAXIMUM_ADDRESS) {
        return STATUS_INVALID_ADDRESS;
    }

    *PdptIndex = (ULONG)(address >> 30);
    *PdIndex = (ULONG)((address >> 21) & 0x1ff);
    *PtIndex = (ULONG)((address >> 12) & 0x1ff);
    return STATUS_SUCCESS;
}

static BOOLEAN
IntelSlatMayChange(
    _In_ HV_STATE* State
    )
{
    LONG lifecycle = InterlockedCompareExchange(&State->Lifecycle, 0, 0);
    ULONG index;

    if (lifecycle == HV_LIFECYCLE_STARTING ||
        lifecycle == HV_LIFECYCLE_RUNNING) {
        return TRUE;
    }
    if (lifecycle != HV_LIFECYCLE_STOPPING) {
        return FALSE;
    }
    for (index = 0; index < State->CpuCount; ++index) {
        if (InterlockedCompareExchange(&State->Cpus[index].State, 0, 0) !=
            HV_CPU_PREPARED) {
            return FALSE;
        }
    }
    return TRUE;
}

NTSTATUS
IntelQueryOwnedPageAccess(
    _Inout_ HV_STATE* State,
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _Out_ HV_PAGE_ACCESS* Access
    )
{
    INTEL_BACKEND_CONTEXT* context;
    ULONG pdptIndex;
    ULONG pdIndex;
    ULONG ptIndex;
    ULONG64 entry;
    NTSTATUS status;

    if (State == NULL || Access == NULL || State->BackendContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    context = (INTEL_BACKEND_CONTEXT*)State->BackendContext;
    status = IntelValidateOwnedAddress(
        context, PhysicalAddress, &pdptIndex, &pdIndex, &ptIndex);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&context->SlatLock);
    entry = ((ULONG64*)context->Pds[pdptIndex])[pdIndex];
    if ((entry & EPT_LARGE_PAGE) == 0) {
        ULONG64* pt = (ULONG64*)IntelFindSplitPt(
            context, pdptIndex, pdIndex);
        if (pt == NULL) {
            status = STATUS_DATA_ERROR;
            goto Exit;
        }
        entry = pt[ptIndex];
    }
    *Access = (HV_PAGE_ACCESS)(entry & EPT_ACCESS_MASK);
    status = STATUS_SUCCESS;

Exit:
    ExReleasePushLockShared(&context->SlatLock);
    KeLeaveCriticalRegion();
    return status;
}

NTSTATUS
IntelSetOwnedPageAccess(
    _Inout_ HV_STATE* State,
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ HV_PAGE_ACCESS Access,
    _Out_ HV_PAGE_ACCESS* PreviousAccess
    )
{
    INTEL_BACKEND_CONTEXT* context;
    INTEL_SLAT_SPLIT* split = NULL;
    ULONG pdptIndex;
    ULONG pdIndex;
    ULONG ptIndex;
    ULONG64* pd;
    ULONG64* pt;
    ULONG64 pde;
    ULONG64 pte;
    NTSTATUS status;
    BOOLEAN invalidate = FALSE;

    if (State == NULL || PreviousAccess == NULL ||
        State->BackendContext == NULL ||
        (((ULONG)Access) & ~EPT_ACCESS_MASK) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!IntelSlatMayChange(State) || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    context = (INTEL_BACKEND_CONTEXT*)State->BackendContext;
    status = IntelValidateOwnedAddress(
        context, PhysicalAddress, &pdptIndex, &pdIndex, &ptIndex);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&context->SlatLock);
    pd = (ULONG64*)context->Pds[pdptIndex];
    pde = pd[pdIndex];
    if ((pde & EPT_LARGE_PAGE) != 0) {
        split = (INTEL_SLAT_SPLIT*)ExAllocatePool2(
            POOL_FLAG_NON_PAGED, sizeof(*split), HV_POOL_TAG_SLAT_SPLIT);
        if (split == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto Exit;
        }
        RtlZeroMemory(split, sizeof(*split));
        split->Pt = IntelAllocatePage(MAXLONGLONG);
        if (split->Pt == NULL) {
            ExFreePoolWithTag(split, HV_POOL_TAG_SLAT_SPLIT);
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto Exit;
        }

        pt = (ULONG64*)split->Pt;
        for (ULONG index = 0; index < 512; ++index) {
            pt[index] = (pde & EPT_2MB_ADDRESS_MASK) |
                        ((ULONG64)index << PAGE_SHIFT) |
                        (pde & (EPT_ACCESS_MASK | (7ull << 3)));
        }
        split->PdptIndex = pdptIndex;
        split->PdIndex = pdIndex;
        InsertTailList(&context->SplitList, &split->Link);
        KeMemoryBarrier();
        InterlockedExchange64(
            (volatile LONG64*)&pd[pdIndex],
            (LONG64)(((ULONG64)MmGetPhysicalAddress(pt).QuadPart &
                      EPT_ADDRESS_MASK) | EPT_ACCESS_MASK));
    } else {
        pt = (ULONG64*)IntelFindSplitPt(context, pdptIndex, pdIndex);
        if (pt == NULL) {
            status = STATUS_DATA_ERROR;
            goto Exit;
        }
    }

    pte = pt[ptIndex];
    *PreviousAccess = (HV_PAGE_ACCESS)(pte & EPT_ACCESS_MASK);
    InterlockedExchange64(
        (volatile LONG64*)&pt[ptIndex],
        (LONG64)((pte & ~EPT_ACCESS_MASK) | (ULONG64)Access));
    KeMemoryBarrier();
    invalidate = InterlockedCompareExchange(&State->Lifecycle, 0, 0) ==
        HV_LIFECYCLE_RUNNING;
    status = STATUS_SUCCESS;

Exit:
    ExReleasePushLockExclusive(&context->SlatLock);
    KeLeaveCriticalRegion();
    if (NT_SUCCESS(status) && invalidate) {
        IntelInvalidateRunningSlat(State, context);
    }
    return status;
}

