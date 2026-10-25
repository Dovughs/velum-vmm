// hypervisor/include/vmx.h
#pragma once
#include <ntddk.h>

#define MSR_IA32_FEATURE_CONTROL       0x03Au
#define MSR_IA32_VMX_BASIC             0x480u
#define MSR_IA32_VMX_PINBASED_CTLS     0x481u
#define MSR_IA32_VMX_PROCBASED_CTLS    0x482u
#define MSR_IA32_VMX_EXIT_CTLS         0x483u
#define MSR_IA32_VMX_ENTRY_CTLS        0x484u
#define MSR_IA32_VMX_MISC              0x485u
#define MSR_IA32_VMX_CR0_FIXED0        0x486u
#define MSR_IA32_VMX_CR0_FIXED1        0x487u
#define MSR_IA32_VMX_CR4_FIXED0        0x488u
#define MSR_IA32_VMX_CR4_FIXED1        0x489u
#define MSR_IA32_VMX_VMCS_ENUM         0x48Au
#define MSR_IA32_VMX_PROCBASED_CTLS2   0x48Bu
#define MSR_IA32_VMX_EPT_VPID_CAP      0x48Cu
// "TRUE" capability MSRs (SDM Vol. 3D A.1). When IA32_VMX_BASIC[55] is set, these
// report the real allowed-0/allowed-1 masks for the pin-based, primary, exit and
// entry controls; the legacy 0x481-0x484 MSRs otherwise over-report extra bits as
// "must be 1", which bloats the control fields. There is no TRUE variant of the
// secondary controls MSR (0x48B): its masks always come from the legacy MSR.
#define MSR_IA32_VMX_TRUE_PINBASED_CTLS  0x48Du
#define MSR_IA32_VMX_TRUE_PROCBASED_CTLS 0x48Eu
#define MSR_IA32_VMX_TRUE_EXIT_CTLS      0x48Fu
#define MSR_IA32_VMX_TRUE_ENTRY_CTLS     0x490u

// Non-VMX MSRs captured into the guest/host state areas: SDM Vol. 3D A,
// "MSRs in the guest and host state areas".
#define MSR_IA32_DEBUGCTL              0x1D9u
#define MSR_IA32_SYSENTER_CS           0x174u
#define MSR_IA32_SYSENTER_ESP          0x175u
#define MSR_IA32_SYSENTER_EIP          0x176u
#define MSR_IA32_PAT                   0x277u
#define MSR_IA32_EFER                  0xC0000080u
#define MSR_IA32_FS_BASE               0xC0000100u
#define MSR_IA32_GS_BASE               0xC0000101u

#define VMX_BIT(n)                    (1ull << (n))
#define FEATURE_LOCK                 VMX_BIT(0)
#define FEATURE_VMX_OUTSIDE_SMX       VMX_BIT(2)
#define CR4_VMXE                     VMX_BIT(13)
#define PRIMARY_SECONDARY_CONTROLS   (1u << 31)
#define SECONDARY_ENABLE_EPT         (1u << 1)

