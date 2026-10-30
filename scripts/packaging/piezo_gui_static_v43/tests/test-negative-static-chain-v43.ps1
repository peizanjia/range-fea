[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CandidateManifestPath,
    [Parameter(Mandatory = $true)][string]$PreBuildSourcePreflightPath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$AuditRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'This v43 negative static-chain test requires Windows PowerShell 5.1.'
}
if (-not $AuditRoot) { $AuditRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..\build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit')) }
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)
$CandidateManifestPath = [IO.Path]::GetFullPath($CandidateManifestPath)
$PreBuildSourcePreflightPath = [IO.Path]::GetFullPath($PreBuildSourcePreflightPath)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)

function Get-Sha256File([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}
function Read-Json([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { throw "Required evidence file is missing: $Path" }
    $bytes = [IO.File]::ReadAllBytes($Path)
    return ((New-Object Text.UTF8Encoding($false, $true)).GetString($bytes) | ConvertFrom-Json -ErrorAction Stop)
}
function Quote-WindowsArgument([string]$Value) {
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $builder = New-Object Text.StringBuilder
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') { $slashes++; continue }
        if ($character -eq '"') {
            [void]$builder.Append(('\' * (2 * $slashes + 1)))
            [void]$builder.Append('"')
            $slashes = 0
            continue
        }
        if ($slashes -gt 0) { [void]$builder.Append(('\' * $slashes)); $slashes = 0 }
        [void]$builder.Append($character)
    }
    if ($slashes -gt 0) { [void]$builder.Append(('\' * (2 * $slashes))) }
    [void]$builder.Append('"')
    return $builder.ToString()
}
function Invoke-ChildPowerShell([string[]]$ScriptArguments, [string]$WorkingDirectory) {
    $powerShellPath = [IO.Path]::GetFullPath((Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'))
    $allArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass') + $ScriptArguments
    $quoted = New-Object 'System.Collections.Generic.List[string]'
    foreach ($argument in $allArguments) { [void]$quoted.Add((Quote-WindowsArgument ([string]$argument))) }
    $processInfo = New-Object Diagnostics.ProcessStartInfo
    $processInfo.FileName = $powerShellPath
    $processInfo.Arguments = [string]::Join(' ', $quoted.ToArray())
    $processInfo.WorkingDirectory = $WorkingDirectory
    $processInfo.UseShellExecute = $false
    $processInfo.CreateNoWindow = $true
    $processInfo.RedirectStandardOutput = $true
    $processInfo.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $processInfo
    try {
        if (-not $process.Start()) { throw 'Could not start a v43 negative-test child.' }
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $stdout = $process.StandardOutput.ReadToEnd()
        $process.WaitForExit()
        return [pscustomobject]@{ exitCode = [int]$process.ExitCode; stdout = $stdout; stderr = $stderrTask.Result }
    }
    finally { $process.Dispose() }
}
function Assert-FailureReceipt([string]$TestId, [string]$ScriptPath, [string[]]$Arguments, [string]$ExpectedStatus) {
    $child = Invoke-ChildPowerShell (@('-File', $ScriptPath) + $Arguments) ([IO.Path]::GetDirectoryName($CandidateManifestPath))
    if ($child.exitCode -eq 0) { throw "$TestId accepted a deliberately incorrect pin." }
    $outputIndex = [Array]::IndexOf($Arguments, '-OutputPath')
    if ($outputIndex -lt 0 -or $outputIndex + 1 -ge $Arguments.Length) { throw "$TestId has no failure receipt path." }
    $path = [IO.Path]::GetFullPath($Arguments[$outputIndex + 1])
    if (-not [IO.File]::Exists($path)) { throw "$TestId did not write its failure receipt. exit=$($child.exitCode) stdout=$($child.stdout.Trim()) stderr=$($child.stderr.Trim())" }
    $receipt = Read-Json $path
    if ($receipt.status -cne $ExpectedStatus -or $receipt.goPermitted -ne $false) { throw "$TestId wrote an unexpected status or GO value." }
    foreach ($actionName in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        if ($receipt.productActions.$actionName -ne $false) { throw "$TestId failure receipt permits product action $actionName." }
    }
    return [pscustomobject]@{ testId = $TestId; status = 'PASS'; childExitCode = $child.exitCode; observedReceiptStatus = $receipt.status; receiptPath = $path; receiptSha256 = Get-Sha256File $path }
}

$outputRoot = [IO.Path]::GetFullPath((Join-Path $AuditRoot ''))
if (-not $OutputPath.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Negative-test receipt must remain under the external v43 audit root.' }
if ([IO.File]::Exists($OutputPath)) { throw "Refusing to overwrite existing evidence: $OutputPath" }
$receipt = $null
$failure = $null
try {
    if (-not [IO.File]::Exists($CandidateManifestPath)) { throw 'Candidate manifest fixture is missing.' }
    $preflight = Read-Json $PreBuildSourcePreflightPath
    if ($preflight.schema -cne 'range.pc11-v43-source-preflight/v1' -or $preflight.status -cne 'PASS_SOURCE_PREFLIGHT' -or $preflight.goPermitted -ne $false) {
        throw 'Negative freeze test requires a passing source snapshot receipt; it does not assert a source freeze.'
    }
    $negativeRoot = Join-Path $AuditRoot ('negative-chain-fixtures-' + [Guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($negativeRoot)
    $badBuildPath = Join-Path $negativeRoot 'malformed-build-receipt.json'
    $badBuild = [ordered]@{ schema = 'range.pc11-v43-release-build-receipt/invalid-test'; status = 'FAIL'; candidateVersion = 'v43'; variant = 'OFF'; configuration = 'Release'; staticOnly = $true; productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }; goPermitted = $false }
    [IO.File]::WriteAllText($badBuildPath, (($badBuild | ConvertTo-Json -Depth 8) + "`n"), (New-Object Text.UTF8Encoding($false)))
    $badBuildSha = Get-Sha256File $badBuildPath
    $preflightSha = Get-Sha256File $PreBuildSourcePreflightPath
    $freezeFailurePath = Join-Path $negativeRoot 'source-freeze-failure.json'
    $freezeResult = Assert-FailureReceipt 'freezeRejectsMalformedBuildReceipt' (Join-Path $PSScriptRoot '..\freeze-v43.ps1') @(
        '-PreBuildSourcePreflightPath', $PreBuildSourcePreflightPath,
        '-PreBuildSourcePreflightSha256', $preflightSha,
        '-ReleaseBuildReceiptPath', $badBuildPath,
        '-ReleaseBuildReceiptSha256', $badBuildSha,
        '-CMakeCachePath', (Join-Path $negativeRoot 'unused-CMakeCache.txt'),
        '-RepoRoot', [string]$preflight.sourceRootPath,
        '-AuditRoot', $AuditRoot,
        '-RunId', ('negative-freeze-' + [Guid]::NewGuid().ToString('N')),
        '-OutputPath', $freezeFailurePath
    ) 'SOURCE_FREEZE_FAIL'
    $packageFailurePath = Join-Path $negativeRoot 'package-tree-failure.json'
    $packageResult = Assert-FailureReceipt 'packageTreeRejectsWrongManifestSha' (Join-Path $PSScriptRoot '..\package-tree-v43.ps1') @(
        '-CandidateManifestPath', $CandidateManifestPath,
        '-CandidateManifestSha256', ('0' * 64),
        '-AuditRoot', $AuditRoot,
        '-RunId', ('negative-package-' + [Guid]::NewGuid().ToString('N')),
        '-OutputPath', $packageFailurePath
    ) 'FAIL'
    $finalizeFailurePath = Join-Path $negativeRoot 'finalize-failure.json'
    $finalizeResult = Assert-FailureReceipt 'finalizeRejectsWrongManifestSha' (Join-Path $PSScriptRoot '..\finalize-v43.ps1') @(
        '-CandidateManifestPath', $CandidateManifestPath,
        '-CandidateManifestSha256', ('0' * 64),
        '-AuditRoot', $AuditRoot,
        '-RunId', ('negative-finalize-' + [Guid]::NewGuid().ToString('N')),
        '-OutputPath', $finalizeFailurePath
    ) 'FINALIZE_FAIL'
    $receipt = [ordered]@{
        schema = 'range.pc11-v43-negative-static-chain-tests/v1'
        status = 'PASS_NEGATIVE_CASES'
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        testOnly = $true
        sourcePreflightReceiptPath = $PreBuildSourcePreflightPath
        sourcePreflightReceiptSha256 = $preflightSha
        cases = @($freezeResult, $packageResult, $finalizeResult)
        caseCount = 3
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
}
catch { $failure = $_.Exception.Message }

if ($null -eq $receipt) {
    $receipt = [ordered]@{
        schema = 'range.pc11-v43-negative-static-chain-tests/v1'
        status = 'FAIL'
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        testOnly = $true
        failure = $failure
        caseCount = 0
        cases = @()
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
}
$parent = [IO.Path]::GetDirectoryName($OutputPath)
if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
$json = $receipt | ConvertTo-Json -Depth 16
[IO.File]::WriteAllText($OutputPath, $json + "`n", (New-Object Text.UTF8Encoding($false)))
Write-Output ("{0} cases={1} goPermitted=false output={2}" -f $receipt.status, $receipt.caseCount, $OutputPath)
if ($receipt.status -cne 'PASS_NEGATIVE_CASES') { exit 1 }
