#include "intel/intel_internal.h"
#include "hv_log.h"
#include "hook_observe.h"

static NTSTATUS
IntelValidateSupervisorCet(
    _Out_opt_ PULONG64 SupervisorCet
    )
{
    int registers[4];
    ULONG64 supervisorCet = 0;

    if (SupervisorCet != NULL) {
        *SupervisorCet = 0;
    }
    if ((__readcr4() & HV_CR4_CET) == 0) {
        return STATUS_SUCCESS;
    }

    __cpuid(registers, 0);
    if ((ULONG)registers[0] < 7) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }
    __cpuidex(registers, 7, 0);
    if ((((ULONG)registers[2] & CPUID_CET_SS) == 0) &&
        (((ULONG)registers[3] & CPUID_CET_IBT) == 0)) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }

    supervisorCet = __readmsr(IA32_S_CET);
    if (SupervisorCet != NULL) {
        *SupervisorCet = supervisorCet;
    }
    return supervisorCet == 0 ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

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

    if (Read) {
        Bitmap[readBase + index / 8] |= (UCHAR)(1u << (index & 7));
    }
    if (Write) {
        Bitmap[writeBase + index / 8] |= (UCHAR)(1u << (index & 7));
    }
}

static VOID
IntelInitializeMsrBitmap(
    _Inout_updates_bytes_(PAGE_SIZE) UCHAR* Bitmap
    )
{
    ULONG msr;

    RtlZeroMemory(Bitmap, PAGE_SIZE);
    IntelSetMsrBitmapBit(Bitmap, IA32_FEATURE_CONTROL, TRUE, TRUE);
    IntelSetMsrBitmapBit(Bitmap, IA32_S_CET, FALSE, TRUE);
    for (msr = IA32_VMX_BASIC; msr <= IA32_VMX_VMFUNC; ++msr) {
        IntelSetMsrBitmapBit(Bitmap, msr, TRUE, TRUE);
    }
}

static NTSTATUS
IntelSupport(
    VOID
    )
{
    int registers[4];
    ULONG64 supervisorCet;
    ULONG64 featureControl;
    ULONG64 basic;
    ULONG64 eptCapabilities;
    NTSTATUS status;

    status = IntelValidateSupervisorCet(&supervisorCet);
    if (!NT_SUCCESS(status)) {
        HV_LOG_ERROR(
            "Intel supervisor CET is active or inconsistent "
            "(CR4=0x%016llX, IA32_S_CET=0x%016llX); root CET/SSP "
            "virtualization is not implemented.\n",
            __readcr4(),
            supervisorCet);
        return status;
    }
    // if ((__readcr4() & HV_CR4_CET) != 0) {
    //     HV_LOG_INFO(
    //         "Intel CET facility is enabled with IA32_S_CET=0; "
    //         "supervisor CET is inactive, continuing.\n");
    // }

    __cpuid(registers, 1);
    if ((((ULONG)registers[2]) & (1u << 5)) == 0) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }

    featureControl = __readmsr(IA32_FEATURE_CONTROL);
    if ((featureControl & (1ull | (1ull << 2))) !=
        (1ull | (1ull << 2))) {
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
        POOL_FLAG_NON_PAGED | POOL_FLAG_CACHE_ALIGNED, sizeof(*context), HV_POOL_TAG_BACKEND);
    if (context == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(context, sizeof(*context));
    context->VmxBasic = __readmsr(IA32_VMX_BASIC);
    context->EptVpidCapabilities = __readmsr(IA32_VMX_EPT_VPID_CAP);
    context->SlatGeneration = 1;

    status = IntelHypercallPrepare();
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(context, HV_POOL_TAG_BACKEND);
        return status;
    }

    HookThunkInitialize();
    IntelHookResetTable();

    State->BackendContext = context;

    status = IntelRendezvousPrepare(context, State);
    if (NT_SUCCESS(status)) {
        status = IntelBuildEpt(context);
    }
    if (NT_SUCCESS(status)) {
        status = IntelHookEnsureSecondaryRoot(context);
    }
    if (!NT_SUCCESS(status)) {
        IntelFreeEpt(context);
        IntelRendezvousFree(context);
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
    IntelHookTeardown();
    ObserveHookReset();
    IntelFreeEpt(context);
    IntelRendezvousFree(context);
    ExFreePoolWithTag(context, HV_POOL_TAG_BACKEND);
    State->BackendContext = NULL;
}

