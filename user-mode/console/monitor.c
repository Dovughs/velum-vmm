#include "app.h"
#include "telemetry_fields.h"
#include "telemetry_symbols.h"
#include "kernel_telemetry.h"
#include <errno.h>
#include <io.h>
#include <wctype.h>
#define HV_UPCASE(c) ((WCHAR)towupper(c))
#include "telemetry_match.h"
#include "telemetry_policy.h"
static const char *const g_DetailLabels[HV_MONITOR_TARGETS][12] = HV_DETAIL_LABELS;

static HV_RUNTIME_CONFIG g_RuntimeView;
static HANDLE g_Device = INVALID_HANDLE_VALUE;
static HV_MONITOR_CONFIG g_Settings = { HV_MONITOR_VERSION, 0, 0, 0 };
typedef struct _CONSOLE_PROFILE {
    ULONG Magic, Version, Size, Abi, Targets, SelectedHooks, Checksum, Reserved;
    HV_LOG_FILTER Filters[HV_MONITOR_TARGETS];
} CONSOLE_PROFILE;
static CONSOLE_PROFILE g_Profile;
static WCHAR g_ProfilePath[32768];
static int Configure(int Capturing);
static int RuntimeRead(HV_RUNTIME_CONFIG *Config);
static int LogFilterIo(HV_LOG_FILTER *Filter, int Write);
static int ProfileInitialize(void);
static int PolicyIo(HV_POLICY_CONFIG *Policy, int Write);
_Static_assert(sizeof(HV_MONITOR_EVENT) == 3376, "monitor ABI size");
_Static_assert(offsetof(HV_MONITOR_EVENT, Raw) == 1328, "monitor ABI offset");

static int OpenMonitor(void)
{
    DWORD bytes;
    if (g_Device != INVALID_HANDLE_VALUE) { return 1; }
    g_Device = CreateFileW(HV_MONITOR_DEVICE, GENERIC_READ | GENERIC_WRITE, 0,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_Device == INVALID_HANDLE_VALUE) {
        AppError("Open monitor (run as administrator; only one console may connect)", GetLastError());
        return 0;
    }
    if (!DeviceIoControl(g_Device, IOCTL_HV_MONITOR_GET, NULL, 0, &g_Settings,
        sizeof(g_Settings), &bytes, NULL) || bytes != sizeof(g_Settings) ||
        g_Settings.Version != HV_MONITOR_VERSION) {
        puts("[ERROR] Monitor protocol unavailable or incompatible. Load the matching driver.");
        AppMonitorClose(); return 0;
    }
    // A console connection never restores log routes or enables saved hooks.
    g_Settings.DebugMask = g_Settings.ConsoleMask = 0;
    if (!Configure(0) || !ProfileInitialize()) { AppMonitorClose(); return 0; }
    return 1;
}

static int Configure(int Capturing)
{
    HV_MONITOR_CONFIG config = g_Settings;
    DWORD bytes;
    if (!Capturing) { config.ConsoleMask = 0; }
    if (!DeviceIoControl(g_Device, IOCTL_HV_MONITOR_SET, &config, sizeof(config), NULL, 0, &bytes, NULL)) {
        AppError("Configure log routing", GetLastError()); return 0;
    }
    return 1;
}

int AppProcessorIo(HV_PROCESSOR_CONFIG *Config, int Write)
{
    DWORD bytes = 0;
    if (!OpenMonitor()) { return 0; }
    if (!DeviceIoControl(g_Device, Write ? IOCTL_HV_PROCESSOR_SET : IOCTL_HV_PROCESSOR_GET,
        Config, sizeof(*Config), Write ? NULL : Config, Write ? 0 : sizeof(*Config),
        &bytes, NULL)) {
        AppError(Write ? "Apply processor settings" : "Read processor settings", GetLastError());
        return 0;
    }
    if (!Write && (bytes != sizeof(*Config) || !HvProcessorValid(Config))) {
        puts("[ERROR] Invalid processor configuration response."); return 0;
    }
    return 1;
}

void AppMonitorClose(void)
{
    if (g_Device != INVALID_HANDLE_VALUE) { CloseHandle(g_Device); g_Device = INVALID_HANDLE_VALUE; }
}

static void PrintPath(const WCHAR *Path)
{
    WCHAR safe[HV_MONITOR_PATH_CHARS];
    char text[HV_MONITOR_PATH_CHARS * 4];
    ULONG i;
    UINT codepage = GetConsoleOutputCP();
    for (i = 0; i < HV_MONITOR_PATH_CHARS - 1 && Path[i]; ++i) {
        safe[i] = Path[i] < 32 || Path[i] == 127 ? L'?' : Path[i];
    }
    safe[i] = 0;
    if (WideCharToMultiByte(codepage ? codepage : CP_UTF8, 0, safe, -1,
        text, sizeof(text), NULL, NULL)) { fputs(text, stdout); }
    else { fputs("<unavailable>", stdout); }
}

static int IsOwnConsoleWrite(const HV_MONITOR_EVENT *Event)
{
    static const WCHAR consolePath[] = L"\\Device\\ConDrv";
    const size_t length = ARRAYSIZE(consolePath) - 1;
    return Event->Kind == 2 && Event->Target == 5 && Event->Action == 0 &&
        Event->ProcessId == GetCurrentProcessId() && Event->PathKind == 1 && Event->Status == 0 &&
        !_wcsnicmp(Event->Raw, consolePath, length) &&
        (Event->Raw[length] == 0 || Event->Raw[length] == L'\\');
}

