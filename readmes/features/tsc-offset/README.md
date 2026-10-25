# Fixed TSC offset

[Documentation index](../../README.md) | [Project](../../../README.md)


## Configuration walkthrough

Dashboard **[5] TSC offset** opens a draft:

1. **[1] Set signed tick value**: enter `1000`.
2. **[2] Toggle fixed offset ON/OFF**: make the draft ON.
3. **[4] Apply TSC on all CPUs**. Verify the currently-applied state.
4. Return and reopen the page to check it survived menu navigation.
5. **[3] Reset TSC draft -> [4] Apply** restores OFF and zero configured ticks.

Back discards unapplied edits. Toggling OFF and applying makes the effective
offset zero but retains the configured number. Reset also clears that number.
TSC Apply/Reset leaves CPUID rules unchanged. The walkthrough verifies that the
configuration is applied, retained and reset.

## Units and clock behavior

Values are signed decimal or hexadecimal, in **TSC ticks**, not milliseconds.
Range: -1,000,000,000..+1,000,000,000. Defaults are OFF and zero. Applied values
survive reconnect, reset on driver reload, and are not stored in the profile.

An unchanged offset cancels from elapsed-time differences:

```text
(end + offset) - (start + offset) = end - start
```

It does not shorten CPUID execution or compensate for VM-exit overhead. There
is no automatic per-exit timing adjustment. Capability-dependent scaling is
initialized with an identity multiplier and is not changed by this control.

Timestamp reads use VMCS hardware controls. Supported IA32_TSC_DEADLINE accesses
are intercepted only while the effective offset is nonzero, because their timer
values need separate translation. Zero disarms; expired/overflowing translations
saturate. An armed deadline is rebased on changes and shutdown. Apply is serialized
across CPUs; an unrecoverable rollback stops instead of resuming inconsistent state.

## Clock transitions

A live offset change introduces a clock discontinuity. Deadline translation
does not guarantee QPC monotonicity or synchronization with other time sources.
Use small values in a disposable VM and check idle/timer-heavy activity, positive
and negative offsets, OFF, reconnect and shutdown.

Implementation: [TSC state](../../../hypervisor/src/tsc.c),
[configuration](../../../shared/processor_config.h),
[console](../../../user-mode/console/processor.c).
Hardware reference: [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

## Recorded example

These captures show the physical Windows 11 test session.
See the [session manifest](../../../assets/capture-manifest.md) for environment and reproduction details.

### TSC offset configuration

A three-state configuration sequence: draft ON/100 ticks, applied ON/100, then reset OFF/0.

![TSC offset configuration](../../../assets/screenshots/07-tsc-configuration.png)
