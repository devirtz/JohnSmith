#include "amd_internal.h"
#include "../common/x86_common.h"

DECLSPEC_NORETURN
static VOID
AmdFailEventCollision(
    _In_ const AMD_VMCB* Vmcb,
    _In_ const HV_CPU* Cpu,
    _In_ ULONG64 NewInformation
    )
{
    KeBugCheckEx(
        HYPERVISOR_ERROR,
        AMD_BUGCHECK_EVENT_COLLISION,
        Vmcb == NULL ? MAXULONG :
            (((ULONG64)(Cpu == NULL ? MAXULONG : Cpu->ProcessorIndex)) << 32) |
            (Vmcb->Control.ExitCode & MAXULONG),
        Vmcb == NULL ? NewInformation :
            ((Vmcb->Control.EventInjection & MAXULONG) << 32) |
            (NewInformation & MAXULONG),
        Vmcb == NULL ? 0 : Vmcb->State.Rip);
}

static VOID
AmdInjectException(
    _Inout_ AMD_VMCB* Vmcb,
    _In_ const HV_CPU* Cpu,
    _In_ ULONG64 Information,
    _In_ ULONG ErrorCode
    )
{
    if ((Vmcb->Control.EventInjection & AMD_EVENT_VALID) != 0) {
        AmdFailEventCollision(Vmcb, Cpu, Information);
    }
    Vmcb->Control.EventInjection = Information |
        ((ULONG64)ErrorCode << 32);
    Vmcb->Control.VmcbClean = 0;
}

static VOID
AmdInjectInvalidOpcode(
    _Inout_ AMD_VMCB* Vmcb,
    _In_ const HV_CPU* Cpu
    )
{
    AmdInjectException(Vmcb, Cpu, AMD_EVENT_INJECT_UD, 0);
}

static BOOLEAN
AmdIsPrivateHypercall(
    _In_ const AMD_GUEST_REGISTERS* Registers,
    _In_ const AMD_VMCB* Vmcb
    )
{
    return Vmcb->State.Cpl == 0 &&
           Vmcb->State.Rax == HV_HYPERCALL_MAGIC_RAX &&
           Registers->Rcx == HV_HYPERCALL_MAGIC_RCX &&
           Registers->Rdx == HV_HYPERCALL_MAGIC_RDX &&
           Registers->R8 == HV_HYPERCALL_MAGIC_R8;
}

