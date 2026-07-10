# Architecture documentation hub

These notes are navigation aids, not replacements for vendor manuals. Follow
the links to the current primary document and verify the cited heading before
changing code.

## Guides

- [Intel VMX/EPT](intel-vmx.md)
- [AMD SVM/NPT](amd-svm.md)
- [SLAT and invalidation](slat-and-invalidation.md)
- [Windows kernel and x64 ABI](windows-kernel.md)
- [Extended reading list](reading-list.md)
- [Verification checklist](verification-checklist.md)
- [Repository-wide evidence policy](../../DOCUMENTATION.md)

## Local manual cache

PDFs in this directory are deliberately ignored by Git. This avoids copying
large vendor documents into the repository and prevents a stale cache from
becoming the apparent source of truth. Download from the official landing pages
listed in `DOCUMENTATION.md`.

The files present in the working copy on 2026-07-10 had these SHA-256 hashes:

| Local file | SHA-256 | Status |
| --- | --- | --- |
| `24593_3.44_APM_Vol2.pdf` | `465454E3E7761B126075C789B4BF8B73190F8284637BE604D053252DAB6C11FD` | Filename claims AMD 24593 rev. 3.44; verify the embedded cover |
| `325462-092-sdm-vol-1-2abcd-3abcd-4.pdf` | `16A9336104750613AE2F2BAB6EB7A1B21A7E1EF60CED35E9AB2E0D8C7EFCEC68` | Filename claims Intel SDM version 092; verify the embedded cover |
| `326019-sdm-vol-3c.pdf` | `961FA795D2E504082C475643813E05CC7F72B6A3741B179234C711B0BCC50A2B` | Revision not encoded in the filename; do not cite without checking |

Checksums identify a file; they do not prove that the file is current or that
an implementation correctly applies it.

## How to cite a constant

Good:

```c
/* Intel SDM 325462-092, Vol. 3C, "VMXON Region": revision ID is bits 30:0. */
#define VMX_REVISION_MASK 0x7fffffffu
```

Bad:

```c
/* common VMX value */
#define VMX_REVISION_MASK 0x7fffffffu
```

Use a section or table title because page numbers move between revisions.