static void PrintEvent(const HV_MONITOR_EVENT *Event)
{
    FILETIME utc, local;
    SYSTEMTIME time = { 0 };
    if (!HvTargetSupported(Event->Target) || IsOwnConsoleWrite(Event)) { return; }
    utc.dwLowDateTime = (DWORD)Event->Time100ns;
    utc.dwHighDateTime = (DWORD)(Event->Time100ns >> 32);
    if (FileTimeToLocalFileTime(&utc, &local)) { FileTimeToSystemTime(&local, &time); }
    printf("%02u:%02u:%02u.%03u #%llu CPU%-2lu %-24s ", time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds, (unsigned long long)Event->Sequence,
        Event->Core, g_TargetNames[Event->Target]);
    if (Event->Kind == 1) {
        printf("ENTRY RCX=%016llX RDX=%016llX R8=%016llX R9=%016llX RSP=%016llX RIP=%016llX\n",
            Event->Arguments[0], Event->Arguments[1], Event->Arguments[2],
            Event->Arguments[3], Event->Arguments[4], Event->Arguments[5]);
    } else if (Event->Kind == 2 || Event->Kind == 3) {
        ULONG i;
        if (Event->Kind == 3) {
            puts("KERNEL ENTRY (inputs only; no return tracking)");
            if (Event->Reserved & 2u) { printf("    caller_return=0x%llX\n", Event->Arguments[4]); }
            else { puts("    caller_return=<not captured in this context>"); }
            if (Event->Reserved & 4u) {
                fputs("    caller_module=", stdout); PrintPath(Event->Raw);
                printf("+0x%llX (module snapshot at drain time)\n", Event->Arguments[5]);
            } else { puts("    caller_module=<unresolved>"); }
        }
        else printf("%s PID=%lu TID=%lu target_PID=%lu requested_TID=%lu mode=%s capture=%08lX\n",
            Event->Action == 1 ? "BLOCKED" : Event->Action == 2 ? "MODIFIED" : "CONTINUE", Event->ProcessId, Event->ThreadId,
            Event->TargetProcessId, Event->TargetThreadId, Event->PreviousMode ? "user" : "kernel", Event->Status);
        if (Event->Kind != 3 && Event->Raw[0]) {
            fputs(Event->Target == HV_TARGET_DUPLICATE_OBJECT ? "    source_process_image=\"" :
                Event->PathKind == 2 ? "    section_object_name=\"" : "    raw_path=\"", stdout);
            PrintPath(Event->Raw); puts("\"");
        }
        if (Event->Resolved[0]) {
            fputs(Event->Target == HV_TARGET_DUPLICATE_OBJECT ? "    recipient_process_image=\"" :
                Event->PathKind >= 2 ? "    target_image=\"" : "    resolved_path=\"", stdout);
            PrintPath(Event->Resolved); puts("\"");
        }
        if (Event->Destination[0]) {
            fputs(Event->Target == 6 ? "    requested_section_name=\"" : "    destination_path=\"", stdout);
            PrintPath(Event->Destination); puts("\"");
        }
        for (i = 0; i < 12; ++i) {
            if (Event->FieldMask & (1u << i)) {
                char value[HV_SYMBOL_TEXT_CAPACITY];
                HvFormatField(Event->Target, i, Event->FieldMask, Event->Fields, value, sizeof(value));
                printf("    %s=%s\n", g_DetailLabels[Event->Target][i] ?
                    g_DetailLabels[Event->Target][i] : "field", value);
            }
        }
        if (Event->Kind == 3) {
            int tagField = HvKernelTagField(Event->Target);
            int sizeField = HvKernelSizeField(Event->Target);
            if (tagField >= 0 && (Event->FieldMask & (1u << tagField))) {
                char tag[5]; HvKernelTagText((ULONG)Event->Fields[tagField], tag);
                printf("    pool_tag_text='%s' (memory byte order)\n", tag);
            }
            if (sizeField >= 0 && (Event->FieldMask & (1u << sizeField))) {
                ULONG64 size = Event->Fields[sizeField];
                printf("    size=%llu bytes (%.2f KiB)\n", size, (double)size / 1024.0);
            }
        }
        if (Event->Reserved & 1u) { printf("    effective_access=0x%llX (original request above)\n", Event->Arguments[1]); }
        if (Event->Target == HV_TARGET_DUPLICATE_OBJECT) {
            puts("    target_PID identifies the recipient process; source_pid owns source_handle.");
            puts("    Object type/granted source access and returned handle are not captured.");
            if (Event->Action == 1 && (Event->FieldMask & (1u << 9)) && Event->Fields[9]) {
                puts("    CLOSE_SOURCE was not executed because this call was denied.");
            }
        }
        if (Event->Target == 7) {
            if (!Event->Raw[0]) { puts(Event->Status == 0 ? "    section_object_name=<unnamed>" : "    section_object_name=<unavailable>"); }
            puts("    backing_file_path=<not resolved; section name is not a file path>");
        }
        if (Event->Target == HV_TARGET_COPY_VIRTUAL_MEMORY) {
            puts("    Process objects are addresses, not handles/PIDs. Output pointer is not a copied-byte result.");
            if (Event->FieldMask != 0x7Fu) { puts("    Stack arguments were not fully captured; missing fields are unavailable."); }
        } else if (Event->Status == 0xC00000BBu) { puts("    capture_note=path or metadata resolution is unsupported; raw data retained"); }
        if (Event->Status) { printf("    Partial capture. Valid argument mask=0x%lX; missing decoded fields are unavailable.\n", Event->ArgumentMask); }
    } else { puts("<unknown event type>"); }
}

static void LiveView(void)
{
    HV_MONITOR_BATCH_DATA *batch;
    DWORD bytes, inputMode;
    ULONG64 lastDropped = 0;
    int paused = 0;
    if (!g_Settings.ConsoleMask) { puts("Select Console or Both for at least one target first."); return; }
    if (!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &inputMode)) {
        puts("[ERROR] Live view requires an interactive console."); return;
    }
    batch = calloc(1, sizeof(*batch));
    if (!batch) { puts("[ERROR] Cannot allocate monitor buffer."); return; }
    if (!Configure(1)) { free(batch); return; }
    InterlockedExchange(&g_Interrupted, 0);
    puts("\nLIVE MONITOR  |  P: pause/resume  |  C: clear screen  |  Q / Esc / Ctrl+C: stop");
    puts("Timestamps reflect drain time. Capture status is not the syscall result.\n");
    puts("Own console-output writes are omitted from this view to prevent feedback.\n");
    while (!g_Interrupted) {
        ULONG i;
        fflush(stdout);
        if (_kbhit()) {
            int key = _getch();
            if (key == 'q' || key == 'Q' || key == 27) { break; }
            if (key == 'c' || key == 'C') {
                AppClearScreen();
                puts(paused ? "[PAUSED] P: resume | C: clear | Q: stop" : "[LIVE] P: pause | C: clear | Q: stop");
            }
            if (key == 'p' || key == 'P') {
                paused = !paused;
                if (!Configure(!paused)) { break; }
                lastDropped = 0;
                puts(paused ? "[PAUSED] Console capture is disabled; events are not retained." : "[LIVE] Console capture resumed.");
            }
        }
        if (paused) { Sleep(50); continue; }
        if (!DeviceIoControl(g_Device, IOCTL_HV_MONITOR_READ, NULL, 0,
            batch, sizeof(*batch), &bytes, NULL)) {
            AppError("Read live events", GetLastError()); break;
        }
        if (bytes < offsetof(HV_MONITOR_BATCH_DATA, Events) ||
            batch->Version != HV_MONITOR_VERSION || batch->Count > HV_MONITOR_BATCH ||
            bytes != offsetof(HV_MONITOR_BATCH_DATA, Events) + batch->Count * sizeof(HV_MONITOR_EVENT)) {
            puts("[ERROR] Invalid event batch; capture stopped."); break;
        }
        if (batch->Dropped != lastDropped) {
            printf("[OVERFLOW] Delivery queue dropped %llu event(s) in this session.\n", batch->Dropped);
            lastDropped = batch->Dropped;
        }
        for (i = 0; i < batch->Count; ++i) { PrintEvent(&batch->Events[i]); }
        // Flush once per batch, not once per printf fragment or event.
        fflush(stdout);
        // Drain backlog immediately. Only an empty queue needs a polling delay.
        if (batch->Count == 0) { Sleep(10); }
    }
    (void)Configure(0);
    free(batch);
    puts("[STOPPED] Console capture disabled. DebugView routing is unchanged.");
}


