<#
.SYNOPSIS
    Stops and removes the JohnSmith kernel service created by load-kdu.ps1.

.DESCRIPTION
    No DSE change is needed here: Driver Signature Enforcement is only checked
    at image-load time. Unloading a resident driver never triggers CI, so KDU
    is not involved. This simply stops and deletes the SCM service.

.PARAMETER ServiceName
    SCM service name. Default: JohnSmith.

.EXAMPLE
    .\tools\unload-kdu.ps1
#>
[CmdletBinding()]
param(
    [string] $ServiceName = 'JohnSmith'
)

$ErrorActionPreference = 'Stop'

$svc = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if (-not $svc) {
    Write-Host "Service '$ServiceName' is not installed. Nothing to do."
    return
}

if ($svc.Status -ne 'Stopped') {
    Write-Host "Stopping service '$ServiceName' (current: $($svc.Status))..."
    Stop-Service -Name $ServiceName -Force -ErrorAction Stop
    Start-Sleep -Seconds 1
}

Write-Host "Deleting service '$ServiceName'..."
& sc.exe delete $ServiceName | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "sc.exe delete failed (exit $LASTEXITCODE)."
}

Write-Host "[+] '$ServiceName' stopped and removed." -ForegroundColor Green
