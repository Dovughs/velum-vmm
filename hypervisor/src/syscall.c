// Syscall introspection: VM-exit is the per-core producer; PASSIVE worker
// drains raw register snapshots. No guest pointer dereferences or detour calls.
#include "syscall.h"
#include "common.h"
#include "hook.h"
#include "persist.h"
#include "filter.h"
#include "monitor.h"
#include "kernel_telemetry_capture.h"
#include "kernel_telemetry.h"
#include "telemetry_fields.h"
#include "telemetry_symbols.h"
#include "target_entries.h"
C_ASSERT(SYSCALL_TARGET_COUNT == HV_MONITOR_TARGETS);
C_ASSERT(SyscallTargetNtDuplicateObject == HV_TARGET_DUPLICATE_OBJECT);
C_ASSERT(SyscallTargetMmCopyVirtualMemory == HV_TARGET_COPY_VIRTUAL_MEMORY);

#define SYSCALL_TAG 'llcS'

// Wide routine names for MmGetSystemRoutineAddress (order matches the enum).
static const PCWSTR g_SyscallNames[SYSCALL_TARGET_COUNT] = {
    L"NtCreateFile",
    L"NtAllocateVirtualMemory",
    L"NtOpenFile",
    L"NtDeleteFile",
    L"NtSetInformationFile",
    L"NtWriteFile",
    L"NtCreateSection",
    L"NtMapViewOfSection",
    L"NtOpenProcess",
    NULL, // retired ID 9; never reused by profile migration
    L"NtProtectVirtualMemory",
    NULL, // retired ID 11
    L"NtReadFile",
    L"ExAllocatePool2",
    L"ExAllocatePoolWithTag",
    L"ExFreePoolWithTag",
    L"ExAllocatePoolWithTagPriority",
    L"ExAllocatePool3",
    L"ExAllocatePool",
    L"ExFreePool",
    L"MmAllocateContiguousMemory",
    L"MmFreeContiguousMemory",
    L"MmMapIoSpaceEx",
    L"MmUnmapIoSpace",
    L"NtDuplicateObject",
    L"MmCopyVirtualMemory",
};

// Per-target metadata. Address is filled at install. Argument labels follow the
// Windows x64 calling convention (SDM Vol. 1 3.2.3): arg1=RCX, arg2=RDX,
// arg3=R8, arg4=R9; arg5+ would require reading the guest stack (not done here).
SYSCALL_TARGET_META g_SyscallTargetMeta[SYSCALL_TARGET_COUNT] = {
    { "NtCreateFile",              0, "FileHandle",            "DesiredAccess",     "ObjectAttributes", "IoStatusBlock" },
    { "NtAllocateVirtualMemory",   0, "ProcessHandle",         "BaseAddress",       "ZeroBits",         "RegionSize" },
    { "NtOpenFile", 0, "FileHandle", "DesiredAccess", "ObjectAttributes", "IoStatusBlock" },
    { "NtDeleteFile", 0, "ObjectAttributes", "Unused", "Unused", "Unused" },
    { "NtSetInformationFile", 0, "FileHandle", "IoStatusBlock", "FileInformation", "Length" },
    { "NtWriteFile", 0, "Arg1", "Arg2", "Arg3", "Arg4" },
    { "NtCreateSection", 0, "Arg1", "Arg2", "Arg3", "Arg4" },
    { "NtMapViewOfSection", 0, "Arg1", "Arg2", "Arg3", "Arg4" },
    { "NtOpenProcess", 0, "Arg1", "Arg2", "Arg3", "Arg4" },
    { NULL, 0, NULL, NULL, NULL, NULL },
    { "NtProtectVirtualMemory", 0, "Arg1", "Arg2", "Arg3", "Arg4" },
    { NULL, 0, NULL, NULL, NULL, NULL },
    { "NtReadFile", 0, "FileHandle", "Event", "ApcRoutine", "ApcContext" },
    { "ExAllocatePool2", 0, "Flags", "NumberOfBytes", "Tag", "Unused" },
    { "ExAllocatePoolWithTag", 0, "PoolType", "NumberOfBytes", "Tag", "Unused" },
    { "ExFreePoolWithTag", 0, "Pointer", "Tag", "Unused", "Unused" },
    { "ExAllocatePoolWithTagPriority", 0, "PoolType", "NumberOfBytes", "Tag", "Priority" },
    { "ExAllocatePool3", 0, "Flags", "NumberOfBytes", "Tag", "ExtendedParameters" },
    { "ExAllocatePool", 0, "PoolType", "NumberOfBytes", "Unused", "Unused" },
    { "ExFreePool", 0, "Pointer", "Unused", "Unused", "Unused" },
    { "MmAllocateContiguousMemory", 0, "NumberOfBytes", "HighestAcceptableAddress", "Unused", "Unused" },
    { "MmFreeContiguousMemory", 0, "BaseAddress", "Unused", "Unused", "Unused" },
    { "MmMapIoSpaceEx", 0, "PhysicalAddress", "NumberOfBytes", "Protect", "Unused" },
    { "MmUnmapIoSpace", 0, "BaseAddress", "NumberOfBytes", "Unused", "Unused" },
    { "NtDuplicateObject", 0, "SourceProcessHandle", "SourceHandle", "TargetProcessHandle", "TargetHandlePointer" },
    { "MmCopyVirtualMemory", 0, "SourceProcess", "SourceAddress", "TargetProcess", "TargetAddress" },
};

