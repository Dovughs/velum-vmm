// hypervisor/include/vmcs.h
#pragma once
// Intel SDM Vol. 3D Appendix B.4.3/B.4.4: natural-width CET fields.
#define VMCS_GUEST_S_CET                0x00006828u
#define VMCS_GUEST_SSP                  0x0000682Au
#define VMCS_GUEST_INTERRUPT_SSP_TABLE  0x0000682Cu
#define VMCS_HOST_S_CET                 0x00006C18u
#define VMCS_HOST_SSP                   0x00006C1Au
#define VMCS_HOST_INTERRUPT_SSP_TABLE   0x00006C1Cu
#include <ntddk.h>

// References: SDM Vol. 3D 332831-086US Appendix B; Vol. 3C 326019-078US.
// Vol. 3C 25.11.2: encoding = (width << 13) | (type << 10) | (index << 1) | access.
// Width: 0=16, 1=64, 2=32, 3=natural. Type: 0=control, 1=read-only, 2=guest, 3=host.
// Bit 12 and bits 31:15 are reserved. Full access=0; high-half access=1.
// Full access reads/writes all 64 bits in x64. Optional fields still require capabilities.

#define VMCS_VPID 0x00000000u // SDM Vol. 3D B.1.1.

#define VMCS_GUEST_ES_SELECTOR 0x00000800u // SDM Vol. 3D B.1.2.
#define VMCS_GUEST_CS_SELECTOR 0x00000802u // SDM Vol. 3D B.1.2.
#define VMCS_GUEST_SS_SELECTOR 0x00000804u // SDM Vol. 3D B.1.2.
#define VMCS_GUEST_DS_SELECTOR 0x00000806u // SDM Vol. 3D B.1.2.
#define VMCS_GUEST_FS_SELECTOR 0x00000808u // SDM Vol. 3D B.1.2.
#define VMCS_GUEST_GS_SELECTOR 0x0000080Au // SDM Vol. 3D B.1.2.
#define VMCS_GUEST_LDTR_SELECTOR 0x0000080Cu // SDM Vol. 3D B.1.2.
#define VMCS_GUEST_TR_SELECTOR 0x0000080Eu // SDM Vol. 3D B.1.2.

#define VMCS_HOST_ES_SELECTOR 0x00000C00u // SDM Vol. 3D B.1.3.
#define VMCS_HOST_CS_SELECTOR 0x00000C02u // SDM Vol. 3D B.1.3.
#define VMCS_HOST_SS_SELECTOR 0x00000C04u // SDM Vol. 3D B.1.3.
#define VMCS_HOST_DS_SELECTOR 0x00000C06u // SDM Vol. 3D B.1.3.
#define VMCS_HOST_FS_SELECTOR 0x00000C08u // SDM Vol. 3D B.1.3.
#define VMCS_HOST_GS_SELECTOR 0x00000C0Au // SDM Vol. 3D B.1.3.
#define VMCS_HOST_TR_SELECTOR 0x00000C0Cu // SDM Vol. 3D B.1.3.

#define VMCS_IO_BITMAP_A 0x00002000u // SDM Vol. 3D B.2.1.
#define VMCS_IO_BITMAP_A_HIGH 0x00002001u // SDM Vol. 3D B.2.1.
#define VMCS_IO_BITMAP_B 0x00002002u // SDM Vol. 3D B.2.1.
#define VMCS_IO_BITMAP_B_HIGH 0x00002003u // SDM Vol. 3D B.2.1.
#define VMCS_MSR_BITMAP 0x00002004u // SDM Vol. 3D B.2.1.
#define VMCS_MSR_BITMAP_HIGH 0x00002005u // SDM Vol. 3D B.2.1.
#define VMCS_EXIT_MSR_STORE_ADDRESS 0x00002006u // SDM Vol. 3D B.2.1.
#define VMCS_EXIT_MSR_STORE_ADDRESS_HIGH 0x00002007u // SDM Vol. 3D B.2.1.
#define VMCS_EXIT_MSR_LOAD_ADDRESS 0x00002008u // SDM Vol. 3D B.2.1.
#define VMCS_EXIT_MSR_LOAD_ADDRESS_HIGH 0x00002009u // SDM Vol. 3D B.2.1.
#define VMCS_ENTRY_MSR_LOAD_ADDRESS 0x0000200Au // SDM Vol. 3D B.2.1.
#define VMCS_ENTRY_MSR_LOAD_ADDRESS_HIGH 0x0000200Bu // SDM Vol. 3D B.2.1.
#define VMCS_TSC_OFFSET 0x00002010u // SDM Vol. 3D B.2.1.
#define VMCS_TSC_OFFSET_HIGH 0x00002011u // SDM Vol. 3D B.2.1.
#define VMCS_EPT_POINTER 0x0000201Au // SDM Vol. 3D B.2.1.
#define VMCS_EPT_POINTER_HIGH 0x0000201Bu // SDM Vol. 3D B.2.1.

