// hypervisor/src/vmexit.c
// IF=0 on a private nonpaged stack. Only VMREAD/VMWRITE and resident data
// accesses here; defer logging, VMCLEAR and diagnostics until the original
// Windows state returns. The EPT read-emulation path swaps a cached PML1 leaf
// (no lock: this handler is the only writer on this core) and arms MTF.
#include "common.h"
#include "vmcs.h"
#include "ept.h"
#include "hook.h"
#include "persist.h"
#include "syscall.h"
#include "filter.h"

// Byte offset of PER_CPU_DATA.LaunchCallerRsp, read by HvHostEntryStub's
// VMRESUME failure fallback (src/vmx.asm). A runtime lookup avoids duplicating
// the struct layout in assembly.
const ULONG g_LaunchCallerRspOffset =
    (ULONG)FIELD_OFFSET(PER_CPU_DATA, LaunchCallerRsp);

// Byte offset of PER_CPU_DATA.GuestRegs, read by HvHostEntryStub's GPR
// save/restore (src/vmx.asm). Runtime lookup avoids duplicating struct layout.
const ULONG g_GuestRegsOffset =
    (ULONG)FIELD_OFFSET(PER_CPU_DATA, GuestRegs);

// Byte offset of PER_CPU_DATA.Persist for the assembly shutdown path.
const ULONG g_PersistOffset =
    (ULONG)FIELD_OFFSET(PER_CPU_DATA, Persist);

static void HookEnqueue(PPER_CPU_DATA Cpu, ULONG64 Tag, ULONG64 Value)
{
    if (Cpu->Hook != NULL) {
        // KeGetCurrentProcessorNumber is safe at any IRQL (reads the KPCR; no
        // blocking, no pageable access) and returns the producer's own core.
        (VOID)HookRingEnqueue(&Cpu->Hook->Ring, Tag, Value,
            (ULONG64)KeGetCurrentProcessorNumber());
    }
}

// Shared "cannot continue the guest" path: record a failure and exit to the
// launcher. Never logs, allocates, blocks, or takes a blocking lock.
static ULONG64 VmExitFail(PPER_CPU_DATA Cpu)
{
    if (Cpu->Persist.Launched) { PersistFatal(Cpu, Cpu->ExitReason); }
    Cpu->LaunchResult = 3u;
    *(ULONG64 *)(ULONG_PTR)Cpu->LaunchCallerRsp = Cpu->LaunchResult;
    return Cpu->LaunchCallerRsp;
}

// Find the installed hook whose shadow page maps the faulting GPA (identity EPT:
// GPA == the original 4KB page base). Searches the self-test hook and the
// syscall hook slots. Returns NULL when no hook owns the page.
static PHOOK_CONTEXT VmExitFindHookByGpa(PPER_CPU_DATA Cpu, ULONG64 Gpa)
{
    ULONG64 page = Gpa & ~(PAGE_SIZE - 1ull);
    ULONG i;
    if (Cpu->Hook != NULL && Cpu->Hook->TargetGpa == page) { return Cpu->Hook; }
    for (i = 0; i < SYSCALL_TARGET_COUNT; ++i) {
        PHOOK_CONTEXT h = Cpu->SyscallHooks[i];
        if (h != NULL && h->TargetGpa == page) { return h; }
    }
    return NULL;
}

// Shared by syscall entry replay and EPT data access. Hardware executes the
// original instruction; no synthetic stack, register changes or relocated code.
static ULONG64 VmExitReplayOriginal(PPER_CPU_DATA Cpu, PHOOK_CONTEXT hook)
{
    ULONG64 primary = 0;
    // Publish the original page and invalidate cached execute translations.
    (VOID)InterlockedExchange64((volatile LONG64 *)hook->Pml1Entry,
        (LONG64)hook->OriginalPml1Entry);
    if (EptInvalidateCurrent(Cpu) != 0) { return VmExitFail(Cpu); }

    // Arm MTF so we regain control after one guest instruction.
    if (VmxRead(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS, &primary) != 0) {
        return VmExitFail(Cpu);
    }
    if (VmxWrite(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS,
        primary | (ULONG64)PRIMARY_MONITOR_TRAP_FLAG) != 0) {
        return VmExitFail(Cpu);
    }
    Cpu->ReadMtfHook = hook;
    hook->ReadMtfArmed = 1;
    return VMEXIT_ACTION_RESUME;
}

