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
    [Parameter(Mandatory = $true)][string]$FixtureScriptSha256,
    [Parameter(Mandatory = $true)][string]$HandshakeScriptSha256,
    [Parameter(Mandatory = $true)][string]$WorkerErrorPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$worker = Join-Path (Split-Path -Parent $PSScriptRoot) '..\runtime\worker-handshake-v43.ps1'
$worker = [IO.Path]::GetFullPath($worker)
function Get-FixtureSha256([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}
if ($FixtureScriptSha256 -notmatch '^[0-9a-f]{64}$' -or $HandshakeScriptSha256 -notmatch '^[0-9a-f]{64}$' -or
    (Get-FixtureSha256 $PSCommandPath) -cne $FixtureScriptSha256 -or (Get-FixtureSha256 $worker) -cne $HandshakeScriptSha256) {
    [Console]::Error.WriteLine('v43 fixture worker source bytes differ from the supervisor pins.')
    exit 73
}
try {
    & $worker -CandidateManifestPath $CandidateManifestPath -CandidateManifestSha256 $CandidateManifestSha256 `
        -CandidateManifestIdentitySha256 $CandidateManifestIdentitySha256 -EvidenceRoot $EvidenceRoot `
        -SupervisorReadyPath $SupervisorReadyPath -SupervisorTerminalPath $SupervisorTerminalPath `
        -SupervisorToken $SupervisorToken -SupervisorPid $SupervisorPid
    exit 0
}
catch {
    [IO.File]::WriteAllText($WorkerErrorPath, $_.Exception.Message, (New-Object Text.UTF8Encoding($false)))
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 73
}
