// hypervisor/src/hook.c
// Execute-only EPT hidden hook. Prepare/remove run at PASSIVE_LEVEL on
// the pinned owning core; commit/restore run in a KeIpiGenericCall at IPI_LEVEL.
// Nothing reachable from the VM-exit handler lives here: the read-emulation path
// in vmexit.c only swaps a cached PML1 leaf and appends to the lock-free ring.
#include "hook.h"
#include "vmcs.h"
#include "ept.h"
#include "persist.h"
#include "syscall.h"

#define HOOK_CONTEXT_TAG 'KkOH'

// Execute-only EPT translations: IA32_VMX_EPT_VPID_CAP bit 0 (SDM Vol. 3D A.10).
#define EPT_CAP_EXECUTE_ONLY (1ull << 0)

// Serializes the PASSIVE-side consumers (worker-thread drain and the explicit
// test drain) against HookRemoveAllCores, so a hook context is never freed
// while being drained. The VM-exit handler (the producer) never takes this lock.
static KSPIN_LOCK g_HookDrainLock;
static VOID HookRemoveLocked(ULONG64 TargetVirtualAddress);
static VOID HookRemoveLockedIndexed(ULONG64 TargetVirtualAddress, ULONG Index);
static VOID HookRemoveSyscallsLocked(VOID);

VOID HookInitialize(VOID)
{
    KeInitializeSpinLock(&g_HookDrainLock);
}

// Nonblocking EPT lock, mirroring ept.c EptTryLock/EptUnlock. Never spin at
// IPI_LEVEL: the interrupted thread could own EptLock itself (the guest IS us).
static BOOLEAN HookTryEptLock(PPER_CPU_DATA Cpu, KIRQL *PreviousIrql)
{
    *PreviousIrql = KeGetCurrentIrql();
    if (*PreviousIrql < DISPATCH_LEVEL) { KeRaiseIrql(DISPATCH_LEVEL, PreviousIrql); }
    if (KeTryToAcquireSpinLockAtDpcLevel(&Cpu->EptLock)) { return TRUE; }
    if (*PreviousIrql < DISPATCH_LEVEL) { KeLowerIrql(*PreviousIrql); }
    return FALSE;
}

static VOID HookUnlockEpt(PPER_CPU_DATA Cpu, KIRQL PreviousIrql)
{
    KeReleaseSpinLockFromDpcLevel(&Cpu->EptLock);
    if (PreviousIrql < DISPATCH_LEVEL) { KeLowerIrql(PreviousIrql); }
}

static VOID HookFreeShadowPage(PHOOK_CONTEXT Hook)
{
    if (Hook->PageOwner != NULL) { Hook->ShadowPageVa = NULL; return; }
    if (Hook->ShadowPageMdl != NULL) {
        if (Hook->ShadowPageVa != NULL) {
            MmUnmapLockedPages(Hook->ShadowPageVa, Hook->ShadowPageMdl);
        }
        MmFreePagesFromMdl(Hook->ShadowPageMdl);
        ExFreePool(Hook->ShadowPageMdl);
        Hook->ShadowPageMdl = NULL;
    } else if (Hook->ShadowPageVa != NULL) {
        MmFreeContiguousMemory(Hook->ShadowPageVa);
    }
    Hook->ShadowPageVa = NULL;
    Hook->ShadowPagePa.QuadPart = 0;
}

