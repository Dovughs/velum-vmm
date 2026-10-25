// Shared policy capability and validation; include telemetry_match.h first.
#pragma once

#define HV_POLICY_OFF 0u
#define HV_POLICY_DENY 1u
#define HV_POLICY_REMOVE_ACCESS 2u

// Duplicate options are never rewritten. SAME_ACCESS ignores DesiredAccess:
// conservatively deny a matching restriction instead of racing a handle-table
// snapshot. CLOSE_SOURCE is not executed when the call is denied.
#define HV_DUP_REASON_NONE 0u
#define HV_DUP_REASON_SAME_ACCESS 1u
#define HV_DUP_REASON_GENERIC_ACCESS 2u
#define HV_DUP_REASON_STACK_WRITE 3u
#define HV_DUP_REASON_UNKNOWN_OPTIONS 4u
#define HV_DUP_REASON_DENY_RULE 5u
static __inline ULONG HvPolicyRestrictDuplicate(ULONG Requested, ULONG Options,
    ULONG Remove, ULONG *Effective, ULONG *Reason)
{
    *Effective = Requested;
    *Reason = HV_DUP_REASON_NONE;
    if (Options & ~7u) { *Reason = HV_DUP_REASON_UNKNOWN_OPTIONS; return 1; }
    if (Options & 2u) { *Reason = HV_DUP_REASON_SAME_ACCESS; return 1; }
    if (Requested & 0xF2000000u) { *Reason = HV_DUP_REASON_GENERIC_ACCESS; return 1; }
    *Effective = Requested & ~Remove;
    return *Effective == Requested ? 0u : 2u;
}

// Return the event action (continue/deny/modified). Never add a requested right.
static __inline ULONG HvPolicyRestrictAccess(ULONG Requested, ULONG Remove, ULONG *Effective)
{
    *Effective = Requested;
    if (Requested & 0xF2000000u) { return 1; } // Generic or MAXIMUM_ALLOWED.
    *Effective = Requested & ~Remove;
    return *Effective == Requested ? 0u : 2u;
}

static __inline int HvPolicyCanRemoveAccess(ULONG Target)
{
    return Target == 0 || Target == 2 || Target == 6 || Target == 8 ||
        Target == HV_TARGET_DUPLICATE_OBJECT;
}

static __inline int HvPolicyValid(const HV_POLICY_CONFIG *Policy)
{
    const HV_LOG_FILTER *match = &Policy->Match;

    if (!HvFilterValid(match) || Policy->Reserved || Policy->Action > HV_POLICY_REMOVE_ACCESS ||
        !HvTargetHasPolicy(match->Target) || match->Action || match->Mode != 1) {
        return 0;
    }
    if (Policy->Action == HV_POLICY_OFF) { return 1; }
    // policy_reason is an output, not an input criterion for its own decision.
    if (match->Target == HV_TARGET_DUPLICATE_OBJECT && match->Field == 12) { return 0; }

    // Require a concrete scope; a bare user-mode catch-all could prevent the
    // management console itself from opening files or talking to the driver.
    if (!match->CallerPid && !match->TargetPid && !match->PathSource && !match->Field) { return 0; }
    if (Policy->Action == HV_POLICY_REMOVE_ACCESS &&
        (!HvPolicyCanRemoveAccess(match->Target) || !Policy->RemoveAccess ||
         Policy->RemoveAccess > 0xFFFFFFFFull)) {
        return 0;
    }
    return 1;
}

