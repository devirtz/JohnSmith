#include "intel.h"

#include <intrin.h>

#define IA32_FEATURE_CONTROL                0x0000003Au
#define IA32_SYSENTER_CS                    0x00000174u
#define IA32_SYSENTER_ESP                   0x00000175u
#define IA32_SYSENTER_EIP                   0x00000176u
#define IA32_DEBUGCTL                       0x000001D9u
#define IA32_PAT                            0x00000277u
#define IA32_XSS                            0x00000DA0u
#define IA32_EFER                           0xC0000080u
#define IA32_FS_BASE                        0xC0000100u
#define IA32_GS_BASE                        0xC0000101u

#define IA32_VMX_BASIC                      0x00000480u
#define IA32_VMX_PINBASED_CTLS              0x00000481u
#define IA32_VMX_PROCBASED_CTLS             0x00000482u
#define IA32_VMX_EXIT_CTLS                  0x00000483u
#define IA32_VMX_ENTRY_CTLS                 0x00000484u
#define IA32_VMX_CR0_FIXED0                 0x00000486u
#define IA32_VMX_CR0_FIXED1                 0x00000487u
#define IA32_VMX_CR4_FIXED0                 0x00000488u
#define IA32_VMX_CR4_FIXED1                 0x00000489u
#define IA32_VMX_PROCBASED_CTLS2            0x0000048Bu
#define IA32_VMX_EPT_VPID_CAP               0x0000048Cu
#define IA32_VMX_TRUE_PINBASED_CTLS         0x0000048Du
#define IA32_VMX_TRUE_PROCBASED_CTLS        0x0000048Eu
#define IA32_VMX_TRUE_EXIT_CTLS             0x0000048Fu
#define IA32_VMX_TRUE_ENTRY_CTLS            0x00000490u

#define VMCS_VPID                           0x00000000u
#define VMCS_GUEST_ES_SELECTOR              0x00000800u
#define VMCS_GUEST_CS_SELECTOR              0x00000802u
#define VMCS_GUEST_SS_SELECTOR              0x00000804u
#define VMCS_GUEST_DS_SELECTOR              0x00000806u
#define VMCS_GUEST_FS_SELECTOR              0x00000808u
#define VMCS_GUEST_GS_SELECTOR              0x0000080Au
#define VMCS_GUEST_LDTR_SELECTOR            0x0000080Cu
#define VMCS_GUEST_TR_SELECTOR              0x0000080Eu
#define VMCS_HOST_ES_SELECTOR               0x00000C00u
#define VMCS_HOST_CS_SELECTOR               0x00000C02u
#define VMCS_HOST_SS_SELECTOR               0x00000C04u
#define VMCS_HOST_DS_SELECTOR               0x00000C06u
#define VMCS_HOST_FS_SELECTOR               0x00000C08u
#define VMCS_HOST_GS_SELECTOR               0x00000C0Au
#define VMCS_HOST_TR_SELECTOR               0x00000C0Cu

#define VMCS_IO_BITMAP_A                    0x00002000u
#define VMCS_IO_BITMAP_B                    0x00002002u
#define VMCS_MSR_BITMAP                     0x00002004u
#define VMCS_EPT_POINTER                    0x0000201Au
#define VMCS_XSS_EXITING_BITMAP             0x0000202Cu
#define VMCS_GUEST_PHYSICAL_ADDRESS         0x00002400u
#define VMCS_GUEST_VMCS_LINK_POINTER        0x00002800u
#define VMCS_GUEST_DEBUGCTL                 0x00002802u
#define VMCS_GUEST_PAT                      0x00002804u
#define VMCS_GUEST_EFER                     0x00002806u
#define VMCS_GUEST_XSS                      0x00002812u
#define VMCS_HOST_PAT                       0x00002C00u
#define VMCS_HOST_EFER                      0x00002C02u
#define VMCS_HOST_XSS                       0x00002C06u

#define VMCS_PIN_BASED_CONTROLS             0x00004000u
#define VMCS_PRIMARY_PROCESSOR_CONTROLS     0x00004002u
#define VMCS_EXCEPTION_BITMAP               0x00004004u
#define VMCS_PAGE_FAULT_ERROR_MASK          0x00004006u
#define VMCS_PAGE_FAULT_ERROR_MATCH         0x00004008u
#define VMCS_CR3_TARGET_COUNT               0x0000400Au
#define VMCS_EXIT_CONTROLS                  0x0000400Cu
#define VMCS_EXIT_MSR_STORE_COUNT           0x0000400Eu
#define VMCS_EXIT_MSR_LOAD_COUNT            0x00004010u
#define VMCS_ENTRY_CONTROLS                 0x00004012u
#define VMCS_ENTRY_MSR_LOAD_COUNT           0x00004014u
#define VMCS_ENTRY_INTERRUPTION_INFO        0x00004016u
#define VMCS_ENTRY_EXCEPTION_ERROR          0x00004018u
#define VMCS_ENTRY_INSTRUCTION_LENGTH       0x0000401Au
#define VMCS_TPR_THRESHOLD                  0x0000401Cu
#define VMCS_SECONDARY_PROCESSOR_CONTROLS   0x0000401Eu
#define VMCS_VM_INSTRUCTION_ERROR           0x00004400u
#define VMCS_EXIT_REASON                    0x00004402u
#define VMCS_EXIT_INTERRUPTION_INFO         0x00004404u
#define VMCS_EXIT_INTERRUPTION_ERROR        0x00004406u
#define VMCS_IDT_VECTORING_INFO             0x00004408u
#define VMCS_IDT_VECTORING_ERROR            0x0000440Au
#define VMCS_EXIT_INSTRUCTION_LENGTH        0x0000440Cu

#define VMCS_GUEST_ES_LIMIT                 0x00004800u
#define VMCS_GUEST_CS_LIMIT                 0x00004802u
#define VMCS_GUEST_SS_LIMIT                 0x00004804u
#define VMCS_GUEST_DS_LIMIT                 0x00004806u
#define VMCS_GUEST_FS_LIMIT                 0x00004808u
#define VMCS_GUEST_GS_LIMIT                 0x0000480Au
#define VMCS_GUEST_LDTR_LIMIT               0x0000480Cu
#define VMCS_GUEST_TR_LIMIT                 0x0000480Eu
#define VMCS_GUEST_GDTR_LIMIT               0x00004810u
#define VMCS_GUEST_IDTR_LIMIT               0x00004812u
#define VMCS_GUEST_ES_AR                    0x00004814u
#define VMCS_GUEST_CS_AR                    0x00004816u
#define VMCS_GUEST_SS_AR                    0x00004818u
#define VMCS_GUEST_DS_AR                    0x0000481Au
#define VMCS_GUEST_FS_AR                    0x0000481Cu
#define VMCS_GUEST_GS_AR                    0x0000481Eu
#define VMCS_GUEST_LDTR_AR                  0x00004820u
#define VMCS_GUEST_TR_AR                    0x00004822u
#define VMCS_GUEST_INTERRUPTIBILITY         0x00004824u
#define VMCS_GUEST_ACTIVITY_STATE           0x00004826u
#define VMCS_GUEST_SMBASE                   0x00004828u
#define VMCS_GUEST_SYSENTER_CS              0x0000482Au
#define VMCS_HOST_SYSENTER_CS               0x00004C00u

#define VMCS_CR0_GUEST_HOST_MASK            0x00006000u
#define VMCS_CR4_GUEST_HOST_MASK            0x00006002u
#define VMCS_CR0_READ_SHADOW                0x00006004u
#define VMCS_CR4_READ_SHADOW                0x00006006u
#define VMCS_EXIT_QUALIFICATION             0x00006400u
#define VMCS_GUEST_LINEAR_ADDRESS           0x0000640Au

