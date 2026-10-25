# NtDuplicateObject

[Documentation index](../../README.md) | [Project](../../../README.md)

Observe duplication or close-source operations between process handle tables.

## Capability contract

| Capability | Support |
| --- | --- |
| Stable protocol target ID | 24 (not the Windows syscall number or console row) |
| Hook / input observation | If the export resolves and the entry is supported |
| Numeric log condition | One of the fields below |
| Path sources | target image (recipient process) |
| Caller PID/TID, mode/action log criteria | Yes, when metadata is captured |
| Target PID criterion | Yes |
| Blocking policy | Yes: scoped deny |
| Argument modification | Yes: explicit requested access bits only |

## Enable and observe

1. Complete the [VM setup](../../getting-started/README.md) and load the matching pair.
2. Dashboard **[2] Hooks and telemetry -> NtDuplicateObject -> [1] Enable hook**.
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
| 1 | `desired_access` | Explicit requested access mask; SAME_ACCESS can make it inapplicable. |
| 2 | `source_process_handle` | Input handle identifying the source process handle table. |
| 3 | `source_handle` | Input handle in the source process handle table. |
| 4 | `recipient_process_handle` | Input target process handle; zero may be used in close-only cases. |
| 5 | `handle_attributes` | Requested duplicate handle attribute bits. |
| 6 | `duplicate_options` | Input DUPLICATE_* options; never rewritten by policy. |
| 7 | `source_pid` | Best-effort resolved source process ID. |
| 8 | `output_handle_pointer` | Address where Windows would store a handle; never a captured output handle. |
| 9 | `desired_access_ignored` | Whether SAME_ACCESS makes the DesiredAccess input irrelevant. |
| 10 | `close_source` | Whether DUPLICATE_CLOSE_SOURCE was requested; denied calls do not execute it. |
| 11 | `same_attributes` | Whether DUPLICATE_SAME_ATTRIBUTES was requested. |
| 12 | `policy_reason` | Decision explanation; available for log filtering, excluded from policy scope. |

## Filter example

Select **[4] Log filter -> [7] Field condition -> duplicate_options -> Has all bits**,
then enter **DUPLICATE_SAME_ACCESS**. For a range enter the lower and upper bound separately.
Angle-bracket values mean substitute a value from your own baseline; do not
paste them literally. Save with **[9]**. Add the test process Caller PID to isolate the workload.
Supported path sources: target image (recipient process). Configure one under [6] Path filter.

All conditions combine with AND. Use `?` to list symbols for the selected field;
numeric input is accepted. A missing field does not match. Reset with **[8] -> [9]**.
See [filter semantics](../../features/filters/README.md).

## Controlled workload and expected evidence

Duplicate a known test process handle with explicit DesiredAccess 0x38 and remove PROCESS_VM_WRITE (0x20). Expect effective_access 0x18, then verify the new handle cannot perform the removed operation. Repeat separately with SAME_ACCESS and CLOSE_SOURCE.

The event proves an observed input at this entry point. It does not establish
completion, returned handles/pointers or transferred bytes. Pair it with the
test application's result. Do not use incidental desktop traffic as proof of a
specific API-to-entry mapping.

## Blocking and modification
Select **NtDuplicateObject -> [5] Policy -> [1] Edit scope**. Set a test caller PID
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



This target is resolved only through MmGetSystemRoutineAddress. The export was
present on the development host; another Windows build must still resolve it
and pass the existing entry-instruction hook checks. No SSDT or signature fallback
has been added.

Detailed events contain caller PID/TID, source PID/image, recipient PID/image,
source/recipient process handles, source handle, requested access, handle
attributes, duplicate options and the output-handle pointer. Target PID and
target-image filters refer to the **recipient process**, not the process object
that the source handle might represent. The source PID can be matched through
the source_pid field. NULL recipient handles for close-only operations retain
the source metadata and have no recipient identity.

The source handle's object type, granted-access snapshot, duplicated output
handle and operation result are not captured. Source/recipient metadata is an
entry-time observation and is not atomic with Windows subsequently resolving
the handles. As with existing targets, incomplete metadata fails open.
Object-specific access bits therefore remain numeric in logs. Process/thread/file
symbol names accepted by the editor are numeric aliases, not object-type
conditions; scope such rules to controlled callers and expected handle types.

For an explicit access mask, a matched remove-access rule clears only the selected
bits in argument 5. A guarded atomic update verifies the captured stack argument
before replacing it; failure denies the call. Original fields remain in the event,
with effective_access reporting the replacement. Duplicate options and output
memory are never modified.

For **DUPLICATE_SAME_ACCESS**, DesiredAccess is ignored by Windows. A matched
remove-access rule therefore denies the call with STATUS_ACCESS_DENIED and
policy_reason=SAME_ACCESS_RESTRICTION_DENIED. It does not attempt to convert a
racy source-handle access snapshot into an authorization decision. Generic or
MAXIMUM_ALLOWED access and unknown option bits are also denied under a matching
restriction. See the [WDK duplicate-object contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-zwduplicateobject).

A deny rule rejects the entire call. In particular, **DUPLICATE_CLOSE_SOURCE
does not close the source handle when Velum denies the call**. With policy OFF,
or without a matching rule, the original operation and options are preserved.
policy_reason is usable as a log filter but is not a policy scope input.

Suggested VM validation: duplicate a test process handle with explicit access
0x38 while removing PROCESS_VM_WRITE; expect MODIFIED/effective_access=0x18
and denied writes through the new handle. Repeat with DUPLICATE_SAME_ACCESS;
expect BLOCKED with the reason above. Test SAME_ACCESS|CLOSE_SOURCE and verify
the source handle remains usable after denial. Finally disable the policy and
verify normal Windows behavior. Host tests do not validate the live guest stack
mutation, VM exits or return path; these require a dedicated VM run.


## Target-specific limits

Target PID/image mean the recipient process. Source PID is a separate field. Object type and the returned handle value are not captured; access symbols are numeric aliases, not object-type constraints.

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
