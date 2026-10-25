# Diagnostics and clean shutdown

[Documentation index](../../README.md) | [Project](../../../README.md)


## Status

Dashboard **[3] Diagnostics** or `velum-console.exe status` pins the verification
thread to each available CPU, queries the applied CPUID rules and the control
ABI, and reports per-CPU results. Restricted process affinity prevents a full
check. PASS establishes these checks only, not universal hypervisor correctness.

## Shutdown

```bat
velum-console.exe stop
velum-console.exe remove
```

`stop` requests the hypervisor's shutdown handshake. `remove` requests shutdown
and then stops/deletes the kernel-driver service. Dashboard **[6] Shutdown** also
performs shutdown and service removal. Closing the console only stops delivery;
it is not driver teardown. Service removal can remain pending while handles exist.
Do not replace a loaded binary and assume the running code changed.

## Symbolized investigation

Build with `build.bat diagnostic`, deploy that driver/console pair, and retain
their matching PDBs on the debugger machine. A PDB rebuilt later need not match
an older binary. Add the actual symbol directory in WinDbg with `.sympath+`.
Before loading the driver, collect:

```text
.logopen /t C:\Users\Public\velum-startup.txt
ed nt!Kd_IHVDRIVER_Mask 0xf
vertarget
!cpuinfo
g
```

The log file is on the debugger machine. If an unexpected stop occurs, capture
`.lastevent`, `r`, `kv`, and `!running -it` before resuming. If a bugcheck occurred,
also capture `!analyze -v`. A connection message alone does not prove a bugcheck.
Close a completed log with `.logclose`. Never publish KDNET keys or credentials.

## Regression procedure

Record CPU, Windows build, VMM version/settings, source revision and binary hash.
Test hook enable/disable, controlled operations, live-view transitions, overflow,
shutdown and reload. Exercise policy OFF/ON/OFF with an independent application
result. Host tests do not exercise real VMX/NMI delivery or guest stack mutations.

References: [Microsoft multiprocessor debugging](https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/multiprocessor-syntax),
[debug-message filtering](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/reading-and-filtering-debugging-messages).
Implementation: [diagnostics](../../../user-mode/console/cpuid.c),
[service removal](../../../user-mode/console/service.c),
[native return](../../../hypervisor/src/native.c).

## Recorded example

These captures are from the physical Windows 11 session, not a VMware guest.
See the [session manifest](../../../assets/capture-manifest.md) for environment and reproduction details.

### Per-CPU diagnostics

The final viewport reports 20/20 processors passed. This validates the displayed checks, not every hypervisor behavior.

![Per-CPU diagnostics](../../../assets/screenshots/01b-diagnostics.png)

### Verified shutdown

The console reports native return verified on all processors and service removal requested. The session manifest records the subsequent service-absence check.

![Verified shutdown](../../../assets/screenshots/08-shutdown.png)
