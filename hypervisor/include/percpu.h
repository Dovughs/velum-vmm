// hypervisor/include/percpu.h
#pragma once
#include "vmx.h"
#include "ept.h"
#include "native.h"
#include "syscall.h"
#include "vpid.h"
#include "tsc.h"
#include "processor_config.h"

struct _HOOK_CONTEXT;

// Guest general-purpose registers. SDM Vol. 3C 27.1: the GPRs are NOT saved into
// the VMCS on VM-exit, so the exit stub (src/vmx.asm) must save/restore them
// manually. RSP is intentionally absent (it lives in VMCS_GUEST_RSP). The field
// order is fixed; vmexit.c and vmx.asm depend on offsets 0, 8, 16, ... (8 bytes).
typedef struct _GUEST_GPREGS {
    ULONG64 Rax;
    ULONG64 Rcx;
    ULONG64 Rdx;
    ULONG64 Rbx;
    ULONG64 Rbp;
    ULONG64 Rsi;
    ULONG64 Rdi;
    ULONG64 R8;
    ULONG64 R9;
    ULONG64 R10;
    ULONG64 R11;
    ULONG64 R12;
    ULONG64 R13;
    ULONG64 R14;
    ULONG64 R15;
} GUEST_GPREGS, *PGUEST_GPREGS;

// per-core persistent state. Accessed by vmexit.c (shutdown) and vmx.asm
// (reads ShutdownRip/ShutdownRsp via FIELD_OFFSET-based offsets).
typedef struct _PERSIST_STATE {
    volatile LONG Launched;     // 1 once this core is a guest (VMLAUNCH succeeded).
    volatile LONG ShuttingDown; // 1 while this core's VM is shutting down.
    ULONG64 ShutdownRip;        // Native continuation RIP (guest RIP past the VMCALL).
    ULONG64 ShutdownRsp;        // Native continuation RSP (guest RSP at the VMCALL).
    volatile LONG PendingNmi;
    BOOLEAN Preparing;
    PVOID MsrBitmap;
    PVOID RootIdt;
} PERSIST_STATE;

#define HOST_STACK_SIZE (16u * 1024u)
#define PER_CPU_ALIGNMENT 64u

#pragma warning(push)
#pragma warning(disable:4324)
typedef struct __declspec(align(64)) _PER_CPU_DATA {
    PVOID AllocationBase;
    PVOID VmxonVa;
    PHYSICAL_ADDRESS VmxonPa;
    PMDL VmxonMdl;
    PVOID VmcsVa;
    PHYSICAL_ADDRESS VmcsPa;
    PVOID HostStackVa;
    // Virtual stack pointer for VMCS HOST_RSP in the next step.
    ULONG_PTR HostStackTop;
    VMX_CAPABILITIES Capabilities;
    ULONG64 SavedCr4;
    volatile LONG VmxEnabled;
    volatile LONG VmcsActive;
    volatile LONG StartResult;
    ULONG VmxInstructionStatus;
    KSPIN_LOCK EptLock;
    PMDL VmcsMdl;
    ULONG LastVmInstructionError;
    BOOLEAN VmInstructionErrorValid;
    // Assembly launch frame restored before returning to the pinned C caller.
    ULONG64 LaunchCallerRsp;
    ULONG LaunchResult;
    ULONG64 ExitReason;
    ULONG64 ExitGuestRax;
    BOOLEAN ExitReasonValid;
    BOOLEAN TransferCet;
    PHYSICAL_ADDRESS EptPml4Pa;
    EPT_CONTEXT *Ept;
    ULONG64 Eptp;
    struct _HOOK_CONTEXT *Hook;
    // up to SYSCALL_TARGET_COUNT simultaneously installed syscall hooks
    // (one HOOK_CONTEXT each, prepared/committed/removed per slot). The
    // self-test keeps using the single Hook field above.
    struct _HOOK_CONTEXT *SyscallHooks[SYSCALL_TARGET_COUNT];
    // The hook being committed/removed by the current IPI/CPUID pass. Set during
    // prepare (and before a remove pass) so HookCommitOneCore/HookRemoveCommitOneCore
    // operate on the right context without threading an index through the IPI.
    struct _HOOK_CONTEXT *PendingHook;
    // The hook whose MTF read-emulation is currently armed (vmexit.c).
    struct _HOOK_CONTEXT *ReadMtfHook;
    // per-core lock-free syscall ring (allocated at install, freed at remove).
    PSYSCALL_RING SyscallRing;
    // One-instruction guest RET retirement; never reused for a sleeping helper.
    volatile BOOLEAN FilterReturnPending;
    ULONG64 FilterReturnFlags;
    ULONG64 FilterReturnPrimary;
    ULONG64 FilterReturnRsp;
    GUEST_GPREGS GuestRegs;
    PERSIST_STATE Persist;
    HV_NATIVE_STATE Native;
    VPID_STATE Vpid;
    TSC_STATE Tsc;
    HV_PROCESSOR_CONFIG ProcessorConfig;
    volatile LONG ProcessorCommitStatus;
} PER_CPU_DATA, *PPER_CPU_DATA;
#pragma warning(pop)

NTSTATUS PerCpuAllocate(VOID);
NTSTATUS PerCpuAllocateRegions(VOID);
NTSTATUS PerCpuAllocateStacks(VOID);
VOID PerCpuFree(VOID);
NTSTATUS PerCpuAllocateVmcsRegions(VOID);
VOID PerCpuFreeVmcsRegions(VOID);