static int RuntimeRead(HV_RUNTIME_CONFIG *Config)
{
    DWORD bytes;
    if (!DeviceIoControl(g_Device, IOCTL_HV_RUNTIME_GET, NULL, 0, Config,
        sizeof(*Config), &bytes, NULL) || bytes != sizeof(*Config) ||
        Config->Version != HV_MONITOR_VERSION || Config->PathCount > HV_PROTECTED_MAX ||
        Config->Enabled > 1 || Config->Reserved != 0 || (Config->HookMask & ~HV_MONITOR_ALL)) {
        AppError("Read hook state", GetLastError()); return 0;
    }
    return 1;
}


static int RuntimeWrite(HV_RUNTIME_CONFIG *Config)
{
    DWORD bytes;
    Config->Enabled = Config->HookMask != 0;
    if (!DeviceIoControl(g_Device, IOCTL_HV_RUNTIME_SET, Config, sizeof(*Config),
        NULL, 0, &bytes, NULL)) {
        DWORD error = GetLastError();
        AppError("Update hook configuration", error);
        if (error == ERROR_NOT_SUPPORTED) {
            puts("  A selected hook or entry instruction is unsupported on this Windows build.");
            puts("  Check the driver's HOOK prepare/commit failure for the target name and status.");
        } else if (error == ERROR_INVALID_PARAMETER) {
            puts("  Check configuration values, duplicate paths and local drive paths.");
        }
        return 0;
    }
    return 1;
}

void AppMonitorSummary(void)
{
    ULONG i, hooks = 0;
    if (!OpenMonitor() || !RuntimeRead(&g_RuntimeView)) {
        puts("  Driver unavailable."); return;
    }
    for (i = 0; i < HV_MONITOR_TARGETS; ++i) {
        if (g_RuntimeView.HookMask & (1u << i)) { ++hooks; }
    }
    printf("  Active hooks: %lu | Files: %lu / %u\n", hooks, g_RuntimeView.PathCount, HV_PROTECTED_MAX);
    printf("  DebugView: %s | Console logs: %s\n",
        g_Settings.DebugMask ? "ON" : "OFF", g_Settings.ConsoleMask ? "ARMED" : "OFF");
}

static int ReadLine(char *Line, int Capacity)
{
    size_t length;
    fflush(stdout);
    if (!fgets(Line, Capacity, stdin)) { return 0; }
    if (!strchr(Line, '\n') && !feof(stdin)) {
        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF) { }
        puts("  Input is too long."); return 0;
    }
    length = strcspn(Line, "\r\n"); Line[length] = 0;
    return 1;
}

static int NormalizePath(const char *Line, UINT Codepage, WCHAR *Output)
{
    WCHAR input[HV_MONITOR_PATH_CHARS];
    DWORD chars;
    if (!MultiByteToWideChar(Codepage, MB_ERR_INVALID_CHARS, Line, -1, input, HV_MONITOR_PATH_CHARS)) {
        puts("  Invalid text encoding or path too long."); return 0;
    }
    if (!AppCleanPath(input)) { puts("  Empty path or unmatched quotes."); return 0; }
    if (wcslen(input) < 4 || !(((input[0] >= L'A' && input[0] <= L'Z') ||
        (input[0] >= L'a' && input[0] <= L'z')) && input[1] == L':' && input[2] == L'\\')) {
        puts("  Use an absolute local path, for example D:\\Data\\document.txt."); return 0;
    }
    ZeroMemory(Output, HV_MONITOR_PATH_CHARS * sizeof(WCHAR));
    chars = GetFullPathNameW(input, HV_MONITOR_PATH_CHARS, Output, NULL);
    if (chars == 0 || chars >= HV_MONITOR_PATH_CHARS) { puts("  Invalid path or path too long."); return 0; }
    return 1;
}

static int AppendPath(HV_RUNTIME_CONFIG *Config, const char *Line, UINT Codepage)
{
    WCHAR path[HV_MONITOR_PATH_CHARS];
    ULONG i;
    if (!NormalizePath(Line, Codepage, path)) { return 0; }
    for (i = 0; i < Config->PathCount; ++i) {
        if (_wcsicmp(Config->Paths[i], path) == 0) { return 2; }
    }
    if (Config->PathCount == HV_PROTECTED_MAX) { puts("  Protected list is full."); return 0; }
    memcpy(Config->Paths[Config->PathCount++], path, sizeof(path));
    return 1;
}

