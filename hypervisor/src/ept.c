// hypervisor/src/ept.c
// Setup/allocation: PASSIVE_LEVEL. VMCS activation: pinned, <= DISPATCH_LEVEL.
// Hardware acceptance: 1/2 CPUs, expected VMCALL with EPT active, unload/reload,
// Verifier/Poolmon, partial allocation failure and invalid EPTP rollback.
// Also test 2MB coverage, RAM/MTRR edges, prepare/commit/restore, INVEPT failures.
// Default .text/.data are nonpaged; no PAGE/INIT section is used for this module.
#include "common.h"
#include "vmcs.h"
#include "ept.h"

#define EPT_POOL_TAG 'TPeH'

static PPHYSICAL_MEMORY_RANGE g_EptRam;
static ULONG g_EptRamCount;

ULONG EptInvalidateCurrent(PPER_CPU_DATA Cpu)
{
    // EPT initialization requires single-context INVEPT support. Keep this
    // narrow scope even with VPID enabled: mapping/permission changes invalidate
    // EPT-derived translations for this EPTP, across all VPIDs/PCIDs. Existing
    // callers retain their failure/rollback policy; no extra INVVPID is needed.
    return EptInveptSingleContext(Cpu->Eptp);
}

static BOOLEAN EptValidType(UCHAR Type)
{
    return Type == EPT_TYPE_UC || Type == EPT_TYPE_WC ||
        Type == EPT_TYPE_WT || Type == EPT_TYPE_WP || Type == EPT_TYPE_WB;
}

