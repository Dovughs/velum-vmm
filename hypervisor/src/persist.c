// Persistent lifecycle runs in Windows guest context; root handlers never call
// scheduling, allocation or IPI APIs. The worker serializes shutdown with unload.
#include <ntifs.h>
#include "syscall.h"
#include "common.h"
#include "vmcs.h"
#include "persist.h"
#include "hook.h"
#include "filter.h"
#include "processor.h"

#define PERSIST_TAG 'rPvH'
volatile LONG g_PersistActive = HV_STATE_STARTING;
static EX_PUSH_LOCK g_LifecycleLock = { 0 };

VOID PersistAcquire(VOID)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusiveEx(&g_LifecycleLock, 0);
}

VOID PersistRelease(VOID)
{
    ExReleasePushLockExclusiveEx(&g_LifecycleLock, 0);
    KeLeaveCriticalRegion();
}

typedef struct _ROOT_GATE {
    USHORT Low, Selector;
    UCHAR Ist, Attributes;
    USHORT Middle;
    ULONG High, Reserved;
} ROOT_GATE;
C_ASSERT(sizeof(ROOT_GATE) == 16);

static VOID SetGate(ROOT_GATE *Gate, ULONG64 Target, USHORT Selector)
{
    Gate->Low = (USHORT)Target;
    Gate->Middle = (USHORT)(Target >> 16);
    Gate->High = (ULONG)(Target >> 32);
    Gate->Selector = Selector;
    Gate->Attributes = 0x8E;
}

NTSTATUS PersistAllocate(VOID)
{
    ULONG i, vector;
    for (i = 0; i < g_CpuCount; ++i) {
        PPER_CPU_DATA cpu = g_PerCpuData[i];
        NTSTATUS status;
        cpu->Persist.MsrBitmap = ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE, PERSIST_TAG);
        cpu->Persist.RootIdt = ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE, PERSIST_TAG);
        if (cpu->Persist.MsrBitmap == NULL || cpu->Persist.RootIdt == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        if (((ULONG_PTR)cpu->Persist.MsrBitmap & (PAGE_SIZE - 1)) != 0) {
            return STATUS_DATATYPE_MISALIGNMENT;
        }
        for (vector = 0; vector < 256; ++vector) {
            SetGate(&((ROOT_GATE *)cpu->Persist.RootIdt)[vector],
                (ULONG64)(ULONG_PTR)(vector == 2 ? HvRootNmi : HvRootFault), (USHORT)HvGetCs());
        }
        status = EptPrepareRuntime(cpu);
        if (!NT_SUCCESS(status)) { return status; }
    }
    return STATUS_SUCCESS;
}

VOID PersistFree(VOID)
{
    ULONG i;
    for (i = 0; i < g_CpuCount; ++i) {
        PPER_CPU_DATA cpu = g_PerCpuData[i];
        if (cpu == NULL) { continue; }
        if (cpu->Persist.MsrBitmap != NULL) {
            ExFreePoolWithTag(cpu->Persist.MsrBitmap, PERSIST_TAG);
            cpu->Persist.MsrBitmap = NULL;
        }
        if (cpu->Persist.RootIdt != NULL) {
            ExFreePoolWithTag(cpu->Persist.RootIdt, PERSIST_TAG);
            cpu->Persist.RootIdt = NULL;
        }
    }
}

