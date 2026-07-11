#include "amd_internal.h"

#pragma pack(push, 1)
typedef struct _AMD_DESCRIPTOR_TABLE_REGISTER {
    USHORT Limit;
    ULONG64 Base;
} AMD_DESCRIPTOR_TABLE_REGISTER;
#pragma pack(pop)

static NTSTATUS
AmdCaptureSegment(
    _In_ USHORT Selector,
    _In_ const AMD_DESCRIPTOR_TABLE_REGISTER* Gdtr,
    _In_ ULONG MsrBase,
    _Out_ AMD_VMCB_SEGMENT* Segment
    )
{
    ULONG offset = Selector & ~7u;
    ULONG64 descriptor;
    ULONG64 base;
    ULONG64 limit;

    RtlZeroMemory(Segment, sizeof(*Segment));
    Segment->Selector = Selector;
    if (Selector == 0) return STATUS_SUCCESS;
    if ((Selector & 4) != 0 || offset > Gdtr->Limit ||
        Gdtr->Limit - offset < sizeof(ULONG64) - 1) {
        return STATUS_DATA_ERROR;
    }
    descriptor = *(UNALIGNED const ULONG64*)(Gdtr->Base + offset);
    base = ((descriptor >> 16) & 0xffffull) |
           ((descriptor >> 32) & 0xff0000ull) |
           ((descriptor >> 56) & 0xff000000ull);
    if (((descriptor >> 44) & 1) == 0) {
        if (Gdtr->Limit - offset < 11) return STATUS_DATA_ERROR;
        base |= (ULONG64)*(UNALIGNED const ULONG*)(Gdtr->Base + offset + 8)
                << 32;
    }
    limit = (descriptor & 0xffff) | ((descriptor >> 32) & 0xf0000);
    if ((descriptor & (1ull << 55)) != 0) {
        limit = (limit << PAGE_SHIFT) | (PAGE_SIZE - 1);
    }
    Segment->Limit = (ULONG)limit;
    Segment->Attributes =
        (USHORT)(((descriptor >> 40) & 0xff) |
                 (((descriptor >> 52) & 0xf) << 8));
    Segment->Base = MsrBase != 0 ? __readmsr(MsrBase) : base;
    return STATUS_SUCCESS;
}

NTSTATUS
AmdSetupVmcb(
    _Inout_ HV_STATE* State,
    _Inout_ HV_CPU* Cpu
    )
{
    AMD_BACKEND_CONTEXT* backend =
        (AMD_BACKEND_CONTEXT*)State->BackendContext;
    AMD_CPU_CONTEXT* context = (AMD_CPU_CONTEXT*)Cpu->VendorContext;
    AMD_VMCB* vmcb = context->Vmcb;
    AMD_DESCRIPTOR_TABLE_REGISTER gdtr;
    AMD_DESCRIPTOR_TABLE_REGISTER idtr;
    NTSTATUS status;

    RtlZeroMemory(vmcb, PAGE_SIZE);
    AmdAsmStoreGdtr(&gdtr);
    AmdAsmStoreIdtr(&idtr);
    status = AmdCaptureSegment(AmdAsmReadEs(), &gdtr, 0, &vmcb->State.Es);
    if (!NT_SUCCESS(status)) return status;
    status = AmdCaptureSegment(AmdAsmReadCs(), &gdtr, 0, &vmcb->State.Cs);
    if (!NT_SUCCESS(status)) return status;
    status = AmdCaptureSegment(AmdAsmReadSs(), &gdtr, 0, &vmcb->State.Ss);
    if (!NT_SUCCESS(status)) return status;
    status = AmdCaptureSegment(AmdAsmReadDs(), &gdtr, 0, &vmcb->State.Ds);
    if (!NT_SUCCESS(status)) return status;
    status = AmdCaptureSegment(
        AmdAsmReadFs(), &gdtr, AMD_MSR_FS_BASE, &vmcb->State.Fs);
    if (!NT_SUCCESS(status)) return status;
    status = AmdCaptureSegment(
        AmdAsmReadGs(), &gdtr, AMD_MSR_GS_BASE, &vmcb->State.Gs);
    if (!NT_SUCCESS(status)) return status;
    status = AmdCaptureSegment(AmdAsmReadLdtr(), &gdtr, 0, &vmcb->State.Ldtr);
    if (!NT_SUCCESS(status)) return status;
    status = AmdCaptureSegment(AmdAsmReadTr(), &gdtr, 0, &vmcb->State.Tr);
    if (!NT_SUCCESS(status)) return status;

    vmcb->State.Gdtr.Limit = gdtr.Limit;
    vmcb->State.Gdtr.Base = gdtr.Base;
    vmcb->State.Idtr.Limit = idtr.Limit;
    vmcb->State.Idtr.Base = idtr.Base;
    vmcb->State.Cpl = (UCHAR)(vmcb->State.Cs.Selector & 3);
    vmcb->State.Efer = context->GuestEfer | AMD_EFER_SVME;
    vmcb->State.Cr4 = __readcr4();
    vmcb->State.Cr3 = __readcr3();
    vmcb->State.Cr0 = __readcr0();
    vmcb->State.Dr7 = __readdr(7);
    vmcb->State.Dr6 = __readdr(6);
    vmcb->State.Rflags = __readeflags();
    vmcb->State.Cr2 = __readcr2();
    vmcb->State.GPat = __readmsr(AMD_MSR_PAT);
    vmcb->State.DebugCtl = __readmsr(AMD_MSR_DEBUGCTL);
    vmcb->State.Star = __readmsr(AMD_MSR_STAR);
    vmcb->State.Lstar = __readmsr(AMD_MSR_LSTAR);
    vmcb->State.Cstar = __readmsr(AMD_MSR_CSTAR);
    vmcb->State.Sfmask = __readmsr(AMD_MSR_SFMASK);
    vmcb->State.KernelGsBase = __readmsr(AMD_MSR_KERNEL_GS_BASE);
    vmcb->State.SysenterCs = __readmsr(AMD_MSR_SYSENTER_CS);
    vmcb->State.SysenterEsp = __readmsr(AMD_MSR_SYSENTER_ESP);
    vmcb->State.SysenterEip = __readmsr(AMD_MSR_SYSENTER_EIP);
    vmcb->State.Rax = 0;

    vmcb->Control.InterceptMisc1 =
        AMD_INTERCEPT_CPUID | AMD_INTERCEPT_IOIO | AMD_INTERCEPT_MSR;
    vmcb->Control.InterceptMisc2 = AMD_INTERCEPT_SVM_INSTRUCTIONS;
    context->GuestAsid = Cpu->ProcessorIndex + 1;
    if (context->GuestAsid == 0 ||
        context->GuestAsid >= backend->MaxAsid) {
        context->GuestAsid = 1;
    }
    vmcb->Control.GuestAsid = context->GuestAsid;
    vmcb->Control.TlbControl = 1;
    vmcb->Control.IopmBase = backend->IopmPhysical.QuadPart;
    vmcb->Control.MsrpmBase = context->MsrpmPhysical.QuadPart;
    vmcb->Control.VirtualInterrupt = 1ull << 24;
    vmcb->Control.NestedPagingEnable = 1;
    vmcb->Control.NestedCr3 =
        (ULONG64)MmGetPhysicalAddress(backend->Pml4).QuadPart &
        NPT_ADDRESS_MASK;
    vmcb->Control.VmcbClean = 0;
    return STATUS_SUCCESS;
}

