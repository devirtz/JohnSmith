#include "amd/amd_internal.h"
#include "hv_log.h"

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
    if (Read) {
        Msrpm[offset + bit / 8] |= (UCHAR)(1u << (bit & 7));
    }
    ++bit;
    if (Write) {
        Msrpm[offset + bit / 8] |= (UCHAR)(1u << (bit & 7));
    }
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

static BOOLEAN
AmdFindPatCacheFlags(
    _In_ ULONG64 Pat,
    _In_ UCHAR MemoryType,
    _Out_ ULONG64* CacheFlags
    )
{
    ULONG index;

    for (index = 0; index < 4; ++index) {
        if (((Pat >> (index * 8)) & 0xff) == MemoryType) {
            *CacheFlags = ((index & 1) != 0 ? NPT_PWT : 0) |
                          ((index & 2) != 0 ? NPT_PCD : 0);
            return TRUE;
        }
    }
    return FALSE;
}

static NTSTATUS
AmdSupport(
    VOID
    )
{
    int registers[4];
    ULONG64 cacheFlags;
    ULONG64 pat;

    if ((__readcr4() & HV_CR4_CET) != 0) {
        HV_LOG_ERROR(
            "AMD CET is enabled (CR4=0x%016llX); SVM CET/SSP state "
            "virtualization is not implemented.\n",
            __readcr4());
        return STATUS_NOT_SUPPORTED;
    }

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
    if ((ULONG)registers[0] == 0 || (ULONG)registers[1] < 2 ||
        (((ULONG)registers[3]) &
         (AMD_SVM_FEATURE_NPT | AMD_SVM_FEATURE_NRIPS)) !=
        (AMD_SVM_FEATURE_NPT | AMD_SVM_FEATURE_NRIPS)) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }
    pat = __readmsr(AMD_MSR_PAT);
    if (!AmdFindPatCacheFlags(pat, 6, &cacheFlags) ||
        !AmdFindPatCacheFlags(pat, 0, &cacheFlags)) {
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
    if (context == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(context, sizeof(*context));
    InitializeListHead(&context->SplitList);
    if (!AmdFindPatCacheFlags(
            __readmsr(AMD_MSR_PAT), 6, &context->RamCacheFlags) ||
        !AmdFindPatCacheFlags(
            __readmsr(AMD_MSR_PAT), 0, &context->MmioCacheFlags)) {
        ExFreePoolWithTag(context, HV_POOL_TAG_BACKEND);
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }
    __cpuid(registers, 0x8000000A);
    context->AsidCount = (ULONG)registers[1];
    context->TlbFlushCommand =
        (((ULONG)registers[3] & AMD_SVM_FEATURE_FLUSH_BY_ASID) != 0)
        ? 3 : 1;
    context->SlatGeneration = 1;
    State->BackendContext = context;
    status = AmdBuildNpt(context);
    if (!NT_SUCCESS(status)) {
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
    if (State == NULL || State->BackendContext == NULL) {
        return;
    }
    context = (AMD_BACKEND_CONTEXT*)State->BackendContext;
    AmdFreeNpt(context);
    ExFreePoolWithTag(context, HV_POOL_TAG_BACKEND);
    State->BackendContext = NULL;
}

static VOID
AmdDestroyCpuContext(
    _In_opt_ AMD_CPU_CONTEXT* Context
    )
{
    if (Context == NULL) {
        return;
    }
    if (Context->HostStack != NULL) {
        RtlSecureZeroMemory(Context->HostStack, AMD_HOST_STACK_SIZE);
        ExFreePoolWithTag(Context->HostStack, HV_POOL_TAG_BACKEND);
    }
    if (Context->Msrpm != NULL) {
        MmFreeContiguousMemory(Context->Msrpm);
    }
    if (Context->HostSave != NULL) {
        MmFreeContiguousMemory(Context->HostSave);
    }
    if (Context->HostVmcb != NULL) {
        MmFreeContiguousMemory(Context->HostVmcb);
    }
    if (Context->Vmcb != NULL) {
        MmFreeContiguousMemory(Context->Vmcb);
    }
    RtlSecureZeroMemory(Context, sizeof(*Context));
    ExFreePoolWithTag(Context, HV_POOL_TAG_BACKEND);
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
    if (context == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
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
        AmdDestroyCpuContext(context);
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
    if (Cpu == NULL || Cpu->VendorContext == NULL) {
        return;
    }
    context = (AMD_CPU_CONTEXT*)Cpu->VendorContext;
    AmdDestroyCpuContext(context);
    Cpu->VendorContext = NULL;
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

_IRQL_requires_(PASSIVE_LEVEL)
static VOID
AmdQuiesce(
    _Inout_ HV_STATE* State
    )
{
    UNREFERENCED_PARAMETER(State);
}

static VOID
AmdReportStartFailure(
    _In_ HV_STATE* State,
    _In_ const HV_CPU* Cpu
    )
{
    const AMD_CPU_CONTEXT* context;

    UNREFERENCED_PARAMETER(State);
    if (Cpu == NULL || Cpu->VendorContext == NULL) {
        return;
    }
    context = (const AMD_CPU_CONTEXT*)Cpu->VendorContext;
    HV_LOG_ERROR(
        "CPU %lu SVM start failed: NTSTATUS 0x%08X, EXITCODE "
        "0x%016llX.\n",
        Cpu->ProcessorIndex,
        (ULONG)Cpu->Status,
        context->Vmcb != NULL ? context->Vmcb->Control.ExitCode : 0);
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
    if ((__readcr4() & HV_CR4_CET) != 0) {
        return STATUS_NOT_SUPPORTED;
    }
    if ((__readmsr(AMD_MSR_VM_CR) & AMD_VM_CR_SVMDIS) != 0) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
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
    if (!context->Virtualized) {
        return STATUS_SUCCESS;
    }
    AmdAsmStop(context->StopCookie);
    if (context->Virtualized) {
        return STATUS_HV_OPERATION_FAILED;
    }
    __writemsr(AMD_MSR_PAT, context->Vmcb->State.GPat);
    __writemsr(AMD_MSR_VM_HSAVE_PA, context->OriginalHostSavePhysical);
    __writemsr(AMD_MSR_EFER, context->GuestEfer);
    return STATUS_SUCCESS;
}

static const HV_BACKEND_OPS AmdBackendOps = {
    "AMD SVM/NPT",
    AmdSupport,
    AmdPrepare,
    AmdFree,
    AmdPrepareCpu,
    AmdFreeCpu,
    AmdQuiesce,
    AmdStart,
    AmdStop,
    AmdReportStartFailure,
    AmdQueryOwnedPageAccess,
    AmdSetOwnedPageAccess,
    NULL,
    NULL,
    NULL
};

const HV_BACKEND_OPS*
HvAmdGetBackendOps(
    VOID
    )
{
    return &AmdBackendOps;
}
