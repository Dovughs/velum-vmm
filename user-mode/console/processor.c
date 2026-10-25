#include "app.h"
#include <errno.h>
#include <ctype.h>

static int ProcessorLine(const char *Prompt, char *Line, size_t Capacity)
{
    size_t length;
    printf("%s", Prompt);
    fflush(stdout);
    if (!fgets(Line, (int)Capacity, stdin)) { return 0; }
    if (!strchr(Line, '\n') && !feof(stdin)) {
        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF) { }
        puts("[ERROR] Input too long."); return 0;
    }
    length = strcspn(Line, "\r\n");
    Line[length] = 0;
    return 1;
}

// Decimal by default; 0x explicitly selects hexadecimal (never implicit octal).
static int ProcessorNumber(const char *Text, int Signed, ULONG64 *Value)
{
    const char *start = Text, *digits;
    char *end;
    ULONG64 value;
    int base;
    while (isspace((unsigned char)*start)) { ++start; }
    if (!*start || (!Signed && *start == '-')) { return 0; }
    digits = start + (*start == '-' || *start == '+');
    base = digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X') ? 16 : 10;
    errno = 0;
    if (Signed) { value = (ULONG64)_strtoi64(start, &end, base); }
    else { value = _strtoui64(start, &end, base); }
    if (end == start || errno == ERANGE) { return 0; }
    while (isspace((unsigned char)*end)) { ++end; }
    if (*end) { return 0; }
    *Value = value;
    return 1;
}

static int ProcessorReadNumber(const char *Prompt, int Signed, ULONG64 *Value)
{
    char line[128];
    if (!ProcessorLine(Prompt, line, sizeof(line))) { return 0; }
    if (!ProcessorNumber(line, Signed, Value)) {
        puts("[ERROR] Enter a decimal number or 0x hexadecimal value."); return 0;
    }
    return 1;
}

static int ProcessorString(const char *Text, char *Bytes, size_t Capacity)
{
    size_t length, i;
    while (*Text == ' ' || *Text == '\t') { ++Text; }
    length = strlen(Text);
    while (length && (Text[length - 1] == ' ' || Text[length - 1] == '\t')) { --length; }
    if (length >= 2 && Text[0] == '"' && Text[length - 1] == '"') {
        ++Text; length -= 2;
    }
    if (length > Capacity) { return 0; }
    for (i = 0; i < length; ++i) {
        if ((unsigned char)Text[i] < 32 || (unsigned char)Text[i] > 126 || Text[i] == '"') { return 0; }
    }
    memset(Bytes, 0, Capacity);
    memcpy(Bytes, Text, length);
    return 1;
}

static HV_CPUID_RULE *ProcessorRule(HV_PROCESSOR_CONFIG *Config, ULONG Leaf, ULONG Subleaf)
{
    ULONG i;
    for (i = 0; i < Config->RuleCount; ++i) {
        if (Config->Rules[i].Leaf == Leaf && Config->Rules[i].Subleaf == Subleaf) {
            return &Config->Rules[i];
        }
    }
    if (Config->RuleCount == HV_CPUID_RULES) { return NULL; }
    Config->Rules[Config->RuleCount].Leaf = Leaf;
    Config->Rules[Config->RuleCount].Subleaf = Subleaf;
    return &Config->Rules[Config->RuleCount++];
}

static void ProcessorName(HV_PROCESSOR_CONFIG *Config, int Brand)
{
    HV_PROCESSOR_CONFIG draft = *Config;
    char line[128], bytes[48];
    ULONG i;
    if (!ProcessorLine(Brand ? "  CPU brand (0..48 ASCII bytes; \"\" = empty) > " :
        "  Hypervisor vendor (0..12 ASCII bytes; \"\" = empty) > ", line, sizeof(line))) { return; }
    if (!ProcessorString(line, bytes, Brand ? 48 : 12)) {
        puts("[ERROR] Invalid ASCII name, quotation marks or length."); return;
    }
    for (i = 0; i < (Brand ? 3u : 1u); ++i) {
        HV_CPUID_RULE *rule = ProcessorRule(&draft, Brand ? 0x80000002u + i : 0x40000000u, 0);
        if (!rule) { puts("[ERROR] Rule table is full; remove a rule first."); return; }
        if (Brand) {
            rule->RegisterMask = 15;
            memcpy(rule->Values, bytes + i * 16, 16);
        } else {
            // CPUID vendor order is EBX, EDX, ECX, not register-array order.
            rule->RegisterMask |= 14;
            memcpy(&rule->Values[1], bytes, 4);
            memcpy(&rule->Values[3], bytes + 4, 4);
            memcpy(&rule->Values[2], bytes + 8, 4);
        }
    }
    *Config = draft;
}

