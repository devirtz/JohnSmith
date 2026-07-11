$dg = Get-CimInstance `
    -Namespace root\Microsoft\Windows\DeviceGuard `
    -ClassName Win32_DeviceGuard `
    -ErrorAction SilentlyContinue

$hv = Get-WindowsOptionalFeature `
    -Online `
    -FeatureName Microsoft-Hyper-V-All `
    -ErrorAction SilentlyContinue

$boot = bcdedit /enum '{current}' |
    Select-String 'hypervisorlaunchtype'

$hvci = Get-ItemProperty `
    'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity' `
    -ErrorAction SilentlyContinue

[pscustomobject]@{
    HyperVFeature          = if ($hv) { $hv.State } else { 'NotPresent' }
    HypervisorLaunch       = if ($boot) { ($boot -split '\s+')[-1] } else { 'Default' }
    HypervisorPresent      = (Get-CimInstance Win32_ComputerSystem).HypervisorPresent
    VBSStatus              = $dg.VirtualizationBasedSecurityStatus
    SecurityServicesRunning = ($dg.SecurityServicesRunning -join ',')
    MemoryIntegrityRunning = [bool]($dg.SecurityServicesRunning -contains 2)
    MemoryIntegrityRegistry = if ($null -ne $hvci.Enabled) { $hvci.Enabled } else { 'NotSet' }
}