// Shared x64 kernel/user protocol. Include ntddk.h or Windows.h + winioctl.h first.
#pragma once
#define HV_MONITOR_VERSION 10u
#define HV_MONITOR_TARGETS 26u
#define HV_MONITOR_RETIRED ((1u << 9) | (1u << 11))
#define HV_MONITOR_ALL (0x3FFFFFFu & ~HV_MONITOR_RETIRED)
// IDs are append-only so saved hook/filter selections retain their meaning.
#define HV_TARGET_DUPLICATE_OBJECT 24u
#define HV_TARGET_COPY_VIRTUAL_MEMORY 25u
static __inline int HvTargetSupported(ULONG Target)
{
    return Target < HV_MONITOR_TARGETS && Target != 9 && Target != 11;
}
static __inline int HvTargetIsKernel(ULONG Target)
{
    return (Target >= 13 && Target < 24) || Target == HV_TARGET_COPY_VIRTUAL_MEMORY;
}
static __inline int HvTargetHasPolicy(ULONG Target)
{
    return (Target < 13 && Target != 9 && Target != 11) || Target == HV_TARGET_DUPLICATE_OBJECT;
}
#define HV_MONITOR_PATH_CHARS 512u
#define HV_MONITOR_BATCH 64u
#define HV_PROTECTED_MAX 256u
#define HV_MONITOR_DEVICE L"\\\\.\\VelumControl"
#define IOCTL_HV_MONITOR_GET CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_HV_MONITOR_SET CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_HV_MONITOR_READ CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS)

#define IOCTL_HV_RUNTIME_GET CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_HV_RUNTIME_SET CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_HV_LOG_FILTER_GET CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_HV_LOG_FILTER_SET CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_HV_POLICY_GET CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_HV_POLICY_SET CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_WRITE_ACCESS)
// Independent per-target delivery filters. Zero criteria means all events.
// Conditions combine with AND. No effect on protection decisions or hook state.
typedef struct _HV_LOG_FILTER {
    ULONG Version, Target;
    ULONG CallerPid, CallerTid, TargetPid; // 0 = any
    ULONG Action; // 0=any, 1=continue, 2=blocked, 3=modified
    ULONG Mode; // 0=any, 1=user, 2=kernel
    ULONG PathSource; // 0=off, 1=file, 2=rename/link destination, 3=section name, 4=target image
    ULONG PathMatch; // 0=off, 1=exact, 2=directory subtree, 3=contains; case insensitive
    ULONG Field; // 0=off; otherwise Fields[Field-1]
    ULONG Operator; // 0=off, 1=equals, 2=has all bits, 3=inclusive unsigned range
    ULONG Reserved;
    ULONG64 Value, Upper;
    WCHAR Path[HV_MONITOR_PATH_CHARS]; // native path; no wildcard expansion
} HV_LOG_FILTER;

// Independent session policy. A disabled policy does not alter any operation.
// Policy matching is input-only; Action must be zero and Mode must be user.
typedef struct _HV_POLICY_CONFIG {
    HV_LOG_FILTER Match;
    ULONG Action; // 0=disabled, 1=deny, 2=remove requested access bits
    ULONG Reserved;
    ULONG64 RemoveAccess;
} HV_POLICY_CONFIG;
typedef struct _HV_RUNTIME_CONFIG {
    ULONG Version;
    ULONG Enabled; // GET: any retained hooks; SET: must equal (HookMask != 0).
    ULONG HookMask;
    ULONG AvailableMask; // GET: unique exported entry representatives; SET: ignored.
    ULONG PathCount;
    ULONG Reserved;
    WCHAR Paths[HV_PROTECTED_MAX][HV_MONITOR_PATH_CHARS]; // Zero count: telemetry only.
} HV_RUNTIME_CONFIG;

typedef struct _HV_MONITOR_CONFIG {
    ULONG Version;
    ULONG DebugMask;
    ULONG ConsoleMask;
    ULONG Reserved;
} HV_MONITOR_CONFIG;

typedef struct _HV_MONITOR_EVENT {
    ULONG64 Sequence;
    ULONG64 Time100ns; // System time at drain, not exact syscall entry time.
    ULONG Kind;       // 1: raw syscall fallback; 2: detailed syscall; 3: kernel register inputs.
    ULONG Target;
    ULONG Core;
    ULONG Action;     // Detailed syscall: 0=continue, 1=block, 2=modified.
    ULONG ProcessId;
    ULONG ThreadId;
    ULONG Status;     // Path capture status, NOT syscall completion status.
    ULONG Reserved;   // Bit 0: effective access in Arguments[1]. Kind 3: bit 1 caller in Arguments[4],
                      // bit 2 module basename in Raw and return-site offset in Arguments[5].
    ULONG64 Arguments[6]; // Raw: RCX,RDX,R8,R9,RSP,RIP; detail: RootDirectory in slot 0.
    ULONG TargetProcessId;
    ULONG TargetThreadId; // Requested client ID, not a newly created thread ID.
    ULONG FieldMask; // Valid decoded Fields; missing fields must not be printed as zero.
    ULONG ArgumentMask;
    ULONG PathKind; // 0=none, 1=file, 2=section object name (NOT backing file), 3=target image.
    ULONG PreviousMode;
    ULONG64 Fields[12]; // Per-target input schema; no completion results.
    ULONG64 InputArguments[11];
    WCHAR Destination[HV_MONITOR_PATH_CHARS]; // Rename/link destination.
    WCHAR Raw[HV_MONITOR_PATH_CHARS];
    WCHAR Resolved[HV_MONITOR_PATH_CHARS];
} HV_MONITOR_EVENT;

// Internal matching view: avoids copying a full event onto a guest kernel stack.
typedef struct _HV_LOG_VIEW {
    ULONG Kind, Target, Action, ProcessId, ThreadId, TargetProcessId, PreviousMode, PathKind, FieldMask;
    const ULONG64 *Fields;
    const WCHAR *Raw, *Resolved, *Destination;
} HV_LOG_VIEW;

typedef struct _HV_MONITOR_BATCH_DATA {
    ULONG Version;
    ULONG Count;
    ULONG64 Dropped; // Overflow in this delivery queue only, cumulative per session.
    HV_MONITOR_EVENT Events[HV_MONITOR_BATCH];
} HV_MONITOR_BATCH_DATA;

#define IOCTL_HV_KERNEL_STATS CTL_CODE(FILE_DEVICE_UNKNOWN, 0x809, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_HV_KERNEL_STATS_RESET CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80A, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define HV_KERNEL_STATS_CAPACITY 256u
typedef struct _HV_KERNEL_STAT {
    ULONG Target, HasTag, Tag, Reserved;
    ULONG64 ModuleBase, UnknownCaller, Calls, RequestedBytes;
    ULONG64 MissingSizeCalls; // Input-byte totals exclude these partial captures.
    WCHAR Module[64];
} HV_KERNEL_STAT;
typedef struct _HV_KERNEL_STATS {
    ULONG Version, Count;
    ULONG64 CaptureDropped, TableOverflow, ModuleSnapshotTime;
    HV_KERNEL_STAT Entries[HV_KERNEL_STATS_CAPACITY];
} HV_KERNEL_STATS;

