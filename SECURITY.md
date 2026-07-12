# Security policy

JohnSmith is experimental kernel and virtualization code. It is not hardened
for hostile guests or production deployment.

## Reporting

Report vulnerabilities privately through GitHub Security Advisories. Include:

- affected CPU vendor/model and Windows build;
- firmware, Hyper-V, VBS, HVCI, and CET state;
- driver configuration, service path, and SHA-256;
- reproduction steps and expected/actual result;
- VM-exit reason, qualification, and bugcheck parameters when available.

Do not publish working kernel exploitation details before a fix is available.
Never attach private signing keys or crash dumps containing secrets.

## Supported security boundary

The project is intended for controlled research. It does not claim isolation
against a malicious guest, side-channel resistance, nested-virtualization
security, supervisor-CET support, or production-grade device isolation.
