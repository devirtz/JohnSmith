#include "x86_common.h"

#include <intrin.h>

#define XCR0_X87            (1ull << 0)
#define XCR0_SSE            (1ull << 1)
#define XCR0_AVX            (1ull << 2)
#define XCR0_MPX_MASK       (3ull << 3)
#define XCR0_AVX512_MASK    (7ull << 5)
#define XCR0_TILECFG        (1ull << 17)
#define XCR0_TILEDATA       (1ull << 18)

ULONG64
HvX86GetSlatMapLimit(
    VOID
    )
{
    int registers[4];
    ULONG physicalAddressBits;
    ULONG64 processorLimit;

    __cpuid(registers, 0x80000000);
    if ((ULONG)registers[0] < 0x80000008u) {
        processorLimit = 1ull << 36;
    } else {
        __cpuid(registers, 0x80000008);
        physicalAddressBits = (ULONG)registers[0] & 0xffu;
        processorLimit = physicalAddressBits >= 63
            ? MAXLONGLONG
            : 1ull << physicalAddressBits;
    }

    return processorLimit < HV_SLAT_MAXIMUM_ADDRESS
        ? processorLimit
        : HV_SLAT_MAXIMUM_ADDRESS;
}

BOOLEAN
HvX86RangeIsRam(
    _In_opt_ PPHYSICAL_MEMORY_RANGE Ranges,
    _In_ ULONG64 Base,
    _In_ ULONG64 Size
    )
{
    ULONG index;

    if (Ranges == NULL || Size == 0 || Base > MAXULONGLONG - Size) {
        return FALSE;
    }

    for (index = 0; Ranges[index].NumberOfBytes.QuadPart != 0; ++index) {
        ULONG64 rangeBase = (ULONG64)Ranges[index].BaseAddress.QuadPart;
        ULONG64 rangeSize = (ULONG64)Ranges[index].NumberOfBytes.QuadPart;
        ULONG64 offset;

        if (Base < rangeBase) {
            continue;
        }
        offset = Base - rangeBase;
        if (offset <= rangeSize && Size <= rangeSize - offset) {
            return TRUE;
        }
    }

    return FALSE;
}

BOOLEAN
HvX86RangeIntersectsRam(
    _In_opt_ PPHYSICAL_MEMORY_RANGE Ranges,
    _In_ ULONG64 Base,
    _In_ ULONG64 Size
    )
{
    ULONG index;
    ULONG64 end;

    if (Ranges == NULL || Size == 0 || Base > MAXULONGLONG - Size) {
        return FALSE;
    }
    end = Base + Size;

    for (index = 0; Ranges[index].NumberOfBytes.QuadPart != 0; ++index) {
        ULONG64 rangeBase = (ULONG64)Ranges[index].BaseAddress.QuadPart;
        ULONG64 rangeSize = (ULONG64)Ranges[index].NumberOfBytes.QuadPart;
        ULONG64 rangeEnd = rangeBase > MAXULONGLONG - rangeSize
            ? MAXULONGLONG
            : rangeBase + rangeSize;

        if (Base < rangeEnd && rangeBase < end) {
            return TRUE;
        }
    }

    return FALSE;
}

BOOLEAN
HvX86IsValidXcr0(
    _In_ ULONG64 Value
    )
{
    /* Intel SDM rev. 092, XSETBV and Chapter 13 dependency rules. */
    int registers[4];
    ULONG64 supported;
    ULONG64 avx512;

    __cpuid(registers, 0);
    if ((ULONG)registers[0] < 0xDu) {
        return FALSE;
    }

    __cpuidex(registers, 0xD, 0);
    supported = ((ULONG64)(ULONG)registers[3] << 32) |
                (ULONG)registers[0];
    if ((Value & ~supported) != 0 || (Value & XCR0_X87) == 0) {
        return FALSE;
    }
    if ((Value & XCR0_AVX) != 0 && (Value & XCR0_SSE) == 0) {
        return FALSE;
    }
    if ((Value & XCR0_MPX_MASK) != 0 &&
        (Value & XCR0_MPX_MASK) != XCR0_MPX_MASK) {
        return FALSE;
    }

    avx512 = Value & XCR0_AVX512_MASK;
    if (avx512 != 0 &&
        (avx512 != XCR0_AVX512_MASK || (Value & XCR0_AVX) == 0)) {
        return FALSE;
    }
    if ((Value & XCR0_TILEDATA) != 0 && (Value & XCR0_TILECFG) == 0) {
        return FALSE;
    }

    return TRUE;
}