static NTSTATUS EptCaptureMtrrs(EPT_MTRR_STATE *State)
{
    int regs[4];
    ULONG bits;
    ULONG i;
    ULONG64 cap;
    __cpuid(regs, 1);
    if (((ULONG)regs[3] & (1u << 12)) == 0) { return STATUS_NOT_SUPPORTED; }
    __cpuid(regs, (int)0x80000000u);
    if ((ULONG)regs[0] < 0x80000008u) { return STATUS_NOT_SUPPORTED; }
    __cpuid(regs, (int)0x80000008u);
    bits = (ULONG)regs[0] & 0xFFu;
    if (bits < 32 || bits > 52) { return STATUS_NOT_SUPPORTED; }
    State->PhysicalLimit = 1ull << bits;
    State->PhysicalMask = (State->PhysicalLimit - 1ull) & EPT_ADDRESS_MASK;
    __try {
        cap = __readmsr(EPT_MSR_MTRRCAP);
        State->Default = __readmsr(EPT_MSR_MTRR_DEFAULT);
        State->VariableCount = (ULONG)(cap & 0xFFu);
        State->FixedSupported = (cap & EPT_MTRR_FIXED_SUPPORTED) != 0;
        if ((State->Default & EPT_MTRR_ENABLE) == 0) { return STATUS_SUCCESS; }
        if (!EptValidType((UCHAR)State->Default)) { return STATUS_NOT_SUPPORTED; }
        if (State->FixedSupported && (State->Default & EPT_MTRR_FIXED_ENABLE) != 0) {
            State->Fixed[0] = __readmsr(EPT_MSR_MTRR_FIXED64);
            for (i = 0; i < 2; ++i) {
                State->Fixed[1 + i] = __readmsr(EPT_MSR_MTRR_FIXED16 + i);
            }
            for (i = 0; i < 8; ++i) {
                State->Fixed[3 + i] = __readmsr(EPT_MSR_MTRR_FIXED4 + i);
            }
        }
        for (i = 0; i < State->VariableCount; ++i) {
            ULONG64 base = __readmsr(EPT_MSR_MTRR_BASE0 + 2u * i);
            ULONG64 mask = __readmsr(EPT_MSR_MTRR_BASE0 + 2u * i + 1u);
            ULONG64 size;
            EPT_MTRR_RANGE *range = &State->Variable[i];
            range->Valid = (mask & EPT_MTRR_VALID) != 0;
            if (!range->Valid) { continue; }
            range->Type = (UCHAR)base;
            range->Base = base & State->PhysicalMask;
            range->Mask = mask & State->PhysicalMask;
            size = ((~range->Mask) & State->PhysicalMask) + PAGE_SIZE;
            if (!EptValidType(range->Type) || (size & (size - 1ull)) != 0 ||
                (range->Base & (size - 1ull)) != 0) {
                return STATUS_NOT_SUPPORTED;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return STATUS_NOT_SUPPORTED;
    }
    return STATUS_SUCCESS;
}

// Memory-type policy hook. Only OS RAM reaches here; MMIO remains unmapped.
// SDM MTRR precedence: fixed first; UC wins; WT+WB => WT; other mixtures undefined.
static NTSTATUS EptMemoryType(const EPT_MTRR_STATE *State, ULONG64 Pa, UCHAR *Type)
{
    ULONG types = 0;
    ULONG i;
    if ((State->Default & EPT_MTRR_ENABLE) == 0) {
        *Type = EPT_TYPE_UC;
        return STATUS_SUCCESS;
    }
    if (Pa < 0x100000ull && State->FixedSupported &&
        (State->Default & EPT_MTRR_FIXED_ENABLE) != 0) {
        ULONG reg;
        ULONG byte;
        if (Pa < 0x80000ull) {
            reg = 0;
            byte = (ULONG)(Pa >> 16);
        } else if (Pa < 0xC0000ull) {
            reg = 1u + (ULONG)((Pa - 0x80000ull) >> 17);
            byte = (ULONG)((Pa >> 14) & 7ull);
        } else {
            reg = 3u + (ULONG)((Pa - 0xC0000ull) >> 15);
            byte = (ULONG)((Pa >> 12) & 7ull);
        }
        *Type = (UCHAR)(State->Fixed[reg] >> (byte * 8u));
        return EptValidType(*Type) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
    }
    for (i = 0; i < State->VariableCount; ++i) {
        const EPT_MTRR_RANGE *range = &State->Variable[i];
        if (range->Valid && (Pa & range->Mask) == range->Base) {
            types |= 1u << range->Type;
        }
    }
    if (types == 0) { *Type = (UCHAR)State->Default; }
    else if ((types & (1u << EPT_TYPE_UC)) != 0) { *Type = EPT_TYPE_UC; }
    else if (types == ((1u << EPT_TYPE_WB) | (1u << EPT_TYPE_WT))) { *Type = EPT_TYPE_WT; }
    else if ((types & (types - 1u)) == 0) {
        for (i = 0; i <= EPT_TYPE_WB; ++i) {
            if (types == (1u << i)) { *Type = (UCHAR)i; break; }
        }
    } else { return STATUS_NOT_SUPPORTED; }
    return EptValidType(*Type) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

static VOID EptReleasePage(EPT_PAGE *Page)
{
    if (Page->Mdl != NULL) {
        if (Page->Va != NULL) { MmUnmapLockedPages(Page->Va, Page->Mdl); }
        MmFreePagesFromMdl(Page->Mdl);
        ExFreePool(Page->Mdl);
    } else if (Page->Va != NULL) {
        MmFreeContiguousMemory(Page->Va);
    }
    ExFreePoolWithTag(Page, EPT_POOL_TAG);
}

static NTSTATUS EptAllocateDetachedPage(const EPT_MTRR_STATE *Mtrr, EPT_PAGE **Page)
{
    PHYSICAL_ADDRESS low = { 0 };
    PHYSICAL_ADDRESS high;
    PHYSICAL_ADDRESS boundary;
    EPT_PAGE *page = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*page), EPT_POOL_TAG);
    if (page == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    high.QuadPart = (LONGLONG)(Mtrr->PhysicalLimit - 1ull);
    boundary.QuadPart = PAGE_SIZE;
    page->Va = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, low, high,
        boundary, MmCached);
    if (page->Va == NULL) {
        page->Mdl = MmAllocatePagesForMdlEx(low, high, boundary, PAGE_SIZE, MmCached,
            MM_ALLOCATE_REQUIRE_CONTIGUOUS_CHUNKS | MM_ALLOCATE_FULLY_REQUIRED);
        if (page->Mdl == NULL) { EptReleasePage(page); return STATUS_INSUFFICIENT_RESOURCES; }
        page->Va = MmMapLockedPagesSpecifyCache(page->Mdl, KernelMode, MmCached,
            NULL, FALSE, (ULONG)NormalPagePriority | MdlMappingNoExecute);
        if (page->Va == NULL) { EptReleasePage(page); return STATUS_INSUFFICIENT_RESOURCES; }
    }
    page->Pa = MmGetPhysicalAddress(page->Va);
    if ((((ULONG_PTR)page->Va | (ULONG64)page->Pa.QuadPart) & (PAGE_SIZE - 1u)) != 0 ||
        ((ULONG64)page->Pa.QuadPart & ~Mtrr->PhysicalMask) != 0) {
        HV_ERROR("EPT: invalid table alignment/address VA=%p PA=0x%I64X\n",
            page->Va, (ULONG64)page->Pa.QuadPart);
        EptReleasePage(page);
        return STATUS_DATATYPE_MISALIGNMENT;
    }
    RtlZeroMemory(page->Va, PAGE_SIZE);
    *Page = page;
    return STATUS_SUCCESS;
}

static VOID EptAttachPage(EPT_CONTEXT *Context, EPT_PAGE *Page)
{
    Page->Next = Context->Pages;
    Context->Pages = Page;
    ++Context->TableCount;
}

static NTSTATUS EptAllocatePage(EPT_CONTEXT *Context, EPT_PAGE **Page)
{
    NTSTATUS status = EptAllocateDetachedPage(&Context->Mtrr, Page);
    if (NT_SUCCESS(status)) { EptAttachPage(Context, *Page); }
    return status;
}

static NTSTATUS EptBuildPerCore(PPER_CPU_DATA Cpu)
{
    EPT_CONTEXT *ctx;
    EPT_PAGE *pdpt = NULL;
    EPT_PAGE *pd = NULL;
    EPT_PAGE *pt = NULL;
    ULONG64 last39 = MAXULONGLONG;
    ULONG64 last30 = MAXULONGLONG;
    ULONG64 last21 = MAXULONGLONG;
    ULONG r;
    NTSTATUS status;
    if (Cpu->Ept != NULL || g_EptRam == NULL) { return STATUS_INVALID_PARAMETER; }
    if (!Cpu->Capabilities.EptWb || !Cpu->Capabilities.EptWalk4 ||
        (Cpu->Capabilities.EptVpidCap & EPT_CAP_2MB) == 0 ||
        !Cpu->Capabilities.EptSupported || !Cpu->Capabilities.Invept ||
        !Cpu->Capabilities.InveptSingle ||
        (Cpu->Capabilities.Primary.Allowed1 & PRIMARY_SECONDARY_CONTROLS) == 0 ||
        (Cpu->Capabilities.Secondary.Allowed0 & ~SECONDARY_ENABLE_EPT) != 0) {
        HV_ERROR("EPT: required WB/four-level/2MB/single-context/control capabilities unavailable\n");
        return STATUS_NOT_SUPPORTED;
    }
    ctx = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*ctx), EPT_POOL_TAG);
    if (ctx == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    Cpu->Ept = ctx;
    status = EptCaptureMtrrs(&ctx->Mtrr);
    if (!NT_SUCCESS(status)) { return status; }
    status = EptAllocatePage(ctx, &ctx->Root);
    if (!NT_SUCCESS(status)) { return status; }
    for (r = 0; r < g_EptRamCount; ++r) {
        ULONG64 pa = (ULONG64)g_EptRam[r].BaseAddress.QuadPart;
        ULONG64 end = pa + (ULONG64)g_EptRam[r].NumberOfBytes.QuadPart;
        if (end > ctx->Mtrr.PhysicalLimit || end > EPT_GPA_LIMIT) {
            return STATUS_NOT_SUPPORTED;
        }
        while (pa < end) {
            ULONG64 region = pa & ~(EPT_LARGE_SIZE - 1ull);
            ULONG64 stop = end < region + EPT_LARGE_SIZE ? end : region + EPT_LARGE_SIZE;
            ULONG64 cursor;
            UCHAR types[EPT_TABLE_ENTRIES];
            UCHAR firstType = EPT_TYPE_UC;
            BOOLEAN homogeneous = TRUE;
            ULONG64 *pde;
            if ((pa >> EPT_PML4_SHIFT) != last39) {
                status = EptAllocatePage(ctx, &pdpt);
                if (!NT_SUCCESS(status)) { return status; }
                last39 = pa >> EPT_PML4_SHIFT;
                ((ULONG64 *)ctx->Root->Va)[last39] = (ULONG64)pdpt->Pa.QuadPart | EPT_ENTRY_RWX;
            }
            if ((pa >> EPT_PML3_SHIFT) != last30) {
                status = EptAllocatePage(ctx, &pd);
                if (!NT_SUCCESS(status)) { return status; }
                last30 = pa >> EPT_PML3_SHIFT;
                ((ULONG64 *)pdpt->Va)[last30 & EPT_INDEX_MASK] = (ULONG64)pd->Pa.QuadPart | EPT_ENTRY_RWX;
            }
            pde = &((ULONG64 *)pd->Va)[(pa >> EPT_PML2_SHIFT) & EPT_INDEX_MASK];
            for (cursor = pa; cursor < stop; cursor += PAGE_SIZE) {
                ULONG index = (ULONG)((cursor - region) >> EPT_PML1_SHIFT);
                status = EptMemoryType(&ctx->Mtrr, cursor, &types[index]);
                if (!NT_SUCCESS(status)) {
                    HV_ERROR("EPT: undefined MTRR combination at PA=0x%I64X\n", cursor);
                    return status;
                }
                if (cursor == pa) { firstType = types[index]; }
                else if (types[index] != firstType) { homogeneous = FALSE; }
            }
            // SDM 29.3.2/29.3.7: one large leaf has ONE memory type. RAM holes
            // and mixed MTRRs require 4KB leaves; never map the surrounding hole.
            if (pa == region && stop - pa == EPT_LARGE_SIZE && homogeneous) {
                *pde = pa | EPT_ENTRY_RWX | EPT_ENTRY_LARGE_PAGE |
                    ((ULONG64)firstType << EPT_ENTRY_TYPE_SHIFT);
                ++ctx->Statistics.LargePageCount;
                ctx->LeafCount += EPT_TABLE_ENTRIES;
                if (firstType != EPT_TYPE_WB) { ctx->NonWbCount += EPT_TABLE_ENTRIES; }
                pa = stop;
                continue;
            }
            if ((pa >> EPT_PML2_SHIFT) != last21) {
                EPT_SPLIT *split;
                status = EptAllocatePage(ctx, &pt);
                if (!NT_SUCCESS(status)) { return status; }
                split = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*split), EPT_POOL_TAG);
                if (split == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
                split->RegionBase = region;
                split->Pml1 = pt;
                split->Pml1Pa = (ULONG64)pt->Pa.QuadPart;
                split->State = EptSplitPermanent;
                split->Next = ctx->Splits;
                ctx->Splits = split;
                last21 = pa >> EPT_PML2_SHIFT;
                *pde = split->Pml1Pa | EPT_ENTRY_RWX;
                ++ctx->Statistics.SplitPageCount;
            }
            for (; pa < stop; pa += PAGE_SIZE) {
                UCHAR type = types[(pa - region) >> EPT_PML1_SHIFT];
                // Keep ignore-PAT clear: guest PAT still participates (SDM 29.3.7).
                ((ULONG64 *)pt->Va)[(pa >> EPT_PML1_SHIFT) & EPT_INDEX_MASK] =
                    pa | EPT_ENTRY_RWX | ((ULONG64)type << EPT_ENTRY_TYPE_SHIFT);
                ++ctx->LeafCount;
                ++ctx->Statistics.SplitLeafCount;
                if (type != EPT_TYPE_WB) { ++ctx->NonWbCount; }
            }
        }
    }
    Cpu->EptPml4Pa = ctx->Root->Pa;
    Cpu->Eptp = EPTP_BUILD(Cpu->EptPml4Pa.QuadPart, EPT_TYPE_WB, 4, FALSE);
    return STATUS_SUCCESS;
}

