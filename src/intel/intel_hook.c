#include "intel_internal.h"

static INTEL_HOOK_POLICY g_HookTable[INTEL_HOOK_TABLE_CAPACITY];

static LONG64
IntelHookReadSequence(
    _In_ const INTEL_HOOK_POLICY* Policy
    )
{
    return InterlockedCompareExchange64(
        (volatile LONG64*)&Policy->Sequence, 0, 0);
}

/* Caller holds the backend hook lock exclusively. */
static VOID
IntelHookBeginUpdate(
    _Inout_ INTEL_HOOK_POLICY* Policy
    )
{
    LONG64 sequence = InterlockedIncrement64(&Policy->Sequence);
    NT_ASSERT((sequence & 1) != 0);
    UNREFERENCED_PARAMETER(sequence);
    KeMemoryBarrier();
}

/* Caller holds the backend hook lock exclusively. */
static VOID
IntelHookEndUpdate(
    _Inout_ INTEL_HOOK_POLICY* Policy
    )
{
    LONG64 sequence;

    KeMemoryBarrier();
    sequence = InterlockedIncrement64(&Policy->Sequence);
    NT_ASSERT((sequence & 1) == 0);
    UNREFERENCED_PARAMETER(sequence);
}

_Success_(return != FALSE)
BOOLEAN
IntelHookLookup(
    _In_ const INTEL_BACKEND_CONTEXT* Backend,
    _In_ ULONG64 GuestPhysicalAddress,
    _Out_ INTEL_HOOK_POLICY* Out
    )
{
    ULONG64 target;
    ULONG index;

    if (Out == NULL) {
        return FALSE;
    }
    RtlZeroMemory(Out, sizeof(*Out));

    if (Backend == NULL ||
        InterlockedCompareExchange(
            (volatile LONG*)&Backend->ForcePrimaryEpt, 0, 0) != 0) {
        return FALSE;
    }

    target = GuestPhysicalAddress & ~((ULONG64)PAGE_SIZE - 1);

    for (index = 0; index < INTEL_HOOK_TABLE_CAPACITY; ++index) {
        LONG64 firstSequence = IntelHookReadSequence(&g_HookTable[index]);
        ULONG64 slotGpa = (ULONG64)InterlockedCompareExchange64(
            (volatile LONG64*)&g_HookTable[index].GuestPhysicalAddress,
            0, 0);

        if ((firstSequence & 1) != 0 || slotGpa == 0 || slotGpa != target) {
            continue;
        }

        Out->Sequence = firstSequence;
        Out->GuestPhysicalAddress = slotGpa;
        Out->ShadowHostPhysicalAddress =
            g_HookTable[index].ShadowHostPhysicalAddress;
        Out->ShadowVirtual = g_HookTable[index].ShadowVirtual;
        Out->OriginalPrimaryPte = g_HookTable[index].OriginalPrimaryPte;
        Out->OriginalSecondaryPte = g_HookTable[index].OriginalSecondaryPte;
        Out->Kind = g_HookTable[index].Kind;
        Out->Cookie = g_HookTable[index].Cookie;
        KeMemoryBarrier();
        if (firstSequence == IntelHookReadSequence(&g_HookTable[index]) &&
            InterlockedCompareExchange(
                (volatile LONG*)&Backend->ForcePrimaryEpt, 0, 0) == 0) {
            return Out->Kind != INTEL_HOOK_KIND_NONE;
        }
        RtlZeroMemory(Out, sizeof(*Out));
    }
    return FALSE;
}

VOID
IntelHookResetTable(
    VOID
    )
{
    RtlZeroMemory(g_HookTable, sizeof(g_HookTable));
}

VOID
IntelHookTeardown(
    VOID
    )
{
    ULONG index;

    for (index = 0; index < INTEL_HOOK_TABLE_CAPACITY; ++index) {
        if (g_HookTable[index].ShadowVirtual != NULL) {
            MmFreeContiguousMemory(g_HookTable[index].ShadowVirtual);
        }
    }
    IntelHookResetTable();
}

