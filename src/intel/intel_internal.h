#pragma once

#include "intel.h"

#include <intrin.h>

#define IA32_FEATURE_CONTROL                0x0000003Au
#define IA32_SYSENTER_CS                    0x00000174u
#define IA32_SYSENTER_ESP                   0x00000175u
#define IA32_SYSENTER_EIP                   0x00000176u
#define IA32_DEBUGCTL                       0x000001D9u
#define IA32_PAT                            0x00000277u
#define IA32_S_CET                          0x000006A2u
#define IA32_EFER                           0xC0000080u
#define IA32_FS_BASE                        0xC0000100u
#define IA32_GS_BASE                        0xC0000101u

#define IA32_VMX_BASIC                      0x00000480u
#define IA32_VMX_PINBASED_CTLS              0x00000481u
#define IA32_VMX_PROCBASED_CTLS             0x00000482u
#define IA32_VMX_EXIT_CTLS                  0x00000483u
#define IA32_VMX_ENTRY_CTLS                 0x00000484u
#define IA32_VMX_CR0_FIXED0                 0x00000486u
#define IA32_VMX_CR0_FIXED1                 0x00000487u
#define IA32_VMX_CR4_FIXED0                 0x00000488u
#define IA32_VMX_CR4_FIXED1                 0x00000489u
#define IA32_VMX_PROCBASED_CTLS2            0x0000048Bu
#define IA32_VMX_EPT_VPID_CAP               0x0000048Cu
#define IA32_VMX_TRUE_PINBASED_CTLS         0x0000048Du
#define IA32_VMX_TRUE_PROCBASED_CTLS        0x0000048Eu
#define IA32_VMX_TRUE_EXIT_CTLS             0x0000048Fu
#define IA32_VMX_TRUE_ENTRY_CTLS            0x00000490u
#define IA32_VMX_VMFUNC                     0x00000491u

#define VMCS_VPID                           0x00000000u
#define VMCS_GUEST_ES_SELECTOR              0x00000800u
#define VMCS_GUEST_CS_SELECTOR              0x00000802u
#define VMCS_GUEST_SS_SELECTOR              0x00000804u
#define VMCS_GUEST_DS_SELECTOR              0x00000806u
#define VMCS_GUEST_FS_SELECTOR              0x00000808u
#define VMCS_GUEST_GS_SELECTOR              0x0000080Au
#define VMCS_GUEST_LDTR_SELECTOR            0x0000080Cu
#define VMCS_GUEST_TR_SELECTOR              0x0000080Eu
#define VMCS_HOST_ES_SELECTOR               0x00000C00u
#define VMCS_HOST_CS_SELECTOR               0x00000C02u
#define VMCS_HOST_SS_SELECTOR               0x00000C04u
#define VMCS_HOST_DS_SELECTOR               0x00000C06u
#define VMCS_HOST_FS_SELECTOR               0x00000C08u
#define VMCS_HOST_GS_SELECTOR               0x00000C0Au
#define VMCS_HOST_TR_SELECTOR               0x00000C0Cu

#define VMCS_MSR_BITMAP                     0x00002004u
#define VMCS_TSC_OFFSET                     0x00002010u
#define VMCS_EPT_POINTER                    0x0000201Au
#define VMCS_XSS_EXITING_BITMAP             0x0000202Cu
#define VMCS_GUEST_PHYSICAL_ADDRESS         0x00002400u
#define VMCS_GUEST_VMCS_LINK_POINTER        0x00002800u
#define VMCS_GUEST_DEBUGCTL                 0x00002802u
#define VMCS_GUEST_PAT                      0x00002804u
#define VMCS_GUEST_EFER                     0x00002806u

#define VMCS_HOST_PAT                       0x00002C00u
#define VMCS_HOST_EFER                      0x00002C02u

