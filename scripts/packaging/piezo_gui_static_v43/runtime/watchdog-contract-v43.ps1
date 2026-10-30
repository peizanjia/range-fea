$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'contract-common-v43.ps1')
. (Join-Path $PSScriptRoot 'supervisor-core-v43.ps1')

function Assert-V43WatchdogSupervisorResult($Result, [string]$CandidateIdentitySha256) {
    if ($CandidateIdentitySha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'v43 watchdog candidate identity is not a SHA256 pin.' }
    if ($null -eq $Result -or -not ($Result.success -is [bool]) -or $Result.success -ne $true -or
        -not ($Result.status -is [string]) -or $Result.status -cne 'FIXTURE_COMPLETE' -or
        -not ($Result.candidateManifestIdentitySha256 -is [string]) -or
        $Result.candidateManifestIdentitySha256 -cne $CandidateIdentitySha256.ToLowerInvariant() -or
        -not ($Result.goPermitted -is [bool]) -or $Result.goPermitted -ne $false) {
        throw 'v43 watchdog rejected an unsuccessful, unbound, or GO-permitting supervisor result.'
    }
    foreach ($name in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        $action = $Result.productActions.$name
        if (-not ($action -is [bool]) -or $action -ne $false) { throw "v43 watchdog result permits product action $name." }
    }
    return $Result
}

function Invoke-V43StaticFixtureWatchdog {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$CandidateManifestPath,
        [Parameter(Mandatory = $true)][string]$CandidateManifestSha256,
        [Parameter(Mandatory = $true)][string]$CandidateManifestIdentitySha256,
        [Parameter(Mandatory = $true)][string]$FixtureEvidenceRoot,
        [int]$TimeoutSeconds = 20
    )
    $result = Invoke-V43StaticFixtureSupervisor -CandidateManifestPath $CandidateManifestPath `
        -CandidateManifestSha256 $CandidateManifestSha256 `
        -CandidateManifestIdentitySha256 $CandidateManifestIdentitySha256 `
        -FixtureEvidenceRoot $FixtureEvidenceRoot -TimeoutSeconds $TimeoutSeconds
    return Assert-V43WatchdogSupervisorResult $result $CandidateManifestIdentitySha256
}
