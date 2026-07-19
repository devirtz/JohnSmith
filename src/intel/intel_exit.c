#include "intel_internal.h"
#include "../common/x86_common.h"

#define VMX_EVENT_INFORMATION_MASK 0x80000FFFu

VOID
IntelDecodeEptViolation(
    _In_ ULONG64 Qualification,
    _In_ ULONG64 GuestPhysicalAddress,
    _In_ ULONG64 GuestLinearAddress,
    _Out_ INTEL_EPT_VIOLATION* Decoded
    )
{

    RtlZeroMemory(Decoded, sizeof(*Decoded));
    Decoded->Qualification = Qualification;
    Decoded->GuestPhysicalAddress = GuestPhysicalAddress;
    Decoded->LinearValid =
        (Qualification & VMX_EPT_QUAL_GLA_VALID) != 0;
    if (Decoded->LinearValid) {
        Decoded->GuestLinearAddress = GuestLinearAddress;
    }
    Decoded->Translation =
        (Qualification & VMX_EPT_QUAL_TRANSLATION) != 0;
    Decoded->Read = (Qualification & VMX_EPT_QUAL_READ) != 0;
    Decoded->Write = (Qualification & VMX_EPT_QUAL_WRITE) != 0;
    Decoded->Execute = (Qualification & VMX_EPT_QUAL_EXECUTE) != 0;
    Decoded->EptReadable =
        (Qualification & VMX_EPT_QUAL_EPT_READABLE) != 0;
    Decoded->EptWritable =
        (Qualification & VMX_EPT_QUAL_EPT_WRITABLE) != 0;
    Decoded->EptExecutable =
        (Qualification & VMX_EPT_QUAL_EPT_EXECUTABLE) != 0;
}

DECLSPEC_NORETURN
VOID
IntelFailEptViolation(
    _In_ const HV_CPU* Cpu,
    _In_ const INTEL_EPT_VIOLATION* Decoded,
    _In_ ULONG64 GuestRip
    )
{

    ULONG64 packed = ((ULONG64)Cpu->ProcessorIndex & 0xffull) |
                     ((ULONG64)(Decoded->LinearValid ? 1u : 0u) << 8) |
                     ((ULONG64)(Decoded->Translation ? 1u : 0u) << 9) |
                     ((ULONG64)(Decoded->Read ? 1u : 0u) << 10) |
                     ((ULONG64)(Decoded->Write ? 1u : 0u) << 11) |
                     ((ULONG64)(Decoded->Execute ? 1u : 0u) << 12) |
                     ((ULONG64)(Decoded->EptReadable ? 1u : 0u) << 13) |
                     ((ULONG64)(Decoded->EptWritable ? 1u : 0u) << 14) |
                     ((ULONG64)(Decoded->EptExecutable ? 1u : 0u) << 15);
    KeBugCheckEx(
        HYPERVISOR_ERROR,
        INTEL_BUGCHECK_EPT_VIOLATION,
        Decoded->Qualification,
        Decoded->GuestPhysicalAddress,
        (ULONG_PTR)(packed ^ (GuestRip & ~0xffffull)));
}

