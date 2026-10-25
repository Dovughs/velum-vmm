# Runtime capture gallery

[Project overview](../README.md) | [Usage guides](../readmes/README.md) | [Capture manifest](capture-manifest.md)

The main capture session ran on physical Windows 11 build 26300.9711 with an
Intel i7-12700F and 20 logical processors. The [manifest](capture-manifest.md)
records workloads, binary hashes and results. GIFs assemble captured screens
in chronological order; comparison images pair related console and probe views.

Images are embedded below; open them at full size for small console text.


### Startup in DebugView

The visible startup tail records persistent launch on 20 CPUs and DriverEntry completion.

![Velum startup in DebugView](screenshots/00-debugview-startup.png)

### Control dashboard

The console immediately after loading, with hooks OFF.

![Control dashboard](screenshots/01-dashboard.png)

### Per-CPU diagnostics

All 20 processors passed the startup diagnostic checks.

![Per-CPU diagnostics](screenshots/01b-diagnostics.png)

### NtWriteFile entry telemetry

A six-byte request is captured with its caller, path and input fields. CONTINUE indicates that the policy allowed execution to proceed.

![NtWriteFile entry telemetry](screenshots/02-hook-telemetry.png)

### Scoped deny: OFF / DENY / OFF

The same caller and file handle succeed, fail with Win32 error 5 while contents remain unchanged, then succeed after disabling the policy. A chronological screenshot sequence with reading pauses.

![Scoped deny: OFF / DENY / OFF](demos/03-scoped-deny.gif)

### Access reduction and independent result

Requested access 0x12019F becomes 0x120199 after removing mask 0x6. The open succeeds, but a subsequent write is denied. This labeled composite pairs real monitor and probe captures.

![Access reduction and independent result](screenshots/04-access-reduction.png)

### Applied access policy

Caller/path scope and remove mask 0x6 for NtCreateFile.

![Applied access policy](screenshots/04a-access-policy.png)

### Independent access probe

The original probe window compares a successful baseline write with a reduced-rights handle that cannot write.

![Independent access probe](screenshots/04b-access-independent-result.png)

### Kernel telemetry summary

ExAllocatePool2 observations grouped by routine, module and tag. The displayed queue-drop count (1,303,982) and summary-overflow count (2,384) make capture saturation visible alongside the input totals.

![Kernel telemetry summary](screenshots/05-kernel-summary.png)

### CPUID apply and reset

A live vendor query changes to VelumLabDemo and returns to the Velum default after Reset/Apply. Reset does not turn overrides OFF. The GIF presents the captured states in chronological order.

![CPUID apply and reset](demos/06-cpuid-apply-reset.gif)

### TSC offset configuration

A three-state configuration sequence: draft ON/100 ticks, applied ON/100, then reset OFF/0.

![TSC offset configuration](screenshots/07-tsc-configuration.png)

### Verified shutdown

The console reports native return verified on all processors and service removal requested. The session manifest records the subsequent service-absence check.

![Verified shutdown](screenshots/08-shutdown.png)

## Additional PoC screenshots

![All-target hook selection and DebugView entry diagnostics](screenshots/09-all-targets-hook-setup.png)

![Live ExAllocatePool2 entry telemetry alongside DebugView](screenshots/10-pool-entry-live-telemetry.png)
