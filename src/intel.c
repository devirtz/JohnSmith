#include "intel/intel_internal.h"

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