// Control-field bits. Each is bit n of the corresponding VMCS control field and
// of the matching IA32_VMX_*_CTLS MSR: SDM Vol. 3C 24.6 (pin/primary/secondary),
// 24.7 (exit), 24.8 (entry); MSR layout in SDM Vol. 3D A.1.
#define PIN_EXTERNAL_INTERRUPT_EXITING   (1u << 0)
#define PIN_NMI_EXITING                  (1u << 3)
#define PIN_VIRTUAL_NMIS                 (1u << 5)
#define PIN_PREEMPTION_TIMER_ACTIVATE    (1u << 6)
#define PIN_PROCESS_POSTED_INTERRUPTS    (1u << 7)
#define PRIMARY_INTERRUPT_WINDOW_EXITING (1u << 2)
#define PRIMARY_USE_TSC_OFFSETTING       (1u << 3)
#define PRIMARY_HLT_EXITING              (1u << 7)
#define PRIMARY_INVLPG_EXITING           (1u << 9)
#define PRIMARY_RDPMC_EXITING            (1u << 11)
#define PRIMARY_RDTSC_EXITING            (1u << 12)
#define PRIMARY_CR3_LOAD_EXITING         (1u << 15)
#define PRIMARY_CR3_STORE_EXITING        (1u << 16)
#define PRIMARY_CR8_LOAD_EXITING         (1u << 19)
#define PRIMARY_CR8_STORE_EXITING        (1u << 20)
#define PRIMARY_TPR_SHADOW               (1u << 21)
#define PRIMARY_NMI_WINDOW_EXITING       (1u << 22)
#define PRIMARY_MOV_DR_EXITING           (1u << 23)
#define PRIMARY_UNCONDITIONAL_IO_EXITING (1u << 24)
#define PRIMARY_USE_IO_BITMAPS           (1u << 25)
#define PRIMARY_MONITOR_TRAP_FLAG        (1u << 27)
#define PRIMARY_USE_MSR_BITMAPS          (1u << 28)
#define PRIMARY_MONITOR_EXITING          (1u << 29)
#define PRIMARY_PAUSE_EXITING            (1u << 30)
#define SECONDARY_RDTSCP                 (1u << 3)
#define SECONDARY_ENABLE_VPID            (1u << 5)
#define SECONDARY_UNRESTRICTED_GUEST     (1u << 7)
#define EXIT_SAVE_DEBUG_CONTROLS         (1u << 2)
#define EXIT_HOST_ADDRESS_SPACE_64       (1u << 9)
#define EXIT_LOAD_IA32_PERF_GLOBAL_CTRL  (1u << 12)
#define EXIT_ACKNOWLEDGE_INTERRUPT       (1u << 15)
#define EXIT_SAVE_IA32_PAT               (1u << 18)
#define EXIT_LOAD_IA32_PAT               (1u << 19)
#define EXIT_SAVE_IA32_EFER              (1u << 20)
#define EXIT_LOAD_IA32_EFER              (1u << 21)
#define EXIT_SAVE_PREEMPTION_TIMER       (1u << 22)
#define ENTRY_LOAD_DEBUG_CONTROLS        (1u << 2)
#define ENTRY_IA32E_MODE_GUEST           (1u << 9)
#define ENTRY_LOAD_IA32_PERF_GLOBAL_CTRL (1u << 13)
#define ENTRY_LOAD_IA32_PAT              (1u << 14)
#define ENTRY_LOAD_IA32_EFER             (1u << 15)
// Intel SDM Vol. 3C Tables 25-13/25-15: load all three CET state fields.
#define EXIT_LOAD_CET_STATE             (1u << 28)
#define ENTRY_LOAD_CET_STATE            (1u << 20)
#define CR4_CET                         VMX_BIT(23)

// Descriptor-table output of SGDT/SIDT: the 16-bit limit immediately followed by
// the 64-bit base, ten contiguous bytes (SDM Vol. 3A 2.4, "GDTR and IDTR").
// The layout MUST be packed: with natural alignment the compiler places Base at
// offset 8, while SGDT/SIDT write it at offset 2, so Base would read the two high
// bytes of the real base address plus untouched padding and every descriptor
// lookup would dereference a bogus GDT base.
#pragma pack(push, 1)
typedef struct _HV_DESCRIPTOR_TABLE {
    USHORT Limit;
    ULONG64 Base;
} HV_DESCRIPTOR_TABLE;
#pragma pack(pop)

// The instruction writes exactly these bytes, so the size and the base offset are
// fixed: a regression here is an access violation at descriptor decode time.
_Static_assert(sizeof(HV_DESCRIPTOR_TABLE) == 10, "SGDT/SIDT store ten bytes");
_Static_assert(FIELD_OFFSET(HV_DESCRIPTOR_TABLE, Base) == 2,
    "SGDT/SIDT store the base starting at offset 2");
// Intel SDM A.10: bit 6 is four-level EPT walks, not 4 KB leaf support.
#define EPT_WALK_LENGTH_4             VMX_BIT(6)
#define EPT_MEMORY_UC                 VMX_BIT(8)
#define EPT_MEMORY_WB                 VMX_BIT(14)
#define EPT_INVEPT                    VMX_BIT(20)
#define EPT_INVEPT_SINGLE             VMX_BIT(25)
#define EPT_INVEPT_ALL                VMX_BIT(26)
#define VMX_MEMORY_WB                6u

