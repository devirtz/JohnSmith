#include <ntifs.h>
#include <wdmsec.h>

#include "device.h"
#include "hv_log.h"
#include "johnsmith_ioctl.h"

static const UNICODE_STRING JsDeviceSddl =
    RTL_CONSTANT_STRING(
        L"D:P(A;;GRGW;;;SY)(A;;GRGW;;;BA)");

// {94F3A0B5-8B0F-4A47-9F5A-6D4B7E5A0B10}
static const GUID JsDeviceClassGuid = {
    0x94f3a0b5, 0x8b0f, 0x4a47,
    { 0x9f, 0x5a, 0x6d, 0x4b, 0x7e, 0x5a, 0x0b, 0x10 }
};

static PDEVICE_OBJECT g_DeviceObject;
static UNICODE_STRING g_SymbolicLink;
static BOOLEAN g_SymbolicLinkCreated;
static HV_STATE* volatile g_PublishedHypervisor;
static EX_RUNDOWN_REF g_HypervisorRundown;
static volatile LONG g_AcceptingRequests;

static BOOLEAN
JsAcquirePublishedHypervisor(
    _Outptr_result_maybenull_ HV_STATE** State
    )
{
    *State = NULL;
    if (InterlockedCompareExchange(&g_AcceptingRequests, 0, 0) == 0 ||
        !ExAcquireRundownProtection(&g_HypervisorRundown)) {
        return FALSE;
    }
    if (InterlockedCompareExchange(&g_AcceptingRequests, 0, 0) == 0) {
        ExReleaseRundownProtection(&g_HypervisorRundown);
        return FALSE;
    }
    *State = (HV_STATE*)InterlockedCompareExchangePointer(
        (PVOID volatile*)&g_PublishedHypervisor, NULL, NULL);
    return TRUE;
}

static VOID
JsReleasePublishedHypervisor(
    VOID
    )
{
    ExReleaseRundownProtection(&g_HypervisorRundown);
}

static ULONG
JsWireLifecycle(
    _In_ LONG Value
    )
{
    switch (Value) {
    case HV_LIFECYCLE_STARTING: return JohnSmithLifecycleStarting;
    case HV_LIFECYCLE_RUNNING:  return JohnSmithLifecycleRunning;
    case HV_LIFECYCLE_STOPPING: return JohnSmithLifecycleStopping;
    default: return JohnSmithLifecycleStopped;
    }
}

static VOID
JsCopyBackendName(
    _Out_writes_bytes_(JOHNSMITH_BACKEND_NAME_LENGTH) char* Destination,
    _In_opt_z_ PCSTR Source
    )
{
    ULONG index = 0;

    RtlZeroMemory(Destination, JOHNSMITH_BACKEND_NAME_LENGTH);
    if (Source == NULL) {
        return;
    }
    while (index < JOHNSMITH_BACKEND_NAME_LENGTH - 1 &&
           Source[index] != '\0') {
        Destination[index] = Source[index];
        ++index;
    }
}