static NTSTATUS HookAllocateShadowPage(PHOOK_CONTEXT Hook)
{
    PHYSICAL_ADDRESS low = { 0 };
    PHYSICAL_ADDRESS high;
    PHYSICAL_ADDRESS boundary;
    high.QuadPart = MAXLONGLONG;
    boundary.QuadPart = PAGE_SIZE;
    Hook->ShadowPageVa = MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, low, high, boundary, MmCached);
    if (Hook->ShadowPageVa == NULL) {
        Hook->ShadowPageMdl = MmAllocatePagesForMdlEx(low, high, boundary,
            PAGE_SIZE, MmCached,
            MM_ALLOCATE_REQUIRE_CONTIGUOUS_CHUNKS | MM_ALLOCATE_FULLY_REQUIRED);
        if (Hook->ShadowPageMdl == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
        Hook->ShadowPageVa = MmMapLockedPagesSpecifyCache(Hook->ShadowPageMdl,
            KernelMode, MmCached, NULL, FALSE,
            (ULONG)NormalPagePriority | MdlMappingNoExecute);
        if (Hook->ShadowPageVa == NULL) {
            MmFreePagesFromMdl(Hook->ShadowPageMdl);
            ExFreePool(Hook->ShadowPageMdl);
            Hook->ShadowPageMdl = NULL;
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    Hook->ShadowPagePa = MmGetPhysicalAddress(Hook->ShadowPageVa);
    if ((((ULONG_PTR)Hook->ShadowPageVa | (ULONG64)Hook->ShadowPagePa.QuadPart) &
        (PAGE_SIZE - 1u)) != 0) {
        HookFreeShadowPage(Hook);
        return STATUS_DATATYPE_MISALIGNMENT;
    }
    return STATUS_SUCCESS;
}

// ---- lock-free ring -------------------------------------------------------

BOOLEAN HookRingEnqueue(PHOOK_RING Ring, ULONG64 Tag, ULONG64 Value, ULONG64 CpuIndex)
{
    LONG64 head = Ring->Head;   // producer owns Head
    LONG64 tail = Ring->Tail;   // consumer owns Tail
    if (head - tail >= HOOK_RING_ENTRIES) { return FALSE; }  // full: drop silently
    Ring->Entry[head & HOOK_RING_MASK].Tag = Tag;
    Ring->Entry[head & HOOK_RING_MASK].Value = Value;
    Ring->Entry[head & HOOK_RING_MASK].CpuIndex = CpuIndex;
    InterlockedIncrement64(&Ring->Head);   // publish (release)
    return TRUE;
}

ULONG HookRingDrain(PHOOK_RING Ring, HOOK_EVENT_CALLBACK Callback, PVOID Context)
{
    ULONG count = 0;
    LONG64 tail = Ring->Tail;
    LONG64 head = Ring->Head;   // snapshot; entries added later stay for next drain
    while (tail != head) {
        HOOK_RING_ENTRY *entry = &Ring->Entry[tail & HOOK_RING_MASK];
        Callback(entry->Tag, entry->Value, entry->CpuIndex, Context);
        ++tail;
        ++count;
    }
    InterlockedExchange64(&Ring->Tail, tail);   // publish (release)
    return count;
}

// ---- VA -> GPA translation (PASSIVE_LEVEL, 4-level 4KB paging) ------------

PVOID HookTranslateVaToPa(ULONG64 TargetVirtualAddress, PULONG64 OffsetOut)
{
    PHYSICAL_ADDRESS pa;
    ULONG64 *table;
    ULONG64 entry;
    ULONG i;

    if (OffsetOut == NULL) { return NULL; }
    *OffsetOut = TargetVirtualAddress & (PAGE_SIZE - 1ull);

    // SDM Vol. 3A 4.5 (4-level paging): PML4 -> PDPT -> PD -> PT. Windows maps
    // the kernel image (ntoskrnl .text, i.e. every Nt* entry) with 2MB large
    // pages, so the walker must also honor the page-size (PS) flag:
    //   - PDPT entry (PML3) PS=1 (bit 7) -> 1GB leaf,  phys = entry[51:30].
    //   - PD   entry (PML2) PS=1 (bit 7) -> 2MB leaf,  phys = entry[51:21].
    // For a leaf, the byte address is (entry & leaf_mask) + (VA & (leaf_size-1));
    // the low 12 bits are then masked so the caller gets the 4KB-aligned PA.
    pa.QuadPart = (LONGLONG)(__readcr3() & EPT_ADDRESS_MASK);
    table = (ULONG64 *)MmGetVirtualForPhysical(pa);
    if (table == NULL) { return NULL; }

    for (i = 0; i < 3; ++i) {
        ULONG shift = 39u - 9u * i;   // PML4=39, PDPT=30, PD=21
        entry = table[(TargetVirtualAddress >> shift) & EPT_INDEX_MASK];
        if ((entry & 1ull) == 0) { return NULL; }          // not present
        // The PS bit exists only at PDPT (1GB) and PD (2MB), never at PML4 (i==0).
        if (i != 0 && (entry & (1ull << 7)) != 0) {
            ULONG64 leafMask = (i == 1) ? 0x000FFFFFC0000000ull   // 1GB
                                        : 0x000FFFFFFFE00000ull;  // 2MB
            ULONG64 leafSize = (i == 1) ? 0x40000000ull : 0x200000ull;
            return (PVOID)(ULONG_PTR)(((entry & leafMask) +
                (TargetVirtualAddress & (leafSize - 1ull))) & ~(PAGE_SIZE - 1ull));
        }
        pa.QuadPart = (LONGLONG)(entry & EPT_ADDRESS_MASK);
        table = (ULONG64 *)MmGetVirtualForPhysical(pa);
        if (table == NULL) { return NULL; }
    }
    entry = table[(TargetVirtualAddress >> EPT_PML1_SHIFT) & EPT_INDEX_MASK];
    if ((entry & 1ull) == 0) { return NULL; }
    return (PVOID)(ULONG_PTR)(entry & EPT_ADDRESS_MASK);
}

// Decode supported entry instructions, including two-byte hotpatch PUSH forms.
// No relocation is required: the CPU executes this instruction at its original
// RIP under the original EPT mapping. Unknown forms fail before publication.
static ULONG HookReplayInstructionLength(const UCHAR *Code, ULONG Available)
{
    ULONG pos = 0, mod, rm, length;
    UCHAR opcode, modrm;
    if (Available < 2) { return 0; }
    if ((Code[pos] & 0xF0u) == 0x40u) { ++pos; }
    opcode = Code[pos++];
    if (opcode >= 0x50 && opcode <= 0x57) { return pos == 2 ? pos : 0; }
    if (opcode == 0xEB) { return pos + 1 <= Available ? pos + 1 : 0; }
    if (opcode == 0xE9 || opcode == 0xE8) { return pos + 4 <= Available ? pos + 4 : 0; }
    if (pos >= Available) { return 0; }
    if (opcode != 0x89 && opcode != 0x8B && opcode != 0x8D &&
        opcode != 0x81 && opcode != 0x83) { return 0; }
    modrm = Code[pos++];
    mod = modrm >> 6;
    rm = modrm & 7;
    // Arithmetic entry support is deliberately limited to SUB RSP, imm.
    if ((opcode == 0x81 || opcode == 0x83) &&
        (Code[0] != 0x48 || modrm != 0xEC)) { return 0; }
    if (opcode == 0x8D && mod == 3) { return 0; }
    length = pos;
    if (mod != 3) {
        if (rm == 4) {
            if (length >= Available) { return 0; }
            if (mod == 0 && (Code[length] & 7) == 5) { pos = 4; }
            else { pos = 0; }
            ++length;
            length += pos;
        } else if (mod == 0 && rm == 5) { length += 4; }
        if (mod == 1) { ++length; }
        if (mod == 2) { length += 4; }
    }
    if (opcode == 0x81) { length += 4; }
    if (opcode == 0x83) { ++length; }
    return length >= 2 && length <= 15 && length <= Available ? length : 0;
}

// Diagnostic hex dump (PASSIVE_LEVEL only). Dumps exactly 16 bytes.
static VOID HookDumpBytes(PCSTR Label, const UCHAR *Bytes)
{
    HV_INFO("HOOK: %s: %02X %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X %02X %02X %02X\n",
        Label,
        Bytes[0], Bytes[1], Bytes[2], Bytes[3], Bytes[4], Bytes[5], Bytes[6], Bytes[7],
        Bytes[8], Bytes[9], Bytes[10], Bytes[11], Bytes[12], Bytes[13], Bytes[14], Bytes[15]);
}

// ---- prepare / commit / remove -------------------------------------------

static NTSTATUS HookPrepareOneCoreIntoSlot(PPER_CPU_DATA Cpu, ULONG64 TargetGpa, ULONG64 FunctionOffset, PVOID HookHandlerVa, PHOOK_CONTEXT *Slot)
{
    PHYSICAL_ADDRESS pa;
    PVOID original;
    PHOOK_CONTEXT hook;
    PHOOK_CONTEXT owner = NULL;
    NTSTATUS status;
    ULONG64 pageBase = TargetGpa & ~(PAGE_SIZE - 1ull);
    ULONG patchSize;
    ULONG slotIndex;
    UCHAR *shadow;

    if (Cpu == NULL || Cpu->Ept == NULL || Slot == NULL || *Slot != NULL) { return STATUS_INVALID_PARAMETER; }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return STATUS_INVALID_PARAMETER; }
    if (FunctionOffset + (HookHandlerVa != NULL ? HOOK_JUMP_ABSOLUTE_SIZE : 2u) > PAGE_SIZE) { return STATUS_INVALID_PARAMETER; }

    // 1. Capability checks (SDM Vol. 3D A.10, Vol. 3C 24.6.2).
    if ((Cpu->Capabilities.EptVpidCap & (EPT_CAP_2MB | EPT_MEMORY_WB |
            EPT_INVEPT_SINGLE | EPT_CAP_EXECUTE_ONLY)) !=
        (EPT_CAP_2MB | EPT_MEMORY_WB | EPT_INVEPT_SINGLE | EPT_CAP_EXECUTE_ONLY)) {
        return STATUS_NOT_SUPPORTED;
    }
    if ((Cpu->Capabilities.Primary.Allowed1 & PRIMARY_MONITOR_TRAP_FLAG) == 0) {
        return STATUS_NOT_SUPPORTED;
    }

    // A syscall batch shares one shadow per GPA; legacy self-test ownership is exclusive.
    if (Cpu->Hook != NULL && Cpu->Hook->TargetGpa == pageBase) { return STATUS_CONFLICTING_ADDRESSES; }
    for (slotIndex = 0; slotIndex < SYSCALL_TARGET_COUNT; ++slotIndex) {
        if (Cpu->SyscallHooks[slotIndex] != NULL &&
            Cpu->SyscallHooks[slotIndex]->TargetGpa == pageBase) {
            owner = Cpu->SyscallHooks[slotIndex];
            if (owner->Installed || HookHandlerVa != NULL) { return STATUS_CONFLICTING_ADDRESSES; }
            if (owner->PageOwner != NULL) { owner = owner->PageOwner; }
            break;
        }
    }

    // 2. Read the original 4KB page through the direct map (identity-mapped RAM).
    pa.QuadPart = (LONGLONG)pageBase;
    original = MmGetVirtualForPhysical(pa);
    if (original == NULL) { return STATUS_INVALID_ADDRESS; }

    // 3. Allocate the shadow page (nonpaged, contiguous, 4KB-aligned, cached).
    hook = (PHOOK_CONTEXT)ExAllocatePool2(POOL_FLAG_NON_PAGED | POOL_FLAG_CACHE_ALIGNED,
        sizeof(HOOK_CONTEXT), HOOK_CONTEXT_TAG);
    if (hook == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(hook, sizeof(*hook));
    hook->PageOwner = owner;
    if (owner != NULL) {
        // All targets are prepared before publication, so this shared copy is
        // still unreachable by guest execution. The owner is freed last.
        hook->ShadowPageVa = owner->ShadowPageVa;
        hook->ShadowPagePa = owner->ShadowPagePa;
    } else {
        status = HookAllocateShadowPage(hook);
        if (!NT_SUCCESS(status)) { ExFreePoolWithTag(hook, HOOK_CONTEXT_TAG); return status; }
        RtlCopyMemory(hook->ShadowPageVa, original, PAGE_SIZE);
    }
    shadow = (UCHAR *)hook->ShadowPageVa;

    if (HookHandlerVa == NULL) {
        // Preserve a CET ENDBR64 landing pad, if present.
        if (FunctionOffset + 4 <= PAGE_SIZE &&
            RtlCompareMemory(shadow + FunctionOffset, "\xF3\x0F\x1E\xFA", 4) == 4) {
            FunctionOffset += 4;
        }
        patchSize = HookReplayInstructionLength(shadow + FunctionOffset,
            (ULONG)(PAGE_SIZE - FunctionOffset));
        if (patchSize == 0) {
            HV_ERROR("HOOK: unsupported entry at GPA=0x%I64X offset=0x%I64X\n",
                pageBase, FunctionOffset);
            if (FunctionOffset + 16 <= PAGE_SIZE) {
                HookDumpBytes("rejected original entry", (const UCHAR *)original + FunctionOffset);
                HookDumpBytes("rejected shadow entry", shadow + FunctionOffset);
            }
            HookFreeShadowPage(hook);
            ExFreePoolWithTag(hook, HOOK_CONTEXT_TAG);
            return STATUS_NOT_SUPPORTED;
        }
        // VMCALL or UD2 traps before changing guest state. Never overwrite
        // beyond the first whole instruction; keep all continuation bytes intact.
        shadow[FunctionOffset] = 0x0F;
        if (patchSize == 2) {
            shadow[FunctionOffset + 1] = 0x0B; // UD2: preserves a two-byte entry instruction.
        } else {
            shadow[FunctionOffset + 1] = 0x01;
            shadow[FunctionOffset + 2] = 0xC1;
        }
    } else {
        // One-shot self-test detour never returns to the patched entry.
        patchSize = HOOK_JUMP_ABSOLUTE_SIZE;
        shadow[FunctionOffset] = 0x48;
        shadow[FunctionOffset + 1] = 0xB8;
        *(ULONG64 *)(shadow + FunctionOffset + 2) = (ULONG64)(ULONG_PTR)HookHandlerVa;
        shadow[FunctionOffset + 10] = 0xFF;
        shadow[FunctionOffset + 11] = 0xE0;
    }
    if (Cpu == g_PerCpuData[0] && FunctionOffset + 16 <= PAGE_SIZE) {
        HookDumpBytes("original entry", (const UCHAR *)original + FunctionOffset);
        HookDumpBytes("shadow entry", shadow + FunctionOffset);
    }

    // Record the context; ShadowPml1Entry is built at commit (needs the leaf type).
    hook->TargetGpa = pageBase;
    hook->TargetFunctionOffset = FunctionOffset;
    hook->HookStubVa = shadow + FunctionOffset;
    hook->ActualPatchSize = patchSize;
    hook->Installed = 0;
    hook->ReadMtfArmed = 0;
    hook->CommitStatus = STATUS_SUCCESS;

    // Prepare the 2MB split (PASSIVE); the commit IPI publishes it.
    status = EptPrepareSplit(Cpu, pageBase);
    if (!NT_SUCCESS(status)) {
        HookFreeShadowPage(hook);
        ExFreePoolWithTag(hook, HOOK_CONTEXT_TAG);
        return status;
    }

    *Slot = hook;
    Cpu->PendingHook = hook;
    return STATUS_SUCCESS;
}

NTSTATUS HookPrepareOneCore(PPER_CPU_DATA Cpu, ULONG64 TargetGpa, ULONG64 FunctionOffset, PVOID HookHandlerVa)
{
    return HookPrepareOneCoreIntoSlot(Cpu, TargetGpa, FunctionOffset, HookHandlerVa, &Cpu->Hook);
}

static NTSTATUS HookPrepareOneCoreIndexed(PPER_CPU_DATA Cpu, ULONG64 TargetGpa, ULONG64 FunctionOffset, ULONG Index)
{
    if (Index >= SYSCALL_TARGET_COUNT) { return STATUS_INVALID_PARAMETER; }
    return HookPrepareOneCoreIntoSlot(Cpu, TargetGpa, FunctionOffset, NULL, &Cpu->SyscallHooks[Index]);
}

NTSTATUS HookCommitOneCore(PPER_CPU_DATA Cpu)
{
    PHOOK_CONTEXT hook = Cpu->PendingHook;
    KIRQL irql;
    ULONG64 *entry;
    NTSTATUS status;

    if (hook == NULL) { return STATUS_INVALID_PARAMETER; }

    if (hook->PageOwner != NULL) {
        PHOOK_CONTEXT owner = hook->PageOwner;
        if (!owner->Installed) { hook->CommitStatus = STATUS_INVALID_DEVICE_STATE; return hook->CommitStatus; }
        hook->Pml1Entry = owner->Pml1Entry;
        hook->OriginalPml1Entry = owner->OriginalPml1Entry;
        hook->ShadowPml1Entry = owner->ShadowPml1Entry;
        hook->Installed = 1;
        hook->CommitStatus = owner->CommitStatus;
        return hook->CommitStatus;
    }
    // 1. Publish the split (self-locking; INVEPT inside). Prepare ran at PASSIVE.
    status = EptSplitLargePage(Cpu, hook->TargetGpa);
    if (!NT_SUCCESS(status)) { hook->CommitStatus = status; return status; }

    // 2. Nonblocking lock to modify the leaf. On contention, give up.
    if (!HookTryEptLock(Cpu, &irql)) {
        hook->CommitStatus = STATUS_DEVICE_BUSY;
        return STATUS_DEVICE_BUSY;
    }

    // 3. Locate the PML1 leaf (caller holds the lock; owner; no pending INVEPT).
    entry = EptGetPml1Entry(Cpu, hook->TargetGpa);
    if (entry == NULL) {
        HookUnlockEpt(Cpu, irql);
        hook->CommitStatus = STATUS_INVALID_DEVICE_STATE;
        return STATUS_INVALID_DEVICE_STATE;
    }
    hook->Pml1Entry = entry;

    // 4. Save the original leaf; build the execute-only shadow leaf.
    //    SDM 29.3.2: preserve memory type (5:3) and ignore-PAT (6); R/W/X=100b.
    hook->OriginalPml1Entry = *entry;
    hook->ShadowPml1Entry = ((ULONG64)hook->ShadowPagePa.QuadPart & EPT_ADDRESS_MASK) |
        EPT_ENTRY_EXECUTE |
        (hook->OriginalPml1Entry & (EPT_ENTRY_TYPE_MASK | EPT_ENTRY_IGNORE_PAT));

    hook->Installed = 1; // Retain ownership even if invalidation fails.
    // 5. Atomically publish the execute-only mapping.
    (VOID)InterlockedExchange64((volatile LONG64 *)entry, (LONG64)hook->ShadowPml1Entry);

    // 6. Invalidate this EPT context (targeted single-context INVEPT).
    status = (EptInvalidateCurrent(Cpu) == 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;

    HookUnlockEpt(Cpu, irql);
    hook->CommitStatus = status;
    return status;
}

NTSTATUS HookRemoveCommitOneCore(PPER_CPU_DATA Cpu)
{
    PHOOK_CONTEXT hook = Cpu->PendingHook;
    KIRQL irql;
    if (hook == NULL || !hook->Installed) { return STATUS_SUCCESS; }
    if (!HookTryEptLock(Cpu, &irql)) { return STATUS_DEVICE_BUSY; }
    (VOID)InterlockedExchange64((volatile LONG64 *)hook->Pml1Entry,
        (LONG64)hook->OriginalPml1Entry);
    if (EptInvalidateCurrent(Cpu) != 0) {
        HookUnlockEpt(Cpu, irql);
        return STATUS_UNSUCCESSFUL;
    }
    if (Cpu->ReadMtfHook == hook) {
        ULONG64 primary = 0;
        if (VmxRead(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS, &primary) != 0 ||
            VmxWrite(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS,
                primary & ~(ULONG64)PRIMARY_MONITOR_TRAP_FLAG) != 0) {
            HookUnlockEpt(Cpu, irql);
            return STATUS_UNSUCCESSFUL;
        }
        Cpu->ReadMtfHook = NULL;
        hook->ReadMtfArmed = 0;
    }
    hook->Installed = 0;
    HookUnlockEpt(Cpu, irql);
    return STATUS_SUCCESS;
}

// ---- IPI callbacks --------------------------------------------------------

static ULONG_PTR HookCommitIpiCallback(ULONG_PTR Context)
{
    ULONG index = KeGetCurrentProcessorNumber();
    UNREFERENCED_PARAMETER(Context);
    if (index < g_CpuCount && g_PerCpuData[index] != NULL) {
        PPER_CPU_DATA cpu = g_PerCpuData[index];
        if (cpu->Persist.Launched) { (VOID)HvHookControl(HV_CONTROL_HOOK_COMMIT, 0); }
        else { (VOID)HookCommitOneCore(cpu); }
    }
    return 0;
}

static ULONG_PTR HookRemoveIpiCallback(ULONG_PTR Context)
{
    ULONG index = KeGetCurrentProcessorNumber();
    UNREFERENCED_PARAMETER(Context);
    if (index < g_CpuCount && g_PerCpuData[index] != NULL) {
        PPER_CPU_DATA cpu = g_PerCpuData[index];
        if (cpu->Persist.Launched) { (VOID)HvHookControl(HV_CONTROL_HOOK_REMOVE, 0); }
        else if (cpu->VmxEnabled) { (VOID)HookRemoveCommitOneCore(cpu); }
        else if (cpu->PendingHook != NULL) { cpu->PendingHook->Installed = 0; }
    }
    return 0;
}

// ---- public API -----------------------------------------------------------

static NTSTATUS HookInstallLocked(ULONG64 TargetVirtualAddress, PVOID HookHandlerVa)
{
    ULONG64 gpa;
    ULONG64 offset;
    NTSTATUS status;
    ULONG i;

    if (TargetVirtualAddress == 0 || HookHandlerVa == NULL) { return STATUS_INVALID_PARAMETER; }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return STATUS_INVALID_PARAMETER; }

    // Translate once: the GPA is identical on every core (identity-mapped RAM).
    gpa = (ULONG64)(ULONG_PTR)HookTranslateVaToPa(TargetVirtualAddress, &offset);
    if (gpa == 0) { return STATUS_INVALID_ADDRESS; }
    gpa &= ~(PAGE_SIZE - 1ull);

    // Phase 1: prepare each core (PASSIVE, pinned).
    for (i = 0; i < g_CpuCount; ++i) {
        PPER_CPU_DATA cpu = g_PerCpuData[i];
        GROUP_AFFINITY target = { 0 };
        GROUP_AFFINITY previous;
        target.Mask = (KAFFINITY)1 << i;
        KeSetSystemGroupAffinityThread(&target, &previous);
        status = HookPrepareOneCore(cpu, gpa, offset, HookHandlerVa);
        KeRevertToUserGroupAffinityThread(&previous);
        if (!NT_SUCCESS(status)) { HookRemoveLocked(TargetVirtualAddress); return status; }
    }

    // Phase 2: commit all cores via one IPI broadcast.
    (VOID)KeIpiGenericCall(HookCommitIpiCallback, 0);
    for (i = 0; i < g_CpuCount; ++i) {
        PPER_CPU_DATA cpu = g_PerCpuData[i];
        if (cpu != NULL && cpu->Hook != NULL && !NT_SUCCESS(cpu->Hook->CommitStatus)) {
            status = cpu->Hook->CommitStatus;
            HookRemoveLocked(TargetVirtualAddress);
            return status;
        }
    }
    return STATUS_SUCCESS;
}

static VOID HookRemoveLocked(ULONG64 TargetVirtualAddress)
{
    PHOOK_CONTEXT hooks[MAXIMUM_PROCESSORS];
    ULONG coreIndex[MAXIMUM_PROCESSORS];
    ULONG count = 0;
    ULONG i;
    KIRQL irql;
    UNREFERENCED_PARAMETER(TargetVirtualAddress);

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return; }

    for (i = 0; i < g_CpuCount; ++i) {
        if (g_PerCpuData[i] != NULL) { g_PerCpuData[i]->PendingHook = g_PerCpuData[i]->Hook; }
    }
    // Commit phase: restore the original PML1 leaf + INVEPT on each owner.
    (VOID)KeIpiGenericCall(HookRemoveIpiCallback, 0);
    for (i = 0; i < g_CpuCount; ++i) {
        if (g_PerCpuData[i] != NULL && g_PerCpuData[i]->Hook != NULL &&
            g_PerCpuData[i]->Hook->Installed) {
            HV_ERROR("HOOK: removal incomplete; retaining contexts and shadow pages\n");
            return;
        }
    }

    // Detach every context under the drain lock so a concurrent drain cannot
    // reference a context we are about to free.
    KeAcquireSpinLock(&g_HookDrainLock, &irql);
    for (i = 0; i < g_CpuCount; ++i) {
        PPER_CPU_DATA cpu = g_PerCpuData[i];
        if (cpu != NULL && cpu->Hook != NULL) {
            hooks[count] = cpu->Hook;
            coreIndex[count] = i;
            cpu->Hook = NULL;
            if (cpu->PendingHook == hooks[count]) { cpu->PendingHook = NULL; }
            if (cpu->ReadMtfHook == hooks[count]) { cpu->ReadMtfHook = NULL; }
            ++count;
        }
    }
    KeReleaseSpinLock(&g_HookDrainLock, irql);

    // PASSIVE: restore the large page, reclaim, free shadow + context.
    for (i = 0; i < count; ++i) {
        PHOOK_CONTEXT hook = hooks[i];
        GROUP_AFFINITY target = { 0 };
        GROUP_AFFINITY previous;
        target.Mask = (KAFFINITY)1 << coreIndex[i];
        KeSetSystemGroupAffinityThread(&target, &previous);
        if (g_PerCpuData[coreIndex[i]]->Persist.Launched) {
            (VOID)HvHookControl(HV_CONTROL_EPT_RESTORE, hook->TargetGpa);
        } else if (g_PerCpuData[coreIndex[i]]->VmxEnabled) {
            (VOID)EptRestoreLargePage(g_PerCpuData[coreIndex[i]], hook->TargetGpa);
        }
        (VOID)EptReclaimRetired(g_PerCpuData[coreIndex[i]]);
        HookFreeShadowPage(hook);
        ExFreePoolWithTag(hook, HOOK_CONTEXT_TAG);
        KeRevertToUserGroupAffinityThread(&previous);
    }
}

// Per-slot install/remove; caller holds the lifecycle lock.

static VOID HookRemoveLockedIndexed(ULONG64 TargetVirtualAddress, ULONG Index)
{
    PHOOK_CONTEXT hooks[MAXIMUM_PROCESSORS];
    ULONG coreIndex[MAXIMUM_PROCESSORS];
    ULONG count = 0;
    ULONG i;
    KIRQL irql;
    UNREFERENCED_PARAMETER(TargetVirtualAddress);

    if (Index >= SYSCALL_TARGET_COUNT) { return; }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return; }

    // Point PendingHook at the slot before the remove IPI so the per-core
    // callback removes the right context (mirrors HookRemoveLocked).
    for (i = 0; i < g_CpuCount; ++i) {
        if (g_PerCpuData[i] != NULL) {
            g_PerCpuData[i]->PendingHook = g_PerCpuData[i]->SyscallHooks[Index];
        }
    }

    (VOID)KeIpiGenericCall(HookRemoveIpiCallback, 0);
    for (i = 0; i < g_CpuCount; ++i) {
        if (g_PerCpuData[i] != NULL && g_PerCpuData[i]->SyscallHooks[Index] != NULL &&
            g_PerCpuData[i]->SyscallHooks[Index]->Installed) {
            HV_ERROR("HOOK: removal incomplete; retaining contexts and shadow pages\n");
            return;
        }
    }

    // Detach every context under the drain lock so a concurrent drain cannot
    // reference a context we are about to free.
    KeAcquireSpinLock(&g_HookDrainLock, &irql);
    for (i = 0; i < g_CpuCount; ++i) {
        PPER_CPU_DATA cpu = g_PerCpuData[i];
        if (cpu != NULL && cpu->SyscallHooks[Index] != NULL) {
            hooks[count] = cpu->SyscallHooks[Index];
            coreIndex[count] = i;
            cpu->SyscallHooks[Index] = NULL;
            if (cpu->PendingHook == hooks[count]) { cpu->PendingHook = NULL; }
            if (cpu->ReadMtfHook == hooks[count]) { cpu->ReadMtfHook = NULL; }
            ++count;
        }
    }
    KeReleaseSpinLock(&g_HookDrainLock, irql);

    for (i = 0; i < count; ++i) {
        PHOOK_CONTEXT hook = hooks[i];
        GROUP_AFFINITY target = { 0 };
        GROUP_AFFINITY previous;
        target.Mask = (KAFFINITY)1 << coreIndex[i];
        KeSetSystemGroupAffinityThread(&target, &previous);
        if (g_PerCpuData[coreIndex[i]]->Persist.Launched) {
            (VOID)HvHookControl(HV_CONTROL_EPT_RESTORE, hook->TargetGpa);
        } else if (g_PerCpuData[coreIndex[i]]->VmxEnabled) {
            (VOID)EptRestoreLargePage(g_PerCpuData[coreIndex[i]], hook->TargetGpa);
        }
        (VOID)EptReclaimRetired(g_PerCpuData[coreIndex[i]]);
        HookFreeShadowPage(hook);
        ExFreePoolWithTag(hook, HOOK_CONTEXT_TAG);
        KeRevertToUserGroupAffinityThread(&previous);
    }
}

