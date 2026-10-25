// hypervisor/src/vmcs.c
// Acceptance tests (not executed here): one/two CPUs load; per-core ordered
// VMCLEAR -> VMPTRLD -> VMREAD logs; unload without BSOD or Verifier/Poolmon leaks.
// Test-only revision corruption must fail load and roll back all CPUs/allocations.
// No zero-error-field requirement: VMREAD success, not its value, is checked.
#include "common.h"
#include "vmcs.h"
#include "persist.h"

static NTSTATUS VmcsConfigureRuntime(PPER_CPU_DATA Cpu, ULONG Index);

ULONG VmcsQueryInstructionError(PULONG Error)
{
    ULONG64 value = 0;
    ULONG result = VmxRead(VMCS_INSTRUCTION_ERROR, &value);
    if (result == 0) {
        *Error = (ULONG)value;
    }
    return result;
}

NTSTATUS VmcsInitializeAll(VOID)
{
    NTSTATUS status = PerCpuAllocateVmcsRegions();
    ULONG i;
    if (!NT_SUCCESS(status)) {
        HV_ERROR("VMCS: region allocation failed, status 0x%08lX\n", (ULONG)status);
        return status;
    }
    for (i = 0; i < g_CpuCount; ++i) {
        PPER_CPU_DATA cpu = g_PerCpuData[i];
        GROUP_AFFINITY target = { 0 };
        GROUP_AFFINITY previous;
        ULONG result;
        ULONG queryResult;
        ULONG error = 0;
        ULONG64 guestRip = 0;
        PCSTR operation = "VMCLEAR";
        target.Mask = (KAFFINITY)1 << i;
        KeSetSystemGroupAffinityThread(&target, &previous);

        // SDM 25.11.3: establish implementation-specific state before first VMPTRLD.
        result = VmxClear((ULONG64)cpu->VmcsPa.QuadPart);
        if (result != 0) { goto CoreFailed; }
        HV_INFO("VMCS: VMCLEAR ok on core %lu\n", i);

        operation = "VMPTRLD";
        result = VmxPtrLoad((ULONG64)cpu->VmcsPa.QuadPart);
        if (result != 0) { goto CoreFailed; }
        cpu->VmcsActive = 1;
        HV_INFO("VMCS: VMPTRLD ok on core %lu\n", i);

        operation = "VMREAD(VM_INSTRUCTION_ERROR)";
        result = VmcsQueryInstructionError(&error);
        if (result != 0) { goto CoreFailed; }
        cpu->LastVmInstructionError = error;
        cpu->VmInstructionErrorValid = TRUE;
        HV_INFO("VMCS: core %lu VM_INSTRUCTION_ERROR=0x%08lX (informational)\n", i, error);

        // Defer VMWRITE operations to population; this read's value is unspecified.
        operation = "VMREAD(GUEST_RIP)";
        result = VmxRead(VMCS_GUEST_RIP, &guestRip);
        if (result != 0) { goto CoreFailed; }
        HV_INFO("VMCS: VMREAD(GUEST_RIP) ok on core %lu; value=0x%016I64X (uninitialized)\n",
            i, guestRip);
        KeRevertToUserGroupAffinityThread(&previous);
        continue;

CoreFailed:
        cpu->VmInstructionErrorValid = FALSE;
        if (result == 1) {
            queryResult = VmcsQueryInstructionError(&error);
            if (queryResult == 0) {
                cpu->LastVmInstructionError = error;
                cpu->VmInstructionErrorValid = TRUE;
                HV_ERROR("VMCS: core %lu %s: VMfailValid; VM_INSTRUCTION_ERROR=%lu (0x%08lX)\n",
                    i, operation, error, error);
            } else {
                HV_ERROR("VMCS: core %lu %s: VMfailValid; error-field VMREAD failed, status=%lu\n",
                    i, operation, queryResult);
            }
        } else {
            HV_ERROR("VMCS: core %lu %s: VMfailInvalid; error code unavailable (no current VMCS)\n",
                i, operation);
        }
        KeRevertToUserGroupAffinityThread(&previous);
        // DriverEntry owns recovery: VmxStopAll -> PerCpuFree, even after partial success.
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Populate the guest-state, host-state and control areas of every VMCS.
//
// Every field is written on the core that executed VMPTRLD for that VMCS: a VMCS
// is current only on the core that made it current (SDM Vol. 3C 25.11.3, 31.3).
// Guest and host values are the live state of that core, so the populated VMCS
// describes exactly the context that VMLAUNCH will use. Control values
// are derived from the captured IA32_VMX_*_CTLS capability MSRs, so
// they are legal on every core by construction.
// References: SDM Vol. 3C 24.6-24.8, 26.1-26.3, 31.4; Vol. 3D B.1-B.4.
// ---------------------------------------------------------------------------

// Propagates the first VMWRITE failure out of the calling helper.
#define VMCS_WRITE(cpu, index, field, value, name)                       \
    do {                                                                 \
        NTSTATUS writeStatus = VmcsWriteField((cpu), (index), (field),   \
            (value), (name));                                            \
        if (!NT_SUCCESS(writeStatus)) {                                  \
            return writeStatus;                                          \
        }                                                                \
    } while (0)

// Field IDs per guest segment, in VMCS_SEGMENT_INDEX order. The order matches
// SDM Vol. 3D B.3.3/B.4.3 (ES, CS, SS, DS, FS, GS, LDTR, TR).
static const VMCS_SEGMENT_FIELDS g_GuestSegmentFields[VmcsSegmentCount] = {
    { VMCS_GUEST_ES_SELECTOR, VMCS_GUEST_ES_LIMIT, VMCS_GUEST_ES_ACCESS_RIGHTS,
      VMCS_GUEST_ES_BASE, "GUEST_ES" },
    { VMCS_GUEST_CS_SELECTOR, VMCS_GUEST_CS_LIMIT, VMCS_GUEST_CS_ACCESS_RIGHTS,
      VMCS_GUEST_CS_BASE, "GUEST_CS" },
    { VMCS_GUEST_SS_SELECTOR, VMCS_GUEST_SS_LIMIT, VMCS_GUEST_SS_ACCESS_RIGHTS,
      VMCS_GUEST_SS_BASE, "GUEST_SS" },
    { VMCS_GUEST_DS_SELECTOR, VMCS_GUEST_DS_LIMIT, VMCS_GUEST_DS_ACCESS_RIGHTS,
      VMCS_GUEST_DS_BASE, "GUEST_DS" },
    { VMCS_GUEST_FS_SELECTOR, VMCS_GUEST_FS_LIMIT, VMCS_GUEST_FS_ACCESS_RIGHTS,
      VMCS_GUEST_FS_BASE, "GUEST_FS" },
    { VMCS_GUEST_GS_SELECTOR, VMCS_GUEST_GS_LIMIT, VMCS_GUEST_GS_ACCESS_RIGHTS,
      VMCS_GUEST_GS_BASE, "GUEST_GS" },
    { VMCS_GUEST_LDTR_SELECTOR, VMCS_GUEST_LDTR_LIMIT, VMCS_GUEST_LDTR_ACCESS_RIGHTS,
      VMCS_GUEST_LDTR_BASE, "GUEST_LDTR" },
    { VMCS_GUEST_TR_SELECTOR, VMCS_GUEST_TR_LIMIT, VMCS_GUEST_TR_ACCESS_RIGHTS,
      VMCS_GUEST_TR_BASE, "GUEST_TR" }
};

// SDM Vol. 3D A.1: bit n of the low 32 bits of an IA32_VMX_*_CTLS MSR must be 1,
// a 1 in the high 32 bits permits a 1 in that control bit.
static ULONG VmcsAdjustControl(ULONG Desired, VMX_CONTROL_MASKS Allowed)
{
    return (Desired & Allowed.Allowed1) | Allowed.Allowed0;
}

// Writes one VMCS field on the calling core. The field encoding is logged with
// the name so a failure inside a multi-field helper is unambiguous. VMWRITE does
// not validate field values; it fails only for an unsupported/invalid encoding
// or with no current VMCS, both reported via VM_INSTRUCTION_ERROR
// (SDM Vol. 3C 31.4/31.5).
static NTSTATUS VmcsWriteField(PPER_CPU_DATA Cpu, ULONG Index, ULONG64 Field,
    ULONG64 Value, PCSTR Name)
{
    ULONG result = VmxWrite(Field, Value);
    ULONG error = 0;
    if (result == 0) {
        return STATUS_SUCCESS;
    }
    Cpu->VmInstructionErrorValid = FALSE;
    if (result == 1) {
        if (VmcsQueryInstructionError(&error) == 0) {
            Cpu->LastVmInstructionError = error;
            Cpu->VmInstructionErrorValid = TRUE;
            HV_ERROR("VMCS: core %lu VMWRITE(%s, 0x%04I64X) VMfailValid; VM_INSTRUCTION_ERROR=%lu (0x%08lX)\n",
                Index, Name, Field, error, error);
        } else {
            HV_ERROR("VMCS: core %lu VMWRITE(%s, 0x%04I64X) VMfailValid; error field unreadable\n",
                Index, Name, Field);
        }
    } else {
        HV_ERROR("VMCS: core %lu VMWRITE(%s, 0x%04I64X) VMfailInvalid; no current VMCS\n",
            Index, Name, Field);
    }
    return STATUS_UNSUCCESSFUL;
}

// Writes the selector, limit, access rights and base of one segment.
static NTSTATUS VmcsWriteSegment(PPER_CPU_DATA Cpu, ULONG Index,
    const VMCS_SEGMENT_FIELDS *Fields, const VMCS_SEGMENT_STATE *State)
{
    VMCS_WRITE(Cpu, Index, Fields->Selector, State->Selector, Fields->Name);
    VMCS_WRITE(Cpu, Index, Fields->Limit, State->Limit, Fields->Name);
    VMCS_WRITE(Cpu, Index, Fields->AccessRights, State->AccessRights, Fields->Name);
    return VmcsWriteField(Cpu, Index, Fields->Base, State->Base, Fields->Name);
}

// Decodes a GDT descriptor into VMCS guest-segment form (SDM Vol. 3A 3.4.5,
// Figures 3-8/3-9 for the split fields; 3.5 for the 64-bit system layout).
// Access rights retain type/S/DPL/P and AVL/L/D-B/G (bits 12..15).
// A null selector is unusable. A system descriptor occupies sixteen bytes.
static NTSTATUS VmcsDecodeSegment(ULONG64 GdtBase, ULONG GdtLimit,
    ULONG Selector, VMCS_SEGMENT_STATE *State)
{
    const UCHAR *descriptor;
    ULONG offset;
    ULONG limit;
    ULONG accessRights;
    ULONG highBase = 0;

    State->Selector = Selector;
    if ((Selector & ~3u) == 0) {
        State->AccessRights = VMCS_ACCESS_RIGHTS_UNUSABLE;
        State->Limit = 0;
        State->Base = 0;
        return STATUS_SUCCESS;
    }
    if ((Selector & VMCS_SELECTOR_TI) != 0) {
        // The descriptor lives in the LDT. The kernel runs at CPL 0 with GDT
        // selectors, so an LDT selector means population is being asked to model a
        // state it does not understand: report it instead of guessing.
        HV_ERROR("VMCS: selector 0x%04lX has TI set; only GDT selectors are supported\n",
            Selector);
        return STATUS_NOT_SUPPORTED;
    }
    offset = Selector & VMCS_SELECTOR_INDEX_MASK;
    // The descriptor must lie wholly inside the GDT (SDM Vol. 3A 3.5.1): a
    // selector past the limit names no descriptor, and reading it would fault.
    if (offset + (VMCS_DESCRIPTOR_SIZE - 1u) > GdtLimit) {
        HV_ERROR("VMCS: selector 0x%04lX is outside the GDT (limit 0x%04lX)\n",
            Selector, GdtLimit);
        return STATUS_INVALID_PARAMETER;
    }
    descriptor = (const UCHAR *)(GdtBase + offset);
    limit = (ULONG)descriptor[0] | ((ULONG)descriptor[1] << 8) |
        (((ULONG)descriptor[VMCS_DESCRIPTOR_ATTR_HIGH_BYTE] &
            VMCS_DESCRIPTOR_LIMIT_HIGH_MASK) << 16);
    accessRights = (ULONG)descriptor[VMCS_DESCRIPTOR_ATTR_BYTE] |
        (((ULONG)descriptor[VMCS_DESCRIPTOR_ATTR_HIGH_BYTE] &
            0xF0u) << 8);
    if ((accessRights & VMCS_ACCESS_RIGHTS_GRANULARITY) != 0) {
        // G = 1: the 20-bit limit is scaled by 4 KB (SDM Vol. 3A 3.4.5).
        limit = (limit << 12) | 0xFFFu;
    }
    State->Base = (ULONG64)descriptor[2] | ((ULONG64)descriptor[3] << 8) |
        ((ULONG64)descriptor[4] << 16) | ((ULONG64)descriptor[7] << 24);
    if ((accessRights & VMCS_ACCESS_RIGHTS_SYSTEM) == 0) {
        // System descriptors (TR, LDT) keep base bits 63:32 at offset 8; the
        // copy keeps the read independent of descriptor alignment.
        if (offset + 15u > GdtLimit) {
            return STATUS_INVALID_PARAMETER;
        }
        RtlCopyMemory(&highBase, descriptor + VMCS_DESCRIPTOR_HIGH_BASE_OFFSET,
            sizeof(highBase));
        State->Base |= (ULONG64)highBase << VMCS_DESCRIPTOR_HIGH_BASE_SHIFT;
    }
    State->Limit = limit;
    State->AccessRights = accessRights;
    return STATUS_SUCCESS;
}

// Captures the live state the guest and host areas are built from. MSR reads can
// fault with #GP (for example MSR_IA32_DEBUGCTL on a core without debug-store
// support), so they are guarded. Capture runs pinned at DISPATCH_LEVEL before
// the short assembly transition disables interrupts.
static NTSTATUS VmcsCaptureLiveState(VMCS_LIVE_STATE *State)
{
    HV_DESCRIPTOR_TABLE gdtr;
    HV_DESCRIPTOR_TABLE idtr;
    RtlZeroMemory(&gdtr, sizeof(gdtr));
    RtlZeroMemory(&idtr, sizeof(idtr));

    State->Selector[VmcsSegmentEs] = HvGetEs();
    State->Selector[VmcsSegmentCs] = HvGetCs();
    State->Selector[VmcsSegmentSs] = HvGetSs();
    State->Selector[VmcsSegmentDs] = HvGetDs();
    State->Selector[VmcsSegmentFs] = HvGetFs();
    State->Selector[VmcsSegmentGs] = HvGetGs();
    State->Selector[VmcsSegmentTr] = HvGetTr();
    State->Selector[VmcsSegmentLdtr] = HvGetLdtr();
    State->Cr0 = __readcr0();
    State->Cr3 = __readcr3();
    State->Cr4 = __readcr4();
    State->Dr7 = __readdr(7);
    State->Rflags = HvGetRflags();
    State->Rsp = HvGetRsp();
    HvGetGdtr(&gdtr);
    HvGetIdtr(&idtr);
    State->GdtrBase = gdtr.Base;
    State->GdtrLimit = gdtr.Limit;
    State->IdtrBase = idtr.Base;
    State->IdtrLimit = idtr.Limit;
    __try {
        State->FsBase = __readmsr(MSR_IA32_FS_BASE);
        State->GsBase = __readmsr(MSR_IA32_GS_BASE);
        State->Pat = __readmsr(MSR_IA32_PAT);
        State->Efer = __readmsr(MSR_IA32_EFER);
        State->DebugCtl = __readmsr(MSR_IA32_DEBUGCTL);
        State->SysenterCs = __readmsr(MSR_IA32_SYSENTER_CS);
        State->SysenterEsp = __readmsr(MSR_IA32_SYSENTER_ESP);
        State->SysenterEip = __readmsr(MSR_IA32_SYSENTER_EIP);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        HV_ERROR("VMCS: a guest/host-state MSR read raised an exception on this core\n");
        return STATUS_NOT_SUPPORTED;
    }
    return STATUS_SUCCESS;
}

// Control fields that do not depend on live state. Values that belong to
// features this VMCS leaves disabled (control and MSR load/store counts, TPR
// threshold, TSC offset) are still zeroed so the structure is deterministic:
// VMCLEAR does not promise to preserve fields it does not use (SDM Vol. 3C
// 25.11.3). VMCS_LINK_POINTER must be all ones because this VMCS is not
// shadowed (SDM Vol. 3D B.2.3).
static NTSTATUS VmcsWriteConstantFields(PPER_CPU_DATA Cpu, ULONG Index)
{
    VMCS_WRITE(Cpu, Index, VMCS_EXCEPTION_BITMAP, MAXULONG, "EXCEPTION_BITMAP");
    VMCS_WRITE(Cpu, Index, VMCS_PAGE_FAULT_ERROR_CODE_MASK, 0,
        "PAGE_FAULT_ERROR_CODE_MASK");
    VMCS_WRITE(Cpu, Index, VMCS_PAGE_FAULT_ERROR_CODE_MATCH, 0,
        "PAGE_FAULT_ERROR_CODE_MATCH");
    VMCS_WRITE(Cpu, Index, VMCS_CR3_TARGET_COUNT, 0, "CR3_TARGET_COUNT");
    VMCS_WRITE(Cpu, Index, VMCS_CR3_TARGET_VALUE_0, 0, "CR3_TARGET_VALUE_0");
    VMCS_WRITE(Cpu, Index, VMCS_CR3_TARGET_VALUE_1, 0, "CR3_TARGET_VALUE_1");
    VMCS_WRITE(Cpu, Index, VMCS_CR3_TARGET_VALUE_2, 0, "CR3_TARGET_VALUE_2");
    VMCS_WRITE(Cpu, Index, VMCS_CR3_TARGET_VALUE_3, 0, "CR3_TARGET_VALUE_3");
    VMCS_WRITE(Cpu, Index, VMCS_EXIT_MSR_STORE_COUNT, 0, "EXIT_MSR_STORE_COUNT");
    VMCS_WRITE(Cpu, Index, VMCS_EXIT_MSR_LOAD_COUNT, 0, "EXIT_MSR_LOAD_COUNT");
    VMCS_WRITE(Cpu, Index, VMCS_ENTRY_MSR_LOAD_COUNT, 0, "ENTRY_MSR_LOAD_COUNT");
    VMCS_WRITE(Cpu, Index, VMCS_ENTRY_INTERRUPTION_INFORMATION, 0,
        "ENTRY_INTERRUPTION_INFORMATION");
    VMCS_WRITE(Cpu, Index, VMCS_ENTRY_EXCEPTION_ERROR_CODE, 0,
        "ENTRY_EXCEPTION_ERROR_CODE");
    VMCS_WRITE(Cpu, Index, VMCS_ENTRY_INSTRUCTION_LENGTH, 0,
        "ENTRY_INSTRUCTION_LENGTH");
    VMCS_WRITE(Cpu, Index, VMCS_TPR_THRESHOLD, 0, "TPR_THRESHOLD");
    VMCS_WRITE(Cpu, Index, VMCS_TSC_OFFSET, 0, "TSC_OFFSET");
    VMCS_WRITE(Cpu, Index, VMCS_LINK_POINTER, 0xFFFFFFFFu, "LINK_POINTER");
    VMCS_WRITE(Cpu, Index, VMCS_LINK_POINTER_HIGH, 0xFFFFFFFFu, "LINK_POINTER_HIGH");
    return STATUS_SUCCESS;
}

// Writes the five VMCS control fields plus the CR0/CR4 masks and read shadows.
// With a mask of 0 the guest reads the actual CR bits, not the read shadows. The control values passed in already carry
// the mandatory bits of the capability MSRs.
static NTSTATUS VmcsWriteControlFields(PPER_CPU_DATA Cpu, ULONG Index,
    const VMCS_LIVE_STATE *Live, ULONG Pin, ULONG Primary, ULONG Secondary,
    ULONG Exit, ULONG Entry)
{
    VMCS_WRITE(Cpu, Index, VMCS_PIN_BASED_CONTROLS, Pin, "PIN_BASED_CONTROLS");
    VMCS_WRITE(Cpu, Index, VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS, Primary,
        "PRIMARY_CONTROLS");
    VMCS_WRITE(Cpu, Index, VMCS_SECONDARY_PROCESSOR_BASED_CONTROLS, Secondary,
        "SECONDARY_CONTROLS");
    VMCS_WRITE(Cpu, Index, VMCS_EXIT_CONTROLS, Exit, "EXIT_CONTROLS");
    VMCS_WRITE(Cpu, Index, VMCS_ENTRY_CONTROLS, Entry, "ENTRY_CONTROLS");
    VMCS_WRITE(Cpu, Index, VMCS_CR0_GUEST_HOST_MASK, 0, "CR0_GUEST_HOST_MASK");
    VMCS_WRITE(Cpu, Index, VMCS_CR4_GUEST_HOST_MASK, 0, "CR4_GUEST_HOST_MASK");
    VMCS_WRITE(Cpu, Index, VMCS_CR0_READ_SHADOW, Live->Cr0, "CR0_READ_SHADOW");
    VMCS_WRITE(Cpu, Index, VMCS_CR4_READ_SHADOW, Live->Cr4, "CR4_READ_SHADOW");
    return STATUS_SUCCESS;
}

// Writes the guest-state area (SDM Vol. 3C 26.3.1.1-26.3.1.2, Vol. 3D B.3.3 and
// B.4.3). The guest executes only MOV EAX,magic and VMCALL; IF and TF stay clear.
static NTSTATUS VmcsWriteGuestFields(PPER_CPU_DATA Cpu, ULONG Index,
    const VMCS_SEGMENT_STATE *Segments, const VMCS_LIVE_STATE *Live)
{
    ULONG i;
    for (i = 0; i < VmcsSegmentCount; ++i) {
        NTSTATUS status = VmcsWriteSegment(Cpu, Index, &g_GuestSegmentFields[i],
            &Segments[i]);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_FS_BASE, Live->FsBase, "GUEST_FS_BASE");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_GS_BASE, Live->GsBase, "GUEST_GS_BASE");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_CR0, Live->Cr0, "GUEST_CR0");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_CR3, Live->Cr3, "GUEST_CR3");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_CR4, Live->Cr4, "GUEST_CR4");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_DR7, 0x400u, "GUEST_DR7");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_RSP, Live->Rsp, "GUEST_RSP");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_RIP, (ULONG64)(ULONG_PTR)HvGuestEntryStub,
        "GUEST_RIP");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_RFLAGS, 2, "GUEST_RFLAGS");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_GDTR_BASE, Live->GdtrBase, "GUEST_GDTR_BASE");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_GDTR_LIMIT, Live->GdtrLimit, "GUEST_GDTR_LIMIT");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_IDTR_BASE, Live->IdtrBase, "GUEST_IDTR_BASE");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_IDTR_LIMIT, Live->IdtrLimit, "GUEST_IDTR_LIMIT");
    // No STI/MOV-SS/NMI blocking and no pending debug
    // exceptions: the guest starts in the state VM entry expects for a fresh
    // launch rather than inheriting whatever VMCLEAR left in the field.
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_INTERRUPTIBILITY_STATE, 0,
        "GUEST_INTERRUPTIBILITY_STATE");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_ACTIVITY_STATE, VmcsGuestActive,
        "GUEST_ACTIVITY_STATE");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_PENDING_DEBUG_EXCEPTIONS, 0,
        "GUEST_PENDING_DEBUG_EXCEPTIONS");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_SYSENTER_CS, Live->SysenterCs, "GUEST_SYSENTER_CS");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_SYSENTER_ESP, Live->SysenterEsp,
        "GUEST_SYSENTER_ESP");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_SYSENTER_EIP, Live->SysenterEip,
        "GUEST_SYSENTER_EIP");
    // These three are loaded only when the matching entry/exit control is set
    // (SDM Vol. 3C 26.3.1.1); they carry live values so enabling the controls in
    // a later step cannot change what the guest observes.
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_PAT, Live->Pat, "GUEST_IA32_PAT");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_EFER, Live->Efer, "GUEST_IA32_EFER");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_DEBUGCTL, 0, "GUEST_IA32_DEBUGCTL");
    return STATUS_SUCCESS;
}

