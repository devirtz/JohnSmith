# Virtualization reading list

Read the mandatory sources before editing the corresponding backend. Items
marked contextual explain adjacent systems but are not authority for JohnSmith
VMCS/VMCB values.

## Intel

- **Mandatory:** [Intel 64 and IA-32 SDM landing
  page](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
  Use Volume 3C for VMX, EPT, VM entry/exit and the VMX instruction reference;
  use Volume 2 for general instruction semantics and Volume 4 for MSRs.
- **Mandatory when CPUID behavior changes:** the SDM CPUID instruction entry and
  relevant feature-leaf definitions.
- **Supplemental:** [Intel CPUID passthrough virtualization considerations,
  document 356709-003US revision 3.0](https://cdrdv2.intel.com/v1/dl/getContent/864718).

## AMD

- **Mandatory:** [AMD APM Volume 2, System
  Programming](https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2), especially
  Chapter 15 and Appendices B/C.
- **Mandatory for assembly/instructions:** [AMD APM Volume
  3](https://docs.amd.com/v/u/en-US/24594_3.37).
- **Convenient full set:** [AMD APM Volumes
  1-5](https://docs.amd.com/v/u/en-US/40332_4.09_APM_PUB).

## Windows and toolchain

- **Mandatory:** [Windows driver API
  reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/).
- **Mandatory for assembly:** [Microsoft x64 calling
  convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170).
- **Testing:** [Driver
  Verifier](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/driver-verifier)
  and [setting up kernel-mode
  debugging](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/setting-up-kernel-mode-debugging-in-windbg--cdb--or-ntsd).
- **Release:** [Windows driver signing
  tutorial](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/windows-driver-signing-tutorial).

## Context only

- [Microsoft Hypervisor Top-Level Functional
  Specification](https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/tlfs)
  describes the Hyper-V interface. JohnSmith rejects an already active
  hypervisor, so the TLFS does not define its bare-metal VMX/SVM behavior.

Do not add a source merely to make the list longer. Add it only when a code path
depends on a claim that the source can verify.
