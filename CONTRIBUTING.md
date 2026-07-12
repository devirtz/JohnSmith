# Contributing

Keep changes narrow, architecture-backed, and warning-free.

## Required before review

1. Build Debug, Release, and Benchmark x64.
2. Run WDK C/C++ analysis on Release.
3. Preserve synchronized all-CPU rollback and fail-stop teardown.
4. Validate VMCS/VMCB controls against capability bits.
5. Add complete handling before enabling a new intercept.
6. Add compile-time assertions for assembly-visible layouts.
7. Cite the manual revision and section for architecture changes.
8. Run the relevant bare-metal lifecycle and SLAT tests.

Use the checklist in [Build and test](docs/build-and-test.md). Performance work
must follow [Performance and measurement](docs/performance.md).

## Evidence

Hardware results must identify CPU model/stepping, Windows build, firmware and
security state, driver configuration, installed service path, SHA-256, and
measurement method. A build on one vendor is not runtime proof for the other.

## Repository hygiene

- Do not commit build output, private signing keys, or crash dumps.
- Preserve unrelated worktree changes and vendored submodule state.
- Store only public, unmodified vendor documents or open-access papers.
- Add every archived source to [Reference catalog](docs/references.md) with its
  revision, provenance, role, and SHA-256.