NTSTATUS
IntelHookQuery(
    _In_ HV_STATE* State,
    _In_ ULONG HookId,
    _Out_ ULONG* Valid,
    _Out_ ULONG* Kind,
    _Out_ ULONG* Cookie,
    _Out_ ULONG64* GuestPhysicalAddress,
    _Out_ ULONG64* ShadowHostPhysicalAddress
    )
{
    INTEL_BACKEND_CONTEXT* backend;
    ULONG64 slotGpa;

    *Valid = 0;
    *Kind = 0;
    *Cookie = 0;
    *GuestPhysicalAddress = 0;
    *ShadowHostPhysicalAddress = 0;

    if (State == NULL || State->BackendContext == NULL ||
        HookId >= INTEL_HOOK_TABLE_CAPACITY) {
        return STATUS_INVALID_PARAMETER;
    }

    backend = (INTEL_BACKEND_CONTEXT*)State->BackendContext;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&backend->HookLock);

    slotGpa = (ULONG64)InterlockedCompareExchange64(
        (volatile LONG64*)&g_HookTable[HookId].GuestPhysicalAddress, 0, 0);
    if (slotGpa == 0) {
        ExReleasePushLockShared(&backend->HookLock);
        KeLeaveCriticalRegion();
        return STATUS_SUCCESS;
    }
    *Valid = 1;
    *Kind = g_HookTable[HookId].Kind;
    *Cookie = g_HookTable[HookId].Cookie;
    *GuestPhysicalAddress = slotGpa;
    *ShadowHostPhysicalAddress =
        g_HookTable[HookId].ShadowHostPhysicalAddress;
    ExReleasePushLockShared(&backend->HookLock);
    KeLeaveCriticalRegion();
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
IntelHookEnsureSecondaryRoot(
    _Inout_ INTEL_BACKEND_CONTEXT* Backend
    )
{
    INTEL_EPT_ROOT* root;
    NTSTATUS status;

    if (Backend->HookRoot != NULL) {
        return STATUS_SUCCESS;
    }

    root = (INTEL_EPT_ROOT*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*root), HV_POOL_TAG_HOOK);
    if (root == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IntelBuildIdentityRoot(
        Backend, root, HV_PAGE_ACCESS_READ | HV_PAGE_ACCESS_WRITE);
    if (!NT_SUCCESS(status)) {
        IntelFreeRoot(root);
        ExFreePoolWithTag(root, HV_POOL_TAG_HOOK);
        return status;
    }
    Backend->HookRoot = root;
    return STATUS_SUCCESS;
}

static ULONG
IntelHookFindFreeSlot(
    VOID
    )
{
    ULONG index;

    for (index = 0; index < INTEL_HOOK_TABLE_CAPACITY; ++index) {
        if (InterlockedCompareExchange64(
                (volatile LONG64*)&g_HookTable[index].GuestPhysicalAddress,
                0, 0) == 0) {
            return index;
        }
    }
    return INTEL_HOOK_TABLE_CAPACITY;
}

/* Caller holds the backend hook lock exclusively. */
static BOOLEAN
IntelHookGpaExists(
    _In_ ULONG64 GuestPhysicalAddress
    )
{
    ULONG index;

    for (index = 0; index < INTEL_HOOK_TABLE_CAPACITY; ++index) {
        if ((ULONG64)InterlockedCompareExchange64(
                (volatile LONG64*)&g_HookTable[index].GuestPhysicalAddress,
                0, 0) == GuestPhysicalAddress) {
            return TRUE;
        }
    }
    return FALSE;
}

