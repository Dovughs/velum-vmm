#pragma once

// Independent additive protocol: existing monitor/profile ABI stays unchanged.
#define HV_PROCESSOR_VERSION 1u
#define HV_CPUID_RULES 16u
#define HV_TSC_OFFSET_LIMIT 1000000000ll
#define IOCTL_HV_PROCESSOR_GET CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80B, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_HV_PROCESSOR_SET CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80C, METHOD_BUFFERED, FILE_WRITE_ACCESS)

typedef struct _HV_CPUID_RULE {
    ULONG Leaf, Subleaf, RegisterMask, Reserved;
    ULONG Values[4]; // EAX, EBX, ECX, EDX; mask selects whole registers.
} HV_CPUID_RULE;

typedef struct _HV_PROCESSOR_CONFIG {
    ULONG Version, CpuidEnabled, OffsetEnabled, RuleCount;
    LONG64 OffsetTicks; // Remembered while OFF; effective offset is then zero.
    HV_CPUID_RULE Rules[HV_CPUID_RULES];
} HV_PROCESSOR_CONFIG;

static __inline void HvProcessorDefaults(HV_PROCESSOR_CONFIG *Config)
{
    HV_PROCESSOR_CONFIG defaults = {0};
    defaults.Version = HV_PROCESSOR_VERSION;
    defaults.CpuidEnabled = 1;
    defaults.RuleCount = 2;
    defaults.Rules[0].Leaf = 0x40000000u;
    defaults.Rules[0].RegisterMask = 15;
    defaults.Rules[0].Values[0] = 0x40000001u;
    defaults.Rules[0].Values[1] = 0x756C6556u;
    defaults.Rules[0].Values[2] = 0x20202020u;
    defaults.Rules[0].Values[3] = 0x2020206Du;
    defaults.Rules[1].Leaf = 0x40000001u;
    defaults.Rules[1].RegisterMask = 1;
    defaults.Rules[1].Values[0] = 0x7648794Du;
    *Config = defaults;
}

static __inline ULONG HvCpuidEditableMask(ULONG Leaf, ULONG Subleaf)
{
    if (Subleaf == 0 && Leaf == 0) { return 14; } // Vendor only, keep max basic leaf.
    if (Subleaf == 0 && Leaf >= 0x80000002u && Leaf <= 0x80000004u) { return 15; }
    if (Leaf >= 0x40000000u && Leaf <= 0x400000FFu) { return 15; }
    return 0; // Feature, topology, address-width, frequency and control leaves.
}

static __inline int HvProcessorValid(const HV_PROCESSOR_CONFIG *Config)
{
    ULONG i, j;
    if (Config->Version != HV_PROCESSOR_VERSION || Config->CpuidEnabled > 1 ||
        Config->OffsetEnabled > 1 || Config->RuleCount > HV_CPUID_RULES ||
        Config->OffsetTicks < -HV_TSC_OFFSET_LIMIT ||
        Config->OffsetTicks > HV_TSC_OFFSET_LIMIT) { return 0; }

    for (i = 0; i < HV_CPUID_RULES; ++i) {
        const HV_CPUID_RULE *rule = &Config->Rules[i];
        if (rule->Reserved) { return 0; }
        if (i >= Config->RuleCount) {
            if (rule->Leaf || rule->Subleaf || rule->RegisterMask) { return 0; }
            for (j = 0; j < 4; ++j) { if (rule->Values[j]) { return 0; } }
            continue;
        }
        if (!rule->RegisterMask ||
            (rule->RegisterMask & ~HvCpuidEditableMask(rule->Leaf, rule->Subleaf))) { return 0; }
        for (j = 0; j < i; ++j) {
            if (Config->Rules[j].Leaf == rule->Leaf &&
                Config->Rules[j].Subleaf == rule->Subleaf) { return 0; }
        }
    }
    return 1;
}

static __inline void HvCpuidApply(const HV_PROCESSOR_CONFIG *Config,
    ULONG Leaf, ULONG Subleaf, ULONG Values[4])
{
    ULONG i, j;
    if (!Config->CpuidEnabled) { return; }
    for (i = 0; i < Config->RuleCount; ++i) {
        const HV_CPUID_RULE *rule = &Config->Rules[i];
        if (rule->Leaf != Leaf || rule->Subleaf != Subleaf) { continue; }
        for (j = 0; j < 4; ++j) {
            if (rule->RegisterMask & (1u << j)) { Values[j] = rule->Values[j]; }
        }
        return;
    }
}

// Deadline zero disarms the timer. Saturation must never turn a real deadline
// into zero or wrap an expired deadline into the distant future.
static __inline ULONG64 HvDeadlineTranslate(ULONG64 Deadline, LONG64 Delta)
{
    if (!Deadline) { return 0; }
    if (Delta >= 0) {
        ULONG64 add = (ULONG64)Delta;
        return Deadline > ~0ull - add ? ~0ull : Deadline + add;
    }
    {
        ULONG64 subtract = 0ull - (ULONG64)Delta;
        return Deadline <= subtract ? 1 : Deadline - subtract;
    }
}