static void ProcessorEditRule(HV_PROCESSOR_CONFIG *Config)
{
    HV_CPUID_RULE rule = {0}, *slot;
    ULONG64 value;
    ULONG i, allowed;
    static const char *const prompts[4] = { "  EAX > ", "  EBX > ", "  ECX > ", "  EDX > " };
    puts("  Editable: leaf 0 vendor registers; 0x80000002..4 brand;");
    puts("  0x40000000..0x400000FF hypervisor leaves (exact subleaf).");
    puts("  CPU features/topology/frequency and the control ABI are protected.");
    if (!ProcessorReadNumber("  Leaf > ", 0, &value) || value > 0xFFFFFFFFull) { return; }
    rule.Leaf = (ULONG)value;
    if (!ProcessorReadNumber("  Subleaf > ", 0, &value) || value > 0xFFFFFFFFull) { return; }
    rule.Subleaf = (ULONG)value;
    allowed = HvCpuidEditableMask(rule.Leaf, rule.Subleaf);
    if (!allowed) { puts("[ERROR] This leaf/subleaf is not runtime-editable."); return; }
    printf("  Register mask: EAX=1 EBX=2 ECX=4 EDX=8; allowed=0x%lX\n", allowed);
    if (!ProcessorReadNumber("  Register mask > ", 0, &value) || !value ||
        value > 15 || ((ULONG)value & ~allowed)) {
        puts("[ERROR] Invalid register mask."); return;
    }
    rule.RegisterMask = (ULONG)value;
    for (i = 0; i < 4; ++i) {
        if (!(rule.RegisterMask & (1u << i))) { continue; }
        if (!ProcessorReadNumber(prompts[i], 0, &value) || value > 0xFFFFFFFFull) {
            puts("[ERROR] A CPUID register must fit in 32 bits."); return;
        }
        rule.Values[i] = (ULONG)value;
    }
    slot = ProcessorRule(Config, rule.Leaf, rule.Subleaf);
    if (!slot) { puts("[ERROR] Rule table is full."); return; }
    *slot = rule;
}

static void ProcessorQuery(void)
{
    ULONG64 leaf, subleaf;
    int regs[4];
    if (!ProcessorReadNumber("  Leaf > ", 0, &leaf) || leaf > 0xFFFFFFFFull ||
        !ProcessorReadNumber("  Subleaf > ", 0, &subleaf) || subleaf > 0xFFFFFFFFull) { return; }
    // Never execute commands on the control leaf from the raw query UI.
    if ((ULONG)leaf == HV_CONTROL_LEAF) { puts("[ERROR] Use Diagnostics for the control leaf."); return; }
    __cpuidex(regs, (int)(ULONG)leaf, (int)(ULONG)subleaf);
    printf("  Live CPU: EAX=%08X EBX=%08X ECX=%08X EDX=%08X\n",
        (unsigned)regs[0], (unsigned)regs[1], (unsigned)regs[2], (unsigned)regs[3]);
    if ((leaf == 0 || leaf == 0x40000000u) && subleaf == 0) {
        char vendor[13] = {0};
        size_t i;
        memcpy(vendor, &regs[1], 4);
        memcpy(vendor + 4, &regs[3], 4);
        memcpy(vendor + 8, &regs[2], 4);
        for (i = 0; i < 12; ++i) {
            if (vendor[i] && ((unsigned char)vendor[i] < 32 || (unsigned char)vendor[i] > 126)) {
                vendor[i] = '.';
            }
        }
        printf("  Vendor text: \"%s\" (raw register bytes above)\n", vendor);
    }
    puts("  Reads the applied configuration, not the unsaved draft.");
}

// Each page owns only its fields. Refresh the other page's applied state before
// committing, so Apply and Reset cannot revert unrelated settings.
static void ProcessorCopyCpuid(HV_PROCESSOR_CONFIG *To, const HV_PROCESSOR_CONFIG *From)
{
    To->CpuidEnabled = From->CpuidEnabled;
    To->RuleCount = From->RuleCount;
    memcpy(To->Rules, From->Rules, sizeof(To->Rules));
}

static void ProcessorApplyPage(HV_PROCESSOR_CONFIG *Draft,
    HV_PROCESSOR_CONFIG *Applied, int Timing)
{
    HV_PROCESSOR_CONFIG next = { .Version = HV_PROCESSOR_VERSION };
    if (!AppProcessorIo(&next, 0)) { return; }
    if (Timing) {
        if ((Draft->OffsetEnabled ? Draft->OffsetTicks : 0ll) !=
            (next.OffsetEnabled ? next.OffsetTicks : 0ll)) {
            puts("  Live offset changes can move Windows time forward/backward.");
            puts("  Use a disposable VM; timer translation does not make QPC monotonic.");
        }
        next.OffsetEnabled = Draft->OffsetEnabled;
        next.OffsetTicks = Draft->OffsetTicks;
    } else { ProcessorCopyCpuid(&next, Draft); }
    if (AppProcessorIo(&next, 1)) {
        *Draft = *Applied = next;
        puts("[OK] Applied on all processors.");
    }
}