static NTSTATUS
JsValidateHeader(
    _In_reads_bytes_(BufferLength) const VOID* Buffer,
    _In_ ULONG BufferLength,
    _In_ ULONG ExpectedSize
    )
{
    const JOHNSMITH_REQUEST_HEADER* header;

    if (Buffer == NULL || BufferLength < sizeof(*header) ||
        BufferLength < ExpectedSize) {
        return STATUS_INVALID_PARAMETER;
    }
    header = (const JOHNSMITH_REQUEST_HEADER*)Buffer;
    if (header->Size != ExpectedSize ||
        header->Version != JOHNSMITH_ABI_VERSION ||
        header->Reserved != 0) {
        return STATUS_REVISION_MISMATCH;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
JsHandleStatus(
    _In_reads_bytes_(InputLength) const VOID* Input,
    _In_ ULONG InputLength,
    _Out_writes_bytes_to_opt_(OutputLength, *BytesWritten) VOID* Output,
    _In_ ULONG OutputLength,
    _Out_ PULONG BytesWritten
    )
{
    JOHNSMITH_STATUS_RESPONSE* response;
    HV_STATE* hv;
    NTSTATUS status;

    *BytesWritten = 0;
    status = JsValidateHeader(
        Input, InputLength, sizeof(JOHNSMITH_STATUS_REQUEST));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (Output == NULL || OutputLength < sizeof(*response)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    response = (JOHNSMITH_STATUS_RESPONSE*)Output;
    RtlZeroMemory(response, sizeof(*response));
    response->AbiVersion = JOHNSMITH_ABI_VERSION;

    if (!JsAcquirePublishedHypervisor(&hv)) {
        return STATUS_DELETE_PENDING;
    }
    if (hv == NULL) {
        response->Lifecycle = JohnSmithLifecycleStopped;
        JsCopyBackendName(response->BackendName, NULL);
    } else {
        LONG lifecycle =
            InterlockedCompareExchange(&hv->Lifecycle, 0, 0);
        ULONG running = 0;

        response->Lifecycle = JsWireLifecycle(lifecycle);
        response->CpuCount = hv->CpuCount;
        response->BackendPresent = hv->Backend != NULL ? 1u : 0u;
        JsCopyBackendName(
            response->BackendName,
            hv->Backend != NULL ? hv->Backend->Name : NULL);
        if (hv->Cpus != NULL) {
            ULONG index;
            for (index = 0; index < hv->CpuCount; ++index) {
                if (InterlockedCompareExchange(
                        &hv->Cpus[index].State, 0, 0) == HV_CPU_RUNNING) {
                    ++running;
                }
            }
        }
        response->RunningCpuCount = running;
    }

    JsReleasePublishedHypervisor();
    *BytesWritten = sizeof(*response);
    return STATUS_SUCCESS;
}

static NTSTATUS
JsHandleHookInstall(
    _In_reads_bytes_(InputLength) const VOID* Input,
    _In_ ULONG InputLength,
    _Out_writes_bytes_to_opt_(OutputLength, *BytesWritten) VOID* Output,
    _In_ ULONG OutputLength,
    _Out_ PULONG BytesWritten
    )
{
    const JOHNSMITH_HOOK_INSTALL_REQUEST* request;
    JOHNSMITH_HOOK_INSTALL_RESPONSE* response;
    HV_STATE* hv;
    ULONG hookId = 0;
    NTSTATUS status;

    *BytesWritten = 0;
    status = JsValidateHeader(
        Input, InputLength, sizeof(JOHNSMITH_HOOK_INSTALL_REQUEST));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    request = (const JOHNSMITH_HOOK_INSTALL_REQUEST*)Input;
    if (request->PatchSize == 0 ||
        request->PatchSize > JOHNSMITH_HOOK_MAX_PATCH ||
        (ULONG64)request->PatchOffset + request->PatchSize > PAGE_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Output == NULL || OutputLength < sizeof(*response)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (!JsAcquirePublishedHypervisor(&hv)) {
        return STATUS_DELETE_PENDING;
    }
    if (hv == NULL || hv->Backend == NULL ||
        hv->Backend->HookInstall == NULL) {
        status = STATUS_NOT_SUPPORTED;
        goto Exit;
    }
    status = hv->Backend->HookInstall(
        hv,
        request->GuestPhysicalAddress,
        request->PatchBytes,
        request->PatchOffset,
        request->PatchSize,
        request->Cookie,
        &hookId);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    response = (JOHNSMITH_HOOK_INSTALL_RESPONSE*)Output;
    RtlZeroMemory(response, sizeof(*response));
    response->HookId = hookId;
    *BytesWritten = sizeof(*response);
    status = STATUS_SUCCESS;

Exit:
    JsReleasePublishedHypervisor();
    return status;
}

static NTSTATUS
JsHandleHookRemove(
    _In_reads_bytes_(InputLength) const VOID* Input,
    _In_ ULONG InputLength,
    _Out_ PULONG BytesWritten
    )
{
    const JOHNSMITH_HOOK_REMOVE_REQUEST* request;
    HV_STATE* hv;
    NTSTATUS status;

    *BytesWritten = 0;
    status = JsValidateHeader(
        Input, InputLength, sizeof(JOHNSMITH_HOOK_REMOVE_REQUEST));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    request = (const JOHNSMITH_HOOK_REMOVE_REQUEST*)Input;
    if (request->Reserved != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    if (!JsAcquirePublishedHypervisor(&hv)) {
        return STATUS_DELETE_PENDING;
    }
    if (hv == NULL || hv->Backend == NULL ||
        hv->Backend->HookRemove == NULL) {
        status = STATUS_NOT_SUPPORTED;
    } else {
        status = hv->Backend->HookRemove(hv, request->HookId);
    }
    JsReleasePublishedHypervisor();
    return status;
}

static NTSTATUS
JsHandleHookQuery(
    _In_reads_bytes_(InputLength) const VOID* Input,
    _In_ ULONG InputLength,
    _Out_writes_bytes_to_opt_(OutputLength, *BytesWritten) VOID* Output,
    _In_ ULONG OutputLength,
    _Out_ PULONG BytesWritten
    )
{
    const JOHNSMITH_HOOK_QUERY_REQUEST* request;
    JOHNSMITH_HOOK_QUERY_RESPONSE* response;
    HV_STATE* hv;
    ULONG valid = 0;
    ULONG kind = 0;
    ULONG cookie = 0;
    ULONG64 gpa = 0;
    ULONG64 shadow = 0;
    NTSTATUS status;

    *BytesWritten = 0;
    status = JsValidateHeader(
        Input, InputLength, sizeof(JOHNSMITH_HOOK_QUERY_REQUEST));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    request = (const JOHNSMITH_HOOK_QUERY_REQUEST*)Input;
    if (request->Reserved != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Output == NULL || OutputLength < sizeof(*response)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (!JsAcquirePublishedHypervisor(&hv)) {
        return STATUS_DELETE_PENDING;
    }
    if (hv == NULL || hv->Backend == NULL ||
        hv->Backend->HookQuery == NULL) {
        status = STATUS_NOT_SUPPORTED;
        goto Exit;
    }
    status = hv->Backend->HookQuery(
        hv, request->HookId, &valid, &kind, &cookie, &gpa, &shadow);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    response = (JOHNSMITH_HOOK_QUERY_RESPONSE*)Output;
    RtlZeroMemory(response, sizeof(*response));
    response->Valid = valid;
    response->Kind = kind;
    response->Cookie = cookie;
    response->GuestPhysicalAddress = gpa;
    response->ShadowHostPhysicalAddress = shadow;
    *BytesWritten = sizeof(*response);
    status = STATUS_SUCCESS;

Exit:
    JsReleasePublishedHypervisor();
    return status;
}

_Function_class_(DRIVER_DISPATCH)
_Dispatch_type_(IRP_MJ_CREATE)
_Dispatch_type_(IRP_MJ_CLOSE)
_Dispatch_type_(IRP_MJ_CLEANUP)
_IRQL_requires_max_(APC_LEVEL)
static NTSTATUS
JsDispatchCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

_Function_class_(DRIVER_DISPATCH)
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)
_IRQL_requires_max_(APC_LEVEL)
static NTSTATUS
JsDispatchDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
{
    PIO_STACK_LOCATION stack;
    ULONG code;
    ULONG inputLength;
    ULONG outputLength;
    ULONG bytesWritten = 0;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

    UNREFERENCED_PARAMETER(DeviceObject);
    stack = IoGetCurrentIrpStackLocation(Irp);
    code = stack->Parameters.DeviceIoControl.IoControlCode;
    inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;

    switch (code) {
    case IOCTL_JOHNSMITH_STATUS:
        status = JsHandleStatus(
            Irp->AssociatedIrp.SystemBuffer,
            inputLength,
            Irp->AssociatedIrp.SystemBuffer,
            outputLength,
            &bytesWritten);
        break;
    case IOCTL_JOHNSMITH_HOOK_INSTALL:
        status = JsHandleHookInstall(
            Irp->AssociatedIrp.SystemBuffer,
            inputLength,
            Irp->AssociatedIrp.SystemBuffer,
            outputLength,
            &bytesWritten);
        break;
    case IOCTL_JOHNSMITH_HOOK_REMOVE:
        status = JsHandleHookRemove(
            Irp->AssociatedIrp.SystemBuffer,
            inputLength,
            &bytesWritten);
        break;
    case IOCTL_JOHNSMITH_HOOK_QUERY:
        status = JsHandleHookQuery(
            Irp->AssociatedIrp.SystemBuffer,
            inputLength,
            Irp->AssociatedIrp.SystemBuffer,
            outputLength,
            &bytesWritten);
        break;
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = NT_SUCCESS(status) ? bytesWritten : 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static VOID
JsDeviceInstallDispatch(
    _Inout_ PDRIVER_OBJECT DriverObject
    )
{

#pragma warning(push)
#pragma warning(disable: 28175)
    DriverObject->MajorFunction[IRP_MJ_CREATE] = JsDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = JsDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = JsDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] =
        JsDispatchDeviceControl;
#pragma warning(pop)
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
JsDeviceInitialize(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    UNICODE_STRING deviceName =
        RTL_CONSTANT_STRING(JOHNSMITH_DEVICE_NAME_W);
    UNICODE_STRING linkName =
        RTL_CONSTANT_STRING(JOHNSMITH_LINK_NAME_W);
    PDEVICE_OBJECT deviceObject = NULL;
    NTSTATUS status;

    if (g_DeviceObject != NULL) {
        return STATUS_ALREADY_INITIALIZED;
    }

    ExInitializeRundownProtection(&g_HypervisorRundown);
    InterlockedExchange(&g_AcceptingRequests, 0);
    (VOID)InterlockedExchangePointer(
        (PVOID volatile*)&g_PublishedHypervisor, NULL);

    status = IoCreateDeviceSecure(
        DriverObject,
        0,
        &deviceName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        (PCUNICODE_STRING)&JsDeviceSddl,
        (LPCGUID)&JsDeviceClassGuid,
        &deviceObject);
    if (!NT_SUCCESS(status)) {
        HV_LOG_ERROR(
            "IoCreateDeviceSecure failed: 0x%08X.\n", (ULONG)status);
        return status;
    }

    deviceObject->Flags |= DO_BUFFERED_IO;

    status = IoCreateSymbolicLink(&linkName, &deviceName);
    if (!NT_SUCCESS(status)) {
        HV_LOG_ERROR(
            "IoCreateSymbolicLink failed: 0x%08X.\n", (ULONG)status);
        IoDeleteDevice(deviceObject);
        return status;
    }

    JsDeviceInstallDispatch(DriverObject);

    g_SymbolicLink = linkName;
    g_SymbolicLinkCreated = TRUE;
    g_DeviceObject = deviceObject;
    InterlockedExchange(&g_AcceptingRequests, 1);
    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
JsDeviceTeardown(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);

    JsDevicePublishHypervisor(NULL);
    if (g_SymbolicLinkCreated) {
        (VOID)IoDeleteSymbolicLink(&g_SymbolicLink);
        g_SymbolicLinkCreated = FALSE;
    }
    if (g_DeviceObject != NULL) {
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
    }
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
JsDevicePublishHypervisor(
    _In_opt_ HV_STATE* State
    )
{
    HV_STATE* previous = (HV_STATE*)InterlockedExchangePointer(
        (PVOID volatile*)&g_PublishedHypervisor, State);
    if (State != NULL) {
        HV_LOG_INFO(
            "device: published backend %s, %lu CPUs.\n",
            State->Backend != NULL ? State->Backend->Name : "(none)",
            State->CpuCount);
    } else {
        if (InterlockedExchange(&g_AcceptingRequests, 0) != 0) {
            ExWaitForRundownProtectionRelease(&g_HypervisorRundown);
        }
        if (previous != NULL) {
            HV_LOG_INFO("device: cleared published hypervisor.\n");
        }
    }
}
