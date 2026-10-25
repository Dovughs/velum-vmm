# Control console

[Project](../../README.md) | [Setup](../../readmes/getting-started/README.md) | [Feature guides](../../readmes/README.md)

The administrative console controls driver lifecycle, EPT hooks, live telemetry,
request policies, CPUID identity rules and TSC offsets.

- `main.c`: dashboard, command dispatch and application lifecycle.
- `service.c`: driver installation, status and coordinated removal.
- `monitor.c`: hook menus, filters, policies and live event display.
- `input.c`: validated input and symbolic field values.
- `processor.c`, `cpuid.c`: processor configuration and live identity queries.
- `app.h`: console state and internal interfaces.

Run the matching `velum-console.exe` as administrator after loading the driver.
Use `velum-console.exe status` for status and `velum-console.exe remove` for the
shutdown and service-removal flow. Closing the viewer leaves active hooks installed.