#define VMCS_PIN_BASED_CONTROLS             0x00004000u
#define VMCS_PRIMARY_PROCESSOR_CONTROLS     0x00004002u
#define VMCS_EXCEPTION_BITMAP               0x00004004u
#define VMCS_PAGE_FAULT_ERROR_MASK          0x00004006u
#define VMCS_PAGE_FAULT_ERROR_MATCH         0x00004008u
#define VMCS_CR3_TARGET_COUNT               0x0000400Au
#define VMCS_EXIT_CONTROLS                  0x0000400Cu
#define VMCS_EXIT_MSR_STORE_COUNT           0x0000400Eu
#define VMCS_EXIT_MSR_LOAD_COUNT            0x00004010u
#define VMCS_ENTRY_CONTROLS                 0x00004012u
#define VMCS_ENTRY_MSR_LOAD_COUNT           0x00004014u
#define VMCS_ENTRY_INTERRUPTION_INFO        0x00004016u
#define VMCS_ENTRY_EXCEPTION_ERROR          0x00004018u
#define VMCS_ENTRY_INSTRUCTION_LENGTH       0x0000401Au
#define VMCS_TPR_THRESHOLD                  0x0000401Cu
#define VMCS_SECONDARY_PROCESSOR_CONTROLS   0x0000401Eu
#define VMCS_VM_INSTRUCTION_ERROR           0x00004400u
#define VMCS_EXIT_REASON                    0x00004402u
#define VMCS_EXIT_INTERRUPTION_INFO         0x00004404u
#define VMCS_EXIT_INTERRUPTION_ERROR        0x00004406u
#define VMCS_IDT_VECTORING_INFO             0x00004408u
#define VMCS_IDT_VECTORING_ERROR            0x0000440Au
#define VMCS_EXIT_INSTRUCTION_LENGTH        0x0000440Cu

#define VMCS_GUEST_ES_LIMIT                 0x00004800u
#define VMCS_GUEST_CS_LIMIT                 0x00004802u
#define VMCS_GUEST_SS_LIMIT                 0x00004804u
#define VMCS_GUEST_DS_LIMIT                 0x00004806u
#define VMCS_GUEST_FS_LIMIT                 0x00004808u
#define VMCS_GUEST_GS_LIMIT                 0x0000480Au
#define VMCS_GUEST_LDTR_LIMIT               0x0000480Cu
#define VMCS_GUEST_TR_LIMIT                 0x0000480Eu
#define VMCS_GUEST_GDTR_LIMIT               0x00004810u
#define VMCS_GUEST_IDTR_LIMIT               0x00004812u
#define VMCS_GUEST_ES_AR                    0x00004814u
#define VMCS_GUEST_CS_AR                    0x00004816u
#define VMCS_GUEST_SS_AR                    0x00004818u
#define VMCS_GUEST_DS_AR                    0x0000481Au
#define VMCS_GUEST_FS_AR                    0x0000481Cu
#define VMCS_GUEST_GS_AR                    0x0000481Eu
#define VMCS_GUEST_LDTR_AR                  0x00004820u
#define VMCS_GUEST_TR_AR                    0x00004822u
#define VMCS_GUEST_INTERRUPTIBILITY         0x00004824u
#define VMCS_GUEST_ACTIVITY_STATE           0x00004826u
#define VMCS_GUEST_SMBASE                   0x00004828u
#define VMCS_GUEST_SYSENTER_CS              0x0000482Au
#define VMCS_HOST_SYSENTER_CS               0x00004C00u

#define VMCS_CR0_GUEST_HOST_MASK            0x00006000u
#define VMCS_CR4_GUEST_HOST_MASK            0x00006002u
#define VMCS_CR0_READ_SHADOW                0x00006004u
#define VMCS_CR4_READ_SHADOW                0x00006006u
#define VMCS_EXIT_QUALIFICATION             0x00006400u
#define VMCS_GUEST_LINEAR_ADDRESS           0x0000640Au

