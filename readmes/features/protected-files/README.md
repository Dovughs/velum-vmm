# Protected-file list

[Documentation index](../../README.md) | [Project](../../../README.md)


## Configure

1. In Hooks and telemetry select All targets -> Disable hook.
2. Open dashboard **[1] Protected files**.
3. Choose **[1] Add file** and enter a local absolute path, such as
   `C:\VelumLab\protected.txt`. Surrounding quotes are accepted.
4. Alternatively **[3] Import UTF-8 list** accepts a UTF-8 text file with one
   absolute path per line. An invalid line rejects the import; duplicates are
   skipped. The list is limited to 256 paths.
5. Return to Hooks and telemetry and enable the entry points needed by your test.
   Paths alone do not install hooks. Enable a log route separately.
6. Test from a separate terminal with a disposable file, comparing unprotected
   and protected runs and checking the resulting file state.

Menu **[2]** removes by displayed index, **[4]** clears the list, **[5]/[6]**
change pages. These changes apply to the current driver session and require
hooks OFF. Paths are not automatically recovered from the console profile.

## Coverage and limitations
The console accepts up to 256 local absolute paths. Change the list with hooks
OFF. Relevant enabled hooks can deny matching creates/opens, deletion,
rename/link operations, size changes, writes and file-backed section creation.
This legacy protected-path list does not cover reads or process/memory requests;
those operations can use the independent scoped policies described below.

This is path-based PoC coverage, not file-identity enforcement. Aliases, hard links,
reparse points, already mapped views, capture races and operations bypassing the
selected entry points are not comprehensively covered. Capture failures generally
fail open; a known protected rename/link source remains blocked when destination
capture fails. Disabling a hook reduces coverage. Hook-mask changes briefly rebuild
the interception batch. Reapply path configuration after drive-mapping changes.

For a clean write-blocking demonstration, open a test file handle before enabling
only `NtWriteFile`; otherwise an earlier NtCreateFile/NtOpenFile denial can stop
the test before the write entry is reached. The same distinction applies to
deletion APIs that use NtSetInformationFile rather than NtDeleteFile.

## Cleanup

Disable hooks, clear/remove the test paths, then restore the desired hook mask.
Do not use a policy reset as a substitute for clearing the protected-file list.

Implementation: [path UI](../../../user-mode/console/monitor.c),
[enforcement](../../../hypervisor/src/filter.c).