// EPT violation (SDM Vol. 3C 27.2.1): data access => read emulation via MTF;
// instruction fetch => unexpected (our shadow mapping is X=1).
static ULONG64 VmExitHandleEptViolation(PPER_CPU_DATA Cpu)
{
    ULONG64 qualification = 0;
    ULONG64 gpa = 0;
    PHOOK_CONTEXT hook;

    if (VmxRead(VMCS_EXIT_QUALIFICATION, &qualification) != 0) { return VmExitFail(Cpu); }
    if (Cpu->Persist.Launched && (qualification & 0x38ull) == 0) {
        if (VmxRead(VMCS_GUEST_PHYSICAL_ADDRESS, &gpa) != 0 ||
            !NT_SUCCESS(EptMapRuntimeHole(Cpu, gpa))) { return VmExitFail(Cpu); }
        return VMEXIT_ACTION_RESUME;
    }
    if ((qualification & 0x4ull) != 0) {                 // bit 2: instruction fetch
        HookEnqueue(Cpu, HOOK_EVENT_EPT_EXECUTE, qualification);
        return VmExitFail(Cpu);
    }
    if ((qualification & 0x3ull) == 0) { return VmExitFail(Cpu); }  // not data read/write
    if (VmxRead(VMCS_GUEST_PHYSICAL_ADDRESS, &gpa) != 0) { return VmExitFail(Cpu); }
    hook = VmExitFindHookByGpa(Cpu, gpa);
    if (hook == NULL || !hook->Installed) {
        HookEnqueue(Cpu, HOOK_EVENT_ERROR, gpa);
        return VmExitFail(Cpu);
    }

    return VmExitReplayOriginal(Cpu, hook);
}

// Monitor trap flag (SDM Vol. 3C 25.5.2): one guest instruction executed under
// the original mapping; restore the shadow mapping and invalidate.
static ULONG64 VmExitHandleMtf(PPER_CPU_DATA Cpu)
{
    ULONG64 primary = 0;
    PHOOK_CONTEXT hook = Cpu->ReadMtfHook;

    if (hook == NULL || !hook->ReadMtfArmed) { return VmExitFail(Cpu); }

    if (VmxRead(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS, &primary) != 0) {
        return VmExitFail(Cpu);
    }
    if (VmxWrite(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS,
        primary & ~(ULONG64)PRIMARY_MONITOR_TRAP_FLAG) != 0) {
        return VmExitFail(Cpu);
    }

    (VOID)InterlockedExchange64((volatile LONG64 *)hook->Pml1Entry,
        (LONG64)hook->ShadowPml1Entry);
    if (EptInvalidateCurrent(Cpu) != 0) { return VmExitFail(Cpu); }
    hook->ReadMtfArmed = 0;
    Cpu->ReadMtfHook = NULL;
    return VMEXIT_ACTION_RESUME;
}

// An intervening exit may arrive before replay completes. Restore the shadow
// before dispatching that event; a retry at the original RIP traps again.
// One-shot self-tests retain their original failure contract.
static ULONG64 VmExitHandleDeferredMtf(PPER_CPU_DATA Cpu, ULONG64 Reason)
{
    PHOOK_CONTEXT hook = Cpu->ReadMtfHook;
    ULONG64 primary = 0;

    if (VmxRead(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS, &primary) != 0 ||
        VmxWrite(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS,
            primary & ~(ULONG64)PRIMARY_MONITOR_TRAP_FLAG) != 0) { return VmExitFail(Cpu); }
    (VOID)InterlockedExchange64((volatile LONG64 *)hook->Pml1Entry,
        (LONG64)hook->ShadowPml1Entry);
    if (EptInvalidateCurrent(Cpu) != 0) { return VmExitFail(Cpu); }
    hook->ReadMtfArmed = 0;
    Cpu->ReadMtfHook = NULL;

    HookEnqueue(Cpu, HOOK_EVENT_ERROR, Reason);
    return Cpu->Persist.Launched ? VMEXIT_ACTION_RESUME : VmExitFail(Cpu);
}

