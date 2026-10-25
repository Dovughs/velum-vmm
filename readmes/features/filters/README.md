# Log filters and symbolic input

[Documentation index](../../README.md) | [Project](../../../README.md)


## Configure one target

Open **Hooks and telemetry -> target name -> [4] Log filter**.

| Menu | Criterion |
| --- | --- |
| 1 / 2 | Caller PID / TID; 0 means any |
| 3 | Target PID, only on targets with process identity |
| 4 | Any / Continue / Blocked / Modified |
| 5 | Any / User / Kernel execution mode |
| 6 | A supported path source and match mode |
| 7 | One captured numeric field condition |
| 8 | Reset the draft to all events |
| 9 | Save to driver and profile |
| 0 | Discard edits and return |

Conditions combine with **AND**, not OR. One target has one numeric condition,
not an arbitrary expression tree. Kernel routine filters offer numeric fields
but not caller PID/TID, execution mode or action criteria. Use each target's
guide for its allowed path sources and field numbers.

Numeric operators: Equals, Has all bits (`(actual & mask) == mask`), and an
inclusive lower/upper range. A missing captured field does not match.

Path matching is case-insensitive: exact, directory subtree or contains.
Drive-qualified paths are resolved to native paths. Exact/subtree requires a
full path; contains also accepts a fragment. Reapply configuration if drive
mappings change. A target process image and a file path are different sources.

## Example

For `NtWriteFile`, set Caller PID to the PID printed by `$PID` in a dedicated
PowerShell terminal. Set file path to exact `C:\VelumLab\sample.txt`, then field
**bytes_requested**, inclusive range **1..4096**. Save. Enter live monitor and
write to that file from the same terminal. All three conditions must match.
Use a baseline first: .NET buffering can split a logical write into several calls.

## Input formats
Choose a field, then enter decimal, hexadecimal, or its symbolic names joined
with `|`, for example `PAGE_EXECUTE_READWRITE | PAGE_GUARD`. Names are
case-insensitive; `?` lists the selected field's supported names. `PAGE_NO_ACCESS`
is accepted as an alias of the official `PAGE_NOACCESS`. A bitwise expression is
a numeric filter, not validation of whether Windows accepts that flag combination.
In particular `PAGE_EXECUTE_READWRITE | PAGE_NOACCESS` is not a valid base
protection request. Enumerations such as create disposition cannot be ORed.

Protected-file paths, path filters and import-file paths accept surrounding
double quotes and surrounding whitespace. Matching curly quotation marks are
also accepted. Embedded or unmatched quotes are rejected.

The single `flags_or_replace` condition cannot also constrain an information
class: a rename flag name is still a numeric comparison against that field.

## Reset and distinction from policies

Select **[8] Reset to all -> [9] Save**. This changes event visibility only.
It neither enables/disables hooks nor removes a deny policy. A blocked operation
can be absent from the log if the log filter excludes it.

Implementation: [matching](../../../shared/telemetry_match.h),
[field schema](../../../shared/telemetry_fields.h),
[symbol tables](../../../shared/telemetry_symbols.h).

## Recorded example

These captures show the physical Windows 11 test session.
See the [session manifest](../../../assets/capture-manifest.md) for environment and reproduction details.

### NtWriteFile entry telemetry

A six-byte request is captured with its caller, path and input fields. CONTINUE indicates that the policy allowed execution to proceed.

![NtWriteFile entry telemetry](../../../assets/screenshots/02-hook-telemetry.png)
