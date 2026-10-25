// hypervisor/src/tsc.c
#include "common.h"
#include "vmcs.h"
#include "tsc.h"

_Static_assert(TSC_MULTIPLIER_ONE == 0x0001000000000000ull,
    "TSC multiplier uses 48 fractional bits");

NTSTATUS TscInitializeForCore(PPER_CPU_DATA Cpu)
{
    TSC_STATE *state = &Cpu->Tsc;
    int regs[4];
    ULONG i;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || state->Initialized ||
        Cpu->Persist.Launched) { return STATUS_INVALID_DEVICE_STATE; }
    if ((Cpu->Capabilities.Primary.Allowed1 &
        (PRIMARY_USE_TSC_OFFSETTING | PRIMARY_SECONDARY_CONTROLS)) !=
        (PRIMARY_USE_TSC_OFFSETTING | PRIMARY_SECONDARY_CONTROLS)) {
        return STATUS_NOT_SUPPORTED;
    }
    state->OwnerIndex = KeGetCurrentProcessorNumber();
    // Diagnostic only. No subtraction or per-core epoch reset. In nested VMX
    // this is the clock exposed by the outer VMM, not necessarily physical TSC.
    state->HostTscAtInitialize = __rdtsc();
    state->Offset = 0;
    __cpuidex(regs, 1, 0);
    state->DeadlineSupported = ((ULONG)regs[2] & (1u << 24)) != 0;
    state->ScalingEnabled =
        (Cpu->Capabilities.Secondary.Allowed1 & SECONDARY_TSC_SCALING) != 0;
    state->Multiplier = state->ScalingEnabled ? TSC_MULTIPLIER_ONE : 0;
    __cpuidex(regs, 0, 0);
    state->MaxBasicLeaf = (ULONG)regs[0];
    if (state->MaxBasicLeaf >= 0x15) {
        __cpuidex(regs, 0x15, 0);
        for (i = 0; i < 4; ++i) { state->Leaf15[i] = (ULONG)regs[i]; }
        // Full 32-bit factors fit in a 64-bit product. Zero crystal/ratio is
        // unknown, never filled in from leaf 16 or a guessed CPU-model table.
        if (state->Leaf15[0] && state->Leaf15[1] && state->Leaf15[2]) {
            state->EnumeratedTscHz =
                ((ULONG64)state->Leaf15[2] * state->Leaf15[1]) / state->Leaf15[0];
        }
    }
    if (state->MaxBasicLeaf >= 0x16) {
        __cpuidex(regs, 0x16, 0);
        for (i = 0; i < 4; ++i) { state->Leaf16[i] = (ULONG)regs[i]; }
    }
    state->Initialized = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS TscWriteChecked(PPER_CPU_DATA Cpu, ULONG Field, ULONG64 Value)
{
    ULONG64 actual;
    Cpu->Tsc.LastVmcsField = Field;
    if (VmxWrite(Field, Value) != 0 || VmxRead(Field, &actual) != 0 ||
        actual != Value) { return STATUS_UNSUCCESSFUL; }
    return STATUS_SUCCESS;
}

#define TSC_DEADLINE_MSR 0x6E0u

// Only the owning core edits its bitmap, while running in VMX root. Preserve
// all unrelated interception bits. RDTSC/RDTSCP/RDMSR(TSC) remain in hardware.
static VOID TscDeadlineIntercept(PPER_CPU_DATA Cpu, BOOLEAN Enable)
{
    UCHAR *bitmap = Cpu->Persist.MsrBitmap;
    ULONG byte = TSC_DEADLINE_MSR / 8;
    UCHAR bit = (UCHAR)(1u << (TSC_DEADLINE_MSR % 8));
    if (Enable) { bitmap[byte] |= bit; bitmap[2048 + byte] |= bit; }
    else { bitmap[byte] &= (UCHAR)~bit; bitmap[2048 + byte] &= (UCHAR)~bit; }
}

NTSTATUS TscSetOffsetCurrent(PPER_CPU_DATA Cpu, BOOLEAN Enabled, LONG64 Ticks)
{
    TSC_STATE *state = &Cpu->Tsc;
    LONG64 next = Enabled ? Ticks : 0;
    ULONG64 primary, now, deadline = 0, guestDeadline = 0;
    NTSTATUS status;
    if (!state->Initialized || !Cpu->Persist.Launched ||
        Ticks < -HV_TSC_OFFSET_LIMIT || Ticks > HV_TSC_OFFSET_LIMIT ||
        (state->ScalingEnabled && state->Multiplier != TSC_MULTIPLIER_ONE)) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (next == state->Offset) { return STATUS_SUCCESS; }
    now = __rdtsc();
    if ((next < 0 && now <= 0ull - (ULONG64)next) ||
        (next > 0 && now > ~0ull - (ULONG64)next)) { return STATUS_INVALID_PARAMETER; }
    if (VmxRead(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS, &primary) != 0 ||
        !(primary & PRIMARY_USE_TSC_OFFSETTING)) { return STATUS_INVALID_DEVICE_STATE; }

    if (state->DeadlineSupported) {
        deadline = __readmsr(TSC_DEADLINE_MSR);
        if (deadline) {
            guestDeadline = state->Offset ? state->DeadlineGuest : deadline;
        }
    }
    status = TscWriteChecked(Cpu, VMCS_TSC_OFFSET, (ULONG64)next);
    if (!NT_SUCCESS(status)) {
        // Restore locally even when a write succeeded but readback failed.
        if (!NT_SUCCESS(TscWriteChecked(Cpu, VMCS_TSC_OFFSET, (ULONG64)state->Offset))) {
            KeBugCheckEx(0x20001, 0x311, (ULONG_PTR)Cpu, 0, 0);
        }
        return status;
    }
    if (state->DeadlineSupported) {
        state->DeadlineGuest = guestDeadline;
        // Rebase an armed deadline into the new clock domain. A deadline that
        // expired during the read/write window can produce an extra timer IRQ.
        if (deadline) {
            __writemsr(TSC_DEADLINE_MSR, HvDeadlineTranslate(guestDeadline, -next));
        }
        TscDeadlineIntercept(Cpu, next != 0);
    }
    state->Offset = next;
    return STATUS_SUCCESS;
}

