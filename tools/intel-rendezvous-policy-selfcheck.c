#include <assert.h>

#include "../src/intel/intel_rendezvous_policy.h"

int
main(void)
{
    unsigned budget;
    unsigned index;
    unsigned long long oldOffset = 1000ull;
    unsigned long long frozenStart = 5000ull;
    unsigned long long finish = 5200ull;
    unsigned long long resume = 5300ull;
    unsigned long long newOffset;

    assert(INTEL_RENDEZVOUS_ICR_LOW == 0x000C4400u);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_CPUID, 0, 0) == INTEL_POLICY_MANDATORY);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_RDTSC, 0, 0) == INTEL_POLICY_MANDATORY);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_EPT_VIOLATION, 0, 0) ==
        INTEL_POLICY_MANDATORY);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_RDTSCP, 0, 0) == INTEL_POLICY_MANDATORY);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_RDMSR, INTEL_POLICY_IA32_TSC, 0) ==
        INTEL_POLICY_MANDATORY);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_RDMSR, 0, 1) == INTEL_POLICY_CONDITIONAL);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_RDMSR, 0, 0) == INTEL_POLICY_NONE);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_CR_ACCESS, 0, 1) == INTEL_POLICY_CONDITIONAL);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_CR_ACCESS, 0, 0) == INTEL_POLICY_NONE);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_WRMSR, 0, 1) == INTEL_POLICY_CONDITIONAL);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_WRMSR, 0, 0) == INTEL_POLICY_NONE);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_GDTR_IDTR, 0, 1) == INTEL_POLICY_CONDITIONAL);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_LDTR_TR, 0, 1) == INTEL_POLICY_CONDITIONAL);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_EXTERNAL_INTERRUPT, 0, 8) ==
        INTEL_POLICY_EXCLUDED);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_MTF, 0, 8) == INTEL_POLICY_EXCLUDED);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_PREEMPTION_TIMER, 0, 8) ==
        INTEL_POLICY_EXCLUDED);
    assert(IntelRendezvousClassifyPolicy(
        0xFFFFFFFFu, 0, 8) == INTEL_POLICY_NONE);

    budget = INTEL_RENDEZVOUS_HOOK_BUDGET;
    for (index = 0; index < INTEL_RENDEZVOUS_HOOK_BUDGET; ++index) {
        budget = IntelRendezvousConsumeBudget(
            INTEL_POLICY_EXIT_CPUID, budget);
        assert(budget == INTEL_RENDEZVOUS_HOOK_BUDGET - index - 1);
    }
    assert(budget == 0);
    assert(IntelRendezvousConsumeBudget(
        INTEL_POLICY_EXIT_CPUID, budget) == 0);
    assert(IntelRendezvousConsumeBudget(
        INTEL_POLICY_EXIT_EXTERNAL_INTERRUPT, 8) == 8);
    assert(IntelRendezvousConsumeBudget(
        INTEL_POLICY_EXIT_MTF, 8) == 8);
    assert(IntelRendezvousConsumeBudget(
        INTEL_POLICY_EXIT_PREEMPTION_TIMER, 8) == 8);
    assert(IntelRendezvousReloadBudget() == 8);
    assert(INTEL_POLICY_REQUIRED_PIN_CONTROLS ==
           ((1u << 3) | (1u << 5)));
    assert(INTEL_POLICY_REQUIRED_PRIMARY_CONTROLS ==
           ((1u << 31) | (1u << 28) | (1u << 3)));
    assert(INTEL_POLICY_REQUIRED_DYNAMIC_PRIMARY_CONTROLS == (1u << 22));

    assert(IntelRendezvousApplyTscDelta(1000ull, 200ull) == 800ull);
    assert(IntelRendezvousFrozenGuestTsc(5000ull, 1000ull) == 6000ull);
    assert(IntelRendezvousApplyTscDelta(0ull, 1ull) == ~0ull);
    assert(IntelRendezvousFrozenGuestTsc(frozenStart, oldOffset) == 6000ull);
    newOffset = IntelRendezvousApplyTscDelta(
        oldOffset, finish - frozenStart);
    assert(resume >= finish);
    assert(resume + newOffset >= frozenStart + oldOffset);
    return 0;
}
