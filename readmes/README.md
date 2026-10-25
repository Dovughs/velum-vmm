# Velum documentation

[Project overview](../README.md) | [Build and VM setup](getting-started/README.md)

Use these guides to configure entry observation, build focused log views, apply
request policies and run CPUID/TSC experiments. Each target guide includes its
input schema and a controlled workload.

Each guide lists the available fields, menu steps and policy behavior. The console
checks exports at runtime and presents one target per unique entry address.
Target IDs identify the shared protocol; Windows syscall service numbers are separate.

## Technical article

[Velum: preserving Windows execution across VMX transitions](technical-writeup/README.md)
explains the architecture, source-level transitions, debugging sessions and runtime results.

## Start here

1. [Build, F7 loading, VMware and Windows setup](getting-started/README.md).
2. Enable one hook and observe a controlled baseline before creating a policy.
3. Follow its guide, compare OFF/ON/OFF, and restore the test state.
4. [Architecture and compatibility investigations](engineering/README.md).
5. [Runtime screenshot/GIF gallery](../assets/README.md).

## Feature guides

- [Hooks and live logging](features/logging/README.md)
- [Filters and symbolic input](features/filters/README.md)
- [Scoped policies](features/policies/README.md)
- [Protected-file list](features/protected-files/README.md)
- [Kernel telemetry summary](features/kernel-summary/README.md)
- [Profiles and lifetime](features/profiles/README.md)
- [CPUID settings](features/cpuid/README.md)
- [Fixed TSC offset](features/tsc-offset/README.md)
- [Diagnostics and shutdown](features/diagnostics/README.md)

## Nt target guides

| Target | Scoped deny | Access reduction |
| --- | --- | --- |
| [NtCreateFile](syscalls/NtCreateFile/README.md) | Yes | Yes |
| [NtAllocateVirtualMemory](syscalls/NtAllocateVirtualMemory/README.md) | Yes | No |
| [NtOpenFile](syscalls/NtOpenFile/README.md) | Yes | Yes |
| [NtDeleteFile](syscalls/NtDeleteFile/README.md) | Yes | No |
| [NtSetInformationFile](syscalls/NtSetInformationFile/README.md) | Yes | No |
| [NtWriteFile](syscalls/NtWriteFile/README.md) | Yes | No |
| [NtCreateSection](syscalls/NtCreateSection/README.md) | Yes | Yes |
| [NtMapViewOfSection](syscalls/NtMapViewOfSection/README.md) | Yes | No |
| [NtOpenProcess](syscalls/NtOpenProcess/README.md) | Yes | Yes |
| [NtProtectVirtualMemory](syscalls/NtProtectVirtualMemory/README.md) | Yes | No |
| [NtReadFile](syscalls/NtReadFile/README.md) | Yes | No |
| [NtDuplicateObject](syscalls/NtDuplicateObject/README.md) | Yes | Yes |

The table describes policies for available targets with eligible captured inputs.
NtProtectVirtualMemory availability depends on the running kernel's exports.

## Kernel routine guides

These targets provide kernel-entry telemetry, numeric filters and module/tag summaries.

- [ExAllocatePool2](kernel-routines/ExAllocatePool2/README.md)
- [ExAllocatePoolWithTag](kernel-routines/ExAllocatePoolWithTag/README.md)
- [ExFreePoolWithTag](kernel-routines/ExFreePoolWithTag/README.md)
- [ExAllocatePoolWithTagPriority](kernel-routines/ExAllocatePoolWithTagPriority/README.md)
- [ExAllocatePool3](kernel-routines/ExAllocatePool3/README.md)
- [ExAllocatePool](kernel-routines/ExAllocatePool/README.md)
- [ExFreePool](kernel-routines/ExFreePool/README.md)
- [MmAllocateContiguousMemory](kernel-routines/MmAllocateContiguousMemory/README.md)
- [MmFreeContiguousMemory](kernel-routines/MmFreeContiguousMemory/README.md)
- [MmMapIoSpaceEx](kernel-routines/MmMapIoSpaceEx/README.md)
- [MmUnmapIoSpace](kernel-routines/MmUnmapIoSpace/README.md)
- [MmCopyVirtualMemory](kernel-routines/MmCopyVirtualMemory/README.md)
