#include "app.h"

volatile LONG g_Interrupted;
static char g_OutputBuffer[64 * 1024];
const char *const g_TargetNames[HV_MONITOR_TARGETS] = {
    "NtCreateFile",
    "NtAllocateVirtualMemory",
    "NtOpenFile",
    "NtDeleteFile",
    "NtSetInformationFile",
    "NtWriteFile",
    "NtCreateSection",
    "NtMapViewOfSection",
    "NtOpenProcess",
    NULL, // retired ID 9
    "NtProtectVirtualMemory",
    NULL, // retired ID 11
    "NtReadFile",
    "ExAllocatePool2",
    "ExAllocatePoolWithTag",
    "ExFreePoolWithTag",
    "ExAllocatePoolWithTagPriority",
    "ExAllocatePool3",
    "ExAllocatePool",
    "ExFreePool",
    "MmAllocateContiguousMemory",
    "MmFreeContiguousMemory",
    "MmMapIoSpaceEx",
    "MmUnmapIoSpace",
    "NtDuplicateObject",
    "MmCopyVirtualMemory",
};

static BOOL WINAPI ConsoleControl(DWORD Event)
{
    if (Event == CTRL_C_EVENT || Event == CTRL_BREAK_EVENT) {
        InterlockedExchange(&g_Interrupted, 1);
        return TRUE;
    }
    return FALSE; // Teardown closes the device: log routes stop, installed hooks remain.
}

void AppError(const char *Operation, DWORD Error)
{
    char message[512] = { 0 };
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, Error, 0, message, sizeof(message), NULL);
    fprintf(stderr, "[ERROR] %s (%lu): %s\n", Operation, Error, message);
}

void AppClearScreen(void)
{
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    COORD origin = { 0, 0 };
    DWORD cells, written;
    fflush(stdout);
    if (!GetConsoleScreenBufferInfo(output, &info)) {
        puts("[INFO] Clear screen requires a console output handle."); return;
    }
    cells = (DWORD)info.dwSize.X * (DWORD)info.dwSize.Y;
    if (!FillConsoleOutputCharacterW(output, L' ', cells, origin, &written) ||
        !FillConsoleOutputAttribute(output, info.wAttributes, cells, origin, &written) ||
        !SetConsoleCursorPosition(output, origin)) {
        AppError("Clear screen", GetLastError());
    }
}

int AppChoice(const char *Prompt, int Minimum, int Maximum)
{
    char line[64], *end;
    long value;
    for (;;) {
        printf("%s", Prompt);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) { return -1; }
        if (strchr(line, '\n') == NULL && !feof(stdin)) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) { }
            puts("Please enter a menu number.");
            continue;
        }
        value = strtol(line, &end, 10);
        if (end == line) { puts("Please enter a menu number."); continue; }
        while (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t') { ++end; }
        if (end != line && *end == 0 && value >= Minimum && value <= Maximum) {
            return (int)value;
        }
        puts("Invalid selection. Please try again.");
    }
}

void AppHeading(const char *Text)
{
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    BOOL color = GetConsoleScreenBufferInfo(output, &info);
    fflush(stdout);
    if (color) { SetConsoleTextAttribute(output, FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY); }
    printf("  %s\n", Text);
    fflush(stdout); // Flush while the intended color is active.
    if (color) { SetConsoleTextAttribute(output, info.wAttributes); }
}

void AppPause(void)
{
    char line[32];
    puts("\n  Press Enter to continue...");
    fflush(stdout);
    if (fgets(line, sizeof(line), stdin) && !strchr(line, '\n')) {
        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF) { }
    }
}

void AppPage(const char *Title)
{
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    BOOL color = GetConsoleScreenBufferInfo(output, &info);
    if (color) { AppClearScreen(); }
    fflush(stdout);
    if (color) {
        SetConsoleTextAttribute(output,
            (info.wAttributes & 0xFFF0) | FOREGROUND_GREEN | FOREGROUND_BLUE);
    }
    puts("\n"
         "  __     __ _____  _      _   _  __  __\n"
         "  \\ \\   / /| ____|| |    | | | ||  \\/  |\n"
         "   \\ \\ / / |  _|  | |    | | | || |\\/| |\n"
         "    \\ V /  | |___ | |___ | |_| || |  | |\n"
         "     \\_/   |_____||_____| \\___/ |_|  |_|\n");
    fflush(stdout);
    if (color) { SetConsoleTextAttribute(output, info.wAttributes); }
    puts("  Intel VT-x Research Hypervisor  |  Windows x64");
    puts("  --------------------------------------------------------\n");
    AppHeading(Title);
    putchar('\n');
}

int main(int argc, char **argv)
{
    int selection, result = 0;
    (void)setvbuf(stdout, g_OutputBuffer, _IOFBF, sizeof(g_OutputBuffer));
    SetConsoleTitleW(L"Velum | Control Console");
    SetConsoleCtrlHandler(ConsoleControl, TRUE);
    if (argc == 2) {
        if (strcmp(argv[1], "status") == 0) { return AppVerify(); }
        if (strcmp(argv[1], "stop") == 0) { return AppShutdown(); }
        if (strcmp(argv[1], "remove") == 0) {
            result = AppShutdown();
            return result ? result : AppRemoveService();
        }
        if (strcmp(argv[1], "logs") == 0) { AppMonitorMenu(); AppMonitorClose(); return 0; }
    }
    if (argc != 1) {
        puts("Usage: velum-console.exe [status | stop | remove | logs]");
        return 2;
    }
    for (;;) {
        AppPage("DASHBOARD");
        AppMonitorSummary();
        puts("\n  [1] Protected files");
        puts("  [2] Hooks and telemetry");
        puts("  [3] Diagnostics");
        puts("  [4] CPUID");
        puts("  [5] TSC offset");
        puts("  [6] Shutdown");
        puts("  [0] Exit console\n");
        selection = AppChoice("\n  Select > ", 0, 6);
        if (selection <= 0) { break; }
        if (selection == 1) { AppFilesMenu(); }
        else if (selection == 2) { AppMonitorMenu(); }
        else if (selection == 3) { AppPage("DIAGNOSTICS"); (void)AppVerify(); AppPause(); }
        else if (selection == 4) { AppCpuidMenu(); }
        else if (selection == 5) { AppTscMenu(); }
        else {
            AppMonitorClose();
            result = AppShutdown();
            if (!result) { result = AppRemoveService(); }
            AppPause();
        }
    }
    AppMonitorClose();
    puts("Console closed.");
    return result;
}