static NTSTATUS LaunchCore(PPER_CPU_DATA Cpu, ULONG Index)
{
    GROUP_AFFINITY target = { 0 }, previous;
    KIRQL oldIrql;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    ULONG result;
    target.Mask = (KAFFINITY)1 << Index;
    KeSetSystemGroupAffinityThread(&target, &previous);
    ProcessorInitialize(Cpu);
    // Owner-core sample/capabilities at PASSIVE_LEVEL; no clock epoch reset.
    status = TscInitializeForCore(Cpu);
    if (!NT_SUCCESS(status)) {
        HV_ERROR("TSC: core %lu initialization failed status=0x%08lX\n", Index, (ULONG)status);
        KeRevertToUserGroupAffinityThread(&previous);
        return status;
    }
    // Keep the existing failure result if VMCLEAR/VMPTRLD below fails.
    status = STATUS_UNSUCCESSFUL;
    oldIrql = KeRaiseIrqlToDpcLevel();
    Cpu->Persist.Preparing = TRUE;
    if (VmxClear((ULONG64)Cpu->VmcsPa.QuadPart) == 0 &&
        VmxPtrLoad((ULONG64)Cpu->VmcsPa.QuadPart) == 0) {
        Cpu->VmcsActive = 1;
        status = VmcsPopulateCurrent(Cpu, Index);
        if (NT_SUCCESS(status)) {
            *(PPER_CPU_DATA *)Cpu->HostStackTop = Cpu;
            Cpu->LaunchResult = 3;
            // Mark before entry: even the first VM exit must use runtime rules.
            Cpu->Persist.Launched = 1;
            result = VmcsLaunchOnCurrentCoreTest(&Cpu->LaunchCallerRsp, Cpu->TransferCet, 1);
            if (result != 0) {
                Cpu->Persist.Launched = 0;
                (VOID)VmcsQueryInstructionError(&Cpu->LastVmInstructionError);
                HV_ERROR("VMX: persistent core %lu entry failed status=%lu error=%lu\n",
                    Index, result, Cpu->LastVmInstructionError);
                status = STATUS_UNSUCCESSFUL;
            }
        }
    }
    Cpu->Persist.Preparing = FALSE;
    KeLowerIrql(oldIrql);
    if (NT_SUCCESS(status)) { TscLogState(Cpu); }
    else if (Cpu->Tsc.LastVmcsField != 0) {
        HV_ERROR("TSC: core %lu VMCS setup failed field=0x%lX status=0x%08lX\n",
            Index, Cpu->Tsc.LastVmcsField, (ULONG)status);
    }
    KeRevertToUserGroupAffinityThread(&previous);
    return status;
}

NTSTATUS PersistLaunchAllCores(VOID)
{
    ULONG i;
    NTSTATUS status = STATUS_SUCCESS;
    KAPC_STATE attach;
    InterlockedExchange(&g_PersistActive, HV_STATE_STARTING);
    // Root CR3 must outlive the loader's process and map all kernel allocations.
    KeStackAttachProcess((PRKPROCESS)PsInitialSystemProcess, &attach);
    for (i = 0; i < g_CpuCount; ++i) {
        HV_INFO("START: persistent core %lu entering launch\n", i);
        status = LaunchCore(g_PerCpuData[i], i);
        HV_INFO("START: persistent core %lu returned status=0x%08lX\n", i, (ULONG)status);
        if (!NT_SUCCESS(status)) { break; }
    }
    KeUnstackDetachProcess(&attach);
    if (!NT_SUCCESS(status)) {
        PersistShutdownAllCores(); // per-core flags also cover a partial launch
        return status;
    }
    InterlockedExchange(&g_PersistActive, HV_STATE_RUNNING);
    return STATUS_SUCCESS;
}

static ULONG_PTR ShutdownCore(ULONG_PTR Context)
{
    ULONG index = KeGetCurrentProcessorNumber();
    PPER_CPU_DATA cpu = g_PerCpuData[index];
    UNREFERENCED_PARAMETER(Context);
    if (cpu != NULL && cpu->Persist.Launched) {
        HvPersistentShutdown();
        // These stores run natively only after the assembly VMXOFF succeeded.
        cpu->Persist.Launched = 0;
        cpu->VmxEnabled = 0;
        cpu->VmcsActive = 0;
    }
    return 0;
}

VOID PersistShutdownAllCores(VOID)
{
    if (g_CpuCount == 0) { return; }
    SyscallControlAcquire();
    // Close admission before VMXOFF; guest helpers may be sleeping on paging I/O.
    FilterCloseAndDrain();
    PersistAcquire();
    InterlockedExchange(&g_PersistActive, HV_STATE_STOPPING);
    (VOID)KeIpiGenericCall(ShutdownCore, 0);
    InterlockedExchange(&g_PersistActive, HV_STATE_STOPPED);
    PersistRelease();
    SyscallControlRelease();
}

VOID PersistPoll(VOID)
{
    if (g_PersistActive == HV_STATE_STOP_REQUESTED) {
        PersistShutdownAllCores();
        HV_INFO("VMX: CPUID shutdown complete on all cores; Windows continues natively\n");
    }
}