// CPUID (SDM Vol. 3C 25.1.2, exit reason 10): execute the guest's requested
// leaf/subleaf in the root, optionally spoof the result, advance GUEST_RIP, and
// resume. The guest GPRs were saved to Cpu->GuestRegs by the exit stub; write the
// (possibly modified) EAX/EBX/ECX/EDX back there so VMRESUME restores them.
static ULONG64 VmExitHandleCpuid(PPER_CPU_DATA Cpu, ULONG64 GuestRax)
{
    int leaf = (int)(GuestRax & 0xFFFFFFFFull);
    int subleaf = (int)(Cpu->GuestRegs.Rcx & 0xFFFFFFFFull);
    int regs[4];
    ULONG64 rip = 0;
    ULONG64 length = 0;

    if (Cpu->Persist.Launched && (ULONG)leaf == HV_CONTROL_LEAF) {
        return PersistHandleControl(Cpu);
    }
    __cpuidex(regs, leaf, subleaf);
    // SDM Vol. 2A CPUID: 15h/16h intentionally pass through, including unknowns.

    if (leaf == 1) {
        if (Cpu->Persist.Launched) { regs[2] &= ~(1u << 5); } // no nested VMX
        regs[2] &= 0x7FFFFFFF;                    // clear ECX bit 31 (hypervisor present)
    } else if (!Cpu->Persist.Launched && leaf == 0x40000000) {
        regs[0] = (int)HV_HYPERVISOR_MAX_LEAF;    // EAX = max hypervisor leaf
        regs[1] = (int)HV_HYPERVISOR_VENDOR_EBX;  // EBX = "Velu"
        regs[3] = (int)HV_HYPERVISOR_VENDOR_EDX;  // EDX = "m   "
        regs[2] = (int)HV_HYPERVISOR_VENDOR_ECX;  // ECX = "    "
    } else if (!Cpu->Persist.Launched && leaf == 0x40000001) {
        regs[0] = (int)HV_HYPERVISOR_INTERFACE;   // EAX = stable control ABI signature.
    }
    if (Cpu->Persist.Launched) {
        HvCpuidApply(&Cpu->ProcessorConfig, (ULONG)leaf, (ULONG)subleaf, (ULONG *)regs);
    }

    // Write results back to the saved guest GPRs.
    Cpu->GuestRegs.Rax = (ULONG64)(ULONG)regs[0];
    Cpu->GuestRegs.Rbx = (ULONG64)(ULONG)regs[1];
    Cpu->GuestRegs.Rcx = (ULONG64)(ULONG)regs[2];
    Cpu->GuestRegs.Rdx = (ULONG64)(ULONG)regs[3];

    // Advance the guest RIP past the CPUID. VMCS_EXIT_INSTRUCTION_LENGTH is valid
    // for CPUID exits; CPUID is always 2 bytes (0F A2, SDM Vol. 2A) as fallback.
    if (VmxRead(VMCS_EXIT_INSTRUCTION_LENGTH, &length) != 0 || length == 0) {
        length = 2;
    }
    if (VmxRead(VMCS_GUEST_RIP, &rip) != 0) { return VmExitFail(Cpu); }
    if (VmxWrite(VMCS_GUEST_RIP, rip + length) != 0) { return VmExitFail(Cpu); }

    // Deferred event: the drain worker logs the leaf/subleaf at PASSIVE_LEVEL.
    if (!Cpu->Persist.Launched) {
        HookEnqueue(Cpu, HOOK_EVENT_CPUID, HOOK_CPUID_PACK(leaf, subleaf));
    }

    return VMEXIT_ACTION_RESUME;
}

// Advance the guest RIP past the faulting instruction. VMCS_EXIT_INSTRUCTION_LENGTH
// is valid for the instructions we emulate here; FallbackLength covers the rest.
static NTSTATUS VmExitAdvanceRip(PPER_CPU_DATA Cpu, ULONG64 FallbackLength)
{
    ULONG64 rip = 0;
    ULONG64 length = 0;
    UNREFERENCED_PARAMETER(Cpu);
    if (VmxRead(VMCS_EXIT_INSTRUCTION_LENGTH, &length) != 0 || length == 0) {
        length = FallbackLength;
    }
    if (VmxRead(VMCS_GUEST_RIP, &rip) != 0) { return STATUS_UNSUCCESSFUL; }
    return VmxWrite(VMCS_GUEST_RIP, rip + length);
}

// HLT: put the guest to sleep (activity state = HLT) with RIP advanced. The CPU
// wakes the guest and delivers the interrupt natively when it arrives because
// external-interrupt exiting is off (SDM Vol. 3C 25.3, 24.4.2).
static ULONG64 VmExitHandleHlt(PPER_CPU_DATA Cpu)
{
    if (VmExitAdvanceRip(Cpu, 1) != 0) { return VmExitFail(Cpu); }
    if (VmxWrite(VMCS_GUEST_ACTIVITY_STATE, VmcsGuestHlt) != 0) { return VmExitFail(Cpu); }
    return VMEXIT_ACTION_RESUME;
}

static ULONG64 VmExitInjectGp(PPER_CPU_DATA Cpu)
{
    if (VmxWrite(VMCS_ENTRY_EXCEPTION_ERROR_CODE, 0) != 0 ||
        VmxWrite(VMCS_ENTRY_INTERRUPTION_INFORMATION, 0x80000B0Dull) != 0) {
        return VmExitFail(Cpu);
    }
    return VMEXIT_ACTION_RESUME;
}