// Never spin at IPI_LEVEL: the interrupted thread might own this very lock.
// Raising only below DISPATCH also pins a PASSIVE caller before checking ownership.
static BOOLEAN EptTryLock(PPER_CPU_DATA Cpu, KIRQL *PreviousIrql)
{
    *PreviousIrql = KeGetCurrentIrql();
    if (*PreviousIrql < DISPATCH_LEVEL) { KeRaiseIrql(DISPATCH_LEVEL, PreviousIrql); }
    if (KeTryToAcquireSpinLockAtDpcLevel(&Cpu->EptLock)) { return TRUE; }
    if (*PreviousIrql < DISPATCH_LEVEL) { KeLowerIrql(*PreviousIrql); }
    return FALSE;
}

static VOID EptUnlock(PPER_CPU_DATA Cpu, KIRQL PreviousIrql)
{
    KeReleaseSpinLockFromDpcLevel(&Cpu->EptLock);
    if (PreviousIrql < DISPATCH_LEVEL) { KeLowerIrql(PreviousIrql); }
}

static BOOLEAN EptIsOwner(PPER_CPU_DATA Cpu)
{
    ULONG index = KeGetCurrentProcessorNumber();
    return index < g_CpuCount && g_PerCpuData[index] == Cpu && Cpu->VmxEnabled != 0;
}