ULONG64 g_SyscallTargetVa[SYSCALL_TARGET_COUNT] = { 0 };

// Protect ring allocation/free against the PASSIVE drain worker. Root never
// takes this lock; hooks are removed on all owners before freeing their rings.
static EX_PUSH_LOCK g_SyscallDrainLock;

static VOID SyscallAcquire(VOID)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_SyscallDrainLock);
}

static VOID SyscallRelease(VOID)
{
    ExReleasePushLockExclusive(&g_SyscallDrainLock);
    KeLeaveCriticalRegion();
}

VOID SyscallLogAcquire(VOID) { SyscallAcquire(); }
VOID SyscallLogRelease(VOID) { SyscallRelease(); }

VOID SyscallRecord(PSYSCALL_RING Ring, const SYSCALL_RING_ENTRY *Entry)
{
    LONG64 head;
    if (Ring == NULL || (!MonitorDebugEnabled((ULONG)Entry->TargetIndex) &&
        !MonitorConsoleEnabled((ULONG)Entry->TargetIndex))) { return; }
    head = Ring->Head;
    if ((ULONG64)head - (ULONG64)Ring->Tail >= SYSCALL_RING_ENTRIES) { return; }
    Ring->Entry[head & SYSCALL_RING_MASK] = *Entry;
    InterlockedIncrement64(&Ring->Head);
}

// Hit counters, incremented by the PASSIVE drain worker only.
volatile LONG64 g_SyscallHitCount[SYSCALL_TARGET_COUNT] = { 0 };

// Canonical x64 kernel-address check (SDM Vol. 3A 3.3.7.1): kernel VAs have
// bits 63:48 all set. PoC sanity check; a full ntoskrnl .text range check is
// omitted for brevity.
static BOOLEAN SyscallIsKernelText(ULONG64 Address)
{
    return (Address & 0xFFFF000000000000ull) == 0xFFFF000000000000ull;
}

// Log one ring entry at PASSIVE_LEVEL (the ONLY place DbgPrintEx is reachable).
static VOID SyscallLogEntry(ULONG Core, const SYSCALL_RING_ENTRY *Entry)
{
    ULONG idx = (ULONG)Entry->TargetIndex;
    HV_MONITOR_EVENT event = { 0 };
    if (!HvTargetSupported(idx)) { return; }
    InterlockedIncrement64(&g_SyscallHitCount[idx]);
    {
        event.Kind = 1;
        event.Target = idx;
        event.Core = Core;
        event.Arguments[0] = Entry->Rcx;
        event.Arguments[1] = Entry->Rdx;
        event.Arguments[2] = Entry->R8;
        event.Arguments[3] = Entry->R9;
        event.Arguments[4] = Entry->Rsp;
        event.Arguments[5] = Entry->Rip;
        if (HvTargetIsKernel(idx)) {
            // Ineligible guest contexts retain register-only fallback telemetry.
            event.Kind = 3;
            event.ArgumentMask = 0xFu;
            event.FieldMask = HvKernelFieldMask(idx) & event.ArgumentMask;
            HvKernelDecode(idx, event.Arguments, event.Fields, event.ArgumentMask);
            if (idx == HV_TARGET_COPY_VIRTUAL_MEMORY) { event.Status = (ULONG)STATUS_NOT_SUPPORTED; }
            event.Arguments[4] = event.Arguments[5] = 0;
            KernelTelemetryPublish(&event);
            return;
        }
        if (!MonitorMatches(&event)) { return; }
        MonitorPublish(&event);
    }
    if (!MonitorDebugEnabled(idx)) { return; }
    HV_INFO("SYSCALL: %s (idx=%u) core=%u RCX=%p RDX=%p R8=%p R9=%p RSP=%p RIP=%p\n",
        g_SyscallTargetMeta[idx].Name, idx, Core,
        (PVOID)Entry->Rcx, (PVOID)Entry->Rdx, (PVOID)Entry->R8, (PVOID)Entry->R9,
        (PVOID)Entry->Rsp, (PVOID)Entry->Rip);
}

