<#
.SYNOPSIS
    Loads JohnSmith.sys as a normal Windows kernel service, using KDU to
    temporarily override Driver Signature Enforcement (DSE).
.DESCRIPTION
    1. kdu.exe -dse 0      -> disable DSE (g_CiOptions = 0)
    2. sc create + sc start -> load JohnSmith as a normal kernel service
    3. kdu.exe -dse <orig>  -> restore DSE (always, via try/finally)
    DSE is only checked at image-load time; restoring after start is safe.
.PARAMETER Config
    JohnSmith build config. Default: Release.
.PARAMETER ServiceName
    SCM service name. Default: JohnSmith.
.PARAMETER KduConfig
    KDU build config. Default: Release.
.PARAMETER RestoreDse
    g_CiOptions restore value. 0 = auto-detect from KDU output.
.PARAMETER Provider
    Optional KDU provider id (-prv). Omit for auto-select.
.EXAMPLE
    .\tools\load-kdu.ps1
    .\tools\load-kdu.ps1 -Config Debug -Provider 1
    .\tools\load-kdu.ps1 -Config Benchmark
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'Benchmark')]
    [string] $Config = 'Release',

    [string] $ServiceName = 'JohnSmith',

    [ValidateSet('Release', 'Debug')]
    [string] $KduConfig = 'Release',

    [int] $RestoreDse = 0,

    [int] $Provider = -1
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot

$driver = Join-Path $root "build\bin\$Config\JohnSmith.sys"
if (-not (Test-Path -LiteralPath $driver)) {
    throw "Driver not found: $driver`nBuild JohnSmith first (Config=$Config)."
}

# --- locate KDU -----------------------------------------------------------
$kduExeDir = Join-Path $root "external\KDU\Source\Hamakaze\output\x64\$KduConfig"
$kduExe = Join-Path $kduExeDir 'kdu.exe'
if (-not (Test-Path -LiteralPath $kduExe)) {
    throw "kdu.exe not found: $kduExe`nBuild KDU first: external\KDU\Source\KDU.sln (Config=$KduConfig)."
}

$kduDllSrc = Join-Path $root "external\KDU\Source\Tanikaze\output\x64\$KduConfig\drv64.dll"
if (-not (Test-Path -LiteralPath $kduDllSrc)) {
    throw "drv64.dll not found: $kduDllSrc`nMake sure the Tanikaze project built."
}
$kduDll = Join-Path $kduExeDir 'drv64.dll'
Copy-Item -LiteralPath $kduDllSrc -Destination $kduDll -Force

$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "This script requires an elevated (Administrator) shell."
}

# KDU quirks:
#  1. KDU exits non-zero ("Return value: 1"), so we ignore $LASTEXITCODE.
#  2. KDU arg parser is DECIMAL only; "-dse 0x7E0" parses as 0.
#     The value KDU prints ("value: %lX") is HEX — we capture and feed back for restore.
function Invoke-KduDse {
    param([Parameter(Mandatory)][int]$Value)
    $kduArgs = @()
    if ($Provider -ge 0) { $kduArgs += @('-prv', "$Provider") }
    $kduArgs += @('-dse', "$Value")
    Write-Host "=> kdu.exe $($kduArgs -join ' ')"
    $out = & $kduExe @kduArgs
    $out | Write-Host
    $blob = ($out -join "`n")

    if ($blob -match 'value:\s*([0-9A-Fa-f]+)') {
        $script:KduReportedCurrentValue = [Convert]::ToInt32($matches[1], 16)
    }

    if ($blob -notmatch 'Write result verification succeeded') {
        throw "kdu.exe did not confirm the DSE write for value $Value (no 'verification succeeded')."
    }
}

# Remove an existing instance before invoking KDU. KDU providers may modify
# CR0.WP; doing that under JohnSmith while CR4.CET is set is architecturally
# invalid and can raise #GP before the provider restores its state.
$svcReg = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName"
$existing = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($existing -and $existing.Status -ne 'Stopped') {
    Write-Host "Stopping existing '$ServiceName' instance before KDU..."
    Stop-Service -Name $ServiceName -Force -ErrorAction Stop
    Start-Sleep -Seconds 1
}
& sc.exe delete $ServiceName 2>&1 | Out-Null
for ($i = 0; $i -lt 20; $i++) {
    if (-not (Test-Path -LiteralPath $svcReg)) { break }
    Start-Sleep -Milliseconds 500
}
if (Test-Path -LiteralPath $svcReg) {
    throw "Stale service key '$svcReg' could not be deleted. Close any open handles and re-run."
}

# --- DSE off -> load pending service -> DSE restored -> start VMX ---
$dseTouched = $false
$driverLoadedPending = $false
try {
    Invoke-KduDse 0
    $dseTouched = $true
    Write-Host "DSE disabled (g_CiOptions = 0).`n"

    $binPath = (Get-Item -LiteralPath $driver).FullName
    Write-Host "Creating service '$ServiceName' -> $binPath"
    $createArgs = @(
        $ServiceName,
        'type=', 'kernel',
        'start=', 'demand',
        "binPath=$binPath"
    )
    & sc.exe create @createArgs | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "sc.exe create failed (exit $LASTEXITCODE)." }

    # DriverEntry returns with VMX intentionally pending. This lets the KDU
    # provider restore DSE while no JohnSmith hypervisor is active.
    New-ItemProperty -LiteralPath $svcReg -Name StartRequested `
        -PropertyType DWord -Value 0 -Force | Out-Null
    New-ItemProperty -LiteralPath $svcReg -Name StartState `
        -PropertyType DWord -Value 0 -Force | Out-Null
    New-ItemProperty -LiteralPath $svcReg -Name StartStatus `
        -PropertyType DWord -Value 0 -Force | Out-Null

    Write-Host "Loading service '$ServiceName' with VMX start pending..."
    & sc.exe start $ServiceName
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "sc.exe start returned exit $LASTEXITCODE (DriverEntry may have failed)."
    }

    $driverLoadedPending = $true
}
finally {
    if ($dseTouched) {
        $target = if ($RestoreDse -gt 0) {
            $RestoreDse
        }
        elseif ($null -ne $script:KduReportedCurrentValue) {
            $script:KduReportedCurrentValue
        }
        else {
            6
        }
        Write-Host "`nRestoring DSE to $target..."
        try {
            Invoke-KduDse $target
            Write-Host "[+] DSE restored to $target." -ForegroundColor Green
        }
        catch {
            Write-Error @"
FAILED to restore DSE. System DSE is still DISABLED.
Manually fix it now:  & "$kduExe" -dse $target
$_
"@
        }
    }
}

if ($driverLoadedPending) {
    Write-Host "`nRequesting JohnSmith VMX start after DSE restoration..."
    Set-ItemProperty -LiteralPath $svcReg -Name StartRequested `
        -Type DWord -Value 1

    $state = 0
    for ($i = 0; $i -lt 300; $i++) {
        $values = Get-ItemProperty -LiteralPath $svcReg
        $state = [int]$values.StartState
        if ($state -eq 2 -or $state -eq 3 -or $state -eq 4) { break }
        Start-Sleep -Milliseconds 100
    }

    if ($state -eq 2) {
        Write-Host "[+] JohnSmith loaded, DSE restored, and VMX is running." `
            -ForegroundColor Green
    } else {
        $status = (Get-ItemProperty -LiteralPath $svcReg).StartStatus
        throw ("JohnSmith deferred start failed: state={0}, NTSTATUS=0x{1:X8}." -f `
            $state, [uint32]$status)
    }
}
