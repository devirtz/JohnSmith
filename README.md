# JohnSmith (Development)

> **The Swiss Army Knife of Hypervisors**

![banner](./static/img/main.png)

JohnSmith is a versatile, multi-purpose x64 hypervisor designed for red teamer.

## Features

- Synchronized all-CPU startup, rollback, and clean shutdown.
- Intel VMX/EPT and AMD SVM/NPT backends.
- Identity SLAT up to 512 GiB with explicit capability and range checks.
- Runtime 4 KiB permission changes with cross-CPU EPT/NPT invalidation.
- Transparent CPUID passthrough with explicit nested-virtualization rejection.
- VMX CR0/CR4 virtualization, VPID, AMD ASIDs, MSR and I/O bitmaps.
- One CPL0 signature-and-cookie-checked hypercall, used only for shutdown.
- One internally allocated contiguous introspection page; no external address input.

## Requirements

- Windows 10 version 2004 or newer, or Windows 11 x64, on bare metal with
  VT-x/EPT or AMD-V/NPT enabled in firmware.
- Visual Studio 2022 with Desktop C++ and Windows Driver Kit 10.0.26100.
- Hyper-V, VBS, and other active hypervisors disabled.
- Intel `IA32_S_CET` must be zero, AMD CET
  must be disabled. ( Root CET/SSP state virtualization is not implemented. )

## Limits

- CPUID is passed through unchanged; nested virtualization is not implemented.
- Guest VMX/SVM instructions receive `#UD`, except for the checked CPL0 stop call.
- Processor and physical-memory hot-add/remove are unsupported while active.


## Documentation

Start with [DOCUMENTATION.md](DOCUMENTATION.md). It defines the primary-source
policy, pinned manual revisions, code-to-manual map, and the distinction between
build, architecture, and hardware verification.

## Safety

This is educational kernel-mode code, not a production security boundary. Test only in a disposable, kernel-debugger-enabled environment. A defect in VM-entry, VM-exit, or teardown code can crash or corrupt the host.

## Loading (with KDU)

JohnSmith is an unsigned WDM driver. For testing on a box with Secure Boot /
VBS / HVCI off, you can load it as a **normal SCM service** with
[KDU](https://github.com/hfiref0x/KDU) temporarily overriding Driver Signature
Enforcement. 

```pwsh
git clone https://github.com/hfiref0x/KDU.git external/KDU

# build JohnSmith.sys (Release or Debug)
d                 # DSE off > service load > DSE restored
.\tools\unload-kdu.ps1               # stop + remove the service
```

`load-kdu.ps1` runs `kdu.exe -dse 0`, creates and starts the `JohnSmith`
service via `sc.exe`, then restores DSE to `6` in a `try/finally` so DSE is
always restored even on failure. See `tools/check.ps1` to confirm the box is in
a KDU-compatible state first.

## License

[MIT](LICENSE)
