// hypervisor/include/vpid.h
#pragma once
#include <ntddk.h>

// Owned by one fixed logical processor, just like its private EPT. No allocator,
// locks, Windows calls or guest-memory access in the root-side operations.
typedef struct _VPID_STATE {
    USHORT Id;
    UCHAR InvalidationType; // 1: single context including globals; 2: all contexts.
    BOOLEAN Enabled;
    ULONG LastInstructionStatus;
    ULONG64 Invalidations;
} VPID_STATE;

struct _PER_CPU_DATA;
// VMX root, current VMCS loaded, pinned to Cpu's owner, before VMLAUNCH.
NTSTATUS VpidPrepareCurrent(struct _PER_CPU_DATA *Cpu, ULONG Index,
    PULONG SecondaryControls);
// VMX root on the owner. Disables tagging BEFORE invalidation so a final guest
// handshake cannot refill the retired VPID. Required before native handoff.
NTSTATUS VpidDeactivateCurrent(struct _PER_CPU_DATA *Cpu);
// Root-only rollback with no subsequent guest entry; no current VMCS required.
NTSTATUS VpidInvalidateCurrent(struct _PER_CPU_DATA *Cpu);
// Resident assembly wrapper; 0=success, 1=VMfailValid, 2=VMfailInvalid.
ULONG VpidInvvpid(ULONG Type, USHORT Id);