static ULONG64 VmExitHandleTimingMsr(PPER_CPU_DATA Cpu, BOOLEAN Write)
{
    ULONG64 cs = 0;
    ULONG64 value = ((ULONG64)(ULONG)Cpu->GuestRegs.Rdx << 32) |
        (ULONG)Cpu->GuestRegs.Rax;
    if (VmxRead(VMCS_GUEST_CS_SELECTOR, &cs) != 0) { return VmExitFail(Cpu); }
    if ((cs & 3) || !TscDeadlineMsr(Cpu, Write, (ULONG)Cpu->GuestRegs.Rcx, &value)) {
        return VmExitInjectGp(Cpu);
    }
    if (!Write) {
        Cpu->GuestRegs.Rax = (ULONG)value;
        Cpu->GuestRegs.Rdx = (ULONG)(value >> 32);
    }
    if (VmExitAdvanceRip(Cpu, 2) != 0) { return VmExitFail(Cpu); }
    return VMEXIT_ACTION_RESUME;
}

static ULONG64 VmExitHandleCr(PPER_CPU_DATA Cpu)
{
    ULONG64 q = 0, value = 0;
    ULONG reg;
    if (VmxRead(VMCS_EXIT_QUALIFICATION, &q) != 0) { return VmExitFail(Cpu); }
    if ((q & 0x3F) != 4) { return VmExitInjectGp(Cpu); } // MOV to CR4
    reg = (ULONG)((q >> 8) & 15);
    if (reg == 4) {
        if (VmxRead(VMCS_GUEST_RSP, &value) != 0) { return VmExitFail(Cpu); }
    } else {
        value = ((ULONG64 *)&Cpu->GuestRegs)[reg < 4 ? reg : reg - 1];
    }
    if (((value | CR4_VMXE) & ~Cpu->Capabilities.Cr4Fixed1) != 0 ||
        ((value | CR4_VMXE) & Cpu->Capabilities.Cr4Fixed0) != Cpu->Capabilities.Cr4Fixed0) {
        return VmExitInjectGp(Cpu);
    }
    if (VmxWrite(VMCS_GUEST_CR4, value | CR4_VMXE) != 0 ||
        VmxWrite(VMCS_CR4_READ_SHADOW, value & ~CR4_VMXE) != 0 ||
        VmExitAdvanceRip(Cpu, 3) != 0) { return VmExitFail(Cpu); }
    return VMEXIT_ACTION_RESUME;
}

static ULONG64 VmExitHandleNmi(PPER_CPU_DATA Cpu, BOOLEAN Window)
{
    ULONG64 primary = 0, pendingEvent = 0;
    if (!Window) { InterlockedExchange(&Cpu->Persist.PendingNmi, 1); }
    if (VmxRead(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS, &primary) != 0 ||
        VmxRead(VMCS_ENTRY_INTERRUPTION_INFORMATION, &pendingEvent) != 0) {
        return VmExitFail(Cpu);
    }
    if (Window && (pendingEvent & (1ull << 31)) == 0) {
        if (InterlockedExchange(&Cpu->Persist.PendingNmi, 0)) {
            if (VmxWrite(VMCS_ENTRY_INTERRUPTION_INFORMATION, 0x80000202ull) != 0) {
                return VmExitFail(Cpu);
            }
        }
        primary &= ~(ULONG64)PRIMARY_NMI_WINDOW_EXITING;
    } else { primary |= PRIMARY_NMI_WINDOW_EXITING; }
    if (VmxWrite(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS, primary) != 0) {
        return VmExitFail(Cpu);
    }
    return VMEXIT_ACTION_RESUME;
}