static VOID
AmdApplyCpuidPolicy(
    _In_ ULONG Leaf,
    _In_ ULONG Subleaf,
    _Out_writes_(4) int Result[4]
    )
{
    __cpuidex(Result, (int)Leaf, (int)Subleaf);

    if (Leaf == 1) {
        Result[2] &= (int)~((1u << 31) | (1u << 5));
        NT_ASSERT((((ULONG)Result[2]) & ((1u << 31) | (1u << 5))) == 0);
    } else if (Leaf == 0x80000001u) {
        Result[2] &= (int)~(1u << 2);
        NT_ASSERT((((ULONG)Result[2]) & (1u << 2)) == 0);
    } else if (Leaf == 0x8000000Au) {
        RtlZeroMemory(Result, sizeof(int) * 4);
    }
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

    if (Registers == NULL || Cpu == NULL || Cpu->VendorContext == NULL) {
        KeBugCheckEx(HYPERVISOR_ERROR, AMD_BUGCHECK_UNEXPECTED_EXIT,
            MAXULONG, 0, 0);
    }
    context = (AMD_CPU_CONTEXT*)Cpu->VendorContext;
    vmcb = context->Vmcb;
    exitCode = vmcb->Control.ExitCode;
    AmdPrepareTlbControl(context);
    vmcb->Control.EventInjection =
        (vmcb->Control.ExitInterruptInfo & AMD_EVENT_VALID) != 0
        ? vmcb->Control.ExitInterruptInfo : 0;

    if (exitCode == AMD_EXIT_INVALID &&
        InterlockedCompareExchange(&Cpu->State, 0, 0) ==
            HV_CPU_STARTING) {
        context->Virtualized = FALSE;
        return AMD_VMEXIT_STOP;
    }

    if (exitCode == AMD_EXIT_CPUID) {
        ULONG leaf = (ULONG)vmcb->State.Rax;
        ULONG subleaf = (ULONG)Registers->Rcx;
        int cpuid[4];

        AmdApplyCpuidPolicy(leaf, subleaf, cpuid);
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
                AmdInjectException(vmcb, Cpu, AMD_EVENT_INJECT_GP, 0);
                return AMD_VMEXIT_RESUME;
            }
            vmcb->State.Rax = (ULONG)msrValue;
            Registers->Rdx = (ULONG)(msrValue >> 32);
        } else {
            msrValue = ((ULONG64)(ULONG)Registers->Rdx << 32) |
                       (ULONG)vmcb->State.Rax;
            if (msr == AMD_MSR_EFER) {
                const ULONG64 validEfer = 0x000000000036FD01ull;
                if ((msrValue & ~validEfer) != 0 ||
                    (((msrValue ^ context->GuestEfer) & (1ull << 8)) != 0 &&
                     (vmcb->State.Cr0 & (1ull << 31)) != 0)) {
                    AmdInjectException(vmcb, Cpu, AMD_EVENT_INJECT_GP, 0);
                    return AMD_VMEXIT_RESUME;
                }
                context->GuestEfer =
                    (msrValue & ~(1ull << 10)) |
                    (context->GuestEfer & (1ull << 10));
                vmcb->State.Efer = context->GuestEfer | AMD_EFER_SVME;
            } else if (msr == AMD_MSR_VM_CR) {
                if ((msrValue & ~0x1full) != 0) {
                    AmdInjectException(vmcb, Cpu, AMD_EVENT_INJECT_GP, 0);
                    return AMD_VMEXIT_RESUME;
                }
                if ((context->GuestVmCr & AMD_VM_CR_LOCK) != 0) {
                    msrValue = (msrValue & ~AMD_VM_CR_LOCK_SVMDIS) |
                               (context->GuestVmCr & AMD_VM_CR_LOCK_SVMDIS);
                }
                context->GuestVmCr = msrValue;
            } else if (msr == AMD_MSR_VM_HSAVE_PA) {
                if ((msrValue & (PAGE_SIZE - 1)) != 0 ||
                    msrValue >= HvX86GetPhysicalAddressLimit()) {
                    AmdInjectException(vmcb, Cpu, AMD_EVENT_INJECT_GP, 0);
                    return AMD_VMEXIT_RESUME;
                }
                context->GuestHostSavePhysical = msrValue;
            } else {
                AmdInjectException(vmcb, Cpu, AMD_EVENT_INJECT_GP, 0);
                return AMD_VMEXIT_RESUME;
            }
        }
        vmcb->State.Rip = vmcb->Control.NextRip;
        vmcb->Control.VmcbClean = 0;
        return AMD_VMEXIT_RESUME;
    }

    if (exitCode == AMD_EXIT_NPF) {
        KeBugCheckEx(
            HYPERVISOR_ERROR,
            AMD_BUGCHECK_NPF,
            Cpu->ProcessorIndex,
            (ULONG_PTR)vmcb->Control.ExitInfo1,
            (ULONG_PTR)vmcb->Control.ExitInfo2);
    }

    if (exitCode == AMD_EXIT_INVLPGA) {
        AmdInjectInvalidOpcode(vmcb, Cpu);
        return AMD_VMEXIT_RESUME;
    }

    if (exitCode == AMD_EXIT_XSETBV) {
        AmdInjectException(vmcb, Cpu, AMD_EVENT_INJECT_GP, 0);
        return AMD_VMEXIT_RESUME;
    }

    if (exitCode == AMD_EXIT_VMMCALL) {
        if (!AmdIsPrivateHypercall(Registers, vmcb)) {
            AmdInjectInvalidOpcode(vmcb, Cpu);
            return AMD_VMEXIT_RESUME;
        }
        if (Registers->R9 == context->StopCookie) {
            context->ResumeRsp = vmcb->State.Rsp;
            context->ResumeRip = vmcb->Control.NextRip;
            context->Virtualized = FALSE;
            return AMD_VMEXIT_STOP;
        }
        if (Registers->R9 == HV_HYPERCALL_SLAT_R9) {
            vmcb->State.Rip = vmcb->Control.NextRip;
            vmcb->Control.VmcbClean = 0;
            return AMD_VMEXIT_RESUME;
        }
        AmdInjectInvalidOpcode(vmcb, Cpu);
        return AMD_VMEXIT_RESUME;
    }

    if (exitCode >= AMD_EXIT_VMRUN && exitCode <= AMD_EXIT_SKINIT) {
        AmdInjectInvalidOpcode(vmcb, Cpu);
        return AMD_VMEXIT_RESUME;
    }

    KeBugCheckEx(HYPERVISOR_ERROR, AMD_BUGCHECK_UNEXPECTED_EXIT,
        Cpu->ProcessorIndex, (ULONG_PTR)exitCode, vmcb->State.Rip);
}
