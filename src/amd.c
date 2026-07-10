#include "amd.h"

#include <intrin.h>

#define AMD_MSR_EFER                    0xC0000080u
#define AMD_MSR_STAR                    0xC0000081u
#define AMD_MSR_LSTAR                   0xC0000082u
#define AMD_MSR_CSTAR                   0xC0000083u
#define AMD_MSR_SFMASK                  0xC0000084u
#define AMD_MSR_FS_BASE                 0xC0000100u
#define AMD_MSR_GS_BASE                 0xC0000101u
#define AMD_MSR_KERNEL_GS_BASE          0xC0000102u
#define AMD_MSR_SYSENTER_CS             0x00000174u
#define AMD_MSR_SYSENTER_ESP            0x00000175u
#define AMD_MSR_SYSENTER_EIP            0x00000176u
#define AMD_MSR_DEBUGCTL                0x000001D9u
#define AMD_MSR_PAT                     0x00000277u
#define AMD_MSR_VM_CR                   0xC0010114u
#define AMD_MSR_VM_HSAVE_PA             0xC0010117u

#define AMD_EFER_SVME                   (1ull << 12)
#define AMD_VM_CR_SVMDIS                (1ull << 4)

#define AMD_SVM_FEATURE_NPT             (1u << 0)
#define AMD_SVM_FEATURE_NRIPS           (1u << 3)

#define AMD_INTERCEPT_CPUID             (1u << 18)
#define AMD_INTERCEPT_IOIO              (1u << 27)
#define AMD_INTERCEPT_MSR               (1u << 28)
#define AMD_INTERCEPT_SVM_INSTRUCTIONS  0x7fu
#define AMD_EXIT_CPUID                  0x72ull
#define AMD_EXIT_VMRUN                  0x80ull
#define AMD_EXIT_VMMCALL                0x81ull
#define AMD_EXIT_SKINIT                 0x86ull
#define AMD_EXIT_MSR                    0x7Cull
#define AMD_EXIT_NPF                    0x400ull
#define AMD_EVENT_INJECT_UD             0x80000306ull
#define AMD_EVENT_INJECT_GP             0x80000B0Dull
#define AMD_EVENT_INJECT_PF             0x80000B0Eull
#define AMD_BUGCHECK_UNEXPECTED_EXIT    0x41564D43u
#define AMD_BUGCHECK_INVALIDATION       0x414E5054u

#define AMD_IOPM_SIZE                   (3u * PAGE_SIZE)
#define AMD_MSRPM_SIZE                  (2u * PAGE_SIZE)
#define AMD_VMCB_CLEAN_ASID             (1u << 2)

#define NPT_PRESENT                     (1ull << 0)
#define NPT_WRITE                       (1ull << 1)
#define NPT_USER                        (1ull << 2)
#define NPT_PWT                         (1ull << 3)
#define NPT_PCD                         (1ull << 4)
#define NPT_LARGE_PAGE                  (1ull << 7)
#define NPT_NO_EXECUTE                  (1ull << 63)
#define NPT_ADDRESS_MASK                0x000FFFFFFFFFF000ull
#define NPT_2MB_ADDRESS_MASK            0x000FFFFFFFE00000ull
#define NPT_TABLE_PERMISSIONS           (NPT_PRESENT | NPT_WRITE | NPT_USER)

typedef struct _AMD_SLAT_SPLIT {
    LIST_ENTRY Link;
    ULONG PdptIndex;
    ULONG PdIndex;
    PVOID Pt;
} AMD_SLAT_SPLIT;

typedef struct _AMD_BACKEND_CONTEXT {
    PVOID Pml4;
    PVOID Pdpt;
    PVOID Iopm;
    PHYSICAL_ADDRESS IopmPhysical;
    PVOID Pds[512];
    LIST_ENTRY SplitList;
    EX_PUSH_LOCK SlatLock;
    ULONG64 MapLimit;
    ULONG MaxAsid;
    volatile LONG64 SlatGeneration;
} AMD_BACKEND_CONTEXT;

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

#pragma pack(push, 1)
typedef struct _AMD_DESCRIPTOR_TABLE_REGISTER {
    USHORT Limit;
    ULONG64 Base;
} AMD_DESCRIPTOR_TABLE_REGISTER;
#pragma pack(pop)

