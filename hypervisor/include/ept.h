// hypervisor/include/ept.h
#pragma once
#include <ntddk.h>

// SDM 326019-078US: 29.3.2 (older editions: 28.2); no separate present bit.
#define EPT_ENTRY_READ              (1ull << 0) // SDM 29.3.2.
#define EPT_ENTRY_WRITE             (1ull << 1) // SDM 29.3.2.
#define EPT_ENTRY_EXECUTE           (1ull << 2) // SDM 29.3.2.
#define EPT_ENTRY_RWX               7ull        // SDM 29.3.2.
#define EPT_ENTRY_TYPE_SHIFT        3u          // SDM 29.3.2, leaf only.
#define EPT_ENTRY_TYPE_MASK         (7ull << 3) // SDM 29.3.2, leaf only.
#define EPT_ENTRY_IGNORE_PAT        (1ull << 6) // SDM 29.3.7, leaf only.
#define EPT_ENTRY_LARGE_PAGE        (1ull << 7) // SDM 29.3.2: PDPTE=1GB, PDE=2MB.
#define EPT_ENTRY_ACCESSED          (1ull << 8) // SDM 29.3.5; disabled here.
#define EPT_ENTRY_DIRTY             (1ull << 9) // SDM 29.3.5, leaf only.
#define EPT_ADDRESS_MASK           0x000FFFFFFFFFF000ull // SDM 29.3.2; also bound by MAXPHYADDR.
#define EPT_TABLE_ENTRIES          512u        // SDM 29.3.2: 4KB / 8 bytes.
#define EPT_INDEX_MASK             511ull      // SDM 29.3.2: nine-bit index.
#define EPT_PML4_SHIFT             39u         // SDM 29.3.2.
#define EPT_PML3_SHIFT             30u         // SDM 29.3.2.
#define EPT_PML2_SHIFT             21u         // SDM 29.3.2.
#define EPT_PML1_SHIFT             12u         // SDM 29.3.2.
#define EPT_GPA_LIMIT              (1ull << 48) // SDM 29.3.2: four-level walk.
#define EPT_LARGE_SIZE             (1ull << 21) // SDM 29.3.2, Table 29-4.
#define EPT_LARGE_ADDRESS_MASK     (EPT_ADDRESS_MASK & ~(EPT_LARGE_SIZE - 1ull))
#define EPT_CAP_2MB                (1ull << 16) // SDM Vol. 3D A.10, IA32_VMX_EPT_VPID_CAP.
#define EPT_LEAF_ATTRIBUTES        (EPT_ENTRY_RWX | EPT_ENTRY_TYPE_MASK | EPT_ENTRY_IGNORE_PAT)
#define EPT_TYPE_UC                0u          // SDM 29.3.7.
#define EPT_TYPE_WC                1u          // SDM 29.3.7.
#define EPT_TYPE_WT                4u          // SDM 29.3.7.
#define EPT_TYPE_WP                5u          // SDM 29.3.7.
#define EPT_TYPE_WB                6u          // SDM 29.3.7.
#define EPTP_TYPE_MASK             7ull        // SDM 25.6.11 / Vol. 3D B.2.1.
#define EPTP_WALK_SHIFT            3u          // SDM 25.6.11: levels minus one.
#define EPTP_AD_ENABLE             (1ull << 6) // SDM 25.6.11.
#define EPTP_BUILD(pa, type, levels, ad) \
    (((ULONG64)(pa) & EPT_ADDRESS_MASK) | ((ULONG64)(type) & EPTP_TYPE_MASK) | \
    (((ULONG64)(levels) - 1ull) << EPTP_WALK_SHIFT) | ((ad) ? EPTP_AD_ENABLE : 0ull))
// SDM 29.3.2: accessors leave unrelated fields intact.
#define EPT_ENTRY_ADDRESS(e) ((ULONG64)(e) & EPT_ADDRESS_MASK)
#define EPT_ENTRY_PRESENT(e) (((ULONG64)(e) & EPT_ENTRY_RWX) != 0)
#define EPT_ENTRY_HAS(e, bits) (((ULONG64)(e) & (bits)) == (bits))
#define EPT_ENTRY_SET(e, bits) ((ULONG64)(e) | (bits))
#define EPT_ENTRY_CLEAR(e, bits) ((ULONG64)(e) & ~(ULONG64)(bits))
#define EPT_ENTRY_TYPE(e) (((ULONG64)(e) & EPT_ENTRY_TYPE_MASK) >> EPT_ENTRY_TYPE_SHIFT)
#define EPT_INVEPT_TYPE_SINGLE     1ull // SDM 31.3 INVEPT (older editions 30.3).
#define EPT_INVEPT_TYPE_ALL        2ull // SDM 31.3 INVEPT.

typedef struct _EPT_INVEPT_DESCRIPTOR { // SDM 31.3 Figure 31-1, 128 bits.
    ULONG64 Eptp;
    ULONG64 Reserved;
} EPT_INVEPT_DESCRIPTOR;
C_ASSERT(sizeof(EPT_INVEPT_DESCRIPTOR) == 16);

