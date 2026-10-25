# Shared interfaces

[Project](../README.md) | [Usage guides](../readmes/README.md)

These headers define the contracts shared by the kernel runtime and console.

- `control.h`, `monitor_protocol.h`: versioned control requests and event messages.
- `target_entries.h`: target identities and address-based export deduplication.
- `telemetry_fields.h`, `telemetry_symbols.h`: input fields and symbolic values.
- `telemetry_match.h`, `telemetry_policy.h`: filtering and request-policy validation.
- `kernel_telemetry.h`: Ex/Mm input schemas and decoding.
- `processor_config.h`: CPUID identity rules and TSC configuration.

Protocol target IDs identify Velum entries independently of Windows syscall
service numbers. The driver and console use the same definitions for field
interpretation, configuration validation and event delivery.