// XSETBV: execute it in the root with the guest's operands (ECX = XCR index,
// EDX:EAX = value). Guest and host share the physical XCR0 (self-virtualizing).
static ULONG64 VmExitHandleXsetbv(PPER_CPU_DATA Cpu)
{
    ULONG index = (ULONG)Cpu->GuestRegs.Rcx;
    ULONG64 value = ((Cpu->GuestRegs.Rdx & 0xFFFFFFFFull) << 32) |
                    (Cpu->GuestRegs.Rax & 0xFFFFFFFFull);
    ULONG64 cs = 0, allowed;
    int regs[4];
    if (VmxRead(VMCS_GUEST_CS_SELECTOR, &cs) != 0) { return VmExitFail(Cpu); }
    __cpuidex(regs, 0xD, 0);
    allowed = (ULONG)regs[0] | ((ULONG64)(ULONG)regs[3] << 32);
    // Validate before executing in root: a bad guest operand must fault in guest.
    if ((cs & 3) != 0 || index != 0 || (value & 1) == 0 || (value & ~allowed) != 0 ||
        ((value & 4) && !(value & 2)) ||
        ((value & 0x18) != 0 && (value & 0x18) != 0x18) ||
        ((value & 0xE0) != 0 && ((value & 0xE0) != 0xE0 || (value & 6) != 6)) ||
        ((value & (1ull << 18)) && !(value & (1ull << 17)))) {
        return VmExitInjectGp(Cpu);
    }
    __xsetbv(index, value);
    if (VmExitAdvanceRip(Cpu, 3) != 0) { return VmExitFail(Cpu); }  // 0F 01 D1
    return VMEXIT_ACTION_RESUME;
}

// WBINVD: flush the caches in the root, then advance RIP (0F 09).
static ULONG64 VmExitHandleWbinvd(PPER_CPU_DATA Cpu)
{
    __wbinvd();
    if (VmExitAdvanceRip(Cpu, 2) != 0) { return VmExitFail(Cpu); }
    return VMEXIT_ACTION_RESUME;
}

// INVD exits are handled conservatively with writeback before invalidation.
// This preserves dirty data but is not exact INVD emulation.
static ULONG64 VmExitHandleInvd(PPER_CPU_DATA Cpu)
{
    __wbinvd();
    if (VmExitAdvanceRip(Cpu, 2) != 0) { return VmExitFail(Cpu); }
    return VMEXIT_ACTION_RESUME;
}

// Inject #UD (vector 6, hardware exception) to the guest without advancing RIP.
// Used for instructions the guest must never execute (VMX ops, GETSEC, INVEPT,
// INVVPID) and any unhandled exit reason. SDM Vol. 3C 26.6.1 (event injection).
static ULONG64 VmExitInjectUd(PPER_CPU_DATA Cpu, ULONG64 Reason)
{
    HookEnqueue(Cpu, HOOK_EVENT_ERROR, Reason);
    if (VmxWrite(VMCS_ENTRY_INTERRUPTION_INFORMATION, 0x80000306ull) != 0) {
        return VmExitFail(Cpu);
    }
    (VOID)VmxWrite(VMCS_ENTRY_EXCEPTION_ERROR_CODE, 0);
    return VMEXIT_ACTION_RESUME;
}

// Return gates are reached after the guest helper has restored all arguments,
// volatile registers, SIMD state, RFLAGS and the original syscall entry stack.
static ULONG64 VmExitRecordSyscall(PPER_CPU_DATA Cpu, ULONG Index)
{
    SYSCALL_RING_ENTRY entry;
    entry.TargetIndex = Index;
    entry.Rcx = Cpu->GuestRegs.Rcx;
    entry.Rdx = Cpu->GuestRegs.Rdx;
    entry.R8 = Cpu->GuestRegs.R8;
    entry.R9 = Cpu->GuestRegs.R9;
    entry.Rip = g_SyscallTargetVa[Index];
    if (VmxRead(VMCS_GUEST_RSP, &entry.Rsp) != 0 ||
        VmxRead(VMCS_GUEST_RFLAGS, &entry.Flags) != 0) { return VmExitFail(Cpu); }
    SyscallRecord(Cpu->SyscallRing, &entry);
    return VMEXIT_ACTION_RESUME;
}

static ULONG64 VmExitFilterBlock(PPER_CPU_DATA Cpu)
{
    ULONG64 flags = 0, primary = 0, rsp = 0;
    if (VmxRead(VMCS_GUEST_RFLAGS, &flags) != 0 ||
        VmxRead(VMCS_GUEST_RSP, &rsp) != 0 ||
        VmxRead(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS, &primary) != 0) {
        return VmExitFail(Cpu);
    }
    Cpu->FilterReturnFlags = flags;
    Cpu->FilterReturnPrimary = primary;
    Cpu->FilterReturnRsp = rsp + sizeof(ULONG64);
    Cpu->FilterReturnPending = TRUE;
    Cpu->GuestRegs.Rax = FILTER_DENIED_STATUS;
    // Mask IRQ delivery for ONE guest instruction, with no Windows calls in
    // this interval. Queue NMIs until MTF confirms the actual RET has retired.
    // Keeping the reference through RET prevents unload of a suspended stub.
    if (VmxWrite(VMCS_GUEST_RFLAGS, flags & ~0x200ull) != 0 ||
        VmxWrite(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS,
            (primary | PRIMARY_MONITOR_TRAP_FLAG) & ~(ULONG64)PRIMARY_NMI_WINDOW_EXITING) != 0 ||
        VmxWrite(VMCS_GUEST_RIP, (ULONG64)(ULONG_PTR)HvFilterReturn) != 0) {
        return VmExitFail(Cpu);
    }
    return VMEXIT_ACTION_RESUME;
}