static NTSTATUS
IntelHookBuildShadow(
    _In_ ULONG64 HostPhysicalAddress,
    _In_reads_bytes_(PatchSize) const VOID* PatchBytes,
    _In_ ULONG PatchOffset,
    _In_ ULONG PatchSize,
    _Outptr_ PVOID* ShadowVirtual,
    _Out_ ULONG64* ShadowPhysical
    )
{
    PHYSICAL_ADDRESS originalPhysical;
    PVOID originalMapping;
    PVOID shadow;

    *ShadowVirtual = NULL;
    *ShadowPhysical = 0;

    shadow = IntelAllocatePage(MAXLONGLONG);
    if (shadow == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    originalPhysical.QuadPart = (LONGLONG)(HostPhysicalAddress &
                                           ~((ULONG64)PAGE_SIZE - 1));
    originalMapping = MmMapIoSpaceEx(
        originalPhysical, PAGE_SIZE, PAGE_READWRITE);
    if (originalMapping == NULL) {
        MmFreeContiguousMemory(shadow);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(shadow, originalMapping, PAGE_SIZE);
    MmUnmapIoSpace(originalMapping, PAGE_SIZE);

    RtlCopyMemory((PUCHAR)shadow + PatchOffset, PatchBytes, PatchSize);

    *ShadowVirtual = shadow;
    *ShadowPhysical = (ULONG64)MmGetPhysicalAddress(shadow).QuadPart;
    return STATUS_SUCCESS;
}

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
    )
{
    INTEL_BACKEND_CONTEXT* backend;
    PVOID shadowVirtual = NULL;
    ULONG64 shadowPhysical = 0;
    ULONG64* primaryPt = NULL;
    ULONG64* secondaryPt = NULL;
    ULONG primaryPtIndex = 0;
    ULONG secondaryPtIndex = 0;
    ULONG slot;
    ULONG64 alignedGpa;
    ULONG64 originalPrimary = 0;
    ULONG64 originalSecondary = 0;
    NTSTATUS status;
    BOOLEAN backendLockHeld = FALSE;
    BOOLEAN primaryLocked = FALSE;
    BOOLEAN secondaryLocked = FALSE;
    BOOLEAN mutationOwned = FALSE;

    if (State == NULL || HookId == NULL || PatchBytes == NULL ||
        PatchSize == 0 || State->BackendContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((ULONG64)PatchOffset + (ULONG64)PatchSize > PAGE_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    *HookId = INTEL_HOOK_TABLE_CAPACITY;
    backend = (INTEL_BACKEND_CONTEXT*)State->BackendContext;

    if ((backend->EptVpidCapabilities & 1) == 0) {
        return STATUS_NOT_SUPPORTED;
    }

    alignedGpa = GuestPhysicalAddress & ~((ULONG64)PAGE_SIZE - 1);
    if (alignedGpa == 0) {

        return STATUS_INVALID_ADDRESS;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&backend->HookLock);
    backendLockHeld = TRUE;

    if (InterlockedCompareExchange(
            &backend->HookMutationActive, 1, 0) != 0) {
        status = STATUS_DEVICE_BUSY;
        goto Exit;
    }
    mutationOwned = TRUE;

    status = IntelHookEnsureSecondaryRoot(backend);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    if (IntelHookGpaExists(alignedGpa)) {
        status = STATUS_OBJECT_NAME_COLLISION;
        goto Exit;
    }

    slot = IntelHookFindFreeSlot();
    if (slot == INTEL_HOOK_TABLE_CAPACITY) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    status = IntelHookAcquirePt(
        &backend->PrimaryRoot, backend->MapLimit, alignedGpa,
        INTEL_SPLIT_REASON_HOOK, &primaryPt, &primaryPtIndex);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    primaryLocked = TRUE;

    status = IntelHookAcquirePt(
        backend->HookRoot, backend->MapLimit, alignedGpa,
        INTEL_SPLIT_REASON_HOOK, &secondaryPt, &secondaryPtIndex);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    secondaryLocked = TRUE;

    originalPrimary = primaryPt[primaryPtIndex];
    originalSecondary = secondaryPt[secondaryPtIndex];

    status = IntelHookBuildShadow(
        originalPrimary & EPT_ADDRESS_MASK,
        PatchBytes, PatchOffset, PatchSize,
        &shadowVirtual, &shadowPhysical);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    InterlockedExchange64(
        (volatile LONG64*)&primaryPt[primaryPtIndex],
        (LONG64)((originalPrimary & ~EPT_ACCESS_MASK) |
                 HV_PAGE_ACCESS_READ | HV_PAGE_ACCESS_WRITE));
    InterlockedExchange64(
        (volatile LONG64*)&secondaryPt[secondaryPtIndex],
        (LONG64)(((originalSecondary & EPT_MEMORY_TYPE_MASK)) |
                 (shadowPhysical & EPT_ADDRESS_MASK) |
                 HV_PAGE_ACCESS_EXECUTE));

    KeMemoryBarrier();

    IntelHookBeginUpdate(&g_HookTable[slot]);
    g_HookTable[slot].ShadowHostPhysicalAddress = shadowPhysical;
    g_HookTable[slot].ShadowVirtual = shadowVirtual;
    g_HookTable[slot].OriginalPrimaryPte = originalPrimary;
    g_HookTable[slot].OriginalSecondaryPte = originalSecondary;
    g_HookTable[slot].Kind = INTEL_HOOK_KIND_EXECUTE;
    g_HookTable[slot].Cookie = Cookie;
    KeMemoryBarrier();
    InterlockedExchange64(
        (volatile LONG64*)&g_HookTable[slot].GuestPhysicalAddress,
        (LONG64)alignedGpa);
    IntelHookEndUpdate(&g_HookTable[slot]);

    IntelHookReleasePt(backend->HookRoot);
    secondaryLocked = FALSE;
    IntelHookReleasePt(&backend->PrimaryRoot);
    primaryLocked = FALSE;

    ExReleasePushLockExclusive(&backend->HookLock);
    KeLeaveCriticalRegion();
    backendLockHeld = FALSE;

    IntelHookInvalidateEverywhere(State, backend);
    InterlockedExchange(&backend->HookMutationActive, 0);

    *HookId = slot;
    return STATUS_SUCCESS;

Exit:
    if (secondaryLocked) {
        IntelHookReleasePt(backend->HookRoot);
    }
    if (primaryLocked) {
        IntelHookReleasePt(&backend->PrimaryRoot);
    }
    if (shadowVirtual != NULL) {
        MmFreeContiguousMemory(shadowVirtual);
    }
    if (backendLockHeld) {
        ExReleasePushLockExclusive(&backend->HookLock);
        KeLeaveCriticalRegion();
    }
    if (mutationOwned) {
        InterlockedExchange(&backend->HookMutationActive, 0);
    }
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
IntelHookRemove(
    _Inout_ HV_STATE* State,
    _In_ ULONG HookId
    )
{
    INTEL_BACKEND_CONTEXT* backend;
    ULONG64* primaryPt = NULL;
    ULONG64* secondaryPt = NULL;
    ULONG primaryPtIndex = 0;
    ULONG secondaryPtIndex = 0;
    ULONG64 alignedGpa;
    ULONG64 originalPrimary;
    ULONG64 originalSecondary;
    PVOID shadowVirtual;
    NTSTATUS status;
    BOOLEAN primaryLocked = FALSE;
    BOOLEAN secondaryLocked = FALSE;

    if (State == NULL || State->BackendContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (HookId >= INTEL_HOOK_TABLE_CAPACITY) {
        return STATUS_INVALID_PARAMETER;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    backend = (INTEL_BACKEND_CONTEXT*)State->BackendContext;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&backend->HookLock);

    if (InterlockedCompareExchange(
            &backend->HookMutationActive, 1, 0) != 0) {
        ExReleasePushLockExclusive(&backend->HookLock);
        KeLeaveCriticalRegion();
        return STATUS_DEVICE_BUSY;
    }

    alignedGpa = (ULONG64)InterlockedCompareExchange64(
        (volatile LONG64*)&g_HookTable[HookId].GuestPhysicalAddress, 0, 0);
    if (alignedGpa == 0) {
        InterlockedExchange(&backend->HookMutationActive, 0);
        ExReleasePushLockExclusive(&backend->HookLock);
        KeLeaveCriticalRegion();
        return STATUS_NOT_FOUND;
    }

    originalPrimary = g_HookTable[HookId].OriginalPrimaryPte;
    originalSecondary = g_HookTable[HookId].OriginalSecondaryPte;
    shadowVirtual = g_HookTable[HookId].ShadowVirtual;

    IntelHookBeginUpdate(&g_HookTable[HookId]);
    InterlockedExchange64(
        (volatile LONG64*)&g_HookTable[HookId].GuestPhysicalAddress, 0);
    IntelHookEndUpdate(&g_HookTable[HookId]);

    ExReleasePushLockExclusive(&backend->HookLock);
    KeLeaveCriticalRegion();

    IntelHookRetireSecondaryViews(State, backend);

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&backend->HookLock);

    status = IntelHookAcquirePt(
        &backend->PrimaryRoot, backend->MapLimit, alignedGpa,
        INTEL_SPLIT_REASON_HOOK, &primaryPt, &primaryPtIndex);
    if (!NT_SUCCESS(status)) {
        goto Republish;
    }
    primaryLocked = TRUE;

    if (backend->HookRoot == NULL) {
        status = STATUS_INVALID_DEVICE_STATE;
        goto Republish;
    }
    status = IntelHookAcquirePt(
        backend->HookRoot, backend->MapLimit, alignedGpa,
        INTEL_SPLIT_REASON_HOOK, &secondaryPt, &secondaryPtIndex);
    if (!NT_SUCCESS(status)) {
        goto Republish;
    }
    secondaryLocked = TRUE;

    InterlockedExchange64(
        (volatile LONG64*)&primaryPt[primaryPtIndex],
        (LONG64)originalPrimary);
    InterlockedExchange64(
        (volatile LONG64*)&secondaryPt[secondaryPtIndex],
        (LONG64)originalSecondary);

    IntelHookReleasePt(backend->HookRoot);
    secondaryLocked = FALSE;
    IntelHookReleasePt(&backend->PrimaryRoot);
    primaryLocked = FALSE;

    ExReleasePushLockExclusive(&backend->HookLock);
    KeLeaveCriticalRegion();

    IntelHookInvalidateEverywhere(State, backend);

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&backend->HookLock);

    IntelHookBeginUpdate(&g_HookTable[HookId]);
    g_HookTable[HookId].ShadowHostPhysicalAddress = 0;
    g_HookTable[HookId].ShadowVirtual = NULL;
    g_HookTable[HookId].OriginalPrimaryPte = 0;
    g_HookTable[HookId].OriginalSecondaryPte = 0;
    g_HookTable[HookId].Kind = INTEL_HOOK_KIND_NONE;
    g_HookTable[HookId].Cookie = 0;
    IntelHookEndUpdate(&g_HookTable[HookId]);
    InterlockedExchange(&backend->HookMutationActive, 0);

    ExReleasePushLockExclusive(&backend->HookLock);
    KeLeaveCriticalRegion();
    IntelHookAllowSecondaryViews(backend);

    if (shadowVirtual != NULL) {
        MmFreeContiguousMemory(shadowVirtual);
    }
    return STATUS_SUCCESS;

Republish:
    if (secondaryLocked) {
        IntelHookReleasePt(backend->HookRoot);
    }
    if (primaryLocked) {
        IntelHookReleasePt(&backend->PrimaryRoot);
    }
    IntelHookBeginUpdate(&g_HookTable[HookId]);
    InterlockedExchange64(
        (volatile LONG64*)&g_HookTable[HookId].GuestPhysicalAddress,
        (LONG64)alignedGpa);
    IntelHookEndUpdate(&g_HookTable[HookId]);
    InterlockedExchange(&backend->HookMutationActive, 0);
    ExReleasePushLockExclusive(&backend->HookLock);
    KeLeaveCriticalRegion();
    IntelHookAllowSecondaryViews(backend);
    return status;
}
