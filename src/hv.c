#include "hv.h"
#include "introspection.h"
#include "hv_log.h"

#include <intrin.h>

#define HV_FAIL_STOP_ROLLBACK 0x52424B31u
#define HV_FAIL_STOP_SHUTDOWN 0x53444E31u

typedef struct _HV_IPI_CONTEXT {
    HV_STATE* State;
    volatile LONG Failure;
} HV_IPI_CONTEXT;

static HV_STATE HvGlobalState;
static EX_RUNDOWN_REF HvPublishRundown;

NTSTATUS IntelHypercallWorkerStart(VOID);
VOID IntelHypercallWorkerStop(VOID);

BOOLEAN
HvTryAcquireForWorker(
    _Out_ HV_STATE** OutState
    )
{
    *OutState = NULL;
    if (!ExAcquireRundownProtection(&HvPublishRundown)) {
        return FALSE;
    }
    *OutState = &HvGlobalState;
    return TRUE;
}

VOID
HvReleaseForWorker(
    VOID
    )
{
    ExReleaseRundownProtection(&HvPublishRundown);
}

static BOOLEAN
HvIsHypervisorActive(
    VOID
    )
{
    int registers[4];

    __cpuid(registers, 1);
    if ((((ULONG)registers[2]) & (1u << 31)) == 0) {
        return FALSE;
    }

    return TRUE;
}

static const HV_BACKEND_OPS*
HvSelectBackend(
    VOID
    )
{
    int registers[4];

    __cpuid(registers, 0);
    if ((ULONG)registers[1] == 0x756e6547u &&
        (ULONG)registers[3] == 0x49656e69u &&
        (ULONG)registers[2] == 0x6c65746eu) {
        return HvIntelGetBackendOps();
    }

    if ((ULONG)registers[1] == 0x68747541u &&
        (ULONG)registers[3] == 0x69746e65u &&
        (ULONG)registers[2] == 0x444d4163u) {
        return HvAmdGetBackendOps();
    }

    return NULL;
}

static BOOLEAN
HvIsBackendContractValid(
    _In_ const HV_BACKEND_OPS* Backend
    )
{
    return Backend != NULL &&
           Backend->Name != NULL &&
           Backend->Support != NULL &&
           Backend->Prepare != NULL &&
           Backend->Free != NULL &&
           Backend->PrepareCpu != NULL &&
           Backend->FreeCpu != NULL &&
           Backend->Quiesce != NULL &&
           Backend->Start != NULL &&
           Backend->Stop != NULL &&
           Backend->ReportStartFailure != NULL &&
           Backend->QueryOwnedPageAccess != NULL &&
           Backend->SetOwnedPageAccess != NULL;
}

static HV_CPU*
HvGetCurrentCpu(
    _In_ HV_IPI_CONTEXT* Context
    )
{
    PROCESSOR_NUMBER processorNumber;
    ULONG index;

    KeGetCurrentProcessorNumberEx(&processorNumber);
    index = KeGetProcessorIndexFromNumber(&processorNumber);
    if (index == INVALID_PROCESSOR_INDEX || index >= Context->State->CpuCount) {
        InterlockedExchange(&Context->Failure, TRUE);
        return NULL;
    }

    if (Context->State->Cpus[index].ProcessorIndex != index) {
        InterlockedExchange(&Context->Failure, TRUE);
        return NULL;
    }

    return &Context->State->Cpus[index];
}

