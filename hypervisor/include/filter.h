// hypervisor/include/filter.h
#pragma once
#include <ntddk.h>
#include "syscall.h"
#include "monitor_protocol.h"

#define FILTER_ACTION_CONTINUE 0u
#define FILTER_ACTION_BLOCK    1u
// WDK shared/ntstatus.h: STATUS_ACCESS_DENIED == (NTSTATUS)0xC0000022L.
#define FILTER_DENIED_STATUS ((ULONG)STATUS_ACCESS_DENIED)

// Layout matches the saved volatile GPR block in the guest assembly frame.
typedef struct _FILTER_ARGUMENTS {
    ULONG64 Rax, Rcx, Rdx, R8, R9, R10, R11;
} FILTER_ARGUMENTS;

// Guest Windows context, PASSIVE_LEVEL and IF=1. Memory/name queries may page.
// Pointer failures fail open. Path logging copies data to a bounded queue;
// formatting is deferred to the worker, never VMX root.
ULONG FilterDecide(ULONG TargetIdx, FILTER_ARGUMENTS *Args, ULONG64 OriginalRsp);
NTSTATUS FilterInitialize(VOID);
NTSTATUS FilterConfigure(const HV_RUNTIME_CONFIG *Config);
VOID FilterDestroy(VOID);
VOID FilterDrainLogs(VOID);

// Resident atomic rundown. Root owns acquisition and release; shutdown closes
// admission and drains at PASSIVE_LEVEL before VMXOFF or freeing driver code.
BOOLEAN FilterTryEnter(VOID);
VOID FilterLeave(VOID);
VOID FilterCloseAndDrain(VOID);

// Per-target guest entry and continuation gates; NULL for unfiltered targets.
typedef VOID (*FILTER_GUEST_GATE)(VOID);
extern FILTER_GUEST_GATE const g_FilterGuestEntries[SYSCALL_TARGET_COUNT];
extern FILTER_GUEST_GATE const g_FilterContinueGates[SYSCALL_TARGET_COUNT];
extern FILTER_GUEST_GATE const g_FilterBlockGates[SYSCALL_TARGET_COUNT];
extern VOID HvFilterReturn(VOID);