#define VMCS_GUEST_CR0                      0x00006800u
#define VMCS_GUEST_CR3                      0x00006802u
#define VMCS_GUEST_CR4                      0x00006804u
#define VMCS_GUEST_ES_BASE                  0x00006806u
#define VMCS_GUEST_CS_BASE                  0x00006808u
#define VMCS_GUEST_SS_BASE                  0x0000680Au
#define VMCS_GUEST_DS_BASE                  0x0000680Cu
#define VMCS_GUEST_FS_BASE                  0x0000680Eu
#define VMCS_GUEST_GS_BASE                  0x00006810u
#define VMCS_GUEST_LDTR_BASE                0x00006812u
#define VMCS_GUEST_TR_BASE                  0x00006814u
#define VMCS_GUEST_GDTR_BASE                0x00006816u
#define VMCS_GUEST_IDTR_BASE                0x00006818u
#define VMCS_GUEST_DR7                      0x0000681Au
#define VMCS_GUEST_RSP                      0x0000681Cu
#define VMCS_GUEST_RIP                      0x0000681Eu
#define VMCS_GUEST_RFLAGS                   0x00006820u
#define VMCS_GUEST_PENDING_DEBUG            0x00006822u
#define VMCS_GUEST_SYSENTER_ESP             0x00006824u
#define VMCS_GUEST_SYSENTER_EIP             0x00006826u

#define VMCS_HOST_CR0                       0x00006C00u
#define VMCS_HOST_CR3                       0x00006C02u
#define VMCS_HOST_CR4                       0x00006C04u
#define VMCS_HOST_FS_BASE                   0x00006C06u
#define VMCS_HOST_GS_BASE                   0x00006C08u
#define VMCS_HOST_TR_BASE                   0x00006C0Au
#define VMCS_HOST_GDTR_BASE                 0x00006C0Cu
#define VMCS_HOST_IDTR_BASE                 0x00006C0Eu
#define VMCS_HOST_SYSENTER_ESP              0x00006C10u
#define VMCS_HOST_SYSENTER_EIP              0x00006C12u
#define VMCS_HOST_RSP                       0x00006C14u
#define VMCS_HOST_RIP                       0x00006C16u

#define VMX_PRIMARY_ACTIVATE_SECONDARY      (1u << 31)
#define VMX_PRIMARY_USE_IO_BITMAPS          (1u << 25)
#define VMX_PRIMARY_USE_MSR_BITMAPS         (1u << 28)
#define VMX_SECONDARY_ENABLE_EPT            (1u << 1)
#define VMX_SECONDARY_ENABLE_RDTSCP         (1u << 3)
#define VMX_SECONDARY_ENABLE_VPID           (1u << 5)
#define VMX_SECONDARY_ENABLE_INVPCID        (1u << 12)
#define VMX_SECONDARY_ENABLE_XSAVES         (1u << 20)
#define VMX_EXIT_HOST_ADDRESS_SPACE_SIZE    (1u << 9)
#define VMX_ENTRY_IA32E_MODE                (1u << 9)
#define VMX_CR4_VMXE                        (1ull << 13)

#define EPT_ACCESS_MASK                     0x7ull
#define EPT_MEMORY_TYPE_SHIFT               3u
#define EPT_MEMORY_TYPE_WB                  6ull
#define EPT_LARGE_PAGE                      (1ull << 7)
#define EPT_ADDRESS_MASK                    0x000FFFFFFFFFF000ull
#define EPT_2MB_ADDRESS_MASK                0x000FFFFFFFE00000ull

#define VMX_EXIT_EXCEPTION_OR_NMI           0u
#define VMX_EXIT_CPUID                      10u
#define VMX_EXIT_VMCALL                     18u
#define VMX_EXIT_CR_ACCESS                  28u
#define VMX_EXIT_RDMSR                      31u
#define VMX_EXIT_WRMSR                      32u
#define VMX_EXIT_EPT_VIOLATION              48u
#define VMX_EXIT_EPT_MISCONFIGURATION       49u
#define VMX_EXIT_XSETBV                     55u
#define VMX_ENTRY_INJECT_UD                 0x80000306u
#define VMX_ENTRY_INJECT_GP                 0x80000B0Du
#define VMX_ENTRY_INJECT_PF                 0x80000B0Eu
#define INTEL_BUGCHECK_UNEXPECTED_EXIT      0x49564D58u
#define INTEL_BUGCHECK_INVALIDATION         0x494E5645u

#define INVEPT_SINGLE_CONTEXT               1u
#define INVEPT_ALL_CONTEXTS                 2u
#define INVVPID_SINGLE_CONTEXT              1u

typedef struct _INTEL_SLAT_SPLIT {
    LIST_ENTRY Link;
    ULONG PdptIndex;
    ULONG PdIndex;
    PVOID Pt;
} INTEL_SLAT_SPLIT;

typedef struct _INTEL_BACKEND_CONTEXT {
    PVOID Pml4;
    PVOID Pdpt;
    PVOID Pds[512];
    LIST_ENTRY SplitList;
    EX_PUSH_LOCK SlatLock;
    ULONG64 MapLimit;
    ULONG64 VmxBasic;
    ULONG64 EptVpidCapabilities;
    volatile LONG64 SlatGeneration;
} INTEL_BACKEND_CONTEXT;

typedef struct DECLSPEC_ALIGN(16) _INTEL_INVALIDATION_DESCRIPTOR {
    ULONG64 Context;
    ULONG64 Reserved;
} INTEL_INVALIDATION_DESCRIPTOR;

#pragma pack(push, 1)
typedef struct _INTEL_DESCRIPTOR_TABLE_REGISTER {
    USHORT Limit;
    ULONG64 Base;
} INTEL_DESCRIPTOR_TABLE_REGISTER;
#pragma pack(pop)

typedef struct _INTEL_SEGMENT_STATE {
    USHORT Selector;
    ULONG Limit;
    ULONG AccessRights;
    ULONG64 Base;
} INTEL_SEGMENT_STATE;

static VOID
IntelSetMsrBitmapBit(
    _Inout_updates_bytes_(PAGE_SIZE) UCHAR* Bitmap,
    _In_ ULONG Msr,
    _In_ BOOLEAN Read,
    _In_ BOOLEAN Write
    )
{
    ULONG index;
    ULONG readBase;
    ULONG writeBase;

    if (Msr <= 0x1fffu) {
        index = Msr;
        readBase = 0;
        writeBase = 2048;
    } else if (Msr >= 0xC0000000u && Msr <= 0xC0001fffu) {
        index = Msr - 0xC0000000u;
        readBase = 1024;
        writeBase = 3072;
    } else {
        return;
    }

    if (Read) Bitmap[readBase + index / 8] |= (UCHAR)(1u << (index & 7));
    if (Write) Bitmap[writeBase + index / 8] |= (UCHAR)(1u << (index & 7));
}

static VOID
IntelInitializeMsrBitmap(
    _Inout_updates_bytes_(PAGE_SIZE) UCHAR* Bitmap
    )
{
    ULONG msr;

    RtlZeroMemory(Bitmap, PAGE_SIZE);
    IntelSetMsrBitmapBit(Bitmap, IA32_FEATURE_CONTROL, FALSE, TRUE);
    for (msr = IA32_VMX_BASIC; msr <= 0x491u; ++msr) {
        IntelSetMsrBitmapBit(Bitmap, msr, FALSE, TRUE);
    }
}

static VOID
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

static PVOID
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

static NTSTATUS
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

static VOID
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

static NTSTATUS
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

static NTSTATUS
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

static NTSTATUS
IntelSupport(
    VOID
    )
{
    int registers[4];
    ULONG64 featureControl;
    ULONG64 basic;
    ULONG64 eptCapabilities;

    __cpuid(registers, 1);
    if ((((ULONG)registers[2]) & (1u << 5)) == 0) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }

    featureControl = __readmsr(IA32_FEATURE_CONTROL);
    if ((featureControl & 1) != 0 &&
        (featureControl & (1ull << 2)) == 0) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }

    basic = __readmsr(IA32_VMX_BASIC);
    if (((basic >> 50) & 0xf) != EPT_MEMORY_TYPE_WB) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }

    eptCapabilities = __readmsr(IA32_VMX_EPT_VPID_CAP);
    if ((eptCapabilities & (1ull << 6)) == 0 ||
        (eptCapabilities & (1ull << 14)) == 0 ||
        (eptCapabilities & (1ull << 16)) == 0 ||
        (eptCapabilities & (1ull << 20)) == 0 ||
        (eptCapabilities & ((1ull << 25) | (1ull << 26))) == 0) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
