#include "common.h"
#include "monitor.h"
#include "kernel_telemetry_capture.h"
#include "syscall.h"
#include "filter.h"
#include "processor.h"
#include <wdmsec.h>
#define HV_UPCASE(c) RtlUpcaseUnicodeChar(c)
#include "telemetry_match.h"
#include "telemetry_fields.h"
#include "telemetry_policy.h"
static const PCSTR g_LogFieldLabels[HV_MONITOR_TARGETS][12] = HV_DETAIL_LABELS;
static EX_PUSH_LOCK g_LogFilterLock;
static HV_LOG_FILTER g_LogFilters[HV_MONITOR_TARGETS];

// Rules are independent from delivery filters and live for the driver session.
// Never acquire this lock from VMX root or at elevated IRQL.
static EX_PUSH_LOCK g_PolicyLock;
static HV_POLICY_CONFIG g_TargetPolicies[HV_MONITOR_TARGETS];

ULONG MonitorPolicyAction(const HV_LOG_VIEW *Event, ULONG64 *RemoveAccess)
{
    ULONG action = HV_POLICY_OFF;
    const HV_POLICY_CONFIG *policy;

    if (!HvTargetHasPolicy(Event->Target) || Event->PreviousMode != UserMode) { return action; }

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_PolicyLock);

    policy = &g_TargetPolicies[Event->Target];
    if (policy->Action && HvEventMatches(&policy->Match, Event)) {
        action = policy->Action;
        *RemoveAccess = policy->RemoveAccess;
    }

    ExReleasePushLockShared(&g_PolicyLock);
    KeLeaveCriticalRegion();
    return action;
}

BOOLEAN MonitorPolicyEnabled(ULONG Target)
{
    return HvTargetHasPolicy(Target) && ReadULongNoFence(&g_TargetPolicies[Target].Action) != 0;
}
BOOLEAN MonitorMatchesView(const HV_LOG_VIEW *Event)
{
    BOOLEAN matches;
    if (Event->Target >= HV_MONITOR_TARGETS) { return FALSE; }
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_LogFilterLock);
    matches = (BOOLEAN)HvEventMatches(&g_LogFilters[Event->Target], Event);
    ExReleasePushLockShared(&g_LogFilterLock);
    KeLeaveCriticalRegion();
    return matches;
}

BOOLEAN MonitorMatches(const HV_MONITOR_EVENT *Event)
{
    HV_LOG_VIEW view = { Event->Kind, Event->Target, Event->Action, Event->ProcessId,
        Event->ThreadId, Event->TargetProcessId, Event->PreviousMode, Event->PathKind,
        Event->FieldMask, Event->Fields, Event->Raw, Event->Resolved, Event->Destination };
    return MonitorMatchesView(&view);
}

#define MONITOR_CAPACITY 1024u
static PDEVICE_OBJECT g_MonitorDevice;
static KSPIN_LOCK g_MonitorLock;
static volatile LONG g_MonitorReady;
static ULONG g_DebugMask, g_ConsoleMask;
static HV_RUNTIME_CONFIG g_Runtime = { .Version = HV_MONITOR_VERSION };
static ULONG64 g_Head, g_Tail, g_Dropped, g_Sequence;
static HV_MONITOR_EVENT g_Events[MONITOR_CAPACITY];
static const GUID g_MonitorClass =
    { 0x16ee9863, 0x409e, 0x47d9, { 0x91, 0x41, 0xc8, 0xba, 0x39, 0x61, 0xa6, 0x75 } };

C_ASSERT(sizeof(HV_MONITOR_CONFIG) == 16);
C_ASSERT(sizeof(HV_LOG_FILTER) == 1088);
C_ASSERT(sizeof(HV_RUNTIME_CONFIG) == 262168);
C_ASSERT(sizeof(HV_MONITOR_EVENT) == 3376);
C_ASSERT(FIELD_OFFSET(HV_MONITOR_EVENT, Raw) == 1328);