_Function_class_(KIPI_BROADCAST_WORKER)
_IRQL_requires_(IPI_LEVEL)
static ULONG_PTR
HvStartProcessor(
    _In_ ULONG_PTR Argument
    )
{
    HV_IPI_CONTEXT* context = (HV_IPI_CONTEXT*)Argument;
    HV_CPU* cpu = HvGetCurrentCpu(context);
    NTSTATUS status;

    if (cpu == NULL) {
        return 0;
    }

    if (InterlockedCompareExchange(
            &cpu->State,
            HV_CPU_STARTING,
            HV_CPU_PREPARED) != HV_CPU_PREPARED) {
        cpu->Status = STATUS_INVALID_DEVICE_STATE;
        InterlockedExchange(&context->Failure, TRUE);
        return 0;
    }

    status = context->State->Backend->Start(context->State, cpu);
    cpu->Status = status;
    InterlockedExchange(
        &cpu->State,
        NT_SUCCESS(status) ? HV_CPU_RUNNING : HV_CPU_START_FAILED);
    if (!NT_SUCCESS(status)) {
        InterlockedExchange(&context->Failure, TRUE);
    }

    return 0;
}

_Function_class_(KIPI_BROADCAST_WORKER)
_IRQL_requires_(IPI_LEVEL)
static ULONG_PTR
HvStopProcessor(
    _In_ ULONG_PTR Argument
    )
{
    HV_IPI_CONTEXT* context = (HV_IPI_CONTEXT*)Argument;
    HV_CPU* cpu = HvGetCurrentCpu(context);
    LONG cpuState;
    NTSTATUS status;

    if (cpu == NULL) {
        return 0;
    }

    cpuState = InterlockedCompareExchange(&cpu->State, 0, 0);
    if (cpuState == HV_CPU_PREPARED) {
        cpu->Status = STATUS_SUCCESS;
        return 0;
    }

    if (cpuState != HV_CPU_RUNNING && cpuState != HV_CPU_START_FAILED) {
        cpu->Status = STATUS_INVALID_DEVICE_STATE;
        InterlockedExchange(&context->Failure, TRUE);
        return 0;
    }

    InterlockedExchange(&cpu->State, HV_CPU_STOPPING);
    status = context->State->Backend->Stop(context->State, cpu);
    cpu->Status = status;
    InterlockedExchange(
        &cpu->State,
        NT_SUCCESS(status) ? HV_CPU_PREPARED : HV_CPU_STOP_FAILED);
    if (!NT_SUCCESS(status)) {
        InterlockedExchange(&context->Failure, TRUE);
    }

    return 0;
}

static NTSTATUS
HvStartProcessors(
    _Inout_ HV_STATE* State
    )
{
    HV_IPI_CONTEXT context;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG index;

    context.State = State;
    context.Failure = FALSE;
    KeIpiGenericCall(HvStartProcessor, (ULONG_PTR)&context);

    for (index = 0; index < State->CpuCount; ++index) {
        if (InterlockedCompareExchange(&State->Cpus[index].State, 0, 0) !=
            HV_CPU_RUNNING) {
            State->Backend->ReportStartFailure(
                State, &State->Cpus[index]);
            if (NT_SUCCESS(status)) {
                status = NT_SUCCESS(State->Cpus[index].Status)
                    ? STATUS_HV_OPERATION_FAILED
                    : State->Cpus[index].Status;
            }
        }
    }

    if (InterlockedCompareExchange(&context.Failure, 0, 0) != FALSE &&
        NT_SUCCESS(status)) {
        status = STATUS_HV_OPERATION_FAILED;
    }

    return status;
}

DECLSPEC_NORETURN
static VOID
HvFailStop(
    _In_ HV_STATE* State,
    _In_ ULONG Reason
    )
{
    ULONG index;

    for (index = 0; index < State->CpuCount; ++index) {
        if (InterlockedCompareExchange(&State->Cpus[index].State, 0, 0) !=
            HV_CPU_PREPARED) {
            KeBugCheckEx(
                HYPERVISOR_ERROR,
                Reason,
                index,
                (ULONG_PTR)(LONG_PTR)State->Cpus[index].Status,
                (ULONG_PTR)State->Cpus[index].State);
        }
    }

    KeBugCheckEx(
        HYPERVISOR_ERROR,
        Reason,
        INVALID_PROCESSOR_INDEX,
        (ULONG_PTR)(LONG_PTR)STATUS_HV_OPERATION_FAILED,
        0);
}

