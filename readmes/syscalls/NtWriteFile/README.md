# NtWriteFile

[Documentation index](../../README.md) | [Project](../../../README.md)

Observe requested writes to an existing file handle.

## Capability contract

| Capability | Support |
| --- | --- |
| Stable protocol target ID | 5 (not the Windows syscall number or console row) |
| Hook / input observation | If the export resolves and the entry is supported |
| Numeric log condition | One of the fields below |
| Path sources | file path |
| Caller PID/TID, mode/action log criteria | Yes, when metadata is captured |
| Target PID criterion | No |
| Blocking policy | Yes: scoped deny |
| Argument modification | No |

## Enable and observe

1. Complete the [VM setup](../../getting-started/README.md) and load the matching pair.
2. Dashboard **[2] Hooks and telemetry -> NtWriteFile -> [1] Enable hook**.
   Select by name: row numbers change with export availability and aliases.
3. Select the target again -> **[3] Log route -> [2] Console**.
4. Select **Live monitor**, then run the controlled workload below elsewhere in the guest.
5. Verify the target name and captured fields before adding a filter or policy.
   Press Q to leave live view; the hook remains installed.

If the row is missing, the export may not resolve or may share another target's
address. If enabling fails, capture the named prepare failure rather than
assuming the menu or a file path is invalid. No SSDT/signature fallback is used.

## Captured numeric fields

These are input metadata. Field numbers match the filter menu, including gaps.
Pointers and handles are numeric values; requested sizes are bytes. Not every
field is available on every event. Use the event's partial-capture information.

| Field number | Console label | Meaning / interpretation |
| --- | --- | --- |
| 1 | `file_handle` | Input file handle; not the file name or a returned handle. |
| 2 | `bytes_requested` | Requested byte count; not completed, allocated, copied or freed bytes. |
| 3 | `byte_offset` | Captured input offset; special signed sentinel values may appear numerically. |
| 4 | `implicit_offset` | Whether the offset pointer was absent; does not supply the actual file position. |

## Filter example

Select **[4] Log filter -> [7] Field condition -> bytes_requested -> Inclusive range**,
then enter **1..4096**. For a range enter the lower and upper bound separately.
Angle-bracket values mean substitute a value from your own baseline; do not
paste them literally. Save with **[9]**. Add the test process Caller PID to isolate the workload.
Supported path sources: file path. Configure one under [6] Path filter.

All conditions combine with AND. Use `?` to list symbols for the selected field;
numeric input is accepted. A missing field does not match. Reset with **[8] -> [9]**.
See [filter semantics](../../features/filters/README.md).

## Controlled workload and expected evidence

Write a short line to a disposable file from a dedicated terminal. For an isolated write-denial test, obtain the handle before enabling only this hook, then check the write result and unchanged contents.

The event proves an observed input at this entry point. It does not establish
completion, returned handles/pointers or transferred bytes. Pair it with the
test application's result. Do not use incidental desktop traffic as proof of a
specific API-to-entry mapping.

## Blocking and modification
Select **NtWriteFile -> [5] Policy -> [1] Edit scope**. Set a test caller PID
and, where applicable, one of the path/field criteria above. **[9] Save** returns
to the policy draft; choose **[3] Deny matching requests -> [9] Apply**.
Enable the hook separately. Expected decision: BLOCKED and STATUS_ACCESS_DENIED
for eligible matching calls. Confirm the failure in the application. Disable
with **[2] Disable -> [9] Apply**, then verify the same workload succeeds again.

Policies apply to captured user-mode requests at PASSIVE_LEVEL. Kernel-mode calls,
raw fallback and incomplete captures are not enforced. Protected-path denials
take precedence. A log filter can hide a policy event without disabling policy.

Argument changes are not supported for this target; deny is the only enforcement action.

## Target-specific limits

bytes_requested is the requested length, not bytes written. No buffer contents are captured or changed. implicit_offset distinguishes a missing byte-offset pointer.

## End the experiment

Disable the policy and Apply first. Set log route Off and disable this hook.
Remove any protected test paths separately with hooks OFF. A console exit alone
does not remove hooks. See [shutdown](../../features/diagnostics/README.md).

## Source of truth

- [Export metadata](../../../hypervisor/src/syscall.c)
- [Field labels](../../../shared/telemetry_fields.h)
- [Filter capability rules](../../../shared/telemetry_match.h)
- [Guest capture and decisions](../../../hypervisor/src/filter.c)
- [Policy validation](../../../shared/telemetry_policy.h)

## Recorded example

These captures are from the physical Windows 11 session, not a VMware guest.
See the [session manifest](../../../assets/capture-manifest.md) for environment and reproduction details.

### NtWriteFile entry telemetry

A six-byte request from the scoped caller is observed. CONTINUE describes the policy decision, not the completed write result.

![NtWriteFile entry telemetry](../../../assets/screenshots/02-hook-telemetry.png)

### Scoped deny: OFF / DENY / OFF

The same caller and file handle succeed, fail with Win32 error 5 while contents remain unchanged, then succeed after disabling the policy. A chronological screenshot sequence with reading pauses, not continuous video.

![Scoped deny: OFF / DENY / OFF](../../../assets/demos/03-scoped-deny.gif)
