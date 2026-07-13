#include <ntifs.h>

#include "device.h"
#include "hv.h"
#include "hv_log.h"

DRIVER_INITIALIZE DriverEntry;

#define JOHNSMITH_POOL_TAG_REGISTRY 'rSvJ'
#define JOHNSMITH_START_PENDING 1u
#define JOHNSMITH_START_SUCCEEDED 2u
#define JOHNSMITH_START_FAILED 3u
#define JOHNSMITH_START_CANCELLED 4u

static HV_STATE* g_Hypervisor;
static PETHREAD g_StartThread;
static UNICODE_STRING g_RegistryPath;
static volatile LONG g_StartWorkerQueued;
static volatile LONG g_StopRequested;

static NTSTATUS
JohnSmithOpenServiceKey(
    _In_ ACCESS_MASK DesiredAccess,
    _Out_ PHANDLE KeyHandle
    )
{
    OBJECT_ATTRIBUTES attributes;

    InitializeObjectAttributes(
        &attributes,
        &g_RegistryPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL);
    return ZwOpenKey(KeyHandle, DesiredAccess, &attributes);
}

static NTSTATUS
JohnSmithQueryDword(
    _In_ PCWSTR ValueName,
    _Out_ PULONG Value
    )
{
    union {
        KEY_VALUE_PARTIAL_INFORMATION Information;
        UCHAR Buffer[
            FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) +
            sizeof(ULONG)];
    } valueInformation;
    UNICODE_STRING name;
    HANDLE keyHandle;
    ULONG resultLength;
    NTSTATUS status;

    RtlInitUnicodeString(&name, ValueName);
    status = JohnSmithOpenServiceKey(KEY_QUERY_VALUE, &keyHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ZwQueryValueKey(
        keyHandle,
        &name,
        KeyValuePartialInformation,
        &valueInformation,
        sizeof(valueInformation),
        &resultLength);
    if (NT_SUCCESS(status)) {
        if (valueInformation.Information.Type != REG_DWORD ||
            valueInformation.Information.DataLength != sizeof(ULONG)) {
            status = STATUS_OBJECT_TYPE_MISMATCH;
        } else {
            RtlCopyMemory(
                Value,
                valueInformation.Information.Data,
                sizeof(*Value));
        }
    }
    ZwClose(keyHandle);
    return status;
}

static VOID
JohnSmithSetStartResult(
    _In_ ULONG State,
    _In_ NTSTATUS Status
    )
{
    UNICODE_STRING stateName = RTL_CONSTANT_STRING(L"StartState");
    UNICODE_STRING statusName = RTL_CONSTANT_STRING(L"StartStatus");
    HANDLE keyHandle;

    if (!NT_SUCCESS(JohnSmithOpenServiceKey(KEY_SET_VALUE, &keyHandle))) {
        return;
    }
    (VOID)ZwSetValueKey(
        keyHandle, &stateName, 0, REG_DWORD, &State, sizeof(State));
    (VOID)ZwSetValueKey(
        keyHandle, &statusName, 0, REG_DWORD, &Status, sizeof(Status));
    ZwClose(keyHandle);
}

static VOID
JohnSmithFreeRegistryPath(
    VOID
    )
{
    if (g_RegistryPath.Buffer != NULL) {
        ExFreePoolWithTag(
            g_RegistryPath.Buffer, JOHNSMITH_POOL_TAG_REGISTRY);
        RtlZeroMemory(&g_RegistryPath, sizeof(g_RegistryPath));
    }
}

_IRQL_requires_(PASSIVE_LEVEL)
static VOID
JohnSmithStartWorker(
    _In_opt_ PVOID Context
    )
{
    LARGE_INTEGER interval;
    ULONG requested = 0;
    ULONG state;
    NTSTATUS status = STATUS_CANCELLED;

    UNREFERENCED_PARAMETER(Context);
    interval.QuadPart = -100 * 10 * 1000; /* 100 ms, relative. */

    for (;;) {
        if (InterlockedCompareExchange(&g_StopRequested, 0, 0) != 0) {
            break;
        }
        status = JohnSmithQueryDword(L"StartRequested", &requested);
        if (!NT_SUCCESS(status) || requested != 0) {
            break;
        }
        (VOID)KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }

    if (NT_SUCCESS(status) && requested != 0 &&
        InterlockedCompareExchange(&g_StopRequested, 0, 0) == 0) {
        status = HvStart(&g_Hypervisor);
        if (NT_SUCCESS(status)) {
            JsDevicePublishHypervisor(g_Hypervisor);
        }
        state = NT_SUCCESS(status)
            ? JOHNSMITH_START_SUCCEEDED
            : JOHNSMITH_START_FAILED;
    } else {
        g_Hypervisor = NULL;
        if (NT_SUCCESS(status)) {
            status = STATUS_CANCELLED;
        }
        state = InterlockedCompareExchange(&g_StopRequested, 0, 0) != 0
            ? JOHNSMITH_START_CANCELLED
            : JOHNSMITH_START_FAILED;
    }

    JohnSmithSetStartResult(state, status);
    if (!NT_SUCCESS(status) && status != STATUS_CANCELLED) {
        HV_LOG_ERROR(
            "Deferred hypervisor start failed: NTSTATUS 0x%08X.\n",
            (ULONG)status);
    }
    PsTerminateSystemThread(status);
}