// Writes the host-state area (SDM Vol. 3C 26.2, Vol. 3D B.1.3/B.3.4/B.4.4).
// These values are loaded by the CPU when it exits to the root, so they are the
// live root values of this core, not the guest's.
static NTSTATUS VmcsWriteHostFields(PPER_CPU_DATA Cpu, ULONG Index,
    const VMCS_SEGMENT_STATE *Segments, const VMCS_LIVE_STATE *Live)
{
    static const ULONG hostSelectorFields[VmcsSegmentCount - 1] = {
        VMCS_HOST_ES_SELECTOR, VMCS_HOST_CS_SELECTOR, VMCS_HOST_SS_SELECTOR,
        VMCS_HOST_DS_SELECTOR, VMCS_HOST_FS_SELECTOR, VMCS_HOST_GS_SELECTOR,
        VMCS_HOST_TR_SELECTOR
    };
    static const ULONG hostSelectorSegments[VmcsSegmentCount - 1] = {
        VmcsSegmentEs, VmcsSegmentCs, VmcsSegmentSs, VmcsSegmentDs,
        VmcsSegmentFs, VmcsSegmentGs, VmcsSegmentTr
    };
    ULONG i;
    for (i = 0; i < VmcsSegmentCount - 1; ++i) {
        // RPL and TI are cleared so each selector is a plain descriptor index.
        // The kernel selectors are 0x10/0x2B-style GDT values, so masking keeps
        // the descriptor being named (SDM Vol. 3C, host-state selector checks).
        VMCS_WRITE(Cpu, Index, hostSelectorFields[i],
            Segments[hostSelectorSegments[i]].Selector & VMCS_SELECTOR_INDEX_MASK,
            "HOST_SELECTOR");
    }
    // Every host entry executes FXSAVE64, including the startup probes.
    // CR0.TS inherited from Windows would raise #NM before dispatch runs.
    // Guest CR0 is unchanged; LaunchRestore restores the native caller's CR0.
    VMCS_WRITE(Cpu, Index, VMCS_HOST_CR0, Live->Cr0 & ~8ull, "HOST_CR0");
    VMCS_WRITE(Cpu, Index, VMCS_HOST_CR3, Live->Cr3, "HOST_CR3");
    VMCS_WRITE(Cpu, Index, VMCS_HOST_CR4, Live->Cr4, "HOST_CR4");
    VMCS_WRITE(Cpu, Index, VMCS_HOST_FS_BASE, Live->FsBase, "HOST_FS_BASE");
    VMCS_WRITE(Cpu, Index, VMCS_HOST_GS_BASE, Live->GsBase, "HOST_GS_BASE");
    // The TR base is only reachable through its GDT descriptor (SDM Vol. 3A 3.5).
    VMCS_WRITE(Cpu, Index, VMCS_HOST_TR_BASE, Segments[VmcsSegmentTr].Base,
        "HOST_TR_BASE");
    VMCS_WRITE(Cpu, Index, VMCS_HOST_GDTR_BASE, Live->GdtrBase, "HOST_GDTR_BASE");
    VMCS_WRITE(Cpu, Index, VMCS_HOST_IDTR_BASE, Live->IdtrBase, "HOST_IDTR_BASE");
    VMCS_WRITE(Cpu, Index, VMCS_HOST_SYSENTER_CS, Live->SysenterCs, "HOST_SYSENTER_CS");
    VMCS_WRITE(Cpu, Index, VMCS_HOST_SYSENTER_ESP, Live->SysenterEsp,
        "HOST_SYSENTER_ESP");
    VMCS_WRITE(Cpu, Index, VMCS_HOST_SYSENTER_EIP, Live->SysenterEip,
        "HOST_SYSENTER_EIP");
    VMCS_WRITE(Cpu, Index, VMCS_HOST_RSP, (ULONG64)Cpu->HostStackTop, "HOST_RSP");
    VMCS_WRITE(Cpu, Index, VMCS_HOST_RIP, (ULONG64)(ULONG_PTR)HvHostEntryStub,
        "HOST_RIP");
    // Loaded only when the matching VM-exit control is set; the fields mirror the
    // live values so the root's views of PAT/EFER cannot drift (SDM Vol. 3C 26.2).
    VMCS_WRITE(Cpu, Index, VMCS_HOST_PAT, Live->Pat, "HOST_IA32_PAT");
    VMCS_WRITE(Cpu, Index, VMCS_HOST_EFER, Live->Efer, "HOST_IA32_EFER");
    return STATUS_SUCCESS;
}

