// hypervisor/src/filter.c
#include <ntifs.h>
#include "filter.h"
#include "common.h"
#include <intrin.h>
#include "monitor.h"
#include "kernel_telemetry_capture.h"
#include "telemetry_fields.h"
#include "telemetry_symbols.h"
#define HV_UPCASE(c) RtlUpcaseUnicodeChar(c)
#include "telemetry_match.h"
#include "telemetry_policy.h"
static const PCSTR g_DetailLabels[SYSCALL_TARGET_COUNT][12] = HV_DETAIL_LABELS;

C_ASSERT(sizeof(OBJECT_ATTRIBUTES) == 0x30);
C_ASSERT(FIELD_OFFSET(OBJECT_ATTRIBUTES, ObjectName) == 0x10);
C_ASSERT(sizeof(UNICODE_STRING) == 0x10);
C_ASSERT(FIELD_OFFSET(UNICODE_STRING, Buffer) == 8);
C_ASSERT(sizeof(FILTER_ARGUMENTS) == 56);
C_ASSERT(FIELD_OFFSET(FILTER_ARGUMENTS, Rcx) == 8);
C_ASSERT(FIELD_OFFSET(FILTER_ARGUMENTS, R8) == 24);
C_ASSERT(FIELD_OFFSET(FILTER_ARGUMENTS, R9) == 32);
C_ASSERT(FILTER_DENIED_STATUS == 0xC0000022u);

#define FILTER_PATH_CHARS 512u
#define FILTER_LOG_COUNT 256u
#define FILTER_CLOSED 0x40000000L
static volatile LONG g_FilterReferences = FILTER_CLOSED;
#define FILTER_POLICY_TAG 'loPF'
typedef struct _FILTER_POLICY {
    ULONG Count;
    USHORT VolumeChars[26];
    WCHAR Volumes[26][FILTER_PATH_CHARS];
    USHORT TargetChars[HV_PROTECTED_MAX];
    WCHAR Targets[HV_PROTECTED_MAX][FILTER_PATH_CHARS];
} FILTER_POLICY;
// Immutable while hooks are installed. Replaced only after guest rundown.
static FILTER_POLICY *g_Policy;

typedef struct _FILTER_PATH_EVENT {
    ULONG TargetIdx;
    ULONG Action;
    NTSTATUS Status;
    ULONG ProcessId;
    ULONG ThreadId;
    ULONG Core;
    ULONG64 RootDirectory;
    ULONG64 EffectiveAccess;
    ULONG TargetProcessId, TargetThreadId, FieldMask, ArgumentMask, PathKind, PreviousMode;
    ULONG64 Fields[12], InputArguments[11];
    WCHAR Destination[FILTER_PATH_CHARS];
    USHORT RawChars;
    USHORT ResolvedChars;
    WCHAR Raw[FILTER_PATH_CHARS];
    WCHAR Resolved[FILTER_PATH_CHARS];
} FILTER_PATH_EVENT;

// Guest producers may migrate/preempt: use a bounded MPSC queue, not a per-core
// SPSC ring. Try-lock only; never wait in a syscall. All storage is resident.
static FILTER_PATH_EVENT g_PathLog[FILTER_LOG_COUNT];
static volatile LONG g_PathLogBusy;
static ULONG g_PathHead, g_PathTail;
static volatile LONG g_PathDropped;

#define FILTER_DECLARE(n) extern VOID HvFilterGuestEntry_##n(VOID); \
    extern VOID HvFilterContinue_##n(VOID); extern VOID HvFilterBlock_##n(VOID)
FILTER_DECLARE(0);
FILTER_DECLARE(1);
FILTER_DECLARE(2);
FILTER_DECLARE(3);
FILTER_DECLARE(4);
FILTER_DECLARE(5);
FILTER_DECLARE(6);
FILTER_DECLARE(7);
FILTER_DECLARE(8);
FILTER_DECLARE(10);
FILTER_DECLARE(12);
FILTER_DECLARE(13);
FILTER_DECLARE(14);
FILTER_DECLARE(15);
FILTER_DECLARE(16);
FILTER_DECLARE(17);
FILTER_DECLARE(18);
FILTER_DECLARE(19);
FILTER_DECLARE(20);
FILTER_DECLARE(21);
FILTER_DECLARE(22);
FILTER_DECLARE(23);
FILTER_DECLARE(24);
FILTER_DECLARE(25);
FILTER_GUEST_GATE const g_FilterGuestEntries[SYSCALL_TARGET_COUNT] = {
    HvFilterGuestEntry_0, HvFilterGuestEntry_1, HvFilterGuestEntry_2, HvFilterGuestEntry_3, HvFilterGuestEntry_4, HvFilterGuestEntry_5, HvFilterGuestEntry_6, HvFilterGuestEntry_7, HvFilterGuestEntry_8, NULL, HvFilterGuestEntry_10, NULL, HvFilterGuestEntry_12,
    HvFilterGuestEntry_13, HvFilterGuestEntry_14, HvFilterGuestEntry_15, HvFilterGuestEntry_16, HvFilterGuestEntry_17, HvFilterGuestEntry_18, HvFilterGuestEntry_19, HvFilterGuestEntry_20, HvFilterGuestEntry_21, HvFilterGuestEntry_22, HvFilterGuestEntry_23, HvFilterGuestEntry_24, HvFilterGuestEntry_25
};
FILTER_GUEST_GATE const g_FilterContinueGates[SYSCALL_TARGET_COUNT] = {
    HvFilterContinue_0, HvFilterContinue_1, HvFilterContinue_2, HvFilterContinue_3, HvFilterContinue_4, HvFilterContinue_5, HvFilterContinue_6, HvFilterContinue_7, HvFilterContinue_8, NULL, HvFilterContinue_10, NULL, HvFilterContinue_12,
    HvFilterContinue_13, HvFilterContinue_14, HvFilterContinue_15, HvFilterContinue_16, HvFilterContinue_17, HvFilterContinue_18, HvFilterContinue_19, HvFilterContinue_20, HvFilterContinue_21, HvFilterContinue_22, HvFilterContinue_23, HvFilterContinue_24, HvFilterContinue_25
};
FILTER_GUEST_GATE const g_FilterBlockGates[SYSCALL_TARGET_COUNT] = {
    HvFilterBlock_0, HvFilterBlock_1, HvFilterBlock_2, HvFilterBlock_3, HvFilterBlock_4, HvFilterBlock_5, HvFilterBlock_6, HvFilterBlock_7, HvFilterBlock_8, NULL, HvFilterBlock_10, NULL, HvFilterBlock_12,
    HvFilterBlock_13, HvFilterBlock_14, HvFilterBlock_15, HvFilterBlock_16, HvFilterBlock_17, HvFilterBlock_18, HvFilterBlock_19, HvFilterBlock_20, HvFilterBlock_21, HvFilterBlock_22, HvFilterBlock_23, HvFilterBlock_24, HvFilterBlock_25
};

