# Build and run in a Windows test VM

[Documentation index](../README.md) | [Project](../../README.md)

## Requirements

- Intel x64 with exposed VT-x/EPT; AMD SVM is not implemented.
- Tested on **Windows 10 22H2** and **Windows 11 26H2**,
  with the Release driver and console. The import baseline is Windows 10 2004+.
- One fixed processor group, at most 64 processors, without CPU hot-add.
- Administrative access in a disposable development VM and a recovery snapshot.
- The unsigned Release driver uses the temporary Startup Settings option below
  for this lab workflow. The build does not sign the driver.

## 1. VMware configuration

Fully power off the guest, not suspend it. Back up the VM's `.vmx` file. Enable
**Virtualize Intel VT-x/EPT** under the VM processor settings. The corresponding
configuration is:

```ini
vhv.enable = "TRUE"
```

For the Windows 10 compatibility configuration used in this project, also set
one occurrence of:

```ini
hypervisor.cpuid.v0 = "FALSE"
```

This matched the working Windows 11 guest and resolved the startup
freeze in the tested VMware setup. It is an observed workaround, not a universal
VMware requirement or an instruction-level diagnosis. It changes boot-time
CPUID presentation, not whether nested VT-x is enabled. Do not confuse it with
Velum's runtime CPUID menu. See [investigation](../engineering/README.md).

The test environment used Intel i7-12700F and VMware Workstation 26.0.0
build 25388281. Host and guest Hyper-V settings are separate: changing the guest
cannot fix a host configuration that does not expose nested VT-x. If VMware
rejects the nested configuration, resolve that platform issue before loading
Velum; do not change the host's security configuration by guesswork.

## 2. Guest hypervisor ownership

This implementation does not support coexistence with an active Microsoft
Hyper-V/VBS owner inside the guest. In the **test guest**, turn Memory Integrity
off in Windows Security -> Device security -> Core isolation details. From an
elevated terminal set the test boot entry:

```bat
bcdedit /set hypervisorlaunchtype off
```

Restart. Verify the active boot entry and actual VBS state:

```powershell
bcdedit /enum "{current}"
Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard |
    Select-Object VirtualizationBasedSecurityStatus, SecurityServicesRunning
```

The investigated configuration has hypervisorlaunchtype Off. VBS status 2 means
running; 0 means disabled and 1 enabled but not running. An isolated UI toggle
does not prove that policy-controlled VBS/Credential Guard stopped. Resolve
remaining policy settings through Windows' documented administration procedures.

These are compatibility requirements for this lab, not general security advice.
Memory Integrity and CET are different mechanisms; Velum has CET state-transfer
code and does not require globally disabling CET as a substitute for correct state.

