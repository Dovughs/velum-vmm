// Bounded guest-context kernel telemetry. No PID/TID collection or return hooks.
#include "common.h"
#include "kernel_telemetry_capture.h"
#include "monitor.h"
#include "kernel_telemetry.h"
#include "telemetry_fields.h"
#include "telemetry_symbols.h"
#include <aux_klib.h>

#define KERNEL_QUEUE_CAPACITY 512u
#define KERNEL_MODULE_CAPACITY 512u
typedef struct _KERNEL_INPUT {
    ULONG Target, Core, CallerValid, ArgumentMask;
    NTSTATUS Status;
    ULONG64 Args[7], Caller;
} KERNEL_INPUT;

static KERNEL_INPUT g_Queue[KERNEL_QUEUE_CAPACITY];
static volatile LONG g_QueueBusy;
static ULONG g_Head, g_Tail;
static volatile LONG64 g_CaptureDropped;
static PVOID volatile g_WorkerThread;
static AUX_MODULE_EXTENDED_INFO g_Modules[KERNEL_MODULE_CAPACITY];
static ULONG g_ModuleCount;
static ULONG64 g_LastRefresh;
static BOOLEAN g_Initialized;
static HV_KERNEL_STATS g_Stats;
static const PCSTR g_Labels[HV_MONITOR_TARGETS][12] = HV_DETAIL_LABELS;

NTSTATUS KernelTelemetryInitialize(VOID)
{
    NTSTATUS status;
    if (g_Initialized) { return STATUS_SUCCESS; }
    status = AuxKlibInitialize();
    if (NT_SUCCESS(status)) { g_Initialized = TRUE; }
    return status;
}

