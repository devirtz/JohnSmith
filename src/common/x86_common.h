#pragma once

#include "hv.h"

ULONG64
HvX86GetSlatMapLimit(
    VOID
    );

BOOLEAN
HvX86RangeIsRam(
    _In_opt_ PPHYSICAL_MEMORY_RANGE Ranges,
    _In_ ULONG64 Base,
    _In_ ULONG64 Size
    );

BOOLEAN
HvX86RangeIntersectsRam(
    _In_opt_ PPHYSICAL_MEMORY_RANGE Ranges,
    _In_ ULONG64 Base,
    _In_ ULONG64 Size
    );

BOOLEAN
HvX86IsValidXcr0(
    _In_ ULONG64 Value
    );
