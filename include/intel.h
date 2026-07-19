#pragma once

#include "hv.h"

#define INTEL_HOST_STACK_SIZE       (16u * 1024u)
#define INTEL_EXIT_HISTORY_COUNT    64u

#ifndef JOHNSMITH_DIAGNOSTICS
#if DBG
#define JOHNSMITH_DIAGNOSTICS 1
#else
#define JOHNSMITH_DIAGNOSTICS 0
#endif
#endif

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

/* VMCS host RSP addresses this fixed record for the assembly fast path. */
typedef struct _INTEL_HOST_STACK_FRAME {
    HV_CPU* Cpu;
    volatile LONG64* CpuSlatGeneration;
    volatile LONG64* BackendSlatGeneration;
    ULONG CpuidLeaf0Eax;
    ULONG CpuidLeaf0Ebx;
    ULONG CpuidLeaf0Ecx;
    ULONG CpuidLeaf0Edx;
    ULONG64 FastPathEnabled;
    volatile LONG* RendezvousPhase;
    ULONG64 Reserved;
} INTEL_HOST_STACK_FRAME;

typedef struct _INTEL_EXIT_RECORD {
    volatile ULONG64 Sequence;
    ULONG64 GuestRip;
    ULONG64 ExitQualification;
    ULONG64 GuestLinearAddress;
    ULONG64 GuestPhysicalAddress;
    ULONG Reason;
    ULONG InstructionLength;
    ULONG ExitInterruptionInformation;
    ULONG IdtVectoringInformation;
    ULONG EntryInterruptionInformation;
    ULONG RawReason;
} INTEL_EXIT_RECORD;

typedef struct _INTEL_CPU_EPT_SPLIT {
    LIST_ENTRY Link;
    ULONG PdptIndex;
    ULONG PdIndex;
    volatile LONG ReferenceCount;
    PVOID Pt;
} INTEL_CPU_EPT_SPLIT;

typedef struct _INTEL_CPU_EPT_VIEW {
    PVOID Pml4;
    PVOID Pdpt;
    PVOID Pds[512];
    ULONG PdHookCount[512];
    LIST_ENTRY SplitList;
    ULONG64 EptPointer;
    volatile LONG64 SlatGeneration;
} INTEL_CPU_EPT_VIEW;

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

    ULONG64 GuestCr2;
    BOOLEAN VmxOn;
    BOOLEAN Launched;
    PVOID MsrBitmap;
    PVOID BackendContext;
    PHYSICAL_ADDRESS MsrBitmapPhysical;
    ULONG64 EptPointer;
    INTEL_CPU_EPT_VIEW PrimaryEpt;
    INTEL_CPU_EPT_VIEW SecondaryEpt;
    volatile LONG64 SlatGeneration;
    ULONG64 StopCookie;
    ULONG LastVmxError;
    USHORT Vpid;
    ULONG64 ResumeCr0;
    ULONG64 ResumeCr3;
    ULONG64 ResumeCr4;
    ULONG64 ResumeDr7;
    ULONG64 ResumeFsBase;
    ULONG64 ResumeGsBase;
    ULONG64 ResumePat;
    ULONG64 ResumeEfer;
    ULONG64 ResumeSysenterCs;
    ULONG64 ResumeSysenterEsp;
    ULONG64 ResumeSysenterEip;
    ULONG64 ResumeGdtrBase;
    ULONG64 ResumeIdtrBase;
    ULONG64 ResumeGdtrLimit;
    ULONG64 ResumeIdtrLimit;
    ULONG StartFailureStage;
    ULONG ControlFailureMask;
    ULONG PinControls;
    ULONG PrimaryControls;
    ULONG SecondaryControls;
    ULONG ExitControls;
    ULONG EntryControls;
    ULONG DesiredPrimaryControls;
    ULONG DesiredSecondaryControls;
    ULONG RequiredSecondaryControls;
    ULONG RequiredExitControls;
    ULONG RequiredEntryControls;
    ULONG CpuidClearLeaf7Ebx;
    ULONG CpuidClearLeafDEax;
    ULONG CpuidClearLeaf80000001Edx;
    ULONG CpuidLeaf0Eax;
    ULONG CpuidLeaf0Ebx;
    ULONG CpuidLeaf0Ecx;
    ULONG CpuidLeaf0Edx;
    ULONG ClampedCpuidEax;
    ULONG ClampedCpuidEbx;
    ULONG ClampedCpuidEcx;
    ULONG ClampedCpuidEdx;
    EX_RUNDOWN_REF HypercallPageRundown;
    PMDL HypercallPageMdl;
    PVOID HypercallSharedPage;
    PEPROCESS HypercallOwnerProcess;
    BOOLEAN HypercallPageRegistered;
    BOOLEAN Hypercall1GbPagesSupported;
    volatile LONG64 ExitSequence;
    volatile LONG64 CompletedExitSequence;
    ULONG64 LastExitEntryTsc;
    ULONG64 LastExitCompletionTsc;

    volatile LONG PendingNmi;
    BOOLEAN VirtualNmiEnabled;
    volatile LONG PendingInterruptValid;
    UCHAR PendingInterruptVector;
    ULONG ProcessorIndex;
    volatile LONG HookRendezvousBudget;
    volatile LONG RendezvousJoinGuard;
    volatile LONG64 RendezvousJoinedEpoch;
    volatile LONG64 RendezvousPreparedEpoch;
    volatile LONG64 RendezvousAppliedEpoch;
    volatile LONG64 RendezvousExpectedNmiEpoch;
    volatile LONG64 RendezvousConsumedNmiEpoch;
    volatile LONG64 RendezvousOwnedEpoch;
    ULONG64 TscOffset;
    INTEL_EXIT_RECORD ExitHistory[INTEL_EXIT_HISTORY_COUNT];
} INTEL_CPU_CONTEXT;