ULONG64 PersistHandleControl(PPER_CPU_DATA Cpu)
{
    ULONG command = (ULONG)Cpu->GuestRegs.Rcx;
    ULONG64 rip = 0, cs = 0;
    if (command >= HV_CONTROL_HOOK_COMMIT && command <= HV_CONTROL_PROCESSOR_COMMIT) {
        NTSTATUS status = STATUS_ACCESS_DENIED;
        if (VmxRead(VMCS_GUEST_RIP, &rip) != 0 || VmxRead(VMCS_GUEST_CS_SELECTOR, &cs) != 0) {
            PersistFatal(Cpu, 0x106);
        }
        if ((cs & 3) == 0 && rip == (ULONG64)(ULONG_PTR)HvHookCpuid &&
            (g_PersistActive == HV_STATE_RUNNING || g_PersistActive == HV_STATE_STOP_REQUESTED)) {
            if (command == HV_CONTROL_HOOK_COMMIT) { status = HookCommitOneCore(Cpu); }
            else if (command == HV_CONTROL_HOOK_REMOVE) { status = HookRemoveCommitOneCore(Cpu); }
            else if (command == HV_CONTROL_EPT_RESTORE) { status = EptRestoreLargePage(Cpu, Cpu->GuestRegs.Rdx); }
            else { status = ProcessorCommitCurrent(Cpu); }
        }
        Cpu->GuestRegs.Rax = (ULONG)status;
        if (VmxWrite(VMCS_GUEST_RIP, rip + 2) != 0) { PersistFatal(Cpu, 0x107); }
        return 0;
    }
    if (command == HV_CONTROL_LEAVE_CORE) {
        if (VmxRead(VMCS_GUEST_RIP, &rip) != 0 || VmxRead(VMCS_GUEST_CS_SELECTOR, &cs) != 0) {
            PersistFatal(Cpu, 0x101);
        }
        if ((cs & 3) == 0 && rip == (ULONG64)(ULONG_PTR)HvShutdownCpuid &&
            g_PersistActive == HV_STATE_STOPPING) {
            // Let a queued NMI run before the final, two-instruction handshake.
            if (Cpu->Persist.PendingNmi) { return 0; }
            if (!NT_SUCCESS(TscSetOffsetCurrent(Cpu, FALSE, 0))) { PersistFatal(Cpu, 0x312); }
            return PersistPrepareNative(Cpu);
        }
    }
    if (command == HV_CONTROL_STOP) {
        (VOID)InterlockedCompareExchange(&g_PersistActive,
            HV_STATE_STOP_REQUESTED, HV_STATE_RUNNING);
    }
    Cpu->GuestRegs.Rax = HV_CONTROL_SIGNATURE;
    Cpu->GuestRegs.Rbx = (ULONG)g_PersistActive;
    Cpu->GuestRegs.Rcx = 1; // ABI version
    Cpu->GuestRegs.Rdx = g_CpuCount;
    if (VmxRead(VMCS_GUEST_RIP, &rip) != 0 || VmxWrite(VMCS_GUEST_RIP, rip + 2) != 0) {
        PersistFatal(Cpu, 0x102);
    }
    return 0;
}

__declspec(noreturn) VOID PersistFatal(PPER_CPU_DATA Cpu, ULONG64 Reason)
{
    ULONG64 rip = 0, qualification = 0;
    (VOID)VmxRead(VMCS_GUEST_RIP, &rip);
    (VOID)VmxRead(VMCS_EXIT_QUALIFICATION, &qualification);
    // Never jump to an expired launch frame or silently HLT a Windows CPU.
    KeBugCheckEx(0x20001, (ULONG_PTR)Reason, (ULONG_PTR)rip,
        (ULONG_PTR)qualification, (ULONG_PTR)Cpu);
    __assume(0);
}

VOID PersistRootNmi(VOID)
{
    PPER_CPU_DATA cpu = g_PerCpuData[KeGetCurrentProcessorNumber()];
    ULONG64 primary = 0;
    InterlockedExchange(&cpu->Persist.PendingNmi, 1);
    if (cpu->FilterReturnPending) { return; } // Deliver after the guest RET retires.
    if (VmxRead(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS, &primary) != 0 ||
        VmxWrite(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS,
            primary | PRIMARY_NMI_WINDOW_EXITING) != 0) {
        PersistFatal(cpu, 0x103);
    }
}

__declspec(noreturn) VOID PersistRootFault(VOID)
{
    PersistFatal(g_PerCpuData[KeGetCurrentProcessorNumber()], 0x104);
}