BOOLEAN MonitorDebugEnabled(ULONG Target)
{
    return Target < HV_MONITOR_TARGETS &&
        (g_MonitorReady && (ReadULongNoFence(&g_DebugMask) & (1u << Target)) != 0);
}

BOOLEAN MonitorConsoleEnabled(ULONG Target)
{
    return g_MonitorReady && Target < HV_MONITOR_TARGETS &&
        (ReadULongNoFence(&g_ConsoleMask) & (1u << Target)) != 0;
}

VOID MonitorPublish(HV_MONITOR_EVENT *Event)
{
    KIRQL oldIrql;
    LARGE_INTEGER now;
    if (!MonitorConsoleEnabled(Event->Target)) { return; }
    KeQuerySystemTime(&now);
    Event->Time100ns = (ULONG64)now.QuadPart;
    KeAcquireSpinLock(&g_MonitorLock, &oldIrql);
    if ((g_ConsoleMask & (1u << Event->Target)) != 0) {
        if (g_Head - g_Tail >= MONITOR_CAPACITY) { ++g_Tail; ++g_Dropped; }
        Event->Sequence = ++g_Sequence;
        g_Events[g_Head++ % MONITOR_CAPACITY] = *Event;
    }
    KeReleaseSpinLock(&g_MonitorLock, oldIrql);
}

static NTSTATUS MonitorRuntimeSet(const HV_RUNTIME_CONFIG *config)
{
    NTSTATUS status;
    ULONG oldMask = SyscallHooksMask();
    BOOLEAN samePaths;
    if (config->Version != HV_MONITOR_VERSION || config->Enabled != (ULONG)(config->HookMask != 0) ||
        (config->HookMask & ~HV_MONITOR_ALL) || config->Reserved != 0 || config->PathCount > HV_PROTECTED_MAX) {
        return STATUS_INVALID_PARAMETER;
    }
    if (config->HookMask & ~SyscallAvailableMask()) { return STATUS_PROCEDURE_NOT_FOUND; }
    samePaths = config->PathCount == g_Runtime.PathCount &&
        RtlCompareMemory(config->Paths, g_Runtime.Paths, config->PathCount * sizeof(config->Paths[0])) ==
        config->PathCount * sizeof(config->Paths[0]);
    if (oldMask && !samePaths && config->HookMask) { return STATUS_DEVICE_BUSY; }
    if (oldMask && samePaths && oldMask == config->HookMask) { return STATUS_SUCCESS; }
    // Shared shadow pages are rebuilt as a batch; retained slots are never freed after a failed remove.
    SyscallHookRemove();
    if (SyscallHooksPresent()) { return STATUS_DEVICE_BUSY; }
    status = samePaths ? STATUS_SUCCESS : FilterConfigure(config);
    if (NT_SUCCESS(status)) {
        g_Runtime.PathCount = config->PathCount;
        RtlZeroMemory(g_Runtime.Paths, sizeof(g_Runtime.Paths));
        RtlCopyMemory(g_Runtime.Paths, config->Paths, config->PathCount * sizeof(config->Paths[0]));
        if (config->HookMask) { status = SyscallHookInstall(config->HookMask); }
    }
    if (!NT_SUCCESS(status)) {
        NTSTATUS original = status;
        SyscallHookRemove();
        // Restore the previous selected hooks when changing only the mask.
        if (!SyscallHooksPresent() && oldMask && samePaths) {
            if (!NT_SUCCESS(SyscallHookInstall(oldMask))) { SyscallHookRemove(); }
        }
        status = original;
    }
    g_Runtime.HookMask = SyscallHooksMask();
    g_Runtime.Enabled = g_Runtime.HookMask != 0;
    return status;
}