static EPT_PAGE *EptFindPage(EPT_CONTEXT *Context, ULONG64 Pa)
{
    EPT_PAGE *page;
    for (page = Context->Pages; page != NULL; page = page->Next) {
        if ((ULONG64)page->Pa.QuadPart == Pa) { return page; }
    }
    return NULL;
}

static ULONG64 *EptFindPde(EPT_CONTEXT *Context, ULONG64 Gpa)
{
    EPT_PAGE *table = Context->Root;
    ULONG64 entry;
    if (Gpa >= EPT_GPA_LIMIT || table == NULL) { return NULL; }
    entry = ((ULONG64 *)table->Va)[Gpa >> EPT_PML4_SHIFT];
    if (!EPT_ENTRY_PRESENT(entry)) { return NULL; }
    table = EptFindPage(Context, EPT_ENTRY_ADDRESS(entry));
    if (table == NULL) { return NULL; }
    entry = ((ULONG64 *)table->Va)[(Gpa >> EPT_PML3_SHIFT) & EPT_INDEX_MASK];
    if (!EPT_ENTRY_PRESENT(entry) || (entry & EPT_ENTRY_LARGE_PAGE) != 0) { return NULL; }
    table = EptFindPage(Context, EPT_ENTRY_ADDRESS(entry));
    return table == NULL ? NULL : &((ULONG64 *)table->Va)[(Gpa >> EPT_PML2_SHIFT) & EPT_INDEX_MASK];
}

// Reserve table pages at PASSIVE_LEVEL; VM exits may never allocate or wait.
NTSTATUS EptPrepareRuntime(PPER_CPU_DATA Cpu)
{
    EPT_CONTEXT *ctx = Cpu->Ept;
    NTSTATUS status;
    while (ctx->RuntimeAvailable < RTL_NUMBER_OF(ctx->RuntimePages)) {
        EPT_PAGE *page;
        status = EptAllocatePage(ctx, &page);
        if (!NT_SUCCESS(status)) { return status; }
        ctx->RuntimePages[ctx->RuntimeAvailable++] = page;
    }
    return STATUS_SUCCESS;
}

// Identity-map only absent leaves. Existing RAM/cache types and hook permissions
// are never replaced. Device/firmware holes use UC; guest PAT cannot make UC WB.
NTSTATUS EptMapRuntimeHole(PPER_CPU_DATA Cpu, ULONG64 Gpa)
{
    static const ULONG shifts[] = { 39, 30, 21, 12 };
    EPT_CONTEXT *ctx = Cpu->Ept;
    EPT_PAGE *table = ctx->Root;
    ULONG level;
    if (Gpa >= EPT_GPA_LIMIT || Gpa >= ctx->Mtrr.PhysicalLimit) {
        return STATUS_INVALID_PARAMETER;
    }
    for (level = 0; level < 4; ++level) {
        ULONG64 *entry = &((ULONG64 *)table->Va)[(Gpa >> shifts[level]) & EPT_INDEX_MASK];
        if (EPT_ENTRY_PRESENT(*entry)) {
            if (level == 3 || (*entry & EPT_ENTRY_LARGE_PAGE) != 0) {
                return STATUS_ACCESS_DENIED;
            }
            table = EptFindPage(ctx, EPT_ENTRY_ADDRESS(*entry));
            if (table == NULL) { return STATUS_INVALID_DEVICE_STATE; }
        } else if (level == 2 || level == 3) {
            ULONG64 mask = level == 2 ? EPT_LARGE_ADDRESS_MASK : EPT_ADDRESS_MASK;
            *entry = (Gpa & mask) | EPT_ENTRY_RWX |
                (level == 2 ? EPT_ENTRY_LARGE_PAGE : 0);
            return EptInvalidateCurrent(Cpu) == 0 ?
                STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        } else {
            if (ctx->RuntimeAvailable == 0) { return STATUS_INSUFFICIENT_RESOURCES; }
            table = ctx->RuntimePages[--ctx->RuntimeAvailable];
            *entry = (ULONG64)table->Pa.QuadPart | EPT_ENTRY_RWX;
        }
    }
    return STATUS_UNSUCCESSFUL;
}

static EPT_SPLIT *EptFindSplit(EPT_CONTEXT *Context, ULONG64 Gpa)
{
    EPT_SPLIT *split;
    ULONG64 region = Gpa & ~(EPT_LARGE_SIZE - 1ull);
    for (split = Context->Splits; split != NULL; split = split->Next) {
        if (split->RegionBase == region) { return split; }
    }
    return NULL;
}

static ULONG64 EptSplitLeaf(ULONG64 OriginalPde, ULONG Index)
{
    // SDM 29.3.2 Tables 29-4/29-6, 29.3.5: preserve PA/RWX/type/ignore-PAT.
    // A/D are disabled. New leaves clear bits 7,8,9 and all unused features.
    return ((OriginalPde & EPT_LARGE_ADDRESS_MASK) + (ULONG64)Index * PAGE_SIZE) |
        (OriginalPde & EPT_LEAF_ATTRIBUTES);
}