BOOLEAN TscDeadlineMsr(PPER_CPU_DATA Cpu, BOOLEAN Write, ULONG Msr, ULONG64 *Value)
{
    TSC_STATE *state = &Cpu->Tsc;
    if (Msr != TSC_DEADLINE_MSR || !state->DeadlineSupported || !state->Offset) {
        return FALSE;
    }
    if (Write) {
        state->DeadlineGuest = *Value;
        __writemsr(TSC_DEADLINE_MSR, HvDeadlineTranslate(*Value, -state->Offset));
    } else {
        *Value = __readmsr(TSC_DEADLINE_MSR) ? state->DeadlineGuest : 0;
    }
    return TRUE;
}

NTSTATUS TscWriteVmcs(PPER_CPU_DATA Cpu)
{
    TSC_STATE *state = &Cpu->Tsc;
    ULONG64 primary, secondary;
    NTSTATUS status;
    if (!state->Initialized || !Cpu->Persist.Preparing || Cpu->Persist.Launched ||
        state->Offset != 0 ||
        state->Multiplier != (state->ScalingEnabled ? TSC_MULTIPLIER_ONE : 0)) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    // Read the actual configured controls. Never enable instruction exiting.
    state->LastVmcsField = VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS;
    if (VmxRead(state->LastVmcsField, &primary) != 0) { return STATUS_UNSUCCESSFUL; }
    if ((primary & (PRIMARY_USE_TSC_OFFSETTING | PRIMARY_SECONDARY_CONTROLS)) !=
        (PRIMARY_USE_TSC_OFFSETTING | PRIMARY_SECONDARY_CONTROLS) ||
        (primary & (PRIMARY_RDTSC_EXITING | PRIMARY_RDPMC_EXITING)) != 0) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    state->LastVmcsField = VMCS_SECONDARY_PROCESSOR_BASED_CONTROLS;
    if (VmxRead(state->LastVmcsField, &secondary) != 0) { return STATUS_UNSUCCESSFUL; }
    if (((secondary & SECONDARY_TSC_SCALING) != 0) != state->ScalingEnabled) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    // SDM: guest = low64((host * multiplier) >> 48) + offset. Identity leaves
    // reads and TSC-deadline programming on the same timeline. Full-width x64
    // VMWRITE/VMREAD access only the low encoding; no HIGH-field write is needed.
    status = TscWriteChecked(Cpu, VMCS_TSC_OFFSET, (ULONG64)state->Offset);
    if (NT_SUCCESS(status) && state->ScalingEnabled) {
        status = TscWriteChecked(Cpu, VMCS_TSC_MULTIPLIER, state->Multiplier);
    }
    if (NT_SUCCESS(status)) { state->LastVmcsField = 0; }
    return status;
}

VOID TscLogState(PPER_CPU_DATA Cpu)
{
    const TSC_STATE *state = &Cpu->Tsc;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !state->Initialized) { return; }
    HV_INFO("TSC: core %lu offset=0x%016I64X scaling=%s multiplier=0x%016I64X sample_tsc=0x%016I64X\n",
        state->OwnerIndex, (ULONG64)state->Offset,
        state->ScalingEnabled ? "on" : "off", state->Multiplier, state->HostTscAtInitialize);
    // SDM Vol. 2A CPUID: leaf 16 is display information, NOT a TSC calibration
    // source. Report both independently; no equality assertion or guest spoof.
    HV_INFO("TSC: core %lu CPUID max=%lX 15h=%08lX/%08lX/%08lX/%08lX tsc_hz=%I64u (0=unknown) 16h=%08lX/%08lX/%08lX/%08lX\n",
        state->OwnerIndex, state->MaxBasicLeaf,
        state->Leaf15[0], state->Leaf15[1], state->Leaf15[2], state->Leaf15[3],
        state->EnumeratedTscHz, state->Leaf16[0], state->Leaf16[1],
        state->Leaf16[2], state->Leaf16[3]);
    if (state->MaxBasicLeaf >= 0x15 && state->Leaf15[1] != 0 && state->Leaf15[0] == 0) {
        HV_INFO("TSC: core %lu CPUID 15h ratio has zero denominator; values retained unchanged\n",
            state->OwnerIndex);
    }
}
