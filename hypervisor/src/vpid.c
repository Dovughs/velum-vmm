// hypervisor/src/vpid.c
#include "common.h"
#include "vmcs.h"
#include "vpid.h"

// Intel SDM IA32_VMX_EPT_VPID_CAP: instruction support and supported scopes.
#define VPID_INVVPID_SUPPORTED (1ull << 32)
#define VPID_INVVPID_SINGLE    (1ull << 41)
#define VPID_INVVPID_ALL       (1ull << 42)

NTSTATUS VpidInvalidateCurrent(PPER_CPU_DATA Cpu)
{
    VPID_STATE *state = &Cpu->Vpid;
    if (!state->Enabled) { return STATUS_SUCCESS; }
    state->LastInstructionStatus = VpidInvvpid(state->InvalidationType,
        state->InvalidationType == 1 ? state->Id : 0);
    ++state->Invalidations;
    return state->LastInstructionStatus == 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

NTSTATUS VpidPrepareCurrent(PPER_CPU_DATA Cpu, ULONG Index, PULONG SecondaryControls)
{
    VPID_STATE *state = &Cpu->Vpid;
    const VMX_CAPABILITIES *caps = &Cpu->Capabilities;
    ULONG64 features = caps->EptVpidCap;
    NTSTATUS status;

    // Never silently recycle an active context. Current topology is fixed for
    // the driver's lifetime (one processor group, no vCPU migration/hot-add).
    if (state->Enabled) { return STATUS_INVALID_DEVICE_STATE; }
    *SecondaryControls &= ~SECONDARY_ENABLE_VPID;
    if ((caps->Secondary.Allowed1 & SECONDARY_ENABLE_VPID) == 0 ||
        (features & VPID_INVVPID_SUPPORTED) == 0 ||
        (features & (VPID_INVVPID_SINGLE | VPID_INVVPID_ALL)) == 0) {
        HV_INFO("VPID: core %lu disabled (required capability unavailable)\n", Index);
        return (caps->Secondary.Allowed0 & SECONDARY_ENABLE_VPID) != 0 ?
            STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
    }
    if (Index >= 0xFFFFu) { return STATUS_NOT_SUPPORTED; }
    state->Id = (USHORT)(Index + 1);
    state->InvalidationType = (features & VPID_INVVPID_SINGLE) != 0 ? 1 : 2;
    state->Enabled = TRUE;
    // VMCLEAR/VMXOFF must not be assumed to retire tagged translations. Flush
    // before FIRST use too, covering driver reload and an earlier VMX owner.
    status = VpidInvalidateCurrent(Cpu);
    if (!NT_SUCCESS(status)) {
        state->Enabled = FALSE;
        HV_INFO("VPID: core %lu disabled (initial INVVPID failed: %lu)\n",
            Index, state->LastInstructionStatus);
        return (caps->Secondary.Allowed0 & SECONDARY_ENABLE_VPID) != 0 ?
            status : STATUS_SUCCESS;
    }
    if (VmxWrite(VMCS_VPID, state->Id) != 0) {
        return STATUS_UNSUCCESSFUL;
    }
    *SecondaryControls |= SECONDARY_ENABLE_VPID;
    HV_INFO("VPID: core %lu id=%u INVVPID type=%u; private EPT retained\n",
        Index, (unsigned int)state->Id, (unsigned int)state->InvalidationType);
    return STATUS_SUCCESS;
}

NTSTATUS VpidDeactivateCurrent(PPER_CPU_DATA Cpu)
{
    ULONG64 secondary;
    NTSTATUS status;
    if (!Cpu->Vpid.Enabled) { return STATUS_SUCCESS; }
    if (VmxRead(VMCS_SECONDARY_PROCESSOR_BASED_CONTROLS, &secondary) != 0 ||
        VmxWrite(VMCS_SECONDARY_PROCESSOR_BASED_CONTROLS,
            secondary & ~(ULONG64)SECONDARY_ENABLE_VPID) != 0) {
        return STATUS_UNSUCCESSFUL;
    }
    status = VpidInvalidateCurrent(Cpu);
    if (NT_SUCCESS(status)) { Cpu->Vpid.Enabled = FALSE; }
    return status;
}
