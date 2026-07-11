#pragma once

#include "amd.h"

#include <intrin.h>

#define AMD_MSR_EFER                    0xC0000080u
#define AMD_MSR_STAR                    0xC0000081u
#define AMD_MSR_LSTAR                   0xC0000082u
#define AMD_MSR_CSTAR                   0xC0000083u
#define AMD_MSR_SFMASK                  0xC0000084u
#define AMD_MSR_FS_BASE                 0xC0000100u
#define AMD_MSR_GS_BASE                 0xC0000101u
#define AMD_MSR_KERNEL_GS_BASE          0xC0000102u
#define AMD_MSR_SYSENTER_CS             0x00000174u
#define AMD_MSR_SYSENTER_ESP            0x00000175u
#define AMD_MSR_SYSENTER_EIP            0x00000176u
#define AMD_MSR_DEBUGCTL                0x000001D9u
#define AMD_MSR_PAT                     0x00000277u
#define AMD_MSR_VM_CR                   0xC0010114u
#define AMD_MSR_VM_HSAVE_PA             0xC0010117u

#define AMD_EFER_SVME                   (1ull << 12)
#define AMD_VM_CR_SVMDIS                (1ull << 4)

#define AMD_SVM_FEATURE_NPT             (1u << 0)
#define AMD_SVM_FEATURE_NRIPS           (1u << 3)

#define AMD_INTERCEPT_CPUID             (1u << 18)
#define AMD_INTERCEPT_IOIO              (1u << 27)
#define AMD_INTERCEPT_MSR               (1u << 28)
#define AMD_INTERCEPT_SVM_INSTRUCTIONS  0x7fu
#define AMD_EXIT_CPUID                  0x72ull
#define AMD_EXIT_VMRUN                  0x80ull
#define AMD_EXIT_VMMCALL                0x81ull
#define AMD_EXIT_SKINIT                 0x86ull
#define AMD_EXIT_MSR                    0x7Cull
#define AMD_EXIT_NPF                    0x400ull
#define AMD_EVENT_INJECT_UD             0x80000306ull
#define AMD_EVENT_INJECT_GP             0x80000B0Dull
#define AMD_EVENT_INJECT_PF             0x80000B0Eull
#define AMD_BUGCHECK_UNEXPECTED_EXIT    0x41564D43u
#define AMD_BUGCHECK_INVALIDATION       0x414E5054u

#define AMD_IOPM_SIZE                   (3u * PAGE_SIZE)
#define AMD_MSRPM_SIZE                  (2u * PAGE_SIZE)
#define AMD_VMCB_CLEAN_ASID             (1u << 2)

#define NPT_PRESENT                     (1ull << 0)
#define NPT_WRITE                       (1ull << 1)
#define NPT_USER                        (1ull << 2)
#define NPT_PWT                         (1ull << 3)
#define NPT_PCD                         (1ull << 4)
#define NPT_LARGE_PAGE                  (1ull << 7)
#define NPT_NO_EXECUTE                  (1ull << 63)
#define NPT_ADDRESS_MASK                0x000FFFFFFFFFF000ull
#define NPT_2MB_ADDRESS_MASK            0x000FFFFFFFE00000ull
#define NPT_TABLE_PERMISSIONS           (NPT_PRESENT | NPT_WRITE | NPT_USER)

typedef struct _AMD_SLAT_SPLIT {
    LIST_ENTRY Link;
    ULONG PdptIndex;
    ULONG PdIndex;
    PVOID Pt;
} AMD_SLAT_SPLIT;

typedef struct _AMD_BACKEND_CONTEXT {
    PVOID Pml4;
    PVOID Pdpt;
    PVOID Iopm;
    PHYSICAL_ADDRESS IopmPhysical;
    PVOID Pds[512];
    LIST_ENTRY SplitList;
    EX_PUSH_LOCK SlatLock;
    ULONG64 MapLimit;
    ULONG MaxAsid;
    volatile LONG64 SlatGeneration;
} AMD_BACKEND_CONTEXT;

PVOID
AmdAllocateContiguous(
    _In_ SIZE_T Size
    );

PVOID
AmdAllocatePage(
    VOID
    );

NTSTATUS
AmdBuildNpt(
    _Inout_ AMD_BACKEND_CONTEXT* Context
    );

VOID
AmdFreeNpt(
    _Inout_ AMD_BACKEND_CONTEXT* Context
    );

VOID
AmdPrepareTlbControl(
    _Inout_ AMD_CPU_CONTEXT* Context
    );

NTSTATUS
AmdQueryOwnedPageAccess(
    _Inout_ HV_STATE* State,
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _Out_ HV_PAGE_ACCESS* Access
    );

NTSTATUS
AmdSetOwnedPageAccess(
    _Inout_ HV_STATE* State,
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ HV_PAGE_ACCESS Access,
    _Out_ HV_PAGE_ACCESS* PreviousAccess
    );

NTSTATUS
AmdSetupVmcb(
    _Inout_ HV_STATE* State,
    _Inout_ HV_CPU* Cpu
    );
