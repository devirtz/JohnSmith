#include "intel_internal.h"
#include "../common/x86_common.h"

#define VMX_EVENT_INFORMATION_MASK 0x80000FFFu

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
    if ((Context->EntryControls & VMX_ENTRY_IA32E_MODE) == 0 ||
        (csAccessRights & (1ull << 13)) == 0) {
        /* Outside 64-bit code, architectural RIP is the zero-extended EIP. */
        nextRip = (ULONG)nextRip;
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
        IntelFailEventCollision(
            Cpu, Reason, PendingInformation, Information, GuestRip);
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

            /*
             * Intel SDM rev. 092, MOV to/from Control Registers: with
             * CR4.PCIDE set, source bit 63 suppresses invalidation but is
             * never stored in CR3.
             */
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

static BOOLEAN
IntelHandleXsetbv(
    _In_ INTEL_GUEST_REGISTERS* Registers
    )
{
    ULONG64 requested;
    ULONG64 csSelector;
    ULONG64 guestCr4;

    if ((ULONG)Registers->Rcx != 0 ||
        !IntelVmReadValue(VMCS_GUEST_CS_SELECTOR, &csSelector) ||
        !IntelVmReadValue(VMCS_GUEST_CR4, &guestCr4) ||
        (csSelector & 3) != 0 || (guestCr4 & (1ull << 18)) == 0) {
        return FALSE;
    }
    requested = ((ULONG64)(ULONG)Registers->Rdx << 32) |
                (ULONG)Registers->Rax;
    if (!HvX86IsValidXcr0(requested)) {
        return FALSE;
    }
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

static BOOLEAN
IntelCaptureStopState(
    _Out_ INTEL_CPU_CONTEXT* Context
    )
{
    return IntelVmReadValue(VMCS_CR0_READ_SHADOW, &Context->ResumeCr0) &&
           IntelVmReadValue(VMCS_GUEST_CR3, &Context->ResumeCr3) &&
           IntelVmReadValue(VMCS_CR4_READ_SHADOW, &Context->ResumeCr4) &&
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

    if (reason == VMX_EXIT_INIT_SIGNAL) {
        /*
         * Processor reset/hotplug is unsupported while JohnSmith is active.
         * Intel specifies that an INIT-induced VM exit has not modified the
         * guest state.  Leave the VMCS active and resume that unchanged state;
         * a subsequent SIPI is then discarded by hardware because the guest
         * is not in the wait-for-SIPI activity state.
         */
        return IntelCompleteVmExit(context, INTEL_VMEXIT_RESUME);
    }

    if (reason == VMX_EXIT_CPUID) {
        ULONG leaf = (ULONG)Registers->Rax;
        ULONG subleaf = (ULONG)Registers->Rcx;

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

    if (reason == VMX_EXIT_EXCEPTION_OR_NMI) {
        ULONG64 information;
        ULONG64 errorCode = 0;
        if (!IntelVmReadValue(VMCS_EXIT_INTERRUPTION_INFO, &information) ||
            (information & (1ull << 31)) == 0) {
            KeBugCheckEx(HYPERVISOR_ERROR,
                INTEL_BUGCHECK_UNEXPECTED_EXIT, reason, 6, guestRip);
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

    if (reason == VMX_EXIT_RDMSR) {
        ULONG msr = (ULONG)Registers->Rcx;
        ULONG64 msrValue;

        if (msr == IA32_FEATURE_CONTROL) {
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

        if (!IntelVmReadValue(VMCS_EXIT_QUALIFICATION, &qualification) ||
            !IntelVmReadValue(
                VMCS_GUEST_PHYSICAL_ADDRESS, &guestPhysical)) {
            KeBugCheckEx(HYPERVISOR_ERROR,
                INTEL_BUGCHECK_UNEXPECTED_EXIT, reason, 5, guestRip);
        }
        KeBugCheckEx(
            HYPERVISOR_ERROR,
            INTEL_BUGCHECK_EPT_VIOLATION,
            qualification,
            guestPhysical,
            guestRip);
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