#define VMCS_GUEST_CR0                      0x00006800u
#define VMCS_GUEST_CR3                      0x00006802u
#define VMCS_GUEST_CR4                      0x00006804u
#define VMCS_GUEST_ES_BASE                  0x00006806u
#define VMCS_GUEST_CS_BASE                  0x00006808u
#define VMCS_GUEST_SS_BASE                  0x0000680Au
#define VMCS_GUEST_DS_BASE                  0x0000680Cu
#define VMCS_GUEST_FS_BASE                  0x0000680Eu
#define VMCS_GUEST_GS_BASE                  0x00006810u
#define VMCS_GUEST_LDTR_BASE                0x00006812u
#define VMCS_GUEST_TR_BASE                  0x00006814u
#define VMCS_GUEST_GDTR_BASE                0x00006816u
#define VMCS_GUEST_IDTR_BASE                0x00006818u
#define VMCS_GUEST_DR7                      0x0000681Au
#define VMCS_GUEST_RSP                      0x0000681Cu
#define VMCS_GUEST_RIP                      0x0000681Eu
#define VMCS_GUEST_RFLAGS                   0x00006820u
#define VMCS_GUEST_PENDING_DEBUG            0x00006822u
#define VMCS_GUEST_SYSENTER_ESP             0x00006824u
#define VMCS_GUEST_SYSENTER_EIP             0x00006826u

#define VMCS_HOST_CR0                       0x00006C00u
#define VMCS_HOST_CR3                       0x00006C02u
#define VMCS_HOST_CR4                       0x00006C04u
#define VMCS_HOST_FS_BASE                   0x00006C06u
#define VMCS_HOST_GS_BASE                   0x00006C08u
#define VMCS_HOST_TR_BASE                   0x00006C0Au
#define VMCS_HOST_GDTR_BASE                 0x00006C0Cu
#define VMCS_HOST_IDTR_BASE                 0x00006C0Eu
#define VMCS_HOST_SYSENTER_ESP              0x00006C10u
#define VMCS_HOST_SYSENTER_EIP              0x00006C12u
#define VMCS_HOST_RSP                       0x00006C14u
#define VMCS_HOST_RIP                       0x00006C16u

#define VMX_PRIMARY_ACTIVATE_SECONDARY      (1u << 31)
#define VMX_PRIMARY_USE_MSR_BITMAPS         (1u << 28)
#define VMX_PRIMARY_INTERRUPT_WINDOW        (1u << 2)
#define VMX_PRIMARY_NMI_WINDOW              (1u << 22)
#define VMX_SECONDARY_ENABLE_EPT            (1u << 1)
#define VMX_SECONDARY_ENABLE_RDTSCP         (1u << 3)
#define VMX_SECONDARY_ENABLE_VPID           (1u << 5)
#define VMX_SECONDARY_ENABLE_INVPCID        (1u << 12)
#define VMX_SECONDARY_ENABLE_XSAVES         (1u << 20)
#define VMX_EXIT_SAVE_DEBUG_CONTROLS         (1u << 2)
#define VMX_EXIT_HOST_ADDRESS_SPACE_SIZE    (1u << 9)
#define VMX_EXIT_SAVE_PAT                   (1u << 18)
#define VMX_EXIT_LOAD_PAT                   (1u << 19)
#define VMX_EXIT_SAVE_EFER                  (1u << 20)
#define VMX_EXIT_LOAD_EFER                  (1u << 21)
#define VMX_ENTRY_LOAD_DEBUG_CONTROLS        (1u << 2)
#define VMX_ENTRY_IA32E_MODE                (1u << 9)
#define VMX_ENTRY_LOAD_PAT                  (1u << 14)
#define VMX_ENTRY_LOAD_EFER                 (1u << 15)
#define VMX_CR4_VMXE                        (1ull << 13)
#define X86_CR4_DE                          (1ull << 3)
#define X86_CR4_PCIDE                       (1ull << 17)
#define X86_CR3_NOFLUSH                     (1ull << 63)
#define CPUID_CET_SS                        (1u << 7)
#define CPUID_CET_IBT                       (1u << 20)
#define IA32_DEBUGCTL_BTF                    (1ull << 1)
#define RFLAGS_TF                            (1ull << 8)
#define VMX_GUEST_BLOCKING_BY_STI            (1ull << 0)
#define VMX_GUEST_BLOCKING_BY_MOV_SS         (1ull << 1)
#define VMX_GUEST_BLOCKING_BY_NMI            (1ull << 3)
#define VMX_PENDING_DEBUG_BS                 (1ull << 14)
#define IA32_FEATURE_CONTROL_VMX_IN_SMX      (1ull << 1)
#define IA32_FEATURE_CONTROL_VMX_OUTSIDE_SMX (1ull << 2)

