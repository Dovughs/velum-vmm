// hypervisor/include/hook.h
#pragma once
#include "common.h"

// ---------------------------------------------------------------------------
// execute-only EPT hidden hook (per-core EPT; targeted single-context
// INVEPT). SDM references: Vol. 3C 29.3.2 (EPT leaf entry, Table 29-6),
// 27.2.1 (EPT violation qualification, Table 27-7), 25.5.2 (monitor trap flag),
// 24.6.2 (primary processor-based controls); Vol. 3D A.10 (EPT_VPID_CAP bits);
// Vol. 3C Appendix C (basic exit reasons).
// ---------------------------------------------------------------------------

// Lock-free SPSC ring for deferred VM-exit events (no logging in the handler).
#define HOOK_RING_ENTRIES 256u
#define HOOK_RING_MASK    (HOOK_RING_ENTRIES - 1u)

// Jump encodings (Intel SDM Vol. 2, JMP / MOV).
// 12-byte absolute: 48 B8 imm64 (mov rax, imm64) ; FF E0 (jmp rax). Reaches any
// 64-bit destination, which is required because the shadow page and the handler
// are unrelated addresses (a 5-byte E9 rel32 is limited to +/-2 GiB).
#define HOOK_JUMP_ABSOLUTE_SIZE 12u
// Syscall entry trap: 0F 01 C1 (VMCALL).
#define HOOK_REPLAY_TRAP_SIZE 3u

// Deferred-event tags consumed by HookDrainAllCores' callback.
#define HOOK_EVENT_FIRED       0x1ull  // VMCALL with RAX == 0xCAFEBABE.
#define HOOK_EVENT_NOT_FIRED   0x2ull  // VMCALL with RAX == 0xDEADBEEF while installed.
#define HOOK_EVENT_EPT_EXECUTE 0x3ull  // Unexpected instruction-fetch EPT violation.
#define HOOK_EVENT_ERROR       0x4ull  // Generic handler error.
#define HOOK_EVENT_CPUID       0x5ull  // CPUID serviced in root (Value = packed leaf/subleaf).

// Pack/unpack CPUID leaf (bits 63:32) and subleaf (bits 31:0) into one ring Value.
#define HOOK_CPUID_PACK(leaf, subleaf) \
    ((((ULONG64)(leaf) & 0xFFFFFFFFull) << 32) | ((ULONG64)(subleaf) & 0xFFFFFFFFull))
#define HOOK_CPUID_LEAF(v)    ((ULONG)((v) >> 32))
#define HOOK_CPUID_SUBLEAF(v) ((ULONG)(v))

// CPUID spoof constants (SDM Vol. 2A leaves 0x40000000/0x40000001).
// Vendor string "Velum       " = EBX:EDX:ECX, first char in the low byte of EBX.
#define HV_HYPERVISOR_VENDOR_EBX  0x756C6556ull  // "Velu"
#define HV_HYPERVISOR_VENDOR_EDX  0x2020206Dull  // "m   "
#define HV_HYPERVISOR_VENDOR_ECX  0x20202020ull  // "    "
#define HV_HYPERVISOR_MAX_LEAF    0x40000001ull
#define HV_HYPERVISOR_INTERFACE   0x7648794Dull  // Stable control ABI signature.

// VmExitDispatch return contract (src/vmexit.c -> src/vmx.asm HvHostEntryStub):
// 0 requests VMRESUME; 1 requests the persistent shutdown path (VMCLEAR+VMXOFF +
// native continuation); any other value is the launcher RSP for the exit path.
#define VMEXIT_ACTION_RESUME   0ull
#define VMEXIT_ACTION_SHUTDOWN 1ull

// Cache-line aligned (C4324 is expected and harmless for these two ring types).
#pragma warning(push)
#pragma warning(disable:4324)
typedef struct __declspec(align(64)) _HOOK_RING_ENTRY {
    ULONG64 Tag;
    ULONG64 Value;
    ULONG64 CpuIndex;   // Producer's CPU index, captured at enqueue time.
} HOOK_RING_ENTRY;

