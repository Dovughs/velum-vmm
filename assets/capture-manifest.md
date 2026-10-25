# Runtime capture manifest  -  2026-10-24

Actual local session, approximately 19:55-20:03 Europe/Istanbul (UTC+03:00).
Project source and binaries were not edited or rebuilt. The root README was left unchanged during capture; documentation integration followed on 2026-10-25.

## Shared environment

- Windows 11 Home, build **26300.9711**.
- Physical MSI MS-7E02 system; Intel Core i7-12700F, 12 cores / 20 logical processors. These are **bare-metal captures, not VMware captures**. No outer VMM version applies.
- Before loading: HypervisorPresent=False, firmware virtualization enabled, VBS status=0, SecurityServicesRunning={0}. Existing signing configuration was used without modification.
- Source revision: unavailable; this supplied folder has no `.git` directory. Source-tree inventory digest (sorted relative paths plus SHA256, hypervisor/user-mode/shared/tests): `660db30e4e9d1cbddd5e3d306fa57c449a6e10ce48dbcf1cae57583a57813b61`.
- Release driver SHA256: `1e892bf073a8db6e95a93ae8c162dc82ba61ef21165c0f2f4fec90c801917f57`.
- Release console SHA256: `9a232cdabaa6ec4b90b2bca73080c56303c0707e4dd8f9525b2562cb4f4e6e9e`.
- `Velum` demand-start service loaded the existing Release binary. All **20/20** processor diagnostics passed. Final console shutdown verified native return on every processor; SCM subsequently returned 1060 (service absent).

The DebugView startup image is included with the operator's approval. All capture pixels remain unchanged. The source inventory above describes the capture session, including maintainer-local tests that are excluded from the public source distribution.

## Media

All rows use the shared OS, CPU, source digest and binary hashes above. Paths are relative to `assets/`.

| File | Target / route / scope / workload | Actual outcome | Limits |
| --- | --- | --- | --- |
| screenshots/00-debugview-startup.png | DebugView kernel capture before first load | Persistent launch on 20 cores; DriverEntry completed | Visible startup tail, not the complete log |
| screenshots/01-dashboard.png | Console immediately after successful load; hooks OFF, protected file list empty | Dashboard responds | Pair with 01b for running-state checks |
| screenshots/01b-diagnostics.png | Console Diagnostics | 20/20 processors passed | Viewport shows final CPUs and aggregate result |
| screenshots/02-hook-telemetry.png | NtWriteFile ON, Console route; caller PID 20660 and exact sample.txt path filter | CONTINUE; six-byte write observed | Entry telemetry; independent result is in deny GIF |
| demos/03-scoped-deny.gif | Same persistent caller PID 20660 and same existing file handle, C:\VelumLab\sample.txt; NtWriteFile only; Console route; legacy protected paths empty | OFF success -> scoped DENY / BLOCKED / Win32 5 and unchanged contents -> OFF success | 20-second sequence of real screenshots with reading pauses; not continuous real-time video |
| screenshots/04-access-reduction.png | NtCreateFile only; Console route; PID 19284 + exact sample.txt; remove mask 0x6 | Requested 0x12019F -> effective 0x120199. Open succeeds, subsequent WriteFile fails with Win32 5; file unchanged | Labeled composite of the real monitor and probe windows; no claim of arbitrary argument editing |
| screenshots/04a-access-policy.png | Same access-reduction rule | Applied remove mask 0x6 | Companion configuration evidence |
| screenshots/04b-access-independent-result.png | Same caller; explicit-rights opens before/after policy | Baseline write succeeds; reduced-rights handle cannot write | Complete original probe-window capture |
| screenshots/05-kernel-summary.png | ExAllocatePool2, DebugView route, unrestricted log filter; ambient desktop workload | Module/tag call and input-byte totals; capture queue drops 1,303,982; summary overflow events 2,384 | Bounded, lossy input telemetry; not allocation completion or leak detection. Top viewport of a larger table; not a controlled benchmark |
| demos/06-cpuid-apply-reset.gif | Console CPUID menu: edit hypervisor vendor, Apply, query leaf 0x40000000/subleaf 0, Reset/Apply, query again | VelumLabDemo observed live, then default Velum vendor restored | 17.5-second sequence of real screenshots with reading pauses; reset restores project defaults, not CPUID overrides OFF |
| screenshots/07-tsc-configuration.png | Console TSC draft 100 ticks -> Apply -> Reset/Apply | Applied ON/100, then OFF/0 | Labeled three-state composite; configuration round trip only, no latency claim |
| screenshots/08-shutdown.png | Dashboard Shutdown, no active hooks | Shutdown verified on all processors; Velum stopped and marked for deletion | Subsequent SCM check independently confirmed service absence |

## Reproduction notes

A small C# file-probe executable was built outside this repository. It used CreateFileW with explicit access 0x0012019F, sharing read/write/delete, OPEN_EXISTING, and WriteFile to append the exact six-byte ASCII payload `MARK\r\n`. The deny sequence reused one handle. For the reduction sequence a fresh explicit-rights handle was opened each time. FILE_WRITE_DATA (0x2) and FILE_APPEND_DATA (0x4) were removed together. The final helper independently read file contents using explicit read rights and shared access.

The deny baseline already contained two MARK lines (including an earlier setup write); denial left both intact and the final successful operation added the third. The access demonstration then added a fourth MARK in its baseline and left those four intact after reduction.

An early helper iteration encountered a sharing violation while re-reading the file; another used generic read access, which the documented conservative policy rejected. The helper was corrected outside the repository and the final independent access comparison was rerun successfully. Failed helper attempts are not presented as successful evidence.

The Windows Graphics Capture backend failed with 0x80070424. Starting the existing per-user CaptureService did not repair that backend. Captures instead used native PrintWindow/GDI; the service was restored to its prior stopped state. GIF frames are captured screens assembled in chronological order with phase captions outside the captured pixels. Composite stills crop only window areas not needed for their stated comparison. No outcomes, timestamps, counters or log text were altered.

No security/signing configuration was changed. Policies were disabled after their tests, CPUID reset, TSC reset to OFF, hooks disabled and driver removed. The disposable sample file remains under C:\VelumLab. DebugView remains open with capture paused; the console remains at its shutdown result. Auxiliary automation and test sources stayed outside the project. No media was uploaded or published.

The later Windows 10 22H2 / Windows 11 26H2 runtime confirmation and current
packaging hashes are in the [Release validation record](release-validation.md).