NTSTATUS EptPrepareSplit(PPER_CPU_DATA Cpu, ULONG64 Gpa)
{
    EPT_CONTEXT *ctx;
    EPT_SPLIT *split;
    EPT_SPLIT *existing;
    ULONG64 *pde;
    ULONG64 original;
    ULONG i;
    KIRQL irql;
    NTSTATUS status;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || Cpu == NULL || Cpu->Ept == NULL ||
        Gpa >= EPT_GPA_LIMIT) { return STATUS_INVALID_PARAMETER; }
    ctx = Cpu->Ept;
    if ((Cpu->Eptp & EPTP_AD_ENABLE) != 0) { return STATUS_NOT_SUPPORTED; }
    if (!EptTryLock(Cpu, &irql)) { return STATUS_DEVICE_BUSY; }
    pde = EptFindPde(ctx, Gpa);
    existing = EptFindSplit(ctx, Gpa);
    status = STATUS_SUCCESS;
    if (ctx->InvalidationPending != NULL) { status = STATUS_DEVICE_BUSY; }
    else if (pde == NULL || !EPT_ENTRY_PRESENT(*pde)) { status = STATUS_INVALID_PARAMETER; }
    else if (existing != NULL) {
        if (existing->State == EptSplitRetired) { status = STATUS_DEVICE_BUSY; }
    } else if ((*pde & EPT_ENTRY_LARGE_PAGE) == 0) { status = STATUS_INVALID_PARAMETER; }
    original = pde == NULL ? 0 : *pde;
    EptUnlock(Cpu, irql);
    if (!NT_SUCCESS(status) || existing != NULL) { return status; }
    // Only current Step-5 features are supported; do not silently lose later controls.
    if ((original & ~(EPT_LARGE_ADDRESS_MASK | EPT_LEAF_ATTRIBUTES | EPT_ENTRY_LARGE_PAGE |
        EPT_ENTRY_ACCESSED | EPT_ENTRY_DIRTY)) != 0) { return STATUS_NOT_SUPPORTED; }
    split = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*split), EPT_POOL_TAG);
    if (split == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    status = EptAllocateDetachedPage(&ctx->Mtrr, &split->Pml1);
    if (!NT_SUCCESS(status)) { ExFreePoolWithTag(split, EPT_POOL_TAG); return status; }
    split->RegionBase = Gpa & ~(EPT_LARGE_SIZE - 1ull);
    split->OriginalPde = original;
    split->Pml1Pa = (ULONG64)split->Pml1->Pa.QuadPart;
    split->State = EptSplitPrepared;
    for (i = 0; i < EPT_TABLE_ENTRIES; ++i) {
        ((ULONG64 *)split->Pml1->Va)[i] = EptSplitLeaf(original, i);
    }
    status = STATUS_DEVICE_BUSY;
    if (EptTryLock(Cpu, &irql)) {
        pde = EptFindPde(ctx, Gpa);
        if (ctx->InvalidationPending == NULL && EptFindSplit(ctx, Gpa) == NULL &&
            pde != NULL && *pde == original) {
            EptAttachPage(ctx, split->Pml1);
            split->Next = ctx->Splits;
            ctx->Splits = split;
            status = STATUS_SUCCESS;
        }
        EptUnlock(Cpu, irql);
    }
    if (!NT_SUCCESS(status)) {
        EptReleasePage(split->Pml1);
        ExFreePoolWithTag(split, EPT_POOL_TAG);
    }
    return status;
}