ULONG KernelCapture(ULONG Target, const FILTER_ARGUMENTS *Args, ULONG64 OriginalRsp)
{
    KERNEL_INPUT input = {0};
    if (!HvTargetIsKernel(Target) ||
        (!MonitorDebugEnabled(Target) && !MonitorConsoleEnabled(Target)) ||
        KeGetCurrentThread() == InterlockedCompareExchangePointer(&g_WorkerThread, NULL, NULL)) {
        return 0;
    }

    input.Target = Target;
    input.Core = KeGetCurrentProcessorNumber();
    input.Args[0] = Args->Rcx;
    input.Args[1] = Args->Rdx;
    input.Args[2] = Args->R8;
    input.Args[3] = Args->R9;
    input.ArgumentMask = 0xFu;

    // Read only the native x64 entry return slot, in the guest address space.
    // A normal kernel stack is resident. No input pointers are followed and no
    // guest memory is touched in VMX root. This is not a stack walk.
    if (KeGetCurrentIrql() <= DISPATCH_LEVEL && (OriginalRsp & 7) == 0 &&
        OriginalRsp >= (ULONG64)(ULONG_PTR)MmSystemRangeStart) {
        __try {
            input.Caller = *(volatile ULONG64 *)(ULONG_PTR)OriginalRsp;
            input.CallerValid = input.Caller >= (ULONG64)(ULONG_PTR)MmSystemRangeStart;
            if (!input.CallerValid) { input.Caller = 0; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            input.Caller = 0;
        }
    }

    // MmCopyVirtualMemory has three additional by-value stack inputs. Capture
    // only this resident entry frame; never dereference process objects, copy
    // buffers or the output-size pointer. Root fallback remains register-only.
    if (Target == HV_TARGET_COPY_VIRTUAL_MEMORY) {
        ULONG i;
        input.Status = STATUS_NOT_SUPPORTED;
        if (KeGetCurrentIrql() <= APC_LEVEL && (OriginalRsp & 7) == 0 &&
            OriginalRsp >= (ULONG64)(ULONG_PTR)MmSystemRangeStart &&
            OriginalRsp <= MAXULONGLONG - 0x3F) {
            input.Status = STATUS_SUCCESS;
            __try {
                for (i = 4; i < 7; ++i) {
                    input.Args[i] = *(volatile ULONG64 *)(ULONG_PTR)(OriginalRsp + 0x28 + (i - 4) * 8);
                    input.ArgumentMask |= 1u << i;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                input.Status = GetExceptionCode();
            }
        }
    }

    // Producers may migrate or be preempted; a try-lock drops instead of waiting.
    if (InterlockedCompareExchange(&g_QueueBusy, 1, 0) != 0) {
        InterlockedIncrement64(&g_CaptureDropped);
        return 0;
    }
    if (g_Head - g_Tail < KERNEL_QUEUE_CAPACITY) {
        g_Queue[g_Head++ % KERNEL_QUEUE_CAPACITY] = input;
    } else {
        InterlockedIncrement64(&g_CaptureDropped);
    }
    InterlockedExchange(&g_QueueBusy, 0);
    return 0; // Always continue the original kernel routine.
}

VOID KernelTelemetryBeginDrain(VOID)
{
    ULONG bytes = sizeof(g_Modules);
    ULONG target;
    BOOLEAN enabled = FALSE;
    ULONG64 now = KeQueryInterruptTime();
    LARGE_INTEGER wall;

    // Suppress this worker's own allocations while querying modules/printing.
    InterlockedExchangePointer(&g_WorkerThread, KeGetCurrentThread());
    for (target = 13; target < HV_MONITOR_TARGETS; ++target) {
        if (!HvTargetIsKernel(target)) { continue; }
        if (MonitorDebugEnabled(target) || MonitorConsoleEnabled(target)) { enabled = TRUE; break; }
    }
    if (g_Initialized && enabled && (!g_LastRefresh || now - g_LastRefresh >= 10000000ull)) {
        g_ModuleCount = 0;
        if (NT_SUCCESS(AuxKlibQueryModuleInformation(&bytes, sizeof(g_Modules[0]), g_Modules)) &&
            bytes <= sizeof(g_Modules)) {
            g_ModuleCount = bytes / sizeof(g_Modules[0]);
        }
        g_LastRefresh = now;
        KeQuerySystemTime(&wall);
        g_Stats.ModuleSnapshotTime = (ULONG64)wall.QuadPart;
    }
}

static ULONG64 ResolveCaller(HV_MONITOR_EVENT *Event)
{
    ULONG i, n;
    ULONG64 caller = Event->Arguments[4];

    if (!(Event->Reserved & 2u)) { return 0; }
    for (i = 0; i < g_ModuleCount; ++i) {
        const AUX_MODULE_EXTENDED_INFO *module = &g_Modules[i];
        ULONG64 base = (ULONG64)(ULONG_PTR)module->BasicInfo.ImageBase;
        // The return site may be one byte beyond the image boundary.
        if (!caller || caller - 1 < base || caller - 1 - base >= module->ImageSize ||
            module->FileNameOffset >= sizeof(module->FullPathName)) { continue; }

        Event->Arguments[5] = caller - base;
        Event->Reserved |= 4u;
        for (n = 0; n < 63 && module->FileNameOffset + n < sizeof(module->FullPathName); ++n) {
            UCHAR c = module->FullPathName[module->FileNameOffset + n];
            if (!c) { break; }
            Event->Raw[n] = c >= 32 && c < 127 ? c : '?';
        }
        Event->Raw[n] = 0;
        return base;
    }
    return 0;
}

static VOID AccountEvent(const HV_MONITOR_EVENT *Event, ULONG64 ModuleBase)
{
    ULONG i;
    int tagField = HvKernelTagField(Event->Target);
    int sizeField = HvKernelSizeField(Event->Target);
    ULONG tag = tagField >= 0 ? (ULONG)Event->Fields[tagField] : 0;
    ULONG64 caller = ModuleBase ? 0 : Event->Arguments[4];
    HV_KERNEL_STAT *row = NULL;

    for (i = 0; i < g_Stats.Count; ++i) {
        row = &g_Stats.Entries[i];
        if (row->Target == Event->Target && row->ModuleBase == ModuleBase &&
            row->UnknownCaller == caller && row->HasTag == (ULONG)(tagField >= 0) &&
            row->Tag == tag && RtlCompareMemory(row->Module, Event->Raw, sizeof(row->Module)) == sizeof(row->Module)) {
            break;
        }
    }
    if (i == g_Stats.Count) {
        if (i == HV_KERNEL_STATS_CAPACITY) {
            g_Stats.TableOverflow = HvSaturatingAdd(g_Stats.TableOverflow, 1);
            return;
        }
        row = &g_Stats.Entries[g_Stats.Count++];
        RtlZeroMemory(row, sizeof(*row));
        row->Target = Event->Target;
        row->HasTag = tagField >= 0;
        row->Tag = tag;
        row->ModuleBase = ModuleBase;
        row->UnknownCaller = caller;
        RtlCopyMemory(row->Module, Event->Raw, sizeof(row->Module));
    }
    row->Calls = HvSaturatingAdd(row->Calls, 1);
    if (sizeField >= 0 && (Event->FieldMask & (1u << sizeField))) {
        row->RequestedBytes = HvSaturatingAdd(row->RequestedBytes, Event->Fields[sizeField]);
    } else if (sizeField >= 0) {
        row->MissingSizeCalls = HvSaturatingAdd(row->MissingSizeCalls, 1);
    }
}

VOID KernelTelemetryPublish(HV_MONITOR_EVENT *Event)
{
    ULONG field;
    ULONG64 moduleBase = ResolveCaller(Event);
    char text[HV_SYMBOL_TEXT_CAPACITY], tag[5];
    int tagField = HvKernelTagField(Event->Target);
    int sizeField = HvKernelSizeField(Event->Target);

    if (!MonitorMatches(Event)) { return; }
    AccountEvent(Event, moduleBase);
    MonitorPublish(Event);
    if (!MonitorDebugEnabled(Event->Target)) { return; }

    HV_INFO("KERNEL_ENTRY: %s core=%lu caller=0x%I64X module=%ws+0x%I64X (drain snapshot; inputs only)\n",
        g_SyscallTargetMeta[Event->Target].Name, Event->Core, Event->Arguments[4],
        Event->Raw[0] ? Event->Raw : L"<unresolved>", Event->Arguments[5]);
    for (field = 0; field < 12; ++field) {
        if (!(Event->FieldMask & (1u << field))) { continue; }
        HvFormatField(Event->Target, field, Event->FieldMask, Event->Fields, text, sizeof(text));
        HV_INFO("KERNEL_FIELD: %s %s=%s\n", g_SyscallTargetMeta[Event->Target].Name, g_Labels[Event->Target][field], text);
    }
    if (tagField >= 0) {
        HvKernelTagText((ULONG)Event->Fields[tagField], tag);
        HV_INFO("KERNEL_TAG: '%s' (memory byte order)\n", tag);
    }
    if (sizeField >= 0 && (Event->FieldMask & (1u << sizeField))) {
        ULONG64 bytes = Event->Fields[sizeField];
        HV_INFO("KERNEL_SIZE: %I64u bytes (%I64u KiB + %I64u bytes)\n", bytes, bytes / 1024, bytes % 1024);
    }
    if (Event->Status) {
        HV_INFO("KERNEL_CAPTURE: status=0x%08lX valid_arguments=0x%lX (not a return status)\n",
            Event->Status, Event->ArgumentMask);
    }
}

VOID KernelTelemetryDrain(VOID)
{
    ULONG budget;
    static HV_MONITOR_EVENT event;
    for (budget = 0; budget < KERNEL_QUEUE_CAPACITY; ++budget) {
        KERNEL_INPUT input;
        if (InterlockedCompareExchange(&g_QueueBusy, 1, 0) != 0) { break; }
        if (g_Head == g_Tail) {
            InterlockedExchange(&g_QueueBusy, 0);
            break;
        }
        input = g_Queue[g_Tail++ % KERNEL_QUEUE_CAPACITY];
        InterlockedExchange(&g_QueueBusy, 0);

        RtlZeroMemory(&event, sizeof(event));
        event.Kind = 3;
        event.Target = input.Target;
        event.Core = input.Core;
        event.ArgumentMask = input.ArgumentMask;
        event.FieldMask = HvKernelFieldMask(input.Target) & input.ArgumentMask;
        event.Status = (ULONG)input.Status;
        HvKernelDecode(input.Target, input.Args, event.Fields, input.ArgumentMask);
        event.Reserved = input.CallerValid ? 2u : 0u;
        event.Arguments[4] = input.Caller;
        InterlockedIncrement64(&g_SyscallHitCount[input.Target]);
        KernelTelemetryPublish(&event);
    }
}

VOID KernelTelemetryEndDrain(VOID)
{
    InterlockedExchangePointer(&g_WorkerThread, NULL);
}

VOID KernelTelemetryStats(HV_KERNEL_STATS *Output, BOOLEAN Reset)
{
    // Caller holds the same drain lock that serializes all accounting updates.
    if (Reset) {
        ULONG64 snapshotTime = g_Stats.ModuleSnapshotTime;
        RtlZeroMemory(&g_Stats, sizeof(g_Stats));
        g_Stats.ModuleSnapshotTime = snapshotTime;
        InterlockedExchange64(&g_CaptureDropped, 0);
    } else {
        *Output = g_Stats;
        Output->Version = HV_MONITOR_VERSION;
        Output->CaptureDropped = (ULONG64)InterlockedCompareExchange64(&g_CaptureDropped, 0, 0);
    }
}

