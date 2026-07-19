#include <ntifs.h>
#include "intel_internal.h"
#include "../../include/hook_observe.h"
#include "../../include/hv_log.h"

ULONG64
JohnSmithHookProbeTarget(
    _In_ ULONG64 Value
    );

/*
 * Deferred-dispatch worker for commands that require PASSIVE_LEVEL.
 *
 * VM exits disable interrupts but retain the interrupted Windows IRQL.  A
 * dispatcher-object signal at PASSIVE_LEVEL can therefore context-switch on
 * the private VM-exit stack.  The handler only publishes to this ring; the
 * PASSIVE_LEVEL worker polls, drains it, and writes results to the caller's
 * shared page.
 *
 * SPSC correctness: the handler is the only producer (one CPU per VM-exit),
 * the worker is the only consumer.  Head/tail are updated with interlocked
 * operations and KeMemoryBarrier; the worker never touches a slot before the
 * producer's store is fenced.
 */

#define INTEL_HCALL_WORKER_CAPACITY 32u

C_ASSERT(INTEL_HOOK_TABLE_CAPACITY * sizeof(HOOK_OBSERVE_QUERY_ENTRY) <=
         sizeof(((PINTEL_HCALL_PAGE)0)->Payload));

typedef struct _INTEL_HCALL_WORKER_ITEM {
    INTEL_HYPERCALL_CMD Cmd;
    INTEL_CPU_CONTEXT* CpuContext;
    PINTEL_HCALL_PAGE Page;
    PEPROCESS Process;
    PVOID SharedPageUserVa;
    ULONG64 Sequence;
    ULONG64 Arg0;
    ULONG64 Arg1;
} INTEL_HCALL_WORKER_ITEM;

static INTEL_HCALL_WORKER_ITEM g_WorkerRing[INTEL_HCALL_WORKER_CAPACITY];
static volatile LONG g_WorkerHead;   /* producer writes, advances */
static volatile LONG g_WorkerTail;   /* consumer reads, advances  */
static KEVENT g_WorkerEvent;
static PKTHREAD g_WorkerThread;
static HANDLE g_WorkerThreadHandle;
static volatile LONG g_WorkerStop;
static EX_RUNDOWN_REF g_WorkerAdmissionRundown;
static EX_PUSH_LOCK g_RegistrationLock;
static BOOLEAN g_ProcessNotifyRegistered;

static VOID
IntelHypercallHookCallback(
    _In_ ULONG HookId,
    _In_ ULONG64 CallerReturnAddress
    )
{
    UNREFERENCED_PARAMETER(HookId);
    UNREFERENCED_PARAMETER(CallerReturnAddress);
}

static VOID
IntelHypercallReleaseMdl(
    _In_opt_ PMDL Mdl,
    _In_opt_ PVOID SystemVa
    )
{
    if (Mdl == NULL) {
        return;
    }
    if (SystemVa != NULL && (Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA) != 0) {
        MmUnmapLockedPages(SystemVa, Mdl);
    }
    if ((Mdl->MdlFlags & MDL_PAGES_LOCKED) != 0) {
        MmUnlockPages(Mdl);
    }
    IoFreeMdl(Mdl);
}

VOID
IntelHypercallReleasePage(
    _Inout_ INTEL_CPU_CONTEXT* Context
    )
{
    PMDL mdl;
    PVOID page;
    PEPROCESS owner;

    if (Context == NULL) {
        return;
    }

    ExWaitForRundownProtectionRelease(&Context->HypercallPageRundown);
    Context->HypercallPageRegistered = FALSE;
    KeMemoryBarrier();
    page = Context->HypercallSharedPage;
    mdl = Context->HypercallPageMdl;
    owner = Context->HypercallOwnerProcess;
    Context->HypercallSharedPage = NULL;
    Context->HypercallPageMdl = NULL;
    Context->HypercallOwnerProcess = NULL;
    IntelHypercallReleaseMdl(mdl, page);
    if (owner != NULL) {
        ObDereferenceObject(owner);
    }
    ExReInitializeRundownProtection(&Context->HypercallPageRundown);
}