IntelPrepare(
    _Inout_ HV_STATE* State
    )
{
    INTEL_BACKEND_CONTEXT* context;
    NTSTATUS status;

    context = (INTEL_BACKEND_CONTEXT*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*context), HV_POOL_TAG_BACKEND);
    if (context == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(context, sizeof(*context));
    InitializeListHead(&context->SplitList);
    context->VmxBasic = __readmsr(IA32_VMX_BASIC);
    context->EptVpidCapabilities = __readmsr(IA32_VMX_EPT_VPID_CAP);
    context->SlatGeneration = 1;
    State->BackendContext = context;

    status = IntelBuildEpt(context);
    if (!NT_SUCCESS(status)) {
        IntelFreeEpt(context);
        ExFreePoolWithTag(context, HV_POOL_TAG_BACKEND);
        State->BackendContext = NULL;
    }
    return status;
}

static VOID
IntelFree(
    _Inout_ HV_STATE* State
    )
{
    INTEL_BACKEND_CONTEXT* context;

    if (State == NULL || State->BackendContext == NULL) {
        return;
    }
    context = (INTEL_BACKEND_CONTEXT*)State->BackendContext;
    IntelFreeEpt(context);
    ExFreePoolWithTag(context, HV_POOL_TAG_BACKEND);
    State->BackendContext = NULL;
}