static ULONG64 VmExitFilterRetired(PPER_CPU_DATA Cpu, ULONG64 Reason)
{
    ULONG64 event = 0, rsp = 0, primary = Cpu->FilterReturnPrimary;
    if (Reason == 0) {
        if (VmxRead(VMCS_EXIT_INTERRUPTION_INFORMATION, &event) != 0 ||
            (event & 0x800007FFull) != 0x80000202ull) { return VmExitFail(Cpu); }
        InterlockedExchange(&Cpu->Persist.PendingNmi, 1);
        if (VmxRead(VMCS_GUEST_RSP, &rsp) != 0) { return VmExitFail(Cpu); }
        // An NMI exit can win arbitration at the boundary after RET. If the
        // return already retired, complete now rather than single-step caller.
        if (rsp == Cpu->FilterReturnRsp - sizeof(ULONG64)) { return VMEXIT_ACTION_RESUME; }
        if (rsp != Cpu->FilterReturnRsp) { return VmExitFail(Cpu); }
    } else if (Reason != EXIT_REASON_MONITOR_TRAP_FLAG ||
        VmxRead(VMCS_GUEST_RSP, &rsp) != 0 || rsp != Cpu->FilterReturnRsp) {
        return VmExitFail(Cpu);
    }
    // RET has consumed the real return address and CET has updated SSP itself.
    // No guest execution address is guessed or extracted from its stack in root.
    if (VmxWrite(VMCS_GUEST_RFLAGS, Cpu->FilterReturnFlags) != 0 ||
        VmxWrite(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS, primary) != 0) {
        return VmExitFail(Cpu);
    }
    Cpu->FilterReturnPending = FALSE;
    // After publishing FALSE, root NMIs arm the window themselves. Re-read
    // controls and only OR the bit, so a concurrent root NMI cannot be lost.
    if (Cpu->Persist.PendingNmi) {
        if (VmxRead(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS, &primary) != 0 ||
            VmxWrite(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS,
                primary | PRIMARY_NMI_WINDOW_EXITING) != 0) { return VmExitFail(Cpu); }
    }
    FilterLeave();
    return VMEXIT_ACTION_RESUME;
}

static ULONG64 VmExitHandleSyscall(PPER_CPU_DATA Cpu)
{
    ULONG i;
    ULONG64 rip = 0, cs = 0, rsp = 0, flags = 0;
    if (VmxRead(VMCS_GUEST_RIP, &rip) != 0 ||
        VmxRead(VMCS_GUEST_CS_SELECTOR, &cs) != 0) { return VmExitFail(Cpu); }
    if ((cs & 3) != 0) { return VmExitInjectUd(Cpu, EXIT_REASON_VMCALL); }

    for (i = 0; i < SYSCALL_TARGET_COUNT; ++i) {
        if (g_FilterBlockGates[i] != NULL && rip == (ULONG64)(ULONG_PTR)g_FilterBlockGates[i]) {
            // Blocked calls intentionally omit the syscall ring entry.
            return VmExitFilterBlock(Cpu);
        }
        if (g_FilterContinueGates[i] != NULL && rip == (ULONG64)(ULONG_PTR)g_FilterContinueGates[i]) {
            PHOOK_CONTEXT hook = Cpu->SyscallHooks[i];
            ULONG64 action;
            if (hook == NULL || !hook->Installed) { return VmExitFail(Cpu); }
            // Guest capture already handled this entry; kernel targets omit PID/TID.
            rip = (g_SyscallTargetVa[i] & ~(PAGE_SIZE - 1ull)) +
                hook->TargetFunctionOffset;
            if (VmxWrite(VMCS_GUEST_RIP, rip) != 0) { return VmExitFail(Cpu); }
            action = VmExitReplayOriginal(Cpu, hook);
            // Guest will resume in original kernel code, never in driver text.
            FilterLeave();
            return action;
        }
    }
    for (i = 0; i < SYSCALL_TARGET_COUNT; ++i) {
        PHOOK_CONTEXT hook = Cpu->SyscallHooks[i];
        ULONG64 action;
        if (hook == NULL || !hook->Installed ||
            rip != ((g_SyscallTargetVa[i] & ~(PAGE_SIZE - 1ull)) + hook->TargetFunctionOffset)) {
            continue;
        }
        if (g_FilterGuestEntries[i] != NULL && g_PersistActive == HV_STATE_RUNNING) {
            if (VmxRead(VMCS_GUEST_RSP, &rsp) != 0 ||
                VmxRead(VMCS_GUEST_RFLAGS, &flags) != 0) { return VmExitFail(Cpu); }
            // Validate the fixed ABI frame and permit paging only with IF=1.
            // Other contexts only record the call; FilterDecide also checks IRQL.
            if ((rsp & 15) == 8 && (flags & 0x200) != 0 && FilterTryEnter()) {
                if (VmxWrite(VMCS_GUEST_RIP, (ULONG64)(ULONG_PTR)g_FilterGuestEntries[i]) != 0) {
                    FilterLeave();
                    return VmExitFail(Cpu);
                }
                return VMEXIT_ACTION_RESUME;
            }
        }
        action = VmExitRecordSyscall(Cpu, i);
        if (action != VMEXIT_ACTION_RESUME) { return action; }
        // Re-execute the original instruction without advancing guest RIP.
        return VmExitReplayOriginal(Cpu, hook);
    }
    return VmExitInjectUd(Cpu, EXIT_REASON_VMCALL);
}

