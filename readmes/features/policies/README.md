# Scoped deny and access-reduction policies

[Documentation index](../../README.md) | [Project](../../../README.md)

Use policies to block matching requests or reduce explicit access rights before
the original routine runs. Caller, path and field criteria let you isolate a
workload and compare its behavior with the rule enabled and disabled.

Select one Nt target, then `[5] Policy`. Use `[1] Edit scope` to configure the
matching PID/path/input-field criteria. Choose deny or, on a supported access
argument, remove-access-bits. `[9] Apply` changes the driver policy; hooks and log
routes remain separate. A policy starts OFF, survives console reconnection, and
resets on driver reload. Policies are not restored from the console profile.

| Target family | Deny | Remove requested access bits |
| --- | --- | --- |
| NtCreateFile, NtOpenFile, NtCreateSection, NtOpenProcess | Yes | Yes, DesiredAccess (RDX) |
| NtDuplicateObject | Yes | Yes, explicit DesiredAccess (argument 5); SAME_ACCESS restriction is denied |
| NtReadFile, NtWriteFile, NtDeleteFile, NtSetInformationFile | Yes | No |
| NtAllocateVirtualMemory, NtMapViewOfSection, NtProtectVirtualMemory | Yes | No |
| Ex/Mm kernel routines | No | No |

Only resolved and successfully hooked targets participate. Rules require a caller
PID, target PID, path or field condition and apply only to captured user-mode
requests at PASSIVE_LEVEL. Kernel-mode calls, raw fallbacks and incomplete captures
are not enforced. Existing protected-file denials take precedence. A deny returns
STATUS_ACCESS_DENIED; access reduction changes the saved RDX argument (or
NtDuplicateObject's fifth argument on the guest kernel stack) before the
original operation resumes. Matching generic/MAXIMUM_ALLOWED access requests are
denied because clearing specific bits would not reliably constrain them. Rules
never add access, write through user pointers, or modify sizes and memory protection.

Modified-event fields describe the original input; effective_access describes the
changed argument. Neither implies that the resumed syscall succeeded. Log filters
are independent and can hide policy events without disabling enforcement.

ABI v10 requires the matching driver and console. Valid ABI v6/v7/v8/v9 filter/hook
profiles are migrated in memory; subsequent saves use the new format. Retired
target selections and filters are cleared; the new MmCopyVirtualMemory hook
starts unselected. Use the old matching console to stop an old driver before
upgrading.



## Controlled deny walkthrough

1. Create a disposable file, then note the PID of a separate test terminal.
2. Keep the protected-file list empty so it cannot mask the policy result.
3. Enable `NtWriteFile` and its Console route.
4. Select **Policy -> [1] Edit scope**. Set Caller PID to the test terminal and
   file path to the disposable file. **[9] Save** returns to the policy draft.
5. Select **[3] Deny matching requests -> [9] Apply to driver**.
6. Open live monitor and attempt a write from the test terminal. Show both the
   BLOCKED event and the application's failed operation. Verify file contents
   did not change. If an application silently retries, inspect its actual result.
7. Select **Policy -> [2] Disable -> [9] Apply**, repeat the same operation and
   verify it now succeeds. This OFF/ON/OFF comparison separates policy from an
   unrelated filesystem permission failure.

## Access-reduction walkthrough

Use a small test application making an explicit access request to a disposable
object. On a supported target choose **Policy -> [4] Remove requested access bits**
and enter the mask, then Apply. The result is `requested & ~remove`; it never
adds rights. If no requested bit changes, the event may remain CONTINUE.
Do not use broad generic rights for the reduction demonstration: matching
GENERIC_* or MAXIMUM_ALLOWED requests are denied conservatively.

There is no general argument editor. Buffer contents, addresses, lengths,
allocation sizes and memory protections are not writable through this policy UI.

## Scope validation and cleanup

An active rule must constrain caller PID, target PID, path or an input field.
Caller TID alone is insufficient. Policies always scope user-mode calls;
mode/action are not independently editable policy criteria. `policy_reason`
on NtDuplicateObject is an output and cannot select the decision itself.

Disable and Apply before changing test processes or reusing PIDs. Remove any
protected paths independently. Disable the hook when the experiment ends.

Implementation: [policy validation](../../../shared/telemetry_policy.h),
[guest capture and policy application](../../../hypervisor/src/filter.c).

## Recorded example

These captures show the physical Windows 11 test session.
See the [session manifest](../../../assets/capture-manifest.md) for environment and reproduction details.

### Scoped deny: OFF / DENY / OFF

The same caller and file handle succeed, fail with Win32 error 5 while contents remain unchanged, then succeed after disabling the policy. A chronological screenshot sequence with reading pauses.

![Scoped deny: OFF / DENY / OFF](../../../assets/demos/03-scoped-deny.gif)

### Applied access policy

Caller/path scope and remove mask 0x6 for NtCreateFile.

![Applied access policy](../../../assets/screenshots/04a-access-policy.png)

### Access reduction and independent result

Requested access 0x12019F becomes 0x120199 after removing mask 0x6. The open succeeds, but a subsequent write is denied. This labeled composite pairs real monitor and probe captures.

![Access reduction and independent result](../../../assets/screenshots/04-access-reduction.png)

### Independent access probe

The original probe window compares a successful baseline write with a reduced-rights handle that cannot write.

![Independent access probe](../../../assets/screenshots/04b-access-independent-result.png)