DECLSPEC_NORETURN
VOID
IntelFailEptMisconfiguration(
    _In_ const HV_CPU* Cpu,
    _In_ ULONG64 GuestPhysicalAddress,
    _In_ ULONG64 GuestRip
    )
{

    KeBugCheckEx(
        HYPERVISOR_ERROR,
        INTEL_BUGCHECK_EPT_MISCONFIG,
        (ULONG_PTR)Cpu->ProcessorIndex,
        GuestPhysicalAddress,
        GuestRip);
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

static NTSTATUS
IntelGetAdvancedGuestRip(
    _In_ const INTEL_CPU_CONTEXT* Context,
    _In_ ULONG64 GuestRip,
    _In_ ULONG InstructionLength,
    _Out_ PULONG64 AdvancedGuestRip
    )
{
    ULONG64 csAccessRights;
    ULONG64 nextRip;

    if (!IntelVmReadValue(VMCS_GUEST_CS_AR, &csAccessRights)) {
        return STATUS_HV_INVALID_VP_STATE;
    }

    nextRip = GuestRip + InstructionLength;
    if ((Context->EntryControls & VMX_ENTRY_IA32E_MODE) != 0 &&
        (csAccessRights & (1ull << 13)) != 0) {
        /* 64-bit code advances the full RIP. */
    } else if ((csAccessRights & (1ull << 14)) != 0) {
        nextRip = (ULONG)nextRip;
    } else {
        nextRip = (GuestRip & ~0xffffull) | (USHORT)nextRip;
    }
    *AdvancedGuestRip = nextRip;
    return STATUS_SUCCESS;
}

static VOID
IntelAdvanceGuestRip(
    _In_ const INTEL_CPU_CONTEXT* Context,
    _In_ const HV_CPU* Cpu,
    _In_ ULONG Reason,
    _In_ ULONG64 GuestRip,
    _In_ ULONG InstructionLength
    )
{
    ULONG64 nextRip;
    ULONG64 interruptibility;
    ULONG64 rflags;
    ULONG64 debugCtl;
    ULONG64 pendingDebug;
    NTSTATUS status;

    status = IntelGetAdvancedGuestRip(
        Context, GuestRip, InstructionLength, &nextRip);
    if (!NT_SUCCESS(status) ||
        !IntelVmReadValue(VMCS_GUEST_INTERRUPTIBILITY, &interruptibility) ||
        !IntelVmReadValue(VMCS_GUEST_RFLAGS, &rflags)) {
        status = STATUS_HV_INVALID_VP_STATE;
        goto Fail;
    }

    if ((interruptibility &
         (VMX_GUEST_BLOCKING_BY_STI | VMX_GUEST_BLOCKING_BY_MOV_SS)) != 0) {
        interruptibility &=
            ~(VMX_GUEST_BLOCKING_BY_STI | VMX_GUEST_BLOCKING_BY_MOV_SS);
        status = IntelVmWrite(
            VMCS_GUEST_INTERRUPTIBILITY, interruptibility);
        if (!NT_SUCCESS(status)) goto Fail;
    }

    if ((rflags & RFLAGS_TF) != 0) {
        if (!IntelVmReadValue(VMCS_GUEST_DEBUGCTL, &debugCtl)) {
            status = STATUS_HV_INVALID_VP_STATE;
            goto Fail;
        }
        if ((debugCtl & IA32_DEBUGCTL_BTF) == 0) {
            if (!IntelVmReadValue(
                    VMCS_GUEST_PENDING_DEBUG, &pendingDebug)) {
                status = STATUS_HV_INVALID_VP_STATE;
                goto Fail;
            }
            status = IntelVmWrite(
                VMCS_GUEST_PENDING_DEBUG,
                pendingDebug | VMX_PENDING_DEBUG_BS);
            if (!NT_SUCCESS(status)) goto Fail;
        }
    }

    status = IntelVmWrite(VMCS_GUEST_RIP, nextRip);
    if (NT_SUCCESS(status)) return;

Fail:
    KeBugCheckEx(
        HYPERVISOR_ERROR,
        INTEL_BUGCHECK_UNEXPECTED_EXIT,
        Cpu->ProcessorIndex,
        ((ULONG64)Reason << 32) | 10,
        GuestRip);
}

static NTSTATUS
IntelRestoreVectoringEvent(
    _In_ ULONG ExitInstructionLength,
    _Out_ PULONG PendingInformation
    )
{
    ULONG64 information;
    ULONG type;
    NTSTATUS status;

    *PendingInformation = 0;
    /* SDM 27.8.3: every VM exit clears the injected-event valid bit. */
    if (!IntelVmReadValue(VMCS_IDT_VECTORING_INFO, &information)) {
        return STATUS_HV_INVALID_VP_STATE;
    }
    if ((information & (1ull << 31)) == 0) {
        return STATUS_SUCCESS;
    }
    type = (ULONG)((information >> 8) & 7);
    information &= VMX_EVENT_INFORMATION_MASK;
    status = IntelVmWrite(VMCS_ENTRY_INTERRUPTION_INFO, information);
    if (!NT_SUCCESS(status)) return status;
    if ((information & (1ull << 11)) != 0) {
        ULONG64 errorCode;
        if (!IntelVmReadValue(VMCS_IDT_VECTORING_ERROR, &errorCode)) {
            return STATUS_HV_INVALID_VP_STATE;
        }
        status = IntelVmWrite(VMCS_ENTRY_EXCEPTION_ERROR, errorCode);
        if (!NT_SUCCESS(status)) return status;
    }
    if (type == 4 || type == 5 || type == 6) {
        status = IntelVmWrite(
            VMCS_ENTRY_INSTRUCTION_LENGTH, ExitInstructionLength);
        if (!NT_SUCCESS(status)) return status;
    }
    *PendingInformation = (ULONG)information;
    return STATUS_SUCCESS;
}

DECLSPEC_NORETURN
static VOID
IntelFailEventCollision(
    _In_ const HV_CPU* Cpu,
    _In_ ULONG Reason,
    _In_ ULONG PendingInformation,
    _In_ ULONG NewInformation,
    _In_ ULONG64 GuestRip
    )
{
    ULONG64 location = ((ULONG64)Cpu->ProcessorIndex << 32) | Reason;
    ULONG64 events = ((ULONG64)PendingInformation << 32) | NewInformation;

    KeBugCheckEx(
        HYPERVISOR_ERROR,
        INTEL_BUGCHECK_EVENT_COLLISION,
        location,
        events,
        GuestRip);
}

typedef enum _INTEL_EVENT_CLASS {
    INTEL_EVENT_CLASS_BENIGN = 0,
    INTEL_EVENT_CLASS_CONTRIBUTORY = 1,
    INTEL_EVENT_CLASS_PAGE_FAULT = 2
} INTEL_EVENT_CLASS;

static INTEL_EVENT_CLASS
IntelClassifyEvent(
    _In_ ULONG Information
    )
{
    ULONG type = (Information >> 8) & 7u;
    ULONG vector = Information & 0xffu;

    /* Only hardware exceptions (type 3) count toward double-fault pairs. */
    if (type != 3) {
        return INTEL_EVENT_CLASS_BENIGN;
    }
    switch (vector) {
    case 0:   /* #DE */
    case 10:  /* #TS */
    case 11:  /* #NP */
    case 12:  /* #SS */
    case 13:  /* #GP */
    case 21:  /* #CP */
        return INTEL_EVENT_CLASS_CONTRIBUTORY;
    case 14:  /* #PF */
    case 20:  /* #VE */
        return INTEL_EVENT_CLASS_PAGE_FAULT;
    default:
        return INTEL_EVENT_CLASS_BENIGN;
    }
}

static BOOLEAN
IntelPairGeneratesDoubleFault(
    _In_ INTEL_EVENT_CLASS Pending,
    _In_ INTEL_EVENT_CLASS New
    )
{

    if (Pending == INTEL_EVENT_CLASS_CONTRIBUTORY &&
        New == INTEL_EVENT_CLASS_CONTRIBUTORY) {
        return TRUE;
    }
    if (Pending == INTEL_EVENT_CLASS_PAGE_FAULT &&
        (New == INTEL_EVENT_CLASS_CONTRIBUTORY ||
         New == INTEL_EVENT_CLASS_PAGE_FAULT)) {
        return TRUE;
    }
    return FALSE;
}

static VOID
IntelSetPrimaryControl(
    _Inout_ INTEL_CPU_CONTEXT* Context,
    _In_ ULONG Bit,
    _In_ BOOLEAN Enable
    )
{
    ULONG controls = Context->PrimaryControls;
    ULONG updated = Enable ? (controls | Bit) : (controls & ~Bit);

    if (updated == controls) {
        return;
    }
    if (!NT_SUCCESS(IntelVmWrite(
            VMCS_PRIMARY_PROCESSOR_CONTROLS, updated))) {
        KeBugCheckEx(
            HYPERVISOR_ERROR,
            INTEL_BUGCHECK_CONTROL_UPDATE,
            Context->ProcessorIndex,
            updated,
            Bit);
    }
    Context->PrimaryControls = updated;
}

static BOOLEAN
IntelGuestCanTakeNmi(
    VOID
    )
{
    ULONG64 interruptibility;

    if (!IntelVmReadValue(
            VMCS_GUEST_INTERRUPTIBILITY, &interruptibility)) {
        return FALSE;
    }
    return (interruptibility &
            (VMX_GUEST_BLOCKING_BY_NMI |
             VMX_GUEST_BLOCKING_BY_STI |
             VMX_GUEST_BLOCKING_BY_MOV_SS)) == 0;
}

static BOOLEAN
IntelGuestCanTakeInterrupt(
    VOID
    )
{
    ULONG64 interruptibility;
    ULONG64 rflags;

    if (!IntelVmReadValue(
            VMCS_GUEST_INTERRUPTIBILITY, &interruptibility) ||
        !IntelVmReadValue(VMCS_GUEST_RFLAGS, &rflags)) {
        return FALSE;
    }
    if ((interruptibility &
         (VMX_GUEST_BLOCKING_BY_STI |
          VMX_GUEST_BLOCKING_BY_MOV_SS)) != 0) {
        return FALSE;
    }
    return (rflags & (1ull << 9)) != 0; /* IF */
}

VOID
IntelRequestNmiInjection(
    _Inout_ INTEL_CPU_CONTEXT* Context
    )
{

    if (!Context->VirtualNmiEnabled) {
        return;
    }
    if (IntelGuestCanTakeNmi()) {
        if (NT_SUCCESS(IntelVmWrite(
                VMCS_ENTRY_INTERRUPTION_INFO, VMX_ENTRY_INJECT_NMI))) {
            return;
        }
    }
    InterlockedExchange(&Context->PendingNmi, 1);
    IntelSetPrimaryControl(Context, VMX_PRIMARY_NMI_WINDOW, TRUE);
}

VOID
IntelRequestInterruptInjection(
    _Inout_ INTEL_CPU_CONTEXT* Context,
    _In_ UCHAR Vector
    )
{
    if (IntelGuestCanTakeInterrupt()) {
        ULONG info = 0x80000000u | (0u << 8) | (ULONG)Vector;
        if (NT_SUCCESS(IntelVmWrite(
                VMCS_ENTRY_INTERRUPTION_INFO, info))) {
            return;
        }
    }
    Context->PendingInterruptVector = Vector;
    InterlockedExchange(&Context->PendingInterruptValid, 1);
    IntelSetPrimaryControl(Context, VMX_PRIMARY_INTERRUPT_WINDOW, TRUE);
}

VOID
IntelHandleNmiWindow(
    _Inout_ INTEL_CPU_CONTEXT* Context
    )
{

    IntelSetPrimaryControl(Context, VMX_PRIMARY_NMI_WINDOW, FALSE);
    if (Context->VirtualNmiEnabled &&
        InterlockedExchange(&Context->PendingNmi, 0) != 0) {
        /*
         * VMX Type 2 (NMI) with vector 2, valid.  Deliver-error-code is
         * clear for NMIs.
         */
        (VOID)IntelVmWrite(
            VMCS_ENTRY_INTERRUPTION_INFO, VMX_ENTRY_INJECT_NMI);
    }
}

VOID
IntelHandleInterruptWindow(
    _Inout_ INTEL_CPU_CONTEXT* Context
    )
{
    if (InterlockedExchange(&Context->PendingInterruptValid, 0) != 0) {
        ULONG info = 0x80000000u | (0u << 8) |
                     ((ULONG)Context->PendingInterruptVector & 0xffu);
        (VOID)IntelVmWrite(VMCS_ENTRY_INTERRUPTION_INFO, info);
    }
    IntelSetPrimaryControl(Context, VMX_PRIMARY_INTERRUPT_WINDOW, FALSE);
}

static VOID
IntelInjectException(
    _In_ ULONG Information,
    _In_ ULONG ErrorCode,
    _In_ ULONG PendingInformation,
    _In_ const HV_CPU* Cpu,
    _In_ ULONG Reason,
    _In_ ULONG64 GuestRip
    )
{
    NTSTATUS status;

    Information &= VMX_EVENT_INFORMATION_MASK;
    if ((PendingInformation & (1u << 31)) != 0) {

        INTEL_EVENT_CLASS pendingClass = IntelClassifyEvent(PendingInformation);
        INTEL_EVENT_CLASS newClass = IntelClassifyEvent(Information);
        BOOLEAN pendingIsDoubleFault =
            ((PendingInformation >> 8) & 7u) == 3u &&
            (PendingInformation & 0xffu) == 8u;

        if (pendingIsDoubleFault &&
            newClass != INTEL_EVENT_CLASS_BENIGN) {
            IntelFailEventCollision(
                Cpu, Reason, PendingInformation, Information, GuestRip);
        }
        if (IntelPairGeneratesDoubleFault(pendingClass, newClass)) {
            Information = VMX_ENTRY_INJECT_DF;
            ErrorCode = 0;
        } else {
            /* IntelRestoreVectoringEvent already restored the older event. */
            return;
        }

    }

    status = IntelVmWrite(VMCS_ENTRY_INTERRUPTION_INFO, Information);
    if (NT_SUCCESS(status) && (Information & (1u << 11)) != 0) {
        status = IntelVmWrite(VMCS_ENTRY_EXCEPTION_ERROR, ErrorCode);
    }
    if (!NT_SUCCESS(status)) {
        KeBugCheckEx(HYPERVISOR_ERROR, INTEL_BUGCHECK_UNEXPECTED_EXIT,
            Cpu->ProcessorIndex, 8, GuestRip);
    }
}

static VOID
IntelInjectInvalidOpcode(
    _In_ ULONG PendingInformation,
    _In_ const HV_CPU* Cpu,
    _In_ ULONG Reason,
    _In_ ULONG64 GuestRip
    )
{
    if ((PendingInformation & (1u << 31)) != 0) {
        return;
    }

    IntelInjectException(
        VMX_ENTRY_INJECT_UD,
        0,
        PendingInformation,
        Cpu,
        Reason,
        GuestRip);
}

#if JOHNSMITH_DIAGNOSTICS
static ULONG64
IntelReadDiagnosticValue(
    _In_ ULONG Field
    )
{
    ULONG64 value;

    return IntelVmReadValue(Field, &value) ? value : MAXULONGLONG;
}

static VOID
IntelRecordExit(
    _Inout_ INTEL_CPU_CONTEXT* Context,
    _In_ ULONG RawReason,
    _In_ ULONG Reason,
    _In_ ULONG InstructionLength,
    _In_ ULONG64 GuestRip
    )
{
    ULONG64 sequence = (ULONG64)InterlockedIncrement64(
        &Context->ExitSequence);
    INTEL_EXIT_RECORD* record = &Context->ExitHistory[
        sequence & (INTEL_EXIT_HISTORY_COUNT - 1)];

    record->Sequence = 0;
    record->GuestRip = GuestRip;
    record->ExitQualification =
        IntelReadDiagnosticValue(VMCS_EXIT_QUALIFICATION);
    record->GuestLinearAddress =
        IntelReadDiagnosticValue(VMCS_GUEST_LINEAR_ADDRESS);
    record->GuestPhysicalAddress =
        IntelReadDiagnosticValue(VMCS_GUEST_PHYSICAL_ADDRESS);
    record->Reason = Reason;
    record->InstructionLength = InstructionLength;
    record->ExitInterruptionInformation = (ULONG)
        IntelReadDiagnosticValue(VMCS_EXIT_INTERRUPTION_INFO);
    record->IdtVectoringInformation = (ULONG)
        IntelReadDiagnosticValue(VMCS_IDT_VECTORING_INFO);
    record->EntryInterruptionInformation = (ULONG)
        IntelReadDiagnosticValue(VMCS_ENTRY_INTERRUPTION_INFO);
    record->RawReason = RawReason;
    KeMemoryBarrier();
    record->Sequence = sequence;
}
#endif

static ULONG
IntelCompleteVmExit(
    _Inout_ INTEL_CPU_CONTEXT* Context,
    _In_ INTEL_VMEXIT_ACTION Action
    )
{
    if (Action == INTEL_VMEXIT_RESUME) {
        if (InterlockedCompareExchange64(
                &Context->RendezvousOwnedEpoch, 0, 0) != 0) {
            IntelRendezvousFinish(Context);
        } else {
            (VOID)IntelRendezvousJoinActive(Context);
        }
    }
#if JOHNSMITH_DIAGNOSTICS
    LONG64 sequence = InterlockedCompareExchange64(
        &Context->ExitSequence, 0, 0);

    Context->LastExitCompletionTsc = __rdtsc();
    KeMemoryBarrier();
    InterlockedExchange64(&Context->CompletedExitSequence, sequence);
#else
    UNREFERENCED_PARAMETER(Context);
#endif
    return (ULONG)Action;
}

static NTSTATUS
IntelGetRegister(
    _In_ INTEL_GUEST_REGISTERS* Registers,
    _In_ ULONG Index,
    _Out_ PULONG64 Value
    )
{
    switch (Index) {
    case 0: *Value = Registers->Rax; return STATUS_SUCCESS;
    case 1: *Value = Registers->Rcx; return STATUS_SUCCESS;
    case 2: *Value = Registers->Rdx; return STATUS_SUCCESS;
    case 3: *Value = Registers->Rbx; return STATUS_SUCCESS;
    case 4:
        return IntelVmReadValue(VMCS_GUEST_RSP, Value)
            ? STATUS_SUCCESS
            : STATUS_HV_INVALID_VP_STATE;
    case 5: *Value = Registers->Rbp; return STATUS_SUCCESS;
    case 6: *Value = Registers->Rsi; return STATUS_SUCCESS;
    case 7: *Value = Registers->Rdi; return STATUS_SUCCESS;
    case 8: *Value = Registers->R8; return STATUS_SUCCESS;
    case 9: *Value = Registers->R9; return STATUS_SUCCESS;
    case 10: *Value = Registers->R10; return STATUS_SUCCESS;
    case 11: *Value = Registers->R11; return STATUS_SUCCESS;
    case 12: *Value = Registers->R12; return STATUS_SUCCESS;
    case 13: *Value = Registers->R13; return STATUS_SUCCESS;
    case 14: *Value = Registers->R14; return STATUS_SUCCESS;
    case 15: *Value = Registers->R15; return STATUS_SUCCESS;
    default: return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS
IntelSetRegister(
    _Inout_ INTEL_GUEST_REGISTERS* Registers,
    _In_ ULONG Index,
    _In_ ULONG64 Value
    )
{
    switch (Index) {
    case 0: Registers->Rax = Value; return STATUS_SUCCESS;
    case 1: Registers->Rcx = Value; return STATUS_SUCCESS;
    case 2: Registers->Rdx = Value; return STATUS_SUCCESS;
    case 3: Registers->Rbx = Value; return STATUS_SUCCESS;
    case 4: return IntelVmWrite(VMCS_GUEST_RSP, Value);
    case 5: Registers->Rbp = Value; return STATUS_SUCCESS;
    case 6: Registers->Rsi = Value; return STATUS_SUCCESS;
    case 7: Registers->Rdi = Value; return STATUS_SUCCESS;
    case 8: Registers->R8 = Value; return STATUS_SUCCESS;
    case 9: Registers->R9 = Value; return STATUS_SUCCESS;
    case 10: Registers->R10 = Value; return STATUS_SUCCESS;
    case 11: Registers->R11 = Value; return STATUS_SUCCESS;
    case 12: Registers->R12 = Value; return STATUS_SUCCESS;
    case 13: Registers->R13 = Value; return STATUS_SUCCESS;
    case 14: Registers->R14 = Value; return STATUS_SUCCESS;
    case 15: Registers->R15 = Value; return STATUS_SUCCESS;
    default: return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS
IntelHandleCrAccess(
    _Inout_ INTEL_GUEST_REGISTERS* Registers,
    _Inout_ INTEL_CPU_CONTEXT* Context
    )
{
    ULONG64 qualification;
    ULONG64 requested;
    ULONG64 value;
    ULONG cr;
    ULONG access;
    ULONG reg;
    NTSTATUS status;

    if (!IntelVmReadValue(VMCS_EXIT_QUALIFICATION, &qualification)) {
        return STATUS_HV_INVALID_VP_STATE;
    }
    cr = (ULONG)(qualification & 0xf);
    access = (ULONG)((qualification >> 4) & 3);
    reg = (ULONG)((qualification >> 8) & 0xf);

    if (access == 0) {
        status = IntelGetRegister(Registers, reg, &requested);
        if (!NT_SUCCESS(status)) return status;

        if (cr == 0) {
            value = (requested | __readmsr(IA32_VMX_CR0_FIXED0)) &
                    __readmsr(IA32_VMX_CR0_FIXED1);
            status = IntelVmWrite(VMCS_GUEST_CR0, value);
            if (NT_SUCCESS(status)) {
                status = IntelVmWrite(VMCS_CR0_READ_SHADOW, requested);
            }
            return status;
        }
        if (cr == 3) {
            ULONG64 guestCr4;
            BOOLEAN suppressInvalidation;

            if (!IntelVmReadValue(VMCS_CR4_READ_SHADOW, &guestCr4)) {
                return STATUS_HV_INVALID_VP_STATE;
            }
            suppressInvalidation =
                (guestCr4 & X86_CR4_PCIDE) != 0 &&
                (requested & X86_CR3_NOFLUSH) != 0;
            if ((guestCr4 & X86_CR4_PCIDE) == 0 &&
                (requested & X86_CR3_NOFLUSH) != 0) {
                return STATUS_NOT_SUPPORTED;
            }

            requested &= ~X86_CR3_NOFLUSH;
            status = IntelVmWrite(VMCS_GUEST_CR3, requested);
            if (NT_SUCCESS(status) && Context->Vpid != 0 &&
                !suppressInvalidation) {
                INTEL_INVALIDATION_DESCRIPTOR descriptor;

                descriptor.Context = Context->Vpid;
                descriptor.Reserved = 0;
                if (IntelAsmInvvpid(
                        INVVPID_SINGLE_CONTEXT, &descriptor) != 0) {
                    status = STATUS_HV_OPERATION_FAILED;
                }
            }
            return status;
        }
        if (cr == 4) {
            value = (requested | __readmsr(IA32_VMX_CR4_FIXED0) |
                     VMX_CR4_VMXE) & __readmsr(IA32_VMX_CR4_FIXED1);
            status = IntelVmWrite(VMCS_GUEST_CR4, value);
            if (NT_SUCCESS(status)) {
                status = IntelVmWrite(VMCS_CR4_READ_SHADOW, requested);
            }
            return status;
        }
        return STATUS_NOT_SUPPORTED;
    }

    if (access == 1) {
        if (cr == 0) {
            if (!IntelVmReadValue(VMCS_CR0_READ_SHADOW, &value)) {
                return STATUS_HV_INVALID_VP_STATE;
            }
        } else if (cr == 3) {
            if (!IntelVmReadValue(VMCS_GUEST_CR3, &value)) {
                return STATUS_HV_INVALID_VP_STATE;
            }
        } else if (cr == 4) {
            if (!IntelVmReadValue(VMCS_CR4_READ_SHADOW, &value)) {
                return STATUS_HV_INVALID_VP_STATE;
            }
        } else {
            return STATUS_NOT_SUPPORTED;
        }
        return IntelSetRegister(Registers, reg, value);
    }

    if (access == 2 && cr == 0) {
        if (!IntelVmReadValue(VMCS_CR0_READ_SHADOW, &requested)) {
            return STATUS_HV_INVALID_VP_STATE;
        }
        requested &= ~(1ull << 3);
        value = (requested | __readmsr(IA32_VMX_CR0_FIXED0)) &
                __readmsr(IA32_VMX_CR0_FIXED1);
        status = IntelVmWrite(VMCS_GUEST_CR0, value);
        if (NT_SUCCESS(status)) {
            status = IntelVmWrite(VMCS_CR0_READ_SHADOW, requested);
        }
        return status;
    }

    if (access == 3 && cr == 0) {
        if (!IntelVmReadValue(VMCS_CR0_READ_SHADOW, &requested)) {
            return STATUS_HV_INVALID_VP_STATE;
        }
        value = (qualification >> 16) & 0xffff;
        value = (requested & ~0xfull) | (value & 0xf);
        if ((requested & 1) != 0) value |= 1;
        requested = value;
        value = (requested | __readmsr(IA32_VMX_CR0_FIXED0)) &
                __readmsr(IA32_VMX_CR0_FIXED1);
        status = IntelVmWrite(VMCS_GUEST_CR0, value);
        if (NT_SUCCESS(status)) {
            status = IntelVmWrite(VMCS_CR0_READ_SHADOW, requested);
        }
        return status;
    }
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS
IntelHandleDrAccess(
    _Inout_ INTEL_GUEST_REGISTERS* Registers
    )
{
    ULONG64 qualification;
    ULONG64 guestCr4;
    ULONG64 value;
    ULONG dr;
    ULONG direction;
    ULONG reg;
    NTSTATUS status;

    if (!IntelVmReadValue(VMCS_EXIT_QUALIFICATION, &qualification) ||
        !IntelVmReadValue(VMCS_CR4_READ_SHADOW, &guestCr4)) {
        return STATUS_HV_INVALID_VP_STATE;
    }
    dr = (ULONG)(qualification & 7);
    direction = (ULONG)((qualification >> 4) & 1);
    reg = (ULONG)((qualification >> 8) & 0xf);

    if (dr == 4 || dr == 5) {
        if ((guestCr4 & X86_CR4_DE) != 0) {
            return STATUS_ILLEGAL_INSTRUCTION;
        }
        dr += 2;
    }
    if (dr > 3 && dr != 6 && dr != 7) {
        return STATUS_NOT_SUPPORTED;
    }

    if (direction == 0) {
        status = IntelGetRegister(Registers, reg, &value);
        if (!NT_SUCCESS(status)) return status;
        switch (dr) {
        case 0: __writedr(0, value); break;
        case 1: __writedr(1, value); break;
        case 2: __writedr(2, value); break;
        case 3: __writedr(3, value); break;
        case 6: __writedr(6, value); break;
        case 7: return IntelVmWrite(VMCS_GUEST_DR7, value);
        }
        return STATUS_SUCCESS;
    }

    switch (dr) {
    case 0: value = __readdr(0); break;
    case 1: value = __readdr(1); break;
    case 2: value = __readdr(2); break;
    case 3: value = __readdr(3); break;
    case 6: value = __readdr(6); break;
    case 7:
        if (!IntelVmReadValue(VMCS_GUEST_DR7, &value)) {
            return STATUS_HV_INVALID_VP_STATE;
        }
        break;
    default: return STATUS_NOT_SUPPORTED;
    }
    return IntelSetRegister(Registers, reg, value);
}

static BOOLEAN
IntelIsVmxInstructionExit(
    _In_ ULONG Reason
    )
{
    return (Reason >= 19 && Reason <= 27) ||
           Reason == 50 || Reason == 53 || Reason == 59;
}

_Success_(return != FALSE)
static BOOLEAN
IntelCaptureGuestControlRegister(
    _In_ ULONG GuestField,
    _In_ ULONG MaskField,
    _In_ ULONG ShadowField,
    _Out_ PULONG64 Value
    )
{
    ULONG64 actual;
    ULONG64 mask;
    ULONG64 shadow;

    *Value = 0;
    if (!IntelVmReadValue(GuestField, &actual) ||
        !IntelVmReadValue(MaskField, &mask) ||
        !IntelVmReadValue(ShadowField, &shadow)) {
        return FALSE;
    }
    *Value = (actual & ~mask) | (shadow & mask);
    NT_ASSERT(((0xa5ull & ~0xf0ull) | (0x3cull & 0xf0ull)) == 0x35ull);
    return TRUE;
}

static BOOLEAN
IntelCaptureStopState(
    _Out_ INTEL_CPU_CONTEXT* Context
    )
{
    return IntelCaptureGuestControlRegister(
               VMCS_GUEST_CR0,
               VMCS_CR0_GUEST_HOST_MASK,
               VMCS_CR0_READ_SHADOW,
               &Context->ResumeCr0) &&
           IntelVmReadValue(VMCS_GUEST_CR3, &Context->ResumeCr3) &&
           IntelCaptureGuestControlRegister(
               VMCS_GUEST_CR4,
               VMCS_CR4_GUEST_HOST_MASK,
               VMCS_CR4_READ_SHADOW,
               &Context->ResumeCr4) &&
           IntelVmReadValue(VMCS_GUEST_DR7, &Context->ResumeDr7) &&
           IntelVmReadValue(VMCS_GUEST_FS_BASE, &Context->ResumeFsBase) &&
           IntelVmReadValue(VMCS_GUEST_GS_BASE, &Context->ResumeGsBase) &&
           IntelVmReadValue(VMCS_GUEST_PAT, &Context->ResumePat) &&
           IntelVmReadValue(VMCS_GUEST_EFER, &Context->ResumeEfer) &&
           IntelVmReadValue(
               VMCS_GUEST_SYSENTER_CS, &Context->ResumeSysenterCs) &&
           IntelVmReadValue(
               VMCS_GUEST_SYSENTER_ESP, &Context->ResumeSysenterEsp) &&
           IntelVmReadValue(
               VMCS_GUEST_SYSENTER_EIP, &Context->ResumeSysenterEip) &&
           IntelVmReadValue(
               VMCS_GUEST_GDTR_BASE, &Context->ResumeGdtrBase) &&
           IntelVmReadValue(
               VMCS_GUEST_IDTR_BASE, &Context->ResumeIdtrBase) &&
           IntelVmReadValue(
               VMCS_GUEST_GDTR_LIMIT, &Context->ResumeGdtrLimit) &&
           IntelVmReadValue(
               VMCS_GUEST_IDTR_LIMIT, &Context->ResumeIdtrLimit);
}

static VOID
IntelHypercallReturnClamped(
    _Inout_ INTEL_CPU_CONTEXT* Context,
    _Inout_ INTEL_GUEST_REGISTERS* Registers
    )
{
    Registers->Rax = Context->ClampedCpuidEax;
    Registers->Rbx = Context->ClampedCpuidEbx;
    Registers->Rcx = Context->ClampedCpuidEcx;
    Registers->Rdx = Context->ClampedCpuidEdx;
}

static VOID
IntelHandleHypercall(
    _Inout_ INTEL_CPU_CONTEXT* Context,
    _In_ const HV_CPU* Cpu,
    _Inout_ INTEL_GUEST_REGISTERS* Registers,
    _In_ ULONG64 GuestRip,
    _In_ ULONG InstructionLength
    )
{
    ULONG subleaf;
    BOOLEAN isRegister;
    PINTEL_HCALL_PAGE page;
    ULONG cmdIndex;

    subleaf = (ULONG)Registers->Rcx;
    isRegister = (subleaf == IntelHypercallRegisterSubleaf());

    if (isRegister) {
        ULONG64 sharedVa = Registers->Rbx;
        (VOID)IntelHypercallWorkerEnqueueRegister(
            Context, PsGetCurrentProcess(), (PVOID)sharedVa);
        IntelHypercallReturnClamped(Context, Registers);
        IntelAdvanceGuestRip(
            Context, Cpu, VMX_EXIT_CPUID, GuestRip, InstructionLength);
        return;
    }

    if (!ExAcquireRundownProtection(&Context->HypercallPageRundown)) {
        IntelHypercallReturnClamped(Context, Registers);
        IntelAdvanceGuestRip(
            Context, Cpu, VMX_EXIT_CPUID, GuestRip, InstructionLength);
        return;
    }

    page = (PINTEL_HCALL_PAGE)Context->HypercallSharedPage;
    if (!Context->HypercallPageRegistered || page == NULL) {
        ExReleaseRundownProtection(&Context->HypercallPageRundown);
        IntelHypercallReturnClamped(Context, Registers);
        IntelAdvanceGuestRip(
            Context, Cpu, VMX_EXIT_CPUID, GuestRip, InstructionLength);
        return;
    }

    for (cmdIndex = 0; cmdIndex < INTEL_HYPERCALL_CMD_COUNT; ++cmdIndex) {
        if (page->CommandId ==
            IntelHypercallCommandId((INTEL_HYPERCALL_CMD)cmdIndex)) {
            break;
        }
    }

    switch (cmdIndex) {
    case (ULONG)INTEL_HYPERCALL_CMD_REGISTER:
        page->Result = (ULONG64)STATUS_INVALID_DEVICE_REQUEST;
        break;
    case (ULONG)INTEL_HYPERCALL_CMD_INSTALL:
    case (ULONG)INTEL_HYPERCALL_CMD_REMOVE:
    case (ULONG)INTEL_HYPERCALL_CMD_READ:
    case (ULONG)INTEL_HYPERCALL_CMD_WRITE:
    case (ULONG)INTEL_HYPERCALL_CMD_QUERY_HOOK:
    case (ULONG)INTEL_HYPERCALL_CMD_LIST_HOOKS:
    case (ULONG)INTEL_HYPERCALL_CMD_PROBE: {
        if (page->ResultSequence == (ULONG64)page->Sequence) {
            break;
        }
        {
            NTSTATUS eq = IntelHypercallWorkerEnqueue(
                (INTEL_HYPERCALL_CMD)cmdIndex,
                Context,
                page,
                page->Args[0],
                page->Args[1]);
            if (eq != STATUS_SUCCESS) {
                page->Result = (ULONG64)eq;
                KeMemoryBarrier();
                page->ResultSequence = (ULONG64)page->Sequence;
            }
        }
        break;
    }
    default:
        page->Result = (ULONG64)STATUS_INVALID_PARAMETER;
        break;
    }

    ExReleaseRundownProtection(&Context->HypercallPageRundown);
    IntelHypercallReturnClamped(Context, Registers);
    IntelAdvanceGuestRip(
        Context, Cpu, VMX_EXIT_CPUID, GuestRip, InstructionLength);
}

ULONG
IntelVmExitHandler(
    _Inout_ INTEL_GUEST_REGISTERS* Registers,
    _Inout_ HV_CPU* Cpu
    )
{
    INTEL_CPU_CONTEXT* context;
    SIZE_T value;
    ULONG rawReason;
    ULONG reason;
    ULONG instructionLength;
    ULONG pendingInformation;
    ULONG64 guestRip;
    INTEL_RENDEZVOUS_POLICY rendezvousPolicy;
    NTSTATUS status;
    int cpuid[4];

    if (Registers == NULL || Cpu == NULL || Cpu->VendorContext == NULL) {
        KeBugCheckEx(HYPERVISOR_ERROR, INTEL_BUGCHECK_UNEXPECTED_EXIT,
            MAXULONG, 0, 0);
    }
    context = (INTEL_CPU_CONTEXT*)Cpu->VendorContext;
#if JOHNSMITH_DIAGNOSTICS
    context->LastExitEntryTsc = __rdtsc();
#endif
    if (__vmx_vmread(VMCS_EXIT_REASON, &value) != 0) {
        KeBugCheckEx(HYPERVISOR_ERROR, INTEL_BUGCHECK_UNEXPECTED_EXIT,
            MAXULONG, 1, 0);
    }
    rawReason = (ULONG)value;
    reason = rawReason & 0xffffu;
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
#if JOHNSMITH_DIAGNOSTICS
    IntelRecordExit(
        context, rawReason, reason, instructionLength, guestRip);
#endif
    if ((rawReason & VMX_EXIT_REASON_ENTRY_FAILURE) != 0) {
        KeBugCheckEx(HYPERVISOR_ERROR, INTEL_BUGCHECK_UNEXPECTED_EXIT,
            Cpu->ProcessorIndex, rawReason, guestRip);
    }
    status = IntelRestoreVectoringEvent(
        instructionLength, &pendingInformation);
    if (!NT_SUCCESS(status)) {
        KeBugCheckEx(HYPERVISOR_ERROR, INTEL_BUGCHECK_UNEXPECTED_EXIT,
            Cpu->ProcessorIndex, 9, guestRip);
    }
    IntelFlushEptIfNeeded(context);
    (VOID)IntelRendezvousJoinActive(context);
    rendezvousPolicy = IntelRendezvousClassifyAndConsume(
        context, reason, (ULONG)Registers->Rcx);
    if (rendezvousPolicy == INTEL_POLICY_MANDATORY ||
        rendezvousPolicy == INTEL_POLICY_CONDITIONAL) {
        (VOID)IntelRendezvousBegin(context);
    }

    if (reason == VMX_EXIT_INIT_SIGNAL) {

        return IntelCompleteVmExit(context, INTEL_VMEXIT_RESUME);
    }

    if (reason == VMX_EXIT_NMI_WINDOW) {

        IntelHandleNmiWindow(context);
        return IntelCompleteVmExit(context, INTEL_VMEXIT_RESUME);
    }

    if (reason == VMX_EXIT_INTERRUPT_WINDOW) {
        IntelHandleInterruptWindow(context);
        return IntelCompleteVmExit(context, INTEL_VMEXIT_RESUME);
    }

    if (reason == VMX_EXIT_CPUID) {
        ULONG leaf = (ULONG)Registers->Rax;
        ULONG subleaf = (ULONG)Registers->Rcx;

        /*
         * Hypercall trigger: CPUID leaf 1 with the per-boot magic subleaf.
         * asm/intel.asm forces leaf 1 to this C handler in every build
         * configuration, so the fast path cannot swallow the trap.  The
         * explicit seeded guard is required because the leaf is now
         * hardcoded.  Leaf 1 ignores ECX on the target CPU, so the subleaf
         * value itself is undetectable; only the VM-exit is.
         */
        if (leaf == 1 && IntelHypercallIsSeeded() &&
            (subleaf == IntelHypercallSubleaf() ||
             subleaf == IntelHypercallRegisterSubleaf())) {
            IntelHandleHypercall(
                context, Cpu, Registers, guestRip, instructionLength);
            return IntelCompleteVmExit(context, INTEL_VMEXIT_RESUME);
        }

        if (leaf == 0) {
            Registers->Rax = context->CpuidLeaf0Eax;
            Registers->Rbx = context->CpuidLeaf0Ebx;
            Registers->Rcx = context->CpuidLeaf0Ecx;
            Registers->Rdx = context->CpuidLeaf0Edx;
        } else {
            __cpuidex(cpuid, (int)leaf, (int)subleaf);
            Registers->Rax = (ULONG)cpuid[0];
            Registers->Rbx = (ULONG)cpuid[1];
            Registers->Rcx = (ULONG)cpuid[2];
            Registers->Rdx = (ULONG)cpuid[3];
        }

        switch (leaf) {
        case 1:
            Registers->Rcx &= 0x7FFFFFDFull;
            break;
        case 7:
            if (subleaf == 0 && context->CpuidClearLeaf7Ebx != 0) {
                Registers->Rbx &= ~(ULONG64)context->CpuidClearLeaf7Ebx;
            }
            break;
        case 0xDu:
            if (subleaf == 1 && context->CpuidClearLeafDEax != 0) {
                Registers->Rax &= ~(ULONG64)context->CpuidClearLeafDEax;
            }
            break;
        case 0x80000001u:
            if (context->CpuidClearLeaf80000001Edx != 0) {
                Registers->Rdx &=
                    ~(ULONG64)context->CpuidClearLeaf80000001Edx;
            }
            break;
        }

        IntelAdvanceGuestRip(
            context, Cpu, reason, guestRip, instructionLength);
        return IntelCompleteVmExit(context, INTEL_VMEXIT_RESUME);
    }

    if (reason == VMX_EXIT_RDTSC || reason == VMX_EXIT_RDTSCP) {
        ULONG64 tsc;

        if (reason == VMX_EXIT_RDTSCP) {
            unsigned int aux;
            tsc = IntelRendezvousGuestTsc(context, __rdtscp(&aux));
            Registers->Rcx = aux;
        } else {
            tsc = IntelRendezvousGuestTsc(context, __rdtsc());
        }
        Registers->Rax = (ULONG)tsc;
        Registers->Rdx = (ULONG)(tsc >> 32);
        IntelAdvanceGuestRip(
            context, Cpu, reason, guestRip, instructionLength);
        return IntelCompleteVmExit(context, INTEL_VMEXIT_RESUME);
    }

    if (reason == VMX_EXIT_EXCEPTION_OR_NMI) {
        ULONG64 information;
        ULONG64 errorCode = 0;
        if (!IntelVmReadValue(VMCS_EXIT_INTERRUPTION_INFO, &information) ||
            (information & (1ull << 31)) == 0) {
            KeBugCheckEx(HYPERVISOR_ERROR,
                INTEL_BUGCHECK_UNEXPECTED_EXIT, reason, 6, guestRip);
        }
        if (context->VirtualNmiEnabled &&
            (information & (VMX_EVENT_VALID | VMX_EVENT_TYPE_MASK |
                            VMX_EVENT_VECTOR_MASK)) ==
                (VMX_EVENT_VALID | VMX_EVENT_TYPE_NMI | 2u)) {
            ULONG64 interruptibility;

            if (IntelRendezvousConsumeExpectedNmi(context)) {
                return IntelCompleteVmExit(
                    context, INTEL_VMEXIT_RESUME);
            }
            if (!IntelVmReadValue(
                    VMCS_GUEST_INTERRUPTIBILITY, &interruptibility)) {
                KeBugCheckEx(
                    HYPERVISOR_ERROR,
                    INTEL_BUGCHECK_UNEXPECTED_EXIT,
                    reason,
                    16,
                    guestRip);
            }
            if (InterlockedCompareExchange(
                    &context->PendingNmi, 0, 0) != 0 ||
                (pendingInformation & VMX_EVENT_VALID) != 0 ||
                (interruptibility &
                 (VMX_GUEST_BLOCKING_BY_STI |
                  VMX_GUEST_BLOCKING_BY_MOV_SS)) != 0) {
                InterlockedExchange(&context->PendingNmi, 1);
                IntelSetPrimaryControl(
                    context, VMX_PRIMARY_NMI_WINDOW, TRUE);
                return IntelCompleteVmExit(
                    context, INTEL_VMEXIT_RESUME);
            }
            if (!NT_SUCCESS(IntelVmWrite(
                        VMCS_GUEST_INTERRUPTIBILITY,
                        interruptibility & ~VMX_GUEST_BLOCKING_BY_NMI)) ||
                !NT_SUCCESS(IntelVmWrite(
                        VMCS_ENTRY_INTERRUPTION_INFO,
                        VMX_ENTRY_INJECT_NMI))) {
                KeBugCheckEx(
                    HYPERVISOR_ERROR,
                    INTEL_BUGCHECK_UNEXPECTED_EXIT,
                    reason,
                    16,
                    guestRip);
            }
            return IntelCompleteVmExit(context, INTEL_VMEXIT_RESUME);
        }
        if ((information & (1ull << 11)) != 0 &&
            !IntelVmReadValue(
                VMCS_EXIT_INTERRUPTION_ERROR, &errorCode)) {
            KeBugCheckEx(HYPERVISOR_ERROR,
                INTEL_BUGCHECK_UNEXPECTED_EXIT, reason, 12, guestRip);
        }
        information &= VMX_EVENT_INFORMATION_MASK;
        IntelInjectException(
            (ULONG)information,
            (ULONG)errorCode,
            pendingInformation,
            Cpu,
            reason,
            guestRip);
        return IntelCompleteVmExit(context, INTEL_VMEXIT_RESUME);
    }

    if (reason == VMX_EXIT_CR_ACCESS) {
        status = IntelHandleCrAccess(Registers, context);
        if (NT_SUCCESS(status)) {
            IntelAdvanceGuestRip(
                context, Cpu, reason, guestRip, instructionLength);
        } else if (status == STATUS_NOT_SUPPORTED) {
            IntelInjectException(
                VMX_ENTRY_INJECT_GP,
                0,
                pendingInformation,
                Cpu,
                reason,
                guestRip);
        } else {
            KeBugCheckEx(HYPERVISOR_ERROR,
                INTEL_BUGCHECK_UNEXPECTED_EXIT, reason, 11, guestRip);
        }
        return IntelCompleteVmExit(context, INTEL_VMEXIT_RESUME);
    }

    if (reason == VMX_EXIT_DR_ACCESS) {
        status = IntelHandleDrAccess(Registers);
        if (NT_SUCCESS(status)) {
            IntelAdvanceGuestRip(
                context, Cpu, reason, guestRip, instructionLength);
        } else if (status == STATUS_ILLEGAL_INSTRUCTION) {
            IntelInjectInvalidOpcode(
                pendingInformation, Cpu, reason, guestRip);
        } else if (status == STATUS_NOT_SUPPORTED) {
            IntelInjectException(
                VMX_ENTRY_INJECT_GP, 0, pendingInformation,
                Cpu, reason, guestRip);
        } else {
            KeBugCheckEx(HYPERVISOR_ERROR,
                INTEL_BUGCHECK_UNEXPECTED_EXIT, reason, 14, guestRip);
        }
        return IntelCompleteVmExit(context, INTEL_VMEXIT_RESUME);
    }

    if (reason == VMX_EXIT_RDMSR) {
        ULONG msr = (ULONG)Registers->Rcx;
        ULONG64 msrValue;

        if (msr == IA32_TIME_STAMP_COUNTER) {
            msrValue = IntelRendezvousGuestTsc(context, __rdtsc());
            Registers->Rax = (ULONG)msrValue;
            Registers->Rdx = (ULONG)(msrValue >> 32);
            IntelAdvanceGuestRip(
                context, Cpu, reason, guestRip, instructionLength);
        } else if (msr == IA32_FEATURE_CONTROL) {
            msrValue = __readmsr(msr);
            msrValue &= ~(IA32_FEATURE_CONTROL_VMX_IN_SMX |
                          IA32_FEATURE_CONTROL_VMX_OUTSIDE_SMX);
            msrValue |= 1;
            Registers->Rax = (ULONG)msrValue;
            Registers->Rdx = (ULONG)(msrValue >> 32);
            IntelAdvanceGuestRip(
                context, Cpu, reason, guestRip, instructionLength);
        } else {
            IntelInjectException(
                VMX_ENTRY_INJECT_GP,
                0,
                pendingInformation,
                Cpu,
                reason,
                guestRip);
        }
        return IntelCompleteVmExit(context, INTEL_VMEXIT_RESUME);
    }

    if (reason == VMX_EXIT_WRMSR) {
        ULONG msr = (ULONG)Registers->Rcx;

        if (msr == IA32_S_CET &&
            (Registers->Rax & MAXULONG) == 0 &&
            (Registers->Rdx & MAXULONG) == 0) {
            __writemsr(IA32_S_CET, 0);
            IntelAdvanceGuestRip(
                context, Cpu, reason, guestRip, instructionLength);
            return IntelCompleteVmExit(context, INTEL_VMEXIT_RESUME);
        }
        IntelInjectException(
            VMX_ENTRY_INJECT_GP,
            0,
            pendingInformation,
            Cpu,
            reason,
            guestRip);
        return IntelCompleteVmExit(context, INTEL_VMEXIT_RESUME);
    }

    if (reason == VMX_EXIT_EPT_VIOLATION) {
        ULONG64 qualification = 0;
        ULONG64 guestPhysical = 0;
        ULONG64 guestLinear = 0;
        INTEL_EPT_VIOLATION decoded;
        INTEL_HOOK_POLICY policy;

        if (!IntelVmReadValue(VMCS_EXIT_QUALIFICATION, &qualification) ||
            !IntelVmReadValue(
                VMCS_GUEST_PHYSICAL_ADDRESS, &guestPhysical)) {
            KeBugCheckEx(HYPERVISOR_ERROR,
                INTEL_BUGCHECK_UNEXPECTED_EXIT, reason, 5, guestRip);
        }
        /* SDM rev. 092, Table 30-7: GLA is architecturally valid only when
           qualification bit 7 is set.  Only read it in that case. */
        if ((qualification & VMX_EPT_QUAL_GLA_VALID) != 0 &&
            !IntelVmReadValue(
                VMCS_GUEST_LINEAR_ADDRESS, &guestLinear)) {
            KeBugCheckEx(HYPERVISOR_ERROR,
                INTEL_BUGCHECK_UNEXPECTED_EXIT, reason, 15, guestRip);
        }
        IntelDecodeEptViolation(
            qualification, guestPhysical, guestLinear, &decoded);

        {
            INTEL_BACKEND_CONTEXT* backend =
                (INTEL_BACKEND_CONTEXT*)context->BackendContext;

            if (IntelHookLookup(backend, guestPhysical, &policy)) {
                IntelRendezvousReloadHookBudget(context);
                const INTEL_CPU_EPT_VIEW* target = NULL;

                if (backend != NULL && backend->HookRoot != NULL) {
                    BOOLEAN onSecondary = (context->EptPointer ==
                        context->SecondaryEpt.EptPointer);
                    if (InterlockedCompareExchange(
                            &backend->ForcePrimaryEpt, 0, 0) != 0) {
                        /* Removal keeps policy visible until primary leaves
                           are executable again. Retry without advancing RIP. */
                        return IntelCompleteVmExit(
                            context, INTEL_VMEXIT_RESUME);
                    }
                    if (decoded.Execute && !onSecondary) {
                        target = &context->SecondaryEpt;
                    } else if (!decoded.Execute && onSecondary) {
                        target = &context->PrimaryEpt;
                    }
                }
                if (target != NULL) {
                    NTSTATUS switchStatus =
                        IntelSwitchActiveEptRoot(context, target);
                    if (NT_SUCCESS(switchStatus)) {
                        /* Retry the guest instruction on the new view; do
                           not advance RIP. */
                        return IntelCompleteVmExit(
                            context, INTEL_VMEXIT_RESUME);
                    }
                }
                /* Policy match but view already correct (or unswitchable):
                   fail-stop with policy identity in bugcheck parameter 4 so
                   bring-up can diagnose the stuck transition. */
                KeBugCheckEx(
                    HYPERVISOR_ERROR,
                    INTEL_BUGCHECK_EPT_VIOLATION,
                    decoded.Qualification,
                    decoded.GuestPhysicalAddress,
                    ((ULONG64)policy.Cookie << 32) | policy.Kind);
            }
        }

        {
            INTEL_BACKEND_CONTEXT* backend =
                (INTEL_BACKEND_CONTEXT*)context->BackendContext;
            if (backend != NULL && backend->HookRoot != NULL &&
                context->EptPointer == context->SecondaryEpt.EptPointer &&
                decoded.Execute) {
                NTSTATUS switchStatus = IntelSwitchActiveEptRoot(
                    context, &context->PrimaryEpt);
                if (NT_SUCCESS(switchStatus)) {
                    return IntelCompleteVmExit(
                        context, INTEL_VMEXIT_RESUME);
                }
            }
        }

        IntelFailEptViolation(Cpu, &decoded, guestRip);
    }

    if (reason == VMX_EXIT_EPT_MISCONFIGURATION) {
        ULONG64 guestPhysical = 0;
        (VOID)IntelVmReadValue(
            VMCS_GUEST_PHYSICAL_ADDRESS, &guestPhysical);
        IntelFailEptMisconfiguration(Cpu, guestPhysical, guestRip);
    }

    if (reason == VMX_EXIT_XSETBV) {
        IntelInjectException(
            VMX_ENTRY_INJECT_GP, 0, pendingInformation,
            Cpu, reason, guestRip);
        return IntelCompleteVmExit(context, INTEL_VMEXIT_RESUME);
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
            if (!IntelCaptureStopState(context)) {
                KeBugCheckEx(HYPERVISOR_ERROR,
                    INTEL_BUGCHECK_UNEXPECTED_EXIT, reason, 7, guestRip);
            }
            if (__vmx_vmread(VMCS_GUEST_RSP, &value) != 0) {
                KeBugCheckEx(HYPERVISOR_ERROR,
                    INTEL_BUGCHECK_UNEXPECTED_EXIT, reason, 4, 0);
            }
            context->ResumeRsp = value;
            status = IntelGetAdvancedGuestRip(
                context,
                guestRip,
                instructionLength,
                &context->ResumeRip);
            if (!NT_SUCCESS(status)) {
                KeBugCheckEx(HYPERVISOR_ERROR,
                    INTEL_BUGCHECK_UNEXPECTED_EXIT, reason, 10, guestRip);
            }
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
            return IntelCompleteVmExit(context, INTEL_VMEXIT_STOP);
        }
        IntelInjectInvalidOpcode(
            pendingInformation, Cpu, reason, guestRip);
        return IntelCompleteVmExit(context, INTEL_VMEXIT_RESUME);
    }

    if (IntelIsVmxInstructionExit(reason)) {
        IntelInjectInvalidOpcode(
            pendingInformation, Cpu, reason, guestRip);
        return IntelCompleteVmExit(context, INTEL_VMEXIT_RESUME);
    }

    KeBugCheckEx(HYPERVISOR_ERROR, INTEL_BUGCHECK_UNEXPECTED_EXIT,
        Cpu->ProcessorIndex, reason, guestRip);
}
