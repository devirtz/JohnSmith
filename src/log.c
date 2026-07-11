#include "hv_log.h"

#include <stdarg.h>

static PCSTR
HvLogPrefix(
    _In_ HV_LOG_LEVEL Level
    )
{
    switch (Level) {
    case HvLogLevelTrace:   return "[JohnSmith][TRACE] ";
    case HvLogLevelInfo:    return "[JohnSmith][INFO ] ";
    case HvLogLevelWarning: return "[JohnSmith][WARN ] ";
    case HvLogLevelError:   return "[JohnSmith][ERROR] ";
    default:                return "[JohnSmith][?????] ";
    }
}

static ULONG
HvLogDebuggerLevel(
    _In_ HV_LOG_LEVEL Level
    )
{
    /* Error-level messages are visible with the default IHVDRIVER mask. */
    return Level == HvLogLevelTrace
        ? DPFLTR_TRACE_LEVEL
        : DPFLTR_ERROR_LEVEL;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
__cdecl
HvLogWrite(
    _In_ HV_LOG_LEVEL Level,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...
    )
{
    va_list arguments;

    if (Format == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    va_start(arguments, Format);
    (VOID)vDbgPrintExWithPrefix(
        HvLogPrefix(Level),
        DPFLTR_IHVDRIVER_ID,
        HvLogDebuggerLevel(Level),
        Format,
        arguments);
    va_end(arguments);
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
HvLogStartBanner(
    _In_z_ PCSTR BackendName,
    _In_ ULONG CpuCount
    )
{
    static const CHAR mode[] = "Bare metal";

    if (BackendName == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    (VOID)DbgPrintEx(
        DPFLTR_IHVDRIVER_ID,
        DPFLTR_ERROR_LEVEL,
        "\n"
        "============================================================\n"
        "  JohnSmith Windows x64 Hypervisor\n"
        "  Backend : %s\n"
        "  CPUs    : %lu\n"
        "  Mode    : %s\n"
        "  Status  : RUNNING\n"
        "============================================================\n",
        BackendName,
        CpuCount,
        mode);
}