static void ImportPaths(HV_RUNTIME_CONFIG *Config)
{
    char line[HV_MONITOR_PATH_CHARS * 4];
    WCHAR filename[HV_MONITOR_PATH_CHARS];
    FILE *file = NULL;
    ULONG before = Config->PathCount, number = 0, duplicates = 0;
    int valid = 1;
    puts("  UTF-8 text file to import (one absolute path per line):");
    if (!ReadLine(line, sizeof(line)) || !line[0] ||
        !NormalizePath(line, GetConsoleCP() ? GetConsoleCP() : CP_UTF8, filename)) { return; }
    if (_wfopen_s(&file, filename, L"rb") || !file) { puts("  Cannot open import file."); return; }
    while (fgets(line, sizeof(line), file)) {
        char *entry = line;
        int added;
        ++number;
        if (!strchr(line, '\n') && !feof(file)) { valid = 0; break; }
        if (number == 1 && (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) { entry += 3; }
        entry[strcspn(entry, "\r\n")] = 0;
        if (!*entry) { continue; }
        added = AppendPath(Config, entry, CP_UTF8);
        if (!added) { valid = 0; break; }
        if (added == 2) { ++duplicates; }
    }
    if (ferror(file)) { valid = 0; }
    fclose(file);
    if (!valid) { printf("  Import rejected near line %lu. Driver list was not changed.\n", number); return; }
    if (RuntimeWrite(Config)) {
        printf("  Imported %lu file(s); skipped %lu duplicate(s).\n", Config->PathCount - before, duplicates);
    }
}

void AppFilesMenu(void)
{
    ULONG page = 0;
    if (!OpenMonitor()) { AppPause(); return; }
    for (;;) {
        ULONG i, first, end;
        int choice;
        char line[HV_MONITOR_PATH_CHARS * 4];
        if (!RuntimeRead(&g_RuntimeView)) { AppPause(); return; }
        if (page * 10 >= g_RuntimeView.PathCount) { page = 0; }
        AppPage("PROTECTED FILES");
        printf("  %lu / %u files | Hooks: %s | Page %lu / %lu\n\n", g_RuntimeView.PathCount,
            HV_PROTECTED_MAX, g_RuntimeView.Enabled ? "ON" : "OFF", page + 1,
            g_RuntimeView.PathCount ? (g_RuntimeView.PathCount + 9) / 10 : 1);
        first = page * 10; end = min(first + 10, g_RuntimeView.PathCount);
        if (!g_RuntimeView.PathCount) { puts("  No files selected. Empty list means telemetry only."); }
        for (i = first; i < end; ++i) {
            printf("  %3lu  ", i + 1); PrintPath(g_RuntimeView.Paths[i]); putchar('\n');
        }
        puts("\n  [1] Add file          [2] Remove by number    [3] Import UTF-8 list");
        puts("  [4] Clear list        [5] Previous page       [6] Next page");
        puts("  [0] Back\n\n  Disable hooks before editing. Changes apply to the current driver session.");
        choice = AppChoice("\n  Select > ", 0, 6);
        if (choice <= 0) { return; }
        if (choice == 5) { if (page) { --page; } continue; }
        if (choice == 6) { if (end < g_RuntimeView.PathCount) { ++page; } continue; }
        if (g_RuntimeView.Enabled) { puts("  Disable hooks in Hooks and telemetry first."); AppPause(); continue; }
        if (choice == 1) {
            int added;
            puts("  Absolute file path (quotes accepted; empty cancels):");
            if (ReadLine(line, sizeof(line)) && line[0]) {
                added = AppendPath(&g_RuntimeView, line, GetConsoleCP() ? GetConsoleCP() : CP_UTF8);
                if (added == 2) { puts("  File is already listed."); }
                else if (added && RuntimeWrite(&g_RuntimeView)) { puts("  File added."); }
            }
        } else if (choice == 2 && g_RuntimeView.PathCount) {
            int number = AppChoice("  File number (0 cancels) > ", 0, (int)g_RuntimeView.PathCount);
            if (number > 0) {
                ULONG index = (ULONG)number - 1;
                if (index + 1 < g_RuntimeView.PathCount) {
                    memmove(g_RuntimeView.Paths[index], g_RuntimeView.Paths[index + 1],
                        (g_RuntimeView.PathCount - index - 1) * sizeof(g_RuntimeView.Paths[0]));
                }
                --g_RuntimeView.PathCount;
                ZeroMemory(g_RuntimeView.Paths[g_RuntimeView.PathCount], sizeof(g_RuntimeView.Paths[0]));
                if (RuntimeWrite(&g_RuntimeView)) { puts("  File removed."); }
            }
        } else if (choice == 3) { ImportPaths(&g_RuntimeView); }
        else if (choice == 4) {
            g_RuntimeView.PathCount = 0;
            ZeroMemory(g_RuntimeView.Paths, sizeof(g_RuntimeView.Paths));
            if (RuntimeWrite(&g_RuntimeView)) { puts("  Protected list cleared."); }
        }
        AppPause();
    }
}


static int LogFilterIo(HV_LOG_FILTER *Filter, int Write)
{
    DWORD bytes;
    if (!DeviceIoControl(g_Device, Write ? IOCTL_HV_LOG_FILTER_SET : IOCTL_HV_LOG_FILTER_GET,
        Filter, sizeof(*Filter), Write ? NULL : Filter, Write ? 0 : sizeof(*Filter), &bytes, NULL) ||
        (!Write && (bytes != sizeof(*Filter) || !HvFilterValid(Filter)))) {
        AppError("Log filter configuration", GetLastError()); return 0;
    }
    return 1;
}


static ULONG ProfileChecksumBytes(const CONSOLE_PROFILE *Profile, size_t Bytes)
{
    const unsigned char *data = (const unsigned char *)Profile;
    ULONG hash = 2166136261u;
    size_t i;
    for (i = 0; i < Bytes; ++i) {
        if (i >= offsetof(CONSOLE_PROFILE, Checksum) && i < offsetof(CONSOLE_PROFILE, Checksum) + sizeof(ULONG)) { continue; }
        hash = (hash ^ data[i]) * 16777619u;
    }
    return hash;
}

static ULONG ProfileChecksum(const CONSOLE_PROFILE *Profile)
{
    return ProfileChecksumBytes(Profile, sizeof(*Profile));
}

static int ProfileValid(const CONSOLE_PROFILE *Profile)
{
    ULONG i;
    if (Profile->Magic != 0x48565046u || Profile->Version != 1 || Profile->Size != sizeof(*Profile) ||
        Profile->Abi != HV_MONITOR_VERSION || Profile->Targets != HV_MONITOR_TARGETS ||
        Profile->Reserved || (Profile->SelectedHooks & ~HV_MONITOR_ALL) ||
        Profile->Checksum != ProfileChecksum(Profile)) { return 0; }
    for (i = 0; i < HV_MONITOR_TARGETS; ++i) {
        const HV_LOG_FILTER *filter = &Profile->Filters[i];
        if (!HvTargetSupported(i)) {
            HV_LOG_FILTER empty = { .Version = HV_MONITOR_VERSION };
            empty.Target = i;
            if (memcmp(filter, &empty, sizeof(empty))) { return 0; }
            continue;
        }
        if (filter->Target != i || !HvFilterValid(filter) ||
            (filter->Field && !g_DetailLabels[i][filter->Field-1])) { return 0; }
    }
    return 1;
}

// ABI v6 profiles contain 13 targets. Verify their original checksum before
// extending the arrays; never discard the user's old selections on an upgrade.
static int ProfileLoad(FILE *File, CONSOLE_PROFILE *Profile)
{
    size_t bytes;
    ULONG i, oldTargets;

    ZeroMemory(Profile, sizeof(*Profile));
    bytes = fread(Profile, 1, sizeof(*Profile), File);
    if (ferror(File) || fgetc(File) != EOF || bytes < offsetof(CONSOLE_PROFILE, Filters) ||
        Profile->Magic != 0x48565046u || Profile->Version != 1 ||
        Profile->Size != bytes || Profile->Reserved ||
        Profile->Checksum != ProfileChecksumBytes(Profile, bytes)) {
        return 0;
    }
    oldTargets = Profile->Targets;
    if (!((Profile->Abi == 6 && oldTargets == 13) ||
          (Profile->Abi == 7 && oldTargets == 20) ||
          (Profile->Abi == 8 && oldTargets == 24) ||
          (Profile->Abi == 9 && oldTargets == 25) ||
          (Profile->Abi == HV_MONITOR_VERSION && oldTargets == HV_MONITOR_TARGETS)) ||
        bytes != offsetof(CONSOLE_PROFILE, Filters) + oldTargets * sizeof(HV_LOG_FILTER) ||
        (Profile->SelectedHooks & ~((1u << oldTargets) - 1))) {
        return 0;
    }
    for (i = 0; i < oldTargets; ++i) {
        if (Profile->Filters[i].Version != Profile->Abi) { return 0; }
        Profile->Filters[i].Version = HV_MONITOR_VERSION;
    }
    for (; i < HV_MONITOR_TARGETS; ++i) {
        Profile->Filters[i].Version = HV_MONITOR_VERSION;
        Profile->Filters[i].Target = i;
    }
    if (Profile->Abi != HV_MONITOR_VERSION) {
        Profile->SelectedHooks &= HV_MONITOR_ALL;
        for (i = 0; i < HV_MONITOR_TARGETS; ++i) {
            if (HvTargetSupported(i)) { continue; }
            ZeroMemory(&Profile->Filters[i], sizeof(Profile->Filters[i]));
            Profile->Filters[i].Version = HV_MONITOR_VERSION;
            Profile->Filters[i].Target = i;
        }
    }
    Profile->Size = sizeof(*Profile);
    Profile->Abi = HV_MONITOR_VERSION;
    Profile->Targets = HV_MONITOR_TARGETS;
    Profile->Checksum = ProfileChecksum(Profile);
    return ProfileValid(Profile);
}
static int ProfilePath(void)
{
    static WCHAR legacy[32768];
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", g_ProfilePath, ARRAYSIZE(g_ProfilePath));
    if (!length || length >= ARRAYSIZE(g_ProfilePath) - 64) { g_ProfilePath[0] = 0; return 0; }
    wcscpy_s(legacy, ARRAYSIZE(legacy), g_ProfilePath);
    wcscat_s(legacy, ARRAYSIZE(legacy), L"\\MyHypervisor\\console-settings.bin");
    wcscat_s(g_ProfilePath, ARRAYSIZE(g_ProfilePath), L"\\Velum");
    if (!CreateDirectoryW(g_ProfilePath, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        g_ProfilePath[0] = 0; return 0;
    }
    wcscat_s(g_ProfilePath, ARRAYSIZE(g_ProfilePath), L"\\console-settings.bin");
    // Preserve the previous profile; never overwrite an existing Velum profile.
    if (GetFileAttributesW(g_ProfilePath) == INVALID_FILE_ATTRIBUTES &&
        GetLastError() == ERROR_FILE_NOT_FOUND &&
        !CopyFileW(legacy, g_ProfilePath, TRUE)) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND &&
            error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            AppError("Migrate previous console settings", error);
            g_ProfilePath[0] = 0; return 0;
        }
    }
    return 1;
}

