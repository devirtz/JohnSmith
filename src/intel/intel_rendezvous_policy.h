#pragma once

#define INTEL_RENDEZVOUS_ICR_LOW          0x000C4400u
#define INTEL_RENDEZVOUS_HOOK_BUDGET      8u

#define INTEL_POLICY_REQUIRED_PIN_CONTROLS     ((1u << 3) | (1u << 5))
#define INTEL_POLICY_REQUIRED_PRIMARY_CONTROLS ((1u << 31) | (1u << 28) | (1u << 3))
#define INTEL_POLICY_REQUIRED_DYNAMIC_PRIMARY_CONTROLS (1u << 22)

#define INTEL_POLICY_IA32_TSC             0x00000010u

#define INTEL_POLICY_EXIT_EXTERNAL_INTERRUPT 1u
#define INTEL_POLICY_EXIT_CPUID              10u
#define INTEL_POLICY_EXIT_RDTSC              16u
#define INTEL_POLICY_EXIT_CR_ACCESS          28u
#define INTEL_POLICY_EXIT_RDMSR              31u
#define INTEL_POLICY_EXIT_WRMSR              32u
#define INTEL_POLICY_EXIT_MTF                37u
#define INTEL_POLICY_EXIT_GDTR_IDTR          46u
#define INTEL_POLICY_EXIT_LDTR_TR            47u
#define INTEL_POLICY_EXIT_EPT_VIOLATION      48u
#define INTEL_POLICY_EXIT_RDTSCP             51u
#define INTEL_POLICY_EXIT_PREEMPTION_TIMER   52u

static inline int
IntelVmxControlsAreToggleable(
    unsigned long long capability,
    unsigned mask
    )
{
    return ((unsigned)capability & mask) == 0 &&
           ((unsigned)(capability >> 32) & mask) == mask;
}

static inline int
IntelVmxTransitionStateCanPersist(
    unsigned long long exitCapability,
    unsigned long long entryCapability,
    unsigned exitControls,
    unsigned entryControls
    )
{
    return ((unsigned)exitCapability & exitControls) == 0 &&
           ((unsigned)entryCapability & entryControls) == 0;
}

typedef enum {
    INTEL_POLICY_NONE = 0,
    INTEL_POLICY_MANDATORY,
    INTEL_POLICY_CONDITIONAL,
    INTEL_POLICY_EXCLUDED
} INTEL_RENDEZVOUS_POLICY;

static inline INTEL_RENDEZVOUS_POLICY
IntelRendezvousClassifyPolicy(
    unsigned reason,
    unsigned msr,
    unsigned hookBudget
    )
{
    switch (reason) {
    case INTEL_POLICY_EXIT_EXTERNAL_INTERRUPT:
    case INTEL_POLICY_EXIT_MTF:
    case INTEL_POLICY_EXIT_PREEMPTION_TIMER:
        return INTEL_POLICY_EXCLUDED;
    case INTEL_POLICY_EXIT_CPUID:
        return hookBudget != 0 ? INTEL_POLICY_CONDITIONAL : INTEL_POLICY_NONE;
    case INTEL_POLICY_EXIT_RDTSC:
    case INTEL_POLICY_EXIT_EPT_VIOLATION:
    case INTEL_POLICY_EXIT_RDTSCP:
        return INTEL_POLICY_MANDATORY;
    case INTEL_POLICY_EXIT_RDMSR:
        if (msr == INTEL_POLICY_IA32_TSC) {
            return INTEL_POLICY_MANDATORY;
        }
        return hookBudget != 0 ? INTEL_POLICY_CONDITIONAL : INTEL_POLICY_NONE;
    case INTEL_POLICY_EXIT_CR_ACCESS:
    case INTEL_POLICY_EXIT_WRMSR:
    case INTEL_POLICY_EXIT_GDTR_IDTR:
    case INTEL_POLICY_EXIT_LDTR_TR:
        return hookBudget != 0 ? INTEL_POLICY_CONDITIONAL : INTEL_POLICY_NONE;
    default:
        return INTEL_POLICY_NONE;
    }
}

static inline unsigned
IntelRendezvousConsumeBudget(
    unsigned reason,
    unsigned hookBudget
    )
{
    if (hookBudget == 0 ||
        reason == INTEL_POLICY_EXIT_EXTERNAL_INTERRUPT ||
        reason == INTEL_POLICY_EXIT_MTF ||
        reason == INTEL_POLICY_EXIT_PREEMPTION_TIMER) {
        return hookBudget;
    }
    return hookBudget - 1;
}

static inline unsigned
IntelRendezvousReloadBudget(void)
{
    return INTEL_RENDEZVOUS_HOOK_BUDGET;
}

static inline unsigned long long
IntelRendezvousApplyTscDelta(
    unsigned long long offset,
    unsigned long long delta
    )
{
    return offset - delta;
}

static inline unsigned long long
IntelRendezvousFrozenGuestTsc(
    unsigned long long frozenStart,
    unsigned long long offset
    )
{
    return frozenStart + offset;
}