static NTSTATUS
IntelPrepareCpu(
    _Inout_ HV_STATE* State,
    _Inout_ HV_CPU* Cpu
    )
{
    INTEL_BACKEND_CONTEXT* backend;
    INTEL_CPU_CONTEXT* context;

    if (State == NULL || Cpu == NULL || State->BackendContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    backend = (INTEL_BACKEND_CONTEXT*)State->BackendContext;
    context = (INTEL_CPU_CONTEXT*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*context), HV_POOL_TAG_BACKEND);
    if (context == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(context, sizeof(*context));

    context->Vmxon = IntelAllocatePage(MAXULONG);
    context->Vmcs = IntelAllocatePage(MAXULONG);
    context->HostStack = ExAllocatePool2(
        POOL_FLAG_NON_PAGED, INTEL_HOST_STACK_SIZE, HV_POOL_TAG_BACKEND);
    context->MsrBitmap = IntelAllocatePage(MAXULONG);
    context->IoBitmapA = IntelAllocatePage(MAXULONG);
    context->IoBitmapB = IntelAllocatePage(MAXULONG);
    if (context->Vmxon == NULL || context->Vmcs == NULL ||
        context->HostStack == NULL || context->MsrBitmap == NULL ||
        context->IoBitmapA == NULL || context->IoBitmapB == NULL) {
        if (context->Vmxon != NULL) MmFreeContiguousMemory(context->Vmxon);
        if (context->Vmcs != NULL) MmFreeContiguousMemory(context->Vmcs);
        if (context->HostStack != NULL) {
            ExFreePoolWithTag(context->HostStack, HV_POOL_TAG_BACKEND);
        }
        if (context->MsrBitmap != NULL) {
            MmFreeContiguousMemory(context->MsrBitmap);
        }
        if (context->IoBitmapA != NULL) {
            MmFreeContiguousMemory(context->IoBitmapA);
        }
        if (context->IoBitmapB != NULL) {
            MmFreeContiguousMemory(context->IoBitmapB);
        }
        ExFreePoolWithTag(context, HV_POOL_TAG_BACKEND);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    context->VmxonPhysical = MmGetPhysicalAddress(context->Vmxon);
    context->VmcsPhysical = MmGetPhysicalAddress(context->Vmcs);
    context->MsrBitmapPhysical = MmGetPhysicalAddress(context->MsrBitmap);
    context->IoBitmapAPhysical = MmGetPhysicalAddress(context->IoBitmapA);
    context->IoBitmapBPhysical = MmGetPhysicalAddress(context->IoBitmapB);
    context->BackendContext = backend;
    context->SlatGeneration = backend->SlatGeneration;
    context->StopCookie = __rdtsc() ^ (ULONG64)context ^
                          (ULONG64)context->VmcsPhysical.QuadPart;
    IntelInitializeMsrBitmap((UCHAR*)context->MsrBitmap);
    *(ULONG*)context->Vmxon = (ULONG)(backend->VmxBasic & 0x7fffffffu);
    *(ULONG*)context->Vmcs = (ULONG)(backend->VmxBasic & 0x7fffffffu);
    Cpu->VendorContext = context;
    return STATUS_SUCCESS;
}

static VOID
IntelFreeCpu(
    _Inout_ HV_STATE* State,
    _Inout_ HV_CPU* Cpu
    )
{
    INTEL_CPU_CONTEXT* context;

    UNREFERENCED_PARAMETER(State);
    if (Cpu == NULL || Cpu->VendorContext == NULL) {
        return;
    }
    context = (INTEL_CPU_CONTEXT*)Cpu->VendorContext;
    if (context->HostStack != NULL) {
        RtlSecureZeroMemory(context->HostStack, INTEL_HOST_STACK_SIZE);
        ExFreePoolWithTag(context->HostStack, HV_POOL_TAG_BACKEND);
    }
    if (context->IoBitmapB != NULL) {
        MmFreeContiguousMemory(context->IoBitmapB);
    }
    if (context->IoBitmapA != NULL) {
        MmFreeContiguousMemory(context->IoBitmapA);
    }
    if (context->MsrBitmap != NULL) {
        MmFreeContiguousMemory(context->MsrBitmap);
    }
    if (context->Vmcs != NULL) MmFreeContiguousMemory(context->Vmcs);
    if (context->Vmxon != NULL) MmFreeContiguousMemory(context->Vmxon);
    RtlSecureZeroMemory(context, sizeof(*context));
    ExFreePoolWithTag(context, HV_POOL_TAG_BACKEND);
    Cpu->VendorContext = NULL;
}

static ULONG64
IntelDescriptorBase(
    _In_ ULONG64 Descriptor,
    _In_opt_ const ULONG* HighBase
    )
{
    ULONG64 base = ((Descriptor >> 16) & 0xffffull) |
                   ((Descriptor >> 32) & 0xff0000ull) |
                   ((Descriptor >> 56) & 0xff000000ull);
    if (HighBase != NULL) {
        base |= (ULONG64)(*HighBase) << 32;
    }
    return base;
}

static NTSTATUS
IntelCaptureSegment(
    _In_ USHORT Selector,
    _In_ const INTEL_DESCRIPTOR_TABLE_REGISTER* Gdtr,
    _In_ ULONG MsrBase,
    _Out_ INTEL_SEGMENT_STATE* Segment
    )
{
    ULONG offset = Selector & ~7u;
    ULONG64 descriptor;
    ULONG64 limit;
    const ULONG* highBase = NULL;

    RtlZeroMemory(Segment, sizeof(*Segment));
    Segment->Selector = Selector;
    if (Selector == 0) {
        Segment->AccessRights = 1u << 16;
        return STATUS_SUCCESS;
    }
    if ((Selector & 4) != 0 || offset > Gdtr->Limit ||
        Gdtr->Limit - offset < sizeof(ULONG64) - 1) {
        return STATUS_DATA_ERROR;
    }

    descriptor = *(UNALIGNED const ULONG64*)(Gdtr->Base + offset);
    if (((descriptor >> 44) & 1) == 0) {
        if (Gdtr->Limit - offset < 11) {
            return STATUS_DATA_ERROR;
        }
        highBase = (const ULONG*)(Gdtr->Base + offset + 8);
    }
    limit = (descriptor & 0xffff) | ((descriptor >> 32) & 0xf0000);
    if ((descriptor & (1ull << 55)) != 0) {
        limit = (limit << PAGE_SHIFT) | (PAGE_SIZE - 1);
    }
    Segment->Limit = (ULONG)limit;
    Segment->AccessRights = (ULONG)((descriptor >> 40) & 0xff) |
                            (ULONG)((descriptor >> 52) & 0xf) << 12;
    Segment->Base = MsrBase != 0
        ? __readmsr(MsrBase)
        : IntelDescriptorBase(descriptor, highBase);
    return STATUS_SUCCESS;
}

static ULONG
IntelAdjustControls(
    _In_ ULONG Desired,
    _In_ ULONG Msr
    )
{
    ULONG64 capability = __readmsr(Msr);
    return (Desired | (ULONG)capability) & (ULONG)(capability >> 32);
}

static NTSTATUS
IntelVmWrite(
    _In_ ULONG Field,
    _In_ ULONG64 Value
    )
{
    return __vmx_vmwrite(Field, (SIZE_T)Value) == 0
        ? STATUS_SUCCESS
        : STATUS_HV_INVALID_VP_STATE;
}

#define VMX_WRITE(_field, _value)                         \
    do {                                                  \
        status = IntelVmWrite((_field), (ULONG64)(_value)); \
        if (!NT_SUCCESS(status)) return status;           \
    } while (0)

static NTSTATUS
IntelSetupVmcs(
    _Inout_ HV_STATE* State,
    _Inout_ HV_CPU* Cpu
    )
{
    INTEL_BACKEND_CONTEXT* backend =
        (INTEL_BACKEND_CONTEXT*)State->BackendContext;
    INTEL_CPU_CONTEXT* context = (INTEL_CPU_CONTEXT*)Cpu->VendorContext;
    INTEL_DESCRIPTOR_TABLE_REGISTER gdtr;
    INTEL_DESCRIPTOR_TABLE_REGISTER idtr;
    INTEL_SEGMENT_STATE es, cs, ss, ds, fs, gs, ldtr, tr;
    ULONG pinMsr;
    ULONG primaryMsr;
    ULONG exitMsr;
    ULONG entryMsr;
    ULONG pinControls;
    ULONG primaryControls;
    ULONG secondaryControls;
    ULONG exitControls;
    ULONG entryControls;
    ULONG desiredSecondary;
    ULONG requiredSecondary;
    ULONG desiredPrimary;
    ULONG64 hostRsp;
    ULONG64 eptp;
    int cpuid[4];
    NTSTATUS status;

    IntelAsmStoreGdtr(&gdtr);
    IntelAsmStoreIdtr(&idtr);
    status = IntelCaptureSegment(IntelAsmReadEs(), &gdtr, 0, &es);
    if (!NT_SUCCESS(status)) return status;
    status = IntelCaptureSegment(IntelAsmReadCs(), &gdtr, 0, &cs);
    if (!NT_SUCCESS(status)) return status;
    status = IntelCaptureSegment(IntelAsmReadSs(), &gdtr, 0, &ss);
    if (!NT_SUCCESS(status)) return status;
    status = IntelCaptureSegment(IntelAsmReadDs(), &gdtr, 0, &ds);
    if (!NT_SUCCESS(status)) return status;
    status = IntelCaptureSegment(IntelAsmReadFs(), &gdtr, IA32_FS_BASE, &fs);
    if (!NT_SUCCESS(status)) return status;
    status = IntelCaptureSegment(IntelAsmReadGs(), &gdtr, IA32_GS_BASE, &gs);
    if (!NT_SUCCESS(status)) return status;
    status = IntelCaptureSegment(IntelAsmReadLdtr(), &gdtr, 0, &ldtr);
    if (!NT_SUCCESS(status)) return status;
    status = IntelCaptureSegment(IntelAsmReadTr(), &gdtr, 0, &tr);
    if (!NT_SUCCESS(status)) return status;

    pinMsr = (backend->VmxBasic & (1ull << 55)) != 0
        ? IA32_VMX_TRUE_PINBASED_CTLS : IA32_VMX_PINBASED_CTLS;
    primaryMsr = (backend->VmxBasic & (1ull << 55)) != 0
        ? IA32_VMX_TRUE_PROCBASED_CTLS : IA32_VMX_PROCBASED_CTLS;
    exitMsr = (backend->VmxBasic & (1ull << 55)) != 0
        ? IA32_VMX_TRUE_EXIT_CTLS : IA32_VMX_EXIT_CTLS;
    entryMsr = (backend->VmxBasic & (1ull << 55)) != 0
        ? IA32_VMX_TRUE_ENTRY_CTLS : IA32_VMX_ENTRY_CTLS;

    desiredSecondary = VMX_SECONDARY_ENABLE_EPT;
    requiredSecondary = VMX_SECONDARY_ENABLE_EPT;
    __cpuid(cpuid, 0x80000000);
    if ((ULONG)cpuid[0] >= 0x80000001u) {
        __cpuid(cpuid, 0x80000001);
        if ((((ULONG)cpuid[3]) & (1u << 27)) != 0) {
            desiredSecondary |= VMX_SECONDARY_ENABLE_RDTSCP;
            requiredSecondary |= VMX_SECONDARY_ENABLE_RDTSCP;
        }
    }
    __cpuid(cpuid, 0);
    if ((ULONG)cpuid[0] >= 7) {
        __cpuidex(cpuid, 7, 0);
        if ((((ULONG)cpuid[1]) & (1u << 10)) != 0) {
            desiredSecondary |= VMX_SECONDARY_ENABLE_INVPCID;
            requiredSecondary |= VMX_SECONDARY_ENABLE_INVPCID;
        }
    }
    __cpuid(cpuid, 0);
    if ((ULONG)cpuid[0] >= 0xDu) {
        __cpuidex(cpuid, 0xD, 1);
        if ((((ULONG)cpuid[0]) & (1u << 3)) != 0) {
            desiredSecondary |= VMX_SECONDARY_ENABLE_XSAVES;
            requiredSecondary |= VMX_SECONDARY_ENABLE_XSAVES;
        }
    }
    if ((backend->EptVpidCapabilities & (1ull << 32)) != 0 &&
        (backend->EptVpidCapabilities & (1ull << 41)) != 0 &&
        Cpu->ProcessorIndex < MAXUSHORT) {
        desiredSecondary |= VMX_SECONDARY_ENABLE_VPID;
    }

    desiredPrimary = VMX_PRIMARY_ACTIVATE_SECONDARY |
                     VMX_PRIMARY_USE_IO_BITMAPS |
                     VMX_PRIMARY_USE_MSR_BITMAPS;
    pinControls = IntelAdjustControls(0, pinMsr);
    primaryControls = IntelAdjustControls(
        desiredPrimary, primaryMsr);
    secondaryControls = IntelAdjustControls(
        desiredSecondary, IA32_VMX_PROCBASED_CTLS2);
    exitControls = IntelAdjustControls(
        VMX_EXIT_HOST_ADDRESS_SPACE_SIZE |
        (1u << 18) | (1u << 19) | (1u << 20) | (1u << 21),
        exitMsr);
    entryControls = IntelAdjustControls(
        VMX_ENTRY_IA32E_MODE | (1u << 14) | (1u << 15), entryMsr);
    if (pinControls != 0 ||
        (primaryControls & ~desiredPrimary) != 0 ||
        (secondaryControls & ~desiredSecondary) != 0 ||
        (primaryControls & VMX_PRIMARY_ACTIVATE_SECONDARY) == 0 ||
        (primaryControls & (VMX_PRIMARY_USE_IO_BITMAPS |
                            VMX_PRIMARY_USE_MSR_BITMAPS)) !=
            (VMX_PRIMARY_USE_IO_BITMAPS | VMX_PRIMARY_USE_MSR_BITMAPS) ||
        (secondaryControls & requiredSecondary) != requiredSecondary ||
        (exitControls & VMX_EXIT_HOST_ADDRESS_SPACE_SIZE) == 0 ||
        (entryControls & VMX_ENTRY_IA32E_MODE) == 0) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }

    eptp = ((ULONG64)MmGetPhysicalAddress(backend->Pml4).QuadPart &
            EPT_ADDRESS_MASK) | EPT_MEMORY_TYPE_WB | (3ull << 3);
    context->EptPointer = eptp;
    context->Vpid = (secondaryControls & VMX_SECONDARY_ENABLE_VPID) != 0
        ? (USHORT)(Cpu->ProcessorIndex + 1) : 0;
    hostRsp = ((ULONG64)context->HostStack + INTEL_HOST_STACK_SIZE - 64) &
              ~0xfull;
    *(HV_CPU**)hostRsp = Cpu;

    VMX_WRITE(VMCS_PIN_BASED_CONTROLS, pinControls);
    VMX_WRITE(VMCS_PRIMARY_PROCESSOR_CONTROLS, primaryControls);
    VMX_WRITE(VMCS_SECONDARY_PROCESSOR_CONTROLS, secondaryControls);
    VMX_WRITE(VMCS_EXIT_CONTROLS, exitControls);
    VMX_WRITE(VMCS_ENTRY_CONTROLS, entryControls);
    VMX_WRITE(VMCS_EXCEPTION_BITMAP, 0);
    VMX_WRITE(VMCS_PAGE_FAULT_ERROR_MASK, 0);
    VMX_WRITE(VMCS_PAGE_FAULT_ERROR_MATCH, 0);
    VMX_WRITE(VMCS_CR3_TARGET_COUNT, 0);
    VMX_WRITE(VMCS_EXIT_MSR_STORE_COUNT, 0);
    VMX_WRITE(VMCS_EXIT_MSR_LOAD_COUNT, 0);
    VMX_WRITE(VMCS_ENTRY_MSR_LOAD_COUNT, 0);
    VMX_WRITE(VMCS_ENTRY_INTERRUPTION_INFO, 0);
    VMX_WRITE(VMCS_ENTRY_EXCEPTION_ERROR, 0);
    VMX_WRITE(VMCS_ENTRY_INSTRUCTION_LENGTH, 0);
    VMX_WRITE(VMCS_TPR_THRESHOLD, 0);
    VMX_WRITE(VMCS_IO_BITMAP_A, context->IoBitmapAPhysical.QuadPart);
    VMX_WRITE(VMCS_IO_BITMAP_B, context->IoBitmapBPhysical.QuadPart);
    VMX_WRITE(VMCS_MSR_BITMAP, context->MsrBitmapPhysical.QuadPart);
    VMX_WRITE(VMCS_EPT_POINTER, eptp);
    VMX_WRITE(VMCS_VPID, context->Vpid);
    if ((secondaryControls & VMX_SECONDARY_ENABLE_XSAVES) != 0) {
        VMX_WRITE(VMCS_XSS_EXITING_BITMAP, 0);
        VMX_WRITE(VMCS_GUEST_XSS, __readmsr(IA32_XSS));
        VMX_WRITE(VMCS_HOST_XSS, __readmsr(IA32_XSS));
    }

    VMX_WRITE(VMCS_GUEST_ES_SELECTOR, es.Selector);
    VMX_WRITE(VMCS_GUEST_CS_SELECTOR, cs.Selector);
    VMX_WRITE(VMCS_GUEST_SS_SELECTOR, ss.Selector);
    VMX_WRITE(VMCS_GUEST_DS_SELECTOR, ds.Selector);
    VMX_WRITE(VMCS_GUEST_FS_SELECTOR, fs.Selector);
    VMX_WRITE(VMCS_GUEST_GS_SELECTOR, gs.Selector);
    VMX_WRITE(VMCS_GUEST_LDTR_SELECTOR, ldtr.Selector);
    VMX_WRITE(VMCS_GUEST_TR_SELECTOR, tr.Selector);
    VMX_WRITE(VMCS_GUEST_ES_LIMIT, es.Limit);
    VMX_WRITE(VMCS_GUEST_CS_LIMIT, cs.Limit);
    VMX_WRITE(VMCS_GUEST_SS_LIMIT, ss.Limit);
    VMX_WRITE(VMCS_GUEST_DS_LIMIT, ds.Limit);
    VMX_WRITE(VMCS_GUEST_FS_LIMIT, fs.Limit);
    VMX_WRITE(VMCS_GUEST_GS_LIMIT, gs.Limit);
    VMX_WRITE(VMCS_GUEST_LDTR_LIMIT, ldtr.Limit);
    VMX_WRITE(VMCS_GUEST_TR_LIMIT, tr.Limit);
    VMX_WRITE(VMCS_GUEST_GDTR_LIMIT, gdtr.Limit);
    VMX_WRITE(VMCS_GUEST_IDTR_LIMIT, idtr.Limit);
    VMX_WRITE(VMCS_GUEST_ES_AR, es.AccessRights);
    VMX_WRITE(VMCS_GUEST_CS_AR, cs.AccessRights);
    VMX_WRITE(VMCS_GUEST_SS_AR, ss.AccessRights);
    VMX_WRITE(VMCS_GUEST_DS_AR, ds.AccessRights);
    VMX_WRITE(VMCS_GUEST_FS_AR, fs.AccessRights);
    VMX_WRITE(VMCS_GUEST_GS_AR, gs.AccessRights);
    VMX_WRITE(VMCS_GUEST_LDTR_AR, ldtr.AccessRights);
    VMX_WRITE(VMCS_GUEST_TR_AR, tr.AccessRights);
    VMX_WRITE(VMCS_GUEST_ES_BASE, es.Base);
    VMX_WRITE(VMCS_GUEST_CS_BASE, cs.Base);
    VMX_WRITE(VMCS_GUEST_SS_BASE, ss.Base);
    VMX_WRITE(VMCS_GUEST_DS_BASE, ds.Base);
    VMX_WRITE(VMCS_GUEST_FS_BASE, fs.Base);
    VMX_WRITE(VMCS_GUEST_GS_BASE, gs.Base);
    VMX_WRITE(VMCS_GUEST_LDTR_BASE, ldtr.Base);
    VMX_WRITE(VMCS_GUEST_TR_BASE, tr.Base);
    VMX_WRITE(VMCS_GUEST_GDTR_BASE, gdtr.Base);
    VMX_WRITE(VMCS_GUEST_IDTR_BASE, idtr.Base);
    VMX_WRITE(VMCS_GUEST_CR0, __readcr0());
    VMX_WRITE(VMCS_GUEST_CR3, __readcr3());
    VMX_WRITE(VMCS_GUEST_CR4, __readcr4());
    VMX_WRITE(VMCS_GUEST_DR7, __readdr(7));
    VMX_WRITE(VMCS_GUEST_RFLAGS, __readeflags());
    VMX_WRITE(VMCS_GUEST_PENDING_DEBUG, 0);
    VMX_WRITE(VMCS_GUEST_INTERRUPTIBILITY, 0);
    VMX_WRITE(VMCS_GUEST_ACTIVITY_STATE, 0);
    VMX_WRITE(VMCS_GUEST_SMBASE, 0);
    VMX_WRITE(VMCS_GUEST_VMCS_LINK_POINTER, MAXULONGLONG);
    VMX_WRITE(VMCS_GUEST_DEBUGCTL, __readmsr(IA32_DEBUGCTL));
    VMX_WRITE(VMCS_GUEST_PAT, __readmsr(IA32_PAT));
    VMX_WRITE(VMCS_GUEST_EFER, __readmsr(IA32_EFER));
    VMX_WRITE(VMCS_GUEST_SYSENTER_CS, __readmsr(IA32_SYSENTER_CS));
    VMX_WRITE(VMCS_GUEST_SYSENTER_ESP, __readmsr(IA32_SYSENTER_ESP));
    VMX_WRITE(VMCS_GUEST_SYSENTER_EIP, __readmsr(IA32_SYSENTER_EIP));
    VMX_WRITE(VMCS_CR0_GUEST_HOST_MASK,
        __readmsr(IA32_VMX_CR0_FIXED0) ^ __readmsr(IA32_VMX_CR0_FIXED1));
    VMX_WRITE(VMCS_CR4_GUEST_HOST_MASK,
        (__readmsr(IA32_VMX_CR4_FIXED0) ^
         __readmsr(IA32_VMX_CR4_FIXED1)) | VMX_CR4_VMXE);
    VMX_WRITE(VMCS_CR0_READ_SHADOW, context->OriginalCr0);
    VMX_WRITE(VMCS_CR4_READ_SHADOW, context->OriginalCr4);

    VMX_WRITE(VMCS_HOST_ES_SELECTOR, es.Selector & ~7u);
    VMX_WRITE(VMCS_HOST_CS_SELECTOR, cs.Selector & ~7u);
    VMX_WRITE(VMCS_HOST_SS_SELECTOR, ss.Selector & ~7u);
    VMX_WRITE(VMCS_HOST_DS_SELECTOR, ds.Selector & ~7u);
    VMX_WRITE(VMCS_HOST_FS_SELECTOR, fs.Selector & ~7u);
    VMX_WRITE(VMCS_HOST_GS_SELECTOR, gs.Selector & ~7u);
    VMX_WRITE(VMCS_HOST_TR_SELECTOR, tr.Selector & ~7u);
    VMX_WRITE(VMCS_HOST_CR0, __readcr0());
    VMX_WRITE(VMCS_HOST_CR3, __readcr3());
    VMX_WRITE(VMCS_HOST_CR4, __readcr4());
    VMX_WRITE(VMCS_HOST_FS_BASE, fs.Base);
    VMX_WRITE(VMCS_HOST_GS_BASE, gs.Base);
    VMX_WRITE(VMCS_HOST_TR_BASE, tr.Base);
    VMX_WRITE(VMCS_HOST_GDTR_BASE, gdtr.Base);
    VMX_WRITE(VMCS_HOST_IDTR_BASE, idtr.Base);
    VMX_WRITE(VMCS_HOST_PAT, __readmsr(IA32_PAT));
    VMX_WRITE(VMCS_HOST_EFER, __readmsr(IA32_EFER));
    VMX_WRITE(VMCS_HOST_SYSENTER_CS, __readmsr(IA32_SYSENTER_CS));
    VMX_WRITE(VMCS_HOST_SYSENTER_ESP, __readmsr(IA32_SYSENTER_ESP));
    VMX_WRITE(VMCS_HOST_SYSENTER_EIP, __readmsr(IA32_SYSENTER_EIP));
    VMX_WRITE(VMCS_HOST_RSP, hostRsp);
    VMX_WRITE(VMCS_HOST_RIP, (ULONG64)IntelAsmVmExit);
    return STATUS_SUCCESS;
}

static BOOLEAN
IntelCurrentCpuMatches(
    _In_ HV_CPU* Cpu
    )
{
    PROCESSOR_NUMBER number;
    KeGetCurrentProcessorNumberEx(&number);
    return KeGetProcessorIndexFromNumber(&number) == Cpu->ProcessorIndex;
}

static NTSTATUS
IntelStart(
    _Inout_ HV_STATE* State,
    _Inout_ HV_CPU* Cpu
    )
{
    INTEL_CPU_CONTEXT* context;
    ULONG64 vmxonPhysical;
    ULONG64 vmcsPhysical;
    ULONG64 cr0;
    ULONG64 cr4;
    ULONG64 featureControl;
    NTSTATUS status;

    if (!IntelCurrentCpuMatches(Cpu) || Cpu->VendorContext == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    context = (INTEL_CPU_CONTEXT*)Cpu->VendorContext;
    context->OriginalCr0 = __readcr0();
    context->OriginalCr4 = __readcr4();

    featureControl = __readmsr(IA32_FEATURE_CONTROL);
    if ((featureControl & 1) == 0) {
        __writemsr(IA32_FEATURE_CONTROL, featureControl | 1 | (1ull << 2));
    }
    cr0 = (context->OriginalCr0 | __readmsr(IA32_VMX_CR0_FIXED0)) &
          __readmsr(IA32_VMX_CR0_FIXED1);
    cr4 = (context->OriginalCr4 | __readmsr(IA32_VMX_CR4_FIXED0) |
           VMX_CR4_VMXE) & __readmsr(IA32_VMX_CR4_FIXED1);
    __writecr0(cr0);
    __writecr4(cr4);

    vmxonPhysical = (ULONG64)context->VmxonPhysical.QuadPart;
    if (__vmx_on(&vmxonPhysical) != 0) {
        status = STATUS_HV_FEATURE_UNAVAILABLE;
        goto RestoreRegisters;
    }
    context->VmxOn = TRUE;

    vmcsPhysical = (ULONG64)context->VmcsPhysical.QuadPart;
    if (__vmx_vmclear(&vmcsPhysical) != 0 ||
        __vmx_vmptrld(&vmcsPhysical) != 0) {
        status = STATUS_HV_INVALID_VP_STATE;
        goto LeaveVmx;
    }
    status = IntelSetupVmcs(State, Cpu);
    if (!NT_SUCCESS(status)) {
        goto LeaveVmx;
    }

    {
        INTEL_INVALIDATION_DESCRIPTOR descriptor;
        descriptor.Context = context->EptPointer;
        descriptor.Reserved = 0;
        ULONG type =
            (((INTEL_BACKEND_CONTEXT*)context->BackendContext)
                ->EptVpidCapabilities & (1ull << 25)) != 0
            ? INVEPT_SINGLE_CONTEXT : INVEPT_ALL_CONTEXTS;
        if (type == INVEPT_ALL_CONTEXTS) descriptor.Context = 0;
        if (IntelAsmInvept(type, &descriptor) != 0) {
            status = STATUS_HV_OPERATION_FAILED;
            goto LeaveVmx;
        }
        if (context->Vpid != 0) {
            descriptor.Context = context->Vpid;
            if (IntelAsmInvvpid(
                    INVVPID_SINGLE_CONTEXT, &descriptor) != 0) {
                status = STATUS_HV_OPERATION_FAILED;
                goto LeaveVmx;
            }
        }
    }

    context->Launched = TRUE;
    if (IntelAsmLaunch() == 0) {
        return STATUS_SUCCESS;
    }
    context->Launched = FALSE;
    {
        SIZE_T instructionError;
        if (__vmx_vmread(
                VMCS_VM_INSTRUCTION_ERROR, &instructionError) == 0) {
            context->LastVmxError = (ULONG)instructionError;
        }
    }
    status = STATUS_HV_INVALID_VP_STATE;

LeaveVmx:
    __vmx_off();
    context->VmxOn = FALSE;
RestoreRegisters:
    __writecr4(context->OriginalCr4);
    __writecr0(context->OriginalCr0);
    return status;
}

static NTSTATUS
IntelStop(
    _Inout_ HV_STATE* State,
    _Inout_ HV_CPU* Cpu
    )
{
    INTEL_CPU_CONTEXT* context;

    UNREFERENCED_PARAMETER(State);
    if (!IntelCurrentCpuMatches(Cpu) || Cpu->VendorContext == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    context = (INTEL_CPU_CONTEXT*)Cpu->VendorContext;
    if (!context->Launched) {
        return STATUS_SUCCESS;
    }

    IntelAsmStop(context->StopCookie);
    if (context->Launched || context->VmxOn) {
        return STATUS_HV_OPERATION_FAILED;
    }
    __writecr4(context->OriginalCr4);
    __writecr0(context->OriginalCr0);
    return STATUS_SUCCESS;
}

NTSTATUS
IntelSetLaunchState(
    _In_ ULONG64 GuestRsp,
    _In_ ULONG64 GuestRip
    )
{
    NTSTATUS status;

    status = IntelVmWrite(VMCS_GUEST_RSP, GuestRsp);
    if (NT_SUCCESS(status)) {
        status = IntelVmWrite(VMCS_GUEST_RIP, GuestRip);
    }
    return status;
}

_Success_(return != FALSE)
static BOOLEAN
IntelVmReadValue(
    _In_ ULONG Field,
    _Out_ ULONG64* Value
    )
{
    SIZE_T value;
    if (__vmx_vmread(Field, &value) != 0) return FALSE;
    *Value = value;
    return TRUE;
}

static VOID
IntelInjectException(
    _In_ ULONG Information,
    _In_ ULONG ErrorCode
    )
{
    (VOID)IntelVmWrite(VMCS_ENTRY_INTERRUPTION_INFO, Information);
    if ((Information & (1u << 11)) != 0) {
        (VOID)IntelVmWrite(VMCS_ENTRY_EXCEPTION_ERROR, ErrorCode);
    }
}

static VOID
IntelInjectInvalidOpcode(
    VOID
    )
{
    IntelInjectException(VMX_ENTRY_INJECT_UD, 0);
}

static VOID
IntelPreserveVectoringEvent(
    _In_ ULONG ExitInstructionLength
    )
{
    ULONG64 information;
    ULONG type;

    (VOID)IntelVmWrite(VMCS_ENTRY_INTERRUPTION_INFO, 0);
    if (!IntelVmReadValue(VMCS_IDT_VECTORING_INFO, &information) ||
        (information & (1ull << 31)) == 0) {
        return;
    }
    type = (ULONG)((information >> 8) & 7);
    if (type == 0) return;
    (VOID)IntelVmWrite(VMCS_ENTRY_INTERRUPTION_INFO, information);
    if ((information & (1ull << 11)) != 0) {
        ULONG64 errorCode;
        if (IntelVmReadValue(VMCS_IDT_VECTORING_ERROR, &errorCode)) {
            (VOID)IntelVmWrite(VMCS_ENTRY_EXCEPTION_ERROR, errorCode);
        }
    }
    if (type == 4 || type == 5 || type == 6) {
        (VOID)IntelVmWrite(
            VMCS_ENTRY_INSTRUCTION_LENGTH, ExitInstructionLength);
    }
}

static ULONG64
IntelGetRegister(
    _In_ INTEL_GUEST_REGISTERS* Registers,
    _In_ ULONG Index
    )
{
    ULONG64 value = 0;
    switch (Index) {
    case 0: value = Registers->Rax; break;
    case 1: value = Registers->Rcx; break;
    case 2: value = Registers->Rdx; break;
    case 3: value = Registers->Rbx; break;
    case 4: (VOID)IntelVmReadValue(VMCS_GUEST_RSP, &value); break;
    case 5: value = Registers->Rbp; break;
    case 6: value = Registers->Rsi; break;
    case 7: value = Registers->Rdi; break;
    case 8: value = Registers->R8; break;
    case 9: value = Registers->R9; break;
    case 10: value = Registers->R10; break;
    case 11: value = Registers->R11; break;
    case 12: value = Registers->R12; break;
    case 13: value = Registers->R13; break;
    case 14: value = Registers->R14; break;
    case 15: value = Registers->R15; break;
    default: break;
    }
    return value;
}

static VOID
IntelSetRegister(
    _Inout_ INTEL_GUEST_REGISTERS* Registers,
    _In_ ULONG Index,
    _In_ ULONG64 Value
    )
{
    switch (Index) {
    case 0: Registers->Rax = Value; break;
    case 1: Registers->Rcx = Value; break;
    case 2: Registers->Rdx = Value; break;
    case 3: Registers->Rbx = Value; break;
    case 4: (VOID)IntelVmWrite(VMCS_GUEST_RSP, Value); break;
    case 5: Registers->Rbp = Value; break;
    case 6: Registers->Rsi = Value; break;
    case 7: Registers->Rdi = Value; break;
    case 8: Registers->R8 = Value; break;
    case 9: Registers->R9 = Value; break;
    case 10: Registers->R10 = Value; break;
    case 11: Registers->R11 = Value; break;
    case 12: Registers->R12 = Value; break;
    case 13: Registers->R13 = Value; break;
    case 14: Registers->R14 = Value; break;
    case 15: Registers->R15 = Value; break;
    default: break;
    }
}

static BOOLEAN
IntelHandleCrAccess(
    _Inout_ INTEL_GUEST_REGISTERS* Registers
    )
{
    ULONG64 qualification;
    ULONG64 requested;
    ULONG64 value;
    ULONG cr;
    ULONG access;
    ULONG reg;

    if (!IntelVmReadValue(VMCS_EXIT_QUALIFICATION, &qualification)) {
        return FALSE;
    }
    cr = (ULONG)(qualification & 0xf);
    access = (ULONG)((qualification >> 4) & 3);
    reg = (ULONG)((qualification >> 8) & 0xf);

    if (access == 0) {
        requested = IntelGetRegister(Registers, reg);
        if (cr == 0) {
            value = (requested | __readmsr(IA32_VMX_CR0_FIXED0)) &
                    __readmsr(IA32_VMX_CR0_FIXED1);
            (VOID)IntelVmWrite(VMCS_GUEST_CR0, value);
            (VOID)IntelVmWrite(VMCS_CR0_READ_SHADOW, requested);
        } else if (cr == 3) {
            (VOID)IntelVmWrite(VMCS_GUEST_CR3, requested);
        } else if (cr == 4) {
            value = (requested | __readmsr(IA32_VMX_CR4_FIXED0) |
                     VMX_CR4_VMXE) & __readmsr(IA32_VMX_CR4_FIXED1);
            (VOID)IntelVmWrite(VMCS_GUEST_CR4, value);
            (VOID)IntelVmWrite(VMCS_CR4_READ_SHADOW, requested);
        } else {
            return FALSE;
        }
        return TRUE;
    }

    if (access == 1) {
        if (cr == 0) {
            if (!IntelVmReadValue(VMCS_CR0_READ_SHADOW, &value)) return FALSE;
        } else if (cr == 3) {
            if (!IntelVmReadValue(VMCS_GUEST_CR3, &value)) return FALSE;
        } else if (cr == 4) {
            if (!IntelVmReadValue(VMCS_CR4_READ_SHADOW, &value)) return FALSE;
        } else {
            return FALSE;
        }
        IntelSetRegister(Registers, reg, value);
        return TRUE;
    }

    if (access == 2 && cr == 0) {
        if (!IntelVmReadValue(VMCS_CR0_READ_SHADOW, &requested)) return FALSE;
        requested &= ~(1ull << 3);
        value = (requested | __readmsr(IA32_VMX_CR0_FIXED0)) &
                __readmsr(IA32_VMX_CR0_FIXED1);
        (VOID)IntelVmWrite(VMCS_GUEST_CR0, value);
        (VOID)IntelVmWrite(VMCS_CR0_READ_SHADOW, requested);
        return TRUE;
    }

    if (access == 3 && cr == 0) {
        if (!IntelVmReadValue(VMCS_CR0_READ_SHADOW, &requested)) return FALSE;
        value = (qualification >> 16) & 0xffff;
        value = (requested & ~0xfull) | (value & 0xf);
        if ((requested & 1) != 0) value |= 1;
        requested = value;
        value = (requested | __readmsr(IA32_VMX_CR0_FIXED0)) &
                __readmsr(IA32_VMX_CR0_FIXED1);
        (VOID)IntelVmWrite(VMCS_GUEST_CR0, value);
        (VOID)IntelVmWrite(VMCS_CR0_READ_SHADOW, requested);
        return TRUE;
    }
    return FALSE;
}

static BOOLEAN
IntelHandleXsetbv(
    _In_ INTEL_GUEST_REGISTERS* Registers
    )
{
    int cpuid[4];
    ULONG64 supported;
    ULONG64 requested;
    ULONG64 avx512;

    if ((ULONG)Registers->Rcx != 0) return FALSE;
    __cpuidex(cpuid, 0xD, 0);
    supported = ((ULONG64)(ULONG)cpuid[3] << 32) | (ULONG)cpuid[0];
    requested = ((ULONG64)(ULONG)Registers->Rdx << 32) |
                (ULONG)Registers->Rax;
    if ((requested & ~supported) != 0 || (requested & 1) == 0) return FALSE;
    if ((requested & (1ull << 2)) != 0 &&
        (requested & (1ull << 1)) == 0) return FALSE;
    if (((requested >> 3) & 3) == 1 || ((requested >> 3) & 3) == 2) {
        return FALSE;
    }
    avx512 = requested & (7ull << 5);
    if (avx512 != 0 &&
        (avx512 != (7ull << 5) || (requested & (1ull << 2)) == 0)) {
        return FALSE;
    }
    if ((requested & (1ull << 18)) != 0 &&
        (requested & (1ull << 17)) == 0) return FALSE;
    _xsetbv(0, requested);
    return TRUE;
}

static BOOLEAN
IntelIsVmxInstructionExit(
    _In_ ULONG Reason
    )
{
    return (Reason >= 19 && Reason <= 27) ||
           Reason == 50 || Reason == 53 || Reason == 59;
}

ULONG
IntelVmExitHandler(
    _Inout_ INTEL_GUEST_REGISTERS* Registers,
    _Inout_ HV_CPU* Cpu
    )
{
    INTEL_CPU_CONTEXT* context;
    SIZE_T value;
    ULONG reason;
    ULONG instructionLength;
    ULONG64 guestRip;
    int cpuid[4];

    if (Registers == NULL || Cpu == NULL || Cpu->VendorContext == NULL) {
        KeBugCheckEx(HYPERVISOR_ERROR, INTEL_BUGCHECK_UNEXPECTED_EXIT,
            MAXULONG, 0, 0);
    }
    context = (INTEL_CPU_CONTEXT*)Cpu->VendorContext;
    if (__vmx_vmread(VMCS_EXIT_REASON, &value) != 0) {
        KeBugCheckEx(HYPERVISOR_ERROR, INTEL_BUGCHECK_UNEXPECTED_EXIT,
            MAXULONG, 1, 0);
    }
    reason = (ULONG)value & 0xffffu;
    if (__vmx_vmread(VMCS_EXIT_INSTRUCTION_LENGTH, &value) != 0) {
        KeBugCheckEx(HYPERVISOR_ERROR, INTEL_BUGCHECK_UNEXPECTED_EXIT,
            reason, 2, 0);
    }
    instructionLength = (ULONG)value;
    if (__vmx_vmread(VMCS_GUEST_RIP, &value) != 0) {
        KeBugCheckEx(HYPERVISOR_ERROR, INTEL_BUGCHECK_UNEXPECTED_EXIT,
            reason, 3, 0);
    }
    guestRip = value;
    IntelPreserveVectoringEvent(instructionLength);
    IntelFlushEptIfNeeded(context);

    if (reason == VMX_EXIT_CPUID) {
        __cpuidex(cpuid, (int)(ULONG)Registers->Rax,
            (int)(ULONG)Registers->Rcx);
        Registers->Rax = (ULONG)cpuid[0];
        Registers->Rbx = (ULONG)cpuid[1];
        Registers->Rcx = (ULONG)cpuid[2];
        Registers->Rdx = (ULONG)cpuid[3];
        (VOID)IntelVmWrite(VMCS_GUEST_RIP, guestRip + instructionLength);
        return INTEL_VMEXIT_RESUME;
    }

    if (reason == VMX_EXIT_EXCEPTION_OR_NMI) {
        ULONG64 information;
        ULONG64 errorCode = 0;
        if (!IntelVmReadValue(VMCS_EXIT_INTERRUPTION_INFO, &information) ||
            (information & (1ull << 31)) == 0) {
            KeBugCheckEx(HYPERVISOR_ERROR,
                INTEL_BUGCHECK_UNEXPECTED_EXIT, reason, 6, guestRip);
        }
        if ((information & (1ull << 11)) != 0) {
            (VOID)IntelVmReadValue(
                VMCS_EXIT_INTERRUPTION_ERROR, &errorCode);
        }
        information &= ~(1ull << 12);
        IntelInjectException((ULONG)information, (ULONG)errorCode);
        return INTEL_VMEXIT_RESUME;
    }

    if (reason == VMX_EXIT_CR_ACCESS) {
        if (!IntelHandleCrAccess(Registers)) {
            IntelInjectException(VMX_ENTRY_INJECT_GP, 0);
        } else {
            (VOID)IntelVmWrite(
                VMCS_GUEST_RIP, guestRip + instructionLength);
        }
        return INTEL_VMEXIT_RESUME;
    }

    if (reason == VMX_EXIT_RDMSR) {
        ULONG msr = (ULONG)Registers->Rcx;
        ULONG64 msrValue;

        if (msr == IA32_FEATURE_CONTROL ||
            (msr >= IA32_VMX_BASIC && msr <= IA32_VMX_EPT_VPID_CAP) ||
            ((context->BackendContext != NULL) &&
             ((((INTEL_BACKEND_CONTEXT*)context->BackendContext)->VmxBasic &
               (1ull << 55)) != 0) &&
             msr >= IA32_VMX_TRUE_PINBASED_CTLS &&
             msr <= IA32_VMX_TRUE_ENTRY_CTLS)) {
            msrValue = __readmsr(msr);
            Registers->Rax = (ULONG)msrValue;
            Registers->Rdx = (ULONG)(msrValue >> 32);
            (VOID)IntelVmWrite(
                VMCS_GUEST_RIP, guestRip + instructionLength);
        } else {
            IntelInjectException(VMX_ENTRY_INJECT_GP, 0);
        }
        return INTEL_VMEXIT_RESUME;
    }

    if (reason == VMX_EXIT_WRMSR) {
        IntelInjectException(VMX_ENTRY_INJECT_GP, 0);
        return INTEL_VMEXIT_RESUME;
    }

    if (reason == VMX_EXIT_EPT_VIOLATION) {
        ULONG64 qualification;
        ULONG64 linearAddress;
        ULONG errorCode = 1;
        ULONG64 csSelector;

        if (!IntelVmReadValue(VMCS_EXIT_QUALIFICATION, &qualification)) {
            KeBugCheckEx(HYPERVISOR_ERROR,
                INTEL_BUGCHECK_UNEXPECTED_EXIT, reason, 5, guestRip);
        }
        if ((qualification & (1ull << 7)) == 0 ||
            !IntelVmReadValue(VMCS_GUEST_LINEAR_ADDRESS, &linearAddress)) {
            IntelInjectException(VMX_ENTRY_INJECT_GP, 0);
            return INTEL_VMEXIT_RESUME;
        }
        if ((qualification & (1ull << 1)) != 0) errorCode |= 2;
        if (IntelVmReadValue(VMCS_GUEST_CS_SELECTOR, &csSelector) &&
            (csSelector & 3) != 0) {
            errorCode |= 4;
        }
        if ((qualification & (1ull << 2)) != 0) errorCode |= 16;
        __writecr2(linearAddress);
        IntelInjectException(VMX_ENTRY_INJECT_PF, errorCode);
        return INTEL_VMEXIT_RESUME;
    }

    if (reason == VMX_EXIT_EPT_MISCONFIGURATION) {
        ULONG64 guestPhysical = 0;
        (VOID)IntelVmReadValue(
            VMCS_GUEST_PHYSICAL_ADDRESS, &guestPhysical);
        KeBugCheckEx(HYPERVISOR_ERROR,
            INTEL_BUGCHECK_UNEXPECTED_EXIT, reason, guestPhysical, guestRip);
    }

    if (reason == VMX_EXIT_XSETBV) {
        if (IntelHandleXsetbv(Registers)) {
            (VOID)IntelVmWrite(
                VMCS_GUEST_RIP, guestRip + instructionLength);
        } else {
            IntelInjectException(VMX_ENTRY_INJECT_GP, 0);
        }
        return INTEL_VMEXIT_RESUME;
    }

    if (reason == VMX_EXIT_VMCALL) {
        SIZE_T csSelector;

        if (__vmx_vmread(VMCS_GUEST_CS_SELECTOR, &csSelector) == 0 &&
            (csSelector & 3) == 0 &&
            Registers->Rax == HV_HYPERCALL_MAGIC_RAX &&
            Registers->Rcx == HV_HYPERCALL_MAGIC_RCX &&
            Registers->Rdx == HV_HYPERCALL_MAGIC_RDX &&
            Registers->R8 == HV_HYPERCALL_MAGIC_R8 &&
            Registers->R9 == context->StopCookie) {
            if (__vmx_vmread(VMCS_GUEST_RSP, &value) != 0) {
                KeBugCheckEx(HYPERVISOR_ERROR,
                    INTEL_BUGCHECK_UNEXPECTED_EXIT, reason, 4, 0);
            }
            context->ResumeRsp = value;
            context->ResumeRip = guestRip + instructionLength;
            if (context->Vpid != 0) {
                INTEL_INVALIDATION_DESCRIPTOR descriptor;
                descriptor.Context = context->Vpid;
                descriptor.Reserved = 0;
                if (IntelAsmInvvpid(
                        INVVPID_SINGLE_CONTEXT, &descriptor) != 0) {
                    KeBugCheckEx(HYPERVISOR_ERROR,
                        INTEL_BUGCHECK_INVALIDATION,
                        context->Vpid, 0, 0);
                }
            }
            context->Launched = FALSE;
            context->VmxOn = FALSE;
            return INTEL_VMEXIT_STOP;
        }
        IntelInjectInvalidOpcode();
        return INTEL_VMEXIT_RESUME;
    }

    if (IntelIsVmxInstructionExit(reason)) {
        IntelInjectInvalidOpcode();
        return INTEL_VMEXIT_RESUME;
    }

    KeBugCheckEx(HYPERVISOR_ERROR, INTEL_BUGCHECK_UNEXPECTED_EXIT,
        Cpu->ProcessorIndex, reason, guestRip);
}

static const HV_BACKEND_OPS IntelBackendOps = {
    "Intel VMX/EPT",
    IntelSupport,
    IntelPrepare,
    IntelFree,
    IntelPrepareCpu,
    IntelFreeCpu,
    IntelStart,
    IntelStop,
    IntelQueryOwnedPageAccess,
    IntelSetOwnedPageAccess
};

const HV_BACKEND_OPS*
HvIntelGetBackendOps(
    VOID
    )
{
    return &IntelBackendOps;
}