static int ProfileSave(void)
{
    static WCHAR temporary[32768];
    FILE *file = NULL;
    int ok;
    if (!g_ProfilePath[0] && !ProfilePath()) { goto Failed; }
    wcscpy_s(temporary, ARRAYSIZE(temporary), g_ProfilePath);
    wcscat_s(temporary, ARRAYSIZE(temporary), L".tmp");
    g_Profile.Checksum = ProfileChecksum(&g_Profile);
    if (_wfopen_s(&file, temporary, L"wb") || !file) { goto Failed; }
    ok = fwrite(&g_Profile, 1, sizeof(g_Profile), file) == sizeof(g_Profile);
    if (ok) { ok = fflush(file) == 0 && _commit(_fileno(file)) == 0; }
    if (fclose(file) != 0) { ok = 0; }
    if (!ok || !MoveFileExW(temporary, g_ProfilePath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary); goto Failed;
    }
    return 1;
Failed:
    puts("  [WARNING] Settings are active in the driver but could not be saved to disk.");
    puts("  Check write access to %LOCALAPPDATA%\\Velum; change/save again to retry.");
    return 0;
}

static int ProfileInitialize(void)
{
    CONSOLE_PROFILE saved;
    FILE *file = NULL;
    ULONG i;
    int loaded = 0;
    ZeroMemory(&g_Profile, sizeof(g_Profile));
    g_Profile.Magic = 0x48565046u; g_Profile.Version = 1; g_Profile.Size = sizeof(g_Profile);
    g_Profile.Abi = HV_MONITOR_VERSION; g_Profile.Targets = HV_MONITOR_TARGETS;
    if (!RuntimeRead(&g_RuntimeView)) { return 0; }
    g_Profile.SelectedHooks = g_RuntimeView.HookMask;
    for (i = 0; i < HV_MONITOR_TARGETS; ++i) {
        g_Profile.Filters[i].Version = HV_MONITOR_VERSION; g_Profile.Filters[i].Target = i;
        if (!HvTargetSupported(i)) { continue; }
        if (!LogFilterIo(&g_Profile.Filters[i], 0)) { return 0; }
    }
    if (!ProfilePath()) {
        puts("  [WARNING] Settings folder unavailable; current driver settings retained."); return 1;
    }
    if (_wfopen_s(&file, g_ProfilePath, L"rb") == 0 && file) {
        loaded = ProfileLoad(file, &saved);
        fclose(file);
        if (!loaded) { puts("  [WARNING] Invalid/incompatible settings file ignored; current driver settings retained."); }
    } else if (errno != ENOENT) {
        puts("  [WARNING] Could not read settings file; current driver settings retained.");
    } else {
        (void)ProfileSave();
    }
    if (loaded) {
        for (i = 0; i < HV_MONITOR_TARGETS; ++i) {
            if (!HvTargetSupported(i)) { continue; }
            if (!LogFilterIo(&saved.Filters[i], 1)) { return 0; }
        }
        g_Profile = saved;
    }
    return 1;
}

static int ReadFilterNumber(const char *Prompt, ULONG64 *Value)
{
    char line[80], *end;
    unsigned long long number;
    fputs(Prompt, stdout);
    if (!ReadLine(line, sizeof(line)) || !line[0]) { return 0; }
    if (line[0] < '0' || line[0] > '9') { puts("  Use unsigned decimal or 0x hexadecimal."); return 0; }
    errno = 0;
    number = strtoull(line, &end, line[0] == '0' && (line[1] == 'x' || line[1] == 'X') ? 16 : 10);
    if (errno == ERANGE || *end) { puts("  Invalid number."); return 0; }
    *Value = number; return 1;
}

