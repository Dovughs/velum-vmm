# Velum documentation

[Project overview](../README.md) | [Build and VM setup](getting-started/README.md)

Use these guides to configure entry observation, build focused log views, apply
request policies and run CPUID/TSC experiments. Each target guide includes its
input schema and a controlled workload.

Guides describe the current implementation. Export availability is checked at
runtime; an entry in this index does not guarantee a selectable target on every
Windows build. Target IDs are protocol identities, not syscall service numbers.
The examples below are procedures to run, not fabricated execution transcripts.

## Technical article

[Velum: preserving Windows execution across VMX transitions](technical-writeup/README.md)
explains the architecture, source-level transitions, debugger evidence and validation limits.

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
| [NtCreateFile](syscalls/NtCreateFile/README.md) | Yes, if available and capture eligible | Yes |
| [NtAllocateVirtualMemory](syscalls/NtAllocateVirtualMemory/README.md) | Yes, if available and capture eligible | No |
| [NtOpenFile](syscalls/NtOpenFile/README.md) | Yes, if available and capture eligible | Yes |
| [NtDeleteFile](syscalls/NtDeleteFile/README.md) | Yes, if available and capture eligible | No |
| [NtSetInformationFile](syscalls/NtSetInformationFile/README.md) | Yes, if available and capture eligible | No |
| [NtWriteFile](syscalls/NtWriteFile/README.md) | Yes, if available and capture eligible | No |
| [NtCreateSection](syscalls/NtCreateSection/README.md) | Yes, if available and capture eligible | Yes |
| [NtMapViewOfSection](syscalls/NtMapViewOfSection/README.md) | Yes, if available and capture eligible | No |
| [NtOpenProcess](syscalls/NtOpenProcess/README.md) | Yes, if available and capture eligible | Yes |
| [NtProtectVirtualMemory](syscalls/NtProtectVirtualMemory/README.md) | Yes, if available and capture eligible | No |
| [NtReadFile](syscalls/NtReadFile/README.md) | Yes, if available and capture eligible | No |
| [NtDuplicateObject](syscalls/NtDuplicateObject/README.md) | Yes, if available and capture eligible | Yes |

NtProtectVirtualMemory was unavailable on the inspected host. NtWriteVirtualMemory
and NtCreateThreadEx are retired protocol slots, not supported features;
NtReadVirtualMemory and process creation are not implemented.

## Kernel routine guides

All are observation-only; no deny or argument-modification policy.

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
