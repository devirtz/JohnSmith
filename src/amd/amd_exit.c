#include "amd_internal.h"

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

