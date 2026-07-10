#include "hv.h"

DRIVER_INITIALIZE DriverEntry;

static HV_STATE* g_Hypervisor;

_IRQL_requires_(PASSIVE_LEVEL)
static VOID
JohnSmithUnload(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);

    if (g_Hypervisor != NULL) {
        HvStop(g_Hypervisor);
        g_Hypervisor = NULL;
    }
}

_Use_decl_annotations_
NTSTATUS
DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverUnload = JohnSmithUnload;
    status = HvStart(&g_Hypervisor);
    if (!NT_SUCCESS(status)) {
        DriverObject->DriverUnload = NULL;
        g_Hypervisor = NULL;
    }

    return status;
}
