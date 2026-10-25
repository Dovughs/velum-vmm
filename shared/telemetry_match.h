// Pure bounded matching, shared with regression tests. Caller supplies HV_UPCASE.
#pragma once
#include "monitor_protocol.h"
static __inline ULONG HvPathSources(ULONG Target)
{
    switch (Target) {
    case 0: case 2: case 3: case 5: case 12: return 1u << 1;
    case 4: return (1u << 1) | (1u << 2);
    case 6: return (1u << 1) | (1u << 3);
    case 7: return (1u << 3) | (1u << 4);
    case 1: case 8: case 10: case HV_TARGET_DUPLICATE_OBJECT: return 1u << 4;
    default: return 0;
    }
}
static __inline int HvFilterActive(const HV_LOG_FILTER *F)
{
    return F->CallerPid || F->CallerTid || F->TargetPid || F->Action || F->Mode || F->PathSource || F->Field;
}
static __inline int HvFilterValid(const HV_LOG_FILTER *F)
{
    ULONG i;
    if (F->Version != HV_MONITOR_VERSION || !HvTargetSupported(F->Target) || F->Reserved ||
        F->Action > 3 || F->Mode > 2 || F->PathSource > 4 || F->PathMatch > 3 ||
        F->Field > 12 || F->Operator > 3 || (!F->Field != !F->Operator) ||
        (!F->PathSource != !F->PathMatch) || (F->Operator == 3 && F->Value > F->Upper)) { return 0; }
    if (F->TargetPid && !(HvPathSources(F->Target) & (1u << 4))) { return 0; }
    if (HvTargetIsKernel(F->Target) && (F->CallerPid || F->CallerTid || F->TargetPid || F->Action || F->Mode)) { return 0; }
    if (F->PathSource && (!(HvPathSources(F->Target) & (1u << F->PathSource)) || !F->Path[0])) { return 0; }
    for (i = 0; i < HV_MONITOR_PATH_CHARS; ++i) { if (!F->Path[i]) { return 1; } }
    return 0;
}
static __inline int HvPathMatch(const WCHAR *Path, const HV_LOG_FILTER *F)
{
    ULONG n = 0, m = 0, start, i;
    while (n < HV_MONITOR_PATH_CHARS && Path[n]) { ++n; }
    while (m < HV_MONITOR_PATH_CHARS && F->Path[m]) { ++m; }
    if (!n || !m || n == HV_MONITOR_PATH_CHARS || m == HV_MONITOR_PATH_CHARS || m > n) { return 0; }
    for (start = 0; start <= n - m; ++start) {
        for (i = 0; i < m; ++i) { if (HV_UPCASE(Path[start+i]) != HV_UPCASE(F->Path[i])) { break; } }
        if (i == m && (F->PathMatch == 3 ||
            (F->PathMatch == 1 && n == m) ||
            (F->PathMatch == 2 && (n == m || F->Path[m-1] == L'\\' || Path[m] == L'\\')))) { return 1; }
        if (F->PathMatch != 3) { break; }
    }
    return 0;
}
static __inline int HvEventMatches(const HV_LOG_FILTER *F, const HV_LOG_VIEW *E)
{
    const WCHAR *path = NULL;
    ULONG64 value;
    if (!HvFilterActive(F)) { return 1; }
    // Raw fallback entries do not carry identities or decoded fields.
    if ((E->Kind != 2 && E->Kind != 3) || (F->CallerPid && F->CallerPid != E->ProcessId) ||
        (F->CallerTid && F->CallerTid != E->ThreadId) ||
        (F->TargetPid && F->TargetPid != E->TargetProcessId) ||
        (F->Action && F->Action != E->Action + 1) ||
        (F->Mode && F->Mode != (E->PreviousMode ? 1u : 2u))) { return 0; }
    if (F->PathSource) {
        switch (F->PathSource) {
        case 1: if (E->PathKind == 1) { path = E->Resolved[0] ? E->Resolved : E->Raw; } break;
        case 2: path = E->Destination; break;
        case 3: path = E->Target == 6 ? E->Destination : E->Raw; break;
        case 4: if (E->PathKind >= 2) { path = E->Resolved; } break;
        default: break;
        }
        if (!path || !HvPathMatch(path, F)) { return 0; }
    }
    if (F->Field) {
        if (!(E->FieldMask & (1u << (F->Field - 1)))) { return 0; }
        value = E->Fields[F->Field - 1];
        if ((F->Operator == 1 && value != F->Value) ||
            (F->Operator == 2 && (value & F->Value) != F->Value) ||
            (F->Operator == 3 && (value < F->Value || value > F->Upper))) { return 0; }
    }
    return 1;
}