static NTSTATUS MonitorDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR bytes = 0;
    KIRQL oldIrql;
    UNREFERENCED_PARAMETER(DeviceObject);
    if (stack->MajorFunction == IRP_MJ_CREATE) {
        status = stack->FileObject->FileName.Length == 0 && g_MonitorReady ?
            STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
    } else if (stack->MajorFunction == IRP_MJ_CLOSE) {
        status = STATUS_SUCCESS;
    } else if (stack->MajorFunction == IRP_MJ_CLEANUP) {
        // Disconnect stops all log delivery; explicit hook state is retained.
        KeAcquireSpinLock(&g_MonitorLock, &oldIrql);
        g_DebugMask = 0;
        g_ConsoleMask = 0;
        g_Tail = g_Head;
        g_Dropped = 0;
        KeReleaseSpinLock(&g_MonitorLock, oldIrql);
        status = STATUS_SUCCESS;
    } else if (stack->MajorFunction == IRP_MJ_DEVICE_CONTROL && g_MonitorReady) {
        ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
        ULONG input = stack->Parameters.DeviceIoControl.InputBufferLength;
        ULONG output = stack->Parameters.DeviceIoControl.OutputBufferLength;
        PVOID buffer = Irp->AssociatedIrp.SystemBuffer;
        if (code == IOCTL_HV_PROCESSOR_GET || code == IOCTL_HV_PROCESSOR_SET) {
            if (!buffer || input != sizeof(HV_PROCESSOR_CONFIG) ||
                (code == IOCTL_HV_PROCESSOR_GET && output < sizeof(HV_PROCESSOR_CONFIG))) {
                status = STATUS_BUFFER_TOO_SMALL;
            } else if (((HV_PROCESSOR_CONFIG *)buffer)->Version != HV_PROCESSOR_VERSION) {
                status = STATUS_REVISION_MISMATCH;
            } else {
                SyscallControlAcquire();
                status = code == IOCTL_HV_PROCESSOR_GET ?
                    ProcessorGet(buffer) : ProcessorSet(buffer);
                SyscallControlRelease();
                if (NT_SUCCESS(status) && code == IOCTL_HV_PROCESSOR_GET) {
                    bytes = sizeof(HV_PROCESSOR_CONFIG);
                }
            }
        } else if (code == IOCTL_HV_KERNEL_STATS || code == IOCTL_HV_KERNEL_STATS_RESET) {
            if (input != sizeof(ULONG) || !buffer ||
                (code == IOCTL_HV_KERNEL_STATS && output < sizeof(HV_KERNEL_STATS))) {
                status = STATUS_BUFFER_TOO_SMALL;
            } else if (*(ULONG *)buffer != HV_MONITOR_VERSION) {
                status = STATUS_REVISION_MISMATCH;
            } else {
                SyscallLogAcquire();
                KernelTelemetryStats(buffer, code == IOCTL_HV_KERNEL_STATS_RESET);
                SyscallLogRelease();
                bytes = code == IOCTL_HV_KERNEL_STATS ? sizeof(HV_KERNEL_STATS) : 0;
                status = STATUS_SUCCESS;
            }
        } else if (code == IOCTL_HV_POLICY_GET || code == IOCTL_HV_POLICY_SET) {
            HV_POLICY_CONFIG *config = buffer;
            if (input != sizeof(*config) || (code == IOCTL_HV_POLICY_GET && output < sizeof(*config))) {
                status = STATUS_BUFFER_TOO_SMALL;
            } else if (config->Match.Version != HV_MONITOR_VERSION || !HvTargetHasPolicy(config->Match.Target) ||
                (code == IOCTL_HV_POLICY_SET && (!HvPolicyValid(config) ||
                 (config->Match.Field && !g_LogFieldLabels[config->Match.Target][config->Match.Field - 1])))) {
                status = STATUS_INVALID_PARAMETER;
            } else {
                ULONG target = config->Match.Target;
                KeEnterCriticalRegion();
                ExAcquirePushLockExclusive(&g_PolicyLock);

                if (code == IOCTL_HV_POLICY_SET) {
                    g_TargetPolicies[target] = *config;
                } else {
                    *config = g_TargetPolicies[target];
                    config->Match.Version = HV_MONITOR_VERSION;
                    config->Match.Target = target;
                    config->Match.Mode = 1;
                    bytes = sizeof(*config);
                }

                ExReleasePushLockExclusive(&g_PolicyLock);
                KeLeaveCriticalRegion();
                status = STATUS_SUCCESS;
            }
        } else if (code == IOCTL_HV_LOG_FILTER_GET || code == IOCTL_HV_LOG_FILTER_SET) {
            HV_LOG_FILTER *config = buffer;
            if (input != sizeof(*config) || (code == IOCTL_HV_LOG_FILTER_GET && output < sizeof(*config))) {
                status = STATUS_BUFFER_TOO_SMALL;
            } else if (config->Version != HV_MONITOR_VERSION || !HvTargetSupported(config->Target)) {
                status = STATUS_INVALID_PARAMETER;
            } else if (code == IOCTL_HV_LOG_FILTER_SET && (!HvFilterValid(config) ||
                (config->Field && !g_LogFieldLabels[config->Target][config->Field - 1]))) {
                status = STATUS_INVALID_PARAMETER;
            } else {
                ULONG target = config->Target;
                // Serialize updates with the drain worker so an old decision
                // cannot be published after the delivery queue is cleared.
                if (code == IOCTL_HV_LOG_FILTER_SET) { SyscallLogAcquire(); }
                KeEnterCriticalRegion();
                ExAcquirePushLockExclusive(&g_LogFilterLock);
                if (code == IOCTL_HV_LOG_FILTER_SET) { g_LogFilters[target] = *config; }
                else {
                    *config = g_LogFilters[target];
                    config->Version = HV_MONITOR_VERSION; config->Target = target;
                    bytes = sizeof(*config);
                }
                ExReleasePushLockExclusive(&g_LogFilterLock);
                KeLeaveCriticalRegion();
                if (code == IOCTL_HV_LOG_FILTER_SET) {
                    KeAcquireSpinLock(&g_MonitorLock, &oldIrql);
                    g_Tail = g_Head; g_Dropped = 0;
                    KeReleaseSpinLock(&g_MonitorLock, oldIrql);
                    SyscallLogRelease();
                }
                status = STATUS_SUCCESS;
            }
        } else if (code == IOCTL_HV_RUNTIME_GET || code == IOCTL_HV_RUNTIME_SET) {
            SyscallControlAcquire();
            if (code == IOCTL_HV_RUNTIME_GET && output >= sizeof(g_Runtime)) {
                g_Runtime.HookMask = SyscallHooksMask();
                g_Runtime.Enabled = g_Runtime.HookMask != 0;
                g_Runtime.AvailableMask = SyscallAvailableMask();
                RtlCopyMemory(buffer, &g_Runtime, sizeof(g_Runtime));
                bytes = sizeof(g_Runtime);
                status = STATUS_SUCCESS;
            } else if (code == IOCTL_HV_RUNTIME_SET && input == sizeof(g_Runtime)) {
                status = MonitorRuntimeSet(buffer);
            } else { status = STATUS_BUFFER_TOO_SMALL; }
            SyscallControlRelease();
        } else if (code == IOCTL_HV_MONITOR_GET && output >= sizeof(HV_MONITOR_CONFIG)) {
            HV_MONITOR_CONFIG *config = buffer;
            KeAcquireSpinLock(&g_MonitorLock, &oldIrql);
            config->Version = HV_MONITOR_VERSION;
            config->DebugMask = g_DebugMask;
            config->ConsoleMask = g_ConsoleMask;
            config->Reserved = 0;
            KeReleaseSpinLock(&g_MonitorLock, oldIrql);
            bytes = sizeof(*config);
            status = STATUS_SUCCESS;
        } else if (code == IOCTL_HV_MONITOR_SET && input == sizeof(HV_MONITOR_CONFIG)) {
            const HV_MONITOR_CONFIG *config = buffer;
            if (config->Version != HV_MONITOR_VERSION || config->Reserved != 0 ||
                ((config->DebugMask | config->ConsoleMask) & ~HV_MONITOR_ALL) != 0) {
                status = STATUS_INVALID_PARAMETER;
            } else {
                KeAcquireSpinLock(&g_MonitorLock, &oldIrql);
                g_DebugMask = config->DebugMask;
                g_ConsoleMask = config->ConsoleMask;
                g_Tail = g_Head; // Configuration changes start a fresh view.
                g_Dropped = 0;
                KeReleaseSpinLock(&g_MonitorLock, oldIrql);
                status = STATUS_SUCCESS;
            }
        } else if (code == IOCTL_HV_MONITOR_READ && output >= sizeof(HV_MONITOR_BATCH_DATA)) {
            HV_MONITOR_BATCH_DATA *batch = buffer;
            ULONG count = 0;
            batch->Version = HV_MONITOR_VERSION;
            // Small contiguous chunks amortize locking without holding it for 64 records.
            while (count < HV_MONITOR_BATCH) {
                ULONG chunk, index;
                KeAcquireSpinLock(&g_MonitorLock, &oldIrql);
                if (g_Tail == g_Head) {
                    KeReleaseSpinLock(&g_MonitorLock, oldIrql);
                    break;
                }
                index = (ULONG)(g_Tail % MONITOR_CAPACITY);
                chunk = (ULONG)min(g_Head - g_Tail, 4u);
                chunk = min(chunk, MONITOR_CAPACITY - index);
                chunk = min(chunk, HV_MONITOR_BATCH - count);
                RtlCopyMemory(&batch->Events[count], &g_Events[index],
                    chunk * sizeof(HV_MONITOR_EVENT));
                g_Tail += chunk;
                count += chunk;
                KeReleaseSpinLock(&g_MonitorLock, oldIrql);
            }
            KeAcquireSpinLock(&g_MonitorLock, &oldIrql);
            batch->Dropped = g_Dropped;
            KeReleaseSpinLock(&g_MonitorLock, oldIrql);
            batch->Count = count;
            bytes = FIELD_OFFSET(HV_MONITOR_BATCH_DATA, Events) + count * sizeof(HV_MONITOR_EVENT);
            status = STATUS_SUCCESS;
        } else if (code == IOCTL_HV_MONITOR_GET || code == IOCTL_HV_MONITOR_SET ||
            code == IOCTL_HV_MONITOR_READ) { status = STATUS_BUFFER_TOO_SMALL; }
    }
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = bytes;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS MonitorInitialize(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\VelumControl");
    UNICODE_STRING link = RTL_CONSTANT_STRING(L"\\DosDevices\\VelumControl");
    UNICODE_STRING sddl = RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    NTSTATUS status;
    KeInitializeSpinLock(&g_MonitorLock);
    status = IoCreateDeviceSecure(DriverObject, 0, &name, FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN, TRUE, &sddl, &g_MonitorClass, &g_MonitorDevice);
    if (!NT_SUCCESS(status)) { return status; }
    DriverObject->MajorFunction[IRP_MJ_CREATE] = MonitorDispatch;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = MonitorDispatch;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = MonitorDispatch;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = MonitorDispatch;
    status = IoCreateSymbolicLink(&link, &name);
    if (!NT_SUCCESS(status)) { IoDeleteDevice(g_MonitorDevice); g_MonitorDevice = NULL; return status; }
    InterlockedExchange(&g_MonitorReady, 1);
    g_MonitorDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

VOID MonitorDestroy(VOID)
{
    UNICODE_STRING link = RTL_CONSTANT_STRING(L"\\DosDevices\\VelumControl");
    if (g_MonitorDevice == NULL) { return; }
    InterlockedExchange(&g_MonitorReady, 0);
    IoDeleteSymbolicLink(&link);
    IoDeleteDevice(g_MonitorDevice);
    g_MonitorDevice = NULL;
}