// Guest unrecoverable (triple fault, INIT, EPT misconfig): VMCLEAR + VMXOFF and
// halt this core. There is no valid guest state to resume.
ULONG64 VmExitResumeFailure(PPER_CPU_DATA Cpu, ULONG Status)
{
    if (Cpu->Persist.Launched) { PersistFatal(Cpu, 0x200 + Status); }
    Cpu->LaunchResult = Status;
    *(ULONG64 *)(ULONG_PTR)Cpu->LaunchCallerRsp = Status;
    return Cpu->LaunchCallerRsp;
}

// Restore interrupted event delivery before any path can resume the guest.
static ULONG VmExitPrepareEvents(ULONG64 Reason)
{
    ULONG64 vectoring = 0, error = 0, length = 0;
    if (VmxRead(VMCS_IDT_VECTORING_INFORMATION, &vectoring) != 0) { return 1; }
    if ((vectoring & (1ull << 31)) != 0) {
        if ((vectoring & (1u << 11)) != 0 &&
            (VmxRead(VMCS_IDT_VECTORING_ERROR_CODE, &error) != 0 ||
             VmxWrite(VMCS_ENTRY_EXCEPTION_ERROR_CODE, error) != 0)) { return 1; }
        if (((vectoring >> 8) & 7) >= 4 &&
            (VmxRead(VMCS_EXIT_INSTRUCTION_LENGTH, &length) != 0 ||
             VmxWrite(VMCS_ENTRY_INSTRUCTION_LENGTH, length) != 0)) { return 1; }
        vectoring &= ~(1ull << 12); // Reserved in VM-entry interruption information.
    } else {
        vectoring = 0;
        if ((Reason & 0xFFFFu) == EXIT_REASON_EPT_VIOLATION) {
            ULONG64 qualification = 0, blocking = 0;
            if (VmxRead(VMCS_EXIT_QUALIFICATION, &qualification) != 0) { return 1; }
            // Runtime requires virtual NMIs. An interrupted IRET can clear
            // NMI blocking before an EPT exit; restore it before retrying IRET.
            // Qualification bit 12 is defined here only without valid vectoring.
            if ((qualification & (1ull << 12)) != 0 &&
                (VmxRead(VMCS_GUEST_INTERRUPTIBILITY_STATE, &blocking) != 0 ||
                 VmxWrite(VMCS_GUEST_INTERRUPTIBILITY_STATE, blocking | 8ull) != 0)) {
                return 1;
            }
        }
    }
    return VmxWrite(VMCS_ENTRY_INTERRUPTION_INFORMATION, vectoring);
}

