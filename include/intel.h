#pragma once

#include "hv.h"

#define INTEL_HOST_STACK_SIZE       (16u * 1024u)

typedef enum _INTEL_VMEXIT_ACTION {
    INTEL_VMEXIT_RESUME = 0,
    INTEL_VMEXIT_STOP = 1
} INTEL_VMEXIT_ACTION;

typedef struct _INTEL_GUEST_REGISTERS {
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
} INTEL_GUEST_REGISTERS;

typedef struct _INTEL_CPU_CONTEXT {
    PVOID Vmxon;
    PVOID Vmcs;
    PVOID HostStack;
    PHYSICAL_ADDRESS VmxonPhysical;
    PHYSICAL_ADDRESS VmcsPhysical;
    ULONG64 OriginalCr0;
    ULONG64 OriginalCr4;
    ULONG64 ResumeRsp;
    ULONG64 ResumeRip;
    BOOLEAN VmxOn;
    BOOLEAN Launched;
    PVOID MsrBitmap;
    PVOID IoBitmapA;
    PVOID IoBitmapB;
    PVOID BackendContext;
    PHYSICAL_ADDRESS MsrBitmapPhysical;
    PHYSICAL_ADDRESS IoBitmapAPhysical;
    PHYSICAL_ADDRESS IoBitmapBPhysical;
    ULONG64 EptPointer;
    volatile LONG64 SlatGeneration;
    ULONG64 StopCookie;
    ULONG LastVmxError;
    USHORT Vpid;
} INTEL_CPU_CONTEXT;

C_ASSERT(FIELD_OFFSET(HV_CPU, VendorContext) == 16);
C_ASSERT(FIELD_OFFSET(INTEL_CPU_CONTEXT, ResumeRsp) == 56);
C_ASSERT(FIELD_OFFSET(INTEL_CPU_CONTEXT, ResumeRip) == 64);

NTSTATUS
IntelSetLaunchState(
    _In_ ULONG64 GuestRsp,
    _In_ ULONG64 GuestRip
    );

ULONG
IntelVmExitHandler(
    _Inout_ INTEL_GUEST_REGISTERS* Registers,
    _Inout_ HV_CPU* Cpu
    );

ULONG
IntelAsmLaunch(
    VOID
    );

VOID
IntelAsmStop(
    _In_ ULONG64 StopCookie
    );

ULONG IntelAsmInvept(_In_ ULONG Type, _In_reads_bytes_(16) const PVOID Descriptor);
ULONG IntelAsmInvvpid(_In_ ULONG Type, _In_reads_bytes_(16) const PVOID Descriptor);

VOID
IntelAsmVmExit(
    VOID
    );

USHORT IntelAsmReadEs(VOID);
USHORT IntelAsmReadCs(VOID);
USHORT IntelAsmReadSs(VOID);
USHORT IntelAsmReadDs(VOID);
USHORT IntelAsmReadFs(VOID);
USHORT IntelAsmReadGs(VOID);
USHORT IntelAsmReadLdtr(VOID);
USHORT IntelAsmReadTr(VOID);
VOID IntelAsmStoreGdtr(_Out_writes_bytes_(10) PVOID Register);
VOID IntelAsmStoreIdtr(_Out_writes_bytes_(10) PVOID Register);