static VOID HookDrainEvent(ULONG64 Tag, ULONG64 Value, ULONG64 CpuIndex, PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    switch (Tag) {
    case HOOK_EVENT_FIRED:
    case HOOK_EVENT_NOT_FIRED:
    case HOOK_EVENT_CPUID:
        // Startup probes validate their results directly; no per-event output.
        break;
    case HOOK_EVENT_EPT_EXECUTE:
        HV_ERROR("HOOK: core %I64u unexpected execute EPT violation (qualification=0x%I64X)\n",
            CpuIndex, Value);
        break;
    case HOOK_EVENT_ERROR:
        HV_ERROR("HOOK: core %I64u error event (value=0x%I64X)\n", CpuIndex, Value);
        break;
    default:
        HV_INFO("HOOK: core %I64u event tag=0x%I64X value=0x%I64X\n", CpuIndex, Tag, Value);
        break;
    }
}

// Copies up to Max entries out of one ring (advancing Tail). Caller must hold
// g_HookDrainLock; the producer still enqueues lock-free.
static ULONG HookRingPopCopy(PHOOK_RING Ring, HOOK_RING_ENTRY *Out, ULONG Max)
{
    ULONG count = 0;
    LONG64 tail = Ring->Tail;
    LONG64 head = Ring->Head;
    while (tail != head && count < Max) {
        Out[count++] = Ring->Entry[tail & HOOK_RING_MASK];
        ++tail;
    }
    InterlockedExchange64(&Ring->Tail, tail);
    return count;
}