#define EPT_ACCESS_MASK                     0x7ull
#define EPT_MEMORY_TYPE_SHIFT               3u
#define EPT_MEMORY_TYPE_MASK                (7ull << EPT_MEMORY_TYPE_SHIFT)
#define EPT_MEMORY_TYPE_WB                  6ull
#define EPT_LARGE_PAGE                      (1ull << 7)
#define EPT_ADDRESS_MASK                    0x000FFFFFFFFFF000ull
#define EPT_2MB_ADDRESS_MASK                0x000FFFFFFFE00000ull

/* Intel SDM rev. 092, Table 28-7 (EPT-violation exit qualification). */
#define VMX_EPT_QUAL_READ                   (1ull << 0)
#define VMX_EPT_QUAL_WRITE                  (1ull << 1)
#define VMX_EPT_QUAL_EXECUTE                (1ull << 2)
#define VMX_EPT_QUAL_EPT_READABLE           (1ull << 3)
#define VMX_EPT_QUAL_EPT_WRITABLE           (1ull << 4)
#define VMX_EPT_QUAL_EPT_EXECUTABLE         (1ull << 5)
#define VMX_EPT_QUAL_GLA_VALID              (1ull << 7)
#define VMX_EPT_QUAL_TRANSLATION            (1ull << 8)

#define VMX_EXIT_EXCEPTION_OR_NMI           0u
#define VMX_EXIT_INIT_SIGNAL                3u
#define VMX_EXIT_INTERRUPT_WINDOW           7u
#define VMX_EXIT_NMI_WINDOW                 8u
#define VMX_EXIT_CPUID                      10u
#define VMX_EXIT_RDTSC                      16u
#define VMX_EXIT_VMCALL                     18u
#define VMX_EXIT_CR_ACCESS                  28u
#define VMX_EXIT_DR_ACCESS                  29u
#define VMX_EXIT_RDMSR                      31u
#define VMX_EXIT_WRMSR                      32u
#define VMX_EXIT_EPT_VIOLATION              48u
#define VMX_EXIT_EPT_MISCONFIGURATION       49u
#define VMX_EXIT_RDTSCP                     51u
#define VMX_EXIT_XSETBV                     55u
#define VMX_EXIT_REASON_ENTRY_FAILURE       (1u << 31)
#define VMX_ENTRY_INJECT_UD                 0x80000306u
#define VMX_ENTRY_INJECT_GP                 0x80000B0Du
#define VMX_ENTRY_INJECT_PF                 0x80000B0Eu

#define VMX_ENTRY_INJECT_DF                 0x80000B08u

/* Intel SDM rev. 092, Vol. 3A §4.7 (page-fault error code). */
#define X86_PFEC_P                          (1u << 0)
#define X86_PFEC_W                          (1u << 1)
#define X86_PFEC_U                          (1u << 2)
#define X86_PFEC_RSVD                       (1u << 3)
#define X86_PFEC_I                          (1u << 4)
#define INTEL_BUGCHECK_UNEXPECTED_EXIT      0x49564D58u
#define INTEL_BUGCHECK_INVALIDATION         0x494E5645u
#define INTEL_BUGCHECK_EVENT_COLLISION      0x49455654u
#define INTEL_BUGCHECK_EPT_VIOLATION        0x45505456u
#define INTEL_BUGCHECK_EPT_MISCONFIG        0x4550544Du

#define INVEPT_SINGLE_CONTEXT               1u
#define INVEPT_ALL_CONTEXTS                 2u
#define INVVPID_SINGLE_CONTEXT              1u