static NTSTATUS FilterReadTrusted(PVOID Target, ULONG64 Source, SIZE_T Bytes)
{
    MM_COPY_ADDRESS address;
    SIZE_T copied = 0;
    NTSTATUS status;
    if (Source == 0 || Bytes == 0 || Source > MAXULONGLONG - (Bytes - 1)) {
        return STATUS_INVALID_ADDRESS;
    }
    address.VirtualAddress = (PVOID)(ULONG_PTR)Source;
    __try {
        status = MmCopyMemory(Target, address, Bytes, MM_COPY_MEMORY_VIRTUAL, &copied);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
    if (!NT_SUCCESS(status)) { return status; }
    return copied == Bytes ? STATUS_SUCCESS : STATUS_PARTIAL_COPY;
}

static NTSTATUS FilterRead(PVOID Target, ULONG64 Source, SIZE_T Bytes)
{
    if (ExGetPreviousMode() == UserMode && (Source >= (ULONG64)(ULONG_PTR)MmUserProbeAddress ||
        Bytes > (ULONG64)(ULONG_PTR)MmUserProbeAddress - Source)) { return STATUS_ACCESS_VIOLATION; }
    return FilterReadTrusted(Target, Source, Bytes);
}

static UNICODE_STRING FilterString(const WCHAR *Buffer, USHORT Chars)
{
    UNICODE_STRING text;
    text.Buffer = (PWCH)Buffer;
    text.Length = text.MaximumLength = Chars * sizeof(WCHAR);
    return text;
}

static BOOLEAN FilterPrefix(const WCHAR *Text, USHORT Chars, PCWSTR Prefix, PUSHORT Skip)
{
    UNICODE_STRING source = FilterString(Text, Chars), prefix;
    RtlInitUnicodeString(&prefix, Prefix);
    if (!RtlPrefixUnicodeString(&prefix, &source, TRUE)) { return FALSE; }
    *Skip = prefix.Length / sizeof(WCHAR);
    return TRUE;
}

// Normalize only captured strings: DOS-device prefixes, separators and . / ..
// components. This does not open files or claim to resolve arbitrary reparse aliases.
static NTSTATUS FilterCanonicalizePolicy(const FILTER_POLICY *Policy, const WCHAR *Input, USHORT Chars, WCHAR *Output, PUSHORT OutChars)
{
    USHORT skip = 0, used = 0, read = 0, write = 0;
    if (Chars == 0 || Input[0] != 0x5C) { return STATUS_OBJECT_PATH_SYNTAX_BAD; }
    if (FilterPrefix(Input, Chars, L"\\??\\", &skip) ||
        FilterPrefix(Input, Chars, L"\\GLOBAL??\\", &skip) ||
        FilterPrefix(Input, Chars, L"\\DosDevices\\", &skip)) {
        WCHAR drive;
        ULONG index;
        if (Chars < skip + 2 || Input[skip + 1] != L':') {
            return STATUS_NOT_SUPPORTED;
        }
        if (Chars > skip + 2 && Input[skip + 2] != L'\\') {
            return STATUS_OBJECT_PATH_SYNTAX_BAD;
        }
        drive = RtlUpcaseUnicodeChar(Input[skip]);
        if (drive < L'A' || drive > L'Z') { return STATUS_OBJECT_PATH_SYNTAX_BAD; }
        index = drive - L'A';
        if (Policy == NULL || Policy->VolumeChars[index] == 0) { return STATUS_DEVICE_NOT_READY; }
        used = Policy->VolumeChars[index];
        RtlCopyMemory(Output, Policy->Volumes[index], used * sizeof(WCHAR));
        skip += 2;
    }
    if ((ULONG)used + Chars - skip >= FILTER_PATH_CHARS) { return STATUS_NAME_TOO_LONG; }
    RtlCopyMemory(Output + used, Input + skip, (Chars - skip) * sizeof(WCHAR));
    used += Chars - skip;
    // Compaction is in place; write never passes read.
    while (read < used) {
        USHORT start, length;
        while (read < used && (Output[read] == 0x5C || Output[read] == '/')) { ++read; }
        start = read;
        while (read < used && Output[read] != 0x5C && Output[read] != '/') { ++read; }
        length = read - start;
        if (length == 0 || (length == 1 && Output[start] == '.')) { continue; }
        if (length == 2 && Output[start] == '.' && Output[start + 1] == '.') {
            while (write != 0 && Output[write - 1] != 0x5C) { --write; }
            if (write != 0) { --write; }
            continue;
        }
        Output[write++] = 0x5C;
        RtlMoveMemory(Output + write, Output + start, length * sizeof(WCHAR));
        write += length;
    }
    Output[write] = 0;
    *OutChars = write;
    return STATUS_SUCCESS;
}


static NTSTATUS FilterCanonicalize(const WCHAR *Input, USHORT Chars, WCHAR *Output, PUSHORT OutChars)
{
    return FilterCanonicalizePolicy(g_Policy, Input, Chars, Output, OutChars);
}

static NTSTATUS FilterResolveVolume(FILTER_POLICY *Policy, USHORT drive)
{
    WCHAR linkName[] = L"\\GLOBAL??\\C:";
    HANDLE link = NULL;
    OBJECT_ATTRIBUTES attributes;
    UNICODE_STRING name, volume;
    NTSTATUS status;
    linkName[10] = (WCHAR)(L'A' + drive);
    if (Policy->VolumeChars[drive] == 0) {
        RtlInitUnicodeString(&name, linkName);
        InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        status = ZwOpenSymbolicLinkObject(&link, SYMBOLIC_LINK_QUERY, &attributes);
        if (!NT_SUCCESS(status)) { return status; }
        volume.Buffer = Policy->Volumes[drive];
        volume.Length = 0;
        volume.MaximumLength = sizeof(Policy->Volumes[drive]) - sizeof(WCHAR);
        status = ZwQuerySymbolicLinkObject(link, &volume, NULL);
        ZwClose(link);
        if (!NT_SUCCESS(status)) { return status; }
        Policy->VolumeChars[drive] = volume.Length / sizeof(WCHAR);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS FilterAddPath(FILTER_POLICY *Policy, PCWSTR Path)
{
    UNICODE_STRING actual;
    NTSTATUS status;
    WCHAR target[FILTER_PATH_CHARS];
    USHORT chars = 0, i, drive, used, length;
    ULONG entry;
    while (chars < FILTER_PATH_CHARS && Path[chars]) { ++chars; }
    if (chars == FILTER_PATH_CHARS) { return STATUS_NAME_TOO_LONG; }
    if (chars < 4 || Path[1] != L':' || Path[2] != L'\\') { return STATUS_OBJECT_PATH_SYNTAX_BAD; }
    drive = RtlUpcaseUnicodeChar(Path[0]);
    if (drive < L'A' || drive > L'Z') { return STATUS_OBJECT_PATH_SYNTAX_BAD; }
    if (Path[chars - 1] == L'\\' || Path[chars - 1] == L'/') { return STATUS_OBJECT_NAME_INVALID; }
    for (i = 2; i < chars; ++i) {
        if (Path[i] < 32 || Path[i] == L':' || Path[i] == L'*' || Path[i] == L'?') {
            return STATUS_OBJECT_NAME_INVALID;
        }
        // Reject dot components in policy input (console normalizes before sending).
        if ((Path[i] == L'\\' || Path[i] == L'/') && i + 1 < chars && Path[i + 1] == L'.') {
            USHORT next = i + 2;
            if (next < chars && Path[next] == L'.') { ++next; }
            if (next == chars || Path[next] == L'\\' || Path[next] == L'/') { return STATUS_OBJECT_NAME_INVALID; }
        }
    }
    drive -= L'A';
    status = FilterResolveVolume(Policy, drive);
    if (!NT_SUCCESS(status)) { return status; }
    used = Policy->VolumeChars[drive];
    if (used + chars - 2 >= FILTER_PATH_CHARS) { return STATUS_NAME_TOO_LONG; }
    RtlCopyMemory(target, Policy->Volumes[drive], used * sizeof(WCHAR));
    RtlCopyMemory(target + used, Path + 2, (chars - 2) * sizeof(WCHAR));
    status = FilterCanonicalizePolicy(Policy, target, used + chars - 2,
        Policy->Targets[Policy->Count], &length);
    if (!NT_SUCCESS(status)) { return status; }
    actual = FilterString(Policy->Targets[Policy->Count], length);
    for (entry = 0; entry < Policy->Count; ++entry) {
        UNICODE_STRING existing = FilterString(Policy->Targets[entry], Policy->TargetChars[entry]);
        if (RtlEqualUnicodeString(&actual, &existing, TRUE)) { return STATUS_OBJECT_NAME_COLLISION; }
    }
    Policy->TargetChars[Policy->Count++] = length;
    return STATUS_SUCCESS;
}

// All-or-nothing replacement; no allocations or policy mutation on the syscall path.
NTSTATUS FilterConfigure(const HV_RUNTIME_CONFIG *Config)
{
    FILTER_POLICY *next, *previous;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i;
    if (Config->PathCount > HV_PROTECTED_MAX) { return STATUS_INVALID_PARAMETER; }
    next = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*next), FILTER_POLICY_TAG);
    if (next == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    // Snapshot existing local drive mappings for telemetry even with an empty policy.
    for (i = 0; i < 26; ++i) { (VOID)FilterResolveVolume(next, (USHORT)i); }
    for (i = 0; i < Config->PathCount; ++i) {
        status = FilterAddPath(next, Config->Paths[i]);
        if (!NT_SUCCESS(status)) { ExFreePoolWithTag(next, FILTER_POLICY_TAG); return status; }
    }
    previous = g_Policy;
    g_Policy = next;
    if (previous != NULL) { ExFreePoolWithTag(previous, FILTER_POLICY_TAG); }
    return STATUS_SUCCESS;
}

VOID FilterDestroy(VOID)
{
    if (g_Policy != NULL) { ExFreePoolWithTag(g_Policy, FILTER_POLICY_TAG); g_Policy = NULL; }
}

NTSTATUS FilterInitialize(VOID)
{
    NTSTATUS telemetryStatus = KernelTelemetryInitialize();
    if (!NT_SUCCESS(telemetryStatus)) { return telemetryStatus; }
    if (g_Policy == NULL) {
        static const HV_RUNTIME_CONFIG empty = { .Version = HV_MONITOR_VERSION };
        NTSTATUS status = FilterConfigure(&empty);
        if (!NT_SUCCESS(status)) { return status; }
    }
    // Previous users were drained and all old hooks removed before reopening.
    g_PathHead = g_PathTail = 0;
    g_PathDropped = 0;
    InterlockedExchange(&g_FilterReferences, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS FilterCopyObjectName(const UNICODE_STRING *Name, WCHAR *Output, PUSHORT Chars)
{
    *Chars = 0; Output[0] = 0;
    // An unnamed section legitimately has a zero-length name and a NULL buffer.
    if (Name->Length == 0) { return STATUS_SUCCESS; }
    if ((Name->Length & 1) || Name->Length > Name->MaximumLength || Name->Buffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Name->Length >= FILTER_PATH_CHARS * sizeof(WCHAR)) { return STATUS_NAME_TOO_LONG; }
    *Chars = Name->Length / sizeof(WCHAR);
    RtlCopyMemory(Output, Name->Buffer, Name->Length);
    Output[*Chars] = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS FilterQueryHandleName(HANDLE Handle, BOOLEAN FileOnly, WCHAR *Output, PUSHORT Chars)
{
    union {
        ULONG64 Alignment;
        UCHAR Bytes[sizeof(OBJECT_NAME_INFORMATION) + FILTER_PATH_CHARS * sizeof(WCHAR)];
    } storage;
    POBJECT_NAME_INFORMATION info = (POBJECT_NAME_INFORMATION)storage.Bytes;
    PVOID object = NULL;
    ULONG required = 0;
    NTSTATUS status;
    status = ObReferenceObjectByHandle(Handle, 0, FileOnly ? *IoFileObjectType : NULL,
        ExGetPreviousMode(), &object, NULL);
    if (!NT_SUCCESS(status)) { return status; }
    status = ObQueryNameString(object, info, sizeof(storage), &required);
    if (NT_SUCCESS(status)) {
        status = FilterCopyObjectName(&info->Name, Output, Chars);
    }
    ObDereferenceObject(object);
    return status;
}

static NTSTATUS FilterCaptureAttributes(ULONG64 Pointer, FILTER_PATH_EVENT *Event)
{
    OBJECT_ATTRIBUTES attributes;
    UNICODE_STRING name;
    WCHAR combined[FILTER_PATH_CHARS];
    USHORT rootChars = 0;
    NTSTATUS status = FilterRead(&attributes, Pointer, sizeof(attributes));
    if (!NT_SUCCESS(status)) { return status; }
    Event->RootDirectory = (ULONG64)(ULONG_PTR)attributes.RootDirectory;
    Event->Fields[7] = attributes.Attributes; Event->FieldMask |= 1u << 7;
    if (attributes.Length != sizeof(attributes)) { return STATUS_INVALID_PARAMETER; }
    status = FilterRead(&name, (ULONG64)(ULONG_PTR)attributes.ObjectName, sizeof(name));
    if (!NT_SUCCESS(status)) { return status; }
    if ((name.Length & 1) != 0 || name.MaximumLength < name.Length) { return STATUS_INVALID_PARAMETER; }
    if (name.Length >= sizeof(Event->Raw)) { return STATUS_NAME_TOO_LONG; }
    if (name.Length == 0) { return STATUS_OBJECT_NAME_INVALID; }
    status = FilterRead(Event->Raw, (ULONG64)(ULONG_PTR)name.Buffer, name.Length);
    if (!NT_SUCCESS(status)) { Event->Raw[0] = 0; return status; }
    Event->RawChars = name.Length / sizeof(WCHAR);
    Event->Raw[Event->RawChars] = 0;
    if (Event->Raw[0] == 0x5C) {
        return FilterCanonicalize(Event->Raw, Event->RawChars, Event->Resolved, &Event->ResolvedChars);
    }
    if (attributes.RootDirectory == NULL) { return STATUS_OBJECT_PATH_SYNTAX_BAD; }
    status = FilterQueryHandleName(attributes.RootDirectory, FALSE, combined, &rootChars);
    if (!NT_SUCCESS(status)) { return status; }
    if ((ULONG)rootChars + 1 + Event->RawChars >= FILTER_PATH_CHARS) { return STATUS_NAME_TOO_LONG; }
    combined[rootChars++] = 0x5C;
    RtlCopyMemory(combined + rootChars, Event->Raw, Event->RawChars * sizeof(WCHAR));
    return FilterCanonicalize(combined, rootChars + Event->RawChars, Event->Resolved, &Event->ResolvedChars);
}

static BOOLEAN FilterMatches(const FILTER_PATH_EVENT *Event)
{
    USHORT length = Event->ResolvedChars, i, lastSlash = 0;
    UNICODE_STRING actual;
    ULONG entry;
    if (g_Policy == NULL) { return FALSE; }
    for (i = 0; i < length; ++i) { if (Event->Resolved[i] == 0x5C) { lastSlash = i; } }
    // Streams belong to this same file; the logged path retains the stream suffix.
    for (i = lastSlash; i < length; ++i) { if (Event->Resolved[i] == ':') { length = i; break; } }
    actual = FilterString(Event->Resolved, length);
    for (entry = 0; entry < g_Policy->Count; ++entry) {
        UNICODE_STRING target = FilterString(g_Policy->Targets[entry], g_Policy->TargetChars[entry]);
        if (RtlEqualUnicodeString(&actual, &target, TRUE)) { return TRUE; }
    }
    return FALSE;
}

static VOID FilterQueuePath(FILTER_PATH_EVENT *Event)
{
    HV_LOG_VIEW view;
    Event->ProcessId = HandleToULong(PsGetCurrentProcessId());
    Event->ThreadId = HandleToULong(PsGetCurrentThreadId());
    Event->Core = KeGetCurrentProcessorNumber();
    view.Kind = 2; view.Target = Event->TargetIdx; view.Action = Event->Action;
    view.ProcessId = Event->ProcessId; view.ThreadId = Event->ThreadId;
    view.TargetProcessId = Event->TargetProcessId; view.PreviousMode = Event->PreviousMode;
    view.PathKind = Event->PathKind; view.FieldMask = Event->FieldMask;
    view.Fields = Event->Fields; view.Raw = Event->Raw; view.Resolved = Event->Resolved;
    view.Destination = Event->Destination;
    // Reject noise before it occupies the bounded delivery pipeline. The decision
    // was already made; a log filter cannot permit or deny the syscall itself.
    if (!MonitorMatchesView(&view)) { return; }
    if (InterlockedCompareExchange(&g_PathLogBusy, 1, 0) != 0) {
        InterlockedIncrement(&g_PathDropped); return;
    }
    if (g_PathHead - g_PathTail < FILTER_LOG_COUNT) {
        g_PathLog[g_PathHead++ % FILTER_LOG_COUNT] = *Event;
    } else { InterlockedIncrement(&g_PathDropped); }
    InterlockedExchange(&g_PathLogBusy, 0);
}


static VOID FilterStatus(FILTER_PATH_EVENT *Event, NTSTATUS Status)
{
    if (!NT_SUCCESS(Status) && NT_SUCCESS(Event->Status)) { Event->Status = Status; }
}
static VOID FilterField(FILTER_PATH_EVENT *Event, ULONG Index, ULONG64 Value)
{
    Event->Fields[Index] = Value;
    Event->FieldMask |= 1u << Index;
}
static VOID FilterPointerField(FILTER_PATH_EVENT *Event, ULONG Index, ULONG64 Pointer, BOOLEAN Optional)
{
    ULONG64 value = 0;
    NTSTATUS status;
    if (!Pointer && Optional) { return; }
    status = FilterRead(&value, Pointer, sizeof(value));
    FilterStatus(Event, status);
    if (NT_SUCCESS(status)) { FilterField(Event, Index, value); }
}
static NTSTATUS FilterFileHandle(FILTER_PATH_EVENT *Event, ULONG64 Handle)
{
    NTSTATUS status;
    Event->PathKind = 1;
    status = FilterQueryHandleName((HANDLE)(ULONG_PTR)Handle, TRUE, Event->Raw, &Event->RawChars);
    if (NT_SUCCESS(status)) {
        status = FilterCanonicalize(Event->Raw, Event->RawChars, Event->Resolved, &Event->ResolvedChars);
    }
    return status;
}
static VOID FilterProcessImage(FILTER_PATH_EVENT *Event, PEPROCESS Process)
{
    PUNICODE_STRING image = NULL;
    NTSTATUS status;
    Event->TargetProcessId = HandleToULong(PsGetProcessId(Process));
    Event->PathKind = 3;
    status = SeLocateProcessImageName(Process, &image);
    if (NT_SUCCESS(status)) {
        if (image->Length >= sizeof(Event->Resolved)) { status = STATUS_NAME_TOO_LONG; }
        else {
            Event->ResolvedChars = image->Length / sizeof(WCHAR);
            RtlCopyMemory(Event->Resolved, image->Buffer, image->Length);
        }
        ExFreePool(image);
    }
    FilterStatus(Event, status);
}
static VOID FilterProcessHandle(FILTER_PATH_EVENT *Event, ULONG64 Handle)
{
    PEPROCESS process = NULL;
    NTSTATUS status;
    if ((HANDLE)(ULONG_PTR)Handle == NtCurrentProcess()) {
        FilterProcessImage(Event, PsGetCurrentProcess()); return;
    }
    status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)Handle, 0, *PsProcessType,
        ExGetPreviousMode(), (PVOID *)&process, NULL);
    FilterStatus(Event, status);
    if (NT_SUCCESS(status)) { FilterProcessImage(Event, process); ObDereferenceObject(process); }
}

static VOID FilterObjectMetadata(FILTER_PATH_EVENT *Event, ULONG64 Pointer, BOOLEAN CaptureName)
{
    OBJECT_ATTRIBUTES attributes;
    UNICODE_STRING name;
    NTSTATUS status;
    if (!Pointer) { return; }
    status = FilterRead(&attributes, Pointer, sizeof(attributes));
    if (NT_SUCCESS(status) && attributes.Length != sizeof(attributes)) { status = STATUS_INVALID_PARAMETER; }
    FilterStatus(Event, status);
    if (!NT_SUCCESS(status)) { return; }
    FilterField(Event, 7, attributes.Attributes);
    if (!CaptureName || !attributes.ObjectName) { return; }
    status = FilterRead(&name, (ULONG64)(ULONG_PTR)attributes.ObjectName, sizeof(name));
    if (NT_SUCCESS(status)) {
        if ((name.Length & 1) || name.Length > name.MaximumLength || name.Length >= sizeof(Event->Destination)) {
            status = STATUS_INVALID_PARAMETER;
        } else if (name.Length) {
            status = FilterRead(Event->Destination, (ULONG64)(ULONG_PTR)name.Buffer, name.Length);
        }
    }
    FilterStatus(Event, status);
}

static NTSTATUS FilterRenameDestination(FILTER_PATH_EVENT *Event, ULONG64 Pointer, ULONG Length, BOOLEAN Extended)
{
    struct { ULONG Flags, Padding; ULONG64 Root; ULONG NameLength; } header;
    WCHAR combined[FILTER_PATH_CHARS];
    USHORT chars, rootChars = 0, i;
    NTSTATUS status;
    C_ASSERT(FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) == 20);
    if (Length < 20) { return STATUS_INFO_LENGTH_MISMATCH; }
    RtlZeroMemory(&header, sizeof(header));
    status = FilterRead(&header, Pointer, 20);
    if (!NT_SUCCESS(status)) { return status; }
    FilterField(Event, 2, Extended ? header.Flags : (UCHAR)header.Flags);
    FilterField(Event, 4, header.Root);
    if (header.NameLength == 0 || (header.NameLength & 1) || header.NameLength > Length - 20) {
        return STATUS_INVALID_PARAMETER;
    }
    if (header.NameLength >= sizeof(Event->Destination)) { return STATUS_NAME_TOO_LONG; }
    if (Pointer > MAXULONGLONG - 20) { return STATUS_INVALID_ADDRESS; }
    status = FilterRead(Event->Destination, Pointer + 20, header.NameLength);
    if (!NT_SUCCESS(status)) { return status; }
    chars = (USHORT)(header.NameLength / sizeof(WCHAR));
    if (Event->Destination[0] == L'\\') {
        RtlCopyMemory(combined, Event->Destination, header.NameLength);
    } else {
        if (header.Root) {
            status = FilterQueryHandleName((HANDLE)(ULONG_PTR)header.Root, TRUE, combined, &rootChars);
            if (!NT_SUCCESS(status)) { return status; }
            if (rootChars + 1 >= FILTER_PATH_CHARS) { return STATUS_NAME_TOO_LONG; }
            combined[rootChars++] = L'\\';
        } else {
            // Simple name is relative to the source parent, not the process CWD.
            if (!Event->ResolvedChars) { return STATUS_OBJECT_PATH_SYNTAX_BAD; }
            rootChars = Event->ResolvedChars;
            if (Event->Destination[0] == L':') {
                for (i = 0; i < rootChars; ++i) { if (Event->Resolved[i] == L':') { rootChars = i; break; } }
            } else {
                while (rootChars && Event->Resolved[rootChars - 1] != L'\\') { --rootChars; }
            }
            RtlCopyMemory(combined, Event->Resolved, rootChars * sizeof(WCHAR));
        }
        if (rootChars + chars >= FILTER_PATH_CHARS) { return STATUS_NAME_TOO_LONG; }
        RtlCopyMemory(combined + rootChars, Event->Destination, header.NameLength);
        chars += rootChars;
    }
    return FilterCanonicalize(combined, chars, Event->Destination, &chars);
}

static BOOLEAN FilterDestinationMatches(FILTER_PATH_EVENT *Event)
{
    // Reuse the matcher without placing another large event on the kernel stack.
    USHORT oldChars = Event->ResolvedChars, chars = 0;
    WCHAR saved[FILTER_PATH_CHARS];
    BOOLEAN matches;
    RtlCopyMemory(saved, Event->Resolved, sizeof(saved));
    while (chars < FILTER_PATH_CHARS - 1 && Event->Destination[chars]) { ++chars; }
    RtlCopyMemory(Event->Resolved, Event->Destination, sizeof(Event->Resolved));
    Event->ResolvedChars = chars;
    matches = FilterMatches(Event);
    RtlCopyMemory(Event->Resolved, saved, sizeof(saved));
    Event->ResolvedChars = oldChars;
    return matches;
}

ULONG FilterDecide(ULONG TargetIdx, FILTER_ARGUMENTS *Args, ULONG64 OriginalRsp)
{
    FILTER_PATH_EVENT event;
    static const UCHAR counts[SYSCALL_TARGET_COUNT] = {
        11,6,6,1,5,9,7,10,4,0,5,0,9,  // IDs 9/11 retired
        0,0,0,0,0,0,0,0,0,0,0,       // kernel targets use KernelCapture
        7,0                           // NtDuplicateObject, MmCopyVirtualMemory
    };
    ULONG i, infoClass, flags = 0;
    ULONG64 *a;
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN block = FALSE, sourceMatch = FALSE;
    if (!HvTargetHasPolicy(TargetIdx) || Args == NULL ||
        (__readeflags() & 0x200u) == 0 || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return FILTER_ACTION_CONTINUE;
    }
    if (!MonitorPolicyEnabled(TargetIdx) && !MonitorDebugEnabled(TargetIdx) && !MonitorConsoleEnabled(TargetIdx) &&
        ((g_Policy == NULL || g_Policy->Count == 0) || TargetIdx == SyscallTargetNtAllocateVirtualMemory ||
         TargetIdx >= SyscallTargetNtMapViewOfSection)) { return FILTER_ACTION_CONTINUE; }
    RtlZeroMemory(&event, sizeof(event));
    event.TargetIdx = TargetIdx;
    event.PreviousMode = (ULONG)ExGetPreviousMode();
    a = event.InputArguments;
    a[0] = Args->Rcx; a[1] = Args->Rdx; a[2] = Args->R8; a[3] = Args->R9;
    event.ArgumentMask = (1u << min(counts[TargetIdx], 4)) - 1;
    for (i = 4; i < counts[TargetIdx]; ++i) {
        if (OriginalRsp > MAXULONGLONG - (0x28 + (i - 4) * 8)) { status = STATUS_INVALID_ADDRESS; }
        else { status = FilterReadTrusted(&a[i], OriginalRsp + 0x28 + (i - 4) * 8, sizeof(ULONG64)); }
        if (!NT_SUCCESS(status)) { FilterStatus(&event, status); goto Record; }
        event.ArgumentMask |= 1u << i;
    }
    // Capture each input schema before applying independent policy rules.
    switch (TargetIdx) {
    case SyscallTargetNtDuplicateObject:
        FilterField(&event, 0, (ULONG)a[4]);
        FilterField(&event, 1, a[0]);
        FilterField(&event, 2, a[1]);
        FilterField(&event, 3, a[2]);
        FilterField(&event, 4, (ULONG)a[5]);
        FilterField(&event, 5, (ULONG)a[6]);
        FilterField(&event, 7, a[3]); // output pointer only: never dereferenced
        FilterField(&event, 8, !!((ULONG)a[6] & DUPLICATE_SAME_ACCESS));
        FilterField(&event, 9, !!((ULONG)a[6] & DUPLICATE_CLOSE_SOURCE));
        FilterField(&event, 10, !!((ULONG)a[6] & DUPLICATE_SAME_ATTRIBUTES));
        FilterField(&event, 11, HV_DUP_REASON_NONE);

        // SourceProcessHandle identifies the owner of SourceHandle, not the
        // object represented by SourceHandle. No remote handle-table attach.
        FilterProcessHandle(&event, a[0]);
        if (event.TargetProcessId) { FilterField(&event, 6, event.TargetProcessId); }
        RtlCopyMemory(event.Raw, event.Resolved, sizeof(event.Raw));
        event.RawChars = event.ResolvedChars;
        RtlZeroMemory(event.Resolved, sizeof(event.Resolved));
        event.ResolvedChars = 0;
        event.TargetProcessId = 0;
        event.PathKind = 0;
        // NULL recipient is legal for CLOSE_SOURCE-only operations.
        if (a[2]) { FilterProcessHandle(&event, a[2]); }
        break;
    case SyscallTargetNtCreateFile:
    case SyscallTargetNtOpenFile:
        event.PathKind = 1;
        status = FilterCaptureAttributes(a[2], &event);
        block = NT_SUCCESS(status) && FilterMatches(&event);
        FilterStatus(&event, status);
        FilterField(&event, 0, (ULONG)a[1]);
        if (TargetIdx == SyscallTargetNtCreateFile) {
            FilterField(&event, 1, (ULONG)a[6]);
            FilterField(&event, 2, (ULONG)a[7]);
            FilterField(&event, 3, (ULONG)a[8]);
            FilterField(&event, 4, (ULONG)a[5]);
            FilterPointerField(&event, 5, a[4], TRUE);
        } else {
            FilterField(&event, 1, (ULONG)a[4]);
            FilterField(&event, 3, (ULONG)a[5]);
        }
        FilterField(&event, 6, event.RootDirectory);
        break;

    case SyscallTargetNtDeleteFile:
        event.PathKind = 1;
        status = FilterCaptureAttributes(a[0], &event);
        block = NT_SUCCESS(status) && FilterMatches(&event);
        FilterStatus(&event, status);
        FilterField(&event, 0, event.RootDirectory);
        break;

    case SyscallTargetNtSetInformationFile:
        infoClass = (ULONG)a[4];
        FilterField(&event, 0, infoClass);
        FilterField(&event, 1, (ULONG)a[3]);
        status = FilterFileHandle(&event, a[0]);
        sourceMatch = NT_SUCCESS(status) && FilterMatches(&event);
        FilterStatus(&event, status);
        if (infoClass == FileDispositionInformation || infoClass == FileDispositionInformationEx) {
            ULONG bytes = infoClass == FileDispositionInformation ? 1 : 4;
            status = (ULONG)a[3] < bytes ? STATUS_INFO_LENGTH_MISMATCH : FilterRead(&flags, a[2], bytes);
            if (NT_SUCCESS(status)) {
                FilterField(&event, 2, flags);
                block = sourceMatch && (bytes == 1 ? flags != 0 : (flags & FILE_DISPOSITION_DELETE) != 0);
            }
        } else if (infoClass == FileRenameInformation || infoClass == FileRenameInformationEx ||
            infoClass == FileLinkInformation || infoClass == FileLinkInformationEx ||
            infoClass == FileRenameInformationBypassAccessCheck || infoClass == FileLinkInformationBypassAccessCheck ||
            infoClass == FileRenameInformationExBypassAccessCheck || infoClass == FileLinkInformationExBypassAccessCheck) {
            status = FilterRenameDestination(&event, a[2], (ULONG)a[3],
                infoClass == FileRenameInformationEx || infoClass == FileLinkInformationEx ||
                infoClass == FileRenameInformationExBypassAccessCheck || infoClass == FileLinkInformationExBypassAccessCheck);
            block = sourceMatch || (NT_SUCCESS(status) && FilterDestinationMatches(&event));
            if (!NT_SUCCESS(status)) { event.Destination[0] = 0; }
        } else if (infoClass == FileEndOfFileInformation || infoClass == FileAllocationInformation) {
            if ((ULONG)a[3] < sizeof(ULONG64)) { status = STATUS_INFO_LENGTH_MISMATCH; }
            else { FilterPointerField(&event, 3, a[2], FALSE); block = sourceMatch && (event.FieldMask & (1u << 3)); }
        }
        FilterStatus(&event, status);
        break;

    case SyscallTargetNtReadFile:
    case SyscallTargetNtWriteFile:
        status = FilterFileHandle(&event, a[0]);
        block = TargetIdx == SyscallTargetNtWriteFile && NT_SUCCESS(status) && FilterMatches(&event);
        FilterStatus(&event, status);
        FilterField(&event, 0, a[0]);
        FilterField(&event, 1, (ULONG)a[6]);
        FilterPointerField(&event, 2, a[7], TRUE);
        FilterField(&event, 3, a[7] == 0);
        break;

    case SyscallTargetNtCreateSection:
        FilterObjectMetadata(&event, a[2], TRUE);
        FilterField(&event, 0, (ULONG)a[1]);
        FilterField(&event, 1, (ULONG)a[4]);
        FilterField(&event, 2, (ULONG)a[5]);
        FilterPointerField(&event, 3, a[3], TRUE);
        FilterField(&event, 4, a[6]);
        if (a[6]) {
            status = FilterFileHandle(&event, a[6]);
            block = NT_SUCCESS(status) && FilterMatches(&event);
            FilterStatus(&event, status);
        }
        break;

    case SyscallTargetNtMapViewOfSection:
        FilterField(&event, 0, a[0]);
        FilterField(&event, 1, a[1]);
        FilterPointerField(&event, 2, a[2], FALSE);
        FilterPointerField(&event, 3, a[6], FALSE);
        FilterPointerField(&event, 4, a[5], TRUE);
        FilterField(&event, 5, a[3]);
        FilterField(&event, 6, a[4]);
        FilterField(&event, 7, (ULONG)a[7]);
        FilterField(&event, 8, (ULONG)a[8]);
        FilterField(&event, 9, (ULONG)a[9]);
        FilterProcessHandle(&event, a[1]);
        // Object name is not a backing-file path; do not invent a file identity.
        status = FilterQueryHandleName((HANDLE)(ULONG_PTR)a[0], FALSE, event.Raw, &event.RawChars);
        FilterStatus(&event, status);
        event.PathKind = 2;
        break;

    case SyscallTargetNtAllocateVirtualMemory:
        FilterField(&event, 0, a[0]);
        FilterPointerField(&event, 1, a[1], FALSE);
        FilterPointerField(&event, 2, a[3], FALSE);
        FilterField(&event, 3, a[2]);
        FilterField(&event, 4, (ULONG)a[4]);
        FilterField(&event, 5, (ULONG)a[5]);
        FilterProcessHandle(&event, a[0]);
        break;

    case SyscallTargetNtOpenProcess:
        {
            CLIENT_ID client;
            PEPROCESS process = NULL;
            FilterField(&event, 0, (ULONG)a[1]);
            FilterObjectMetadata(&event, a[2], FALSE);
            status = FilterRead(&client, a[3], sizeof(client));
            FilterStatus(&event, status);
            if (NT_SUCCESS(status)) {
                event.TargetProcessId = HandleToULong(client.UniqueProcess);
                event.TargetThreadId = HandleToULong(client.UniqueThread);
                FilterField(&event, 1, (ULONG64)(ULONG_PTR)client.UniqueProcess);
                FilterField(&event, 2, (ULONG64)(ULONG_PTR)client.UniqueThread);
                status = PsLookupProcessByProcessId(client.UniqueProcess, &process);
                FilterStatus(&event, status);
                if (NT_SUCCESS(status)) {
                    FilterProcessImage(&event, process);
                    ObDereferenceObject(process);
                }
            }
        }
        break;

    case SyscallTargetNtProtectVirtualMemory:
        FilterField(&event, 0, a[0]);
        FilterPointerField(&event, 1, a[1], FALSE);
        FilterPointerField(&event, 2, a[2], FALSE);
        FilterField(&event, 3, (ULONG)a[3]);
        FilterProcessHandle(&event, a[0]);
        break;

    default: break;
    }
Record:
    event.Action = block ? FILTER_ACTION_BLOCK : FILTER_ACTION_CONTINUE;

    // Legacy protected paths take priority. Policy matching sees the original
    // inputs and can neither undo a denial nor grant additional access.
    event.ProcessId = HandleToULong(PsGetCurrentProcessId());
    event.ThreadId = HandleToULong(PsGetCurrentThreadId());
    if (!block && NT_SUCCESS(event.Status)) {
        HV_LOG_VIEW view = {2, TargetIdx, 0, event.ProcessId, event.ThreadId,
            event.TargetProcessId, event.PreviousMode, event.PathKind, event.FieldMask,
            event.Fields, event.Raw, event.Resolved, event.Destination};
        ULONG64 remove = 0;
        ULONG action = MonitorPolicyAction(&view, &remove);

        if (action == 1) {
            event.Action = FILTER_ACTION_BLOCK;
            if (TargetIdx == SyscallTargetNtDuplicateObject) {
                FilterField(&event, 11, HV_DUP_REASON_DENY_RULE);
            }
        } else if (action == 2) {
            // Generic/MAXIMUM_ALLOWED requests cannot be narrowed by clearing
            // object-specific bits. Deny those instead of silently bypassing a rule.
            ULONG effective;
            if (TargetIdx == SyscallTargetNtDuplicateObject) {
                ULONG reason;
                event.Action = HvPolicyRestrictDuplicate((ULONG)a[4], (ULONG)a[6],
                    (ULONG)remove, &effective, &reason);
                if (event.Action == 2) {
                    // Argument 5 lives in the current guest's kernel stack.
                    // Do not write through the caller's output-handle pointer.
                    NTSTATUS writeStatus = STATUS_INVALID_ADDRESS;
                    if ((OriginalRsp & 7) == 0 &&
                        OriginalRsp >= (ULONG64)(ULONG_PTR)MmSystemRangeStart &&
                        OriginalRsp <= MAXULONGLONG - 0x2B) {
                        __try {
                            volatile LONG *slot = (volatile LONG *)(ULONG_PTR)(OriginalRsp + 0x28);
                            // Preserve a concurrently changed slot by denying rather
                            // than overwriting a request we did not capture.
                            writeStatus = (ULONG)InterlockedCompareExchange(slot,
                                (LONG)effective, (LONG)(ULONG)a[4]) == (ULONG)a[4] ?
                                STATUS_SUCCESS : STATUS_RETRY;
                        } __except (EXCEPTION_EXECUTE_HANDLER) {
                            writeStatus = GetExceptionCode();
                        }
                    }
                    if (NT_SUCCESS(writeStatus)) { event.EffectiveAccess = effective; }
                    else {
                        event.Action = FILTER_ACTION_BLOCK;
                        reason = HV_DUP_REASON_STACK_WRITE;
                        FilterStatus(&event, writeStatus);
                    }
                }
                FilterField(&event, 11, reason);
            } else {
                event.Action = HvPolicyRestrictAccess((ULONG)Args->Rdx, (ULONG)remove, &effective);
                if (event.Action == 2) {
                    Args->Rdx = effective;
                    event.EffectiveAccess = effective;
                }
            }
        }
    }
    if (MonitorDebugEnabled(TargetIdx) || MonitorConsoleEnabled(TargetIdx)) { FilterQueuePath(&event); }
    return event.Action;
}

VOID FilterDrainLogs(VOID)
{
    // Only the serialized PASSIVE drain consumer uses these large buffers.
    static FILTER_PATH_EVENT event;
    static HV_MONITOR_EVENT message;
    ULONG budget, i;
    LONG dropped;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return; }
    for (budget = 0; budget < FILTER_LOG_COUNT; ++budget) {
        UNICODE_STRING raw, resolved, destination;
        if (InterlockedCompareExchange(&g_PathLogBusy, 1, 0) != 0) { break; }
        if (g_PathTail == g_PathHead) {
            InterlockedExchange(&g_PathLogBusy, 0);
            break;
        }
        event = g_PathLog[g_PathTail++ % FILTER_LOG_COUNT];
        InterlockedExchange(&g_PathLogBusy, 0);

        InterlockedIncrement64(&g_SyscallHitCount[event.TargetIdx]);
        raw = FilterString(event.Raw, event.RawChars);
        resolved = FilterString(event.Resolved, event.ResolvedChars);
        RtlInitUnicodeString(&destination, event.Destination);
        {
            RtlZeroMemory(&message, sizeof(message));
            message.Kind = 2; message.Target = event.TargetIdx; message.Core = event.Core;
            message.Action = event.Action; message.ProcessId = event.ProcessId; message.ThreadId = event.ThreadId;
            message.Status = (ULONG)event.Status; message.TargetProcessId = event.TargetProcessId;
            message.Reserved = event.Action == 2 ? 1u : 0u;
            message.Arguments[1] = event.EffectiveAccess;
            message.TargetThreadId = event.TargetThreadId; message.FieldMask = event.FieldMask;
            message.ArgumentMask = event.ArgumentMask; message.PathKind = event.PathKind;
            message.PreviousMode = event.PreviousMode;
            RtlCopyMemory(message.Fields, event.Fields, sizeof(message.Fields));
            RtlCopyMemory(message.InputArguments, event.InputArguments, sizeof(message.InputArguments));
            RtlCopyMemory(message.Raw, event.Raw, sizeof(message.Raw));
            RtlCopyMemory(message.Resolved, event.Resolved, sizeof(message.Resolved));
            RtlCopyMemory(message.Destination, event.Destination, sizeof(message.Destination));
            if (!MonitorMatches(&message)) { continue; }
            MonitorPublish(&message);
        }
        if (!MonitorDebugEnabled(event.TargetIdx)) { continue; }
        HV_INFO("DETAIL: %s %s pid=%lu tid=%lu target_pid=%lu target_tid=%lu mode=%lu capture=0x%08lX kind=%lu raw=\"%wZ\" resolved=\"%wZ\" destination=\"%wZ\"\n",
            g_SyscallTargetMeta[event.TargetIdx].Name, event.Action == 1 ? "BLOCK" : event.Action == 2 ? "MODIFIED" : "CONTINUE",
            event.ProcessId, event.ThreadId, event.TargetProcessId, event.TargetThreadId,
            event.PreviousMode, (ULONG)event.Status, event.PathKind, &raw, &resolved, &destination);
        if (event.Action == 2) { HV_INFO("DETAIL_POLICY: effective_access=0x%I64X (original in access field)\n", event.EffectiveAccess); }
        for (i = 0; i < 12; ++i) {
            if (event.FieldMask & (1u << i)) {
                char value[HV_SYMBOL_TEXT_CAPACITY];
                size_t offset = 0, length = 0;
                HvFormatField(event.TargetIdx, i, event.FieldMask, event.Fields, value, sizeof(value));
                while (value[length]) { ++length; }
                // DbgPrintEx transmits at most 512 bytes per call. Keep long masks intact.
                do {
                    HV_INFO("DETAIL_FIELD: %s %s%s=%.*s\n", g_SyscallTargetMeta[event.TargetIdx].Name,
                        g_DetailLabels[event.TargetIdx][i] ? g_DetailLabels[event.TargetIdx][i] : "field",
                        offset ? " (continued)" : "", 240, value + offset);
                    offset += 240;
                } while (offset < length);
            }
        }
    }
    dropped = InterlockedExchange(&g_PathDropped, 0);
    if (dropped && MonitorDebugEnabled(0)) { HV_INFO("DETAIL: queue dropped=%lu\n", (ULONG)dropped); }
}

BOOLEAN FilterTryEnter(VOID)
{
    LONG references = InterlockedIncrement(&g_FilterReferences);
    if ((references & FILTER_CLOSED) != 0) {
        (VOID)InterlockedDecrement(&g_FilterReferences);
        return FALSE;
    }
    return TRUE;
}

VOID FilterLeave(VOID)
{
    (VOID)InterlockedDecrement(&g_FilterReferences);
}

VOID FilterCloseAndDrain(VOID)
{
    LARGE_INTEGER interval;
    interval.QuadPart = -10000; // 1 ms, shutdown only; never in the filter.
    (VOID)InterlockedOr(&g_FilterReferences, FILTER_CLOSED);
    while ((InterlockedCompareExchange(&g_FilterReferences, 0, 0) & ~FILTER_CLOSED) != 0) {
        // A guest read can fault and schedule. Keep VMX and driver text alive
        // until its final root continuation (or real guest RET) has completed.
        (VOID)KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }
}