VOID HookDrainAllCores(VOID)
{
    HOOK_RING_ENTRY batch[HOOK_RING_ENTRIES];
    ULONG total = 0;
    ULONG i;
    KIRQL irql;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return; }

    // Snapshot entries under the lock so HookRemoveAllCores cannot free a
    // context we are about to read; log at PASSIVE after releasing it.
    KeAcquireSpinLock(&g_HookDrainLock, &irql);
    for (i = 0; i < g_CpuCount && total < HOOK_RING_ENTRIES; ++i) {
        PPER_CPU_DATA cpu = g_PerCpuData[i];
        PHOOK_CONTEXT hook = (cpu != NULL) ? cpu->Hook : NULL;
        if (hook != NULL) {
            total += HookRingPopCopy(&hook->Ring, &batch[total],
                HOOK_RING_ENTRIES - total);
        }
    }
    KeReleaseSpinLock(&g_HookDrainLock, irql);

    for (i = 0; i < total; ++i) {
        HookDrainEvent(batch[i].Tag, batch[i].Value, batch[i].CpuIndex, NULL);
    }
}

NTSTATUS HookInstallAllCores(ULONG64 TargetVirtualAddress, PVOID HookHandlerVa)
{
    NTSTATUS status;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return STATUS_INVALID_DEVICE_STATE; }
    PersistAcquire();
    if (g_PersistActive != HV_STATE_STARTING && g_PersistActive != HV_STATE_RUNNING) {
        status = STATUS_INVALID_DEVICE_STATE;
    } else { status = HookInstallLocked(TargetVirtualAddress, HookHandlerVa); }
    PersistRelease();
    return status;
}