static VOID
HvStopProcessorsOrFail(
    _Inout_ HV_STATE* State,
    _In_ ULONG FailureReason
    )
{
    HV_IPI_CONTEXT context;
    ULONG index;

    context.State = State;
    context.Failure = FALSE;
    KeIpiGenericCall(HvStopProcessor, (ULONG_PTR)&context);

    if (InterlockedCompareExchange(&context.Failure, 0, 0) != FALSE) {
        HvFailStop(State, FailureReason);
    }

    for (index = 0; index < State->CpuCount; ++index) {
        if (InterlockedCompareExchange(&State->Cpus[index].State, 0, 0) !=
            HV_CPU_PREPARED) {
            HvFailStop(State, FailureReason);
        }
    }
}

static VOID
HvReleaseCpus(
    _Inout_ HV_STATE* State,
    _In_ ULONG Count
    )
{
    while (Count != 0) {
        HV_CPU* cpu = &State->Cpus[--Count];

        State->Backend->FreeCpu(State, cpu);
        cpu->VendorContext = NULL;
        cpu->Status = STATUS_SUCCESS;
        InterlockedExchange(&cpu->State, HV_CPU_EMPTY);
    }
}

static VOID
HvResetStoppedState(
    _Inout_ HV_STATE* State
    )
{
    State->Backend = NULL;
    State->BackendContext = NULL;
    State->Cpus = NULL;
    State->CpuCount = 0;
    InterlockedExchange(&State->Lifecycle, HV_LIFECYCLE_STOPPED);
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
HvStart(
    _Outptr_result_maybenull_ HV_STATE** State
    )
{
    const HV_BACKEND_OPS* backend;
    BOOLEAN intelBackend;
    HV_CPU* cpu;
    ULONG preparedCount = 0;
    ULONG cpuCount;
    NTSTATUS status;
    NTSTATUS cleanupStatus;
    SIZE_T allocationSize;

    if (State == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *State = NULL;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (InterlockedCompareExchange(
            &HvGlobalState.Lifecycle,
            HV_LIFECYCLE_STARTING,
            HV_LIFECYCLE_STOPPED) != HV_LIFECYCLE_STOPPED) {
        return STATUS_DEVICE_BUSY;
    }

    ExInitializeRundownProtection(&HvPublishRundown);

    if (HvIsHypervisorActive()) {
        status = STATUS_HV_FEATURE_UNAVAILABLE;
        goto FailLifecycle;
    }

    backend = HvSelectBackend();
    if (!HvIsBackendContractValid(backend)) {
        status = STATUS_NOT_SUPPORTED;
        goto FailLifecycle;
    }
    intelBackend = backend == HvIntelGetBackendOps();

    status = backend->Support();
    if (!NT_SUCCESS(status)) {
        goto FailLifecycle;
    }
    HV_LOG_INFO("selected backend: %s.\n", backend->Name);

    cpuCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (cpuCount == 0 || cpuCount > MAXULONG / sizeof(HV_CPU)) {
        status = STATUS_INVALID_DEVICE_STATE;
        goto FailLifecycle;
    }

    allocationSize = (SIZE_T)cpuCount * sizeof(HV_CPU);
    HvGlobalState.Cpus = (HV_CPU*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        allocationSize,
        HV_POOL_TAG_CPU_ARRAY);
    if (HvGlobalState.Cpus == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto FailLifecycle;
    }
    RtlZeroMemory(HvGlobalState.Cpus, allocationSize);

    HvGlobalState.Backend = backend;
    HvGlobalState.CpuCount = cpuCount;

    status = backend->Prepare(&HvGlobalState);
    if (!NT_SUCCESS(status)) {
        backend->Free(&HvGlobalState);
        goto FailArray;
    }

    for (preparedCount = 0; preparedCount < cpuCount; ++preparedCount) {
        cpu = &HvGlobalState.Cpus[preparedCount];
        cpu->ProcessorIndex = preparedCount;
        cpu->Status = STATUS_SUCCESS;
        InterlockedExchange(&cpu->State, HV_CPU_PREPARING);

        status = backend->PrepareCpu(&HvGlobalState, cpu);
        cpu->Status = status;
        if (!NT_SUCCESS(status)) {
            backend->FreeCpu(&HvGlobalState, cpu);
            cpu->VendorContext = NULL;
            InterlockedExchange(&cpu->State, HV_CPU_EMPTY);
            goto FailPreparedCpus;
        }

        InterlockedExchange(&cpu->State, HV_CPU_PREPARED);
    }

    status = HvIntrospectionStart(&HvGlobalState);
    if (!NT_SUCCESS(status)) {
        goto FailPreparedCpus;
    }

    if (intelBackend) {
        status = IntelHypercallWorkerStart();
        if (!NT_SUCCESS(status)) {
            goto FailIntrospection;
        }
    }

    status = HvStartProcessors(&HvGlobalState);
    if (!NT_SUCCESS(status)) {
        if (intelBackend) {
            IntelHypercallWorkerStop();
        }
        HvStopProcessorsOrFail(&HvGlobalState, HV_FAIL_STOP_ROLLBACK);
        goto FailIntrospection;
    }

    InterlockedExchange(&HvGlobalState.Lifecycle, HV_LIFECYCLE_RUNNING);
    *State = &HvGlobalState;
    HvLogStartBanner(backend->Name, cpuCount);
    return STATUS_SUCCESS;

FailIntrospection:
    cleanupStatus = HvIntrospectionStop(&HvGlobalState);
    if (!NT_SUCCESS(cleanupStatus)) {
        HvFailStop(&HvGlobalState, HV_FAIL_STOP_ROLLBACK);
    }
FailPreparedCpus:
    HvReleaseCpus(&HvGlobalState, preparedCount);
    backend->Free(&HvGlobalState);
FailArray:
    ExFreePoolWithTag(HvGlobalState.Cpus, HV_POOL_TAG_CPU_ARRAY);
FailLifecycle:
    HvResetStoppedState(&HvGlobalState);
    HV_LOG_ERROR("startup failed with NTSTATUS 0x%08X.\n", (ULONG)status);
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
HvStop(
    _Inout_ HV_STATE* State
    )
{
    if (State == NULL || State != &HvGlobalState) {
        return;
    }

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        HvFailStop(State, HV_FAIL_STOP_SHUTDOWN);
    }

    if (InterlockedCompareExchange(
            &State->Lifecycle,
            HV_LIFECYCLE_STOPPING,
            HV_LIFECYCLE_RUNNING) != HV_LIFECYCLE_RUNNING) {
        if (InterlockedCompareExchange(&State->Lifecycle, 0, 0) ==
            HV_LIFECYCLE_STOPPED) {
            return;
        }
        if (InterlockedCompareExchange(&State->Lifecycle, 0, 0) ==
            HV_LIFECYCLE_STOPPING) {
            return;
        }
        HvFailStop(State, HV_FAIL_STOP_SHUTDOWN);
    }

    HV_LOG_INFO(
        "stopping backend %s on %lu logical processors.\n",
        State->Backend->Name,
        State->CpuCount);

    if (State->Backend == HvIntelGetBackendOps()) {
        IntelHypercallWorkerStop();
    }
    State->Backend->Quiesce(State);
    HvStopProcessorsOrFail(State, HV_FAIL_STOP_SHUTDOWN);
    ExWaitForRundownProtectionRelease(&HvPublishRundown);

    {
        NTSTATUS status = HvIntrospectionStop(State);

        if (!NT_SUCCESS(status)) {
            HvFailStop(State, HV_FAIL_STOP_SHUTDOWN);
        }
    }

    HvReleaseCpus(State, State->CpuCount);
    State->Backend->Free(State);
    ExFreePoolWithTag(State->Cpus, HV_POOL_TAG_CPU_ARRAY);
    HvResetStoppedState(State);
    HV_LOG_INFO("hypervisor stopped cleanly.\n");
}
