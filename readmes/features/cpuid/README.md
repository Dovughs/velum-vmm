# Runtime CPUID identity rules

[Documentation index](../../README.md) | [Project](../../../README.md)


## Menu and first experiment

Dashboard **[4] CPUID** opens a draft. **[1]** toggles overrides, **[2]** edits
hypervisor vendor, **[3]** edits CPU brand, **[4]** adds/replaces a register rule,
**[5]** removes a rule, **[6]** queries live CPUID, **[7]** resets the draft, and
**[8]** applies CPUID settings on all CPUs. Back discards unapplied edits.

Enter `VelumLab` in [2], keep overrides ON, apply [8], then query [6] with leaf
`0x40000000`, subleaf `0`. Capture the applied state and returned registers.
Reset [7] and apply [8] afterwards. A query before Apply shows the old settings.

## Rule contract

There are at most 16 exact leaf/subleaf rules. A register mask selects complete
32-bit registers: EAX=1, EBX=2, ECX=4, EDX=8. Unselected registers pass through.
Editable areas are leaf 0 vendor registers, brand leaves 0x80000002..0x80000004
at subleaf 0, and hypervisor leaves 0x40000000..0x400000FF. The control leaf is
not editable. Hardware feature, topology, cache, address-width and frequency
leaves cannot be edited by this UI.

The vendor accepts 12 ASCII bytes, the brand 48; shorter input is zero-padded.
`""` writes an empty identity. Vendor register order is EBX, EDX, ECX. String
editors and raw rules use the same table, so editing a string can replace rules
for its leaves. No missing CPU instruction support is synthesized.

## Lifetime and verification

Defaults enable Velum vendor/interface overrides. OFF passes the editable
identity queries through; existing leaf-1 hypervisor/VMX-bit handling remains.
Rules configure live CPUID identity responses. OS-cached strings may retain
their earlier values. Dashboard Diagnostics checks
applied rules across CPUs; an arbitrary single-thread query only observes its
current CPU.

Applied settings survive console reconnect, reset on driver reload, and are not
stored in the filter profile. CPUID Apply/Reset leaves TSC settings unchanged.
Changing VMware's boot-time `hypervisor.cpuid.v0` is a separate operation.

Implementation: [console](../../../user-mode/console/processor.c),
[rule contract](../../../shared/processor_config.h),
[CPUID handler](../../../hypervisor/src/vmexit.c).

## Recorded example

These captures show the physical Windows 11 test session.
See the [session manifest](../../../assets/capture-manifest.md) for environment and reproduction details.

### CPUID apply and reset

A live vendor query changes to VelumLabDemo and returns to the Velum default after Reset/Apply. Reset does not turn overrides OFF. The GIF presents the captured states in chronological order.

![CPUID apply and reset](../../../assets/demos/06-cpuid-apply-reset.gif)
