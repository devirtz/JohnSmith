#include "intel_internal.h"

#define INTEL_RENDEZVOUS_TIMEOUT_US 2000ull
#define INTEL_RENDEZVOUS_LEAD_US       2ull

static ULONG64
IntelRendezvousTscFrequency(
    VOID
    )
{
    LARGE_INTEGER counterFrequency;
    LARGE_INTEGER counterStart;
    LARGE_INTEGER counterEnd;
    ULONG64 counterTarget;
    ULONG64 counterDelta;
    ULONG64 tscStart;
    ULONG64 tscEnd;
    ULONG64 frequency;
    int registers[4];
    ULONG maximumLeaf;
    ULONG baseMegahertz;

    __cpuid(registers, 0);
    maximumLeaf = (ULONG)registers[0];
    if (maximumLeaf >= 0x15u) {
        __cpuidex(registers, 0x15, 0);
        if (registers[0] != 0 && registers[1] != 0 && registers[2] != 0) {
            frequency = ((ULONG64)(ULONG)registers[2] *
                         (ULONG64)(ULONG)registers[1]) /
                        (ULONG64)(ULONG)registers[0];
            if (frequency != 0) {
                return frequency;
            }
        }
    }
    if (maximumLeaf >= 0x16u) {
        __cpuidex(registers, 0x16, 0);
        baseMegahertz = (ULONG)registers[0] & 0xffffu;
        if (baseMegahertz != 0) {
            return (ULONG64)baseMegahertz * 1000000ull;
        }
    }

    counterStart = KeQueryPerformanceCounter(&counterFrequency);
    if (counterFrequency.QuadPart <= 0) {
        return 0;
    }
    counterTarget = (ULONG64)counterFrequency.QuadPart / 10000ull;
    if (counterTarget == 0) {
        return 0;
    }
    tscStart = __rdtsc();
    do {
        _mm_pause();
        counterEnd = KeQueryPerformanceCounter(NULL);
    } while ((ULONG64)(counterEnd.QuadPart - counterStart.QuadPart) <
             counterTarget);
    tscEnd = __rdtsc();
    counterDelta = (ULONG64)(counterEnd.QuadPart - counterStart.QuadPart);
    if (tscEnd <= tscStart || counterDelta == 0) {
        return 0;
    }
    return ((tscEnd - tscStart) * (ULONG64)counterFrequency.QuadPart) /
           counterDelta;
}

static BOOLEAN
IntelRendezvousInvariantTsc(
    VOID
    )
{
    int registers[4];

    __cpuid(registers, (int)0x80000000u);
    if ((ULONG)registers[0] < 0x80000007u) {
        return FALSE;
    }
    __cpuid(registers, (int)0x80000007u);
    return ((ULONG)registers[3] & (1u << 8)) != 0;
}

static BOOLEAN
IntelRendezvousDeadlineExpired(
    _In_ ULONG64 Deadline
    )
{
    return (LONG64)(__rdtsc() - Deadline) >= 0;
}

static BOOLEAN
IntelRendezvousWaitForXapicIcr(
    _In_ const INTEL_BACKEND_CONTEXT* Backend
    )
{
    ULONG64 deadline;

    if (Backend->XapicBase == NULL || Backend->RendezvousTimeoutTicks == 0) {
        return FALSE;
    }
    deadline = __rdtsc() + Backend->RendezvousTimeoutTicks;
    while ((Backend->XapicBase[XAPIC_ICR_LOW_OFFSET / sizeof(ULONG)] &
            XAPIC_ICR_DELIVERY_STATUS) != 0) {
        if (IntelRendezvousDeadlineExpired(deadline)) {
            return FALSE;
        }
        _mm_pause();
    }
    return TRUE;
}

static VOID
IntelRendezvousBroadcastNmi(
    _In_ const INTEL_BACKEND_CONTEXT* Backend
    )
{
    KeMemoryBarrier();
    if (Backend->ApicMode == INTEL_APIC_MODE_X2APIC) {
        __writemsr(IA32_X2APIC_ICR, INTEL_RENDEZVOUS_ICR_LOW);
    } else {
        Backend->XapicBase[XAPIC_ICR_LOW_OFFSET / sizeof(ULONG)] =
            INTEL_RENDEZVOUS_ICR_LOW;
    }
}

