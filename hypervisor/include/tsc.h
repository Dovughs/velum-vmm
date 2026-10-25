// hypervisor/include/tsc.h
#pragma once
#include <ntddk.h>

// Intel SDM: secondary execution controls; 64-bit VMCS control-field encodings.
#define SECONDARY_TSC_SCALING (1u << 25)
#define VMCS_TSC_MULTIPLIER   0x00002032u
#define TSC_MULTIPLIER_ONE    (1ull << 48)

typedef struct _TSC_STATE {
    ULONG64 HostTscAtInitialize; // Owner-core diagnostic sample, NOT boot time.
    LONG64 Offset;              // Effective offset; zero while runtime option is OFF.
    ULONG64 DeadlineGuest;     // Shadow only while translating deadline MSR accesses.
    ULONG64 Multiplier;         // 1.0 if enabled; zero means field not accessed.
    ULONG64 EnumeratedTscHz;    // Leaf 15 calculation, zero if not enumerated.
    ULONG MaxBasicLeaf;
    ULONG Leaf15[4];
    ULONG Leaf16[4];
    ULONG OwnerIndex;
    ULONG LastVmcsField;        // For setup failure diagnostics only.
    BOOLEAN ScalingEnabled;
    BOOLEAN Initialized;
    BOOLEAN DeadlineSupported;
} TSC_STATE;

// Fixed offset only; never measure/compensate VM-exit duration or change scaling.
// Runtime changes are experimental clock discontinuities, not clock calibration.
struct _PER_CPU_DATA;
NTSTATUS TscInitializeForCore(struct _PER_CPU_DATA *Cpu); // PASSIVE, affinity pinned.
NTSTATUS TscWriteVmcs(struct _PER_CPU_DATA *Cpu); // Owner core, root, current VMCS.
VOID TscLogState(struct _PER_CPU_DATA *Cpu); // PASSIVE, after successful launch.
NTSTATUS TscSetOffsetCurrent(struct _PER_CPU_DATA *Cpu, BOOLEAN Enabled, LONG64 Ticks);
BOOLEAN TscDeadlineMsr(struct _PER_CPU_DATA *Cpu, BOOLEAN Write, ULONG Msr, ULONG64 *Value);
