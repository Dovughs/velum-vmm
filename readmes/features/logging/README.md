# Hooks and live logging

[Documentation index](../../README.md) | [Project](../../../README.md)


## First event

1. Load the matching driver and start the console as administrator.
2. Open dashboard **[2] Hooks and telemetry**. Select a target by **name**;
   displayed row numbers depend on exports and alias deduplication.
3. Choose **[1] Enable hook**. Verify its HOOK column reads ON.
4. Select the same target, **[3] Log route -> [2] Console**.
5. Select **Live monitor**, whose number is printed below the target list.
6. Generate a controlled operation in a second guest terminal. For file tests,
   use a disposable file under `C:\VelumLab` and scope by the terminal's PID.
7. Press **Q**, **Esc**, or **Ctrl+C** to leave the viewer. The hook stays enabled.

Selecting All targets enables only the unique available entries. It does not
automatically enable logging or create a policy. FILTER=ALL means unrestricted
event visibility, not that all hooks are enabled.

## Routes and viewer controls

| Setting | Behavior |
| --- | --- |
| Off | No event route; the hook and any active policy remain separate |
| DebugView | Deferred kernel debug output; configure the debug consumer separately |
| Console | Armed in the menu; capture runs while live monitor is active |
| Both | Debug output plus active console capture |
| P | Pause/resume console capture; paused events are not retained for that route |
| C | Clear the display, not kernel summary totals |
| Q / Esc / Ctrl+C | Exit live view and disable console capture |

Leaving live view keeps DebugView routing unchanged. Closing the connection
disables event routes while explicitly installed hooks remain. Reopening the
console does not restore log routes automatically.

## Reading evidence correctly

`CONTINUE` means the entry was permitted to proceed, not that Windows completed
the request. `BLOCKED` means a policy/protected-path decision prevented the
original call. `MODIFIED` reports access reduction before the original call.
Capture status describes metadata collection, not the returned NTSTATUS.
Timestamps describe drain time. Confirm completion in the test application.

Partial captures retain available fields; a missing field is not a zero value.
Raw fallbacks cannot satisfy custom metadata filters. File bytes and copied
process-memory contents are not included. Bounded queues can drop events;
inspect overflow counters before interpreting counts as complete coverage.

## No events?

Check driver status, target availability, hook ON, route, live-view state, and
filter scope in that order. An arbitrary application may use a different kernel
entry than expected. Capture one unfiltered baseline before creating conditions.
Disable unused high-volume pool hooks while isolating a file-operation test.

Implementation: [monitor console](../../../user-mode/console/monitor.c),
[monitor delivery](../../../hypervisor/src/monitor.c),
[deferred drain](../../../hypervisor/src/syscall.c).

## Recorded example

These captures are from the physical Windows 11 session, not a VMware guest.
See the [session manifest](../../../assets/capture-manifest.md) for environment and reproduction details.

### NtWriteFile entry telemetry

A six-byte request from the scoped caller is observed. CONTINUE describes the policy decision, not the completed write result.

![NtWriteFile entry telemetry](../../../assets/screenshots/02-hook-telemetry.png)