_IRQL_requires_(PASSIVE_LEVEL)
static VOID
JohnSmithUnload(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    if (InterlockedCompareExchange(&g_StartWorkerQueued, 0, 0) != 0) {
        InterlockedExchange(&g_StopRequested, 1);
        (VOID)KeWaitForSingleObject(
            g_StartThread,
            Executive,
            KernelMode,
            FALSE,
            NULL);
        ObDereferenceObject(g_StartThread);
        g_StartThread = NULL;
        JohnSmithFreeRegistryPath();
    }
    JsDevicePublishHypervisor(NULL);
    if (g_Hypervisor != NULL) {
        HvStop(g_Hypervisor);
        g_Hypervisor = NULL;
    }
    JsDeviceTeardown(DriverObject);
}

_Use_decl_annotations_
NTSTATUS
DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    HANDLE threadHandle;
    ULONG startRequested;
    NTSTATUS status;

    DriverObject->DriverUnload = JohnSmithUnload;
    RtlZeroMemory(&g_RegistryPath, sizeof(g_RegistryPath));
    g_StartWorkerQueued = 0;
    g_StopRequested = 0;
    g_StartThread = NULL;

    g_RegistryPath.MaximumLength = RegistryPath->Length + sizeof(WCHAR);
    g_RegistryPath.Buffer = (PWSTR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        g_RegistryPath.MaximumLength,
        JOHNSMITH_POOL_TAG_REGISTRY);
    if (g_RegistryPath.Buffer == NULL) {
        DriverObject->DriverUnload = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(g_RegistryPath.Buffer, g_RegistryPath.MaximumLength);
    RtlCopyUnicodeString(&g_RegistryPath, RegistryPath);

    status = JsDeviceInitialize(DriverObject);
    if (!NT_SUCCESS(status)) {
        JohnSmithFreeRegistryPath();
        DriverObject->DriverUnload = NULL;
        return status;
    }

    status = JohnSmithQueryDword(L"StartRequested", &startRequested);
    if (NT_SUCCESS(status) && startRequested == 0) {
        JohnSmithSetStartResult(JOHNSMITH_START_PENDING, STATUS_PENDING);
        status = PsCreateSystemThread(
            &threadHandle,
            SYNCHRONIZE,
            NULL,
            NULL,
            NULL,
            JohnSmithStartWorker,
            NULL);
        if (!NT_SUCCESS(status)) {
            JohnSmithSetStartResult(JOHNSMITH_START_FAILED, status);
            JsDeviceTeardown(DriverObject);
            JohnSmithFreeRegistryPath();
            DriverObject->DriverUnload = NULL;
            return status;
        }
        status = ObReferenceObjectByHandle(
            threadHandle,
            SYNCHRONIZE,
            *PsThreadType,
            KernelMode,
            (PVOID*)&g_StartThread,
            NULL);
        if (!NT_SUCCESS(status)) {
            NTSTATUS referenceStatus = status;
            NTSTATUS waitStatus;

            InterlockedExchange(&g_StopRequested, 1);
            waitStatus = ZwWaitForSingleObject(
                threadHandle, FALSE, NULL);
            ZwClose(threadHandle);
            if (!NT_SUCCESS(waitStatus)) {
                /* The image must remain resident if thread exit is unknown. */
                DriverObject->DriverUnload = NULL;
                return STATUS_SUCCESS;
            }
            JsDeviceTeardown(DriverObject);
            JohnSmithFreeRegistryPath();
            DriverObject->DriverUnload = NULL;
            return referenceStatus;
        }
        ZwClose(threadHandle);
        InterlockedExchange(&g_StartWorkerQueued, 1);
        return STATUS_SUCCESS;
    }

    JohnSmithFreeRegistryPath();
    if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_NOT_FOUND) {
        JsDeviceTeardown(DriverObject);
        DriverObject->DriverUnload = NULL;
        return status;
    }
    status = HvStart(&g_Hypervisor);
    if (!NT_SUCCESS(status)) {
        JsDeviceTeardown(DriverObject);
        DriverObject->DriverUnload = NULL;
        g_Hypervisor = NULL;
    } else {
        JsDevicePublishHypervisor(g_Hypervisor);
    }

    return status;
}
