#include "introspection.h"

typedef struct _HV_INTROSPECTION_CONTEXT {
    HV_STATE* Owner;
    PVOID Page;
    PHYSICAL_ADDRESS PhysicalAddress;
    HV_PAGE_ACCESS OriginalAccess;
    HV_PAGE_ACCESS RestrictedAccess;
    BOOLEAN Restricted;
    BOOLEAN Started;
} HV_INTROSPECTION_CONTEXT;

static EX_PUSH_LOCK g_IntrospectionLock = {0};
static HV_INTROSPECTION_CONTEXT g_Introspection = {0};

static NTSTATUS
HvIntrospectionRestoreLocked(
    VOID
    )
{
    HV_PAGE_ACCESS previousAccess;
    NTSTATUS status;

    if (!g_Introspection.Restricted) {
        return STATUS_SUCCESS;
    }

    status = g_Introspection.Owner->Backend->SetOwnedPageAccess(
        g_Introspection.Owner,
        g_Introspection.PhysicalAddress,
        g_Introspection.OriginalAccess,
        &previousAccess);
    if (NT_SUCCESS(status)) {
        g_Introspection.Restricted = FALSE;
    }

    return status;
}

static VOID
HvIntrospectionFreeLocked(
    VOID
    )
{
    PVOID page;

    page = g_Introspection.Page;
    RtlZeroMemory(&g_Introspection, sizeof(g_Introspection));
    MmFreeContiguousMemory(page);
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
HvIntrospectionStart(
    _Inout_ HV_STATE* State
    )
{
    const ULONG knownAccess =
        HV_PAGE_ACCESS_READ |
        HV_PAGE_ACCESS_WRITE |
        HV_PAGE_ACCESS_EXECUTE;
    const ULONG requiredAccess =
        HV_PAGE_ACCESS_READ |
        HV_PAGE_ACCESS_WRITE;
    PHYSICAL_ADDRESS lowestAddress;
    PHYSICAL_ADDRESS highestAddress;
    PHYSICAL_ADDRESS boundaryMultiple;
    HV_PAGE_ACCESS queriedAccess;
    HV_PAGE_ACCESS previousAccess;
    HV_PAGE_ACCESS restrictedAccess;
    NTSTATUS cleanupStatus;
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (State == NULL ||
        State->Backend == NULL ||
        State->Backend->QueryOwnedPageAccess == NULL ||
        State->Backend->SetOwnedPageAccess == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (InterlockedCompareExchange(&State->Lifecycle, 0, 0) !=
        HV_LIFECYCLE_STARTING) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_IntrospectionLock);

    if (g_Introspection.Page != NULL) {
        status =
            g_Introspection.Owner == State && g_Introspection.Started
                ? STATUS_SUCCESS
                : STATUS_DEVICE_BUSY;
        goto Exit;
    }

    lowestAddress.QuadPart = 0;
    highestAddress.QuadPart = MAXLONGLONG;
    boundaryMultiple.QuadPart = 0;

    g_Introspection.Page = MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE,
        lowestAddress,
        highestAddress,
        boundaryMultiple,
        MmCached);
    if (g_Introspection.Page == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    RtlZeroMemory(g_Introspection.Page, PAGE_SIZE);
    g_Introspection.Owner = State;
    g_Introspection.PhysicalAddress =
        MmGetPhysicalAddress(g_Introspection.Page);

    if ((((ULONGLONG)g_Introspection.PhysicalAddress.QuadPart) &
         (PAGE_SIZE - 1)) != 0) {
        status = STATUS_DATATYPE_MISALIGNMENT;
        goto Rollback;
    }

    status = State->Backend->QueryOwnedPageAccess(
        State,
        g_Introspection.PhysicalAddress,
        &queriedAccess);
    if (!NT_SUCCESS(status)) {
        goto Rollback;
    }

    if ((((ULONG)queriedAccess & requiredAccess) != requiredAccess) ||
        (((ULONG)queriedAccess & ~knownAccess) != 0)) {
        status = STATUS_DATA_ERROR;
        goto Rollback;
    }

    restrictedAccess = (HV_PAGE_ACCESS)(
        (ULONG)queriedAccess & ~(ULONG)HV_PAGE_ACCESS_WRITE);
    status = State->Backend->SetOwnedPageAccess(
        State,
        g_Introspection.PhysicalAddress,
        restrictedAccess,
        &previousAccess);
    if (!NT_SUCCESS(status)) {
        goto Rollback;
    }

    g_Introspection.OriginalAccess = previousAccess;
    g_Introspection.RestrictedAccess = restrictedAccess;
    g_Introspection.Restricted = TRUE;

    if (previousAccess != queriedAccess) {
        status = STATUS_DATA_ERROR;
        goto Rollback;
    }

    status = State->Backend->QueryOwnedPageAccess(
        State,
        g_Introspection.PhysicalAddress,
        &queriedAccess);
    if (!NT_SUCCESS(status)) {
        goto Rollback;
    }

    if (queriedAccess != restrictedAccess) {
        status = STATUS_DATA_ERROR;
        goto Rollback;
    }

    g_Introspection.Started = TRUE;
    status = STATUS_SUCCESS;
    goto Exit;

Rollback:
    cleanupStatus = HvIntrospectionRestoreLocked();
    if (!NT_SUCCESS(cleanupStatus)) {
        status = cleanupStatus;
        goto Exit;
    }

    HvIntrospectionFreeLocked();

Exit:
    ExReleasePushLockExclusive(&g_IntrospectionLock);
    KeLeaveCriticalRegion();
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
HvIntrospectionStop(
    _Inout_ HV_STATE* State
    )
{
    NTSTATUS status = STATUS_SUCCESS;

    if (State == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_IntrospectionLock);

    if (g_Introspection.Page != NULL && g_Introspection.Owner != State) {
        status = STATUS_INVALID_DEVICE_STATE;
    } else if (g_Introspection.Page != NULL) {
        status = HvIntrospectionRestoreLocked();
        if (NT_SUCCESS(status)) {
            HvIntrospectionFreeLocked();
        }
    }

    ExReleasePushLockExclusive(&g_IntrospectionLock);
    KeLeaveCriticalRegion();
    return status;
}
