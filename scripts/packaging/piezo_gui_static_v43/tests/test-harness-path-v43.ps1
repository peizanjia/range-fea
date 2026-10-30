[CmdletBinding()]
param([string]$AuditRoot)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'This v43 harness-path test requires Windows PowerShell 5.1.'
}
$testsRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$prepRoot = [IO.Path]::GetFullPath((Join-Path $testsRoot '..'))
if (-not $AuditRoot) { $AuditRoot = [IO.Path]::GetFullPath((Join-Path $prepRoot '..\..\..\build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit')) }
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)

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

$harnessPath = Join-Path $testsRoot 'test-static-chain-v43.ps1'
$templatePath = Join-Path $prepRoot 'candidate-manifest-v43.admission-template.json'
$unusedOutputPath = Join-Path $AuditRoot ('harness-path-unused-' + [Guid]::NewGuid().ToString('N') + '.json')
foreach ($path in @($harnessPath, $templatePath)) {
    if (-not [IO.File]::Exists($path)) { throw "Harness path-test prerequisite is missing: $path" }
}
$powerShellPath = [IO.Path]::GetFullPath((Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'))
$arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $harnessPath,
    '-CandidateManifestPath', $templatePath,
    '-OutputPath', $unusedOutputPath,
    '-AuditRoot', $AuditRoot,
    '-VerifyProducerPathsOnly')
$quoted = New-Object 'System.Collections.Generic.List[string]'
foreach ($argument in $arguments) { [void]$quoted.Add((Quote-WindowsArgument ([string]$argument))) }
$processInfo = New-Object Diagnostics.ProcessStartInfo
$processInfo.FileName = $powerShellPath
$processInfo.Arguments = [string]::Join(' ', $quoted.ToArray())
$processInfo.WorkingDirectory = $prepRoot
$processInfo.UseShellExecute = $false
$processInfo.CreateNoWindow = $true
$processInfo.RedirectStandardOutput = $true
$processInfo.RedirectStandardError = $true
$process = New-Object Diagnostics.Process
$process.StartInfo = $processInfo
try {
    if (-not $process.Start()) { throw 'Could not launch static harness producer-path check.' }
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $stdout = $process.StandardOutput.ReadToEnd()
    $process.WaitForExit()
    $stderr = $stderrTask.Result
    if ($process.ExitCode -ne 0 -or $stdout -notmatch 'PASS_HARNESS_PATHS producerCount=4' -or
        $stdout -notmatch [regex]::Escape($prepRoot)) {
        throw "Harness producer-path check failed: exit=$($process.ExitCode) stdout=$($stdout.Trim()) stderr=$($stderr.Trim())"
    }
}
finally { $process.Dispose() }
if ([IO.File]::Exists($unusedOutputPath)) { throw 'Path-only harness test unexpectedly wrote a static receipt.' }
Write-Output ("PASS_HARNESS_PATH_TEST prepRoot={0} producerCount=4" -f $prepRoot)
