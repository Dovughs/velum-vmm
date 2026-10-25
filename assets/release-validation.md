# Release validation record

[Project](../README.md) | [Setup guide](../readmes/getting-started/README.md)

Tested on **Windows 10 22H2** and **Windows 11 26H2** with the Release driver and
console. Runtime tests covered hook activation and normal operation across both systems.

## Release files

These SHA-256 hashes identify the published binaries. Both files use optimized
Release output with no PE debug directory or COFF symbol table.

| File | SHA-256 |
| --- | --- |
| `velum.sys` | `6c7a9d33aa918ee7e2050f30a51261580209700345892211f38a298557893be3` |
| `velum-console.exe` | `c76340b9a756886636240267be2cd9ad5f49ccb866f64dc3f0c7f701d7288884` |

## Recorded demonstrations

The [capture manifest](capture-manifest.md) documents the physical Windows 11
session, its screenshots, GIFs, workloads and original binary hashes.