// Populates one core's VMCS. Must run on the core whose VMCS is current.
static NTSTATUS VmcsPopulateCore(PPER_CPU_DATA Cpu, ULONG Index)
{
    const VMX_CAPABILITIES *caps = &Cpu->Capabilities;
    VMCS_LIVE_STATE live;
    VMCS_SEGMENT_STATE segments[VmcsSegmentCount];
    ULONG pin;
    ULONG primary;
    ULONG secondary;
    ULONG exitControls;
    ULONG entryControls;
    ULONG i;
    NTSTATUS status;

    RtlZeroMemory(&live, sizeof(live));
    RtlZeroMemory(segments, sizeof(segments));
    status = VmcsCaptureLiveState(&live);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    for (i = 0; i < VmcsSegmentCount; ++i) {
        status = VmcsDecodeSegment(live.GdtrBase, live.GdtrLimit,
            live.Selector[i], &segments[i]);
        if (!NT_SUCCESS(status)) {
            HV_ERROR("VMCS: core %lu segment %lu (selector 0x%04lX) not modelled\n",
                Index, i, live.Selector[i]);
            return status;
        }
        // In 64-bit mode the FS/GS bases come from their MSRs, not from the
        // descriptor (SDM Vol. 3C 26.3.1.2), so the live values win.
        if (i == VmcsSegmentFs) {
            segments[i].Base = live.FsBase;
        } else if (i == VmcsSegmentGs) {
            segments[i].Base = live.GsBase;
        }
    }

    // Only HLT exiting and the two fixed 64-bit bits are requested, plus
    // no interrupt acknowledgement or optional backing allocations.
    // SDM 25.6.2: EPT needs primary bit 31; EPTP was read back at PASSIVE_LEVEL.
    // Secondary activation follows population so this function cannot erase it.
    pin = VmcsAdjustControl(0, caps->Pin);
    primary = VmcsAdjustControl(PRIMARY_HLT_EXITING |
        (Cpu->Eptp != 0 ? PRIMARY_SECONDARY_CONTROLS : 0u), caps->Primary);
    secondary = VmcsAdjustControl(0, caps->Secondary);
    exitControls = VmcsAdjustControl(
        EXIT_HOST_ADDRESS_SPACE_64, caps->Exit);
    entryControls = VmcsAdjustControl(ENTRY_IA32E_MODE_GUEST | ENTRY_LOAD_DEBUG_CONTROLS, caps->Entry);
    Cpu->TransferCet = (live.Cr4 & CR4_CET) != 0 ||
        (entryControls & ENTRY_LOAD_CET_STATE) != 0 ||
        (exitControls & EXIT_LOAD_CET_STATE) != 0;
    if (Cpu->TransferCet) {
        if ((caps->Entry.Allowed1 & ENTRY_LOAD_CET_STATE) == 0 ||
            (caps->Exit.Allowed1 & EXIT_LOAD_CET_STATE) == 0) {
            HV_ERROR("VMCS: core %lu CET active but VMX CET entry/exit controls unavailable\n", Index);
            return STATUS_NOT_SUPPORTED;
        }
        entryControls |= ENTRY_LOAD_CET_STATE;
        exitControls |= EXIT_LOAD_CET_STATE;
        // The leaf launcher fills these immediately before entry, without CALLs
        // between RDSSPQ and VMLAUNCH. Both worlds retain the live CET policy.
        HV_INFO("VMCS: core %lu CET transfer enabled; live SSP captured at launch\n", Index);
    }
    if ((live.Cr4 & VMX_BIT(24)) != 0 ||
        segments[VmcsSegmentLdtr].Selector != 0 ||
        segments[VmcsSegmentTr].Limit != 0x67u ||
        (segments[VmcsSegmentTr].AccessRights & 0xFu) != 11u) {
        HV_ERROR("VMCS: core %lu state rejection: CR4=0x%016I64X CET=%lu PKS=%lu LDTR=0x%04lX TR=0x%04lX TR.limit=0x%08lX TR.AR=0x%08lX\n",
            Index, live.Cr4, (ULONG)((live.Cr4 >> 23) & 1u),
            (ULONG)((live.Cr4 >> 24) & 1u),
            segments[VmcsSegmentLdtr].Selector,
            segments[VmcsSegmentTr].Selector,
            segments[VmcsSegmentTr].Limit,
            segments[VmcsSegmentTr].AccessRights);
        if ((live.Cr4 & VMX_BIT(24)) != 0) {
            HV_ERROR("VMCS: PKS state save/restore is not implemented\n");
        }
        if (segments[VmcsSegmentLdtr].Selector != 0) {
            HV_ERROR("VMCS: non-null LDTR is unsupported by this return path\n");
        }
        if (segments[VmcsSegmentTr].Limit != 0x67u) {
            HV_ERROR("VMCS: TSS limit differs from 0x67; restoring this limit after VM-exit is not implemented\n");
        }
        if ((segments[VmcsSegmentTr].AccessRights & 0xFu) != 11u) {
            HV_ERROR("VMCS: TR descriptor is not a busy 64-bit TSS (type 0xB)\n");
        }
        return STATUS_NOT_SUPPORTED;
    }
    if ((pin & (PIN_EXTERNAL_INTERRUPT_EXITING | PIN_NMI_EXITING |
            PIN_VIRTUAL_NMIS | PIN_PREEMPTION_TIMER_ACTIVATE |
            PIN_PROCESS_POSTED_INTERRUPTS)) != 0 ||
        (primary & (PRIMARY_USE_IO_BITMAPS | PRIMARY_USE_MSR_BITMAPS |
            PRIMARY_TPR_SHADOW)) != 0 ||
        (exitControls & (EXIT_LOAD_IA32_PERF_GLOBAL_CTRL |
            EXIT_ACKNOWLEDGE_INTERRUPT)) != 0 ||
        (entryControls & ENTRY_LOAD_IA32_PERF_GLOBAL_CTRL) != 0) {
        HV_ERROR("VMCS: core %lu requires resources outside the one-shot test\n", Index);
        return STATUS_NOT_SUPPORTED;
    }
    if ((entryControls & ENTRY_LOAD_DEBUG_CONTROLS) == 0 ||
        (entryControls & ENTRY_IA32E_MODE_GUEST) == 0 ||
        (exitControls & EXIT_HOST_ADDRESS_SPACE_64) == 0 ||
        (primary & PRIMARY_SECONDARY_CONTROLS) !=
            (Cpu->Eptp != 0 ? PRIMARY_SECONDARY_CONTROLS : 0u) ||
        (secondary & ~SECONDARY_ENABLE_EPT) != 0 ||
        (Cpu->Eptp == 0 && (secondary & SECONDARY_ENABLE_EPT) != 0)) {
        HV_ERROR("VMCS: core %lu cannot form the required controls: primary=0x%08lX exit=0x%08lX entry=0x%08lX\n",
            Index, primary, exitControls, entryControls);
        return STATUS_NOT_SUPPORTED;
    }
    if ((primary & PRIMARY_HLT_EXITING) == 0) {
        HV_INFO("VMCS: core %lu reports no HLT-exiting; a guest HLT would halt the core\n",
            Index);
    }
    HV_INFO("VMCS: core %lu controls pin=0x%08lX primary=0x%08lX secondary=0x%08lX exit=0x%08lX entry=0x%08lX\n",
        Index, pin, primary, secondary, exitControls, entryControls);

    status = VmcsWriteConstantFields(Cpu, Index);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = VmcsWriteControlFields(Cpu, Index, &live, pin, primary, secondary,
        exitControls, entryControls);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = VmcsWriteGuestFields(Cpu, Index, segments, &live);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = VmcsWriteHostFields(Cpu, Index, segments, &live);
    if (!NT_SUCCESS(status)) { return status; }
    status = Cpu->Eptp != 0 ? EptEnableInVmcs(Cpu) : STATUS_SUCCESS;
    if (NT_SUCCESS(status) && Cpu->Persist.Preparing) {
        status = VmcsConfigureRuntime(Cpu, Index);
    }
    return status;
}