// SPSC ring: producer = this core's VM-exit handler, consumer = PASSIVE drain.
// Head is written only by the producer (InterlockedIncrement64), Tail only by
// the consumer (InterlockedExchange64). A full ring drops the entry; never block.
typedef struct __declspec(align(64)) _HOOK_RING {
    volatile LONG64 Head;
    volatile LONG64 Tail;
    HOOK_RING_ENTRY Entry[HOOK_RING_ENTRIES];
} HOOK_RING, *PHOOK_RING;
#pragma warning(pop)

typedef void (*HOOK_EVENT_CALLBACK)(ULONG64 Tag, ULONG64 Value, ULONG64 CpuIndex, PVOID Context);

// One hook per core. All fields nonpaged; the ring is cache-aligned.
typedef struct _HOOK_CONTEXT {
    struct _HOOK_CONTEXT *PageOwner; // Earlier slot owns a shared shadow page; batch lifetime.
    ULONG64 TargetGpa;               // 4KB page base (GPA == host RAM, identity EPT).
    ULONG64 TargetFunctionOffset;    // Offset of the function within the 4KB page.
    PVOID   ShadowPageVa;            // Nonpaged, contiguous, 4KB-aligned, cached copy.
    PHYSICAL_ADDRESS ShadowPagePa;   // Physical address of the shadow page.
    PMDL    ShadowPageMdl;           // NULL when the contiguous-allocator path was used.
    ULONG64 *Pml1Entry;              // Cached PML1 leaf pointer (set at commit).
    ULONG64 OriginalPml1Entry;       // Saved leaf before the execute-only swap.
    ULONG64 ShadowPml1Entry;         // R=0/W=0/X=1, PA=shadow, type/ignore-PAT preserved.
    PVOID   HookStubVa;              // Shadow-page location holding the jump to the handler.
    ULONG64 ActualPatchSize;         // Original replay instruction length, or self-test detour size.
    NTSTATUS CommitStatus;           // Result of the commit IPI on this core.
    volatile LONG Installed;         // 1 once the leaf holds the shadow mapping.
    volatile LONG ReadMtfArmed;      // 1 while MTF is armed (leaf swapped to original).
    HOOK_RING Ring;                  // Deferred events (never logged in the handler).
} HOOK_CONTEXT, *PHOOK_CONTEXT;

// Public API (PASSIVE_LEVEL).
VOID     HookInitialize(VOID);
NTSTATUS HookInstallAllCores(ULONG64 TargetVirtualAddress, PVOID HookHandlerVa);
VOID     HookRemoveAllCores(ULONG64 TargetVirtualAddress);
VOID     HookDrainAllCores(VOID);
PVOID    HookTranslateVaToPa(ULONG64 TargetVirtualAddress, PULONG64 OffsetOut);

// Internal helpers used by vmexit.c / driver.c.
NTSTATUS HookPrepareOneCore(PPER_CPU_DATA Cpu, ULONG64 TargetGpa, ULONG64 FunctionOffset, PVOID HookHandlerVa);
NTSTATUS HookCommitOneCore(PPER_CPU_DATA Cpu);
NTSTATUS HookRemoveCommitOneCore(PPER_CPU_DATA Cpu);
BOOLEAN  HookRingEnqueue(PHOOK_RING Ring, ULONG64 Tag, ULONG64 Value, ULONG64 CpuIndex);
ULONG    HookRingDrain(PHOOK_RING Ring, HOOK_EVENT_CALLBACK Callback, PVOID Context);

// self-hook handler (src/vmx.asm): sets RAX=0xCAFEBABE and VMCALLs.
extern VOID MyHookHandlerVa(VOID);
// guest read probe (src/vmx.asm): reads [HvGuestEntryStub] into RAX, VMCALL.
extern VOID HvGuestReadProbe(VOID);
// guest CPUID probe (src/vmx.asm): executes three CPUIDs, stores results.
extern VOID HvGuestCpuidProbe(VOID);
