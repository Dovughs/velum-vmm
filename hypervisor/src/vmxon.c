// hypervisor/src/vmxon.c
#include "common.h"
#include "persist.h"

#pragma code_seg(push, ".text")

static VMX_CONTROL_MASKS VmxReadControl(ULONG Msr)
{
    ULONG64 value = __readmsr(Msr);
    VMX_CONTROL_MASKS masks;
    masks.Allowed0 = (ULONG)value;
    masks.Allowed1 = (ULONG)(value >> 32);
    return masks;
}

static NTSTATUS VmxDetectCurrent(VMX_CAPABILITIES *Caps)
{
    int registers[4];
    ULONG64 cr0;
    ULONG64 cr4;
    __cpuid(registers, 0);
    if (registers[0] < 1 || registers[1] != 0x756e6547 ||
        registers[3] != 0x49656e69 || registers[2] != 0x6c65746e) {
        return STATUS_NOT_SUPPORTED;
    }
    __cpuid(registers, 1);
    if (((ULONG)registers[2] & (1u << 5)) == 0) {
        return STATUS_NOT_SUPPORTED;
    }
    __try {
        Caps->Basic = __readmsr(MSR_IA32_VMX_BASIC);
        Caps->RevisionId = (ULONG)(Caps->Basic & 0x7fffffffull);
        Caps->RegionSize = (ULONG)((Caps->Basic >> 32) & 0x1fffull);
        Caps->DualMonitor = (Caps->Basic & VMX_BIT(49)) != 0;
        Caps->MemoryType = (UCHAR)((Caps->Basic >> 50) & 0xfull);
        Caps->TrueControls = (Caps->Basic & VMX_BIT(55)) != 0;
        Caps->Pin = VmxReadControl(MSR_IA32_VMX_PINBASED_CTLS);
        Caps->Primary = VmxReadControl(MSR_IA32_VMX_PROCBASED_CTLS);
        Caps->Exit = VmxReadControl(MSR_IA32_VMX_EXIT_CTLS);
        Caps->Entry = VmxReadControl(MSR_IA32_VMX_ENTRY_CTLS);
        // IA32_VMX_BASIC[55] (TrueControls) selects the "TRUE" capability MSRs for
        // the pin, primary, exit and entry controls. The legacy MSRs report bits as
        // mandatory that the TRUE MSRs show are actually optional (SDM Vol. 3D
        // A.1); using only the legacy set produces bloated control fields that can
        // fail VMLAUNCH with VM_INSTRUCTION_ERROR=7. The secondary controls
        // (IA32_VMX_PROCBASED_CTLS2) have no TRUE variant, so their masks are
        // always read from the legacy MSR in the block below.
        if (Caps->TrueControls) {
            Caps->Pin = VmxReadControl(MSR_IA32_VMX_TRUE_PINBASED_CTLS);
            Caps->Primary = VmxReadControl(MSR_IA32_VMX_TRUE_PROCBASED_CTLS);
            Caps->Exit = VmxReadControl(MSR_IA32_VMX_TRUE_EXIT_CTLS);
            Caps->Entry = VmxReadControl(MSR_IA32_VMX_TRUE_ENTRY_CTLS);
            HV_INFO("[VMX] TrueControls=1: using TRUE control MSRs for pin/primary/exit/entry masks\n");
        } else {
            HV_INFO("[VMX] TrueControls=0: using legacy control MSRs\n");
        }
        Caps->Misc = __readmsr(MSR_IA32_VMX_MISC);
        Caps->PreemptionTimerRate = (UCHAR)(Caps->Misc & 0x1full);
        Caps->HltActivityState = (Caps->Misc & VMX_BIT(6)) != 0;
        Caps->Cr0Fixed0 = __readmsr(MSR_IA32_VMX_CR0_FIXED0);
        Caps->Cr0Fixed1 = __readmsr(MSR_IA32_VMX_CR0_FIXED1);
        Caps->Cr4Fixed0 = __readmsr(MSR_IA32_VMX_CR4_FIXED0);
        Caps->Cr4Fixed1 = __readmsr(MSR_IA32_VMX_CR4_FIXED1);
        Caps->VmcsEnum = __readmsr(MSR_IA32_VMX_VMCS_ENUM);
        Caps->FeatureControl = __readmsr(MSR_IA32_FEATURE_CONTROL);
        if ((Caps->Primary.Allowed1 & PRIMARY_SECONDARY_CONTROLS) != 0) {
            Caps->Secondary = VmxReadControl(MSR_IA32_VMX_PROCBASED_CTLS2);
            Caps->EptSupported =
                (Caps->Secondary.Allowed1 & SECONDARY_ENABLE_EPT) != 0;
        }
        if (Caps->EptSupported) {
            Caps->EptVpidCap = __readmsr(MSR_IA32_VMX_EPT_VPID_CAP);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return STATUS_NOT_SUPPORTED;
    }
    Caps->EptWalk4 = (Caps->EptVpidCap & EPT_WALK_LENGTH_4) != 0;
    // EPT supports 4 KB leaves implicitly; no dedicated capability bit exists.
    Caps->Ept4KbPages = Caps->EptSupported;
    Caps->EptUc = (Caps->EptVpidCap & EPT_MEMORY_UC) != 0;
    Caps->EptWb = (Caps->EptVpidCap & EPT_MEMORY_WB) != 0;
    Caps->Invept = (Caps->EptVpidCap & EPT_INVEPT) != 0;
    Caps->InveptSingle = (Caps->EptVpidCap & EPT_INVEPT_SINGLE) != 0;
    Caps->InveptAll = (Caps->EptVpidCap & EPT_INVEPT_ALL) != 0;
    if ((Caps->FeatureControl & FEATURE_LOCK) != 0 &&
        (Caps->FeatureControl & FEATURE_VMX_OUTSIDE_SMX) == 0) {
        return STATUS_ACCESS_DISABLED_BY_POLICY_DEFAULT;
    }
    if (!Caps->EptSupported || !Caps->EptUc || !Caps->EptWalk4 ||
        Caps->RegionSize == 0 || Caps->RegionSize > PAGE_SIZE ||
        Caps->MemoryType != VMX_MEMORY_WB) {
        return STATUS_NOT_SUPPORTED;
    }
    cr0 = __readcr0();
    cr4 = __readcr4();
    if ((cr4 & CR4_VMXE) != 0) {
        return STATUS_DEVICE_BUSY;
    }
    cr4 |= CR4_VMXE;
    if ((cr0 & Caps->Cr0Fixed0) != Caps->Cr0Fixed0 ||
        (cr0 & ~Caps->Cr0Fixed1) != 0 ||
        (cr4 & Caps->Cr4Fixed0) != Caps->Cr4Fixed0 ||
        (cr4 & ~Caps->Cr4Fixed1) != 0) {
        return STATUS_NOT_SUPPORTED;
    }
    return STATUS_SUCCESS;
}

NTSTATUS VmxDetectAll(VOID)
{
    ULONG i;
    for (i = 0; i < g_CpuCount; ++i) {
        GROUP_AFFINITY target = { 0 };
        GROUP_AFFINITY previous;
        NTSTATUS status;
        target.Mask = (KAFFINITY)1 << i;
        KeSetSystemGroupAffinityThread(&target, &previous);
        status = VmxDetectCurrent(&g_PerCpuData[i]->Capabilities);
        KeRevertToUserGroupAffinityThread(&previous);
        if (!NT_SUCCESS(status)) {
            if (status == STATUS_ACCESS_DISABLED_BY_POLICY_DEFAULT) {
                HV_ERROR("VMX: core %lu FEATURE_CONTROL locked; outside-SMX VMX disabled\n", i);
                return STATUS_NOT_SUPPORTED;
            }
            HV_ERROR("VMX: core %lu capability rejection 0x%08lX; EPT=%u UC=%u\n",
                i, (ULONG)status, (unsigned int)g_PerCpuData[i]->Capabilities.EptSupported,
                (unsigned int)g_PerCpuData[i]->Capabilities.EptUc);
            return status;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS VmxConfigureFeatureControl(VOID)
{
    ULONG i;
    for (i = 0; i < g_CpuCount; ++i) {
        GROUP_AFFINITY target = { 0 };
        GROUP_AFFINITY previous;
        NTSTATUS status = STATUS_SUCCESS;
        ULONG64 value;
        target.Mask = (KAFFINITY)1 << i;
        KeSetSystemGroupAffinityThread(&target, &previous);
        __try {
            value = __readmsr(MSR_IA32_FEATURE_CONTROL);
            if ((value & FEATURE_LOCK) == 0) {
                value |= FEATURE_VMX_OUTSIDE_SMX;
                __writemsr(MSR_IA32_FEATURE_CONTROL, value);
                __writemsr(MSR_IA32_FEATURE_CONTROL, value | FEATURE_LOCK);
            }
            value = __readmsr(MSR_IA32_FEATURE_CONTROL);
            g_PerCpuData[i]->Capabilities.FeatureControl = value;
            if ((value & (FEATURE_LOCK | FEATURE_VMX_OUTSIDE_SMX)) !=
                (FEATURE_LOCK | FEATURE_VMX_OUTSIDE_SMX)) {
                status = STATUS_NOT_SUPPORTED;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            status = STATUS_NOT_SUPPORTED;
        }
        KeRevertToUserGroupAffinityThread(&previous);
        if (!NT_SUCCESS(status)) {
            HV_ERROR("VMX: core %lu FEATURE_CONTROL programming failed\n", i);
            return status;
        }
    }
    return STATUS_SUCCESS;
}

// IPI_LEVEL: .text and globals are resident; touch only preallocated nonpaged
// state. No allocation, logging, exceptions, waits, or pageable callees here.
ULONG_PTR VmxonIpiCallback(ULONG_PTR Context)
{
    ULONG index = KeGetCurrentProcessorNumber();
    PPER_CPU_DATA cpu;
    VMX_CAPABILITIES *caps;
    ULONG64 value;
    ULONG64 cr0;
    ULONG64 cr4;
    ULONG64 physical;
    UNREFERENCED_PARAMETER(Context);
    if (index >= g_CpuCount || g_PerCpuData[index] == NULL) {
        InterlockedIncrement(&g_VmxFailureCount);
        return 0;
    }
    cpu = g_PerCpuData[index];
    caps = &cpu->Capabilities;
    // Configuration cached the locked MSR; it cannot change before reset.
    value = caps->FeatureControl;
    if ((value & (FEATURE_LOCK | FEATURE_VMX_OUTSIDE_SMX)) !=
        (FEATURE_LOCK | FEATURE_VMX_OUTSIDE_SMX)) {
        cpu->StartResult = VmxFeatureDisabled;
        goto Failed;
    }
    cr4 = __readcr4();
    cpu->SavedCr4 = cr4;
    if ((cr4 & CR4_VMXE) != 0) {
        cpu->StartResult = VmxAlreadyOwned;
        goto Failed;
    }
    cr0 = __readcr0();
    cr4 |= CR4_VMXE;
    if ((cr0 & caps->Cr0Fixed0) != caps->Cr0Fixed0 ||
        (cr0 & ~caps->Cr0Fixed1) != 0 ||
        (cr4 & caps->Cr4Fixed0) != caps->Cr4Fixed0 ||
        (cr4 & ~caps->Cr4Fixed1) != 0) {
        cpu->StartResult = VmxInvalidFixedBits;
        goto Failed;
    }
    // Own only VMXE. A failed VMXON restores CR4 immediately on this CPU.
    __writecr4(cr4);
    physical = (ULONG64)cpu->VmxonPa.QuadPart;
    cpu->VmxInstructionStatus = VmxOn(physical);
    if (cpu->VmxInstructionStatus != 0) {
        __writecr4(cpu->SavedCr4);
        cpu->StartResult = VmxInstructionFailed;
        goto Failed;
    }
    cpu->VmxEnabled = 1;
    cpu->StartResult = VmxStarted;
    return 0;
Failed:
    InterlockedIncrement(&g_VmxFailureCount);
    return 0;
}

ULONG_PTR VmxoffIpiCallback(ULONG_PTR Context)
{
    ULONG index = KeGetCurrentProcessorNumber();
    PPER_CPU_DATA cpu;
    ULONG64 cr4;
    UNREFERENCED_PARAMETER(Context);
    if (index >= g_CpuCount || g_PerCpuData[index] == NULL) {
        return 0;
    }
    cpu = g_PerCpuData[index];
    if (cpu->VmxEnabled != 0) {
        // Rollback of a prepared/failed launch: no guest re-entry follows.
        // Persistent shutdown retires its context in PersistPrepareNative.
        if (!NT_SUCCESS(VpidInvalidateCurrent(cpu))) { PersistFatal(cpu, 0x10C); }
        (VOID)VmxOff();
        cpu->Vpid.Enabled = FALSE;
        // Restore only our VMXE change, preserving other live OS CR4 bits.
        cr4 = __readcr4();
        __writecr4((cr4 & ~CR4_VMXE) | (cpu->SavedCr4 & CR4_VMXE));
        cpu->VmxEnabled = 0;
        cpu->VmcsActive = 0;
    }
    return 0;
}

VOID VmxStopAll(VOID)
{
    if (g_CpuCount != 0) {
        // KeIpiGenericCall returns only after every callback has completed.
        (VOID)KeIpiGenericCall(VmxoffIpiCallback, 0);
    }
}

#pragma code_seg(pop)