#define VMCS_GUEST_PHYSICAL_ADDRESS 0x00002400u // SDM Vol. 3D B.2.2.
#define VMCS_GUEST_PHYSICAL_ADDRESS_HIGH 0x00002401u // SDM Vol. 3D B.2.2.

#define VMCS_LINK_POINTER 0x00002800u // SDM Vol. 3D B.2.3.
#define VMCS_LINK_POINTER_HIGH 0x00002801u // SDM Vol. 3D B.2.3.
#define VMCS_GUEST_DEBUGCTL 0x00002802u // SDM Vol. 3D B.2.3.
#define VMCS_GUEST_DEBUGCTL_HIGH 0x00002803u // SDM Vol. 3D B.2.3.
#define VMCS_GUEST_PAT 0x00002804u // SDM Vol. 3D B.2.3.
#define VMCS_GUEST_PAT_HIGH 0x00002805u // SDM Vol. 3D B.2.3.
#define VMCS_GUEST_EFER 0x00002806u // SDM Vol. 3D B.2.3.
#define VMCS_GUEST_EFER_HIGH 0x00002807u // SDM Vol. 3D B.2.3.

#define VMCS_HOST_PAT 0x00002C00u // SDM Vol. 3D B.2.4.
#define VMCS_HOST_PAT_HIGH 0x00002C01u // SDM Vol. 3D B.2.4.
#define VMCS_HOST_EFER 0x00002C02u // SDM Vol. 3D B.2.4.
#define VMCS_HOST_EFER_HIGH 0x00002C03u // SDM Vol. 3D B.2.4.

#define VMCS_PIN_BASED_CONTROLS 0x00004000u // SDM Vol. 3D B.3.1.
#define VMCS_PRIMARY_PROCESSOR_BASED_CONTROLS 0x00004002u // SDM Vol. 3D B.3.1.
#define VMCS_EXCEPTION_BITMAP 0x00004004u // SDM Vol. 3D B.3.1.
#define VMCS_PAGE_FAULT_ERROR_CODE_MASK 0x00004006u // SDM Vol. 3D B.3.1.
#define VMCS_PAGE_FAULT_ERROR_CODE_MATCH 0x00004008u // SDM Vol. 3D B.3.1.
#define VMCS_CR3_TARGET_COUNT 0x0000400Au // SDM Vol. 3D B.3.1.
#define VMCS_EXIT_CONTROLS 0x0000400Cu // SDM Vol. 3D B.3.1.
#define VMCS_EXIT_MSR_STORE_COUNT 0x0000400Eu // SDM Vol. 3D B.3.1.
#define VMCS_EXIT_MSR_LOAD_COUNT 0x00004010u // SDM Vol. 3D B.3.1.
#define VMCS_ENTRY_CONTROLS 0x00004012u // SDM Vol. 3D B.3.1.
#define VMCS_ENTRY_MSR_LOAD_COUNT 0x00004014u // SDM Vol. 3D B.3.1.
#define VMCS_ENTRY_INTERRUPTION_INFORMATION 0x00004016u // SDM Vol. 3D B.3.1.
#define VMCS_ENTRY_EXCEPTION_ERROR_CODE 0x00004018u // SDM Vol. 3D B.3.1.
#define VMCS_ENTRY_INSTRUCTION_LENGTH 0x0000401Au // SDM Vol. 3D B.3.1.
#define VMCS_TPR_THRESHOLD 0x0000401Cu // SDM Vol. 3D B.3.1.
#define VMCS_SECONDARY_PROCESSOR_BASED_CONTROLS 0x0000401Eu // SDM Vol. 3D B.3.1.

