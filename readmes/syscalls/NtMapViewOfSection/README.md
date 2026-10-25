# NtMapViewOfSection

[Documentation index](../../README.md) | [Project](../../../README.md)

Observe mapping a section view into a process.

## Capabilities

| Capability | Support |
| --- | --- |
| Stable protocol target ID | 7 (not the Windows syscall number or console row) |
| Hook / input observation | If the export resolves and the entry is supported |
| Numeric log condition | One of the fields below |
| Path sources | section object name; target image |
| Caller PID/TID, mode/action log criteria | Yes, when metadata is captured |
| Target PID criterion | Yes |
| Blocking policy | Yes: scoped deny |
| Argument modification | No |

## Enable and observe

1. Complete the [VM setup](../../getting-started/README.md) and load the matching pair.
2. Dashboard **[2] Hooks and telemetry -> NtMapViewOfSection -> [1] Enable hook**.
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
| 1 | `section_handle` | Input section handle; not its backing-file handle. |
| 2 | `process_handle` | Input process handle; not a process ID. |
| 3 | `base_requested` | Value read from the input base-address pointer, not the returned base. |
| 4 | `view_size_requested` | Input view size in bytes, before Windows updates it. |
| 5 | `section_offset` | Input section offset value if the supplied pointer is captured. |
| 6 | `zero_bits` | Input address-placement constraint. |
| 7 | `commit_size` | Requested commit size in bytes. |
| 8 | `inherit` | SECTION_INHERIT enumeration. |
| 9 | `allocation_type` | Requested memory allocation flag mask. |
| 10 | `protection` | Requested protection value, not the final effective page rights. |

## Filter example

Select **[4] Log filter -> [7] Field condition -> protection -> Equals**,
then enter **PAGE_READWRITE**. For a range enter the lower and upper bound separately.
Angle-bracket values mean substitute a value from your own baseline; do not
paste them literally. Save with **[9]**. Add the test process Caller PID to isolate the workload.
Supported path sources: section object name; target image. Configure one under [6] Path filter.

All conditions combine with AND. Use `?` to list symbols for the selected field;
numeric input is accepted. A missing field does not match. Reset with **[8] -> [9]**.
See [filter semantics](../../features/filters/README.md).

## Example workload and expected result

Map and unmap a small view in the test process. Scope by caller and target PID; verify mapping success in the application.

The entry event records the request inputs. Compare it with the application's
return status and output to follow the operation from entry to completion.

## Blocking and modification
Select **NtMapViewOfSection -> [5] Policy -> [1] Edit scope**. Set a test caller PID
and, where applicable, one of the path/field criteria above. **[9] Save** returns
to the policy draft; choose **[3] Deny matching requests -> [9] Apply**.
Enable the hook separately. Expected decision: BLOCKED and STATUS_ACCESS_DENIED
for eligible matching calls. Confirm the failure in the application. Disable
with **[2] Disable -> [9] Apply**, then verify the same workload succeeds again.

Policies apply to captured user-mode requests at PASSIVE_LEVEL. Kernel-mode calls,
raw fallback and incomplete captures are not enforced. Protected-path denials
take precedence. A log filter can hide a policy event without disabling policy.

This target supports scoped denial while preserving the original arguments.

## Interpretation notes

The section object name is not the backing-file path. Requested base/view size are input values. The policy cannot rewrite protection, size, target process or mapped address.

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