static VOID
IntelHypercallReleaseProcessPages(
    _In_ PEPROCESS Process
    )
{
    HV_STATE* state = NULL;
    ULONG index;

    if (Process == NULL || !HvTryAcquireForWorker(&state) || state == NULL) {
        return;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_RegistrationLock);
    for (index = 0; index < state->CpuCount; ++index) {
        INTEL_CPU_CONTEXT* context =
            (INTEL_CPU_CONTEXT*)state->Cpus[index].VendorContext;
        if (context != NULL && context->HypercallOwnerProcess == Process) {
            IntelHypercallReleasePage(context);
        }
    }
    ExReleasePushLockExclusive(&g_RegistrationLock);
    KeLeaveCriticalRegion();
    HvReleaseForWorker();
}

static VOID
IntelHypercallProcessNotify(
    _In_ HANDLE ParentId,
    _In_ HANDLE ProcessId,
    _In_ BOOLEAN Create
    )
{
    PEPROCESS process = NULL;

    UNREFERENCED_PARAMETER(ParentId);
    if (Create || !NT_SUCCESS(PsLookupProcessByProcessId(ProcessId, &process))) {
        return;
    }
    IntelHypercallReleaseProcessPages(process);
    ObDereferenceObject(process);
}

static VOID
IntelHypercallWorkerServiceRegister(
    _In_ const INTEL_HCALL_WORKER_ITEM* Item
    )
{
    KAPC_STATE apcState;
    PINTEL_HCALL_PAGE userPage;
    PINTEL_HCALL_PAGE systemPage = NULL;
    PMDL mdl = NULL;
    PMDL oldMdl;
    PVOID oldPage;
    PEPROCESS oldOwner;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG_PTR userVa;

    if (Item->CpuContext == NULL || Item->Process == NULL) {
        return;
    }

    userVa = (ULONG_PTR)Item->SharedPageUserVa;
    if ((userVa & (PAGE_SIZE - 1)) != 0 ||
        userVa > (ULONG_PTR)MmHighestUserAddress ||
        userVa > MAXULONG_PTR - (PAGE_SIZE - 1) ||
        userVa + PAGE_SIZE - 1 > (ULONG_PTR)MmHighestUserAddress) {
        return;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_RegistrationLock);
    if (PsGetProcessExitStatus(Item->Process) != STATUS_PENDING) {
        ExReleasePushLockExclusive(&g_RegistrationLock);
        KeLeaveCriticalRegion();
        return;
    }

    userPage = (PINTEL_HCALL_PAGE)Item->SharedPageUserVa;
    KeStackAttachProcess(Item->Process, &apcState);
    __try {
        ProbeForWrite(userPage, sizeof(*userPage), sizeof(ULONG64));
        mdl = IoAllocateMdl(userPage, PAGE_SIZE, FALSE, FALSE, NULL);
        if (mdl == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            MmProbeAndLockPages(mdl, UserMode, IoModifyAccess);
            systemPage = (PINTEL_HCALL_PAGE)MmGetSystemAddressForMdlSafe(
                mdl, NormalPagePriority | MdlMappingNoExecute);
            if (systemPage == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    if (!NT_SUCCESS(status)) {
        __try {
            userPage->Result = (ULONG64)status;
            KeMemoryBarrier();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            NOTHING;
        }
    }
    KeUnstackDetachProcess(&apcState);

    if (!NT_SUCCESS(status)) {
        IntelHypercallReleaseMdl(mdl, systemPage);
        ExReleasePushLockExclusive(&g_RegistrationLock);
        KeLeaveCriticalRegion();
        return;
    }

    ExWaitForRundownProtectionRelease(
        &Item->CpuContext->HypercallPageRundown);
    Item->CpuContext->HypercallPageRegistered = FALSE;
    KeMemoryBarrier();
    oldPage = Item->CpuContext->HypercallSharedPage;
    oldMdl = Item->CpuContext->HypercallPageMdl;
    oldOwner = Item->CpuContext->HypercallOwnerProcess;
    ObReferenceObject(Item->Process);
    Item->CpuContext->HypercallSharedPage = systemPage;
    Item->CpuContext->HypercallPageMdl = mdl;
    Item->CpuContext->HypercallOwnerProcess = Item->Process;
    IntelHypercallReleaseMdl(oldMdl, oldPage);
    if (oldOwner != NULL) {
        ObDereferenceObject(oldOwner);
    }
    Item->CpuContext->HypercallPageRegistered = TRUE;
    ExReInitializeRundownProtection(
        &Item->CpuContext->HypercallPageRundown);
    KeMemoryBarrier();
    systemPage->Result = (ULONG64)STATUS_SUCCESS;
    ExReleasePushLockExclusive(&g_RegistrationLock);
    KeLeaveCriticalRegion();
}

static VOID
IntelHypercallWorkerServiceMemory(
    _In_ const INTEL_HCALL_WORKER_ITEM* Item
    )
{
    KAPC_STATE apcState;
    MM_COPY_ADDRESS source;
    SIZE_T copied = 0;
    ULONG64 targetVa = Item->Arg0;
    ULONG64 size = Item->Arg1;
    PINTEL_HCALL_PAGE page = Item->Page;
    PEPROCESS process = Item->CpuContext->HypercallOwnerProcess;
    NTSTATUS status;
    BOOLEAN userAddress;
    BOOLEAN attached = FALSE;

    if (page == NULL || size == 0 ||
        size > sizeof(page->Payload) || targetVa > MAXULONG_PTR - size) {
        if (page != NULL) {
            page->Result = (ULONG64)STATUS_INVALID_PARAMETER;
        }
        return;
    }

    userAddress = targetVa <= (ULONG64)(ULONG_PTR)MmHighestUserAddress;
    if (userAddress &&
        (process == NULL || targetVa + size - 1 >
            (ULONG64)(ULONG_PTR)MmHighestUserAddress)) {
        page->Result = (ULONG64)STATUS_INVALID_PARAMETER;
        return;
    }
    if (userAddress) {
        KeStackAttachProcess(process, &apcState);
        attached = TRUE;
    }

    if (Item->Cmd == INTEL_HYPERCALL_CMD_READ) {
        source.VirtualAddress = (PVOID)(ULONG_PTR)targetVa;
        status = MmCopyMemory(
            page->Payload, source, (SIZE_T)size,
            MM_COPY_MEMORY_VIRTUAL, &copied);
        if (NT_SUCCESS(status) && copied != (SIZE_T)size) {
            status = STATUS_PARTIAL_COPY;
        }
    } else {
        status = STATUS_SUCCESS;
        __try {
            if (userAddress) {
                ProbeForWrite(
                    (PVOID)(ULONG_PTR)targetVa, (SIZE_T)size, 1);
            }
            if (NT_SUCCESS(status)) {
                RtlCopyMemory(
                    (PVOID)(ULONG_PTR)targetVa,
                    page->Payload,
                    (SIZE_T)size);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            status = GetExceptionCode();
        }
    }
    if (attached) {
        KeUnstackDetachProcess(&apcState);
    }
    page->Result = (ULONG64)status;
}

static VOID
IntelHypercallWorkerServiceItem(
    _In_ const INTEL_HCALL_WORKER_ITEM* Item
    )
{
    HV_STATE* state = NULL;
    PINTEL_HCALL_PAGE page = Item->Page;

    if (Item->Cmd == INTEL_HYPERCALL_CMD_REGISTER) {
        IntelHypercallWorkerServiceRegister(Item);
        ObDereferenceObject(Item->Process);
        return;
    }
    if (page == NULL || Item->CpuContext == NULL) {
        return;
    }

    if (Item->Cmd == INTEL_HYPERCALL_CMD_READ ||
        Item->Cmd == INTEL_HYPERCALL_CMD_WRITE) {
        IntelHypercallWorkerServiceMemory(Item);
        KeMemoryBarrier();
        page->ResultSequence = Item->Sequence;
        ExReleaseRundownProtection(
            &Item->CpuContext->HypercallPageRundown);
        return;
    }

    if (!HvTryAcquireForWorker(&state) || state == NULL) {
        page->Result = (ULONG64)STATUS_DEVICE_REMOVED;
        KeMemoryBarrier();
        page->ResultSequence = Item->Sequence;
        ExReleaseRundownProtection(
            &Item->CpuContext->HypercallPageRundown);
        return;
    }

    switch (Item->Cmd) {
    case INTEL_HYPERCALL_CMD_INSTALL: {
        ULONG hookId = 0;
        NTSTATUS s = ObserveHookInstall(
            state,
            (PVOID)Item->Arg0,   /* target VA */
            Item->Arg1,          /* cookie    */
            IntelHypercallHookCallback,
            &hookId);
        page->Result = NT_SUCCESS(s) ? (ULONG64)hookId : (ULONG64)s;
        break;
    }
    case INTEL_HYPERCALL_CMD_REMOVE: {
        NTSTATUS s = ObserveHookRemove(state, (ULONG)Item->Arg0);
        page->Result = (ULONG64)s;
        break;
    }
    case INTEL_HYPERCALL_CMD_QUERY_HOOK: {
        PHOOK_OBSERVE_QUERY_ENTRY query =
            (PHOOK_OBSERVE_QUERY_ENTRY)page->Payload;
        NTSTATUS s;

        RtlZeroMemory(query, sizeof(*query));
        s = ObserveHookQuery((ULONG)Item->Arg0, query);
        page->Result = (ULONG64)s;
        break;
    }
    case INTEL_HYPERCALL_CMD_LIST_HOOKS: {
        PHOOK_OBSERVE_QUERY_ENTRY queries =
            (PHOOK_OBSERVE_QUERY_ENTRY)page->Payload;
        ULONG capacity = (ULONG)(sizeof(page->Payload) / sizeof(*queries));

        RtlZeroMemory(page->Payload, sizeof(page->Payload));
        page->Result = (ULONG64)ObserveHookList(queries, capacity);
        break;
    }
    case INTEL_HYPERCALL_CMD_PROBE:
        page->Result = JohnSmithHookProbeTarget(Item->Arg0);
        break;
    default:
        page->Result = (ULONG64)STATUS_INVALID_PARAMETER;
        break;
    }

    HvReleaseForWorker();
    KeMemoryBarrier();
    page->ResultSequence = Item->Sequence;
    ExReleaseRundownProtection(&Item->CpuContext->HypercallPageRundown);
}

static VOID
IntelHypercallWorkerThread(
    _In_ PVOID Context
    )
{
    LARGE_INTEGER pollInterval;

    UNREFERENCED_PARAMETER(Context);

    KeSetPriorityThread(KeGetCurrentThread(), LOW_PRIORITY);
    /* ponytail: 1 ms polling avoids dispatcher calls on the VM-exit stack;
       replace with a DPC-backed wakeup if measured idle overhead matters. */
    pollInterval.QuadPart = -10 * 1000;

    for (;;) {
        LONG tail;
        LONG head;

        head = InterlockedCompareExchange(&g_WorkerHead, 0, 0);
        tail = InterlockedCompareExchange(&g_WorkerTail, 0, 0);

        if (tail != head) {
            INTEL_HCALL_WORKER_ITEM item =
                g_WorkerRing[tail & (INTEL_HCALL_WORKER_CAPACITY - 1)];
            KeMemoryBarrier();
            InterlockedIncrement(&g_WorkerTail);
            IntelHypercallWorkerServiceItem(&item);
            continue;
        }

        if (InterlockedCompareExchange(&g_WorkerStop, 0, 0) != 0) {
            break;
        }

        KeWaitForSingleObject(
            &g_WorkerEvent,
            Executive,
            KernelMode,
            FALSE,
            &pollInterval);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS
IntelHypercallWorkerEnqueueRegister(
    _Inout_ INTEL_CPU_CONTEXT* Context,
    _In_ PEPROCESS Process,
    _In_ PVOID SharedPageUserVa
    )
{
    LONG head;
    LONG tail;
    INTEL_HCALL_WORKER_ITEM* slot;

    if (Context == NULL || Process == NULL || SharedPageUserVa == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!ExAcquireRundownProtection(&g_WorkerAdmissionRundown)) {
        return STATUS_DEVICE_NOT_READY;
    }

    ObReferenceObject(Process);
    head = InterlockedCompareExchange(&g_WorkerHead, 0, 0);
    tail = InterlockedCompareExchange(&g_WorkerTail, 0, 0);
    if (head - tail >= (LONG)INTEL_HCALL_WORKER_CAPACITY) {
        ObDereferenceObject(Process);
        ExReleaseRundownProtection(&g_WorkerAdmissionRundown);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    slot = &g_WorkerRing[head & (INTEL_HCALL_WORKER_CAPACITY - 1)];
    RtlZeroMemory(slot, sizeof(*slot));
    slot->Cmd = INTEL_HYPERCALL_CMD_REGISTER;
    slot->CpuContext = Context;
    slot->Process = Process;
    slot->SharedPageUserVa = SharedPageUserVa;
    KeMemoryBarrier();
    InterlockedIncrement(&g_WorkerHead);
    ExReleaseRundownProtection(&g_WorkerAdmissionRundown);
    return STATUS_SUCCESS;
}

NTSTATUS
IntelHypercallWorkerEnqueue(
    _In_ INTEL_HYPERCALL_CMD Cmd,
    _Inout_ INTEL_CPU_CONTEXT* Context,
    _Inout_ PINTEL_HCALL_PAGE Page,
    _In_ ULONG64 Arg0,
    _In_ ULONG64 Arg1
    )
{
    LONG head;
    LONG tail;

    if ((Cmd != INTEL_HYPERCALL_CMD_INSTALL &&
         Cmd != INTEL_HYPERCALL_CMD_REMOVE &&
         Cmd != INTEL_HYPERCALL_CMD_READ &&
         Cmd != INTEL_HYPERCALL_CMD_WRITE &&
         Cmd != INTEL_HYPERCALL_CMD_QUERY_HOOK &&
         Cmd != INTEL_HYPERCALL_CMD_LIST_HOOKS &&
         Cmd != INTEL_HYPERCALL_CMD_PROBE) ||
        Context == NULL || Page == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!ExAcquireRundownProtection(&g_WorkerAdmissionRundown)) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (!ExAcquireRundownProtection(&Context->HypercallPageRundown)) {
        ExReleaseRundownProtection(&g_WorkerAdmissionRundown);
        return STATUS_DEVICE_NOT_READY;
    }

    head = InterlockedCompareExchange(&g_WorkerHead, 0, 0);
    tail = InterlockedCompareExchange(&g_WorkerTail, 0, 0);

    if (head - tail >= (LONG)INTEL_HCALL_WORKER_CAPACITY) {
        ExReleaseRundownProtection(&Context->HypercallPageRundown);
        ExReleaseRundownProtection(&g_WorkerAdmissionRundown);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    {
        INTEL_HCALL_WORKER_ITEM* slot;
        slot = &g_WorkerRing[head & (INTEL_HCALL_WORKER_CAPACITY - 1)];
        RtlZeroMemory(slot, sizeof(*slot));
        slot->Cmd = Cmd;
        slot->CpuContext = Context;
        slot->Page = Page;
        slot->Sequence = (ULONG64)Page->Sequence;
        slot->Arg0 = Arg0;
        slot->Arg1 = Arg1;
    }
    KeMemoryBarrier();
    InterlockedIncrement(&g_WorkerHead);
    ExReleaseRundownProtection(&g_WorkerAdmissionRundown);
    return STATUS_SUCCESS;
}

NTSTATUS
IntelHypercallWorkerStart(
    VOID
    )
{
    NTSTATUS status;

    g_WorkerHead = 0;
    g_WorkerTail = 0;
    g_WorkerStop = 0;
    ExInitializeRundownProtection(&g_WorkerAdmissionRundown);
    ExInitializePushLock(&g_RegistrationLock);
    g_ProcessNotifyRegistered = FALSE;
    KeInitializeEvent(&g_WorkerEvent, SynchronizationEvent, FALSE);
    g_WorkerThread = NULL;
    g_WorkerThreadHandle = NULL;

    status = PsCreateSystemThread(
        &g_WorkerThreadHandle,
        THREAD_ALL_ACCESS,
        NULL,
        NULL,
        NULL,
        IntelHypercallWorkerThread,
        NULL);
    if (!NT_SUCCESS(status)) {
        ExWaitForRundownProtectionRelease(&g_WorkerAdmissionRundown);
        return status;
    }

    status = ObReferenceObjectByHandle(
        g_WorkerThreadHandle,
        THREAD_ALL_ACCESS,
        *PsThreadType,
        KernelMode,
        (PVOID*)&g_WorkerThread,
        NULL);
    if (!NT_SUCCESS(status)) {
        ExWaitForRundownProtectionRelease(&g_WorkerAdmissionRundown);
        InterlockedExchange(&g_WorkerStop, 1);
        KeSetEvent(&g_WorkerEvent, IO_NO_INCREMENT, FALSE);
        (VOID)ZwWaitForSingleObject(g_WorkerThreadHandle, FALSE, NULL);
        ZwClose(g_WorkerThreadHandle);
        g_WorkerThreadHandle = NULL;
        return status;
    }
    ZwClose(g_WorkerThreadHandle);
    g_WorkerThreadHandle = NULL;

    status = PsSetCreateProcessNotifyRoutine(
        IntelHypercallProcessNotify, FALSE);
    if (!NT_SUCCESS(status)) {
        ExWaitForRundownProtectionRelease(&g_WorkerAdmissionRundown);
        InterlockedExchange(&g_WorkerStop, 1);
        KeSetEvent(&g_WorkerEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(
            g_WorkerThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_WorkerThread);
        g_WorkerThread = NULL;
        return status;
    }
    g_ProcessNotifyRegistered = TRUE;
    return STATUS_SUCCESS;
}

VOID
IntelHypercallWorkerStop(
    VOID
    )
{
    if (g_WorkerThread == NULL) {
        return;
    }
    ExWaitForRundownProtectionRelease(&g_WorkerAdmissionRundown);
    InterlockedExchange(&g_WorkerStop, 1);
    KeSetEvent(&g_WorkerEvent, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(
        g_WorkerThread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(g_WorkerThread);
    g_WorkerThread = NULL;
    {
        HV_STATE* state = NULL;
        if (HvTryAcquireForWorker(&state) && state != NULL) {
            ULONG index;
            KeEnterCriticalRegion();
            ExAcquirePushLockExclusive(&g_RegistrationLock);
            for (index = 0; index < state->CpuCount; ++index) {
                INTEL_CPU_CONTEXT* context =
                    (INTEL_CPU_CONTEXT*)state->Cpus[index].VendorContext;
                if (context != NULL) {
                    IntelHypercallReleasePage(context);
                }
            }
            ExReleasePushLockExclusive(&g_RegistrationLock);
            KeLeaveCriticalRegion();
            HvReleaseForWorker();
        }
    }
    if (g_ProcessNotifyRegistered) {
        (VOID)PsSetCreateProcessNotifyRoutine(
            IntelHypercallProcessNotify, TRUE);
        g_ProcessNotifyRegistered = FALSE;
    }
}