#define VMCS_INSTRUCTION_ERROR 0x00004400u // SDM Vol. 3D B.3.2.
#define VMCS_EXIT_REASON 0x00004402u // SDM Vol. 3D B.3.2.
#define EXIT_REASON_VMCALL 18u // SDM Vol. 3C 25.1.2 (basic exit reason for VMCALL).
#define EXIT_REASON_CPUID  10u // SDM Vol. 3C 25.1.2 (basic exit reason for CPUID).
// exit reasons (SDM Vol. 3C Appendix C basic exit reasons).
#define EXIT_REASON_TRIPLE_FAULT 2u  // guest triple faulted (shutdown).
#define EXIT_REASON_INIT         3u  // INIT IPI arrived.
#define EXIT_REASON_GETSEC       11u // GETSEC (unconditional exit).
#define EXIT_REASON_HLT          12u // HLT exiting is enabled.
#define EXIT_REASON_INVD         13u // INVD (unconditional exit).
#define EXIT_REASON_VMCLEAR      19u // VMX instructions exit unconditionally.
#define EXIT_REASON_VMLAUNCH     20u
#define EXIT_REASON_VMPTRLD      21u
#define EXIT_REASON_VMPTRST      22u
#define EXIT_REASON_VMREAD       23u
#define EXIT_REASON_VMWRITE      24u
#define EXIT_REASON_VMXOFF       25u
#define EXIT_REASON_VMXON        26u
#define EXIT_REASON_INVEPT       50u // INVEPT (unconditional exit).
#define EXIT_REASON_INVVPID      53u // INVVPID (unconditional exit).
#define EXIT_REASON_WBINVD       54u // WBINVD (unconditional exit).
#define EXIT_REASON_XSETBV       55u // XSETBV (unconditional exit).

// VMCS fields (SDM Vol. 3D Appendix B).
#define VMCS_GUEST_INTERRUPTIBILITY_STATE  0x00004824u // 32-bit guest-state.
#define VMCS_GUEST_ACTIVITY_STATE          0x00004826u // 32-bit guest-state.
#define VMCS_ENTRY_INTERRUPTION_INFORMATION 0x00004016u // VM-entry control (natural).
#define VMCS_ENTRY_EXCEPTION_ERROR_CODE    0x00004018u // VM-entry control (natural).
// exit reasons. VMCS_EXIT_QUALIFICATION (0x6400, line below) is the EPT
// violation qualification source; PRIMARY_MONITOR_TRAP_FLAG is bit 27 of the
// primary controls (defined in vmx.h).
#define EXIT_REASON_MONITOR_TRAP_FLAG 37u // SDM Vol. 3C 25.5.2 (MTF basic exit reason).
#define EXIT_REASON_EPT_VIOLATION     48u // SDM Vol. 3C 27.2.1 (EPT violation).
#define EXIT_REASON_EPT_MISCONFIG     49u // SDM Vol. 3C 27.2.1 (EPT misconfiguration).
#define VMCS_EXIT_INTERRUPTION_INFORMATION 0x00004404u // SDM Vol. 3D B.3.2.
#define VMCS_EXIT_INTERRUPTION_ERROR_CODE 0x00004406u // SDM Vol. 3D B.3.2.
#define VMCS_IDT_VECTORING_INFORMATION 0x00004408u // SDM Vol. 3D B.3.2.
#define VMCS_IDT_VECTORING_ERROR_CODE 0x0000440Au // SDM Vol. 3D B.3.2.
#define VMCS_EXIT_INSTRUCTION_LENGTH 0x0000440Cu // SDM Vol. 3D B.3.2.
#define VMCS_EXIT_INSTRUCTION_INFORMATION 0x0000440Eu // SDM Vol. 3D B.3.2.