static NTSTATUS EptInvalidateSplit(PPER_CPU_DATA Cpu, EPT_SPLIT *Split)
{
    Split->LastInveptStatus = EptInvalidateCurrent(Cpu);
    Cpu->Ept->InvalidationPending = Split->LastInveptStatus == 0 ? NULL : Split;
    return Split->LastInveptStatus == 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

// Prepare once per CPU at PASSIVE before the IPI. Commit on the owner in VMX root.
// No allocations, frees, logging or blocking in this call graph. SDM 29.4.3:
// publish the complete table atomically, then INVEPT under the lock before reuse.
// Any failure forbids guest re-entry until a successful retry or global VMXOFF.
NTSTATUS EptSplitLargePage(PPER_CPU_DATA Cpu, ULONG64 Gpa)
{
    EPT_CONTEXT *ctx;
    EPT_SPLIT *split;
    ULONG64 *pde;
    KIRQL irql;
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    if (Cpu == NULL || Cpu->Ept == NULL || Gpa >= EPT_GPA_LIMIT) { return status; }
    if ((Cpu->Eptp & EPTP_AD_ENABLE) != 0) { return STATUS_NOT_SUPPORTED; }
    if (!EptTryLock(Cpu, &irql)) { return STATUS_DEVICE_BUSY; }
    ctx = Cpu->Ept;
    split = EptFindSplit(ctx, Gpa);
    pde = EptFindPde(ctx, Gpa);
    if (!EptIsOwner(Cpu)) { status = STATUS_INVALID_DEVICE_STATE; }
    else if (ctx->InvalidationPending != NULL && ctx->InvalidationPending != split) {
        status = STATUS_DEVICE_BUSY;
    } else if (split != NULL && pde != NULL) {
        if (split->State == EptSplitPrepared && *pde == split->OriginalPde) {
            // Table 29-5: preserve RWX; replace PA; clear bits 3..7, A/D and all
            // unused bits. Ignore-PAT and memory type belong only in the leaves.
            (VOID)InterlockedExchange64((volatile LONG64 *)pde,
                (LONG64)(split->Pml1Pa | (split->OriginalPde & EPT_ENTRY_RWX)));
            split->State = EptSplitActive;
            --ctx->Statistics.LargePageCount;
            ++ctx->Statistics.SplitPageCount;
            ctx->Statistics.SplitLeafCount += EPT_TABLE_ENTRIES;
            status = EptInvalidateSplit(Cpu, split);
        } else if ((split->State == EptSplitActive || split->State == EptSplitPermanent) &&
            (*pde & EPT_ENTRY_LARGE_PAGE) == 0 && EPT_ENTRY_ADDRESS(*pde) == split->Pml1Pa) {
            status = ctx->InvalidationPending == split ? EptInvalidateSplit(Cpu, split) : STATUS_SUCCESS;
        } else { status = STATUS_DEVICE_BUSY; }
    } else if (pde != NULL && (*pde & EPT_ENTRY_LARGE_PAGE) != 0) {
        status = STATUS_INVALID_DEVICE_STATE; // Prepare was not called.
    }
    EptUnlock(Cpu, irql);
    return status;
}

// Caller holds EptLock, is on the owning CPU, and must not retain the pointer
// after unlocking. Any later leaf modification also requires local INVEPT.
ULONG64 *EptGetPml1Entry(PPER_CPU_DATA Cpu, ULONG64 Gpa)
{
    EPT_SPLIT *split;
    ULONG64 *pde;
    if (Cpu == NULL || Cpu->Ept == NULL || Gpa >= EPT_GPA_LIMIT ||
        !EptIsOwner(Cpu) || Cpu->Ept->InvalidationPending != NULL) { return NULL; }
    split = EptFindSplit(Cpu->Ept, Gpa);
    pde = EptFindPde(Cpu->Ept, Gpa);
    if (split == NULL || pde == NULL || (*pde & EPT_ENTRY_LARGE_PAGE) != 0 ||
        EPT_ENTRY_ADDRESS(*pde) != split->Pml1Pa ||
        (split->State != EptSplitActive && split->State != EptSplitPermanent)) { return NULL; }
    return &((ULONG64 *)split->Pml1->Va)[(Gpa >> EPT_PML1_SHIFT) & EPT_INDEX_MASK];
}

NTSTATUS EptRestoreLargePage(PPER_CPU_DATA Cpu, ULONG64 Gpa)
{
    EPT_CONTEXT *ctx;
    EPT_SPLIT *split;
    ULONG64 *pde;
    ULONG i;
    KIRQL irql;
    NTSTATUS status = STATUS_SUCCESS;
    if (Cpu == NULL || Cpu->Ept == NULL || Gpa >= EPT_GPA_LIMIT) { return STATUS_INVALID_PARAMETER; }
    if ((Cpu->Eptp & EPTP_AD_ENABLE) != 0) { return STATUS_NOT_SUPPORTED; }
    if (!EptTryLock(Cpu, &irql)) { return STATUS_DEVICE_BUSY; }
    ctx = Cpu->Ept;
    split = EptFindSplit(ctx, Gpa);
    pde = EptFindPde(ctx, Gpa);
    if (!EptIsOwner(Cpu)) { status = STATUS_INVALID_DEVICE_STATE; goto DoneRestore; }
    if (ctx->InvalidationPending != NULL && ctx->InvalidationPending != split) {
        status = STATUS_DEVICE_BUSY; goto DoneRestore;
    }
    if (split == NULL || split->State == EptSplitRetired) { goto DoneRestore; }
    if (split->State == EptSplitPermanent) { status = STATUS_NOT_SUPPORTED; goto DoneRestore; }
    if (pde == NULL) { status = STATUS_INVALID_DEVICE_STATE; goto DoneRestore; }
    if (split->State == EptSplitPrepared) {
        split->State = EptSplitRetired; // Never published: no translation can reference it.
        goto DoneRestore;
    }
    if (split->State == EptSplitActive) {
        ULONG64 expected = split->Pml1Pa | (split->OriginalPde & EPT_ENTRY_RWX);
        if (*pde != expected) { status = STATUS_DEVICE_BUSY; goto DoneRestore; }
        for (i = 0; i < EPT_TABLE_ENTRIES; ++i) {
            if (((ULONG64 *)split->Pml1->Va)[i] != EptSplitLeaf(split->OriginalPde, i)) {
                status = STATUS_DEVICE_BUSY; goto DoneRestore; // A mapping/permission is still modified.
            }
        }
        (VOID)InterlockedExchange64((volatile LONG64 *)pde, (LONG64)split->OriginalPde);
        split->State = EptSplitRetiring;
        ++ctx->Statistics.LargePageCount;
        --ctx->Statistics.SplitPageCount;
        ctx->Statistics.SplitLeafCount -= EPT_TABLE_ENTRIES;
    }
    if (*pde != split->OriginalPde) { status = STATUS_DEVICE_BUSY; goto DoneRestore; }
    // SDM 29.4.3: retain the old page until invalidation completes successfully.
    status = EptInvalidateSplit(Cpu, split);
    if (NT_SUCCESS(status)) { split->State = EptSplitRetired; }
DoneRestore:
    EptUnlock(Cpu, irql);
    return status;
}

NTSTATUS EptReclaimRetired(PPER_CPU_DATA Cpu)
{
    EPT_SPLIT **link;
    EPT_SPLIT *retired = NULL;
    KIRQL irql;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || Cpu == NULL || Cpu->Ept == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!EptTryLock(Cpu, &irql)) { return STATUS_DEVICE_BUSY; }
    for (link = &Cpu->Ept->Splits; *link != NULL;) {
        EPT_SPLIT *split = *link;
        if (split->State == EptSplitRetired) {
            EPT_PAGE **pageLink;
            for (pageLink = &Cpu->Ept->Pages; *pageLink != NULL; pageLink = &(*pageLink)->Next) {
                if (*pageLink == split->Pml1) { *pageLink = split->Pml1->Next; break; }
            }
            --Cpu->Ept->TableCount;
            *link = split->Next;
            split->Next = retired;
            retired = split;
        } else { link = &split->Next; }
    }
    EptUnlock(Cpu, irql);
    while (retired != NULL) {
        EPT_SPLIT *split = retired;
        retired = split->Next;
        EptReleasePage(split->Pml1);
        ExFreePoolWithTag(split, EPT_POOL_TAG);
    }
    return STATUS_SUCCESS;
}

