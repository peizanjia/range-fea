[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CandidateManifestPath,
    [Parameter(Mandatory = $true)][string]$CandidateManifestSha256,
    [Parameter(Mandatory = $true)][string]$CandidateManifestIdentitySha256,
    [Parameter(Mandatory = $true)][string]$EvidenceRoot,
    [Parameter(Mandatory = $true)][string]$SupervisorReadyPath,
    [Parameter(Mandatory = $true)][string]$SupervisorTerminalPath,
    [Parameter(Mandatory = $true)][string]$SupervisorToken,
    [Parameter(Mandatory = $true)][int]$SupervisorPid,
    [int]$TimeoutSeconds = 15
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$common = Join-Path $PSScriptRoot 'contract-common-v43.ps1'
. $common

try {
    $null = Assert-V43CandidateManifest $CandidateManifestPath $CandidateManifestSha256 $CandidateManifestIdentitySha256
    $root = [IO.Path]::GetFullPath($EvidenceRoot)
    $ready = [IO.Path]::GetFullPath($SupervisorReadyPath)
    $terminal = [IO.Path]::GetFullPath($SupervisorTerminalPath)
    if (-not [IO.Directory]::Exists($root) -or
        -not (Test-V43PathUnderRoot $ready $root) -or -not (Test-V43PathUnderRoot $terminal $root)) {
        throw 'Worker handshake paths must remain below the isolated v43 fixture evidence root.'
    }
    if ($SupervisorToken -notmatch '^[0-9a-f]{32}$' -or $SupervisorPid -le 0 -or $TimeoutSeconds -lt 1 -or $TimeoutSeconds -gt 60) {
        throw 'Worker handshake token, supervisor PID, or bounded timeout is invalid.'
    }
    if ([IO.File]::Exists($terminal)) { throw 'Worker terminal path already exists; refusing reuse.' }

    $clock = [Diagnostics.Stopwatch]::StartNew()
    while (-not [IO.File]::Exists($ready)) {
        if ($clock.Elapsed.TotalSeconds -ge $TimeoutSeconds) { throw 'Supervisor ownership handshake timed out.' }
        Start-Sleep -Milliseconds 25
    }
    $readyBytes = [IO.File]::ReadAllBytes($ready)
    if ($readyBytes.Length -gt 65536) { throw 'Supervisor handshake exceeds the bounded JSON size.' }
    $readyText = (New-Object Text.UTF8Encoding($false, $true)).GetString($readyBytes)
    $handshake = $readyText | ConvertFrom-Json -ErrorAction Stop
    if ($handshake.schema -cne 'range.pc11-v43-worker-handshake/v1' -or $handshake.candidateVersion -cne 'v43' -or
        $handshake.token -cne $SupervisorToken -or [int]$handshake.workerPid -ne $PID -or
        [int]$handshake.supervisorPid -ne $SupervisorPid -or
        $handshake.candidateManifestSha256 -cne $CandidateManifestSha256.ToLowerInvariant() -or
        $handshake.candidateManifestIdentitySha256 -cne $CandidateManifestIdentitySha256.ToLowerInvariant() -or
        $handshake.issuedBy -cne 'supervisor-contract-v43' -or $handshake.goPermitted -ne $false) {
        throw 'Supervisor token, process identity, or candidate-bound v43 handshake mismatch.'
    }
    if ($null -eq $handshake.productActions -or @($handshake.productActions.PSObject.Properties.Name).Count -ne 4) {
        throw 'Supervisor handshake must declare the complete product-action boundary.'
    }
    foreach ($name in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        if ($handshake.productActions.$name -ne $false) { throw "Supervisor handshake permits product action $name." }
    }

    $terminalReceipt = [ordered]@{
        schema = 'range.pc11-v43-worker-terminal/v1'
        status = 'FIXTURE_COMPLETE'
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        fixtureOnly = $true
        candidateManifestSha256 = $CandidateManifestSha256.ToLowerInvariant()
        candidateManifestIdentitySha256 = $CandidateManifestIdentitySha256.ToLowerInvariant()
        supervisorToken = $SupervisorToken
        supervisorPid = $SupervisorPid
        workerPid = $PID
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
    Assert-V43NoGoEnvelope $terminalReceipt 'v43 fixture worker terminal' $CandidateManifestIdentitySha256
    $null = Write-V43JsonNewFile $terminal $terminalReceipt 8
    [Console]::Out.WriteLine('V43_WORKER_HANDSHAKE_ACCEPTED_FIXTURE_ONLY')
    exit 0
}
catch {
    throw
}