static NTSTATUS VmcsConfigureRuntime(PPER_CPU_DATA Cpu, ULONG Index)
{
    VMX_CAPABILITIES *caps = &Cpu->Capabilities;
    ULONG primary, secondary = SECONDARY_ENABLE_EPT, pin, exitControls;
    NTSTATUS status;
    ULONG forbidden = PRIMARY_CR3_LOAD_EXITING | PRIMARY_CR3_STORE_EXITING |
        PRIMARY_CR8_LOAD_EXITING | PRIMARY_CR8_STORE_EXITING | PRIMARY_MOV_DR_EXITING |
        PRIMARY_UNCONDITIONAL_IO_EXITING | PRIMARY_USE_IO_BITMAPS |
        PRIMARY_INVLPG_EXITING | PRIMARY_RDPMC_EXITING | PRIMARY_RDTSC_EXITING |
        PRIMARY_INTERRUPT_WINDOW_EXITING | PRIMARY_NMI_WINDOW_EXITING |
        PRIMARY_TPR_SHADOW | PRIMARY_MONITOR_TRAP_FLAG | PRIMARY_PAUSE_EXITING |
        PRIMARY_MONITOR_EXITING | (1u << 10); // MWAIT exiting
    int regs[4];
    if ((caps->Primary.Allowed1 & (PRIMARY_USE_MSR_BITMAPS | PRIMARY_NMI_WINDOW_EXITING)) !=
            (PRIMARY_USE_MSR_BITMAPS | PRIMARY_NMI_WINDOW_EXITING) ||
        (caps->Primary.Allowed0 & forbidden) != 0) {
        return STATUS_NOT_SUPPORTED;
    }
    primary = VmcsAdjustControl(PRIMARY_SECONDARY_CONTROLS | PRIMARY_USE_MSR_BITMAPS |
        PRIMARY_USE_TSC_OFFSETTING,
        caps->Primary);
    pin = VmcsAdjustControl(PIN_NMI_EXITING | PIN_VIRTUAL_NMIS, caps->Pin);
    if ((pin & (PIN_NMI_EXITING | PIN_VIRTUAL_NMIS)) !=
        (PIN_NMI_EXITING | PIN_VIRTUAL_NMIS)) { return STATUS_NOT_SUPPORTED; }
    __cpuidex(regs, (int)0x80000001u, 0);
    if ((regs[3] & (1u << 27)) != 0) { secondary |= SECONDARY_RDTSCP; }
    __cpuidex(regs, 7, 0);
    if ((regs[1] & (1u << 10)) != 0) { secondary |= 1u << 12; } // INVPCID
    if ((regs[2] & (1u << 22)) != 0) { secondary |= SECONDARY_RDTSCP; } // RDPID
    if ((regs[2] & (1u << 5)) != 0) { secondary |= 1u << 26; } // WAITPKG
    __cpuidex(regs, 0xD, 1);
    if ((regs[0] & (1u << 3)) != 0) { secondary |= 1u << 20; } // XSAVES
    if (Cpu->Tsc.ScalingEnabled) { secondary |= SECONDARY_TSC_SCALING; }
    status = VpidPrepareCurrent(Cpu, Index, &secondary);
    if (!NT_SUCCESS(status)) { return status; }
    if ((secondary & ~caps->Secondary.Allowed1) != 0) { return STATUS_NOT_SUPPORTED; }
    secondary = VmcsAdjustControl(secondary, caps->Secondary);
    exitControls = VmcsAdjustControl(EXIT_HOST_ADDRESS_SPACE_64 | EXIT_SAVE_DEBUG_CONTROLS |
        (Cpu->TransferCet ? EXIT_LOAD_CET_STATE : 0), caps->Exit);
    if ((exitControls & EXIT_SAVE_DEBUG_CONTROLS) == 0) { return STATUS_NOT_SUPPORTED; }
    VMCS_WRITE(Cpu, Index, VMCS_PIN_BASED_CONTROLS, pin, "runtime PIN");
    VMCS_WRITE(Cpu, Index, VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS, primary, "runtime PRIMARY");
    VMCS_WRITE(Cpu, Index, VMCS_SECONDARY_PROCESSOR_BASED_CONTROLS, secondary, "runtime SECONDARY");
    VMCS_WRITE(Cpu, Index, VMCS_EXIT_CONTROLS, exitControls, "runtime EXIT");
    VMCS_WRITE(Cpu, Index, VMCS_MSR_BITMAP,
        (ULONG64)MmGetPhysicalAddress(Cpu->Persist.MsrBitmap).QuadPart, "runtime MSR bitmap");
    VMCS_WRITE(Cpu, Index, VMCS_EXCEPTION_BITMAP, (1u << 6), "runtime exceptions (#UD entry traps)");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_DR7, __readdr(7), "runtime DR7");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_DEBUGCTL, __readmsr(MSR_IA32_DEBUGCTL), "runtime DEBUGCTL");
    VMCS_WRITE(Cpu, Index, VMCS_GUEST_RIP,
        (ULONG64)(ULONG_PTR)HvPersistentGuestEntry, "runtime RIP");
    VMCS_WRITE(Cpu, Index, VMCS_CR4_GUEST_HOST_MASK, CR4_VMXE, "runtime CR4 mask");
    VMCS_WRITE(Cpu, Index, VMCS_CR4_READ_SHADOW, __readcr4() & ~CR4_VMXE, "runtime CR4 shadow");
    VMCS_WRITE(Cpu, Index, VMCS_HOST_CR0, __readcr0() & ~8ull, "runtime host TS");
    VMCS_WRITE(Cpu, Index, VMCS_HOST_CR4, __readcr4() & ~CR4_CET, "runtime host CET");
    VMCS_WRITE(Cpu, Index, VMCS_HOST_IDTR_BASE,
        (ULONG64)(ULONG_PTR)Cpu->Persist.RootIdt, "runtime IDT");
    if ((secondary & (1u << 20)) != 0) {
        VMCS_WRITE(Cpu, Index, 0x202C, 0, "XSS exit bitmap");
    }
    HV_INFO("VMCS: core %lu runtime pin=%08lX primary=%08lX secondary=%08lX\n",
        Index, pin, primary, secondary);
    // Only the persistent path reaches this function. Identity timing fields
    // are written/read back after controls are configured and before VMLAUNCH.
    return TscWriteVmcs(Cpu);
}

// Capture and populate immediately before launch while affinity is pinned and
// scheduling is disabled. IA-32e paging uses the live CR3; no PAE PDPTE workaround.
NTSTATUS VmcsPopulateCurrent(PPER_CPU_DATA Cpu, ULONG Index)
{
    return VmcsPopulateCore(Cpu, Index);
}