// MSRs and layouts: SDM Vol. 3A, Memory Cache Control / MTRRs (11.11 in older editions).
#define EPT_MSR_MTRRCAP            0xFEu  // SDM MTRRcap / Vol. 4.
#define EPT_MSR_MTRR_BASE0         0x200u // SDM variable-range MTRRs / Vol. 4.
#define EPT_MSR_MTRR_FIXED64       0x250u // SDM fixed-range MTRRs / Vol. 4.
#define EPT_MSR_MTRR_FIXED16       0x258u // SDM fixed-range MTRRs / Vol. 4.
#define EPT_MSR_MTRR_FIXED4        0x268u // SDM fixed-range MTRRs / Vol. 4.
#define EPT_MSR_MTRR_DEFAULT       0x2FFu // SDM MTRRdefType / Vol. 4.
#define EPT_MTRR_ENABLE            (1ull << 11) // SDM MTRRdefType.E.
#define EPT_MTRR_FIXED_ENABLE      (1ull << 10) // SDM MTRRdefType.FE.
#define EPT_MTRR_FIXED_SUPPORTED   (1ull << 8)  // SDM MTRRcap.FIX.
#define EPT_MTRR_VALID             (1ull << 11) // SDM MTRRphysMask.V.
#define EPT_MTRR_MAX_VARIABLE     255u // SDM MTRRcap.VCNT bits 7:0.
#define EPT_MTRR_FIXED_COUNT      11u  // SDM fixed-range MTRRs: 1+2+8 MSRs.

typedef struct _EPT_MTRR_RANGE {
    ULONG64 Base;
    ULONG64 Mask;
    UCHAR Type;
    BOOLEAN Valid;
} EPT_MTRR_RANGE;

typedef struct _EPT_MTRR_STATE {
    ULONG64 PhysicalMask;
    ULONG64 PhysicalLimit;
    ULONG64 Default;
    ULONG64 Fixed[EPT_MTRR_FIXED_COUNT];
    ULONG VariableCount;
    BOOLEAN FixedSupported;
    EPT_MTRR_RANGE Variable[EPT_MTRR_MAX_VARIABLE];
} EPT_MTRR_STATE;

typedef struct _EPT_PAGE {
    struct _EPT_PAGE *Next;
    PVOID Va;
    PHYSICAL_ADDRESS Pa;
    PMDL Mdl;
} EPT_PAGE;

typedef struct _EPT_STATISTICS {
    ULONG64 LargePageCount;
    ULONG64 SplitPageCount;
    ULONG64 SplitLeafCount;
} EPT_STATISTICS;

typedef struct _EPT_CONTEXT {
    EPT_PAGE *Pages;
    EPT_PAGE *Root;
    EPT_MTRR_STATE Mtrr;
    ULONG64 TableCount;
    ULONG64 LeafCount;  // Mapped 4KB units, retained for compatibility (not hardware leaf entries).
    ULONG64 NonWbCount; // Non-WB mapped 4KB units.
    struct _EPT_SPLIT *Splits;
    struct _EPT_SPLIT *InvalidationPending;
    EPT_STATISTICS Statistics;
    EPT_PAGE *RuntimePages[64];
    ULONG RuntimeAvailable;
} EPT_CONTEXT;

typedef enum _EPT_SPLIT_STATE {
    EptSplitPermanent,  // RAM edge or mixed MTRRs: no original large mapping.
    EptSplitPrepared,
    EptSplitActive,
    EptSplitRetiring,   // Original PDE published; INVEPT has not succeeded yet.
    EptSplitRetired
} EPT_SPLIT_STATE;

// Sparse list: bookkeeping costs memory only for fallback/prepared/split regions.
// All pages, including prepared/retired pages, remain owned by Context->Pages.
typedef struct _EPT_SPLIT {
    struct _EPT_SPLIT *Next;
    ULONG64 RegionBase;
    ULONG64 OriginalPde;
    EPT_PAGE *Pml1;
    ULONG64 Pml1Pa;
    EPT_SPLIT_STATE State;
    ULONG LastInveptStatus;
} EPT_SPLIT;

struct _PER_CPU_DATA;
// VMX root on the EPT owner CPU, after publishing a mapping change. Returns
// instruction status (0/1/2); never replaces INVEPT with INVVPID. Other cores
// perform their own invalidation through the existing commit/IPI protocol.
ULONG EptInvalidateCurrent(struct _PER_CPU_DATA *Cpu);
ULONG64 EptInitializePerCore(struct _PER_CPU_DATA *Cpu);
NTSTATUS EptWriteEptpToVmcs(struct _PER_CPU_DATA *Cpu);
NTSTATUS EptEnableInVmcs(struct _PER_CPU_DATA *Cpu);
VOID EptFreePerCore(struct _PER_CPU_DATA *Cpu);
NTSTATUS EptInitializeAll(VOID);
VOID EptFreeAll(VOID);
// Lifecycle calls must be serialized against teardown. All code/data are nonpaged.
NTSTATUS EptPrepareSplit(struct _PER_CPU_DATA *Cpu, ULONG64 Gpa); // PASSIVE_LEVEL.
NTSTATUS EptSplitLargePage(struct _PER_CPU_DATA *Cpu, ULONG64 Gpa); // Owner CPU, VMX root, <= IPI_LEVEL.
NTSTATUS EptRestoreLargePage(struct _PER_CPU_DATA *Cpu, ULONG64 Gpa); // Same contract.
// Caller holds EptLock; pointer is valid only while that lock remains held.
ULONG64 *EptGetPml1Entry(struct _PER_CPU_DATA *Cpu, ULONG64 Gpa);
NTSTATUS EptReclaimRetired(struct _PER_CPU_DATA *Cpu); // PASSIVE_LEVEL, after successful invalidation.
VOID EptLogStatistics(struct _PER_CPU_DATA *Cpu); // PASSIVE_LEVEL.
VOID EptLogSplitResult(struct _PER_CPU_DATA *Cpu, ULONG64 Gpa, NTSTATUS Result); // PASSIVE_LEVEL.
NTSTATUS EptPrepareRuntime(struct _PER_CPU_DATA *Cpu);
NTSTATUS EptMapRuntimeHole(struct _PER_CPU_DATA *Cpu, ULONG64 Gpa);
