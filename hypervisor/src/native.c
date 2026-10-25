#include "common.h"
#include "vmcs.h"
#include "persist.h"

const ULONG g_NativeOffset = (ULONG)FIELD_OFFSET(PER_CPU_DATA, Native);

static ULONG64 ReadField(PPER_CPU_DATA Cpu, ULONG Field)
{
    ULONG64 value = 0;
    if (VmxRead(Field, &value) != 0) { PersistFatal(Cpu, Field); }
    return value;
}

static VOID WriteField(PPER_CPU_DATA Cpu, ULONG Field, ULONG64 Value)
{
    if (VmxWrite(Field, Value) != 0) { PersistFatal(Cpu, Field); }
}

ULONG64 PersistPrepareNative(PPER_CPU_DATA Cpu)
{
    HV_NATIVE_STATE *s = &Cpu->Native;
    ULONG64 value;
    // Retire the VPID while still using the private root state. The final
    // VMRESUME/VMCALL handshake runs with tagging disabled and cannot refill it.
    if (!NT_SUCCESS(VpidDeactivateCurrent(Cpu))) { PersistFatal(Cpu, 0x10B); }
    // This request is accepted only from our kernel leaf stub at IPI_LEVEL.
    // CPUID -> VMCALL contains no CALL/RET and cannot change the guest SSP.
    s->Rip = (ULONG64)(ULONG_PTR)HvShutdownReturn;
    s->Rsp = ReadField(Cpu, VMCS_GUEST_RSP);
    s->Rflags = ReadField(Cpu, VMCS_GUEST_RFLAGS);
    s->Cr0 = ReadField(Cpu, VMCS_GUEST_CR0);
    s->Cr3 = ReadField(Cpu, VMCS_GUEST_CR3);
    s->Cr4 = ReadField(Cpu, VMCS_GUEST_CR4);
    s->Dr7 = ReadField(Cpu, VMCS_GUEST_DR7);
    s->DebugCtl = ReadField(Cpu, VMCS_GUEST_DEBUGCTL);
    s->FsBase = ReadField(Cpu, VMCS_GUEST_FS_BASE);
    s->GsBase = ReadField(Cpu, VMCS_GUEST_GS_BASE);
    s->Gdtr.Base = ReadField(Cpu, VMCS_GUEST_GDTR_BASE);
    s->Gdtr.Limit = (USHORT)ReadField(Cpu, VMCS_GUEST_GDTR_LIMIT);
    s->Idtr.Base = ReadField(Cpu, VMCS_GUEST_IDTR_BASE);
    s->Idtr.Limit = (USHORT)ReadField(Cpu, VMCS_GUEST_IDTR_LIMIT);
    s->Es = (USHORT)ReadField(Cpu, VMCS_GUEST_ES_SELECTOR);
    s->Ds = (USHORT)ReadField(Cpu, VMCS_GUEST_DS_SELECTOR);
    s->Fs = (USHORT)ReadField(Cpu, VMCS_GUEST_FS_SELECTOR);
    s->Gs = (USHORT)ReadField(Cpu, VMCS_GUEST_GS_SELECTOR);
    s->Ss = (USHORT)ReadField(Cpu, VMCS_GUEST_SS_SELECTOR);
    s->Ldtr = (USHORT)ReadField(Cpu, VMCS_GUEST_LDTR_SELECTOR);
    if (s->Ldtr != 0 || ReadField(Cpu, VMCS_GUEST_TR_LIMIT) != 0x67) {
        PersistFatal(Cpu, 0x105);
    }
    if (Cpu->TransferCet) {
        WriteField(Cpu, VMCS_HOST_S_CET, ReadField(Cpu, VMCS_GUEST_S_CET));
        WriteField(Cpu, VMCS_HOST_SSP, ReadField(Cpu, VMCS_GUEST_SSP));
        WriteField(Cpu, VMCS_HOST_INTERRUPT_SSP_TABLE,
            ReadField(Cpu, VMCS_GUEST_INTERRUPT_SSP_TABLE));
    }
    WriteField(Cpu, VMCS_HOST_CR0, s->Cr0);
    WriteField(Cpu, VMCS_HOST_CR3, s->Cr3);
    WriteField(Cpu, VMCS_HOST_CR4, s->Cr4);
    WriteField(Cpu, VMCS_HOST_GDTR_BASE, s->Gdtr.Base);
    WriteField(Cpu, VMCS_HOST_IDTR_BASE, s->Idtr.Base);
    WriteField(Cpu, VMCS_HOST_FS_BASE, s->FsBase);
    WriteField(Cpu, VMCS_HOST_GS_BASE, s->GsBase);
    WriteField(Cpu, VMCS_HOST_TR_BASE, ReadField(Cpu, VMCS_GUEST_TR_BASE));
    WriteField(Cpu, VMCS_HOST_SYSENTER_CS, ReadField(Cpu, VMCS_GUEST_SYSENTER_CS));
    WriteField(Cpu, VMCS_HOST_SYSENTER_ESP, ReadField(Cpu, VMCS_GUEST_SYSENTER_ESP));
    WriteField(Cpu, VMCS_HOST_SYSENTER_EIP, ReadField(Cpu, VMCS_GUEST_SYSENTER_EIP));
    // Clear VMXE only after VMXOFF; preserve all other live guest CR4 changes.
    s->Cr4 = (s->Cr4 & ~CR4_VMXE) | (Cpu->SavedCr4 & CR4_VMXE);
    WriteField(Cpu, VMCS_GUEST_RIP, (ULONG64)(ULONG_PTR)HvShutdownVmcall);
    value = ReadField(Cpu, VMCS_GUEST_INTERRUPTIBILITY_STATE);
    WriteField(Cpu, VMCS_GUEST_INTERRUPTIBILITY_STATE, value | 8ull);
    WriteField(Cpu, VMCS_HOST_RIP, (ULONG64)(ULONG_PTR)HvNativeExit);
    // No timer/window exit may interrupt the call-free final handshake.
    value = ReadField(Cpu, VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS);
    WriteField(Cpu, VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS,
        value & ~(ULONG64)(PRIMARY_NMI_WINDOW_EXITING | PRIMARY_MONITOR_TRAP_FLAG));
    Cpu->Persist.ShuttingDown = 1;
    return 0;
}
