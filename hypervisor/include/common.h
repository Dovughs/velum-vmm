// hypervisor/include/common.h
#pragma once
#include <ntddk.h>
#include <intrin.h>
#include "percpu.h"

#define PER_CPU_TAG 'UPCH'
#define HOST_STACK_TAG 'KSTH'
#define HV_ERROR(...) ((VOID)DbgPrintEx(DPFLTR_IHVDRIVER_ID, \
    DPFLTR_ERROR_LEVEL, __VA_ARGS__))
#define HV_INFO(...) ((VOID)DbgPrintEx(DPFLTR_IHVDRIVER_ID, \
    DPFLTR_INFO_LEVEL, __VA_ARGS__))

extern PPER_CPU_DATA g_PerCpuData[MAXIMUM_PROCESSORS];
extern ULONG g_CpuCount;
extern volatile LONG g_VmxFailureCount;

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD DriverUnload;
