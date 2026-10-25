#pragma once
#include <ntddk.h>
#include "monitor_protocol.h"
NTSTATUS MonitorInitialize(PDRIVER_OBJECT DriverObject);
VOID MonitorDestroy(VOID); // Worker stopped before calling.
BOOLEAN MonitorDebugEnabled(ULONG Target);
BOOLEAN MonitorConsoleEnabled(ULONG Target);
VOID MonitorPublish(HV_MONITOR_EVENT *Event); // PASSIVE drain worker only.
BOOLEAN MonitorMatches(const HV_MONITOR_EVENT *Event); // PASSIVE drain, both routes.
BOOLEAN MonitorMatchesView(const HV_LOG_VIEW *Event); // Guest PASSIVE or drain; never VMX root.

ULONG MonitorPolicyAction(const HV_LOG_VIEW *Event, ULONG64 *RemoveAccess);
BOOLEAN MonitorPolicyEnabled(ULONG Target);
