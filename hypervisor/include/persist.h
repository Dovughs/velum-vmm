// hypervisor/include/persist.h
#pragma once
#include <ntddk.h>
#include "control.h"

// persistent virtualization. After PersistLaunchAllCores, Windows runs
// as the guest on every core and the VM-exit handler services every exit until
// PersistShutdownAllCores. This is the production hypervisor model (SDM Vol. 3C
// 25/27; the OS is the guest, the VMM is the host).

// Shutdown VMCALL magic. HvPersistentShutdown sets RAX to this before VMCALL;
// the handler recognizes it and VMCLEAR + VMXOFF the core (SDM Vol. 3C 25.2).
// Reads as "SHUT" (bytes S,H,U,T low-to-high); distinct from 0xDEADBEEF/0xCAFEBABE.
#define PERSIST_SHUTDOWN_MAGIC 0x54554853ull

// Global flag: 1 once the persistent launch has begun (Windows is the guest).
extern volatile LONG g_PersistActive;

// Launch a persistent VM on every core (APs first, BSP last). Returns on the BSP
// after every core is a guest. Not re-entrant; on partial failure it shuts down
// already-launched cores and returns an error.
NTSTATUS PersistLaunchAllCores(VOID);

// Shut down the persistent VMs on every core (VMCALL shutdown protocol -> VMXOFF).
// Only valid after PersistLaunchAllCores succeeded; idempotent.
VOID PersistShutdownAllCores(VOID);

// asm entry points (src/vmx.asm).
extern ULONG HvPersistentLaunch(ULONG64 *CallerRspSave); // 0=success, else VMfail code.
extern VOID HvPersistentGuestEntry(VOID);                 // guest continuation label.
extern VOID HvPersistentShutdown(VOID);                   // VMCALLs with the magic.

NTSTATUS PersistAllocate(VOID);
VOID PersistFree(VOID);
VOID PersistPoll(VOID);
ULONG64 PersistHandleControl(struct _PER_CPU_DATA *Cpu);
ULONG64 PersistPrepareNative(struct _PER_CPU_DATA *Cpu);
__declspec(noreturn) VOID PersistFatal(struct _PER_CPU_DATA *Cpu, ULONG64 Reason);
VOID PersistRootNmi(VOID);
__declspec(noreturn) VOID PersistRootFault(VOID);
VOID PersistAcquire(VOID);
VOID PersistRelease(VOID);
NTSTATUS HvHookControl(ULONG Command, ULONG64 Gpa);
extern VOID HvHookCpuid(VOID);
