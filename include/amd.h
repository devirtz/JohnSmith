#pragma once

#include "hv.h"

#define AMD_HOST_STACK_SIZE         (16u * 1024u)

#pragma pack(push, 1)
typedef struct _AMD_VMCB_SEGMENT {
    USHORT Selector;
    USHORT Attributes;
    ULONG Limit;
    ULONG64 Base;
} AMD_VMCB_SEGMENT;

typedef struct _AMD_VMCB_CONTROL {
    USHORT InterceptCrRead;
    USHORT InterceptCrWrite;
    USHORT InterceptDrRead;
    USHORT InterceptDrWrite;
    ULONG InterceptExceptions;
    ULONG InterceptMisc1;
    ULONG InterceptMisc2;
    UCHAR Reserved014[0x3C - 0x14];
    USHORT PauseFilterThreshold;
    USHORT PauseFilterCount;
    ULONG64 IopmBase;
    ULONG64 MsrpmBase;
    ULONG64 TscOffset;
    ULONG GuestAsid;
    UCHAR TlbControl;
    UCHAR Reserved05D[3];
    ULONG64 VirtualInterrupt;
    ULONG64 InterruptShadow;
    ULONG64 ExitCode;
    ULONG64 ExitInfo1;
    ULONG64 ExitInfo2;
    ULONG64 ExitInterruptInfo;
    ULONG64 NestedPagingEnable;
    ULONG64 AvicApicBar;
    ULONG64 GuestPaOfGhcb;
    ULONG64 EventInjection;
    ULONG64 NestedCr3;
    ULONG64 LbrVirtualizationEnable;
    ULONG VmcbClean;
    ULONG Reserved0CC;
    ULONG64 NextRip;
    UCHAR NumberOfBytesFetched;
    UCHAR GuestInstructionBytes[15];
    UCHAR Reserved0E0[0x400 - 0xE0];
} AMD_VMCB_CONTROL;

typedef struct _AMD_VMCB_STATE {
    AMD_VMCB_SEGMENT Es;
    AMD_VMCB_SEGMENT Cs;
    AMD_VMCB_SEGMENT Ss;
    AMD_VMCB_SEGMENT Ds;
    AMD_VMCB_SEGMENT Fs;
    AMD_VMCB_SEGMENT Gs;
    AMD_VMCB_SEGMENT Gdtr;
    AMD_VMCB_SEGMENT Ldtr;
    AMD_VMCB_SEGMENT Idtr;
    AMD_VMCB_SEGMENT Tr;
    UCHAR Reserved4A0[0xCB - 0xA0];
    UCHAR Cpl;
    ULONG Reserved4CC;
    ULONG64 Efer;
    UCHAR Reserved4D8[0x548 - 0x4D8];
    ULONG64 Cr4;
    ULONG64 Cr3;
    ULONG64 Cr0;
    ULONG64 Dr7;
    ULONG64 Dr6;
    ULONG64 Rflags;
    ULONG64 Rip;
    UCHAR Reserved580[0x5D8 - 0x580];
    ULONG64 Rsp;
    UCHAR Reserved5E0[0x5F8 - 0x5E0];
    ULONG64 Rax;
    ULONG64 Star;
    ULONG64 Lstar;
    ULONG64 Cstar;
    ULONG64 Sfmask;
    ULONG64 KernelGsBase;
    ULONG64 SysenterCs;
    ULONG64 SysenterEsp;
    ULONG64 SysenterEip;
    ULONG64 Cr2;
    UCHAR Reserved648[0x668 - 0x648];
    ULONG64 GPat;
    ULONG64 DebugCtl;
    ULONG64 LastBranchFrom;
    ULONG64 LastBranchTo;
    ULONG64 LastExceptionFrom;
    ULONG64 LastExceptionTo;
    UCHAR Reserved698[0xC00 - 0x298];
} AMD_VMCB_STATE;

typedef struct _AMD_VMCB {
    AMD_VMCB_CONTROL Control;
    AMD_VMCB_STATE State;
} AMD_VMCB;
#pragma pack(pop)