static ULONG EptCpuIndex(PPER_CPU_DATA Cpu)
{
    ULONG i;
    for (i = 0; i < g_CpuCount; ++i) { if (g_PerCpuData[i] == Cpu) { return i; } }
    return g_CpuCount;
}

VOID EptLogStatistics(PPER_CPU_DATA Cpu)
{
    ULONG64 large, split, small, tables, leaves, nonWb;
    KIRQL irql;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || Cpu == NULL || Cpu->Ept == NULL) { return; }
    if (!EptTryLock(Cpu, &irql)) { return; }
    large = Cpu->Ept->Statistics.LargePageCount;
    split = Cpu->Ept->Statistics.SplitPageCount;
    small = Cpu->Ept->Statistics.SplitLeafCount;
    tables = Cpu->Ept->TableCount;
    leaves = Cpu->Ept->LeafCount;
    nonWb = Cpu->Ept->NonWbCount;
    EptUnlock(Cpu, irql);
    HV_INFO("EPT: core %lu large-2MB=%I64u split-tables=%I64u leaves-4KB=%I64u tables=%I64u mapped-4KB=%I64u non-WB=%I64u\n",
        EptCpuIndex(Cpu), large, split, small, tables, leaves, nonWb);
}

// Call after the IPI at PASSIVE: DbgPrintEx is not part of the IPI call graph.
VOID EptLogSplitResult(PPER_CPU_DATA Cpu, ULONG64 Gpa, NTSTATUS Result)
{
    EPT_SPLIT *split;
    PCSTR state = "none";
    ULONG instruction = 0;
    KIRQL irql;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || Cpu == NULL || Cpu->Ept == NULL) { return; }
    if (!EptTryLock(Cpu, &irql)) { return; }
    split = EptFindSplit(Cpu->Ept, Gpa);
    if (split != NULL) {
        instruction = split->LastInveptStatus;
        switch (split->State) {
        case EptSplitPermanent: state = "permanent-4KB"; break;
        case EptSplitPrepared: state = "prepared"; break;
        case EptSplitActive: state = "split-active"; break;
        case EptSplitRetiring: state = "restore-awaiting-INVEPT"; break;
        case EptSplitRetired: state = "retired-awaiting-free"; break;
        default: state = "invalid"; break;
        }
    }
    EptUnlock(Cpu, irql);
    HV_INFO("EPT: core %lu region=0x%I64X split/restore status=0x%08lX state=%s INVEPT=%lu\n",
        EptCpuIndex(Cpu), Gpa & ~(EPT_LARGE_SIZE - 1ull), (ULONG)Result, state, instruction);
}

ULONG64 EptInitializePerCore(PPER_CPU_DATA Cpu)
{
    NTSTATUS status = EptBuildPerCore(Cpu);
    if (!NT_SUCCESS(status)) {
        HV_ERROR("EPT: core %lu table construction failed 0x%08lX\n",
            KeGetCurrentProcessorNumber(), (ULONG)status);
        return 0;
    }
    return Cpu->Eptp;
}