VOID HookRemoveAllCores(ULONG64 TargetVirtualAddress)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return; }
    PersistAcquire();
    HookRemoveLocked(TargetVirtualAddress);
    PersistRelease();
}

// Commit/remove each CPU's complete batch at IPI_LEVEL. No guest thread can
// enter a partially published shared shadow on that CPU between slot updates.
static ULONG_PTR HookSyscallsIpi(ULONG_PTR Remove)
{
    ULONG core = KeGetCurrentProcessorNumber(), step;
    PPER_CPU_DATA cpu = core < g_CpuCount ? g_PerCpuData[core] : NULL;
    if (cpu == NULL) { return 0; }
    for (step = 0; step < SYSCALL_TARGET_COUNT; ++step) {
        ULONG index = Remove ? SYSCALL_TARGET_COUNT - 1 - step : step;
        cpu->PendingHook = cpu->SyscallHooks[index];
        if (cpu->PendingHook == NULL) { continue; }
        if (cpu->Persist.Launched) {
            (VOID)HvHookControl(Remove ? HV_CONTROL_HOOK_REMOVE : HV_CONTROL_HOOK_COMMIT, 0);
        } else if (cpu->VmxEnabled) {
            if (Remove) { (VOID)HookRemoveCommitOneCore(cpu); }
            else { (VOID)HookCommitOneCore(cpu); }
        } else if (Remove) { cpu->PendingHook->Installed = 0; }
    }
    return 0;
}