void AppCpuidMenu(void)
{
    HV_PROCESSOR_CONFIG draft = { .Version = HV_PROCESSOR_VERSION };
    HV_PROCESSOR_CONFIG applied;
    int choice;
    if (!AppProcessorIo(&draft, 0)) { AppPause(); return; }
    applied = draft;
    for (;;) {
        ULONG i;
        AppPage("CPUID SETTINGS - DRAFT");
        printf("  Overrides: %s | rules: %lu/%u | currently applied: %s\n",
            draft.CpuidEnabled ? "ON" : "OFF", draft.RuleCount, HV_CPUID_RULES,
            applied.CpuidEnabled ? "ON" : "OFF");
        for (i = 0; i < draft.RuleCount; ++i) {
            const HV_CPUID_RULE *rule = &draft.Rules[i];
            printf("  %2lu: %08lX:%08lX mask=%lX  %08lX/%08lX/%08lX/%08lX\n",
                i + 1, rule->Leaf, rule->Subleaf, rule->RegisterMask,
                rule->Values[0], rule->Values[1], rule->Values[2], rule->Values[3]);
        }
        puts("\n  [1] Toggle overrides       [2] Hypervisor vendor name");
        puts("  [3] CPU brand name         [4] Add/replace register rule");
        puts("  [5] Remove rule            [6] Query live CPUID");
        puts("  [7] Reset CPUID draft      [8] Apply CPUID on all CPUs");
        puts("  [0] Discard/back");
        puts("  Quick test: [2] enter a name, [8] apply, [6] query 0x40000000 / 0.");
        choice = AppChoice("\n  Select > ", 0, 8);
        if (choice <= 0) { return; }
        if (choice == 1) { draft.CpuidEnabled = !draft.CpuidEnabled; }
        else if (choice == 2 || choice == 3) { ProcessorName(&draft, choice == 3); AppPause(); }
        else if (choice == 4) { ProcessorEditRule(&draft); AppPause(); }
        else if (choice == 5 && draft.RuleCount) {
            int index = AppChoice("  Rule number (0 = cancel) > ", 0, (int)draft.RuleCount);
            if (index > 0) {
                for (i = (ULONG)index; i < draft.RuleCount; ++i) { draft.Rules[i - 1] = draft.Rules[i]; }
                memset(&draft.Rules[--draft.RuleCount], 0, sizeof(draft.Rules[0]));
            }
        } else if (choice == 6) { ProcessorQuery(); AppPause(); }
        else if (choice == 7) {
            HV_PROCESSOR_CONFIG defaults;
            HvProcessorDefaults(&defaults);
            ProcessorCopyCpuid(&draft, &defaults);
        } else if (choice == 8) { ProcessorApplyPage(&draft, &applied, 0); AppPause(); }
    }
}

void AppTscMenu(void)
{
    HV_PROCESSOR_CONFIG draft = { .Version = HV_PROCESSOR_VERSION };
    HV_PROCESSOR_CONFIG applied;
    int choice;
    if (!AppProcessorIo(&draft, 0)) { AppPause(); return; }
    applied = draft;
    for (;;) {
        AppPage("TSC OFFSET - DRAFT");
        printf("  Draft: %s | %lld ticks\n",
            draft.OffsetEnabled ? "ON" : "OFF", draft.OffsetTicks);
        printf("  Currently applied: %s | effective: %lld ticks\n",
            applied.OffsetEnabled ? "ON" : "OFF",
            applied.OffsetEnabled ? applied.OffsetTicks : 0ll);
        puts("\n  [1] Set signed tick value");
        puts("  [2] Toggle fixed offset ON/OFF");
        puts("  [3] Reset TSC draft (OFF, 0 ticks)");
        puts("  [4] Apply TSC on all CPUs");
        puts("  [0] Discard/back");
        choice = AppChoice("\n  Select > ", 0, 4);
        if (choice <= 0) { return; }
        if (choice == 1) {
            ULONG64 value;
            printf("  Range: -%lld .. +%lld ticks (not milliseconds).\n", HV_TSC_OFFSET_LIMIT, HV_TSC_OFFSET_LIMIT);
            if (ProcessorReadNumber("  Signed offset > ", 1, &value)) {
                LONG64 ticks = (LONG64)value;
                if (ticks < -HV_TSC_OFFSET_LIMIT || ticks > HV_TSC_OFFSET_LIMIT) {
                    puts("[ERROR] Offset is outside the supported range.");
                } else { draft.OffsetTicks = ticks; }
            }
            AppPause();
        } else if (choice == 2) { draft.OffsetEnabled = !draft.OffsetEnabled; }
        else if (choice == 3) { draft.OffsetEnabled = 0; draft.OffsetTicks = 0; }
        else if (choice == 4) { ProcessorApplyPage(&draft, &applied, 1); AppPause(); }
    }
}
