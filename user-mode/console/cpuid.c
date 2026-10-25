#include "app.h"

#define EXPECTED_VENDOR_EBX 0x756C6556u
#define EXPECTED_VENDOR_EDX 0x2020206Du
#define EXPECTED_VENDOR_ECX 0x20202020u

static int ProcessorMask(DWORD_PTR *Mask)
{
    DWORD_PTR process;
    if (GetActiveProcessorGroupCount() != 1) {
        puts("[ERROR] This driver supports one processor group only."); return 0;
    }
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process, Mask)) {
        AppError("Read processor affinity", GetLastError()); return 0;
    }
    if (process != *Mask || !process) {
        puts("[ERROR] Process affinity is restricted; cannot verify every processor."); return 0;
    }
    return 1;
}

static int VendorMatches(const int Registers[4])
{
    return (unsigned)Registers[0] == 0x40000001u &&
        (unsigned)Registers[1] == EXPECTED_VENDOR_EBX &&
        (unsigned)Registers[2] == EXPECTED_VENDOR_ECX &&
        (unsigned)Registers[3] == EXPECTED_VENDOR_EDX;
}

static int ControlMatches(const int Registers[4])
{
    return (unsigned)Registers[0] == HV_CONTROL_SIGNATURE && Registers[2] == 1 &&
        Registers[1] >= HV_STATE_STOPPED && Registers[1] <= HV_STATE_STOPPING &&
        Registers[3] > 0 && Registers[3] <= 64;
}

int AppVerify(void)
{
    DWORD_PTR mask, bit, previous;
    unsigned cpu = 0, total = 0, passed = 0, expected = 0;
    HV_PROCESSOR_CONFIG config = { .Version = HV_PROCESSOR_VERSION };
    if (!AppProcessorIo(&config, 0)) { return 1; }
    if (!ProcessorMask(&mask)) { return 1; }
    for (bit = 1; bit; bit <<= 1) { if (mask & bit) { ++expected; } }
    for (bit = 1; bit; bit <<= 1, ++cpu) {
        int leaf1[4], control[4];
        int flagsOk, rulesOk = 1, controlOk;
        ULONG i, j;
        if (!(mask & bit)) { continue; }
        previous = SetThreadAffinityMask(GetCurrentThread(), bit);
        if (!previous) { AppError("Pin verification thread", GetLastError()); return 1; }
        __cpuidex(leaf1, 1, 0);
        if (config.CpuidEnabled) {
            for (i = 0; i < config.RuleCount; ++i) {
                int values[4];
                const HV_CPUID_RULE *rule = &config.Rules[i];
                __cpuidex(values, (int)rule->Leaf, (int)rule->Subleaf);
                for (j = 0; j < 4; ++j) {
                    if ((rule->RegisterMask & (1u << j)) &&
                        (ULONG)values[j] != rule->Values[j]) { rulesOk = 0; }
                }
            }
        }
        __cpuidex(control, HV_CONTROL_LEAF, HV_CONTROL_QUERY);
        if (!SetThreadAffinityMask(GetCurrentThread(), previous)) {
            AppError("Restore thread affinity", GetLastError()); return 1;
        }
        flagsOk = ((unsigned)leaf1[2] & ((1u << 31) | (1u << 5))) == 0;
        controlOk = ControlMatches(control) && control[1] == HV_STATE_RUNNING &&
            (unsigned)control[3] == expected;
        printf("CPU %02u  %s\n", cpu, flagsOk && rulesOk && controlOk ? "[PASS]" : "[FAIL / NOT RUNNING]");
        printf("  00000001 ECX=%08X  hypervisor/VMX bits clear: %s\n", (unsigned)leaf1[2], flagsOk ? "PASS" : "FAIL");
        printf("  Applied CPUID rules: %s (%lu configured)\n",
            !config.CpuidEnabled ? "OFF / passthrough" : rulesOk ? "PASS" : "FAIL", config.RuleCount);
        printf("  Control ABI=%d state=%d cores=%d: %s\n", control[2], control[1], control[3], controlOk ? "PASS" : "FAIL");
        ++total;
        if (flagsOk && rulesOk && controlOk) { ++passed; }
    }
    printf("\nCPUID verification: %u/%u processors passed.\n", passed, total);
    return total && passed == total ? 0 : 1;
}

int AppShutdown(void)
{
    DWORD_PTR mask, bit, previous;
    ULONGLONG deadline;
    unsigned active;
    int requested = 0;
    if (!ProcessorMask(&mask)) { return 1; }
    InterlockedExchange(&g_Interrupted, 0);
    puts("[SHUTDOWN] Requesting CPUID shutdown and checking every processor...");
    deadline = GetTickCount64() + 15000;
    do {
        active = 0;
        for (bit = 1; bit; bit <<= 1) {
            int control[4], vendor[4], mismatch;
            if (!(mask & bit)) { continue; }
            previous = SetThreadAffinityMask(GetCurrentThread(), bit);
            if (!previous) { AppError("Pin shutdown thread", GetLastError()); return 1; }
            __cpuidex(control, HV_CONTROL_LEAF, HV_CONTROL_QUERY);
            __cpuidex(vendor, 0x40000000, 0);
            mismatch = !ControlMatches(control) && VendorMatches(vendor);
            if (ControlMatches(control)) {
                ++active;
                __cpuidex(control, HV_CONTROL_LEAF, HV_CONTROL_STOP);
                requested = 1;
            }
            if (!SetThreadAffinityMask(GetCurrentThread(), previous)) {
                AppError("Restore thread affinity", GetLastError()); return 1;
            }
            if (mismatch) {
                puts("[ERROR] Vendor is present but control ABI is invalid; service removal aborted.");
                return 1;
            }
        }
        if (!active) {
            puts(requested ? "[OK] Hypervisor shutdown verified on every processor. Windows continues running." :
                "[OK] No matching hypervisor found on any processor.");
            return 0;
        }
        if (g_Interrupted) { puts("Shutdown wait cancelled; no service removal attempted."); return 1; }
        Sleep(100);
    } while (GetTickCount64() < deadline);
    puts("[ERROR] Shutdown timed out. Velum was not stopped or deleted.");
    return 1;
}
