# NtOpenFile

[Documentation index](../../README.md) | [Project](../../../README.md)

Open an existing file or directory.

## Capabilities

| Capability | Support |
| --- | --- |
| Stable protocol target ID | 2 (not the Windows syscall number or console row) |
| Hook / input observation | If the export resolves and the entry is supported |
| Numeric log condition | One of the fields below |
| Path sources | file path |
| Caller PID/TID, mode/action log criteria | Yes, when metadata is captured |
| Target PID criterion | No |
| Blocking policy | Yes: scoped deny |
| Argument modification | Yes: explicit requested access bits only |

## Enable and observe

1. Complete the [VM setup](../../getting-started/README.md) and load the matching pair.
2. Dashboard **[2] Hooks and telemetry -> NtOpenFile -> [1] Enable hook**.
   Select by name: row numbers change with export availability and aliases.
3. Select the target again -> **[3] Log route -> [2] Console**.
4. Select **Live monitor**, then run the controlled workload below elsewhere in the guest.
5. Verify the target name and captured fields before adding a filter or policy.
   Press Q to leave live view; the hook remains installed.

The menu resolves exports at runtime and combines names that share an entry
address. A preparation failure identifies the target and status in the driver
log; use that entry when investigating an unavailable hook.

## Captured numeric fields

These are input metadata. Field numbers match the filter menu, including gaps.
Pointers and handles are numeric values; requested sizes are bytes. The event's
capture status identifies which fields are available.

| Field number | Console label | Meaning / interpretation |
| --- | --- | --- |
| 1 | `access` | Requested access mask before any policy reduction. |
| 2 | `share` | Requested file sharing flags, not the access granted to the handle. |
| 4 | `open_options` | Requested native open-option bits. |
| 7 | `root_directory` | RootDirectory handle used for relative object-name resolution. |
| 8 | `object_attributes` | Captured OBJECT_ATTRIBUTES attribute mask. |

## Filter example

Select **[4] Log filter -> [7] Field condition -> access -> Has all bits**,
then enter **FILE_READ_DATA**. For a range enter the lower and upper bound separately.
Angle-bracket values mean substitute a value from your own baseline; do not
paste them literally. Save with **[9]**. Add the test process Caller PID to isolate the workload.
Supported path sources: file path. Configure one under [6] Path filter.

All conditions combine with AND. Use `?` to list symbols for the selected field;
numeric input is accepted. A missing field does not match. Reset with **[8] -> [9]**.
See [filter semantics](../../features/filters/README.md).

## Example workload and expected result

Use a controlled native open of an existing disposable file. Enable NtCreateFile during initial observation if the chosen high-level API does not hit this entry.

The entry event records the request inputs. Compare it with the application's
return status and output to follow the operation from entry to completion.

## Blocking and modification
Select **NtOpenFile -> [5] Policy -> [1] Edit scope**. Set a test caller PID
and, where applicable, one of the path/field criteria above. **[9] Save** returns
to the policy draft; choose **[3] Deny matching requests -> [9] Apply**.
Enable the hook separately. Expected decision: BLOCKED and STATUS_ACCESS_DENIED
for eligible matching calls. Confirm the failure in the application. Disable
with **[2] Disable -> [9] Apply**, then verify the same workload succeeds again.

Policies apply to captured user-mode requests at PASSIVE_LEVEL. Kernel-mode calls,
raw fallback and incomplete captures are not enforced. Protected-path denials
take precedence. A log filter can hide a policy event without disabling policy.

For access reduction, choose **[4] Remove requested access bits**, enter a
nonzero mask for this API, then Apply. Only requested access can be reduced;
the result is `requested & ~remove`. Matching generic/MAXIMUM_ALLOWED requests
are denied, not silently converted. MODIFIED and effective_access show the
request transformation, not the final rights on a successful returned handle.

## Interpretation notes

DesiredAccess is reduced in RDX. Some user-mode open APIs reach NtCreateFile instead; verify which hook actually fires.

## End the experiment

Disable the policy and Apply first. Set log route Off and disable this hook.
Remove any protected test paths separately with hooks OFF. A console exit alone
does not remove hooks. See [shutdown](../../features/diagnostics/README.md).

## Implementation

- [Export metadata](../../../hypervisor/src/syscall.c)
- [Field labels](../../../shared/telemetry_fields.h)
- [Filter capability rules](../../../shared/telemetry_match.h)
- [Guest capture and decisions](../../../hypervisor/src/filter.c)
- [Policy validation](../../../shared/telemetry_policy.h)
