# Profiles and settings lifetime

[Documentation index](../../README.md) | [Project](../../../README.md)


| Setting | Console reconnect | Driver reload | Disk profile |
| --- | --- | --- | --- |
| Installed hooks | Remain installed | Start OFF | Selection remembered, not auto-enabled |
| Log routes | Start OFF | OFF | Not automatically restored |
| Per-target log filters | Loaded/restored through console profile | Reapplied by console connection | Yes |
| Protected-file paths | Driver session retained | Lost | No |
| Scoped policies | Driver session retained | OFF | No |
| CPUID/TSC settings | Driver session retained | Defaults | No |
| Summary counters | Driver-owned, subject to reset | Lost | No |

The profile is `%LOCALAPPDATA%\Velum\console-settings.bin`. Hook choices and
log filters are saved by their respective console actions. **Enable remembered
hooks** applies only remembered targets available on the current build and leaves
log routing separate. Connecting never turns remembered hooks on automatically.

ABI v10 is required for the matching driver/console pair. Valid v6/v7/v8/v9
profiles migrate in memory; later saves use the new format. Retired targets are
cleared. Invalid files are rejected. On first use, a legacy MyHypervisor profile
may be copied if no Velum profile exists; the old file is not deleted.

To reset the disk profile, close the console, back up the file outside the repo,
and remove it. Also account for the legacy profile if it exists, otherwise it
may be imported again. Disk reset does not disable a currently active driver
policy or hook. Use the UI for those, or the clean shutdown/reload procedure.

Do not commit profiles: they can contain local paths and identifiers. The
repository ignores `console-settings.bin`.

Implementation: [profile and connection handling](../../../user-mode/console/monitor.c).
