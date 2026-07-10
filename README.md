# JohnSmith (Development)

> **The Swiss Army Knife of Hypervisors**

![alt text](./static/img/main.png)

JohnSmith is a versatile, multi-purpose x64 Type-2 and Type-1 hypervisor designed for red teamer.

## Features

- Synchronized all-CPU startup, rollback, and clean shutdown.
- Intel VMX/EPT and AMD SVM/NPT backends.
- Identity SLAT up to 512 GiB with explicit capability and range checks.
- Runtime 4 KiB permission changes with cross-CPU EPT/NPT invalidation.
- Transparent CPUID and capability-consistent guest instruction controls.
- VMX CR0/CR4 virtualization, VPID, AMD ASIDs, MSR and I/O bitmaps.
- One CPL0 signature-and-cookie-checked hypercall, used only for shutdown.
- One internally allocated contiguous introspection page; no external address input.
- ( Many Features Will developing )

## Requirements

- Windows 10/11 x64 on bare metal with VT-x/EPT or AMD-V/NPT enabled.
- Visual Studio 2022 with Desktop C++ and Windows Driver Kit 10.0.26100.
- Hyper-V, VBS, and other active hypervisors disabled.

## Layout

```text
include/   Common and vendor contracts
src/       Driver, lifecycle, backends, introspection
asm/       VMX/SVM entry, exit, and shutdown stubs
static/docs/ Architecture and WDK source maps
```

## Documentation

Start with [DOCUMENTATION.md](DOCUMENTATION.md). It defines the primary-source
policy, pinned manual revisions, code-to-manual map, and the distinction between
build, architecture, and hardware verification.

## Safety

This is educational kernel-mode code, not a production security boundary. Test only in a disposable, kernel-debugger-enabled environment. A defect in VM-entry, VM-exit, or teardown code can crash or corrupt the host.

## License

[MIT](LICENSE)
