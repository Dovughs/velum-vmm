#include "common.h"
#include "persist.h"
#include "hook.h"
#include "processor.h"

// Lifecycle lock owns these resident snapshots. VMX root reads only the staged
// snapshot during the authenticated, synchronous all-processor commit.
static HV_PROCESSOR_CONFIG g_ProcessorConfig;
static HV_PROCESSOR_CONFIG g_ProcessorPending;
static BOOLEAN g_ProcessorInitialized;
static volatile LONG g_ProcessorCommitArmed;
C_ASSERT(sizeof(HV_PROCESSOR_CONFIG) == 536);

VOID ProcessorInitialize(PPER_CPU_DATA Cpu)
{
    if (!g_ProcessorInitialized) {
        HvProcessorDefaults(&g_ProcessorConfig);
        g_ProcessorInitialized = TRUE;
    }
    Cpu->ProcessorConfig = g_ProcessorConfig;
}

NTSTATUS ProcessorCommitCurrent(PPER_CPU_DATA Cpu)
{
    NTSTATUS status;
    if (!g_ProcessorCommitArmed) { return STATUS_INVALID_DEVICE_STATE; }
    status = TscSetOffsetCurrent(Cpu, g_ProcessorPending.OffsetEnabled != 0,
        g_ProcessorPending.OffsetTicks);
    if (NT_SUCCESS(status)) { Cpu->ProcessorConfig = g_ProcessorPending; }
    return status;
}

static ULONG_PTR ProcessorCommitIpi(ULONG_PTR Context)
{
    ULONG index = KeGetCurrentProcessorNumber();
    UNREFERENCED_PARAMETER(Context);
    if (index < g_CpuCount) {
        PPER_CPU_DATA cpu = g_PerCpuData[index];
        cpu->ProcessorCommitStatus = cpu->Persist.Launched ?
            HvHookControl(HV_CONTROL_PROCESSOR_COMMIT, 0) : STATUS_INVALID_DEVICE_STATE;
    }
    return 0;
}

static NTSTATUS ProcessorBroadcast(const HV_PROCESSOR_CONFIG *Config)
{
    ULONG i;
    NTSTATUS status = STATUS_SUCCESS;
    g_ProcessorPending = *Config;
    for (i = 0; i < g_CpuCount; ++i) {
        g_PerCpuData[i]->ProcessorCommitStatus = STATUS_PENDING;
    }
    InterlockedExchange(&g_ProcessorCommitArmed, 1);
    KeIpiGenericCall(ProcessorCommitIpi, 0);
    InterlockedExchange(&g_ProcessorCommitArmed, 0);
    for (i = 0; i < g_CpuCount; ++i) {
        NTSTATUS result = g_PerCpuData[i]->ProcessorCommitStatus;
        if (result == STATUS_PENDING || !NT_SUCCESS(result)) {
            status = result == STATUS_PENDING ? STATUS_UNSUCCESSFUL : result;
        }
    }
    return status;
}

NTSTATUS ProcessorGet(HV_PROCESSOR_CONFIG *Config)
{
    NTSTATUS status = STATUS_INVALID_DEVICE_STATE;
    PersistAcquire();
    if (g_ProcessorInitialized && g_PersistActive == HV_STATE_RUNNING) {
        *Config = g_ProcessorConfig;
        status = STATUS_SUCCESS;
    }
    PersistRelease();
    return status;
}

NTSTATUS ProcessorSet(const HV_PROCESSOR_CONFIG *Config)
{
    NTSTATUS status = STATUS_INVALID_DEVICE_STATE;
    if (!HvProcessorValid(Config)) { return STATUS_INVALID_PARAMETER; }
    PersistAcquire();
    if (g_ProcessorInitialized && g_PersistActive == HV_STATE_RUNNING) {
        status = ProcessorBroadcast(Config);
        if (NT_SUCCESS(status)) { g_ProcessorConfig = *Config; }
        else if (!NT_SUCCESS(ProcessorBroadcast(&g_ProcessorConfig))) {
            // Continuing with inconsistent per-core clocks is not recoverable.
            KeBugCheckEx(0x20001, 0x310, (ULONG)status, 0, 0);
        }
    }
    PersistRelease();
    return status;
}
