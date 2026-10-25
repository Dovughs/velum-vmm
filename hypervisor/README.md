# Hypervisor runtime

[Project](../README.md) | [Architecture and implementation](../readmes/technical-writeup/README.md)

The kernel runtime brings Windows into VMX non-root operation and manages the
per-CPU VMCS, EPT, hook and native-return state. Entry observation uses EPT shadow
mappings with hardware replay; guest helpers capture inputs and evaluate policies.

- `src/driver.c`, `src/percpu.c`: initialization, state allocation and diagnostics.
- `src/vmcs.c`, `src/vmx.asm`: architectural state and C/assembly transitions.
- `src/ept.c`, `src/hook.c`, `src/vpid.c`: mappings, hook preparation and invalidation.
- `src/vmexit.c`: dispatch, replay retirement and interrupted-event handling.
- `src/filter.c`, `src/kernel_telemetry.c`: guest capture, policies and telemetry.
- `src/monitor.c`, `src/syscall.c`: console control, target resolution and hook coordination.
- `src/processor.c`, `src/tsc.c`: cross-CPU configuration and timestamp domains.
- `src/persist.c`, `src/native.c`: persistent execution and coordinated native return.

Headers in `include/` define the runtime interfaces and per-CPU state layout.
The [build guide](../readmes/getting-started/README.md#3-build) covers toolchain selection.
