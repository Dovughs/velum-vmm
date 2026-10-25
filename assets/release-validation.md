# Release validation record

[Project](../README.md) | [Setup guide](../readmes/getting-started/README.md)

On 25 October 2026, the operator reported successful runtime testing on
**Windows 10 22H2** and **Windows 11 26H2**. Version names are recorded as supplied
by the operator. Exact OS build numbers were not included in this follow-up.

## Prepared release files

The following hashes identify the current local Release files copied into the
publication staging folder on 25 October. They are packaging identifiers; the
operator's runtime report did not independently record loaded-file hashes.
Both files passed a PE inspection for absent debug directories and COFF symbols.

| File | SHA-256 |
| --- | --- |
| `velum.sys` | `6c7a9d33aa918ee7e2050f30a51261580209700345892211f38a298557893be3` |
| `velum-console.exe` | `c76340b9a756886636240267be2cd9ad5f49ccb866f64dc3f0c7f701d7288884` |

## Earlier recorded demonstrations

The [capture manifest](capture-manifest.md) preserves the separate 24 October
session, its screenshots, GIFs and original binary hashes. The newer runtime
confirmation does not change the provenance of those captures.