typedef struct _VMX_CONTROL_MASKS {
    ULONG Allowed0;
    ULONG Allowed1;
} VMX_CONTROL_MASKS;

typedef struct _VMX_CAPABILITIES {
    ULONG64 Basic;
    ULONG64 Misc;
    ULONG64 FeatureControl;
    ULONG64 EptVpidCap;
    ULONG64 VmcsEnum;
    ULONG64 Cr0Fixed0;
    ULONG64 Cr0Fixed1;
    ULONG64 Cr4Fixed0;
    ULONG64 Cr4Fixed1;
    VMX_CONTROL_MASKS Pin;
    VMX_CONTROL_MASKS Primary;
    VMX_CONTROL_MASKS Secondary;
    VMX_CONTROL_MASKS Exit;
    VMX_CONTROL_MASKS Entry;
    ULONG RevisionId;
    ULONG RegionSize;
    UCHAR MemoryType;
    UCHAR PreemptionTimerRate;
    BOOLEAN DualMonitor;
    BOOLEAN TrueControls;
    BOOLEAN HltActivityState;
    BOOLEAN EptSupported;
    BOOLEAN Ept4KbPages;
    BOOLEAN EptWalk4;
    BOOLEAN EptWb;
    BOOLEAN EptUc;
    BOOLEAN Invept;
    BOOLEAN InveptSingle;
    BOOLEAN InveptAll;
} VMX_CAPABILITIES;

typedef enum _VMX_START_RESULT {
    VmxNotAttempted = 0,
    VmxStarted,
    VmxFeatureDisabled,
    VmxAlreadyOwned,
    VmxInvalidFixedBits,
    VmxInstructionFailed
} VMX_START_RESULT;

NTSTATUS VmxDetectAll(VOID);
NTSTATUS VmxConfigureFeatureControl(VOID);
ULONG_PTR VmxonIpiCallback(ULONG_PTR Context);
ULONG_PTR VmxoffIpiCallback(ULONG_PTR Context);
VOID VmxStopAll(VOID);
ULONG VmxOn(ULONG64 RegionPhysicalAddress);
ULONG VmxOff(VOID);
ULONG VmxClear(ULONG64 RegionPhysicalAddress);
ULONG VmxPtrLoad(ULONG64 RegionPhysicalAddress);
ULONG VmxRead(ULONG64 Field, PULONG64 Value);
ULONG VmxWrite(ULONG64 Field, ULONG64 Value);
// SDM 31.2/31.3: 0=success, 1=VMfailValid (ZF), 2=VMfailInvalid (CF).
ULONG EptInveptSingleContext(ULONG64 Eptp);
ULONG EptInveptAllContexts(VOID);
// One-shot launch: 0=expected VMCALL, 1=VMfailValid, 2=VMfailInvalid,
// 3=unexpected VM-exit/VM-entry failure. CallerRspSave receives a saved frame.
ULONG VmcsLaunchOnCurrentCoreTest(ULONG64 *CallerRspSave, ULONG TransferCet, ULONG Persistent);

// Guest-state capture helpers and the entry placeholders (src/vmx.asm).
// All are resident leaf functions that return live state of the calling core.
// The selector/register getters return a value with bits 63:16 cleared.
ULONG HvGetEs(VOID);
ULONG HvGetCs(VOID);
ULONG HvGetSs(VOID);
ULONG HvGetDs(VOID);
ULONG HvGetFs(VOID);
ULONG HvGetGs(VOID);
ULONG HvGetTr(VOID);
ULONG HvGetLdtr(VOID);
ULONG64 HvGetRflags(VOID);
ULONG64 HvGetRsp(VOID);
// Fills ten bytes: 16-bit limit then 64-bit base (HV_DESCRIPTOR_TABLE).
VOID HvGetGdtr(HV_DESCRIPTOR_TABLE *Table);
VOID HvGetIdtr(HV_DESCRIPTOR_TABLE *Table);
// HOST_RIP / GUEST_RIP targets. Used by VM entry and exit after VMCS population.
VOID HvGuestEntryStub(VOID);
VOID HvHostEntryStub(VOID);
