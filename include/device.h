#pragma once

#include <ntddk.h>

#include "hv.h"

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
JsDeviceInitialize(
    _In_ PDRIVER_OBJECT DriverObject
    );

_IRQL_requires_(PASSIVE_LEVEL)
VOID
JsDeviceTeardown(
    _In_ PDRIVER_OBJECT DriverObject
    );

_IRQL_requires_(PASSIVE_LEVEL)
VOID
JsDevicePublishHypervisor(
    _In_opt_ HV_STATE* State
    );
