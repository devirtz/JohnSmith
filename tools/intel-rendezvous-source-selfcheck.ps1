param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$hvHeader = Get-Content -Raw (Join-Path $Root 'include/hv.h')
$hvSource = Get-Content -Raw (Join-Path $Root 'src/hv.c')
$amdSource = Get-Content -Raw (Join-Path $Root 'src/amd.c')
$intelSource = Get-Content -Raw (Join-Path $Root 'src/intel.c')
$intelExitSource = Get-Content -Raw (Join-Path $Root 'src/intel/intel_exit.c')
$rendezvousSource = Get-Content -Raw `
    (Join-Path $Root 'src/intel/intel_rendezvous.c')
$architectureDoc = Get-Content -Raw `
    (Join-Path $Root 'docs/architecture/intel-vmx.md')
$designDoc = Get-Content -Raw `
    (Join-Path $Root `
        'docs/superpowers/specs/2026-07-19-intel-vmx-global-rendezvous-design.md')
$buildDoc = Get-Content -Raw (Join-Path $Root 'docs/build-and-test.md')

function Assert-Contains {
    param(
        [string]$Text,
        [string]$Needle,
        [string]$Failure
    )

    if (-not $Text.Contains($Needle)) {
        throw $Failure
    }
}

function Assert-NotContains {
    param(
        [string]$Text,
        [string]$Needle,
        [string]$Failure
    )

    if ($Text.Contains($Needle)) {
        throw $Failure
    }
}

Assert-Contains $hvHeader 'VOID (*Quiesce)(_Inout_ HV_STATE* State);' `
    'HV_BACKEND_OPS lacks the Quiesce callback.'
Assert-Contains $hvSource 'Backend->Quiesce != NULL' `
    'The backend contract does not require Quiesce.'
Assert-Contains $amdSource 'AmdQuiesce' `
    'The AMD backend lacks its no-op Quiesce callback.'
Assert-Contains $intelSource 'IntelRendezvousQuiesce' `
    'The Intel backend does not route Quiesce to rendezvous.'

$quiesceCall = $hvSource.IndexOf('State->Backend->Quiesce(State);')
$stopCall = $hvSource.IndexOf(
    'HvStopProcessorsOrFail(State, HV_FAIL_STOP_SHUTDOWN);')
if ($quiesceCall -lt 0 -or $stopCall -lt 0 -or $quiesceCall -gt $stopCall) {
    throw 'HvStop does not quiesce before changing per-CPU states.'
}

$begin = $rendezvousSource.Substring(
    $rendezvousSource.IndexOf('IntelRendezvousBegin('))
$guardWait = $begin.IndexOf('IntelRendezvousWaitForJoinGuards(')
$lifecycleRecheck = $begin.IndexOf('HV_LIFECYCLE_RUNNING', $guardWait)
$epochAdvance = $begin.IndexOf('InterlockedIncrement64(')
if ($guardWait -lt 0 -or $lifecycleRecheck -lt 0 -or
    $lifecycleRecheck -gt $epochAdvance) {
    throw 'IntelRendezvousBegin lacks the post-claim lifecycle recheck.'
}

$markerWait = $begin.IndexOf('IntelRendezvousWaitForExpectedNmis(')
$markerPublish = $begin.IndexOf('IntelRendezvousPublishExpectedNmis(')
if ($markerWait -lt 0 -or $markerPublish -lt 0 -or
    $markerWait -gt $markerPublish) {
    throw 'Expected NMI markers are not drained before publication.'
}

Assert-Contains $rendezvousSource 'IntelRendezvousQuiesce(' `
    'Intel rendezvous lacks a PASSIVE_LEVEL quiesce implementation.'

$setPrimaryStart = $intelExitSource.IndexOf('IntelSetPrimaryControl(')
$setPrimaryEnd = $intelExitSource.IndexOf(
    'IntelGuestCanTakeNmi(', $setPrimaryStart)
$setPrimary = $intelExitSource.Substring(
    $setPrimaryStart, $setPrimaryEnd - $setPrimaryStart)
Assert-Contains $setPrimary 'KeBugCheckEx(' `
    'IntelSetPrimaryControl silently ignores VMWRITE failure.'

Assert-NotContains $architectureDoc `
    'Release and Benchmark configurations can use the assembly fast path' `
    'Intel architecture documentation still claims a Release CPUID fast path.'
Assert-Contains $architectureDoc `
    'Only Benchmark enables the guarded assembly VMCALL fast path.' `
    'Intel architecture documentation lacks the Benchmark-only VMCALL boundary.'
Assert-Contains $architectureDoc `
    'Preparation, VMCS apply coordination, the release lead, and final resume overhead remain guest-visible.' `
    'Intel architecture documentation overstates TSC compensation.'
Assert-Contains $architectureDoc `
    'outstanding expected-NMI markers are consumed before VMXOFF' `
    'Intel architecture documentation lacks lifecycle marker drain.'
Assert-Contains $designDoc `
    'Hardware validation of that assumption remains required.' `
    'The approved first-NMI assumption lacks a hardware-validation boundary.'
Assert-NotContains $buildDoc '| Release | Disabled | Enabled | Disabled |' `
    'The build matrix still claims a Release CPUID fast path.'