C_ASSERT(FIELD_OFFSET(HV_CPU, VendorContext) == 16);
C_ASSERT(sizeof(INTEL_HOST_STACK_FRAME) == 64);
C_ASSERT(FIELD_OFFSET(INTEL_HOST_STACK_FRAME, Cpu) == 0);
C_ASSERT(FIELD_OFFSET(INTEL_HOST_STACK_FRAME, CpuSlatGeneration) == 8);
C_ASSERT(FIELD_OFFSET(INTEL_HOST_STACK_FRAME, BackendSlatGeneration) == 16);
C_ASSERT(FIELD_OFFSET(INTEL_HOST_STACK_FRAME, CpuidLeaf0Eax) == 24);
C_ASSERT(FIELD_OFFSET(INTEL_HOST_STACK_FRAME, FastPathEnabled) == 40);
C_ASSERT(FIELD_OFFSET(INTEL_HOST_STACK_FRAME, RendezvousPhase) == 48);
C_ASSERT(FIELD_OFFSET(INTEL_CPU_CONTEXT, ResumeRsp) == 56);
C_ASSERT(FIELD_OFFSET(INTEL_CPU_CONTEXT, ResumeRip) == 64);
C_ASSERT(FIELD_OFFSET(INTEL_CPU_CONTEXT, GuestCr2) == 72);
C_ASSERT((INTEL_EXIT_HISTORY_COUNT & (INTEL_EXIT_HISTORY_COUNT - 1)) == 0);
C_ASSERT(sizeof(INTEL_EXIT_RECORD) == 64);
C_ASSERT((FIELD_OFFSET(INTEL_CPU_CONTEXT, ExitSequence) & 7) == 0);
C_ASSERT((FIELD_OFFSET(INTEL_CPU_CONTEXT, CompletedExitSequence) & 7) == 0);
C_ASSERT((FIELD_OFFSET(INTEL_CPU_CONTEXT, RendezvousJoinGuard) & 3) == 0);
C_ASSERT((FIELD_OFFSET(INTEL_CPU_CONTEXT, RendezvousJoinedEpoch) & 7) == 0);
C_ASSERT((FIELD_OFFSET(INTEL_CPU_CONTEXT, TscOffset) & 7) == 0);

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
VOID IntelAsmLoadGdtr(_In_reads_bytes_(10) const VOID* Register);
VOID IntelAsmLoadIdtr(_In_reads_bytes_(10) const VOID* Register);
