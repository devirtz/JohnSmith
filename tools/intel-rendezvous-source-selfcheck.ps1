param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$hvHeader = Get-Content -Raw (Join-Path $Root 'include/hv.h')
$hvSource = Get-Content -Raw (Join-Path $Root 'src/hv.c')
$amdSource = Get-Content -Raw (Join-Path $Root 'src/amd.c')
$intelSource = Get-Content -Raw (Join-Path $Root 'src/intel.c')
$rendezvousSource = Get-Content -Raw `
    (Join-Path $Root 'src/intel/intel_rendezvous.c')

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