#define INTEL_START_STAGE_NONE              0u
#define INTEL_START_STAGE_CET               1u
#define INTEL_START_STAGE_FEATURE_CONTROL   2u
#define INTEL_START_STAGE_VMXON             3u
#define INTEL_START_STAGE_VMCLEAR           4u
#define INTEL_START_STAGE_VMPTRLD           5u
#define INTEL_START_STAGE_VMCS_SETUP        6u
#define INTEL_START_STAGE_INVEPT            7u
#define INTEL_START_STAGE_INVVPID           8u
#define INTEL_START_STAGE_VMLAUNCH          9u

#define INTEL_CONTROL_FAIL_SECONDARY_ACTIVATION (1u << 3)
#define INTEL_CONTROL_FAIL_BITMAPS              (1u << 4)
#define INTEL_CONTROL_FAIL_SECONDARY_REQUIRED   (1u << 5)
#define INTEL_CONTROL_FAIL_EXIT_REQUIRED        (1u << 6)
#define INTEL_CONTROL_FAIL_ENTRY_REQUIRED       (1u << 7)

typedef enum _INTEL_SPLIT_REASON {
    /* Boot-time split because the covered 2 MiB region mixes RAM and MMIO.
       These splits carry heterogeneous memory types and must never be
       merged back into a single 2 MiB leaf. */
    INTEL_SPLIT_REASON_MIXED_MEMORY_MAP = 0,
    /* Runtime permission change on a single 4 KiB PTE.  These splits carry
       uniform memory types and may be merged once all 512 PTEs return to
       the identical attributes captured before the split. */
    INTEL_SPLIT_REASON_PERMISSION = 1,
    INTEL_SPLIT_REASON_HOOK = 2
} INTEL_SPLIT_REASON;

typedef struct _INTEL_SLAT_SPLIT {
    LIST_ENTRY Link;
    ULONG PdptIndex;
    ULONG PdIndex;
    ULONG Reason;
    PVOID Pt;
} INTEL_SLAT_SPLIT;

typedef struct _INTEL_EPT_ROOT {
    PVOID Pml4;
    PVOID Pdpt;
    PVOID Pds[512];
    LIST_ENTRY SplitList;
    EX_PUSH_LOCK SlatLock;
    ULONG64 EptPointer;
} INTEL_EPT_ROOT;

typedef struct _INTEL_EPT_VIOLATION {
    ULONG64 Qualification;
    ULONG64 GuestPhysicalAddress;
    ULONG64 GuestLinearAddress;
    BOOLEAN LinearValid;
    BOOLEAN Translation;
    BOOLEAN Read;
    BOOLEAN Write;
    BOOLEAN Execute;
    BOOLEAN EptReadable;
    BOOLEAN EptWritable;
    BOOLEAN EptExecutable;
} INTEL_EPT_VIOLATION;

typedef struct _INTEL_BACKEND_CONTEXT {
    /* Identity EPT that maps every guest physical address to itself.
       Every vCPU boots with this EPTP loaded. */
    INTEL_EPT_ROOT PrimaryRoot;
    INTEL_EPT_ROOT* HookRoot;

    EX_PUSH_LOCK HookLock;
    volatile LONG HookMutationActive;
    volatile LONG ForcePrimaryEpt;
    ULONG64 MapLimit;
    ULONG64 VmxBasic;
    ULONG64 EptVpidCapabilities;

    volatile LONG64 SlatGeneration;
} INTEL_BACKEND_CONTEXT;

typedef struct DECLSPEC_ALIGN(16) _INTEL_INVALIDATION_DESCRIPTOR {
    ULONG64 Context;
    ULONG64 Reserved;
} INTEL_INVALIDATION_DESCRIPTOR;

#pragma pack(push, 1)
typedef struct _INTEL_DESCRIPTOR_TABLE_REGISTER {
    USHORT Limit;
    ULONG64 Base;
} INTEL_DESCRIPTOR_TABLE_REGISTER;
#pragma pack(pop)

C_ASSERT(sizeof(INTEL_DESCRIPTOR_TABLE_REGISTER) == 10);

typedef struct _INTEL_SEGMENT_STATE {
    USHORT Selector;
    ULONG Limit;
    ULONG AccessRights;
    ULONG64 Base;
} INTEL_SEGMENT_STATE;

