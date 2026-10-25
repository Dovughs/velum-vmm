#pragma once
#include "vmx.h"
// Fixed ABI for the call-free native transition. A second VM exit transfers CET.
typedef struct _HV_NATIVE_STATE {
    ULONG64 Rip, Rsp, Rflags, Cr0, Cr3, Cr4, Dr7, DebugCtl, FsBase, GsBase;
    HV_DESCRIPTOR_TABLE Gdtr; // 50
    UCHAR PadGdt[6];
    HV_DESCRIPTOR_TABLE Idtr; // 60
    UCHAR PadIdt[6];
    USHORT Es, Ds, Fs, Gs, Ss, Ldtr; // 70
} HV_NATIVE_STATE;
C_ASSERT(FIELD_OFFSET(HV_NATIVE_STATE, Gdtr) == 0x50);
C_ASSERT(FIELD_OFFSET(HV_NATIVE_STATE, Idtr) == 0x60);
C_ASSERT(FIELD_OFFSET(HV_NATIVE_STATE, Es) == 0x70);
extern VOID HvNativeExit(VOID);
extern VOID HvShutdownCpuid(VOID);
extern VOID HvShutdownVmcall(VOID);
extern VOID HvShutdownReturn(VOID);
extern VOID HvRootNmi(VOID);
extern VOID HvRootFault(VOID);