#define VMCS_GUEST_ES_LIMIT 0x00004800u // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_CS_LIMIT 0x00004802u // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_SS_LIMIT 0x00004804u // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_DS_LIMIT 0x00004806u // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_FS_LIMIT 0x00004808u // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_GS_LIMIT 0x0000480Au // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_LDTR_LIMIT 0x0000480Cu // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_TR_LIMIT 0x0000480Eu // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_GDTR_LIMIT 0x00004810u // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_IDTR_LIMIT 0x00004812u // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_ES_ACCESS_RIGHTS 0x00004814u // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_CS_ACCESS_RIGHTS 0x00004816u // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_SS_ACCESS_RIGHTS 0x00004818u // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_DS_ACCESS_RIGHTS 0x0000481Au // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_FS_ACCESS_RIGHTS 0x0000481Cu // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_GS_ACCESS_RIGHTS 0x0000481Eu // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_LDTR_ACCESS_RIGHTS 0x00004820u // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_TR_ACCESS_RIGHTS 0x00004822u // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_INTERRUPTIBILITY_STATE 0x00004824u // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_ACTIVITY_STATE 0x00004826u // SDM Vol. 3D B.3.3.
#define VMCS_GUEST_SYSENTER_CS 0x0000482Au // SDM Vol. 3D B.3.3.
#define VMCS_PREEMPTION_TIMER_VALUE 0x0000482Eu // SDM Vol. 3D B.3.3.

#define VMCS_HOST_SYSENTER_CS 0x00004C00u // SDM Vol. 3D B.3.4.

#define VMCS_CR0_GUEST_HOST_MASK 0x00006000u // SDM Vol. 3D B.4.1.
#define VMCS_CR4_GUEST_HOST_MASK 0x00006002u // SDM Vol. 3D B.4.1.
#define VMCS_CR0_READ_SHADOW 0x00006004u // SDM Vol. 3D B.4.1.
#define VMCS_CR4_READ_SHADOW 0x00006006u // SDM Vol. 3D B.4.1.
#define VMCS_CR3_TARGET_VALUE_0 0x00006008u // SDM Vol. 3D B.4.1.
#define VMCS_CR3_TARGET_VALUE_1 0x0000600Au // SDM Vol. 3D B.4.1.
#define VMCS_CR3_TARGET_VALUE_2 0x0000600Cu // SDM Vol. 3D B.4.1.
#define VMCS_CR3_TARGET_VALUE_3 0x0000600Eu // SDM Vol. 3D B.4.1.

#define VMCS_EXIT_QUALIFICATION 0x00006400u // SDM Vol. 3D B.4.2.
#define VMCS_GUEST_LINEAR_ADDRESS 0x0000640Au // SDM Vol. 3D B.4.2.

#define VMCS_GUEST_CR0 0x00006800u // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_CR3 0x00006802u // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_CR4 0x00006804u // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_ES_BASE 0x00006806u // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_CS_BASE 0x00006808u // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_SS_BASE 0x0000680Au // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_DS_BASE 0x0000680Cu // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_FS_BASE 0x0000680Eu // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_GS_BASE 0x00006810u // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_LDTR_BASE 0x00006812u // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_TR_BASE 0x00006814u // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_GDTR_BASE 0x00006816u // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_IDTR_BASE 0x00006818u // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_DR7 0x0000681Au // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_RSP 0x0000681Cu // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_RIP 0x0000681Eu // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_RFLAGS 0x00006820u // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_PENDING_DEBUG_EXCEPTIONS 0x00006822u // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_SYSENTER_ESP 0x00006824u // SDM Vol. 3D B.4.3.
#define VMCS_GUEST_SYSENTER_EIP 0x00006826u // SDM Vol. 3D B.4.3.