static int ReadFieldValue(ULONG Target, ULONG Field, const char *Prompt, ULONG64 *Value)
{
    char line[2048];

    puts("  Enter decimal, hex, or field-specific names joined with |. Enter ? to list names.");
    for (;;) {
        fputs(Prompt, stdout);
        if (!ReadLine(line, sizeof(line)) || !line[0]) { return 0; }
        if (strcmp(line, "?") == 0) {
            AppListFieldSymbols(Target, Field);
            continue;
        }
        if (AppParseFieldValue(Target, Field, line, Value)) { return 1; }
        puts("  Invalid value for this field. Use ? for names; empty input cancels.");
    }
}
static int ReadFilterPath(HV_LOG_FILTER *Filter)
{
    char line[HV_MONITOR_PATH_CHARS * 4];
    WCHAR input[HV_MONITOR_PATH_CHARS], native[HV_MONITOR_PATH_CHARS], drive[3] = {0};
    size_t length;
    UINT cp = GetConsoleCP();
    puts("  Path (C:\\... or native \\Device\\...); contains mode also accepts a name fragment:");
    if (!ReadLine(line, sizeof(line)) || !line[0] ||
        !MultiByteToWideChar(cp ? cp : CP_UTF8, MB_ERR_INVALID_CHARS, line, -1, input, ARRAYSIZE(input))) { return 0; }
    if (!AppCleanPath(input)) { puts("  Empty path or unmatched quotes."); return 0; }
    if (!input[0]) { return 0; }
    if (input[0] && input[1] == L':') {
        WCHAR full[HV_MONITOR_PATH_CHARS];
        DWORD count;
        if (input[2] != L'\\') { puts("  Drive-relative paths are not supported."); return 0; }
        count = GetFullPathNameW(input, ARRAYSIZE(full), full, NULL);
        drive[0] = input[0]; drive[1] = L':';
        if (!count || count >= ARRAYSIZE(full) || !QueryDosDeviceW(drive, native, ARRAYSIZE(native))) {
            puts("  Cannot resolve drive. Paste the native path from a captured event."); return 0;
        }
        if (wcslen(native) + wcslen(full+2) >= HV_MONITOR_PATH_CHARS) { return 0; }
        wcscat_s(native, ARRAYSIZE(native), full+2);
        wcscpy_s(input, ARRAYSIZE(input), native);
    } else if (Filter->PathMatch != 3 && input[0] != L'\\') {
        puts("  Exact/subtree matching requires a full native or drive path."); return 0;
    }
    length = wcslen(input);
    while (Filter->PathMatch == 2 && length > 1 && input[length-1] == L'\\') { input[--length] = 0; }
    wcscpy_s(Filter->Path, HV_MONITOR_PATH_CHARS, input);
    return 1;
}

static int EditFilterMenu(ULONG Target, HV_LOG_FILTER *Local)
{
    static const char *const sources[] = {"off", "file path", "rename/link destination", "section object name", "target image"};
    HV_LOG_FILTER draft = { .Version = HV_MONITOR_VERSION };
    draft.Target = Target;
    if (Local) { draft = *Local; }
    else if (!LogFilterIo(&draft, 0)) { AppPause(); return 0; }
    for (;;) {
        int choice;
        ULONG i;
        ULONG64 value;
        HV_LOG_FILTER next = draft;
        AppPage(Local ? "POLICY SCOPE" : HvTargetIsKernel(Target) ? "KERNEL ROUTINE LOG FILTER" : "PER-SYSCALL LOG FILTER");
        printf("  %s | %s\n", g_TargetNames[Target], HvFilterActive(&draft) ? "CUSTOM (draft)" : "ALL EVENTS (draft)");
        puts(Local ? "  Policy scope (user mode only); saving returns to the policy draft." :
            "  Both log routes; conditions combine with AND. Protection is independent.");
        puts(Local ? "  Save scope returns to the draft. Apply in the policy menu to activate." :
            "  Missing required metadata does not match. Save also stores settings on disk.");
        if (HvTargetIsKernel(Target)) { puts("  Kernel inputs with best-effort caller metadata; PID/TID, mode and action filters are unavailable."); }
        if (Target == HV_TARGET_DUPLICATE_OBJECT) {
            puts("  Target PID/image = recipient process; source_pid is an independent field.");
        }
        else printf("\n  Caller PID=%lu TID=%lu | Target PID=%lu (0=any)\n", draft.CallerPid, draft.CallerTid, draft.TargetPid);
        printf("  Action=%s | Mode=%s\n", draft.Action == 0 ? "any" : draft.Action == 1 ? "continue" : draft.Action == 2 ? "blocked" : "modified",
            draft.Mode == 0 ? "any" : draft.Mode == 1 ? "user" : "kernel");
        printf("  Path=%s | Match=%s", sources[draft.PathSource], draft.PathMatch == 0 ? "off" : draft.PathMatch == 1 ? "exact" : draft.PathMatch == 2 ? "subtree" : "contains");
        if (draft.PathSource) { fputs(" | ", stdout); PrintPath(draft.Path); }
        putchar('\n');
        if (draft.Field) {
            printf("  Field=%s | %s 0x%llX", g_DetailLabels[Target][draft.Field-1],
                draft.Operator == 1 ? "equals" : draft.Operator == 2 ? "has all bits" : "range from", draft.Value);
            if (draft.Operator == 3) { printf(" to 0x%llX", draft.Upper); }
            putchar('\n');
        } else { puts("  Field condition=off"); }
        if (HvTargetHasPolicy(Target)) {
            puts("\n  [1] Caller PID  [2] Caller TID  [3] Target PID");
            if (!Local) { puts("  [4] Action  [5] Mode"); }
            puts("  [6] Path filter");
        }
        puts("  [7] Field condition [8] Reset to all [9] Save [0] Discard/back");
        choice = AppChoice("  Select > ", 0, 9);
        if (choice <= 0) { return 0; }
        if ((Local && (choice == 4 || choice == 5)) || (HvTargetIsKernel(Target) && choice <= 5)) {
            puts("  This criterion is unavailable for this target/context."); AppPause(); continue;
        }
        if (choice <= 3) {
            if (choice == 3 && !(HvPathSources(Target) & (1u << 4))) { puts("  This syscall has no target process identity."); AppPause(); continue; }
            if (ReadFilterNumber("  ID (0=any, empty cancels) > ", &value) && value <= MAXDWORD) {
                if (choice == 1) { next.CallerPid = (ULONG)value; }
                else if (choice == 2) { next.CallerTid = (ULONG)value; }
                else { next.TargetPid = (ULONG)value; }
            } else { puts("  ID unchanged (requires a 32-bit unsigned value)."); AppPause(); }
        } else if (choice == 4 || choice == 5) {
            int option = AppChoice(choice == 4 ? "  [0] Any [1] Continue [2] Blocked [3] Modified > " : "  [0] Any [1] User [2] Kernel > ", 0, choice == 4 ? 3 : 2);
            if (option >= 0) { if (choice == 4) { next.Action = (ULONG)option; } else { next.Mode = (ULONG)option; } }
        } else if (choice == 6) {
            int source, match;
            puts("  [0] Disable path filter");
            for (i = 1; i <= 4; ++i) { if (HvPathSources(Target) & (1u << i)) { printf("  [%lu] %s\n", i, sources[i]); } }
            source = AppChoice("  Path source > ", 0, 4);
            if (source < 0) { continue; }
            if (source == 0) { next.PathSource = next.PathMatch = 0; ZeroMemory(next.Path, sizeof(next.Path)); }
            else {
                if (!(HvPathSources(Target) & (1u << source))) { puts("  Choose a listed source."); AppPause(); continue; }
                match = AppChoice("  [1] Exact [2] Directory subtree [3] Contains [0] Cancel > ", 0, 3);
                if (match <= 0) { continue; }
                next.PathSource = (ULONG)source; next.PathMatch = (ULONG)match;
                if (!ReadFilterPath(&next)) { AppPause(); continue; }
            }
        } else if (choice == 7) {
            int field, op;
            puts("  Captured fields (not syscall completion results). [0] Disable condition");
            for (i = 0; i < 12; ++i) {
                if (Local && Target == HV_TARGET_DUPLICATE_OBJECT && i == 11) { continue; }
                if (g_DetailLabels[Target][i]) { printf("  [%lu] %s\n", i+1, g_DetailLabels[Target][i]); }
            }
            field = AppChoice("  Field > ", 0, 12);
            if (field < 0) { continue; }
            if (!field) { next.Field = next.Operator = 0; next.Value = next.Upper = 0; }
            else {
                if (!g_DetailLabels[Target][field-1] ||
                    (Local && Target == HV_TARGET_DUPLICATE_OBJECT && field == 12)) {
                    puts("  Choose a listed input field."); AppPause(); continue;
                }
                op = AppChoice("  [1] Equals [2] Has all bits [3] Inclusive range [0] Cancel > ", 0, 3);
                if (op <= 0) { continue; }
                next.Field = (ULONG)field; next.Operator = (ULONG)op;
                if (!ReadFieldValue(Target, (ULONG)field - 1, "  Value / lower bound > ", &next.Value)) { AppPause(); continue; }
                next.Upper = 0;
                if (op == 3 && (!ReadFieldValue(Target, (ULONG)field - 1, "  Upper bound > ", &next.Upper) || next.Upper < next.Value)) { puts("  Invalid range."); AppPause(); continue; }
            }
        } else if (choice == 8) {
            ZeroMemory(&next, sizeof(next)); next.Version = HV_MONITOR_VERSION; next.Target = Target; if (Local) { next.Mode = 1; }
        } else if (choice == 9) {
            if (Local) { next.Mode = 1; next.Action = 0; *Local = next; return 1; }
            if (LogFilterIo(&draft, 1)) {
                g_Profile.Filters[Target] = draft;
                if (ProfileSave()) { puts("  Filter saved to driver and disk. Hook and protection settings are unchanged."); AppPause(); return 0; }
            }
            AppPause(); continue;
        }
        draft = next;
    }
}

