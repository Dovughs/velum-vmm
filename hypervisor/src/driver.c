// hypervisor/src/driver.c
#include <ntifs.h> // ZwWaitForSingleObject for joining a worker by handle.
#include "common.h"
#include "vmcs.h"
#include "hook.h"
#include "persist.h"
#include "syscall.h"
#include "monitor.h"
#include "filter.h"

PPER_CPU_DATA g_PerCpuData[MAXIMUM_PROCESSORS] = { 0 };
ULONG g_CpuCount = 0;
volatile LONG g_VmxFailureCount = 0;

// CPUID probe results, written by HvGuestCpuidProbe (guest stub) and read
// back at PASSIVE_LEVEL. Shared address space (same CR3) so the guest can store
// directly into this nonpaged buffer. Indexed: [0]=leaf-1 ECX, [1]=EBX, [2]=EDX,
// [3]=ECX, [4]=leaf-0x40000001 EAX.
volatile ULONG64 g_CpuidProbe[5] = { 0 };
static PDRIVER_OBJECT g_DriverObject;

static NTSTATUS VmcsLaunchTestAll(ULONG64 GuestRip);
NTSTATUS HookRunSelfTest(VOID);
static NTSTATUS HookCpuidSelfTest(VOID);

// Deferred-event drain worker (PASSIVE). Wakes every ~10 ms and drains
// the per-core hook rings; the VM-exit handler itself never logs or blocks.
static KEVENT g_HookDrainStopEvent;
static volatile LONG g_HookDrainStop = 0;
static PKTHREAD g_HookDrainThread = NULL;