static VOID
IntelDestroyCpuContext(
    _In_opt_ INTEL_CPU_CONTEXT* Context
    )
{
    if (Context == NULL) {
        return;
    }
    IntelHypercallReleasePage(Context);
    IntelFreeCpuEptViews(Context);
    if (Context->HostStack != NULL) {
        RtlSecureZeroMemory(Context->HostStack, INTEL_HOST_STACK_SIZE);
        ExFreePoolWithTag(Context->HostStack, HV_POOL_TAG_BACKEND);
    }
    if (Context->MsrBitmap != NULL) {
        MmFreeContiguousMemory(Context->MsrBitmap);
    }
    if (Context->Vmcs != NULL) {
        MmFreeContiguousMemory(Context->Vmcs);
    }
    if (Context->Vmxon != NULL) {
        MmFreeContiguousMemory(Context->Vmxon);
    }
    RtlSecureZeroMemory(Context, sizeof(*Context));
    ExFreePoolWithTag(Context, HV_POOL_TAG_BACKEND);
}

static NTSTATUS
IntelPrepareCpu(
    _Inout_ HV_STATE* State,
    _Inout_ HV_CPU* Cpu
    )
{
    INTEL_BACKEND_CONTEXT* backend;
    INTEL_CPU_CONTEXT* context;
    NTSTATUS status;

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
    ExInitializeRundownProtection(&context->HypercallPageRundown);
    context->BackendContext = backend;
    context->ProcessorIndex = Cpu->ProcessorIndex;
    context->TscOffset = 0;

    context->Vmxon = IntelAllocatePage(MAXULONG);
    context->Vmcs = IntelAllocatePage(MAXULONG);
    context->HostStack = ExAllocatePool2(
        POOL_FLAG_NON_PAGED, INTEL_HOST_STACK_SIZE, HV_POOL_TAG_BACKEND);
    context->MsrBitmap = IntelAllocatePage(MAXULONG);
    if (context->Vmxon == NULL || context->Vmcs == NULL ||
        context->HostStack == NULL || context->MsrBitmap == NULL) {
        IntelDestroyCpuContext(context);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    context->VmxonPhysical = MmGetPhysicalAddress(context->Vmxon);
    context->VmcsPhysical = MmGetPhysicalAddress(context->Vmcs);
    context->MsrBitmapPhysical = MmGetPhysicalAddress(context->MsrBitmap);
    context->SlatGeneration = backend->SlatGeneration;
    context->StopCookie = __rdtsc() ^ (ULONG64)context ^
                          (ULONG64)context->VmcsPhysical.QuadPart;
    IntelInitializeMsrBitmap((UCHAR*)context->MsrBitmap);
    *(ULONG*)context->Vmxon = (ULONG)(backend->VmxBasic & 0x7fffffffu);
    *(ULONG*)context->Vmcs = (ULONG)(backend->VmxBasic & 0x7fffffffu);
    status = IntelInitializeCpuEptViews(context, backend);
    if (!NT_SUCCESS(status)) {
        IntelDestroyCpuContext(context);
        return status;
    }
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
    IntelDestroyCpuContext(context);
    Cpu->VendorContext = NULL;
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

static PCSTR
IntelStartStageName(
    _In_ ULONG Stage
    )
{
    switch (Stage) {
    case INTEL_START_STAGE_CET: return "CET validation";
    case INTEL_START_STAGE_FEATURE_CONTROL: return "feature control";
    case INTEL_START_STAGE_VMXON: return "VMXON";
    case INTEL_START_STAGE_VMCLEAR: return "VMCLEAR";
    case INTEL_START_STAGE_VMPTRLD: return "VMPTRLD";
    case INTEL_START_STAGE_VMCS_SETUP: return "VMCS setup";
    case INTEL_START_STAGE_INVEPT: return "INVEPT";
    case INTEL_START_STAGE_INVVPID: return "INVVPID";
    case INTEL_START_STAGE_VMLAUNCH: return "VMLAUNCH";
    default: return "preflight";
    }
}

static VOID
IntelReportStartFailure(
    _In_ HV_STATE* State,
    _In_ const HV_CPU* Cpu
    )
{
    const INTEL_CPU_CONTEXT* context;

    UNREFERENCED_PARAMETER(State);
    if (Cpu == NULL || Cpu->VendorContext == NULL) {
        return;
    }
    context = (const INTEL_CPU_CONTEXT*)Cpu->VendorContext;
    HV_LOG_ERROR(
        "CPU %lu start failed at %s (stage %lu): NTSTATUS 0x%08X, "
        "VM-instruction error %lu, VPID %u.\n",
        Cpu->ProcessorIndex,
        IntelStartStageName(context->StartFailureStage),
        context->StartFailureStage,
        (ULONG)Cpu->Status,
        context->LastVmxError,
        context->Vpid);
    if (context->StartFailureStage == INTEL_START_STAGE_VMCS_SETUP &&
        context->ControlFailureMask != 0) {
        HV_LOG_ERROR(
            "CPU %lu VMCS control mismatch 0x%02X: "
            "pin=0x%08X/required=0x%08X, "
            "primary=0x%08X/desired=0x%08X/required=0x%08X, "
            "secondary=0x%08X/desired=0x%08X/required=0x%08X, "
            "exit=0x%08X/required=0x%08X, "
            "entry=0x%08X/required=0x%08X.\n",
            Cpu->ProcessorIndex,
            context->ControlFailureMask,
            context->PinControls,
            context->RequiredPinControls,
            context->PrimaryControls,
            context->DesiredPrimaryControls,
            context->RequiredPrimaryControls,
            context->SecondaryControls,
            context->DesiredSecondaryControls,
            context->RequiredSecondaryControls,
            context->ExitControls,
            context->RequiredExitControls,
            context->EntryControls,
            context->RequiredEntryControls);
    }
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
    context->LastVmxError = 0;
    context->StartFailureStage = INTEL_START_STAGE_CET;
    status = IntelValidateSupervisorCet(NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    context->OriginalCr0 = __readcr0();
    context->OriginalCr4 = __readcr4();

    context->StartFailureStage = INTEL_START_STAGE_FEATURE_CONTROL;
    featureControl = __readmsr(IA32_FEATURE_CONTROL);
    if ((featureControl & (1ull | (1ull << 2))) !=
        (1ull | (1ull << 2))) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }

    cr0 = (context->OriginalCr0 | __readmsr(IA32_VMX_CR0_FIXED0)) &
          __readmsr(IA32_VMX_CR0_FIXED1);
    cr4 = (context->OriginalCr4 | __readmsr(IA32_VMX_CR4_FIXED0) |
           VMX_CR4_VMXE) & __readmsr(IA32_VMX_CR4_FIXED1);
    __writecr0(cr0);
    __writecr4(cr4);

    context->StartFailureStage = INTEL_START_STAGE_VMXON;
    vmxonPhysical = (ULONG64)context->VmxonPhysical.QuadPart;
    if (__vmx_on(&vmxonPhysical) != 0) {
        status = STATUS_HV_FEATURE_UNAVAILABLE;
        goto RestoreRegisters;
    }
    context->VmxOn = TRUE;

    vmcsPhysical = (ULONG64)context->VmcsPhysical.QuadPart;
    context->StartFailureStage = INTEL_START_STAGE_VMCLEAR;
    if (__vmx_vmclear(&vmcsPhysical) != 0) {
        status = STATUS_HV_INVALID_VP_STATE;
        goto LeaveVmx;
    }
    context->StartFailureStage = INTEL_START_STAGE_VMPTRLD;
    if (__vmx_vmptrld(&vmcsPhysical) != 0) {
        status = STATUS_HV_INVALID_VP_STATE;
        goto LeaveVmx;
    }
    context->StartFailureStage = INTEL_START_STAGE_VMCS_SETUP;
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
        if (type == INVEPT_ALL_CONTEXTS) {
            descriptor.Context = 0;
        }
        context->StartFailureStage = INTEL_START_STAGE_INVEPT;
        if (IntelAsmInvept(type, &descriptor) != 0) {
            status = STATUS_HV_OPERATION_FAILED;
            goto LeaveVmx;
        }
        if (context->Vpid != 0) {
            descriptor.Context = context->Vpid;
            context->StartFailureStage = INTEL_START_STAGE_INVVPID;
            if (IntelAsmInvvpid(
                    INVVPID_SINGLE_CONTEXT, &descriptor) != 0) {
                status = STATUS_HV_OPERATION_FAILED;
                goto LeaveVmx;
            }
        }
    }

    context->StartFailureStage = INTEL_START_STAGE_VMLAUNCH;
    context->Launched = TRUE;
    if (IntelAsmLaunch() == 0) {
        context->StartFailureStage = INTEL_START_STAGE_NONE;
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
    INTEL_DESCRIPTOR_TABLE_REGISTER gdtr;
    INTEL_DESCRIPTOR_TABLE_REGISTER idtr;

    UNREFERENCED_PARAMETER(State);
    if (!IntelCurrentCpuMatches(Cpu) || Cpu->VendorContext == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    context = (INTEL_CPU_CONTEXT*)Cpu->VendorContext;
    if (InterlockedCompareExchange64(
            &context->RendezvousOwnedEpoch, 0, 0) != 0) {
        KeBugCheckEx(
            HYPERVISOR_ERROR,
            INTEL_BUGCHECK_RENDEZVOUS,
            Cpu->ProcessorIndex,
            context->RendezvousOwnedEpoch,
            INTEL_VMEXIT_STOP);
    }
    if (!context->Launched) {
        return STATUS_SUCCESS;
    }

    IntelAsmStop(context->StopCookie);
    if (context->Launched || context->VmxOn) {
        return STATUS_HV_OPERATION_FAILED;
    }
    gdtr.Limit = (USHORT)context->ResumeGdtrLimit;
    gdtr.Base = context->ResumeGdtrBase;
    idtr.Limit = (USHORT)context->ResumeIdtrLimit;
    idtr.Base = context->ResumeIdtrBase;
    IntelAsmLoadGdtr(&gdtr);
    IntelAsmLoadIdtr(&idtr);
    __writemsr(IA32_FS_BASE, context->ResumeFsBase);
    __writemsr(IA32_GS_BASE, context->ResumeGsBase);
    __writemsr(IA32_PAT, context->ResumePat);
    __writemsr(IA32_EFER, context->ResumeEfer);
    __writemsr(IA32_SYSENTER_CS, context->ResumeSysenterCs);
    __writemsr(IA32_SYSENTER_ESP, context->ResumeSysenterEsp);
    __writemsr(IA32_SYSENTER_EIP, context->ResumeSysenterEip);
    __writedr(7, context->ResumeDr7);
    __writecr3(context->ResumeCr3);
    __writecr4(context->ResumeCr4);
    __writecr0(context->ResumeCr0);
    return STATUS_SUCCESS;
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
    IntelReportStartFailure,
    IntelQueryOwnedPageAccess,
    IntelSetOwnedPageAccess,
    IntelHookInstall,
    IntelHookRemove,
    IntelHookQuery
};

const HV_BACKEND_OPS*
HvIntelGetBackendOps(
    VOID
    )
{
    return &IntelBackendOps;
}