VOID SyscallDrainAllCores(VOID)
{
    ULONG core;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return; }
    SyscallAcquire();
    KernelTelemetryBeginDrain();
    for (core = 0; core < g_CpuCount; ++core) {
        PSYSCALL_RING ring = (g_PerCpuData[core] != NULL) ? g_PerCpuData[core]->SyscallRing : NULL;
        LONG64 tail;
        LONG64 head;
        if (ring == NULL) { continue; }
        tail = ring->Tail;
        head = ring->Head;
        while (tail != head) {
            SYSCALL_RING_ENTRY batch[16];
            ULONG count = 0, i;
            while (tail != head && count < RTL_NUMBER_OF(batch)) {
                batch[count++] = ring->Entry[tail & SYSCALL_RING_MASK];
                ++tail;
            }
            // Publish freed slots BEFORE potentially slow DebugView/console delivery.
            // Only the local copies are accessed after advancing Tail.
            InterlockedExchange64(&ring->Tail, tail);
            for (i = 0; i < count; ++i) { SyscallLogEntry(core, &batch[i]); }
        }
        InterlockedExchange64(&ring->Tail, tail);
    }
    FilterDrainLogs();
    KernelTelemetryDrain();
    KernelTelemetryEndDrain();
    SyscallRelease();
}

NTSTATUS SyscallHookInstall(ULONG Mask)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i;
    ULONG core;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || Mask == 0 || (Mask & ~HV_MONITOR_ALL)) { return STATUS_INVALID_DEVICE_STATE; }

    PersistAcquire();
    if (g_PersistActive != HV_STATE_RUNNING) {
        PersistRelease();
        return STATUS_INVALID_DEVICE_STATE;
    }
    PersistRelease();

    SyscallAcquire();
    KernelTelemetryBeginDrain();
    for (core = 0; core < g_CpuCount; ++core) {
        if (g_PerCpuData[core] == NULL || g_PerCpuData[core]->SyscallRing != NULL) {
            SyscallRelease();
            return STATUS_INVALID_DEVICE_STATE;
        }
    }
    status = FilterInitialize();
    if (!NT_SUCCESS(status)) { goto Failed; }
    // Allocate the per-core rings first so the producer always has a target.
    for (core = 0; core < g_CpuCount; ++core) {
        PPER_CPU_DATA cpu = g_PerCpuData[core];
        cpu->SyscallRing = (PSYSCALL_RING)ExAllocatePool2(
            POOL_FLAG_NON_PAGED | POOL_FLAG_CACHE_ALIGNED, sizeof(SYSCALL_RING), SYSCALL_TAG);
        if (cpu->SyscallRing == NULL) { status = STATUS_INSUFFICIENT_RESOURCES; goto Failed; }
        RtlZeroMemory(cpu->SyscallRing, sizeof(SYSCALL_RING));
    }

    for (i = 0; i < SYSCALL_TARGET_COUNT; ++i) {
        UNICODE_STRING name;
        PVOID address;

        g_SyscallTargetVa[i] = 0;
        g_SyscallTargetMeta[i].Address = 0;
        if (!(Mask & (1u << i))) { continue; }
        RtlInitUnicodeString(&name, g_SyscallNames[i]);
        address = MmGetSystemRoutineAddress(&name);
        if (address == NULL || !SyscallIsKernelText((ULONG64)(ULONG_PTR)address)) {
            HV_ERROR("SYSCALL: %s not resolved to a kernel text address\n",
                g_SyscallTargetMeta[i].Name);
            status = STATUS_PROCEDURE_NOT_FOUND;
            goto Failed;
        }
        g_SyscallTargetVa[i] = (ULONG64)(ULONG_PTR)address;
        g_SyscallTargetMeta[i].Address = g_SyscallTargetVa[i];

    }
    status = HookInstallSyscallsAllCores(g_SyscallTargetVa);
    if (!NT_SUCCESS(status)) { goto Failed; }
    for (i = 0; i < SYSCALL_TARGET_COUNT; ++i) {
        if (!(Mask & (1u << i)) || !MonitorDebugEnabled(i)) { continue; }
        HV_INFO("SYSCALL: %s hook installed on %lu core(s) (VA=0x%p)\n",
            g_SyscallTargetMeta[i].Name, g_CpuCount, (PVOID)g_SyscallTargetVa[i]);
    }
    SyscallRelease();
    return STATUS_SUCCESS;

