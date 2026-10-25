// Syscall events are recorded by the per-core VM-exit handler and drained at
// PASSIVE_LEVEL. The original entry instruction executes at its original RIP.
#pragma once
#include <ntddk.h>

#define SYSCALL_TARGET_COUNT 26u

// Stable indices: preserve the original two targets and append file operations.
typedef enum _SYSCALL_TARGET_INDEX {
    SyscallTargetNtCreateFile = 0,
    SyscallTargetNtAllocateVirtualMemory = 1,
    SyscallTargetNtOpenFile = 2,
    SyscallTargetNtDeleteFile = 3,
    SyscallTargetNtSetInformationFile = 4,
    SyscallTargetNtWriteFile = 5,
    SyscallTargetNtCreateSection = 6,
    SyscallTargetNtMapViewOfSection = 7,
    SyscallTargetNtOpenProcess = 8,
    SyscallTargetReserved9 = 9,
    SyscallTargetNtProtectVirtualMemory = 10,
    SyscallTargetReserved11 = 11,
    SyscallTargetNtReadFile = 12,
    SyscallTargetExAllocatePool2 = 13,
    SyscallTargetExAllocatePoolWithTag = 14,
    SyscallTargetExFreePoolWithTag = 15,
    SyscallTargetExAllocatePoolWithTagPriority = 16,
    SyscallTargetExAllocatePool3 = 17,
    SyscallTargetExAllocatePool = 18,
    SyscallTargetExFreePool = 19,
    SyscallTargetMmAllocateContiguousMemory = 20,
    SyscallTargetMmFreeContiguousMemory = 21,
    SyscallTargetMmMapIoSpaceEx = 22,
    SyscallTargetMmUnmapIoSpace = 23,
    SyscallTargetNtDuplicateObject = 24,
    SyscallTargetMmCopyVirtualMemory = 25
} SYSCALL_TARGET_INDEX;

// Per-target metadata: name, resolved VA (filled at install), and the x64
// fastcall argument labels (arg1=RCX, arg2=RDX, arg3=R8, arg4=R9; arg5+ would
// require reading the guest stack, captured separately by the guest telemetry path).
typedef struct _SYSCALL_TARGET_META {
    PCSTR   Name;
    ULONG64 Address;
    PCSTR   ArgRcx;
    PCSTR   ArgRdx;
    PCSTR   ArgR8;
    PCSTR   ArgR9;
} SYSCALL_TARGET_META;

// Ring entry: 8 qwords = 64 bytes = exactly one cache line. A producer's slot
// write therefore never straddles a cache line and the producer/consumer never
// false-share on a single entry. Fields are RAW guest register values only.
#pragma warning(push)
#pragma warning(disable:4324)
typedef struct __declspec(align(64)) _SYSCALL_RING_ENTRY {
    ULONG64 TargetIndex;   // SYSCALL_TARGET_INDEX of the hooked Nt* function.
    ULONG64 Rcx;           // arg1 (raw; never dereferenced here).
    ULONG64 Rdx;           // arg2.
    ULONG64 R8;            // arg3.
    ULONG64 R9;            // arg4.
    ULONG64 Rsp;           // guest kernel stack pointer at the Nt* entry.
    ULONG64 Rip;           // the hooked Nt* entry VA.
    ULONG64 Flags;         // unmodified guest RFLAGS at entry.
} SYSCALL_RING_ENTRY;
C_ASSERT(sizeof(SYSCALL_RING_ENTRY) == 64);

#define SYSCALL_RING_ENTRIES 256u
#define SYSCALL_RING_MASK    (SYSCALL_RING_ENTRIES - 1u)

// SPSC ring: producer = the owning core VM-exit handler,
// consumer = the PASSIVE drain worker. Head is written only by the
// producer (lock inc), Tail only by the consumer. A full ring drops silently.
// Head(8) + Tail(8) + Reserved[6](48) = 64, so Entry[] is cache-line aligned.
typedef struct __declspec(align(64)) _SYSCALL_RING {
    volatile LONG64 Head;                  // producer-owned.
    volatile LONG64 Tail;                  // consumer-owned.
    ULONG64 Reserved[6];                   // pad so Entry[] begins at offset 64.
    SYSCALL_RING_ENTRY Entry[SYSCALL_RING_ENTRIES];
} SYSCALL_RING, *PSYSCALL_RING;
#pragma warning(pop)

extern ULONG64 g_SyscallTargetVa[SYSCALL_TARGET_COUNT];
VOID SyscallRecord(PSYSCALL_RING Ring, const SYSCALL_RING_ENTRY *Entry);
// Per-target metadata table (defined in src/syscall.c).
extern SYSCALL_TARGET_META g_SyscallTargetMeta[SYSCALL_TARGET_COUNT];
// Hit counters, incremented by the PASSIVE drain worker only.
extern volatile LONG64 g_SyscallHitCount[SYSCALL_TARGET_COUNT];

// hook.c multi-hook extensions (declared here to keep include/hook.h unchanged).
NTSTATUS HookInstallSyscallsAllCores(const ULONG64 *Addresses);
VOID     HookRemoveSyscallsAllCores(VOID);

// public API (PASSIVE_LEVEL only for install/remove).
NTSTATUS SyscallHookInstall(ULONG Mask);
ULONG SyscallHooksMask(VOID);
ULONG SyscallAvailableMask(VOID);
BOOLEAN SyscallHooksPresent(VOID);
VOID SyscallControlAcquire(VOID);
VOID SyscallControlRelease(VOID);
VOID SyscallLogAcquire(VOID); // Serialize log-filter updates with the drain worker.
VOID SyscallLogRelease(VOID);
VOID     SyscallHookRemove(VOID);
VOID     SyscallDrainAllCores(VOID);