static int PolicyIo(HV_POLICY_CONFIG *Policy, int Write)
{
    DWORD bytes = 0;
    if (!DeviceIoControl(g_Device, Write ? IOCTL_HV_POLICY_SET : IOCTL_HV_POLICY_GET,
        Policy, sizeof(*Policy), Write ? NULL : Policy, Write ? 0 : sizeof(*Policy), &bytes, NULL) ||
        (!Write && bytes != sizeof(*Policy))) {
        AppError("Policy configuration", GetLastError());
        return 0;
    }
    return 1;
}

static void PolicyMenu(ULONG Target)
{
    HV_POLICY_CONFIG draft = {0};

    draft.Match.Version = HV_MONITOR_VERSION;
    draft.Match.Target = Target;
    draft.Match.Mode = 1;
    if (!PolicyIo(&draft, 0)) { AppPause(); return; }

    for (;;) {
        int choice;
        AppPage("PER-SYSCALL POLICY");
        printf("  %s | %s\n", g_TargetNames[Target],
            draft.Action == HV_POLICY_OFF ? "OFF" :
            draft.Action == HV_POLICY_DENY ? "DENY" : "REMOVE ACCESS BITS");
        puts("  Draft changes take effect only with Apply. Hooks are enabled separately.");
        puts("  User-mode calls only; incomplete captures fail open.");
        puts("  Rules survive console reconnect, but reset when the driver is reloaded.");
        puts("  Policy scope is independent of the log filter.");
        printf("  Scope: caller PID=%lu target PID=%lu path=%s field=%lu\n",
            draft.Match.CallerPid, draft.Match.TargetPid,
            draft.Match.PathSource ? "set" : "off", draft.Match.Field);
        printf("  Remove access mask: 0x%llX\n", draft.RemoveAccess);
        if (Target == HV_TARGET_DUPLICATE_OBJECT) {
            puts("  Target PID/path = recipient process. Source PID is a field condition.");
            puts("  Access names are numeric aliases; source object type is not a scope condition.");
            puts("  A matching restriction DENIES SAME_ACCESS or generic/MAXIMUM_ALLOWED requests.");
            puts("  Denial also prevents CLOSE_SOURCE. Explicit masks are reduced in argument 5.");
        }
        puts("\n  [1] Edit scope  [2] Disable  [3] Deny matching requests");
        if (HvPolicyCanRemoveAccess(Target)) { puts("  [4] Remove requested access bits"); }
        puts("  [9] Apply to driver  [0] Discard/back");
        choice = AppChoice("  Select > ", 0, 9);

        if (choice <= 0) { return; }
        if (choice == 1) {
            (void)EditFilterMenu(Target, &draft.Match);
        } else if (choice == 2) {
            draft.Action = HV_POLICY_OFF;
            draft.RemoveAccess = 0;
        } else if (choice == 3) {
            draft.Action = HV_POLICY_DENY;
            draft.RemoveAccess = 0;
        } else if (choice == 4 && HvPolicyCanRemoveAccess(Target)) {
            ULONG64 mask;
            puts("  Only removes rights. Generic/MAXIMUM_ALLOWED requests are denied.");
            if (ReadFieldValue(Target, 0, "  Access bits to REMOVE > ", &mask) && mask && mask <= MAXDWORD) {
                draft.Action = HV_POLICY_REMOVE_ACCESS;
                draft.RemoveAccess = mask;
            }
        } else if (choice == 9) {
            if (!HvPolicyValid(&draft)) {
                puts("  Set a caller PID, target PID, path or field condition; check the action/mask.");
            } else if (PolicyIo(&draft, 1)) {
                puts("  Policy applied. Log delivery and hook switches are unchanged.");
                AppPause();
                return;
            }
        }
    }
}
static void KernelStatsMenu(void)
{
    HV_KERNEL_STATS *stats = calloc(1, sizeof(*stats));
    ULONG version = HV_MONITOR_VERSION, i;
    DWORD bytes;
    int choice;
    if (!stats) { puts("Cannot allocate summary buffer."); return; }
    for (;;) {
        AppPage("KERNEL TELEMETRY SUMMARY");
        if (!DeviceIoControl(g_Device, IOCTL_HV_KERNEL_STATS, &version, sizeof(version),
            stats, sizeof(*stats), &bytes, NULL) || bytes != sizeof(*stats) ||
            stats->Version != version || stats->Count > HV_KERNEL_STATS_CAPACITY) {
            puts("Cannot read kernel summary. Check driver/console versions."); AppPause(); break;
        }
        puts("Observed calls matching log filters. Enable a hook and log route to collect.");
        puts("Input byte totals are NOT live allocations, successful allocations or leaks.");
        printf("Capture queue drops: %llu | Summary table overflow events: %llu\n\n",
            stats->CaptureDropped, stats->TableOverflow);
        for (i = 0; i < stats->Count; ++i) {
            const HV_KERNEL_STAT *row = &stats->Entries[i];
            char tag[5];
            if (!HvTargetSupported(row->Target)) { continue; }
            printf("%s | ", g_TargetNames[row->Target]);
            if (row->ModuleBase) { printf("%.*ls @0x%llX", 63, row->Module, row->ModuleBase); }
            else { printf("unresolved caller=0x%llX", row->UnknownCaller); }
            if (row->HasTag) {
                HvKernelTagText(row->Tag, tag);
                printf(" | tag='%s' (0x%08lX)", tag, row->Tag);
            }
            printf(" | calls=%llu", row->Calls);
            if (HvKernelSizeField(row->Target) >= 0) {
                printf(" | input_bytes=%llu (%.2f MiB)", row->RequestedBytes,
                    (double)row->RequestedBytes / (1024.0 * 1024.0));
                if (row->MissingSizeCalls) { printf(" | size_unknown_calls=%llu", row->MissingSizeCalls); }
            }
            putchar('\n');
        }
        if (!stats->Count) { puts("No matching kernel events have been drained yet."); }
        choice = AppChoice("\n[1] Refresh  [2] Reset counters  [0] Back > ", 0, 2);
        if (choice <= 0) { break; }
        if (choice == 2 && !DeviceIoControl(g_Device, IOCTL_HV_KERNEL_STATS_RESET,
            &version, sizeof(version), NULL, 0, &bytes, NULL)) {
            puts("Cannot reset counters."); AppPause();
        }
    }
    free(stats);
}