PVOID
IntelAllocatePage(
    _In_ ULONG64 HighestAddress
    );

NTSTATUS
IntelBuildEpt(
    _Inout_ INTEL_BACKEND_CONTEXT* Context
    );

VOID
IntelFreeEpt(
    _Inout_ INTEL_BACKEND_CONTEXT* Context
    );

NTSTATUS
IntelBuildIdentityRoot(
    _Inout_ INTEL_BACKEND_CONTEXT* Backend,
    _Inout_ INTEL_EPT_ROOT* Root,
    _In_ HV_PAGE_ACCESS LeafAccess
    );

VOID
IntelFreeRoot(
    _Inout_ INTEL_EPT_ROOT* Root
    );

VOID
IntelFlushEptIfNeeded(
    _Inout_ INTEL_CPU_CONTEXT* Context
    );

NTSTATUS
IntelQueryOwnedPageAccess(
    _Inout_ HV_STATE* State,
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _Out_ HV_PAGE_ACCESS* Access
    );

NTSTATUS
IntelSetOwnedPageAccess(
    _Inout_ HV_STATE* State,
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ HV_PAGE_ACCESS Access,
    _Out_ HV_PAGE_ACCESS* PreviousAccess
    );

NTSTATUS
IntelSetOwnedPageMapping(
    _Inout_ HV_STATE* State,
    _In_ PHYSICAL_ADDRESS GuestPhysicalAddress,
    _In_ PHYSICAL_ADDRESS HostPhysicalAddress,
    _In_ HV_PAGE_ACCESS Access,
    _Out_ PHYSICAL_ADDRESS* PreviousHostPhysicalAddress,
    _Out_ HV_PAGE_ACCESS* PreviousAccess
    );

NTSTATUS
IntelVmWrite(
    _In_ ULONG Field,
    _In_ ULONG64 Value
    );

NTSTATUS
IntelSetupVmcs(
    _Inout_ HV_STATE* State,
    _Inout_ HV_CPU* Cpu
    );

VOID
IntelDecodeEptViolation(
    _In_ ULONG64 Qualification,
    _In_ ULONG64 GuestPhysicalAddress,
    _In_ ULONG64 GuestLinearAddress,
    _Out_ INTEL_EPT_VIOLATION* Decoded
    );

DECLSPEC_NORETURN
VOID
IntelFailEptViolation(
    _In_ const HV_CPU* Cpu,
    _In_ const INTEL_EPT_VIOLATION* Decoded,
    _In_ ULONG64 GuestRip
    );

DECLSPEC_NORETURN
VOID
IntelFailEptMisconfiguration(
    _In_ const HV_CPU* Cpu,
    _In_ ULONG64 GuestPhysicalAddress,
    _In_ ULONG64 GuestRip
    );

FORCEINLINE
ULONG64
IntelGetGuestCr2(
    _In_ const INTEL_CPU_CONTEXT* Context
    )
{
    return Context->GuestCr2;
}

FORCEINLINE
VOID
IntelSetGuestCr2(
    _Inout_ INTEL_CPU_CONTEXT* Context,
    _In_ ULONG64 Value
    )
{
    Context->GuestCr2 = Value;
}

#define INTEL_HOOK_TABLE_CAPACITY 64u

typedef enum _INTEL_HOOK_KIND {
    /* Sentinel used to detect an unused slot.  Never returned to callers. */
    INTEL_HOOK_KIND_NONE = 0,
    /* Execute-view hook: reads/writes stay on the primary root, fetches
       switch to the secondary root's shadow HPA. */
    INTEL_HOOK_KIND_EXECUTE = 1
} INTEL_HOOK_KIND;

