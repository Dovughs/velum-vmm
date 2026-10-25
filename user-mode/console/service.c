#include "app.h"

int AppRemoveService(void)
{
    SC_HANDLE manager, service;
    SERVICE_STATUS_PROCESS state;
    SERVICE_STATUS response;
    DWORD bytes, error;
    ULONGLONG deadline = GetTickCount64() + 15000;
    int result = 1;
    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!manager) { AppError("Open service manager", GetLastError()); return 1; }
    service = OpenServiceW(manager, L"Velum", SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE);
    if (!service) {
        error = GetLastError();
        CloseServiceHandle(manager);
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) { puts("[OK] Velum is already removed."); return 0; }
        AppError("Open Velum", error); return 1;
    }
    for (;;) {
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&state, sizeof(state), &bytes)) {
            AppError("Query Velum", GetLastError()); goto Done;
        }
        if (state.dwServiceType != SERVICE_KERNEL_DRIVER) {
            puts("[ERROR] Velum is not a kernel-driver service; refusing to modify it."); goto Done;
        }
        if (state.dwCurrentState == SERVICE_STOPPED) { break; }
        if (GetTickCount64() >= deadline) { puts("[ERROR] Driver stop timed out; service was not deleted."); goto Done; }
        if (state.dwCurrentState != SERVICE_STOP_PENDING &&
            !ControlService(service, SERVICE_CONTROL_STOP, &response)) {
            error = GetLastError();
            // The worker enables DriverUnload shortly after all CPUs leave VMX.
            if (error != ERROR_INVALID_SERVICE_CONTROL && error != ERROR_SERVICE_CANNOT_ACCEPT_CTRL &&
                error != ERROR_SERVICE_NOT_ACTIVE) {
                AppError("Stop Velum", error); goto Done;
            }
        }
        Sleep(100);
    }
    puts("[OK] Velum stopped.");
    if (!DeleteService(service)) {
        error = GetLastError();
        if (error != ERROR_SERVICE_MARKED_FOR_DELETE) { AppError("Delete Velum", error); goto Done; }
    }
    puts("[OK] Velum marked for deletion. Removal completes after remaining service handles close.");
    result = 0;
Done:
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return result;
}