// Returns VMEXIT_ACTION_RESUME (0) to request VMRESUME, or the launcher RSP for
// the ACTION_EXIT path. Never logs, allocates, blocks, or takes a blocking lock.
ULONG64 VmExitDispatch(PPER_CPU_DATA Cpu, ULONG64 GuestRax)
{
    // No per-exit TSC measurement or compensation. A fixed operator-selected
    // offset is committed separately; hardware handles timestamp instructions.
    ULONG64 reason = 0;
    ULONG result = VmxRead(VMCS_EXIT_REASON, &reason);
    Cpu->ExitReasonValid = result == 0;
    Cpu->ExitReason = reason;
    Cpu->ExitGuestRax = GuestRax;

    if (result != 0) { return VmExitFail(Cpu); }

    if ((reason & (1ull << 31)) != 0) { return VmExitFail(Cpu); }
    if (Cpu->Persist.Launched && VmExitPrepareEvents(reason) != 0) {
        return VmExitFail(Cpu);
    }
    reason &= 0xFFFFu;
    if (Cpu->FilterReturnPending) { return VmExitFilterRetired(Cpu, reason); }

    // Restore any outstanding replay before dispatching an intervening exit.
    // The one-shot VMCALL test completes through its launcher instead.
    if (Cpu->ReadMtfHook != NULL && Cpu->ReadMtfHook->ReadMtfArmed &&
        reason != EXIT_REASON_MONITOR_TRAP_FLAG &&
        (Cpu->Persist.Launched || reason != EXIT_REASON_VMCALL)) {
        ULONG64 action = VmExitHandleDeferredMtf(Cpu, reason);
        if (action != VMEXIT_ACTION_RESUME) { return action; }
    }

    switch ((ULONG)reason) {
    case 0: { // NMI or #UD entry trap; other exceptions execute natively
        ULONG64 event = 0;
        if (VmxRead(VMCS_EXIT_INTERRUPTION_INFORMATION, &event) != 0) { return VmExitFail(Cpu); }
        if ((event & 0x800007FFull) == 0x80000306ull && Cpu->Persist.Launched) {
            return VmExitHandleSyscall(Cpu); // Two-byte UD2 entry trap; others reinject #UD.
        }
        if ((event & 0x800007FFull) != 0x80000202ull) { return VmExitFail(Cpu); }
        return VmExitHandleNmi(Cpu, FALSE);
    }
    case 8:
        return VmExitHandleNmi(Cpu, TRUE);
    case 28:
        return VmExitHandleCr(Cpu);
    case 31:
        return VmExitHandleTimingMsr(Cpu, FALSE);
    case 32:
        return VmExitHandleTimingMsr(Cpu, TRUE);
    case EXIT_REASON_VMCALL:
        // Runtime VMCALL is accepted only at an installed syscall entry.
        // The shutdown handshake has its own HvNativeExit host entry.
        if (Cpu->Persist.Launched) { return VmExitHandleSyscall(Cpu); }
        // Test magics (one-shot launches only; never after persistent launch).
        // Any VMCALL is a successful round-trip; RAX identifies the test outcome.
        Cpu->LaunchResult = 0;
        if (GuestRax == 0xCAFEBABEull) {
            HookEnqueue(Cpu, HOOK_EVENT_FIRED, GuestRax);
        } else if (GuestRax == 0xDEADBEEFull && Cpu->Hook != NULL &&
            Cpu->Hook->Installed) {
            HookEnqueue(Cpu, HOOK_EVENT_NOT_FIRED, GuestRax);
        }
        *(ULONG64 *)(ULONG_PTR)Cpu->LaunchCallerRsp = Cpu->LaunchResult;
        return Cpu->LaunchCallerRsp;

    case EXIT_REASON_EPT_VIOLATION:
        return VmExitHandleEptViolation(Cpu);

    case EXIT_REASON_MONITOR_TRAP_FLAG:
        return VmExitHandleMtf(Cpu);

    case EXIT_REASON_CPUID:
        return VmExitHandleCpuid(Cpu, GuestRax);

    case EXIT_REASON_HLT:
        return VmExitHandleHlt(Cpu);

    case EXIT_REASON_XSETBV:
        return VmExitHandleXsetbv(Cpu);

    case EXIT_REASON_WBINVD:
        return VmExitHandleWbinvd(Cpu);

    case EXIT_REASON_INVD:
        return VmExitHandleInvd(Cpu);

    // Unrecoverable guest states: halt the core.
    case EXIT_REASON_TRIPLE_FAULT:
    case EXIT_REASON_INIT:
    case EXIT_REASON_EPT_MISCONFIG:
        return VmExitFail(Cpu);

    // Instructions the guest must never execute (Windows never does): inject #UD.
    case EXIT_REASON_GETSEC:
    case EXIT_REASON_VMCLEAR:
    case EXIT_REASON_VMLAUNCH:
    case EXIT_REASON_VMPTRLD:
    case EXIT_REASON_VMPTRST:
    case EXIT_REASON_VMREAD:
    case EXIT_REASON_VMWRITE:
    case EXIT_REASON_VMXOFF:
    case EXIT_REASON_VMXON:
    case EXIT_REASON_INVEPT:
    case EXIT_REASON_INVVPID:
        return VmExitInjectUd(Cpu, reason);

    default:
        return VmExitFail(Cpu);
    }
}