#define VMCS_HOST_CR0 0x00006C00u // SDM Vol. 3D B.4.4.
#define VMCS_HOST_CR3 0x00006C02u // SDM Vol. 3D B.4.4.
#define VMCS_HOST_CR4 0x00006C04u // SDM Vol. 3D B.4.4.
#define VMCS_HOST_FS_BASE 0x00006C06u // SDM Vol. 3D B.4.4.
#define VMCS_HOST_GS_BASE 0x00006C08u // SDM Vol. 3D B.4.4.
#define VMCS_HOST_TR_BASE 0x00006C0Au // SDM Vol. 3D B.4.4.
#define VMCS_HOST_GDTR_BASE 0x00006C0Cu // SDM Vol. 3D B.4.4.
#define VMCS_HOST_IDTR_BASE 0x00006C0Eu // SDM Vol. 3D B.4.4.
#define VMCS_HOST_SYSENTER_ESP 0x00006C10u // SDM Vol. 3D B.4.4.
#define VMCS_HOST_SYSENTER_EIP 0x00006C12u // SDM Vol. 3D B.4.4.
#define VMCS_HOST_RSP 0x00006C14u // SDM Vol. 3D B.4.4.
#define VMCS_HOST_RIP 0x00006C16u // SDM Vol. 3D B.4.4.

#define VMCS_ERROR_VMCLEAR_INVALID_PHYSICAL_ADDRESS 2u // SDM Vol. 3C 31.4, Table 31-1.
#define VMCS_ERROR_VMCLEAR_VMXON_POINTER 3u // SDM Vol. 3C 31.4, Table 31-1.
#define VMCS_ERROR_VMLAUNCH_NONCLEAR_VMCS 4u // SDM Vol. 3C 31.4, Table 31-1.
#define VMCS_ERROR_VMRESUME_NONLAUNCHED_VMCS 5u // SDM Vol. 3C 31.4, Table 31-1.
#define VMCS_ERROR_VMRESUME_AFTER_VMXOFF 6u // SDM Vol. 3C 31.4, Table 31-1.
#define VMCS_ERROR_ENTRY_INVALID_CONTROLS 7u // SDM Vol. 3C 31.4, Table 31-1.
#define VMCS_ERROR_ENTRY_INVALID_HOST_STATE 8u // SDM Vol. 3C 31.4, Table 31-1.
#define VMCS_ERROR_VMPTRLD_INVALID_PHYSICAL_ADDRESS 9u // SDM Vol. 3C 31.4, Table 31-1.
#define VMCS_ERROR_VMPTRLD_VMXON_POINTER 10u // SDM Vol. 3C 31.4, Table 31-1.
#define VMCS_ERROR_VMPTRLD_BAD_REVISION 11u // SDM Vol. 3C 31.4, Table 31-1.
#define VMCS_ERROR_UNSUPPORTED_COMPONENT 12u // SDM Vol. 3C 31.4, Table 31-1.
#define VMCS_ERROR_VMWRITE_READ_ONLY_COMPONENT 13u // SDM Vol. 3C 31.4, Table 31-1.
#define VMCS_ERROR_VMXON_IN_ROOT_OPERATION 15u // SDM Vol. 3C 31.4, Table 31-1.
#define VMCS_ERROR_ENTRY_INVALID_EXECUTIVE_POINTER 16u // SDM Vol. 3C 31.4, Table 31-1.
#define VMCS_ERROR_ENTRY_NONLAUNCHED_EXECUTIVE_VMCS 17u // SDM Vol. 3C 31.4, Table 31-1.
#define VMCS_ERROR_ENTRY_EXECUTIVE_POINTER_NOT_VMXON 18u // SDM Vol. 3C 31.4, Table 31-1.
#define VMCS_ERROR_ENTRY_INVALID_EXECUTIVE_CONTROLS 25u // SDM Vol. 3C 31.4, Table 31-1.
#define VMCS_ERROR_ENTRY_BLOCKED_BY_MOV_SS 26u // SDM Vol. 3C 31.4, Table 31-1.

typedef enum _VMCS_GUEST_ACTIVITY {
    VmcsGuestActive = 0,     // SDM Vol. 3C 25.4.2.
    VmcsGuestHlt = 1,        // SDM Vol. 3C 25.4.2.
    VmcsGuestShutdown = 2,   // SDM Vol. 3C 25.4.2.
    VmcsGuestWaitForSipi = 3  // SDM Vol. 3C 25.4.2.
} VMCS_GUEST_ACTIVITY;