static VOID
AmdSetMsrpmBit(
    _Inout_updates_bytes_(AMD_MSRPM_SIZE) UCHAR* Msrpm,
    _In_ ULONG Msr,
    _In_ BOOLEAN Read,
    _In_ BOOLEAN Write
    )
{
    ULONG offset;
    ULONG index;
    ULONG bit;

    if (Msr <= 0x1fffu) {
        offset = 0;
        index = Msr;
    } else if (Msr >= 0xC0000000u && Msr <= 0xC0001fffu) {
        offset = 0x800;
        index = Msr - 0xC0000000u;
    } else if (Msr >= 0xC0010000u && Msr <= 0xC0011fffu) {
        offset = 0x1000;
        index = Msr - 0xC0010000u;
    } else {
        return;
    }

    bit = index * 2;
    if (Read) Msrpm[offset + bit / 8] |= (UCHAR)(1u << (bit & 7));
    ++bit;
    if (Write) Msrpm[offset + bit / 8] |= (UCHAR)(1u << (bit & 7));
}

static VOID
AmdInitializeMsrpm(
    _Inout_updates_bytes_(AMD_MSRPM_SIZE) UCHAR* Msrpm
    )
{
    RtlZeroMemory(Msrpm, AMD_MSRPM_SIZE);
    AmdSetMsrpmBit(Msrpm, AMD_MSR_EFER, TRUE, TRUE);
    AmdSetMsrpmBit(Msrpm, AMD_MSR_VM_CR, TRUE, TRUE);
    AmdSetMsrpmBit(Msrpm, AMD_MSR_VM_HSAVE_PA, TRUE, TRUE);
}

static VOID
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
        Context->Vmcb->Control.TlbControl = 3;
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

static PVOID
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

static PVOID
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

static NTSTATUS
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
    ranges = MmGetPhysicalMemoryRanges();
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
            ULONG64 cacheFlags;
            if (physical >= Context->MapLimit) break;
            cacheFlags = AmdRangeIsRam(ranges, physical, 1ull << 21)
                ? 0 : (NPT_PWT | NPT_PCD);
            pd[pdIndex] = (physical & NPT_2MB_ADDRESS_MASK) |
                          NPT_TABLE_PERMISSIONS | NPT_LARGE_PAGE |
                          cacheFlags;
        }
    }

    if (ranges != NULL) ExFreePool(ranges);
    return STATUS_SUCCESS;
}

static VOID
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

static NTSTATUS
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

static NTSTATUS
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

static NTSTATUS
AmdSupport(
    VOID
    )
{
    int registers[4];
    __cpuid(registers, 0x80000000);
    if ((ULONG)registers[0] < 0x8000000Au) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }
    __cpuid(registers, 0x80000001);
    if ((((ULONG)registers[2]) & (1u << 2)) == 0 ||
        (__readmsr(AMD_MSR_VM_CR) & AMD_VM_CR_SVMDIS) != 0) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }
    __cpuid(registers, 0x8000000A);
    if ((ULONG)registers[0] == 0 || (ULONG)registers[1] == 0 ||
        (((ULONG)registers[3]) &
         (AMD_SVM_FEATURE_NPT | AMD_SVM_FEATURE_NRIPS)) !=
        (AMD_SVM_FEATURE_NPT | AMD_SVM_FEATURE_NRIPS)) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
AmdPrepare(
    _Inout_ HV_STATE* State
    )
{
    AMD_BACKEND_CONTEXT* context;
    int registers[4];
    NTSTATUS status;

    context = (AMD_BACKEND_CONTEXT*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*context), HV_POOL_TAG_BACKEND);
    if (context == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(context, sizeof(*context));
    InitializeListHead(&context->SplitList);
    __cpuid(registers, 0x8000000A);
    context->MaxAsid = (ULONG)registers[1];
    context->SlatGeneration = 1;
    State->BackendContext = context;
    status = AmdBuildNpt(context);
    if (NT_SUCCESS(status)) {
        context->Iopm = AmdAllocateContiguous(AMD_IOPM_SIZE);
        if (context->Iopm == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            context->IopmPhysical = MmGetPhysicalAddress(context->Iopm);
        }
    }
    if (!NT_SUCCESS(status)) {
        if (context->Iopm != NULL) {
            MmFreeContiguousMemory(context->Iopm);
        }
        AmdFreeNpt(context);
        ExFreePoolWithTag(context, HV_POOL_TAG_BACKEND);
        State->BackendContext = NULL;
    }
    return status;
}