void AppMonitorMenu(void)
{
    ULONG i, bits, count, targets[HV_MONITOR_TARGETS];
    if (!OpenMonitor()) { AppPause(); return; }
    for (;;) {
        int selection, action, route;
        if (!RuntimeRead(&g_RuntimeView)) { AppPause(); return; }
        AppPage("HOOKS AND TELEMETRY");
        printf("  Hook mask: 0x%03lX | Files: %lu / %u\n\n", g_RuntimeView.HookMask, g_RuntimeView.PathCount, HV_PROTECTED_MAX);
        count = 0;
        puts("       TARGET                       HOOK     DEBUGVIEW  CONSOLE    FILTER   REMEMBERED POLICY");
        for (i = 0; i < HV_MONITOR_TARGETS; ++i) {
            HV_LOG_FILTER filter = { .Version = HV_MONITOR_VERSION };
            HV_POLICY_CONFIG policy = {0};
            if (!(g_RuntimeView.AvailableMask & (1u << i))) { continue; }
            targets[count++] = i; filter.Target = i;
            if (!LogFilterIo(&filter, 0)) { AppPause(); return; }
            policy.Match.Version = HV_MONITOR_VERSION; policy.Match.Target = i;
            if (HvTargetHasPolicy(i) && !PolicyIo(&policy, 0)) { AppPause(); return; }
            printf("  [%2lu] %-28s %-8s %-10s %-10s %-8s %-10s %s\n", count, g_TargetNames[i],
                g_RuntimeView.HookMask & (1u << i) ? "ON" : "OFF",
                g_Settings.DebugMask & (1u << i) ? "ON" : "OFF",
                g_Settings.ConsoleMask & (1u << i) ? "ARMED" : "OFF", HvFilterActive(&filter) ? "CUSTOM" : "ALL",
                g_Profile.SelectedHooks & (1u << i) ? "YES" : "-",
                HvTargetIsKernel(i) ? "OBSERVE" : policy.Action == 1 ? "DENY" : policy.Action == 2 ? "RESTRICT" : "OFF");
        }
        printf("\n  [%lu] All targets   [%lu] Live monitor   [%lu] Protected files   [0] Back\n", count+1, count+2, count+3);
        printf("  [%lu] Enable remembered hooks (logs stay as currently configured)\n", count+4);
        printf("  [%lu] Kernel telemetry summary\n", count+5);
        puts("  Exported targets sharing an address are listed once. ALL in FILTER means no log restriction.");
        puts("  Disabling a file hook reduces coverage. Mask changes briefly rebuild the batch.");
        selection = AppChoice("\n  Select > ", 0, (int)count+5);
        if (selection <= 0) { return; }
        if (selection == (int)count+5) { KernelStatsMenu(); continue; }
        if (selection == (int)count+4) {
            bits = g_Profile.SelectedHooks & g_RuntimeView.AvailableMask;
            if (!bits) { puts("  No remembered hooks are available on this Windows build."); }
            else {
                g_RuntimeView.HookMask |= bits;
                if (RuntimeWrite(&g_RuntimeView)) { puts("  Remembered hooks enabled. Existing hooks retained."); }
            }
            AppPause(); continue;
        }
        if (selection == (int)count+3) { AppFilesMenu(); continue; }
        if (selection == (int)count+2) {
            if (!g_RuntimeView.HookMask) { puts("  Enable at least one hook first."); }
            else { LiveView(); }
            AppPause(); continue;
        }
        bits = selection == (int)count+1 ? g_RuntimeView.AvailableMask : 1u << targets[selection-1];
        action = AppChoice("  [1] Enable hook  [2] Disable hook  [3] Log route  [4] Log filter  [5] Policy  [0] Cancel > ", 0, 5);
        if (action <= 0) { continue; }
        if (action == 5) {
            if (selection == (int)count+1 || !HvTargetHasPolicy(targets[selection-1])) { puts("  Policies apply to one Nt syscall; pool routines are observation-only."); AppPause(); }
            else { PolicyMenu(targets[selection-1]); }
            continue;
        }
        if (action == 4) {
            if (selection == (int)count+1) { puts("  Select one syscall to configure its independent filter."); AppPause(); }
            else { (void)EditFilterMenu(targets[selection-1], NULL); }
            continue;
        }
        if (action == 1 || action == 2) {
            if (action == 1 && !(g_RuntimeView.AvailableMask & bits)) {
                puts("  This target has no separate exported entry on this Windows build.");
            } else {
                if (action == 1) { g_RuntimeView.HookMask |= bits & g_RuntimeView.AvailableMask; }
                else { g_RuntimeView.HookMask &= ~bits; }
                if (RuntimeWrite(&g_RuntimeView)) {
                    if (action == 1) { g_Profile.SelectedHooks |= bits & g_RuntimeView.AvailableMask; }
                    else { g_Profile.SelectedHooks &= ~bits; }
                    if (ProfileSave()) { puts("  Hook selection updated and remembered on disk. Log routes are independent."); }
                }
            }
        } else {
            route = AppChoice("  Route [1 DebugView / 2 Console / 3 Both / 0 Off] > ", 0, 3);
            if (route < 0) { return; }
            g_Settings.DebugMask = (g_Settings.DebugMask & ~bits) | (route & 1 ? bits : 0);
            g_Settings.ConsoleMask = (g_Settings.ConsoleMask & ~bits) | (route & 2 ? bits : 0);
            if (!Configure(0)) { AppMonitorClose(); AppPause(); return; }
            puts("  Routing updated. Console capture runs only inside live monitor.");
        }
        AppPause();
    }
}