typedef struct _INTEL_HOOK_POLICY {
    /* Even values describe stable snapshots; odd values mean mutation. */
    volatile LONG64 Sequence;
    /* GPA of the hooked 4 KiB page (aligned).  Zero when the slot is
       unused.  Publication uses release semantics; readers must observe
       a nonzero value before reading any other field. */
    volatile ULONG64 GuestPhysicalAddress;
    /* HPA of the shadow page installed in the secondary root. */
    ULONG64 ShadowHostPhysicalAddress;
    /* Retained virtual address of the shadow page for teardown. */
    PVOID ShadowVirtual;
    /* Original PTE from the primary root before the install rewrote it,
       used verbatim on remove. */
    ULONG64 OriginalPrimaryPte;
    /* Original PTE from the secondary root before install. */
    ULONG64 OriginalSecondaryPte;
    /* One of INTEL_HOOK_KIND. */
    ULONG Kind;
    /* Opaque cookie chosen by the caller. */
    ULONG Cookie;
} INTEL_HOOK_POLICY;

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
IntelHookInstall(
    _Inout_ HV_STATE* State,
    _In_ ULONG64 GuestPhysicalAddress,
    _In_reads_bytes_(PatchSize) const VOID* PatchBytes,
    _In_ ULONG PatchOffset,
    _In_ ULONG PatchSize,
    _In_ ULONG Cookie,
    _Out_ ULONG* HookId
    );

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
IntelHookRemove(
    _Inout_ HV_STATE* State,
    _In_ ULONG HookId
    );

NTSTATUS
IntelHookQuery(
    _In_ HV_STATE* State,
    _In_ ULONG HookId,
    _Out_ ULONG* Valid,
    _Out_ ULONG* Kind,
    _Out_ ULONG* Cookie,
    _Out_ ULONG64* GuestPhysicalAddress,
    _Out_ ULONG64* ShadowHostPhysicalAddress
    );

_Success_(return != FALSE)
BOOLEAN
IntelHookLookup(
    _In_ const INTEL_BACKEND_CONTEXT* Backend,
    _In_ ULONG64 GuestPhysicalAddress,
    _Out_ INTEL_HOOK_POLICY* Out
    );

/*
 * Reset the hook table to empty state.  Called from backend teardown; no
 * callers are expected while this runs.
 */
VOID
IntelHookResetTable(
    VOID
    );

VOID
IntelHookTeardown(
    VOID
    );

/*
 * Ensure `Backend->HookRoot` is allocated and initialized.  Idempotent.
 * Called by `IntelHookInstall` before the first shadow page publishes.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
IntelHookEnsureSecondaryRoot(
    _Inout_ INTEL_BACKEND_CONTEXT* Backend
    );

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
IntelHookAcquirePt(
    _Inout_ INTEL_EPT_ROOT* Root,
    _In_ ULONG64 MapLimit,
    _In_ ULONG64 GuestPhysicalAddress,
    _In_ INTEL_SPLIT_REASON NewSplitReason,
    _Outptr_ ULONG64** Pt,
    _Out_ ULONG* PtIndex
    );

VOID
IntelHookReleasePt(
    _Inout_ INTEL_EPT_ROOT* Root
    );

VOID
IntelHookInvalidateEverywhere(
    _Inout_ HV_STATE* State,
    _Inout_ INTEL_BACKEND_CONTEXT* Backend
    );

VOID
IntelHookRetireSecondaryViews(
    _Inout_ HV_STATE* State,
    _Inout_ INTEL_BACKEND_CONTEXT* Backend
    );

VOID
IntelHookAllowSecondaryViews(
    _Inout_ INTEL_BACKEND_CONTEXT* Backend
    );

NTSTATUS
IntelSwitchActiveEptRoot(
    _Inout_ INTEL_CPU_CONTEXT* Context,
    _In_ const INTEL_EPT_ROOT* Root
    );

VOID
IntelRequestNmiInjection(
    _Inout_ INTEL_CPU_CONTEXT* Context
    );

VOID
IntelRequestInterruptInjection(
    _Inout_ INTEL_CPU_CONTEXT* Context,
    _In_ UCHAR Vector
    );

VOID
IntelHandleNmiWindow(
    _Inout_ INTEL_CPU_CONTEXT* Context
    );

VOID
IntelHandleInterruptWindow(
    _Inout_ INTEL_CPU_CONTEXT* Context
    );
