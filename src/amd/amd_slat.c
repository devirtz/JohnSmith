#include "amd_internal.h"

static PVOID
AmdFindSplitPt(
    _In_ AMD_BACKEND_CONTEXT* Context,
    _In_ ULONG PdptIndex,
    _In_ ULONG PdIndex
    )
{
    PLIST_ENTRY entry;

    for (entry = Context->SplitList.Flink;
         entry != &Context->SplitList;
         entry = entry->Flink) {
        AMD_SLAT_SPLIT* split = CONTAINING_RECORD(
            entry, AMD_SLAT_SPLIT, Link);
        if (split->PdptIndex == PdptIndex && split->PdIndex == PdIndex) {
            return split->Pt;
        }
    }
    return NULL;
}

VOID
AmdPrepareTlbControl(
    _Inout_ AMD_CPU_CONTEXT* Context
    )
{
    AMD_BACKEND_CONTEXT* backend =
        (AMD_BACKEND_CONTEXT*)Context->BackendContext;
    LONG64 generation;

    if (backend == NULL) return;
    generation = InterlockedCompareExchange64(
        &backend->SlatGeneration, 0, 0);
    if (Context->SlatGeneration != generation) {
        Context->Vmcb->Control.TlbControl = backend->TlbFlushCommand;
        Context->Vmcb->Control.VmcbClean &= ~AMD_VMCB_CLEAN_ASID;
        InterlockedExchange64(&Context->SlatGeneration, generation);
    } else {
        Context->Vmcb->Control.TlbControl = 0;
    }
}

_Function_class_(KIPI_BROADCAST_WORKER)
_IRQL_requires_(IPI_LEVEL)
static ULONG_PTR
AmdSlatRendezvous(
    _In_ ULONG_PTR Argument
    )
{
    int registers[4];
    UNREFERENCED_PARAMETER(Argument);
    __cpuid(registers, 0);
    return 0;
}

static VOID
AmdInvalidateRunningSlat(
    _Inout_ HV_STATE* State,
    _Inout_ AMD_BACKEND_CONTEXT* Backend
    )
{
    LONG64 generation = InterlockedIncrement64(&Backend->SlatGeneration);
    ULONG index;

    KeIpiGenericCall(AmdSlatRendezvous, 0);
    for (index = 0; index < State->CpuCount; ++index) {
        AMD_CPU_CONTEXT* cpuContext =
            (AMD_CPU_CONTEXT*)State->Cpus[index].VendorContext;
        if (cpuContext == NULL || cpuContext->SlatGeneration != generation) {
            KeBugCheckEx(HYPERVISOR_ERROR, AMD_BUGCHECK_INVALIDATION,
                index, generation,
                cpuContext == NULL ? 0 : cpuContext->SlatGeneration);
        }
    }
}

PVOID
AmdAllocateContiguous(
    _In_ SIZE_T Size
    )
{
    PHYSICAL_ADDRESS low;
    PHYSICAL_ADDRESS high;
    PHYSICAL_ADDRESS boundary;
    PVOID page;

    low.QuadPart = 0;
    high.QuadPart = MAXLONGLONG;
    boundary.QuadPart = 0;
    page = MmAllocateContiguousMemorySpecifyCache(
        Size, low, high, boundary, MmCached);
    if (page != NULL) {
        RtlZeroMemory(page, Size);
    }
    return page;
}

PVOID
AmdAllocatePage(
    VOID
    )
{
    return AmdAllocateContiguous(PAGE_SIZE);
}

