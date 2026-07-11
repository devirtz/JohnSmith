<#
.SYNOPSIS
    Loads JohnSmith.sys as a normal Windows kernel service, using KDU to
    temporarily override Driver Signature Enforcement (DSE).

.DESCRIPTION
    This does NOT use KDU manual mapping (-map / shellcode). JohnSmith is a
    full WDM driver (real DriverEntry parameters, SEH tables, multiple kernel
    imports) and cannot run as "driverless" shellcode. Instead it is loaded as
    a standard SCM service:

      1. kdu.exe -dse 0   -> temporarily disable DSE (g_CiOptions = 0)
      2. sc create + sc start JohnSmith as a normal kernel service
      3. kdu.exe -dse 6   -> restore DSE to the Windows 10/11 default

    DSE is only consulted at image-load time, so restoring it to 6 AFTER a
    successful start does not affect the already-resident driver.

    Step 3 runs in a try/finally so DSE is ALWAYS restored, even if service
    creation or start fails. Leaving DSE disabled would be a security hole, so
    the restore is the one thing that must never be skipped.

    Prerequisites:
      - Built driver:  build\bin\<Config>\JohnSmith.sys
      - Built KDU:     external\KDU\Source\Hamakaze\output\x64\<KduConfig>\kdu.exe
                       with drv64.dll next to it
      - Elevated (Administrator) PowerShell
      - Secure Boot / VBS / HVCI off (KDU cannot patch CI under HVCI; run
        tools\check.ps1 to verify)

.PARAMETER Config
    JohnSmith build configuration to load. Default: Release.

.PARAMETER ServiceName
    SCM service name. Default: JohnSmith.

.PARAMETER KduConfig
    KDU build configuration to use. Default: Release.

.PARAMETER RestoreDse
    Value written to g_CiOptions when restoring. Default 6 (Win10/11 normal:
    signature required + page hash). If your box uses a non-default policy
    (e.g. test-signing on = 0x8, or 0xE), pass your original value here.

.PARAMETER Provider
    Optional KDU provider id (-prv). Omit to let KDU auto-select.

.EXAMPLE
    .\tools\load-kdu.ps1
.EXAMPLE
    .\tools\load-kdu.ps1 -Config Debug -Provider 1
.EXAMPLE
    .\tools\load-kdu.ps1 -RestoreDse 0xE   # box had test-signing enabled
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string] $Config = 'Release',

    [string] $ServiceName = 'JohnSmith',

    [ValidateSet('Release', 'Debug')]
    [string] $KduConfig = 'Release',

    [int] $RestoreDse = 6,

    [int] $Provider = -1
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot   # repo root (parent of tools\)

# --- locate the driver ----------------------------------------------------
$driver = Join-Path $root "build\bin\$Config\JohnSmith.sys"
if (-not (Test-Path -LiteralPath $driver)) {
    throw "Driver not found: $driver`nBuild JohnSmith first (Config=$Config)."
}

# --- locate KDU -----------------------------------------------------------
$kduDir = Join-Path $root "external\KDU\Source\Hamakaze\output\x64\$KduConfig"
$kduExe = Join-Path $kduDir 'kdu.exe'
$kduDll = Join-Path $kduDir 'drv64.dll'
if (-not (Test-Path -LiteralPath $kduExe)) {
    throw "kdu.exe not found: $kduExe`nBuild KDU first: external\KDU\Source\KDU.sln (Config=$KduConfig)."
}
if (-not (Test-Path -LiteralPath $kduDll)) {
    throw "drv64.dll not found next to kdu.exe in $kduDir."
}

# --- require elevation (KDU + sc create both need it) ---------------------
$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "KDU and service creation require an elevated (Administrator) shell."
}

# --- run kdu.exe with a -dse value; throw on non-zero exit ----------------
function Invoke-KduDse {
    param([Parameter(Mandatory)][int]$Value)
    $kduArgs = @()
    if ($Provider -ge 0) { $kduArgs += @('-prv', "$Provider") }
    $kduArgs += @('-dse', "$Value")
    Write-Host "=> kdu.exe $($kduArgs -join ' ')"
    & $kduExe @kduArgs
    if ($LASTEXITCODE -ne 0) {
        throw "kdu.exe exited $LASTEXITCODE while setting DSE=$Value."
    }
}

# --- main workflow: DSE off -> service load -> DSE restored in finally ----
$dseTouched = $false
try {
    # 1) disable DSE so CI accepts the unsigned image at load time
    Invoke-KduDse 0
    $dseTouched = $true
    Write-Host "DSE disabled (g_CiOptions = 0).`n"

    # 2) remove any stale service entry first so create is clean
    if (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) {
        Write-Host "Removing stale service '$ServiceName'..."
        Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
        & sc.exe delete $ServiceName | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "sc.exe delete failed (exit $LASTEXITCODE)." }
        Start-Sleep -Seconds 1
    }

    # sc.exe wants the ImagePath quoted; this form survives spaces in the path
    $binPath = (Get-Item -LiteralPath $driver).FullName
    Write-Host "Creating service '$ServiceName' -> $binPath"
    & sc.exe create $ServiceName type= kernel start= demand binPath= "`"$binPath`"" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "sc.exe create failed (exit $LASTEXITCODE)." }

    Write-Host "Starting service '$ServiceName'..."
    & sc.exe start $ServiceName
    $startExit = $LASTEXITCODE
    if ($startExit -ne 0) {
        Write-Warning "sc.exe start returned exit code $startExit (DriverEntry may have failed)."
    }

    Start-Sleep -Milliseconds 500
    $svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($svc -and $svc.Status -eq 'Running') {
        Write-Host "`n[+] JohnSmith loaded and running as '$ServiceName'." -ForegroundColor Green
    }
    else {
        $state = if ($svc) { $svc.Status } else { '<gone>' }
        Write-Warning "Service state is '$state'. DriverEntry may have failed; check the kernel debugger."
    }
}
finally {
    # 3) ALWAYS restore DSE if we disabled it. Never leave it at 0.
    if ($dseTouched) {
        Write-Host "`nRestoring DSE to $RestoreDse..."
        try {
            Invoke-KduDse $RestoreDse
            Write-Host "[+] DSE restored." -ForegroundColor Green
        }
        catch {
            Write-Error @"
FAILED to restore DSE. System DSE is still DISABLED.
Manually fix it now:  & "$kduExe" -dse $RestoreDse
$_
"@
        }
    }
}