// ---------------------------------------------------------------------------
// VMCS population (guest state, host state, control fields).
// Segment order matches the VMCS field order used by SDM Vol. 3D B.3.3
// (guest) and B.4.3 (64-bit guest), so one table of field IDs serves both.
typedef enum _VMCS_SEGMENT_INDEX {
    VmcsSegmentEs = 0,
    VmcsSegmentCs,
    VmcsSegmentSs,
    VmcsSegmentDs,
    VmcsSegmentFs,
    VmcsSegmentGs,
    VmcsSegmentLdtr,
    VmcsSegmentTr,
    VmcsSegmentCount
} VMCS_SEGMENT_INDEX;

// The four VMCS fields that describe one segment (SDM Vol. 3D B.3.3/B.4.3).
typedef struct _VMCS_SEGMENT_FIELDS {
    ULONG Selector;
    ULONG Limit;
    ULONG AccessRights;
    ULONG Base;
    PCSTR Name;
} VMCS_SEGMENT_FIELDS;

typedef struct _VMCS_SEGMENT_STATE {
    ULONG Selector;
    ULONG Limit;
    ULONG AccessRights;
    ULONG64 Base;
} VMCS_SEGMENT_STATE;

// Live state of the pinned core; captured at DISPATCH_LEVEL before any
// field is written. Tr (base) comes from the GDT decode, Fs/Gs base from MSRs.
typedef struct _VMCS_LIVE_STATE {
    ULONG Selector[VmcsSegmentCount];
    ULONG64 Cr0;
    ULONG64 Cr3;
    ULONG64 Cr4;
    ULONG64 Dr7;
    ULONG64 Rflags;
    ULONG64 Rsp;
    ULONG64 FsBase;
    ULONG64 GsBase;
    ULONG64 GdtrBase;
    ULONG64 IdtrBase;
    ULONG GdtrLimit;
    ULONG IdtrLimit;
    ULONG64 Pat;
    ULONG64 Efer;
    ULONG64 DebugCtl;
    ULONG64 SysenterCs;
    ULONG64 SysenterEsp;
    ULONG64 SysenterEip;
} VMCS_LIVE_STATE;

// Access-rights bits as they appear in the VMCS segment fields (SDM Vol. 3D
// B.3.3): bit 16 marks the segment unusable, bit 15 G, bit 4 S.
#define VMCS_ACCESS_RIGHTS_UNUSABLE   0x00010000u
#define VMCS_ACCESS_RIGHTS_GRANULARITY 0x00008000u
#define VMCS_ACCESS_RIGHTS_SYSTEM     0x00000010u
// Selector and descriptor decode (SDM Vol. 3A 3.4.5, Figures 3-8 and 3-9).
#define VMCS_SELECTOR_TI              0x00000004u
#define VMCS_SELECTOR_INDEX_MASK      0x0000FFF8u
#define VMCS_DESCRIPTOR_SIZE          8u
#define VMCS_DESCRIPTOR_ATTR_BYTE     5u
#define VMCS_DESCRIPTOR_ATTR_HIGH_BYTE 6u
#define VMCS_DESCRIPTOR_LIMIT_HIGH_MASK 0x0Fu
#define VMCS_DESCRIPTOR_GRANULARITY_MASK 0x80u
#define VMCS_DESCRIPTOR_HIGH_BASE_OFFSET 8u
#define VMCS_DESCRIPTOR_HIGH_BASE_SHIFT 32u

NTSTATUS VmcsInitializeAll(VOID);
// writes guest state, host state and controls of every core's VMCS,
// on the pinned core at DISPATCH_LEVEL, immediately before the one-shot test.
NTSTATUS VmcsPopulateCurrent(PPER_CPU_DATA Cpu, ULONG Index);
// Returns the wrapper status; writes Error only on success. Diagnostics on the owning CPU, at IRQL <= DISPATCH_LEVEL.
ULONG VmcsQueryInstructionError(PULONG Error);