static VOID HookRemoveSyscallsLocked(VOID)
{
    ULONG core, index;
    (VOID)KeIpiGenericCall(HookSyscallsIpi, 1);
    for (core = 0; core < g_CpuCount; ++core) {
        for (index = 0; index < SYSCALL_TARGET_COUNT; ++index) {
            PHOOK_CONTEXT hook = g_PerCpuData[core]->SyscallHooks[index];
            if (hook != NULL && hook->Installed) {
                HV_ERROR("HOOK: retaining whole syscall batch after removal failure\n");
                return;
            }
        }
    }
    // No page is hardware-owned now. Reverse order frees borrowers before owners.
    for (index = SYSCALL_TARGET_COUNT; index != 0; --index) {
        HookRemoveLockedIndexed(0, index - 1);
    }
}

NTSTATUS HookInstallSyscallsAllCores(const ULONG64 *Addresses)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG core, index;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || Addresses == NULL) { return STATUS_INVALID_PARAMETER; }
    PersistAcquire();
    if (g_PersistActive != HV_STATE_RUNNING) { status = STATUS_INVALID_DEVICE_STATE; goto Done; }
    // Reject duplicate identities before allocating or patching any shadow.
    // Runtime availability normally selects one representative per address.
    for (index = 0; index < SYSCALL_TARGET_COUNT; ++index) {
        ULONG prior;
        if (Addresses[index] == 0) { continue; }
        for (prior = 0; prior < index; ++prior) {
            if (Addresses[prior] != Addresses[index]) { continue; }
            HV_ERROR("HOOK: %s and %s share entry %p; select one representative\n",
                g_SyscallTargetMeta[prior].Name, g_SyscallTargetMeta[index].Name,
                (PVOID)(ULONG_PTR)Addresses[index]);
            status = STATUS_CONFLICTING_ADDRESSES;
            goto Done;
        }
    }
    for (index = 0; index < SYSCALL_TARGET_COUNT; ++index) {
        ULONG64 offset = 0;
        ULONG64 gpa;
        if (Addresses[index] == 0) { continue; }
        gpa = (ULONG64)(ULONG_PTR)HookTranslateVaToPa(Addresses[index], &offset);
        if (gpa == 0) { status = STATUS_INVALID_ADDRESS; goto Failed; }
        for (core = 0; core < g_CpuCount; ++core) {
            GROUP_AFFINITY target = { 0 }, previous;
            target.Mask = (KAFFINITY)1 << core;
            KeSetSystemGroupAffinityThread(&target, &previous);
            status = HookPrepareOneCoreIndexed(g_PerCpuData[core], gpa, offset, index);
            KeRevertToUserGroupAffinityThread(&previous);
            if (!NT_SUCCESS(status)) {
                HV_ERROR("HOOK: %s idx=%lu core=%lu VA=0x%I64X prepare failed status=0x%08lX\n",
                    g_SyscallTargetMeta[index].Name, index, core, Addresses[index], (ULONG)status);
                goto Failed;
            }
        }
    }
    (VOID)KeIpiGenericCall(HookSyscallsIpi, 0);
    for (core = 0; core < g_CpuCount; ++core) {
        for (index = 0; index < SYSCALL_TARGET_COUNT; ++index) {
            if (g_PerCpuData[core]->SyscallHooks[index] == NULL) { continue; }
            status = g_PerCpuData[core]->SyscallHooks[index]->CommitStatus;
            if (!NT_SUCCESS(status)) {
                HV_ERROR("HOOK: %s idx=%lu core=%lu commit failed status=0x%08lX\n",
                    g_SyscallTargetMeta[index].Name, index, core, (ULONG)status);
                goto Done; // Caller drains guest helpers before removal.
            }
        }
    }
    goto Done;
Failed:
    // Prepare failed before publishing any mapping: no guest helper can exist.
    HookRemoveSyscallsLocked();
Done:
    PersistRelease();
    return status;
}

VOID HookRemoveSyscallsAllCores(VOID)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return; }
    PersistAcquire();
    HookRemoveSyscallsLocked();
    PersistRelease();
}