_Function_class_(NMI_CALLBACK)
static BOOLEAN
IntelRendezvousNmiCallback(
    _In_opt_ PVOID CallbackContext,
    _In_ BOOLEAN Handled
    )
{
    INTEL_BACKEND_CONTEXT* backend;
    INTEL_CPU_CONTEXT* context;
    HV_STATE* state;
    PROCESSOR_NUMBER number;
    ULONG index;

    backend = (INTEL_BACKEND_CONTEXT*)CallbackContext;
    if (backend == NULL || backend->State == NULL) {
        return Handled;
    }
    state = backend->State;
    if (state->BackendContext != backend || state->Cpus == NULL) {
        return Handled;
    }
    KeGetCurrentProcessorNumberEx(&number);
    index = KeGetProcessorIndexFromNumber(&number);
    if (index == INVALID_PROCESSOR_INDEX || index >= state->CpuCount ||
        InterlockedCompareExchange(&state->Cpus[index].State, 0, 0) !=
            HV_CPU_RUNNING) {
        return Handled;
    }
    context = (INTEL_CPU_CONTEXT*)state->Cpus[index].VendorContext;
    if (context == NULL || !IntelRendezvousConsumeExpectedNmi(context)) {
        return Handled;
    }
    (VOID)IntelRendezvousJoinActive(context);
    return TRUE;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
IntelRendezvousPrepare(
    _Inout_ INTEL_BACKEND_CONTEXT* Backend,
    _Inout_ HV_STATE* State
    )
{
    PHYSICAL_ADDRESS apicPhysical;
    ULONG64 apicBase;
    ULONG64 ticksPerMicrosecond;

    if (Backend == NULL || State == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (!IntelRendezvousInvariantTsc()) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }
    Backend->TscFrequency = IntelRendezvousTscFrequency();
    ticksPerMicrosecond = Backend->TscFrequency / 1000000ull;
    Backend->RendezvousTimeoutTicks =
        ticksPerMicrosecond * INTEL_RENDEZVOUS_TIMEOUT_US;
    Backend->RendezvousLeadTicks =
        ticksPerMicrosecond * INTEL_RENDEZVOUS_LEAD_US;
    if (Backend->TscFrequency == 0 || Backend->RendezvousTimeoutTicks == 0 ||
        Backend->RendezvousLeadTicks == 0) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }

    apicBase = __readmsr(IA32_APIC_BASE);
    if ((apicBase & IA32_APIC_BASE_ENABLE) == 0) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }
    Backend->State = State;
    Backend->Rendezvous.OwnerProcessor = INVALID_PROCESSOR_INDEX;
    InterlockedExchange(&Backend->Rendezvous.Phase, INTEL_RENDEZVOUS_IDLE);
    if ((apicBase & IA32_APIC_BASE_X2APIC) != 0) {
        Backend->ApicMode = INTEL_APIC_MODE_X2APIC;
    } else {
        Backend->ApicMode = INTEL_APIC_MODE_XAPIC;
        apicPhysical.QuadPart =
            (LONGLONG)(apicBase & IA32_APIC_BASE_ADDRESS_MASK);
        Backend->XapicBase = (volatile ULONG*)MmMapIoSpaceEx(
            apicPhysical, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
        if (Backend->XapicBase == NULL) {
            Backend->State = NULL;
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    Backend->NmiCallbackHandle =
        KeRegisterNmiCallback(IntelRendezvousNmiCallback, Backend);
    if (Backend->NmiCallbackHandle == NULL) {
        if (Backend->XapicBase != NULL) {
            MmUnmapIoSpace((PVOID)Backend->XapicBase, PAGE_SIZE);
            Backend->XapicBase = NULL;
        }
        Backend->State = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
IntelRendezvousFree(
    _Inout_ INTEL_BACKEND_CONTEXT* Backend
    )
{
    LONG phase;

    if (Backend == NULL) {
        return;
    }
    phase = InterlockedCompareExchange(&Backend->Rendezvous.Phase, 0, 0);
    if (phase != INTEL_RENDEZVOUS_IDLE) {
        KeBugCheckEx(
            HYPERVISOR_ERROR,
            INTEL_BUGCHECK_RENDEZVOUS,
            (ULONG_PTR)phase,
            Backend->Rendezvous.OwnerProcessor,
            (ULONG_PTR)Backend->Rendezvous.Epoch);
    }
    if (Backend->NmiCallbackHandle != NULL) {
        if (!KeDeregisterNmiCallback(Backend->NmiCallbackHandle)) {
            KeBugCheckEx(
                HYPERVISOR_ERROR,
                INTEL_BUGCHECK_RENDEZVOUS,
                INTEL_RENDEZVOUS_IDLE,
                Backend->Rendezvous.OwnerProcessor,
                (ULONG_PTR)Backend->Rendezvous.Epoch);
        }
        Backend->NmiCallbackHandle = NULL;
    }
    if (Backend->XapicBase != NULL) {
        MmUnmapIoSpace((PVOID)Backend->XapicBase, PAGE_SIZE);
        Backend->XapicBase = NULL;
    }
    Backend->State = NULL;
}

static LONG
IntelRendezvousReadPhase(
    _In_ const INTEL_BACKEND_CONTEXT* Backend
    )
{
    return InterlockedCompareExchange(
        (volatile LONG*)&Backend->Rendezvous.Phase, 0, 0);
}

static LONG64
IntelRendezvousReadEpoch(
    _In_ const INTEL_BACKEND_CONTEXT* Backend
    )
{
    return InterlockedCompareExchange64(
        (volatile LONG64*)&Backend->Rendezvous.Epoch, 0, 0);
}

static INTEL_BACKEND_CONTEXT*
IntelRendezvousBackend(
    _In_opt_ const INTEL_CPU_CONTEXT* Context
    )
{
    return Context == NULL
        ? NULL
        : (INTEL_BACKEND_CONTEXT*)Context->BackendContext;
}

BOOLEAN
IntelRendezvousConsumeExpectedNmi(
    _Inout_ INTEL_CPU_CONTEXT* Context
    )
{
    LONG64 consumed;
    LONG64 expected;

    if (Context == NULL) {
        return FALSE;
    }
    for (;;) {
        expected = InterlockedCompareExchange64(
            &Context->RendezvousExpectedNmiEpoch, 0, 0);
        consumed = InterlockedCompareExchange64(
            &Context->RendezvousConsumedNmiEpoch, 0, 0);
        if (expected <= consumed) {
            return FALSE;
        }
        if (InterlockedCompareExchange64(
                &Context->RendezvousConsumedNmiEpoch,
                expected,
                consumed) == consumed) {
            return TRUE;
        }
        _mm_pause();
    }
}

static ULONG
IntelRendezvousPublishParticipants(
    _Inout_ INTEL_BACKEND_CONTEXT* Backend,
    _In_ ULONG OwnerProcessor,
    _In_ LONG64 Epoch
    )
{
    INTEL_CPU_CONTEXT* context;
    HV_STATE* state;
    ULONG count = 0;
    ULONG index;

    state = Backend->State;
    for (index = 0; index < state->CpuCount; ++index) {
        if (InterlockedCompareExchange(&state->Cpus[index].State, 0, 0) !=
            HV_CPU_RUNNING) {
            continue;
        }
        context = (INTEL_CPU_CONTEXT*)state->Cpus[index].VendorContext;
        if (context == NULL) {
            continue;
        }
        ++count;
        if (index != OwnerProcessor) {
            InterlockedExchange64(
                &context->RendezvousExpectedNmiEpoch, Epoch);
        }
    }
    return count;
}

static BOOLEAN
IntelRendezvousWaitForJoinGuards(
    _In_ const INTEL_BACKEND_CONTEXT* Backend,
    _In_ ULONG64 Deadline
    )
{
    INTEL_CPU_CONTEXT* context;
    HV_STATE* state;
    BOOLEAN guarded;
    ULONG index;

    state = Backend->State;
    for (;;) {
        guarded = FALSE;
        for (index = 0; index < state->CpuCount; ++index) {
            if (InterlockedCompareExchange(
                    &state->Cpus[index].State, 0, 0) != HV_CPU_RUNNING) {
                continue;
            }
            context = (INTEL_CPU_CONTEXT*)state->Cpus[index].VendorContext;
            if (context != NULL &&
                InterlockedCompareExchange(
                    &context->RendezvousJoinGuard, 0, 0) != 0) {
                guarded = TRUE;
                break;
            }
        }
        if (!guarded) {
            return TRUE;
        }
        if (IntelRendezvousDeadlineExpired(Deadline)) {
            return FALSE;
        }
        _mm_pause();
    }
}

static BOOLEAN
IntelRendezvousSnapshotMatches(
    _In_ const INTEL_BACKEND_CONTEXT* Backend,
    _In_ LONG Phase,
    _In_ LONG64 Epoch
    )
{
    if (IntelRendezvousReadPhase(Backend) != Phase ||
        IntelRendezvousReadEpoch(Backend) != Epoch) {
        return FALSE;
    }
    return IntelRendezvousReadPhase(Backend) == Phase;
}

static BOOLEAN
IntelRendezvousAdvanceEpoch(
    _Inout_ volatile LONG64* Marker,
    _In_ LONG64 Epoch,
    _Out_ LONG64* Prior
    )
{
    LONG64 observed;
    LONG64 previous;

    observed = InterlockedCompareExchange64(Marker, 0, 0);
    for (;;) {
        *Prior = observed;
        if (observed >= Epoch) {
            return FALSE;
        }
        previous = InterlockedCompareExchange64(Marker, Epoch, observed);
        if (previous == observed) {
            return TRUE;
        }
        observed = previous;
        _mm_pause();
    }
}

static VOID
IntelRendezvousAbort(
    _Inout_ INTEL_BACKEND_CONTEXT* Backend,
    _Inout_ volatile LONG64* TimeoutCounter
    )
{
    InterlockedExchange64(&Backend->Rendezvous.CompensationDelta, 0);
    InterlockedExchange64(
        &Backend->Rendezvous.ResumeTsc, (LONG64)__rdtsc());
    InterlockedIncrement64(TimeoutCounter);
    InterlockedExchange(
        &Backend->Rendezvous.Phase, INTEL_RENDEZVOUS_ABORTING);
    Backend->Rendezvous.OwnerProcessor = INVALID_PROCESSOR_INDEX;
    KeMemoryBarrier();
    InterlockedExchange(&Backend->Rendezvous.Phase, INTEL_RENDEZVOUS_IDLE);
}

static VOID
IntelRendezvousApplyOffset(
    _Inout_ INTEL_CPU_CONTEXT* Context,
    _In_ ULONG64 Delta
    )
{
    ULONG64 offset;

    offset = Context->TscOffset - Delta;
    if (!NT_SUCCESS(IntelVmWrite(VMCS_TSC_OFFSET, offset))) {
        KeBugCheckEx(
            HYPERVISOR_ERROR,
            INTEL_BUGCHECK_RENDEZVOUS,
            Context->ProcessorIndex,
            (ULONG_PTR)offset,
            (ULONG_PTR)Delta);
    }
    Context->TscOffset = offset;
}

BOOLEAN
IntelRendezvousJoinActive(
    _Inout_ INTEL_CPU_CONTEXT* Context
    )
{
    INTEL_BACKEND_CONTEXT* backend;
    LONG64 delta;
    LONG64 epoch;
    LONG64 observed;
    LONG phase;
    LONG phaseBeforeGuard;
    BOOLEAN joined = FALSE;
    BOOLEAN result = FALSE;
    BOOLEAN retry;

    backend = IntelRendezvousBackend(Context);
    if (backend == NULL) {
        return FALSE;
    }
    for (;;) {
        retry = FALSE;
        phase = IntelRendezvousReadPhase(backend);
        if (phase == INTEL_RENDEZVOUS_IDLE) {
            return joined;
        }
        if (phase == INTEL_RENDEZVOUS_CLAIMED) {
            if (InterlockedCompareExchange(
                    &Context->RendezvousJoinGuard, 0, 0) != 0) {
                return TRUE;
            }
            _mm_pause();
            continue;
        }
        phaseBeforeGuard = phase;
        if (InterlockedCompareExchange(
                &Context->RendezvousJoinGuard, 1, 0) != 0) {
            return TRUE;
        }

        phase = IntelRendezvousReadPhase(backend);
        epoch = IntelRendezvousReadEpoch(backend);
        if (phase == INTEL_RENDEZVOUS_IDLE) {
            result = joined;
            goto ReleaseGuard;
        }
        if (phase == INTEL_RENDEZVOUS_CLAIMED ||
            phase != phaseBeforeGuard ||
            !IntelRendezvousSnapshotMatches(backend, phase, epoch)) {
            retry = TRUE;
            goto ReleaseGuard;
        }
        if (phase == INTEL_RENDEZVOUS_ABORTING) {
            result = joined;
            goto ReleaseGuard;
        }
        if (backend->Rendezvous.OwnerProcessor == Context->ProcessorIndex) {
            result = joined;
            goto ReleaseGuard;
        }
        if (phase != INTEL_RENDEZVOUS_ACQUIRING) {
            observed = InterlockedCompareExchange64(
                &Context->RendezvousJoinedEpoch, 0, 0);
            result = observed >= epoch ? TRUE : joined;
            goto ReleaseGuard;
        }
        if (!IntelRendezvousAdvanceEpoch(
                &Context->RendezvousJoinedEpoch, epoch, &observed)) {
            result = observed >= epoch ? TRUE : joined;
            goto ReleaseGuard;
        }
        if (!IntelRendezvousSnapshotMatches(
                backend, INTEL_RENDEZVOUS_ACQUIRING, epoch)) {
            result = joined;
            goto ReleaseGuard;
        }
        InterlockedIncrement(&backend->Rendezvous.ArrivedCount);
        joined = TRUE;

        for (;;) {
            if (IntelRendezvousReadEpoch(backend) != epoch) {
                result = joined;
                goto ReleaseGuard;
            }
            phase = IntelRendezvousReadPhase(backend);
            if (phase == INTEL_RENDEZVOUS_IDLE ||
                phase == INTEL_RENDEZVOUS_ABORTING) {
                result = joined;
                goto ReleaseGuard;
            }
            if (phase == INTEL_RENDEZVOUS_PREPARING) {
                if (IntelRendezvousAdvanceEpoch(
                        &Context->RendezvousPreparedEpoch,
                        epoch,
                        &observed)) {
                    if (!IntelRendezvousSnapshotMatches(
                            backend, INTEL_RENDEZVOUS_PREPARING, epoch)) {
                        result = joined;
                        goto ReleaseGuard;
                    }
                    InterlockedIncrement(&backend->Rendezvous.PreparedCount);
                } else if (observed > epoch) {
                    result = joined;
                    goto ReleaseGuard;
                }
            } else if (phase == INTEL_RENDEZVOUS_APPLYING) {
                observed = InterlockedCompareExchange64(
                    &Context->RendezvousAppliedEpoch, 0, 0);
                if (observed < epoch) {
                    delta = InterlockedCompareExchange64(
                        &backend->Rendezvous.CompensationDelta, 0, 0);
                    IntelRendezvousApplyOffset(Context, (ULONG64)delta);
                    if (IntelRendezvousAdvanceEpoch(
                            &Context->RendezvousAppliedEpoch,
                            epoch,
                            &observed)) {
                        if (!IntelRendezvousSnapshotMatches(
                                backend,
                                INTEL_RENDEZVOUS_APPLYING,
                                epoch)) {
                            result = joined;
                            goto ReleaseGuard;
                        }
                        InterlockedIncrement(
                            &backend->Rendezvous.AppliedCount);
                    }
                } else if (observed > epoch) {
                    result = joined;
                    goto ReleaseGuard;
                }
            } else if (phase == INTEL_RENDEZVOUS_RELEASING) {
                ULONG64 resumeTsc = (ULONG64)InterlockedCompareExchange64(
                    &backend->Rendezvous.ResumeTsc, 0, 0);
                while ((LONG64)(__rdtsc() - resumeTsc) < 0) {
                    _mm_pause();
                }
                result = joined;
                goto ReleaseGuard;
            }
            _mm_pause();
        }
ReleaseGuard:
        InterlockedExchange(&Context->RendezvousJoinGuard, 0);
        if (!retry) {
            return result;
        }
        _mm_pause();
    }
}

BOOLEAN
IntelRendezvousBegin(
    _Inout_ INTEL_CPU_CONTEXT* Context
    )
{
    INTEL_BACKEND_CONTEXT* backend;
    HV_STATE* state;
    LONG64 deadline;
    LONG64 epoch;
    ULONG participantCount;

    backend = IntelRendezvousBackend(Context);
    if (backend == NULL || backend->State == NULL) {
        return FALSE;
    }
    state = backend->State;
    if (InterlockedCompareExchange(&state->Lifecycle, 0, 0) !=
        HV_LIFECYCLE_RUNNING) {
        return FALSE;
    }

    for (;;) {
        if (InterlockedCompareExchange(
                &backend->Rendezvous.Phase,
                INTEL_RENDEZVOUS_CLAIMED,
                INTEL_RENDEZVOUS_IDLE) == INTEL_RENDEZVOUS_IDLE) {
            break;
        }
        (VOID)IntelRendezvousJoinActive(Context);
        _mm_pause();
    }

    deadline = (LONG64)(__rdtsc() + backend->RendezvousTimeoutTicks);
    InterlockedExchange64(&backend->Rendezvous.DeadlineTsc, deadline);
    if (!IntelRendezvousWaitForJoinGuards(backend, (ULONG64)deadline)) {
        InterlockedExchange64(&Context->RendezvousOwnedEpoch, 0);
        IntelRendezvousAbort(
            backend, &backend->Rendezvous.AcquisitionTimeouts);
        return FALSE;
    }
    if (backend->ApicMode == INTEL_APIC_MODE_XAPIC &&
        !IntelRendezvousWaitForXapicIcr(backend)) {
        InterlockedExchange64(&Context->RendezvousOwnedEpoch, 0);
        IntelRendezvousAbort(
            backend, &backend->Rendezvous.AcquisitionTimeouts);
        return FALSE;
    }

    epoch = InterlockedIncrement64(&backend->Rendezvous.Epoch);
    backend->Rendezvous.OwnerProcessor = Context->ProcessorIndex;
    participantCount = IntelRendezvousPublishParticipants(
        backend, Context->ProcessorIndex, epoch);
    backend->Rendezvous.ParticipantCount = participantCount;
    InterlockedExchange(&backend->Rendezvous.ArrivedCount, 1);
    InterlockedExchange(&backend->Rendezvous.PreparedCount, 0);
    InterlockedExchange(&backend->Rendezvous.AppliedCount, 0);
    InterlockedExchange64(&backend->Rendezvous.CompensationDelta, 0);
    InterlockedExchange64(&backend->Rendezvous.ResumeTsc, 0);
    deadline = (LONG64)(__rdtsc() + backend->RendezvousTimeoutTicks);
    InterlockedExchange64(&backend->Rendezvous.DeadlineTsc, deadline);
    InterlockedExchange64(&Context->RendezvousOwnedEpoch, epoch);
    KeMemoryBarrier();
    InterlockedExchange(
        &backend->Rendezvous.Phase, INTEL_RENDEZVOUS_ACQUIRING);

    if (participantCount > 1) {
        IntelRendezvousBroadcastNmi(backend);
    }
    while ((ULONG)InterlockedCompareExchange(
               &backend->Rendezvous.ArrivedCount, 0, 0) !=
           participantCount) {
        if (IntelRendezvousDeadlineExpired((ULONG64)deadline)) {
            InterlockedExchange64(&Context->RendezvousOwnedEpoch, 0);
            IntelRendezvousAbort(
                backend, &backend->Rendezvous.AcquisitionTimeouts);
            return FALSE;
        }
        _mm_pause();
    }

    InterlockedExchange64(
        &backend->Rendezvous.FrozenStartTsc, (LONG64)__rdtsc());
    InterlockedExchange(&backend->Rendezvous.Phase, INTEL_RENDEZVOUS_FROZEN);
    return TRUE;
}

VOID
IntelRendezvousFinish(
    _Inout_ INTEL_CPU_CONTEXT* Context
    )
{
    INTEL_BACKEND_CONTEXT* backend;
    LONG64 deadline;
    LONG64 epoch;
    LONG64 frozenStart;
    LONG64 observed;
    ULONG64 delta;
    ULONG64 resumeTsc;

    backend = IntelRendezvousBackend(Context);
    if (backend == NULL) {
        KeBugCheckEx(
            HYPERVISOR_ERROR, INTEL_BUGCHECK_RENDEZVOUS,
            INVALID_PROCESSOR_INDEX, 0, 0);
    }
    epoch = InterlockedCompareExchange64(
        &Context->RendezvousOwnedEpoch, 0, 0);
    if (epoch == 0 ||
        backend->Rendezvous.OwnerProcessor != Context->ProcessorIndex ||
        IntelRendezvousReadEpoch(backend) != epoch) {
        KeBugCheckEx(
            HYPERVISOR_ERROR,
            INTEL_BUGCHECK_RENDEZVOUS,
            Context->ProcessorIndex,
            (ULONG_PTR)epoch,
            (ULONG_PTR)IntelRendezvousReadEpoch(backend));
    }

    frozenStart = InterlockedCompareExchange64(
        &backend->Rendezvous.FrozenStartTsc, 0, 0);
    delta = __rdtsc() - (ULONG64)frozenStart;
    InterlockedExchange64(
        &backend->Rendezvous.CompensationDelta, (LONG64)delta);
    deadline = (LONG64)(__rdtsc() + backend->RendezvousTimeoutTicks);
    InterlockedExchange64(&backend->Rendezvous.DeadlineTsc, deadline);
    InterlockedExchange(
        &backend->Rendezvous.Phase, INTEL_RENDEZVOUS_PREPARING);

    if (IntelRendezvousAdvanceEpoch(
            &Context->RendezvousPreparedEpoch, epoch, &observed) &&
        IntelRendezvousSnapshotMatches(
            backend, INTEL_RENDEZVOUS_PREPARING, epoch)) {
        InterlockedIncrement(&backend->Rendezvous.PreparedCount);
    }
    while ((ULONG)InterlockedCompareExchange(
               &backend->Rendezvous.PreparedCount, 0, 0) !=
           backend->Rendezvous.ParticipantCount) {
        if (IntelRendezvousDeadlineExpired((ULONG64)deadline)) {
            InterlockedExchange64(&Context->RendezvousOwnedEpoch, 0);
            IntelRendezvousAbort(
                backend, &backend->Rendezvous.PreparedTimeouts);
            return;
        }
        _mm_pause();
    }

    InterlockedExchange(&backend->Rendezvous.Phase, INTEL_RENDEZVOUS_APPLYING);
    IntelRendezvousApplyOffset(Context, delta);
    if (IntelRendezvousAdvanceEpoch(
            &Context->RendezvousAppliedEpoch, epoch, &observed) &&
        IntelRendezvousSnapshotMatches(
            backend, INTEL_RENDEZVOUS_APPLYING, epoch)) {
        InterlockedIncrement(&backend->Rendezvous.AppliedCount);
    }
    while ((ULONG)InterlockedCompareExchange(
               &backend->Rendezvous.AppliedCount, 0, 0) !=
           backend->Rendezvous.ParticipantCount) {
        if (IntelRendezvousDeadlineExpired((ULONG64)deadline)) {
            KeBugCheckEx(
                HYPERVISOR_ERROR,
                INTEL_BUGCHECK_RENDEZVOUS,
                Context->ProcessorIndex,
                (ULONG_PTR)epoch,
                (ULONG_PTR)backend->Rendezvous.AppliedCount);
        }
        _mm_pause();
    }

    resumeTsc = __rdtsc() + backend->RendezvousLeadTicks;
    InterlockedExchange64(
        &backend->Rendezvous.ResumeTsc, (LONG64)resumeTsc);
    KeMemoryBarrier();
    InterlockedExchange(
        &backend->Rendezvous.Phase, INTEL_RENDEZVOUS_RELEASING);
    while ((LONG64)(__rdtsc() - resumeTsc) < 0) {
        _mm_pause();
    }
    InterlockedExchange64(&Context->RendezvousOwnedEpoch, 0);
    backend->Rendezvous.OwnerProcessor = INVALID_PROCESSOR_INDEX;
    KeMemoryBarrier();
    InterlockedExchange(&backend->Rendezvous.Phase, INTEL_RENDEZVOUS_IDLE);
}

INTEL_RENDEZVOUS_POLICY
IntelRendezvousClassifyAndConsume(
    _Inout_ INTEL_CPU_CONTEXT* Context,
    _In_ ULONG Reason,
    _In_ ULONG Msr
    )
{
    INTEL_RENDEZVOUS_POLICY policy;
    ULONG budget;
    ULONG nextBudget;

    if (Context == NULL) {
        return INTEL_POLICY_NONE;
    }
    for (;;) {
        budget = (ULONG)InterlockedCompareExchange(
            &Context->HookRendezvousBudget, 0, 0);
        policy = IntelRendezvousClassifyPolicy(Reason, Msr, budget);
        nextBudget = IntelRendezvousConsumeBudget(Reason, budget);
        if (nextBudget == budget ||
            InterlockedCompareExchange(
                &Context->HookRendezvousBudget,
                (LONG)nextBudget,
                (LONG)budget) == (LONG)budget) {
            return policy;
        }
        _mm_pause();
    }
}

VOID
IntelRendezvousReloadHookBudget(
    _Inout_ INTEL_CPU_CONTEXT* Context
    )
{
    if (Context != NULL) {
        InterlockedExchange(
            &Context->HookRendezvousBudget,
            (LONG)IntelRendezvousReloadBudget());
    }
}
