#pragma once

#include <ntddk.h>

typedef enum _HV_LOG_LEVEL {
    HvLogLevelTrace = 0,
    HvLogLevelInfo,
    HvLogLevelWarning,
    HvLogLevelError
} HV_LOG_LEVEL;

/* Lifecycle logger only: do not call from VM-exit or IPI-level paths. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID
__cdecl
HvLogWrite(
    _In_ HV_LOG_LEVEL Level,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...
    );

_IRQL_requires_(PASSIVE_LEVEL)
VOID
HvLogStartBanner(
    _In_z_ PCSTR BackendName,
    _In_ ULONG CpuCount
    );

#define HV_LOG_TRACE(...)   HvLogWrite(HvLogLevelTrace, __VA_ARGS__)
#define HV_LOG_INFO(...)    HvLogWrite(HvLogLevelInfo, __VA_ARGS__)
#define HV_LOG_WARNING(...) HvLogWrite(HvLogLevelWarning, __VA_ARGS__)
#define HV_LOG_ERROR(...)   HvLogWrite(HvLogLevelError, __VA_ARGS__)