static BOOLEAN
AmdRangeIsRam(
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

static BOOLEAN
AmdRangeIntersectsRam(
    _In_ PPHYSICAL_MEMORY_RANGE Ranges,
    _In_ ULONG64 Base,
    _In_ ULONG64 Size
    )
{
    ULONG index;
    ULONG64 end;

    if (Ranges == NULL || Size == 0 || Base > MAXULONGLONG - Size) {
        return FALSE;
    }
    end = Base + Size;

    for (index = 0; Ranges[index].NumberOfBytes.QuadPart != 0; ++index) {
        ULONG64 rangeBase = (ULONG64)Ranges[index].BaseAddress.QuadPart;
        ULONG64 rangeSize = (ULONG64)Ranges[index].NumberOfBytes.QuadPart;
        ULONG64 rangeEnd = rangeBase > MAXULONGLONG - rangeSize
            ? MAXULONGLONG : rangeBase + rangeSize;

        if (Base < rangeEnd && rangeBase < end) {
            return TRUE;
        }
    }

    return FALSE;
}

static NTSTATUS
AmdCreateMixedMapping(
    _Inout_ AMD_BACKEND_CONTEXT* Context,
    _In_ PPHYSICAL_MEMORY_RANGE Ranges,
    _In_ ULONG PdptIndex,
    _In_ ULONG PdIndex,
    _In_ ULONG64 PhysicalAddress,
    _Out_ ULONG64* Entry
    )
{
    AMD_SLAT_SPLIT* split;
    ULONG64* pt;
    ULONG index;

    split = (AMD_SLAT_SPLIT*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*split), HV_POOL_TAG_SLAT_SPLIT);
    if (split == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(split, sizeof(*split));
    split->Pt = AmdAllocatePage();
    if (split->Pt == NULL) {
        ExFreePoolWithTag(split, HV_POOL_TAG_SLAT_SPLIT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pt = (ULONG64*)split->Pt;
    for (index = 0; index < 512; ++index) {
        ULONG64 pageAddress = PhysicalAddress +
                              ((ULONG64)index << PAGE_SHIFT);
        ULONG64 cacheFlags = AmdRangeIsRam(
            Ranges, pageAddress, PAGE_SIZE)
            ? Context->RamCacheFlags : Context->MmioCacheFlags;

        pt[index] = (pageAddress & NPT_ADDRESS_MASK) |
                    NPT_TABLE_PERMISSIONS |
                    cacheFlags;
    }

    split->PdptIndex = PdptIndex;
    split->PdIndex = PdIndex;
    InsertTailList(&Context->SplitList, &split->Link);
    *Entry = ((ULONG64)MmGetPhysicalAddress(pt).QuadPart &
              NPT_ADDRESS_MASK) | NPT_TABLE_PERMISSIONS;
    return STATUS_SUCCESS;
}

static ULONG64
AmdPhysicalLimit(
    VOID
    )
{
    int registers[4];
    ULONG bits;
    ULONG64 limit;

    __cpuid(registers, 0x80000008);
    bits = (ULONG)registers[0] & 0xffu;
    limit = bits >= 63 ? MAXLONGLONG : (1ull << bits);
    return min(limit, HV_SLAT_MAXIMUM_ADDRESS);
}

NTSTATUS
AmdBuildNpt(
    _Inout_ AMD_BACKEND_CONTEXT* Context
    )
{
    PPHYSICAL_MEMORY_RANGE ranges;
    ULONG64* pml4;
    ULONG64* pdpt;
    ULONG pdptCount;
    ULONG pdptIndex;

    Context->MapLimit = AmdPhysicalLimit();
    if (Context->MapLimit < (1ull << 32)) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }
    Context->Pml4 = AmdAllocatePage();
    Context->Pdpt = AmdAllocatePage();
    if (Context->Pml4 == NULL || Context->Pdpt == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pml4 = (ULONG64*)Context->Pml4;
    pdpt = (ULONG64*)Context->Pdpt;
    pml4[0] = ((ULONG64)MmGetPhysicalAddress(Context->Pdpt).QuadPart &
               NPT_ADDRESS_MASK) | NPT_TABLE_PERMISSIONS;
    ranges = MmGetPhysicalMemoryRangesEx2(NULL, 0);
    if (ranges == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    pdptCount = (ULONG)((Context->MapLimit + ((1ull << 30) - 1)) >> 30);
    for (pdptIndex = 0; pdptIndex < pdptCount; ++pdptIndex) {
        ULONG64* pd;
        ULONG pdIndex;

        Context->Pds[pdptIndex] = AmdAllocatePage();
        if (Context->Pds[pdptIndex] == NULL) {
            if (ranges != NULL) ExFreePool(ranges);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        pd = (ULONG64*)Context->Pds[pdptIndex];
        pdpt[pdptIndex] =
            ((ULONG64)MmGetPhysicalAddress(pd).QuadPart & NPT_ADDRESS_MASK) |
            NPT_TABLE_PERMISSIONS;

        for (pdIndex = 0; pdIndex < 512; ++pdIndex) {
            ULONG64 physical = ((ULONG64)pdptIndex << 30) |
                               ((ULONG64)pdIndex << 21);
            NTSTATUS status;
            if (physical >= Context->MapLimit) break;
            if (AmdRangeIsRam(ranges, physical, 1ull << 21)) {
                pd[pdIndex] = (physical & NPT_2MB_ADDRESS_MASK) |
                              NPT_TABLE_PERMISSIONS |
                              NPT_LARGE_PAGE |
                              Context->RamCacheFlags;
            } else if (AmdRangeIntersectsRam(
                           ranges, physical, 1ull << 21)) {
                status = AmdCreateMixedMapping(
                    Context, ranges, pdptIndex, pdIndex,
                    physical, &pd[pdIndex]);
                if (!NT_SUCCESS(status)) {
                    ExFreePool(ranges);
                    return status;
                }
            } else {
                pd[pdIndex] = (physical & NPT_2MB_ADDRESS_MASK) |
                              NPT_TABLE_PERMISSIONS |
                              NPT_LARGE_PAGE |
                              Context->MmioCacheFlags;
            }
        }
    }

    ExFreePool(ranges);
    return STATUS_SUCCESS;
}

VOID
AmdFreeNpt(
    _Inout_ AMD_BACKEND_CONTEXT* Context
    )
{
    ULONG index;

    while (!IsListEmpty(&Context->SplitList)) {
        PLIST_ENTRY entry = RemoveHeadList(&Context->SplitList);
        AMD_SLAT_SPLIT* split = CONTAINING_RECORD(
            entry, AMD_SLAT_SPLIT, Link);
        MmFreeContiguousMemory(split->Pt);
        ExFreePoolWithTag(split, HV_POOL_TAG_SLAT_SPLIT);
    }
    for (index = 0; index < RTL_NUMBER_OF(Context->Pds); ++index) {
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
AmdValidateOwnedAddress(
    _In_ AMD_BACKEND_CONTEXT* Context,
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
AmdSlatMayChange(
    _In_ HV_STATE* State
    )
{
    LONG lifecycle = InterlockedCompareExchange(&State->Lifecycle, 0, 0);
    ULONG index;
    if (lifecycle == HV_LIFECYCLE_STARTING ||
        lifecycle == HV_LIFECYCLE_RUNNING) return TRUE;
    if (lifecycle != HV_LIFECYCLE_STOPPING) return FALSE;
    for (index = 0; index < State->CpuCount; ++index) {
        if (InterlockedCompareExchange(&State->Cpus[index].State, 0, 0) !=
            HV_CPU_PREPARED) {
            return FALSE;
        }
    }
    return TRUE;
}

static HV_PAGE_ACCESS
AmdEntryAccess(
    _In_ ULONG64 Entry
    )
{
    ULONG access = 0;
    if ((Entry & NPT_PRESENT) != 0) {
        access |= HV_PAGE_ACCESS_READ;
        if ((Entry & NPT_WRITE) != 0) access |= HV_PAGE_ACCESS_WRITE;
        if ((Entry & NPT_NO_EXECUTE) == 0) {
            access |= HV_PAGE_ACCESS_EXECUTE;
        }
    }
    return (HV_PAGE_ACCESS)access;
}

static ULONG64
AmdApplyAccess(
    _In_ ULONG64 Entry,
    _In_ HV_PAGE_ACCESS Access
    )
{
    Entry &= ~(NPT_PRESENT | NPT_WRITE | NPT_NO_EXECUTE);
    if ((((ULONG)Access) & HV_PAGE_ACCESS_READ) != 0) Entry |= NPT_PRESENT;
    if ((((ULONG)Access) & HV_PAGE_ACCESS_WRITE) != 0) Entry |= NPT_WRITE;
    if ((((ULONG)Access) & HV_PAGE_ACCESS_EXECUTE) == 0) {
        Entry |= NPT_NO_EXECUTE;
    }
    return Entry;
}

NTSTATUS
AmdQueryOwnedPageAccess(
    _Inout_ HV_STATE* State,
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _Out_ HV_PAGE_ACCESS* Access
    )
{
    AMD_BACKEND_CONTEXT* context;
    ULONG pdptIndex, pdIndex, ptIndex;
    ULONG64 entry;
    NTSTATUS status;

    if (State == NULL || Access == NULL || State->BackendContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    context = (AMD_BACKEND_CONTEXT*)State->BackendContext;
    status = AmdValidateOwnedAddress(
        context, PhysicalAddress, &pdptIndex, &pdIndex, &ptIndex);
    if (!NT_SUCCESS(status)) return status;

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&context->SlatLock);
    entry = ((ULONG64*)context->Pds[pdptIndex])[pdIndex];
    if ((entry & NPT_LARGE_PAGE) == 0) {
        ULONG64* pt = (ULONG64*)AmdFindSplitPt(
            context, pdptIndex, pdIndex);
        if (pt == NULL) {
            status = STATUS_DATA_ERROR;
            goto Exit;
        }
        entry = pt[ptIndex];
    }
    *Access = AmdEntryAccess(entry);
    status = STATUS_SUCCESS;
Exit:
    ExReleasePushLockShared(&context->SlatLock);
    KeLeaveCriticalRegion();
    return status;
}

NTSTATUS
AmdSetOwnedPageAccess(
    _Inout_ HV_STATE* State,
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ HV_PAGE_ACCESS Access,
    _Out_ HV_PAGE_ACCESS* PreviousAccess
    )
{
    const ULONG known = HV_PAGE_ACCESS_READ | HV_PAGE_ACCESS_WRITE |
                        HV_PAGE_ACCESS_EXECUTE;
    AMD_BACKEND_CONTEXT* context;
    AMD_SLAT_SPLIT* split;
    ULONG pdptIndex, pdIndex, ptIndex;
    ULONG64* pd;
    ULONG64* pt;
    ULONG64 pde;
    ULONG64 pte;
    NTSTATUS status;
    BOOLEAN invalidate = FALSE;

    if (State == NULL || PreviousAccess == NULL ||
        State->BackendContext == NULL || (((ULONG)Access) & ~known) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((((ULONG)Access) & (HV_PAGE_ACCESS_WRITE | HV_PAGE_ACCESS_EXECUTE)) !=
        0 && (((ULONG)Access) & HV_PAGE_ACCESS_READ) == 0) {
        return STATUS_NOT_SUPPORTED;
    }
    if (!AmdSlatMayChange(State) || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    context = (AMD_BACKEND_CONTEXT*)State->BackendContext;
    status = AmdValidateOwnedAddress(
        context, PhysicalAddress, &pdptIndex, &pdIndex, &ptIndex);
    if (!NT_SUCCESS(status)) return status;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&context->SlatLock);
    pd = (ULONG64*)context->Pds[pdptIndex];
    pde = pd[pdIndex];
    if ((pde & NPT_LARGE_PAGE) != 0) {
        split = (AMD_SLAT_SPLIT*)ExAllocatePool2(
            POOL_FLAG_NON_PAGED, sizeof(*split), HV_POOL_TAG_SLAT_SPLIT);
        if (split == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto Exit;
        }
        RtlZeroMemory(split, sizeof(*split));
        split->Pt = AmdAllocatePage();
        if (split->Pt == NULL) {
            ExFreePoolWithTag(split, HV_POOL_TAG_SLAT_SPLIT);
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto Exit;
        }
        pt = (ULONG64*)split->Pt;
        for (ULONG index = 0; index < 512; ++index) {
            pt[index] = (pde & NPT_2MB_ADDRESS_MASK) |
                        ((ULONG64)index << PAGE_SHIFT) |
                        (pde & (NPT_PRESENT | NPT_WRITE | NPT_USER |
                                NPT_PWT | NPT_PCD | NPT_NO_EXECUTE));
        }
        split->PdptIndex = pdptIndex;
        split->PdIndex = pdIndex;
        InsertTailList(&context->SplitList, &split->Link);
        KeMemoryBarrier();
        InterlockedExchange64(
            (volatile LONG64*)&pd[pdIndex],
            (LONG64)(((ULONG64)MmGetPhysicalAddress(pt).QuadPart &
                      NPT_ADDRESS_MASK) | NPT_TABLE_PERMISSIONS));
    } else {
        pt = (ULONG64*)AmdFindSplitPt(context, pdptIndex, pdIndex);
        if (pt == NULL) {
            status = STATUS_DATA_ERROR;
            goto Exit;
        }
    }

    pte = pt[ptIndex];
    *PreviousAccess = AmdEntryAccess(pte);
    InterlockedExchange64(
        (volatile LONG64*)&pt[ptIndex],
        (LONG64)AmdApplyAccess(pte, Access));
    KeMemoryBarrier();
    invalidate = InterlockedCompareExchange(&State->Lifecycle, 0, 0) ==
        HV_LIFECYCLE_RUNNING;
    status = STATUS_SUCCESS;
Exit:
    ExReleasePushLockExclusive(&context->SlatLock);
    KeLeaveCriticalRegion();
    if (NT_SUCCESS(status) && invalidate) {
        AmdInvalidateRunningSlat(State, context);
    }
    return status;
}
