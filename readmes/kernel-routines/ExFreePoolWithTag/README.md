# ExFreePoolWithTag

[Documentation index](../../README.md) | [Project](../../../README.md)

Observe free pointer and supplied tag at an exported pool-free entry.

## Capabilities

| Capability | Support |
| --- | --- |
| Stable protocol target ID | 15 (not the Windows syscall number or console row) |
| Hook / input observation | If the export resolves and the entry is supported |
| Numeric log condition | One of the fields below |
| Path sources | None |
| Caller PID/TID, mode/action log criteria | No |
| Target PID criterion | No |
| Blocking policy | No: observation only |
| Argument modification | No |

## Enable and observe

1. Complete the [VM setup](../../getting-started/README.md) and load the matching pair.
2. Dashboard **[2] Hooks and telemetry -> ExFreePoolWithTag -> [1] Enable hook**.
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
| 1 | `pointer` | Input pointer to the object/buffer being freed; no pointed-to data is copied. |
| 2 | `tag` | Input pool tag; displayed as hex and memory-byte-order text. Alias caveats apply. |

## Filter example

Select **[4] Log filter -> [7] Field condition -> pointer -> Equals**,
then enter **<pointer from your baseline event>**. For a range enter the lower and upper bound separately.
Angle-bracket values mean substitute a value from your own baseline; do not
paste them literally. Save with **[9]**. Caller PID/TID filters are unavailable for this kernel routine.
No path criterion is supported for this target.

All conditions combine with AND. Use `?` to list symbols for the selected field;
numeric input is accepted. A missing field does not match. Reset with **[8] -> [9]**.
See [filter semantics](../../features/filters/README.md).

## Example workload and expected result

Allocate and free a tagged test buffer with a controlled driver. On an aliasing build, compare with an untagged call and treat tag validity accordingly.

The entry event records the request inputs. Compare it with the application's
return status and output to follow the operation from entry to completion.

## Blocking and modification
This target provides input telemetry and numeric filtering.
The [kernel summary](../../features/kernel-summary/README.md) groups observed calls
and requested bytes by routine, return-site module and tag, with overflow counters.

## Interpretation notes

On the tested Windows 10 build this aliases ExFreePool. The preferred representative is ExFreePoolWithTag, but untagged callers may have meaningless RDX/tag. Do not use such tags to infer ownership.

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
