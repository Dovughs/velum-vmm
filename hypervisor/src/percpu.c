// hypervisor/src/percpu.c
#include "common.h"
#include "persist.h"

NTSTATUS PerCpuAllocate(VOID)
{
    ULONG i;
    for (i = 0; i < g_CpuCount; ++i) {
        PVOID raw = ExAllocatePool2(POOL_FLAG_NON_PAGED | POOL_FLAG_CACHE_ALIGNED,
            sizeof(PER_CPU_DATA), PER_CPU_TAG);
        PPER_CPU_DATA cpu;
        if (raw == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        if (((ULONG_PTR)raw & (PER_CPU_ALIGNMENT - 1u)) != 0) {
            ExFreePoolWithTag(raw, PER_CPU_TAG);
            return STATUS_DATATYPE_MISALIGNMENT;
        }
        cpu = (PPER_CPU_DATA)raw;
        RtlZeroMemory(cpu, sizeof(PER_CPU_DATA));
        KeInitializeSpinLock(&cpu->EptLock);
        cpu->AllocationBase = cpu;
        g_PerCpuData[i] = cpu;
    }
    return STATUS_SUCCESS;
}

NTSTATUS PerCpuAllocateRegions(VOID)
{
    ULONG i;
    for (i = 0; i < g_CpuCount; ++i) {
        PPER_CPU_DATA cpu = g_PerCpuData[i];
        PHYSICAL_ADDRESS low = { 0 };
        PHYSICAL_ADDRESS high;
        PHYSICAL_ADDRESS boundary;
        boundary.QuadPart = PAGE_SIZE; // VMXON requires 4-KByte alignment (Intel SDM).
        // VMX_BASIC bit 48 is always zero on Intel 64 (Intel SDM A.1).
        high.QuadPart = MAXLONGLONG;
        cpu->VmxonVa = MmAllocateContiguousMemorySpecifyCache(
            PAGE_SIZE, low, high, boundary, MmCached);
        if (cpu->VmxonVa == NULL) {
            // VMXON must be physically contiguous; REQUIRE_CONTIGUOUS_CHUNKS enforces this.
            cpu->VmxonMdl = MmAllocatePagesForMdlEx(low, high, boundary,
                PAGE_SIZE, MmCached,
                MM_ALLOCATE_REQUIRE_CONTIGUOUS_CHUNKS | MM_ALLOCATE_FULLY_REQUIRED);
            if (cpu->VmxonMdl == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            cpu->VmxonVa = MmMapLockedPagesSpecifyCache(cpu->VmxonMdl,
                KernelMode, MmCached, NULL, FALSE,
                (ULONG)NormalPagePriority | MdlMappingNoExecute);
            if (cpu->VmxonVa == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
        }
        cpu->VmxonPa = MmGetPhysicalAddress(cpu->VmxonVa);
        if ((((ULONG_PTR)cpu->VmxonVa | (ULONG64)cpu->VmxonPa.QuadPart) &
            (PAGE_SIZE - 1u)) != 0) {
            HV_ERROR("VMX: misaligned VMXON region on core %lu\n", i);
            return STATUS_DATATYPE_MISALIGNMENT;
        }
        RtlZeroMemory(cpu->VmxonVa, PAGE_SIZE);
        // VMXON requires only the local revision ID; bit 31 stays zero.
        *(ULONG *)cpu->VmxonVa = cpu->Capabilities.RevisionId & 0x7fffffffu;
    }
    return STATUS_SUCCESS;
}

NTSTATUS PerCpuAllocateStacks(VOID)
{
    ULONG i;
    for (i = 0; i < g_CpuCount; ++i) {
        PPER_CPU_DATA cpu = g_PerCpuData[i];
        cpu->HostStackVa = ExAllocatePool2(POOL_FLAG_NON_PAGED,
            HOST_STACK_SIZE, HOST_STACK_TAG);
        if (cpu->HostStackVa == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        // Future HOST_RSP uses this separate stack, never the guest RSP.
        cpu->HostStackTop = (ULONG_PTR)cpu->HostStackVa + HOST_STACK_SIZE - 0x10u;
        if ((cpu->HostStackTop & 0xfu) != 0) {
            return STATUS_DATATYPE_MISALIGNMENT;
        }
    }
    return STATUS_SUCCESS;
}

VOID PerCpuFree(VOID)
{
    ULONG i;
    // The synchronous VMXOFF broadcast must finish before any region is freed.
    for (i = 0; i < g_CpuCount; ++i) {
        PPER_CPU_DATA cpu = g_PerCpuData[i];
        if (cpu != NULL && cpu->VmxEnabled != 0) {
            KeBugCheckEx(0x20001u, 1, i, (ULONG_PTR)cpu, 0);
        }
    }
    // Global VMXOFF makes EPT unreachable: EPT -> VMCS -> VMXON -> stacks -> CPU.
    PersistFree();
    EptFreeAll();
    // VMXOFF completed on all CPUs; discard VMCS regions before VMXON regions.
    PerCpuFreeVmcsRegions();
    for (i = 0; i < g_CpuCount; ++i) {
        PPER_CPU_DATA cpu = g_PerCpuData[i];
        if (cpu == NULL) {
            continue;
        }
        if (cpu->VmxonMdl != NULL) {
            if (cpu->VmxonVa != NULL) {
                MmUnmapLockedPages(cpu->VmxonVa, cpu->VmxonMdl);
            }
            MmFreePagesFromMdl(cpu->VmxonMdl);
            ExFreePool(cpu->VmxonMdl);
            cpu->VmxonMdl = NULL;
        } else if (cpu->VmxonVa != NULL) {
            MmFreeContiguousMemory(cpu->VmxonVa);
        }
        cpu->VmxonVa = NULL;
    }
    for (i = 0; i < g_CpuCount; ++i) {
        PPER_CPU_DATA cpu = g_PerCpuData[i];
        if (cpu != NULL && cpu->HostStackVa != NULL) {
            ExFreePoolWithTag(cpu->HostStackVa, HOST_STACK_TAG);
            cpu->HostStackVa = NULL;
        }
    }
    for (i = 0; i < g_CpuCount; ++i) {
        PPER_CPU_DATA cpu = g_PerCpuData[i];
        if (cpu != NULL) {
            ExFreePoolWithTag(cpu->AllocationBase, PER_CPU_TAG);
            g_PerCpuData[i] = NULL;
        }
    }
    g_CpuCount = 0;
}

NTSTATUS PerCpuAllocateVmcsRegions(VOID)
{
    ULONG i;
    for (i = 0; i < g_CpuCount; ++i) {
        PPER_CPU_DATA cpu = g_PerCpuData[i];
        PHYSICAL_ADDRESS low = { 0 };
        PHYSICAL_ADDRESS high;
        PHYSICAL_ADDRESS boundary;
        // SDM Vol. 3D A.1: any size in 1..4096 fits our aligned full-page allocation.
        if (cpu->Capabilities.RegionSize == 0 ||
            cpu->Capabilities.RegionSize > PAGE_SIZE) {
            HV_ERROR("VMCS: unsupported region size %lu on core %lu\n",
                cpu->Capabilities.RegionSize, i);
            return STATUS_NOT_SUPPORTED;
        }
        boundary.QuadPart = PAGE_SIZE;
        high.QuadPart = MAXLONGLONG;
        cpu->VmcsVa = MmAllocateContiguousMemorySpecifyCache(
            PAGE_SIZE, low, high, boundary, MmCached);
        if (cpu->VmcsVa == NULL) {
            cpu->VmcsMdl = MmAllocatePagesForMdlEx(low, high, boundary,
                PAGE_SIZE, MmCached,
                MM_ALLOCATE_REQUIRE_CONTIGUOUS_CHUNKS | MM_ALLOCATE_FULLY_REQUIRED);
            if (cpu->VmcsMdl == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            cpu->VmcsVa = MmMapLockedPagesSpecifyCache(cpu->VmcsMdl,
                KernelMode, MmCached, NULL, FALSE,
                (ULONG)NormalPagePriority | MdlMappingNoExecute);
            if (cpu->VmcsVa == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
        }
        cpu->VmcsPa = MmGetPhysicalAddress(cpu->VmcsVa);
        if ((((ULONG_PTR)cpu->VmcsVa | (ULONG64)cpu->VmcsPa.QuadPart) &
            (PAGE_SIZE - 1u)) != 0) {
            HV_ERROR("VMCS: misaligned region on core %lu\n", i);
            return STATUS_DATATYPE_MISALIGNMENT;
        }
        RtlZeroMemory(cpu->VmcsVa, PAGE_SIZE);
        // SDM Vol. 3C 25.2/25.10: offset 0, bit 31=0 selects an ordinary VMCS.
        // For VMXON, bit 31 must instead always be zero (25.11.5).
        *(ULONG *)cpu->VmcsVa = cpu->Capabilities.RevisionId & 0x7fffffffu;
    }
    return STATUS_SUCCESS;
}

VOID PerCpuFreeVmcsRegions(VOID)
{
    ULONG i;
    // Caller has completed VMXOFF everywhere and passed PerCpuFree's invariant.
    // SDM 25.11.1: VMXOFF may invalidate VMCS contents; none are reused here.
    for (i = 0; i < g_CpuCount; ++i) {
        PPER_CPU_DATA cpu = g_PerCpuData[i];
        if (cpu == NULL) {
            continue;
        }
        if (cpu->VmcsMdl != NULL) {
            if (cpu->VmcsVa != NULL) {
                MmUnmapLockedPages(cpu->VmcsVa, cpu->VmcsMdl);
            }
            MmFreePagesFromMdl(cpu->VmcsMdl);
            ExFreePool(cpu->VmcsMdl);
            cpu->VmcsMdl = NULL;
        } else if (cpu->VmcsVa != NULL) {
            MmFreeContiguousMemory(cpu->VmcsVa);
        }
        cpu->VmcsVa = NULL;
        cpu->VmcsPa.QuadPart = 0;
        cpu->VmcsActive = 0;
        cpu->LastVmInstructionError = 0;
        cpu->VmInstructionErrorValid = FALSE;
    }
}
