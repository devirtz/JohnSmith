#include "intel_internal.h"

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

static VOID
IntelInjectException(
    _In_ ULONG Information,
    _In_ ULONG ErrorCode
    )
{
    Information &= VMX_EVENT_INFORMATION_MASK;
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
    information &= VMX_EVENT_INFORMATION_MASK;
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
    ULONG64 csSelector;
    ULONG64 guestCr4;

    if ((ULONG)Registers->Rcx != 0 ||
        !IntelVmReadValue(VMCS_GUEST_CS_SELECTOR, &csSelector) ||
        !IntelVmReadValue(VMCS_GUEST_CR4, &guestCr4) ||
        (csSelector & 3) != 0 || (guestCr4 & (1ull << 18)) == 0) {
        return FALSE;
    }
    __cpuidex(cpuid, 0xD, 0);
    supported = ((ULONG64)(ULONG)cpuid[3] << 32) | (ULONG)cpuid[0];
    requested = ((ULONG64)(ULONG)Registers->Rdx << 32) |
                (ULONG)Registers->Rax;
    if ((requested & ~supported) != 0 || (requested & 3) != 3) return FALSE;
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
               VMCS_GUEST_SYSENTER_EIP, &Context->ResumeSysenterEip);
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
        information &= VMX_EVENT_INFORMATION_MASK;
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
        ULONG msr = (ULONG)Registers->Rcx;

        if (msr == IA32_S_CET &&
            (Registers->Rax & MAXULONG) == 0 &&
            (Registers->Rdx & MAXULONG) == 0) {
            __writemsr(IA32_S_CET, 0);
            (VOID)IntelVmWrite(
                VMCS_GUEST_RIP, guestRip + instructionLength);
            return INTEL_VMEXIT_RESUME;
        }
        IntelInjectException(VMX_ENTRY_INJECT_GP, 0);
        return INTEL_VMEXIT_RESUME;
    }

    if (reason == VMX_EXIT_EPT_VIOLATION) {
        ULONG64 qualification;
        ULONG64 linearAddress;
        ULONG errorCode = 0;
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
        if ((qualification & ((1ull << 3) | (1ull << 4) |
                              (1ull << 5))) != 0) {
            errorCode |= 1;
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
            if (!IntelCaptureStopState(context)) {
                KeBugCheckEx(HYPERVISOR_ERROR,
                    INTEL_BUGCHECK_UNEXPECTED_EXIT, reason, 7, guestRip);
            }
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
