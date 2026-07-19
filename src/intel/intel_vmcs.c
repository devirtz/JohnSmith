#include "intel_internal.h"

static ULONG64
IntelDescriptorBase(
    _In_ ULONG64 Descriptor,
    _In_opt_ const ULONG* HighBase
    )
{
    ULONG64 base = ((Descriptor >> 16) & 0xffffull) |
                   (((Descriptor >> 32) & 0xffull) << 16) |
                   (((Descriptor >> 56) & 0xffull) << 24);
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
    ULONG desiredPin;
    ULONG requiredPin;
    ULONG requiredSecondary;
    ULONG desiredPrimary;
    ULONG requiredPrimary;
    ULONG requiredExit;
    ULONG requiredEntry;
    ULONG maxBasicLeaf;
    ULONG64 cr0Fixed0;
    ULONG64 cr0Fixed1;
    ULONG64 cr4Fixed0;
    ULONG64 cr4Fixed1;
    ULONG64 hostRsp;
    ULONG64 eptp;
    ULONG64 primaryCapability;
    INTEL_HOST_STACK_FRAME* hostFrame;
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
    primaryCapability = __readmsr(primaryMsr);
    exitMsr = (backend->VmxBasic & (1ull << 55)) != 0
        ? IA32_VMX_TRUE_EXIT_CTLS : IA32_VMX_EXIT_CTLS;
    entryMsr = (backend->VmxBasic & (1ull << 55)) != 0
        ? IA32_VMX_TRUE_ENTRY_CTLS : IA32_VMX_ENTRY_CTLS;

    requiredPin = VMX_PIN_NMI_EXITING | VMX_PIN_VIRTUAL_NMIS;
    desiredPin = requiredPin;
    desiredSecondary = VMX_SECONDARY_ENABLE_EPT;
    requiredSecondary = VMX_SECONDARY_ENABLE_EPT;
    __cpuid(cpuid, 0x80000000);
    if ((ULONG)cpuid[0] >= 0x80000001u) {
        __cpuid(cpuid, 0x80000001);
        if ((((ULONG)cpuid[3]) & (1u << 27)) != 0) {
            desiredSecondary |= VMX_SECONDARY_ENABLE_RDTSCP;
        }
    }
    __cpuid(cpuid, 0);
    maxBasicLeaf = (ULONG)cpuid[0];
    context->CpuidLeaf0Eax = (ULONG)cpuid[0];
    context->CpuidLeaf0Ebx = (ULONG)cpuid[1];
    context->CpuidLeaf0Ecx = (ULONG)cpuid[2];
    context->CpuidLeaf0Edx = (ULONG)cpuid[3];

    /*
     * Hypercall clamped return.  The trigger is CPUID leaf 1 with a per-boot
     * subleaf; on the target CPU leaf 1 ignores ECX (verified: every subleaf
     * returns the identical leaf-1 block on bare metal).  The clamped return
     * therefore must reproduce the masked leaf-1 block, not CPUID(max_std,0),
     * or an anti-cheat comparing CPUID(1, magic_subleaf) to CPUID(1, 0) would
     * see differing EAX/EBX/ECX/EDX and detect the trap.  The mask clears
     * VMX (bit 5) and the hypervisor-present bit (bit 31), matching the
     * existing case 1 handler in intel_exit.c.
     */
    __cpuidex(cpuid, 1, 0);
    context->ClampedCpuidEax = (ULONG)cpuid[0];
    context->ClampedCpuidEbx = (ULONG)cpuid[1];
    context->ClampedCpuidEcx = (ULONG)cpuid[2] & 0x7FFFFFDFu;
    context->ClampedCpuidEdx = (ULONG)cpuid[3];

    context->Hypercall1GbPagesSupported = FALSE;
    __cpuid(cpuid, 0x80000000);
    if ((ULONG)cpuid[0] >= 0x80000001u) {
        __cpuidex(cpuid, 0x80000001, 0);
        if ((((ULONG)cpuid[3]) & (1u << 26)) != 0) {
            context->Hypercall1GbPagesSupported = TRUE;
        }
    }

    if (maxBasicLeaf >= 7) {
        __cpuidex(cpuid, 7, 0);
        if ((((ULONG)cpuid[1]) & (1u << 10)) != 0) {
            desiredSecondary |= VMX_SECONDARY_ENABLE_INVPCID;
        }
    }
    if (maxBasicLeaf >= 0xDu) {
        __cpuidex(cpuid, 0xD, 1);
        if ((((ULONG)cpuid[0]) & (1u << 3)) != 0) {
            desiredSecondary |= VMX_SECONDARY_ENABLE_XSAVES;
        }
    }
    if ((backend->EptVpidCapabilities & (1ull << 32)) != 0 &&
        (backend->EptVpidCapabilities & (1ull << 41)) != 0 &&
        Cpu->ProcessorIndex < MAXUSHORT) {
        desiredSecondary |= VMX_SECONDARY_ENABLE_VPID;
    }

    requiredPrimary = VMX_PRIMARY_ACTIVATE_SECONDARY |
                      VMX_PRIMARY_USE_MSR_BITMAPS |
                      VMX_PRIMARY_USE_TSC_OFFSETTING;
    desiredPrimary = requiredPrimary;
    pinControls = IntelAdjustControls(desiredPin, pinMsr);
    primaryControls = IntelAdjustControls(
        desiredPrimary, primaryMsr);
    secondaryControls = IntelAdjustControls(
        desiredSecondary, IA32_VMX_PROCBASED_CTLS2);
    requiredExit = VMX_EXIT_SAVE_DEBUG_CONTROLS |
                   VMX_EXIT_HOST_ADDRESS_SPACE_SIZE |
                   VMX_EXIT_SAVE_PAT | VMX_EXIT_LOAD_PAT |
                   VMX_EXIT_SAVE_EFER | VMX_EXIT_LOAD_EFER;
    requiredEntry = VMX_ENTRY_LOAD_DEBUG_CONTROLS |
                    VMX_ENTRY_IA32E_MODE |
                    VMX_ENTRY_LOAD_PAT | VMX_ENTRY_LOAD_EFER;
    exitControls = IntelAdjustControls(requiredExit, exitMsr);
    entryControls = IntelAdjustControls(requiredEntry, entryMsr);
    context->PinControls = pinControls;
    context->PrimaryControls = primaryControls;
    context->SecondaryControls = secondaryControls;
    context->ExitControls = exitControls;
    context->EntryControls = entryControls;
    context->DesiredPrimaryControls = desiredPrimary;
    context->DesiredSecondaryControls = desiredSecondary;
    context->RequiredPinControls = requiredPin;
    context->RequiredPrimaryControls = requiredPrimary;
    context->VirtualNmiEnabled =
        (pinControls & (VMX_PIN_NMI_EXITING | VMX_PIN_VIRTUAL_NMIS)) ==
        (VMX_PIN_NMI_EXITING | VMX_PIN_VIRTUAL_NMIS);
    context->RequiredSecondaryControls = requiredSecondary;
    context->RequiredExitControls = requiredExit;
    context->RequiredEntryControls = requiredEntry;
    context->CpuidClearLeaf7Ebx = (secondaryControls &
        VMX_SECONDARY_ENABLE_INVPCID) ? 0 : (1u << 10);
    context->CpuidClearLeafDEax = (secondaryControls &
        VMX_SECONDARY_ENABLE_XSAVES) ? 0 : (1u << 3);
    context->CpuidClearLeaf80000001Edx = (secondaryControls &
        VMX_SECONDARY_ENABLE_RDTSCP) ? 0 : (1u << 27);
    context->ControlFailureMask = 0;
    if ((pinControls & requiredPin) != requiredPin) {
        context->ControlFailureMask |= INTEL_CONTROL_FAIL_PIN_REQUIRED;
    }
    if ((primaryControls & requiredPrimary) != requiredPrimary) {
        context->ControlFailureMask |= INTEL_CONTROL_FAIL_PRIMARY_REQUIRED;
    }
    if (((ULONG)(primaryCapability >> 32) &
         INTEL_POLICY_REQUIRED_DYNAMIC_PRIMARY_CONTROLS) !=
        INTEL_POLICY_REQUIRED_DYNAMIC_PRIMARY_CONTROLS) {
        context->ControlFailureMask |= INTEL_CONTROL_FAIL_NMI_WINDOW;
    }
    if ((primaryControls & VMX_PRIMARY_ACTIVATE_SECONDARY) == 0) {
        context->ControlFailureMask |= INTEL_CONTROL_FAIL_SECONDARY_ACTIVATION;
    }
    if ((primaryControls & VMX_PRIMARY_USE_MSR_BITMAPS) == 0) {
        context->ControlFailureMask |= INTEL_CONTROL_FAIL_BITMAPS;
    }
    if ((secondaryControls & requiredSecondary) != requiredSecondary) {
        context->ControlFailureMask |= INTEL_CONTROL_FAIL_SECONDARY_REQUIRED;
    }
    if ((exitControls & requiredExit) != requiredExit) {
        context->ControlFailureMask |= INTEL_CONTROL_FAIL_EXIT_REQUIRED;
    }
    if ((entryControls & requiredEntry) != requiredEntry) {
        context->ControlFailureMask |= INTEL_CONTROL_FAIL_ENTRY_REQUIRED;
    }
    if (context->ControlFailureMask != 0) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }

    eptp = context->PrimaryEpt.EptPointer;
    context->EptPointer = eptp;
    context->Vpid = (secondaryControls & VMX_SECONDARY_ENABLE_VPID) != 0
        ? (USHORT)(Cpu->ProcessorIndex + 1) : 0;
    cr0Fixed0 = __readmsr(IA32_VMX_CR0_FIXED0);
    cr0Fixed1 = __readmsr(IA32_VMX_CR0_FIXED1);
    cr4Fixed0 = __readmsr(IA32_VMX_CR4_FIXED0);
    cr4Fixed1 = __readmsr(IA32_VMX_CR4_FIXED1);
    hostRsp = ((ULONG64)context->HostStack + INTEL_HOST_STACK_SIZE - 64) &
              ~0xfull;
    hostFrame = (INTEL_HOST_STACK_FRAME*)hostRsp;
    RtlZeroMemory(hostFrame, sizeof(*hostFrame));
    hostFrame->Cpu = Cpu;
    hostFrame->CpuSlatGeneration = &context->SlatGeneration;
    hostFrame->BackendSlatGeneration = &backend->SlatGeneration;
    hostFrame->RendezvousPhase = &backend->Rendezvous.Phase;
    hostFrame->CpuidLeaf0Eax = context->CpuidLeaf0Eax;
    hostFrame->CpuidLeaf0Ebx = context->CpuidLeaf0Ebx;
    hostFrame->CpuidLeaf0Ecx = context->CpuidLeaf0Ecx;
    hostFrame->CpuidLeaf0Edx = context->CpuidLeaf0Edx;
#if JOHNSMITH_DIAGNOSTICS
    hostFrame->FastPathEnabled = 0;
#else
    hostFrame->FastPathEnabled = 1;
#endif

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
    VMX_WRITE(VMCS_MSR_BITMAP, context->MsrBitmapPhysical.QuadPart);
    VMX_WRITE(VMCS_TSC_OFFSET, context->TscOffset);
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
    /*
     * A mask bit set to 1 is host-owned.  Own only values that VMX forces
     * to one or zero, plus VMXE which must remain hidden from the guest.
     */
    VMX_WRITE(VMCS_CR0_GUEST_HOST_MASK, cr0Fixed0 | ~cr0Fixed1);
    VMX_WRITE(VMCS_CR4_GUEST_HOST_MASK,
        cr4Fixed0 | ~cr4Fixed1 | VMX_CR4_VMXE);
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
