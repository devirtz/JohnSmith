#pragma once

#include "hv.h"

/* The module creates and owns its single page; callers cannot supply one. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
HvIntrospectionStart(
    _Inout_ HV_STATE* State
    );

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
HvIntrospectionStop(
    _Inout_ HV_STATE* State
    );
