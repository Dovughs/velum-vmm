# ExAllocatePoolWithTagPriority

[Documentation index](../../README.md) | [Project](../../../README.md)

Observe pool type, requested bytes, tag and priority.

## Capabilities

| Capability | Support |
| --- | --- |
| Stable protocol target ID | 16 (not the Windows syscall number or console row) |
| Hook / input observation | If the export resolves and the entry is supported |
| Numeric log condition | One of the fields below |
| Path sources | None |
| Caller PID/TID, mode/action log criteria | No |
| Target PID criterion | No |
| Blocking policy | No: observation only |
| Argument modification | No |

## Enable and observe

1. Complete the [VM setup](../../getting-started/README.md) and load the matching pair.
2. Dashboard **[2] Hooks and telemetry -> ExAllocatePoolWithTagPriority -> [1] Enable hook**.
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
| 1 | `pool_type` | Legacy POOL_TYPE value. |
| 2 | `bytes_requested` | Requested byte count at function entry. |
| 3 | `tag` | Input pool tag; displayed as hex and memory-byte-order text. Alias caveats apply. |
| 4 | `priority` | Input allocation priority value, not observed scheduling priority. |

## Filter example

Select **[4] Log filter -> [7] Field condition -> priority -> Equals**,
then enter **<priority from your baseline event>**. For a range enter the lower and upper bound separately.
Angle-bracket values mean substitute a value from your own baseline; do not
paste them literally. Save with **[9]**. Caller PID/TID filters are unavailable for this kernel routine.
No path criterion is supported for this target.

All conditions combine with AND. Use `?` to list symbols for the selected field;
numeric input is accepted. A missing field does not match. Reset with **[8] -> [9]**.
See [filter semantics](../../features/filters/README.md).

## Example workload and expected result

Use an explicit call in a test driver with known size/tag/priority. Filter on the priority observed in the baseline event.

The entry event records the request inputs. Compare it with the application's
return status and output to follow the operation from entry to completion.

## Blocking and modification
This target provides input telemetry and numeric filtering.
The [kernel summary](../../features/kernel-summary/README.md) groups observed calls
and requested bytes by routine, return-site module and tag, with overflow counters.

## Interpretation notes

Priority is an input hint; the log does not report allocation outcome or latency.

## End the experiment

Set log route Off and disable this hook.
Remove any protected test paths separately with hooks OFF. A console exit alone
does not remove hooks. See [shutdown](../../features/diagnostics/README.md).

## Implementation

- [Export metadata](../../../hypervisor/src/syscall.c)
- [Field labels](../../../shared/telemetry_fields.h)
- [Filter capability rules](../../../shared/telemetry_match.h)
- [Kernel argument decoding](../../../shared/kernel_telemetry.h)
- [Policy validation](../../../shared/telemetry_policy.h)
