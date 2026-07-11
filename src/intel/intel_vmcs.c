#include "intel_internal.h"

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

NTSTATUS
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

NTSTATUS
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