typedef enum _AMD_VMEXIT_ACTION {
    AMD_VMEXIT_RESUME = 0,
    AMD_VMEXIT_STOP = 1
} AMD_VMEXIT_ACTION;

typedef struct _AMD_GUEST_REGISTERS {
    ULONG64 Rax;
    ULONG64 Rcx;
    ULONG64 Rdx;
    ULONG64 Rbx;
    ULONG64 Rbp;
    ULONG64 Rsi;
    ULONG64 Rdi;
    ULONG64 R8;
    ULONG64 R9;
    ULONG64 R10;
    ULONG64 R11;
    ULONG64 R12;
    ULONG64 R13;
    ULONG64 R14;
    ULONG64 R15;
} AMD_GUEST_REGISTERS;

typedef struct _AMD_CPU_CONTEXT {
    AMD_VMCB* Vmcb;
    AMD_VMCB* HostVmcb;
    PVOID HostSave;
    PVOID HostStack;
    PHYSICAL_ADDRESS VmcbPhysical;
    PHYSICAL_ADDRESS HostVmcbPhysical;
    PHYSICAL_ADDRESS HostSavePhysical;
    ULONG64 OriginalEfer;
    ULONG64 OriginalHostSavePhysical;
    ULONG64 ResumeRsp;
    ULONG64 ResumeRip;
    BOOLEAN Virtualized;
    PVOID Msrpm;
    PVOID BackendContext;
    PHYSICAL_ADDRESS MsrpmPhysical;
    volatile LONG64 SlatGeneration;
    ULONG64 StopCookie;
    ULONG64 GuestEfer;
    ULONG64 GuestVmCr;
    ULONG64 GuestHostSavePhysical;
    ULONG GuestAsid;
} AMD_CPU_CONTEXT;

C_ASSERT(sizeof(AMD_VMCB) == PAGE_SIZE);
C_ASSERT(FIELD_OFFSET(AMD_VMCB, Control.EventInjection) == 0xA8);
C_ASSERT(FIELD_OFFSET(AMD_VMCB, Control.NestedCr3) == 0xB0);
C_ASSERT(FIELD_OFFSET(AMD_VMCB, Control.NextRip) == 0xC8);
C_ASSERT(FIELD_OFFSET(AMD_VMCB, State.Efer) == 0x4D0);
C_ASSERT(FIELD_OFFSET(AMD_VMCB, State.Rip) == 0x578);
C_ASSERT(FIELD_OFFSET(AMD_VMCB, State.Rsp) == 0x5D8);
C_ASSERT(FIELD_OFFSET(AMD_VMCB, State.Rax) == 0x5F8);
C_ASSERT(FIELD_OFFSET(HV_CPU, VendorContext) == 16);
C_ASSERT(FIELD_OFFSET(AMD_CPU_CONTEXT, ResumeRsp) == 72);
C_ASSERT(FIELD_OFFSET(AMD_CPU_CONTEXT, ResumeRip) == 80);

ULONG
AmdVmExitHandler(
    _Inout_ AMD_GUEST_REGISTERS* Registers,
    _Inout_ HV_CPU* Cpu
    );

ULONG
AmdAsmLaunch(
    _Inout_ AMD_VMCB* Vmcb,
    _In_ ULONG64 VmcbPhysical,
    _In_ ULONG64 HostRsp,
    _Inout_ HV_CPU* Cpu,
    _In_ ULONG64 HostVmcbPhysical
    );

VOID
AmdAsmStop(
    _In_ ULONG64 StopCookie
    );

USHORT AmdAsmReadEs(VOID);
USHORT AmdAsmReadCs(VOID);
USHORT AmdAsmReadSs(VOID);
USHORT AmdAsmReadDs(VOID);
USHORT AmdAsmReadFs(VOID);
USHORT AmdAsmReadGs(VOID);
USHORT AmdAsmReadLdtr(VOID);
USHORT AmdAsmReadTr(VOID);
VOID AmdAsmStoreGdtr(_Out_writes_bytes_(10) PVOID Register);
VOID AmdAsmStoreIdtr(_Out_writes_bytes_(10) PVOID Register);
