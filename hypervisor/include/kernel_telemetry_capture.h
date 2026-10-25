#pragma once
#include "filter.h"
NTSTATUS KernelTelemetryInitialize(VOID);
ULONG KernelCapture(ULONG Target, const FILTER_ARGUMENTS *Args, ULONG64 OriginalRsp);
VOID KernelTelemetryBeginDrain(VOID);
VOID KernelTelemetryDrain(VOID);
VOID KernelTelemetryEndDrain(VOID);
VOID KernelTelemetryPublish(HV_MONITOR_EVENT *Event);
VOID KernelTelemetryStats(HV_KERNEL_STATS *Output, BOOLEAN Reset);

