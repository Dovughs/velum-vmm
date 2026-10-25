// Kernel input schemas and bounded accounting helpers (no OS calls).
#pragma once
static __inline ULONG HvKernelFieldMask(ULONG Target)
{
    switch (Target) {
    case 13: case 14: case 22: return 7;
    case 15: case 18: case 20: case 23: return 3;
    case 16: case 17: return 15;
    case 19: case 21: return 1;
    case HV_TARGET_COPY_VIRTUAL_MEMORY: return 0x7Fu;
    default: return 0;
    }
}
static __inline int HvKernelTagField(ULONG Target)
{
    if (Target == 15) { return 1; }
    return Target == 13 || Target == 14 || Target == 16 || Target == 17 ? 2 : -1;
}
static __inline int HvKernelSizeField(ULONG Target)
{
    if (Target == 20) { return 0; }
    if (Target == HV_TARGET_COPY_VIRTUAL_MEMORY) { return 4; }
    return Target == 13 || Target == 14 || Target == 16 || Target == 17 ||
        Target == 18 || Target == 22 || Target == 23 ? 1 : -1;
}
static __inline ULONG64 HvSaturatingAdd(ULONG64 A, ULONG64 B)
{
    return B > ~0ull - A ? ~0ull : A + B;
}
static __inline void HvKernelDecode(ULONG Target, const ULONG64 *Arguments, ULONG64 *Fields, ULONG ArgumentMask)
{
    ULONG i;
    for (i = 0; i < 4; ++i) { Fields[i] = Arguments[i]; }
    if (Target == 14 || Target == 16 || Target == 18) { Fields[0] = (ULONG)Fields[0]; }
    if (Target == 15) { Fields[1] = (ULONG)Fields[1]; }
    if (HvKernelTagField(Target) == 2 || Target == 22) { Fields[2] = (ULONG)Fields[2]; }
    if (Target == 16) { Fields[3] = (ULONG)Fields[3]; }
    if (Target == HV_TARGET_COPY_VIRTUAL_MEMORY) {
        for (i = 4; i < 7; ++i) {
            if (ArgumentMask & (1u << i)) { Fields[i] = i == 5 ? (unsigned char)Arguments[i] : Arguments[i]; }
        }
    }
}
static __inline void HvKernelTagText(ULONG Tag, char Text[5])
{
    ULONG i;
    // Memory/PoolMon order (least significant byte first); never emit controls.
    for (i = 0; i < 4; ++i) {
        unsigned char c = (unsigned char)(Tag >> (i * 8));
        Text[i] = c >= 32 && c <= 126 ? (char)c : '.';
    }
    Text[4] = 0;
}

