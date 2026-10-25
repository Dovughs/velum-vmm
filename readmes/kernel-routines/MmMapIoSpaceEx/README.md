# MmMapIoSpaceEx

[Documentation index](../../README.md) | [Project](../../../README.md)

Observe physical range, size and protection supplied to an I/O mapping request.

## Capability contract

| Capability | Support |
| --- | --- |
| Stable protocol target ID | 22 (not the Windows syscall number or console row) |
| Hook / input observation | If the export resolves and the entry is supported |
| Numeric log condition | One of the fields below |
| Path sources | None |
| Caller PID/TID, mode/action log criteria | No |
| Target PID criterion | No |
| Blocking policy | No: observation only |
| Argument modification | No |

## Enable and observe

1. Complete the [VM setup](../../getting-started/README.md) and load the matching pair.
2. Dashboard **[2] Hooks and telemetry -> MmMapIoSpaceEx -> [1] Enable hook**.
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
| 1 | `physical_address` | Input physical range start; not a virtual mapping result. |
| 2 | `bytes_requested` | Requested byte count; not completed, allocated, copied or freed bytes. |
| 3 | `protection` | Requested protection value, not the final effective page rights. |

## Filter example

Select **[4] Log filter -> [7] Field condition -> bytes_requested -> Inclusive range**,
then enter **1..4096**. For a range enter the lower and upper bound separately.
Angle-bracket values mean substitute a value from your own baseline; do not
paste them literally. Save with **[9]**. Caller PID/TID filters are unavailable for this kernel routine.
No path criterion is supported for this target.

All conditions combine with AND. Use `?` to list symbols for the selected field;
numeric input is accepted. A missing field does not match. Reset with **[8] -> [9]**.
See [filter semantics](../../features/filters/README.md).

## Controlled workload and expected evidence

Observe an authorized test driver mapping only its assigned resource. Use a baseline captured protection or size for filtering.

The event proves an observed input at this entry point. It does not establish
completion, returned handles/pointers or transferred bytes. Pair it with the
test application's result. Do not use incidental desktop traffic as proof of a
specific API-to-entry mapping.

## Blocking and modification
This routine is observation-only. There is no deny or argument-edit policy.
Use [kernel summary](../../features/kernel-summary/README.md) for counts and
input-byte totals, with overflow and return-site attribution limits.

## Target-specific limits

No mapped virtual address is returned in telemetry. This guide does not authorize mapping arbitrary physical memory or device ranges.

## End the experiment

Set log route Off and disable this hook.
Remove any protected test paths separately with hooks OFF. A console exit alone
does not remove hooks. See [shutdown](../../features/diagnostics/README.md).

## Source of truth

- [Export metadata](../../../hypervisor/src/syscall.c)
- [Field labels](../../../shared/telemetry_fields.h)
- [Filter capability rules](../../../shared/telemetry_match.h)
- [Kernel argument decoding](../../../shared/kernel_telemetry.h)
- [Policy validation](../../../shared/telemetry_policy.h)