static VOID
AmdFree(
    _Inout_ HV_STATE* State
    )
{
    AMD_BACKEND_CONTEXT* context;
    if (State == NULL || State->BackendContext == NULL) return;
    context = (AMD_BACKEND_CONTEXT*)State->BackendContext;
    if (context->Iopm != NULL) {
        MmFreeContiguousMemory(context->Iopm);
        context->Iopm = NULL;
    }
    AmdFreeNpt(context);
    ExFreePoolWithTag(context, HV_POOL_TAG_BACKEND);
    State->BackendContext = NULL;
}

static NTSTATUS
AmdPrepareCpu(
    _Inout_ HV_STATE* State,
    _Inout_ HV_CPU* Cpu
    )
{
    AMD_BACKEND_CONTEXT* backend;
    AMD_CPU_CONTEXT* context;

    if (State == NULL || State->BackendContext == NULL || Cpu == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    backend = (AMD_BACKEND_CONTEXT*)State->BackendContext;

    context = (AMD_CPU_CONTEXT*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*context), HV_POOL_TAG_BACKEND);
    if (context == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(context, sizeof(*context));
    context->Vmcb = (AMD_VMCB*)AmdAllocatePage();
    context->HostVmcb = (AMD_VMCB*)AmdAllocatePage();
    context->HostSave = AmdAllocatePage();
    context->HostStack = ExAllocatePool2(
        POOL_FLAG_NON_PAGED, AMD_HOST_STACK_SIZE, HV_POOL_TAG_BACKEND);
    context->Msrpm = AmdAllocateContiguous(AMD_MSRPM_SIZE);
    if (context->Vmcb == NULL || context->HostVmcb == NULL ||
        context->HostSave == NULL || context->HostStack == NULL ||
        context->Msrpm == NULL) {
        if (context->Vmcb != NULL) MmFreeContiguousMemory(context->Vmcb);
        if (context->HostVmcb != NULL) {
            MmFreeContiguousMemory(context->HostVmcb);
        }
        if (context->HostSave != NULL) {
            MmFreeContiguousMemory(context->HostSave);
        }
        if (context->HostStack != NULL) {
            ExFreePoolWithTag(context->HostStack, HV_POOL_TAG_BACKEND);
        }
        if (context->Msrpm != NULL) {
            MmFreeContiguousMemory(context->Msrpm);
        }
        ExFreePoolWithTag(context, HV_POOL_TAG_BACKEND);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    context->VmcbPhysical = MmGetPhysicalAddress(context->Vmcb);
    context->HostVmcbPhysical = MmGetPhysicalAddress(context->HostVmcb);
    context->HostSavePhysical = MmGetPhysicalAddress(context->HostSave);
    context->MsrpmPhysical = MmGetPhysicalAddress(context->Msrpm);
    context->BackendContext = backend;
    context->SlatGeneration = backend->SlatGeneration;
    context->StopCookie = __rdtsc() ^ (ULONG64)context ^
                          (ULONG64)context->VmcbPhysical.QuadPart;
    AmdInitializeMsrpm((UCHAR*)context->Msrpm);
    Cpu->VendorContext = context;
    return STATUS_SUCCESS;
}

static VOID
AmdFreeCpu(
    _Inout_ HV_STATE* State,
    _Inout_ HV_CPU* Cpu
    )
{
    AMD_CPU_CONTEXT* context;
    UNREFERENCED_PARAMETER(State);
    if (Cpu == NULL || Cpu->VendorContext == NULL) return;
    context = (AMD_CPU_CONTEXT*)Cpu->VendorContext;
    if (context->HostStack != NULL) {
        RtlSecureZeroMemory(context->HostStack, AMD_HOST_STACK_SIZE);
        ExFreePoolWithTag(context->HostStack, HV_POOL_TAG_BACKEND);
    }
    if (context->Msrpm != NULL) {
        MmFreeContiguousMemory(context->Msrpm);
    }
    if (context->HostSave != NULL) {
        MmFreeContiguousMemory(context->HostSave);
    }
    if (context->HostVmcb != NULL) {
        MmFreeContiguousMemory(context->HostVmcb);
    }
    if (context->Vmcb != NULL) MmFreeContiguousMemory(context->Vmcb);
    RtlSecureZeroMemory(context, sizeof(*context));
    ExFreePoolWithTag(context, HV_POOL_TAG_BACKEND);
    Cpu->VendorContext = NULL;
}

static NTSTATUS
AmdCaptureSegment(
    _In_ USHORT Selector,
    _In_ const AMD_DESCRIPTOR_TABLE_REGISTER* Gdtr,
    _In_ ULONG MsrBase,
    _Out_ AMD_VMCB_SEGMENT* Segment
    )
{
    ULONG offset = Selector & ~7u;
    ULONG64 descriptor;
    ULONG64 base;
    ULONG64 limit;

    RtlZeroMemory(Segment, sizeof(*Segment));
    Segment->Selector = Selector;
    if (Selector == 0) return STATUS_SUCCESS;
    if ((Selector & 4) != 0 || offset > Gdtr->Limit ||
        Gdtr->Limit - offset < sizeof(ULONG64) - 1) {
        return STATUS_DATA_ERROR;
    }
    descriptor = *(UNALIGNED const ULONG64*)(Gdtr->Base + offset);
    base = ((descriptor >> 16) & 0xffffull) |
           ((descriptor >> 32) & 0xff0000ull) |
           ((descriptor >> 56) & 0xff000000ull);
    if (((descriptor >> 44) & 1) == 0) {
        if (Gdtr->Limit - offset < 11) return STATUS_DATA_ERROR;
        base |= (ULONG64)*(UNALIGNED const ULONG*)(Gdtr->Base + offset + 8)
                << 32;
    }
    limit = (descriptor & 0xffff) | ((descriptor >> 32) & 0xf0000);
    if ((descriptor & (1ull << 55)) != 0) {
        limit = (limit << PAGE_SHIFT) | (PAGE_SIZE - 1);
    }
    Segment->Limit = (ULONG)limit;
    Segment->Attributes =
        (USHORT)(((descriptor >> 40) & 0xff) |
                 (((descriptor >> 52) & 0xf) << 8));
    Segment->Base = MsrBase != 0 ? __readmsr(MsrBase) : base;
    return STATUS_SUCCESS;
}

static NTSTATUS
AmdSetupVmcb(
    _Inout_ HV_STATE* State,
    _Inout_ HV_CPU* Cpu
    )
{
    AMD_BACKEND_CONTEXT* backend =
        (AMD_BACKEND_CONTEXT*)State->BackendContext;
    AMD_CPU_CONTEXT* context = (AMD_CPU_CONTEXT*)Cpu->VendorContext;
    AMD_VMCB* vmcb = context->Vmcb;
    AMD_DESCRIPTOR_TABLE_REGISTER gdtr;
    AMD_DESCRIPTOR_TABLE_REGISTER idtr;
    NTSTATUS status;

    RtlZeroMemory(vmcb, PAGE_SIZE);
    AmdAsmStoreGdtr(&gdtr);
    AmdAsmStoreIdtr(&idtr);
    status = AmdCaptureSegment(AmdAsmReadEs(), &gdtr, 0, &vmcb->State.Es);
    if (!NT_SUCCESS(status)) return status;
    status = AmdCaptureSegment(AmdAsmReadCs(), &gdtr, 0, &vmcb->State.Cs);
    if (!NT_SUCCESS(status)) return status;
    status = AmdCaptureSegment(AmdAsmReadSs(), &gdtr, 0, &vmcb->State.Ss);
    if (!NT_SUCCESS(status)) return status;
    status = AmdCaptureSegment(AmdAsmReadDs(), &gdtr, 0, &vmcb->State.Ds);
    if (!NT_SUCCESS(status)) return status;
    status = AmdCaptureSegment(
        AmdAsmReadFs(), &gdtr, AMD_MSR_FS_BASE, &vmcb->State.Fs);
    if (!NT_SUCCESS(status)) return status;
    status = AmdCaptureSegment(
        AmdAsmReadGs(), &gdtr, AMD_MSR_GS_BASE, &vmcb->State.Gs);
    if (!NT_SUCCESS(status)) return status;
    status = AmdCaptureSegment(AmdAsmReadLdtr(), &gdtr, 0, &vmcb->State.Ldtr);
    if (!NT_SUCCESS(status)) return status;
    status = AmdCaptureSegment(AmdAsmReadTr(), &gdtr, 0, &vmcb->State.Tr);
    if (!NT_SUCCESS(status)) return status;

    vmcb->State.Gdtr.Limit = gdtr.Limit;
    vmcb->State.Gdtr.Base = gdtr.Base;
    vmcb->State.Idtr.Limit = idtr.Limit;
    vmcb->State.Idtr.Base = idtr.Base;
    vmcb->State.Cpl = (UCHAR)(vmcb->State.Cs.Selector & 3);
    vmcb->State.Efer = context->GuestEfer | AMD_EFER_SVME;
    vmcb->State.Cr4 = __readcr4();
    vmcb->State.Cr3 = __readcr3();
    vmcb->State.Cr0 = __readcr0();
    vmcb->State.Dr7 = __readdr(7);
    vmcb->State.Dr6 = __readdr(6);
    vmcb->State.Rflags = __readeflags();
    vmcb->State.Cr2 = __readcr2();
    vmcb->State.GPat = __readmsr(AMD_MSR_PAT);
    vmcb->State.DebugCtl = __readmsr(AMD_MSR_DEBUGCTL);
    vmcb->State.Star = __readmsr(AMD_MSR_STAR);
    vmcb->State.Lstar = __readmsr(AMD_MSR_LSTAR);
    vmcb->State.Cstar = __readmsr(AMD_MSR_CSTAR);
    vmcb->State.Sfmask = __readmsr(AMD_MSR_SFMASK);
    vmcb->State.KernelGsBase = __readmsr(AMD_MSR_KERNEL_GS_BASE);
    vmcb->State.SysenterCs = __readmsr(AMD_MSR_SYSENTER_CS);
    vmcb->State.SysenterEsp = __readmsr(AMD_MSR_SYSENTER_ESP);
    vmcb->State.SysenterEip = __readmsr(AMD_MSR_SYSENTER_EIP);
    vmcb->State.Rax = 0;

    vmcb->Control.InterceptMisc1 =
        AMD_INTERCEPT_CPUID | AMD_INTERCEPT_IOIO | AMD_INTERCEPT_MSR;
    vmcb->Control.InterceptMisc2 = AMD_INTERCEPT_SVM_INSTRUCTIONS;
    context->GuestAsid = Cpu->ProcessorIndex + 1;
    if (context->GuestAsid == 0 ||
        context->GuestAsid >= backend->MaxAsid) {
        context->GuestAsid = 1;
    }
    vmcb->Control.GuestAsid = context->GuestAsid;
    vmcb->Control.TlbControl = 1;
    vmcb->Control.IopmBase = backend->IopmPhysical.QuadPart;
    vmcb->Control.MsrpmBase = context->MsrpmPhysical.QuadPart;
    vmcb->Control.VirtualInterrupt = 1ull << 24;
    vmcb->Control.NestedPagingEnable = 1;
    vmcb->Control.NestedCr3 =
        (ULONG64)MmGetPhysicalAddress(backend->Pml4).QuadPart &
        NPT_ADDRESS_MASK;
    vmcb->Control.VmcbClean = 0;
    return STATUS_SUCCESS;
}

static BOOLEAN
AmdCurrentCpuMatches(
    _In_ HV_CPU* Cpu
    )
{
    PROCESSOR_NUMBER number;
    KeGetCurrentProcessorNumberEx(&number);
    return KeGetProcessorIndexFromNumber(&number) == Cpu->ProcessorIndex;
}

static NTSTATUS
AmdStart(
    _Inout_ HV_STATE* State,
    _Inout_ HV_CPU* Cpu
    )
{
    AMD_CPU_CONTEXT* context;
    ULONG64 hostRsp;
    NTSTATUS status;

    if (!AmdCurrentCpuMatches(Cpu) || Cpu->VendorContext == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    context = (AMD_CPU_CONTEXT*)Cpu->VendorContext;
    context->OriginalEfer = __readmsr(AMD_MSR_EFER);
    context->OriginalHostSavePhysical = __readmsr(AMD_MSR_VM_HSAVE_PA);
    context->GuestEfer = context->OriginalEfer;
    context->GuestVmCr = __readmsr(AMD_MSR_VM_CR);
    context->GuestHostSavePhysical = context->OriginalHostSavePhysical;
    __writemsr(AMD_MSR_VM_HSAVE_PA,
        (ULONG64)context->HostSavePhysical.QuadPart);
    __writemsr(AMD_MSR_EFER, context->OriginalEfer | AMD_EFER_SVME);

    status = AmdSetupVmcb(State, Cpu);
    if (!NT_SUCCESS(status)) {
        __writemsr(AMD_MSR_VM_HSAVE_PA, context->OriginalHostSavePhysical);
        __writemsr(AMD_MSR_EFER, context->OriginalEfer);
        return status;
    }
    hostRsp = ((ULONG64)context->HostStack + AMD_HOST_STACK_SIZE - 64) &
              ~0xfull;
    context->Virtualized = TRUE;
    if (AmdAsmLaunch(
            context->Vmcb,
            (ULONG64)context->VmcbPhysical.QuadPart,
            hostRsp,
            Cpu,
            (ULONG64)context->HostVmcbPhysical.QuadPart) == 0) {
        return STATUS_SUCCESS;
    }

    context->Virtualized = FALSE;
    __writemsr(AMD_MSR_VM_HSAVE_PA, context->OriginalHostSavePhysical);
    __writemsr(AMD_MSR_EFER, context->OriginalEfer);
    return STATUS_HV_INVALID_VP_STATE;
}

static NTSTATUS
AmdStop(
    _Inout_ HV_STATE* State,
    _Inout_ HV_CPU* Cpu
    )
{
    AMD_CPU_CONTEXT* context;
    UNREFERENCED_PARAMETER(State);
    if (!AmdCurrentCpuMatches(Cpu) || Cpu->VendorContext == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    context = (AMD_CPU_CONTEXT*)Cpu->VendorContext;
    if (!context->Virtualized) return STATUS_SUCCESS;
    AmdAsmStop(context->StopCookie);
    return context->Virtualized ? STATUS_HV_OPERATION_FAILED : STATUS_SUCCESS;
}

static VOID
AmdInjectException(
    _Inout_ AMD_VMCB* Vmcb,
    _In_ ULONG64 Information,
    _In_ ULONG ErrorCode
    )
{
    Vmcb->Control.EventInjection = Information |
        ((ULONG64)ErrorCode << 32);
    Vmcb->Control.VmcbClean = 0;
}

static VOID
AmdInjectInvalidOpcode(
    _Inout_ AMD_VMCB* Vmcb
    )
{
    AmdInjectException(Vmcb, AMD_EVENT_INJECT_UD, 0);
}

ULONG
AmdVmExitHandler(
    _Inout_ AMD_GUEST_REGISTERS* Registers,
    _Inout_ HV_CPU* Cpu
    )
{
    AMD_CPU_CONTEXT* context;
    AMD_VMCB* vmcb;
    ULONG64 exitCode;
    int cpuid[4];

    if (Registers == NULL || Cpu == NULL || Cpu->VendorContext == NULL) {
        KeBugCheckEx(HYPERVISOR_ERROR, AMD_BUGCHECK_UNEXPECTED_EXIT,
            MAXULONG, 0, 0);
    }
    context = (AMD_CPU_CONTEXT*)Cpu->VendorContext;
    vmcb = context->Vmcb;
    exitCode = vmcb->Control.ExitCode;
    AmdPrepareTlbControl(context);
    vmcb->Control.EventInjection =
        (vmcb->Control.ExitInterruptInfo & (1ull << 31)) != 0
        ? vmcb->Control.ExitInterruptInfo : 0;

    if (exitCode == AMD_EXIT_CPUID) {
        __cpuidex(cpuid, (int)(ULONG)vmcb->State.Rax,
            (int)(ULONG)Registers->Rcx);
        vmcb->State.Rax = (ULONG)cpuid[0];
        Registers->Rbx = (ULONG)cpuid[1];
        Registers->Rcx = (ULONG)cpuid[2];
        Registers->Rdx = (ULONG)cpuid[3];
        vmcb->State.Rip = vmcb->Control.NextRip;
        vmcb->Control.VmcbClean = 0;
        return AMD_VMEXIT_RESUME;
    }

    if (exitCode == AMD_EXIT_MSR) {
        ULONG msr = (ULONG)Registers->Rcx;
        BOOLEAN write = (vmcb->Control.ExitInfo1 & 1) != 0;
        ULONG64 msrValue;

        if (!write) {
            if (msr == AMD_MSR_EFER) {
                msrValue = context->GuestEfer;
            } else if (msr == AMD_MSR_VM_CR) {
                msrValue = context->GuestVmCr;
            } else if (msr == AMD_MSR_VM_HSAVE_PA) {
                msrValue = context->GuestHostSavePhysical;
            } else {
                AmdInjectException(vmcb, AMD_EVENT_INJECT_GP, 0);
                return AMD_VMEXIT_RESUME;
            }
            vmcb->State.Rax = (ULONG)msrValue;
            Registers->Rdx = (ULONG)(msrValue >> 32);
        } else {
            msrValue = ((ULONG64)(ULONG)Registers->Rdx << 32) |
                       (ULONG)vmcb->State.Rax;
            if (msr == AMD_MSR_EFER) {
                const ULONG64 validEfer = 0x000000000036FD01ull;
                if ((msrValue & ~validEfer) != 0) {
                    AmdInjectException(vmcb, AMD_EVENT_INJECT_GP, 0);
                    return AMD_VMEXIT_RESUME;
                }
                context->GuestEfer =
                    (msrValue & ~(1ull << 10)) |
                    (context->GuestEfer & (1ull << 10));
                vmcb->State.Efer = context->GuestEfer | AMD_EFER_SVME;
            } else if (msr == AMD_MSR_VM_CR) {
                context->GuestVmCr = msrValue;
            } else if (msr == AMD_MSR_VM_HSAVE_PA) {
                context->GuestHostSavePhysical = msrValue;
            } else {
                AmdInjectException(vmcb, AMD_EVENT_INJECT_GP, 0);
                return AMD_VMEXIT_RESUME;
            }
        }
        vmcb->State.Rip = vmcb->Control.NextRip;
        vmcb->Control.VmcbClean = 0;
        return AMD_VMEXIT_RESUME;
    }

    if (exitCode == AMD_EXIT_NPF) {
        AmdInjectException(vmcb, AMD_EVENT_INJECT_GP, 0);
        return AMD_VMEXIT_RESUME;
    }

    if (exitCode == AMD_EXIT_VMMCALL) {
        if (vmcb->State.Cpl == 0 &&
            vmcb->State.Rax == HV_HYPERCALL_MAGIC_RAX &&
            Registers->Rcx == HV_HYPERCALL_MAGIC_RCX &&
            Registers->Rdx == HV_HYPERCALL_MAGIC_RDX &&
            Registers->R8 == HV_HYPERCALL_MAGIC_R8 &&
            Registers->R9 == context->StopCookie) {
            context->ResumeRsp = vmcb->State.Rsp;
            context->ResumeRip = vmcb->Control.NextRip;
            context->Virtualized = FALSE;
            __writemsr(
                AMD_MSR_VM_HSAVE_PA, context->OriginalHostSavePhysical);
            __writemsr(AMD_MSR_EFER, context->OriginalEfer);
            return AMD_VMEXIT_STOP;
        }
        AmdInjectInvalidOpcode(vmcb);
        return AMD_VMEXIT_RESUME;
    }

    if (exitCode >= AMD_EXIT_VMRUN && exitCode <= AMD_EXIT_SKINIT) {
        AmdInjectInvalidOpcode(vmcb);
        return AMD_VMEXIT_RESUME;
    }

    KeBugCheckEx(HYPERVISOR_ERROR, AMD_BUGCHECK_UNEXPECTED_EXIT,
        Cpu->ProcessorIndex, (ULONG_PTR)exitCode, vmcb->State.Rip);
}

static const HV_BACKEND_OPS AmdBackendOps = {
    "AMD SVM/NPT",
    AmdSupport,
    AmdPrepare,
    AmdFree,
    AmdPrepareCpu,
    AmdFreeCpu,
    AmdStart,
    AmdStop,
    AmdQueryOwnedPageAccess,
    AmdSetOwnedPageAccess
};

const HV_BACKEND_OPS*
HvAmdGetBackendOps(
    VOID
    )
{
    return &AmdBackendOps;
}