Failed:
    FilterCloseAndDrain();
    HookRemoveSyscallsAllCores();
    // DriverEntry stops the worker and VMX before the final cleanup. Retain
    // rings here in case a failed remove still has a live hardware mapping.
    SyscallRelease();
    return status;
}

VOID SyscallHookRemove(VOID)
{
    ULONG i;
    ULONG core;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return; }
    FilterCloseAndDrain();
    SyscallAcquire();
    HookRemoveSyscallsAllCores();
    for (core = 0; core < g_CpuCount; ++core) {
        for (i = 0; i < SYSCALL_TARGET_COUNT; ++i) {
            if (g_PerCpuData[core] != NULL && g_PerCpuData[core]->SyscallHooks[i] != NULL) {
                HV_ERROR("SYSCALL: retaining rings until hook removal completes\n");
                SyscallRelease();
                return;
            }
        }
    }
    for (core = 0; core < g_CpuCount; ++core) {
        if (g_PerCpuData[core] != NULL && g_PerCpuData[core]->SyscallRing != NULL) {
            ExFreePoolWithTag(g_PerCpuData[core]->SyscallRing, SYSCALL_TAG);
            g_PerCpuData[core]->SyscallRing = NULL;
        }
    }
    SyscallRelease();
}

// Serializes console control against asynchronous persistent shutdown.
static EX_PUSH_LOCK g_ControlLock;
VOID SyscallControlAcquire(VOID)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_ControlLock);
}
VOID SyscallControlRelease(VOID)
{
    ExReleasePushLockExclusive(&g_ControlLock);
    KeLeaveCriticalRegion();
}
BOOLEAN SyscallHooksPresent(VOID)
{
    ULONG core, i;
    for (core = 0; core < g_CpuCount; ++core) {
        if (g_PerCpuData[core] == NULL) { continue; }
        for (i = 0; i < SYSCALL_TARGET_COUNT; ++i) {
            if (g_PerCpuData[core]->SyscallHooks[i] != NULL) { return TRUE; }
        }
    }
    return FALSE;
}

ULONG SyscallHooksMask(VOID)
{
    ULONG core, i, mask = 0;
    for (core = 0; core < g_CpuCount; ++core) {
        if (g_PerCpuData[core] == NULL) { continue; }
        for (i = 0; i < SYSCALL_TARGET_COUNT; ++i) {
            if (g_PerCpuData[core]->SyscallHooks[i] != NULL) { mask |= 1u << i; }
        }
    }
    return mask;
}
ULONG SyscallAvailableMask(VOID)
{
    ULONG i, j, mask;
    ULONG64 addresses[SYSCALL_TARGET_COUNT] = { 0 };
    static volatile LONG reportedAliases;
    for (i = 0; i < SYSCALL_TARGET_COUNT; ++i) {
        UNICODE_STRING name;
        if (!g_SyscallNames[i]) { continue; }
        RtlInitUnicodeString(&name, g_SyscallNames[i]);
        addresses[i] = (ULONG64)(ULONG_PTR)MmGetSystemRoutineAddress(&name);
    }
    // Windows 10 can export these two names at the very same instruction.
    // Prefer ExFreePoolWithTag only for a shared entry. Distinct addresses
    // remain independently selectable on any Windows version. At an aliased
    // entry the caller's original export name (and tag validity) is unknowable.
    mask = HvUniqueEntryMask(addresses, SYSCALL_TARGET_COUNT, SyscallTargetExFreePoolWithTag);
    for (i = 0; i < SYSCALL_TARGET_COUNT; ++i) {
        if (addresses[i] == 0 || (mask & (1u << i)) != 0) { continue; }
        if ((InterlockedOr(&reportedAliases, (LONG)(1u << i)) & (LONG)(1u << i)) != 0) { continue; }
        for (j = 0; j < SYSCALL_TARGET_COUNT; ++j) {
            if ((mask & (1u << j)) != 0 && addresses[j] == addresses[i]) {
                HV_INFO("SYSCALL: %s aliases %s at %p; select the shared entry as %s\n",
                    g_SyscallTargetMeta[i].Name, g_SyscallTargetMeta[j].Name,
                    (PVOID)(ULONG_PTR)addresses[i], g_SyscallTargetMeta[j].Name);
                break;
            }
        }
    }
    return mask;
}