static NTSTATUS EptInstructionStatus(ULONG Result, PCSTR Operation)
{
    ULONG error;
    if (Result == 0) { return STATUS_SUCCESS; }
    if (Result == 1 && VmcsQueryInstructionError(&error) == 0) {
        HV_ERROR("EPT: %s VMfailValid; VM_INSTRUCTION_ERROR=%lu\n", Operation, error);
    } else {
        HV_ERROR("EPT: %s failed, status=%lu; instruction error unavailable\n", Operation, Result);
    }
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS EptWriteEptpToVmcs(PPER_CPU_DATA Cpu)
{
    ULONG64 value = 0;
    NTSTATUS status;
    if (Cpu->Eptp == 0 || Cpu->VmcsActive == 0) { return STATUS_INVALID_PARAMETER; }
    // SDM 25.11.2: full-width VMWRITE in 64-bit mode writes all 64 bits; no HIGH write.
    status = EptInstructionStatus(VmxWrite(VMCS_EPT_POINTER, Cpu->Eptp), "VMWRITE(EPTP)");
    if (!NT_SUCCESS(status)) { return status; }
    status = EptInstructionStatus(VmxRead(VMCS_EPT_POINTER, &value), "VMREAD(EPTP)");
    if (!NT_SUCCESS(status)) { return status; }
    if (value != Cpu->Eptp) {
        HV_ERROR("EPT: EPTP readback mismatch expected=0x%I64X actual=0x%I64X\n", Cpu->Eptp, value);
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS EptEnableInVmcs(PPER_CPU_DATA Cpu)
{
    ULONG64 primary = 0;
    ULONG64 before = 0;
    ULONG64 after = 0;
    ULONG adjusted;
    NTSTATUS status = EptWriteEptpToVmcs(Cpu);
    if (!NT_SUCCESS(status)) { return status; }
    status = EptInstructionStatus(VmxRead(VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS, &primary), "VMREAD(primary)");
    if (!NT_SUCCESS(status)) { return status; }
    if ((primary & PRIMARY_SECONDARY_CONTROLS) == 0) { return STATUS_NOT_SUPPORTED; }
    status = EptInstructionStatus(VmxRead(VMCS_SECONDARY_PROCESSOR_BASED_CONTROLS, &before), "VMREAD(secondary)");
    if (!NT_SUCCESS(status)) { return status; }
    adjusted = (((ULONG)before | SECONDARY_ENABLE_EPT) & Cpu->Capabilities.Secondary.Allowed1) |
        Cpu->Capabilities.Secondary.Allowed0;
    if (adjusted != SECONDARY_ENABLE_EPT || (before & ~(ULONG64)SECONDARY_ENABLE_EPT) != 0) {
        return STATUS_NOT_SUPPORTED;
    }
    // SDM 27.2.1.1/31.4: enabling EPT with invalid EPTP fails VM entry (error 7).
    status = EptInstructionStatus(VmxWrite(VMCS_SECONDARY_PROCESSOR_BASED_CONTROLS, adjusted), "VMWRITE(secondary)");
    if (!NT_SUCCESS(status)) { return status; }
    status = EptInstructionStatus(VmxRead(VMCS_SECONDARY_PROCESSOR_BASED_CONTROLS, &after), "VMREAD(secondary verify)");
    if (!NT_SUCCESS(status)) { return status; }
    if (after != adjusted) { return STATUS_UNSUCCESSFUL; }
    status = EptInstructionStatus(EptInvalidateCurrent(Cpu), "INVEPT(single)");
    if (!NT_SUCCESS(status)) { return status; }
    HV_INFO("EPT: core %lu active EPTP=0x%016I64X secondary=0x%08lX -> 0x%08lX; INVEPT passed\n",
        KeGetCurrentProcessorNumber(), Cpu->Eptp, (ULONG)before, adjusted);
    return STATUS_SUCCESS;
}

NTSTATUS EptInitializeAll(VOID)
{
    ULONG i;
    ULONG64 previousEnd = 0;
    NTSTATUS status = STATUS_SUCCESS;
    g_EptRam = MmGetPhysicalMemoryRangesEx2(NULL, 0);
    if (g_EptRam == NULL) {
        HV_ERROR("EPT: RAM range snapshot allocation failed\n");
        return STATUS_UNSUCCESSFUL;
    }
    g_EptRamCount = 0;
    while (g_EptRam[g_EptRamCount].NumberOfBytes.QuadPart != 0) {
        ULONG64 start = (ULONG64)g_EptRam[g_EptRamCount].BaseAddress.QuadPart;
        ULONG64 length = (ULONG64)g_EptRam[g_EptRamCount].NumberOfBytes.QuadPart;
        if (start < previousEnd || length > MAXULONGLONG - start ||
            ((start | length) & (PAGE_SIZE - 1u)) != 0) {
            status = STATUS_INVALID_PARAMETER;
            goto Done;
        }
        previousEnd = start + length;
        ++g_EptRamCount;
    }
    if (g_EptRamCount == 0) { status = STATUS_NOT_SUPPORTED; goto Done; }
    HV_INFO("EPT: %lu RAM ranges; highest RAM end=0x%I64X; holes unmapped\n", g_EptRamCount, previousEnd);
    for (i = 0; i < g_CpuCount; ++i) {
        GROUP_AFFINITY target = { 0 };
        GROUP_AFFINITY previous;
        PPER_CPU_DATA cpu = g_PerCpuData[i];
        target.Mask = (KAFFINITY)1 << i;
        KeSetSystemGroupAffinityThread(&target, &previous);
        status = EptInitializePerCore(cpu) != 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        if (NT_SUCCESS(status)) { status = EptWriteEptpToVmcs(cpu); }
        KeRevertToUserGroupAffinityThread(&previous);
        if (!NT_SUCCESS(status)) {
            HV_ERROR("EPT: core %lu initialization failed 0x%08lX\n", i, (ULONG)status);
            goto Done;
        }
        EptLogStatistics(cpu);
        HV_INFO("EPT: core %lu ready: EPTP=0x%016I64X; activation deferred to launch preparation\n",
            i, cpu->Eptp);
    }
Done:
    ExFreePool(g_EptRam);
    g_EptRam = NULL;
    g_EptRamCount = 0;
    return NT_SUCCESS(status) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

VOID EptFreePerCore(PPER_CPU_DATA Cpu)
{
    EPT_CONTEXT *ctx = Cpu->Ept;
    if (ctx == NULL) { return; }
    // Reverse allocation order. PerCpuFree has already checked global VMXOFF.
    // VMXOFF ended all use of this EPT; no restore/INVEPT is needed at teardown.
    // Includes prepared pages and pages retained following an INVEPT failure.
    while (ctx->Splits != NULL) {
        EPT_SPLIT *split = ctx->Splits;
        ctx->Splits = split->Next;
        ExFreePoolWithTag(split, EPT_POOL_TAG);
    }
    while (ctx->Pages != NULL) {
        EPT_PAGE *page = ctx->Pages;
        ctx->Pages = page->Next;
        EptReleasePage(page);
    }
    ExFreePoolWithTag(ctx, EPT_POOL_TAG);
    Cpu->Ept = NULL;
    Cpu->EptPml4Pa.QuadPart = 0;
    Cpu->Eptp = 0;
}

VOID EptFreeAll(VOID)
{
    ULONG i;
    for (i = 0; i < g_CpuCount; ++i) {
        if (g_PerCpuData[i] != NULL) { EptFreePerCore(g_PerCpuData[i]); }
    }
}