References: [Microsoft virtualization coexistence](https://learn.microsoft.com/en-us/troubleshoot/windows-client/application-management/virtualization-apps-not-work-with-hyper-v),
[VBS status](https://learn.microsoft.com/en-us/windows/security/hardware-security/enable-virtualization-based-protection-of-code-integrity).

## 3. Build

Install the Desktop development with C++ workload in Visual Studio 2022 (17.x)
or Visual Studio 2026 (18.x), including MSVC x64/x86 tools and the Windows SDK.
Also install the Windows Driver Kit: an SDK-only installation has no kernel
headers/libraries and cannot build the driver. Community, Professional, Enterprise
and Build Tools installations can be discovered when their C++ tools are present.

Use Microsoft's [supported SDK/WDK and Visual Studio combinations](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk).
Current guidance pairs Visual Studio 2026 with the 28000 kit family and Visual Studio
2022 with the 26100 family. SDK and WDK build numbers must match. The exact installed
kit servicing version is not encoded fully by its `10.0.<build>.0` directory name.

The script chooses the latest installed non-preview VS 2022/2026 C++ installation
using `vswhere`, then the highest complete installed SDK/WDK directory. It checks
required kernel, shared, UCRT and user-mode files and passes that kit version to
`vcvarsall`. This is discovery, not a promise that arbitrary compiler/kit combinations
are supported by Microsoft. Select a supported pair explicitly when multiple
versions are installed.

```bat
build.bat check
build.bat
```

`check` prints the selected toolchain without compiling. No argument, or `release`,
builds optimized Release binaries with `/W4 /WX`, `/debug:none`, no compiler debug
switches and no generated PDB. Both PE debug directories and COFF symbol fields
are checked before output is copied to `x64\Release`.
Runtime logging remains available; symbol-free does not mean silent operation.
The build does not sign or load the driver, and needs no Python or PowerShell.

To select installations explicitly, use a Command Prompt, for example:

```bat
set "VS_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "WDK_ROOT=C:\Program Files (x86)\Windows Kits\10"
set "WDK_VER=10.0.26100.0"
build.bat check
build.bat release
```

Replace the example paths with actual installations. Clear an override with
`set "VS_PATH="` or the corresponding variable name to restore automatic selection.
An invalid explicit override fails instead of silently selecting another kit.

The Release build was verified with Visual Studio Community 2026 and SDK/WDK
10.0.26100.0. Discovery skips incomplete SDK-only directories and validates explicit
toolchain overrides before compiling.

For debugging only, request the separate opt-in mode:

```bat
build.bat diagnostic
```

Its optimized binaries and matching PDBs are under `x64\Diagnostic`. Publish only
the intended Release pair. Do not mix an older loaded driver with a newer console.

## 4. Enable the temporary unsigned-driver test session

Complete the guest settings in section 2 first: turn **Memory Integrity off** in
**Windows Security -> Device security -> Core isolation details**, and run this
from an elevated Command Prompt:

```bat
bcdedit /set hypervisorlaunchtype off
```

Save your work, then enter Windows Recovery Environment:

1. Open **Start -> Power**, hold **Shift**, and select **Restart**.
2. Select **Troubleshoot -> Advanced options -> Startup Settings -> Restart**.
3. At the numbered Startup Settings screen, press **7** or **F7** for
   **Disable driver signature enforcement**.
4. Let Windows boot, then install and start the matching Release pair as described
   in section 5. Load it in this same boot session.

Use Shift + Restart, not Shift + Shut down. F7 is selected at the Startup Settings
screen, not during ordinary desktop operation. If device encryption is enabled,
Windows Recovery may request the device's BitLocker recovery key.

This option applies to the current boot session. A normal restart restores
signature enforcement; repeat the F7 selection before loading the unsigned driver
in another session. Separate organizational code-integrity policies can still block
loading; use a lab configuration you administer.

References: [Windows Startup Settings](https://support.microsoft.com/en-us/windows/experience/startup-boot/windows-startup-settings) and
[Microsoft's temporary load-time enforcement explanation](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/test-signing).

## 5. Install and start

Place the prepared driver and console in `C:\Velum`. In an elevated **Command
Prompt**, for a new service:

```bat
sc.exe create Velum type= kernel start= demand binPath= "C:\Velum\velum.sys"
sc.exe qc Velum
sc.exe start Velum
C:\Velum\velum-console.exe status
C:\Velum\velum-console.exe
```

The spaces after `type=`, `start=` and `binPath=` are intentional. If the service
already exists, inspect its binary path; do not blindly create a second service.
Use the old matching console's `remove` flow before replacing an active driver.
Only one monitor console may connect at a time. A successful SCM start does not
replace the per-CPU status check.

## 6. First use and shutdown

Fresh sessions start with hooks/routes OFF. Enable one target, choose Console
route, then enter Live monitor; follow [logging](../features/logging/README.md).
Start with observation before enabling a scoped policy.

```bat
C:\Velum\velum-console.exe remove
```

This uses the project's shutdown handshake before stopping/deleting the service.
Wait for completion. Closing the console is not shutdown. After the experiment,
restore the VM snapshot or restore its previous boot/security settings. Where
the original hypervisor boot policy was Auto:

```bat
bcdedit /set hypervisorlaunchtype auto
```

Restore Memory Integrity to its previous setting after stopping/removing the
driver, then restart normally. The temporary F7 session ends at reboot.
Do not assume a rebuild updated running code.

## Common startup failures

| Symptom | First checks |
| --- | --- |
| Service load/signature error | Actual binary path, current F7 boot session, Memory Integrity, matching architecture |
| Missing import / unsupported older Windows | Current imports require Windows 10 2004+ |
| VMX unavailable/owned | Intel VT-x exposure, guest VBS state, another VMM owner |
| Windows 10 nested startup freeze | Compare `.vmx` and host mode; use the recorded compatibility setup |
| Hook unavailable or error 50 | Export resolution, aliases and supported entry instruction; inspect named driver error |
| Console cannot connect | Driver running, administrator, one console, matching ABI |
| No events | Hook, route, active live view, filter and actual entry used by workload |

For failures, capture the full startup log with the [diagnostics guide](../features/diagnostics/README.md).

## Recorded example

These captures show the physical Windows 11 test session.
See the [session manifest](../../assets/capture-manifest.md) for environment and reproduction details.


### Control dashboard

The console immediately after loading, with hooks OFF.

![Control dashboard](../../assets/screenshots/01-dashboard.png)

### Per-CPU diagnostics

All 20 processors passed the startup diagnostic checks.

![Per-CPU diagnostics](../../assets/screenshots/01b-diagnostics.png)

### Verified shutdown

The console reports native return verified on all processors and service removal requested. The session manifest records the subsequent service-absence check.

![Verified shutdown](../../assets/screenshots/08-shutdown.png)

### Startup capture

The following DebugView image is from the recorded physical Windows 11 session.

![Velum startup in DebugView](../../assets/screenshots/00-debugview-startup.png)