static VOID HookDrainWorker(PVOID Context)
{
    LARGE_INTEGER timeout;
    UNREFERENCED_PARAMETER(Context);
    timeout.QuadPart = -(LONGLONG)(10 * 1000 * 10);   // relative 10 ms; scheduler resolution applies
    for (;;) {
        NTSTATUS wait = KeWaitForSingleObject(&g_HookDrainStopEvent, Executive,
            KernelMode, FALSE, &timeout);
        if (g_HookDrainStop != 0 || wait == STATUS_SUCCESS) { break; }
        HookDrainAllCores();
        PersistPoll();
        SyscallDrainAllCores();
        if (g_PersistActive == HV_STATE_STOPPED) {
            g_DriverObject->DriverUnload = DriverUnload;
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID HookDrainWorkerStop(VOID)
{
    if (g_HookDrainThread == NULL) { return; }
    g_HookDrainStop = 1;
    KeSetEvent(&g_HookDrainStopEvent, 0, FALSE);
    KeWaitForSingleObject(g_HookDrainThread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(g_HookDrainThread);
    g_HookDrainThread = NULL;
}

static PCSTR VmxFailureText(PPER_CPU_DATA Cpu)
{
    switch ((VMX_START_RESULT)Cpu->StartResult) {
    case VmxFeatureDisabled: return "FEATURE_CONTROL disabled/unlocked";
    case VmxAlreadyOwned: return "CR4.VMXE already set; ownership unavailable";
    case VmxInvalidFixedBits: return "CR0/CR4 violates VMX fixed bits";
    case VmxInstructionFailed:
        return Cpu->VmxInstructionStatus == 1 ?
            "VMfailValid (MSVC status 1)" :
            (Cpu->VmxInstructionStatus == 2 ?
                "VMfailInvalid (MSVC status 2)" : "unexpected intrinsic status");
    default: return "IPI callback did not complete VMXON";
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    ULONG i;
    ULONG count;
    UNREFERENCED_PARAMETER(RegistryPath);
    g_DriverObject = DriverObject;
    DriverObject->DriverUnload = NULL;
    {
        RTL_OSVERSIONINFOW version = { 0 };
        int cpuId[4];
        version.dwOSVersionInfoSize = sizeof(version);
        __cpuid(cpuId, 1);
        if (NT_SUCCESS(RtlGetVersion(&version))) {
            HV_INFO("START: Windows %lu.%lu build %lu; CPU signature=%08lX hypervisor=%lu\n",
                version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber,
                (ULONG)cpuId[0], (ULONG)((unsigned int)cpuId[2] >> 31));
        }
    }
    count = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (count == 0 || count > MAXIMUM_PROCESSORS ||
        KeQueryActiveGroupCount() != 1 ||
        KeQueryMaximumProcessorCountEx(ALL_PROCESSOR_GROUPS) != count) {
        HV_ERROR("VMX: requires one fixed processor group, at most %u CPUs\n",
            (unsigned int)MAXIMUM_PROCESSORS);
        return STATUS_NOT_SUPPORTED;
    }
    g_CpuCount = count;
    g_VmxFailureCount = 0;
    RtlZeroMemory(g_PerCpuData, sizeof(g_PerCpuData));
    HV_INFO("START: allocating per-CPU state\n");
    status = PerCpuAllocate();
    if (!NT_SUCCESS(status)) { goto Failed; }
    HV_INFO("START: detecting VMX capabilities\n");
    status = VmxDetectAll();
    if (!NT_SUCCESS(status)) { goto Failed; }
    HV_INFO("START: allocating VMXON regions\n");
    status = PerCpuAllocateRegions();
    if (!NT_SUCCESS(status)) { goto Failed; }
    HV_INFO("START: allocating host stacks\n");
    status = PerCpuAllocateStacks();
    if (!NT_SUCCESS(status)) { goto Failed; }
    HV_INFO("START: configuring feature control\n");
    status = VmxConfigureFeatureControl();
    if (!NT_SUCCESS(status)) { goto Failed; }

    HV_INFO("START: entering VMXON IPI\n");
    (VOID)KeIpiGenericCall(VmxonIpiCallback, 0);
    HV_INFO("START: VMXON IPI returned\n");
    status = g_VmxFailureCount == 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    for (i = 0; i < g_CpuCount; ++i) {
        if (g_PerCpuData[i]->VmxEnabled != 0 &&
            g_PerCpuData[i]->StartResult == VmxStarted) {
            HV_INFO("VMXON ok on core %lu; VMCS_ENUM=0x%016I64X\n",
                i, g_PerCpuData[i]->Capabilities.VmcsEnum);
        } else {
            HV_ERROR("VMX: core %lu: %s\n", i, VmxFailureText(g_PerCpuData[i]));
            status = STATUS_UNSUCCESSFUL;
        }
    }
    if (!NT_SUCCESS(status)) { goto Failed; }
    HV_INFO("START: initializing VMCS\n");
    status = VmcsInitializeAll();
    if (!NT_SUCCESS(status)) { goto Failed; }
    HV_INFO("START: building EPT\n");
    status = EptInitializeAll();
    if (!NT_SUCCESS(status)) { goto Failed; }

    HV_INFO("START: initializing hook state and drain worker\n");
    HookInitialize();

    // Start the deferred-drain worker at PASSIVE_LEVEL.
    KeInitializeEvent(&g_HookDrainStopEvent, NotificationEvent, FALSE);
    g_HookDrainStop = 0;
    {
        HANDLE threadHandle = NULL;
        status = PsCreateSystemThread(&threadHandle, THREAD_ALL_ACCESS, NULL, NULL,
            NULL, HookDrainWorker, NULL);
        if (!NT_SUCCESS(status)) { goto Failed; }
        status = ObReferenceObjectByHandle(threadHandle, THREAD_ALL_ACCESS, NULL,
            KernelMode, (PVOID *)&g_HookDrainThread, NULL);
        if (!NT_SUCCESS(status)) {
            // The thread exists even if obtaining a referenced object failed.
            // Join it through the still-open handle before freeing its state.
            g_HookDrainStop = 1;
            KeSetEvent(&g_HookDrainStopEvent, 0, FALSE);
            (VOID)ZwWaitForSingleObject(threadHandle, FALSE, NULL);
            ZwClose(threadHandle);
            goto Failed;
        }
        ZwClose(threadHandle);
    }

    HV_INFO("START: checking VM-entry on all cores\n");
    status = VmcsLaunchTestAll(0);
    if (!NT_SUCCESS(status)) { goto Failed; }
    HV_INFO("VMX: %lu cores online; core 0 revision 0x%08lX; EPT+UC on all cores\n",
        g_CpuCount, g_PerCpuData[0]->Capabilities.RevisionId);

#if defined(HV_ENABLE_STARTUP_HOOK_TEST)
    status = HookRunSelfTest();
    if (!NT_SUCCESS(status)) { goto Failed; }
#endif

    HV_INFO("START: checking CPUID handling\n");
    status = HookCpuidSelfTest();
    if (!NT_SUCCESS(status)) { goto Failed; }

    HV_INFO("START: allocating persistent runtime\n");
    status = PersistAllocate();
    if (!NT_SUCCESS(status)) { goto Failed; }
    HV_INFO("START: entering persistent launch\n");
    status = PersistLaunchAllCores();
    if (!NT_SUCCESS(status)) { goto Failed; }
    HV_INFO("VMX: persistent launch complete on %lu core(s); CPUID control leaf 0x%08X\n",
        g_CpuCount, HV_CONTROL_LEAF);

    // Publish runtime control with no syscall hooks installed.
    // Hooks are installed only by an explicit runtime control request.
    HV_INFO("START: creating control device\n");
    status = MonitorInitialize(DriverObject);
    if (!NT_SUCCESS(status)) { goto Failed; }
    HV_INFO("START: DriverEntry completed\n");
    return STATUS_SUCCESS;

Failed:
    HV_ERROR("START: initialization failed status=0x%08lX; entering rollback\n", (ULONG)status);
    // Stop the drain worker before freeing any state it may read, then roll back
    // all successful CPUs before freeing hardware-owned memory. VMXOFF leaves no
    // current VMCS (SDM VMXOFF); its contents are discarded, so preserving them
    // with VMCLEAR before VMXOFF is unnecessary here.
    HookDrainWorkerStop();
    MonitorDestroy();
    PersistShutdownAllCores();
    VmxStopAll();
    SyscallHookRemove();
    HookRemoveAllCores(0);
    FilterDestroy();
    PerCpuFree();
    HV_ERROR("VMX: load rolled back, status 0x%08lX\n", (ULONG)status);
    return status;
}

VOID DriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    // Stop the drain worker before freeing anything it may read.
    HookDrainWorkerStop();
    MonitorDestroy();
    // Shut down the persistent VMs (VMCALL shutdown protocol -> VMXOFF per core).
    PersistShutdownAllCores();
    VmxStopAll();
    SyscallHookRemove();
    HookRemoveAllCores(0);
    FilterDestroy();
    PerCpuFree();
    HV_INFO("VMX: clean shutdown\n");
}

// GuestRip == 0 keeps the guest entry set by VmcsPopulateCurrent (HvGuestEntryStub);
// non-zero overrides the guest RIP after population (used by the read probe).
static NTSTATUS VmcsLaunchTestAll(ULONG64 GuestRip)
{
    ULONG i;
    for (i = 0; i < g_CpuCount; ++i) {
        PPER_CPU_DATA cpu = g_PerCpuData[i];
        GROUP_AFFINITY target = { 0 };
        GROUP_AFFINITY previous;
        KIRQL oldIrql;
        NTSTATUS status;
        ULONG result = 0;
        ULONG clearResult = 0;
        BOOLEAN launchAttempted = FALSE;

        target.Mask = (KAFFINITY)1 << i;
        KeSetSystemGroupAffinityThread(&target, &previous);
        oldIrql = KeRaiseIrqlToDpcLevel();
        HV_INFO("START: probe core %lu RIP=0x%I64X CR0=0x%I64X\n",
            i, GuestRip, __readcr0());
        // Keep live CR3, descriptor tables and the launch on one unscheduled CPU.
        cpu->VmInstructionErrorValid = FALSE;
        cpu->ExitReasonValid = FALSE;
        // Re-establish a current VMCS: the prior launch test ended with VMCLEAR,
        // so repeated probes capture the current launch context.
        if (VmxClear((ULONG64)cpu->VmcsPa.QuadPart) != 0 ||
            VmxPtrLoad((ULONG64)cpu->VmcsPa.QuadPart) != 0) {
            KeLowerIrql(oldIrql);
            KeRevertToUserGroupAffinityThread(&previous);
            HV_ERROR("VMCS: core %lu VMCLEAR/VMPTRLD failed before launch\n", i);
            return STATUS_UNSUCCESSFUL;
        }
        cpu->VmcsActive = 1;
        status = VmcsPopulateCurrent(cpu, i);
        if (NT_SUCCESS(status) && GuestRip != 0) {
            if (VmxWrite(VMCS_GUEST_RIP, GuestRip) != 0) { status = STATUS_UNSUCCESSFUL; }
        }
        if (NT_SUCCESS(status)) {
            cpu->LaunchCallerRsp = 0;
            cpu->LaunchResult = 3;
            cpu->ExitReasonValid = FALSE;
            cpu->VmInstructionErrorValid = FALSE;
            *(PPER_CPU_DATA *)cpu->HostStackTop = cpu;
            launchAttempted = TRUE;
            result = VmcsLaunchOnCurrentCoreTest(&cpu->LaunchCallerRsp,
                (ULONG)cpu->TransferCet, 0);
            cpu->LaunchResult = result;
            if (result == 1 && VmcsQueryInstructionError(
                    &cpu->LastVmInstructionError) == 0) {
                cpu->VmInstructionErrorValid = TRUE;
            }
            // Query diagnostics before VMCLEAR, on the CPU owning this VMCS.
            clearResult = VmxClear((ULONG64)cpu->VmcsPa.QuadPart);
            if (clearResult == 0) { cpu->VmcsActive = 0; }
            if (result != 0 || clearResult != 0) { status = STATUS_UNSUCCESSFUL; }
        }
        KeLowerIrql(oldIrql);
        KeRevertToUserGroupAffinityThread(&previous);

        if (cpu->ExitReasonValid) {
            HV_INFO("VMCS: core %lu exit reason=0x%I64X guest RAX=0x%I64X\n",
                i, cpu->ExitReason, cpu->ExitGuestRax);
        }
        if (!NT_SUCCESS(status)) {
            if (launchAttempted) {
                HV_ERROR("VMCS: core %lu test failed: status=0x%08lX instruction=%lu clear=%lu\n",
                    i, (ULONG)status, result, clearResult);
            } else {
                HV_ERROR("VMCS: core %lu preparation failed: status=0x%08lX; VMLAUNCH and VMCLEAR not attempted\n",
                    i, (ULONG)status);
            }
            if (result == 2) {
                HV_ERROR("VMCS: error code unavailable (no current VMCS)\n");
            }
            if (cpu->VmInstructionErrorValid) {
                HV_ERROR("VMCS: core %lu VM_INSTRUCTION_ERROR=%lu\n",
                    i, cpu->LastVmInstructionError);
            }
            return status;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS HookRunSelfTest(VOID)
{
    ULONG64 targetVa = (ULONG64)(ULONG_PTR)HvGuestEntryStub;
    ULONG64 gpa;
    ULONG64 offset;
    ULONG64 pageBase;
    ULONG64 expectedRead = 0;
    ULONG i;
    NTSTATUS status;
    UCHAR probe[16];

    gpa = (ULONG64)(ULONG_PTR)HookTranslateVaToPa(targetVa, &offset);
    if (gpa == 0) {
        HV_ERROR("HOOK: cannot translate target VA 0x%I64X\n", targetVa);
        return STATUS_INVALID_ADDRESS;
    }
    pageBase = gpa & ~(PAGE_SIZE - 1ull);
    HV_INFO("HOOK: target VA=0x%I64X GPA=0x%I64X offset=0x%I64X\n",
        targetVa, pageBase, offset);

    // Map the original page once: reference bytes for the read-view checks.
    {
        PHYSICAL_ADDRESS pa;
        PVOID map;
        pa.QuadPart = (LONGLONG)pageBase;
        map = MmGetVirtualForPhysical(pa);
        if (map == NULL) {
            HV_ERROR("HOOK: cannot map original page\n");
            return STATUS_INVALID_ADDRESS;
        }
        RtlCopyMemory(probe, (UCHAR *)map + offset, sizeof(probe));
        expectedRead = *(ULONG64 *)((UCHAR *)map + offset);
    }

    // Install the execute-only hook on every core.
    status = HookInstallAllCores(targetVa, (PVOID)(ULONG_PTR)MyHookHandlerVa);
    if (!NT_SUCCESS(status)) {
        HV_ERROR("HOOK: install failed 0x%08lX\n", (ULONG)status);
        return status;
    }
    HV_INFO("HOOK: installed on %lu core(s)\n", g_CpuCount);

    // Re-run the launch; the guest executes the shadow (the hook fires).
    status = VmcsLaunchTestAll(0);
    HookDrainAllCores();   // emit the deferred fired/not-fired messages
    for (i = 0; i < g_CpuCount; ++i) {
        if (g_PerCpuData[i]->ExitReasonValid &&
            g_PerCpuData[i]->ExitGuestRax == 0xCAFEBABEull) {
            HV_INFO("HOOK: core %lu guest RAX=0xCAFEBABE -> hook FIRED\n", i);
        } else {
            HV_ERROR("HOOK: core %lu guest RAX=0x%I64X -> hook did NOT fire\n",
                i, g_PerCpuData[i]->ExitGuestRax);
            if (NT_SUCCESS(status)) { status = STATUS_UNSUCCESSFUL; }
        }
    }
    if (!NT_SUCCESS(status)) { HookRemoveAllCores(targetVa); return status; }

    // PRIMARY evidence: a guest-side read of the target must return the ORIGINAL
    // bytes (via read emulation), not the patched shadow bytes.
    status = VmcsLaunchTestAll((ULONG64)(ULONG_PTR)HvGuestReadProbe);
    for (i = 0; i < g_CpuCount; ++i) {
        ULONG64 read = g_PerCpuData[i]->ExitGuestRax;
        if (g_PerCpuData[i]->ExitReasonValid && read == expectedRead) {
            HV_INFO("HOOK: core %lu guest read view = ORIGINAL bytes (0x%I64X)\n", i, read);
        } else {
            HV_ERROR("HOOK: core %lu guest read view = 0x%I64X (expected 0x%I64X) -> mapping broken\n",
                i, read, expectedRead);
            if (NT_SUCCESS(status)) { status = STATUS_UNSUCCESSFUL; }
        }
    }
    if (!NT_SUCCESS(status)) { HookRemoveAllCores(targetVa); return status; }

    // SECONDARY sanity: the original physical page must still be unmodified.
    if (probe[0] != 0xF3 || probe[1] != 0x0F || probe[2] != 0x1E || probe[3] != 0xFA) {
        HV_ERROR("HOOK: original page modified (execute-only mapping broken)\n");
        HookRemoveAllCores(targetVa);
        return STATUS_UNSUCCESSFUL;
    }
    HV_INFO("HOOK: original page unchanged\n");

    // Remove the hook.
    HookRemoveAllCores(targetVa);
    HV_INFO("HOOK: removed from %lu core(s)\n", g_CpuCount);

    // Control: the guest read probe must STILL return the original bytes.
    status = VmcsLaunchTestAll((ULONG64)(ULONG_PTR)HvGuestReadProbe);
    for (i = 0; i < g_CpuCount; ++i) {
        if (!g_PerCpuData[i]->ExitReasonValid ||
            g_PerCpuData[i]->ExitGuestRax != expectedRead) {
            HV_ERROR("HOOK: core %lu after-remove read = 0x%I64X (expected 0x%I64X)\n",
                i, g_PerCpuData[i]->ExitGuestRax, expectedRead);
            if (NT_SUCCESS(status)) { status = STATUS_UNSUCCESSFUL; }
        } else {
            HV_INFO("HOOK: core %lu after-remove read = ORIGINAL bytes\n", i);
        }
    }

    // Confirm the hook is gone: the normal launch returns 0xDEADBEEF again.
    status = VmcsLaunchTestAll(0);
    for (i = 0; i < g_CpuCount; ++i) {
        if (!g_PerCpuData[i]->ExitReasonValid ||
            g_PerCpuData[i]->ExitGuestRax != 0xDEADBEEFull) {
            HV_ERROR("HOOK: core %lu after-remove RAX=0x%I64X (expected 0xDEADBEEF)\n",
                i, g_PerCpuData[i]->ExitGuestRax);
            if (NT_SUCCESS(status)) { status = STATUS_UNSUCCESSFUL; }
        } else {
            HV_INFO("HOOK: core %lu guest RAX=0xDEADBEEF -> hook removed\n", i);
        }
    }
    return status;
}

static NTSTATUS HookCpuidSelfTest(VOID)
{
    ULONG i;
    ULONG64 leaf1Ecx;
    ULONG64 vendorEbx, vendorEdx, vendorEcx;
    ULONG64 signature;
    NTSTATUS status;

    for (i = 0; i < 5; ++i) { g_CpuidProbe[i] = 0; }

    // Launch the guest CPUID probe; each CPUID is serviced + spoofed in the root.
    status = VmcsLaunchTestAll((ULONG64)(ULONG_PTR)HvGuestCpuidProbe);
    if (!NT_SUCCESS(status)) {
        HV_ERROR("CPUID: probe launch failed 0x%08lX\n", (ULONG)status);
        return status;
    }
    HookDrainAllCores();   // emit the deferred CPUID event logs

    leaf1Ecx  = g_CpuidProbe[0];
    vendorEbx = g_CpuidProbe[1];
    vendorEdx = g_CpuidProbe[2];
    vendorEcx = g_CpuidProbe[3];
    signature = g_CpuidProbe[4];

    status = STATUS_SUCCESS;
    if ((leaf1Ecx & 0x80000000ull) != 0) {
        HV_ERROR("CPUID: leaf 1 ECX bit 31 SET (0x%08I64X) -> hypervisor-present not spoofed\n",
            leaf1Ecx);
        status = STATUS_UNSUCCESSFUL;
    }

    if (vendorEbx != HV_HYPERVISOR_VENDOR_EBX ||
        vendorEdx != HV_HYPERVISOR_VENDOR_EDX ||
        vendorEcx != HV_HYPERVISOR_VENDOR_ECX) {
        HV_ERROR("CPUID: leaf 0x40000000 vendor mismatch (EBX=0x%08I64X EDX=0x%08I64X ECX=0x%08I64X)\n",
            vendorEbx, vendorEdx, vendorEcx);
        status = STATUS_UNSUCCESSFUL;
    }

    if (signature != HV_HYPERVISOR_INTERFACE) {
        HV_ERROR("CPUID: leaf 0x40000001 signature mismatch (EAX=0x%08I64X)\n", signature);
        status = STATUS_UNSUCCESSFUL;
    }

    return status;
}

