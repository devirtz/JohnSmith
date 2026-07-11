# Contributing

Keep changes small, architecture-documented, and warning-free.

Before opening a pull request:

1. Build Debug and Release for x64.
2. Preserve synchronized all-CPU rollback and fail-stop teardown.
3. Validate every VMCS/VMCB control against its capability bits.
4. Add a handler before enabling a new VM-exit intercept.
5. Do not commit binaries, signing keys, or build output. Documentation
   snapshots must be public vendor originals with their revision recorded.

Hardware testing must state the CPU model, Windows version, enabled security features, and signing mode.
