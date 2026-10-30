[CmdletBinding()]
param(
    [string]$AuditRoot,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'This producer-to-admission source-scope test requires Windows PowerShell 5.1.'
}
$script:PrepRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $AuditRoot) { $AuditRoot = [IO.Path]::GetFullPath((Join-Path $script:PrepRoot '..\..\..\build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit')) }
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)
if (-not $OutputPath) { $OutputPath = Join-Path $AuditRoot ('source-scope-contract-' + [Guid]::NewGuid().ToString('N') + '.json') }
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$outputRoot = $AuditRoot.TrimEnd('\') + '\'
if (-not $OutputPath.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Source-scope test receipt must remain under the external v43 audit root.' }
if ([IO.File]::Exists($OutputPath)) { throw "Refusing to overwrite existing evidence: $OutputPath" }

function Get-Sha256File([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
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
function Invoke-CapturedProcess([string]$FileName, [string[]]$Arguments, [string]$WorkingDirectory) {
    $quoted = New-Object 'System.Collections.Generic.List[string]'
    foreach ($argument in $Arguments) { [void]$quoted.Add((Quote-WindowsArgument ([string]$argument))) }
    $processInfo = New-Object Diagnostics.ProcessStartInfo
    $processInfo.FileName = $FileName
    $processInfo.Arguments = [string]::Join(' ', $quoted.ToArray())
    $processInfo.WorkingDirectory = $WorkingDirectory
    $processInfo.UseShellExecute = $false
    $processInfo.CreateNoWindow = $true
    $processInfo.RedirectStandardOutput = $true
    $processInfo.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $processInfo
    try {
        if (-not $process.Start()) { throw "Could not start process: $FileName" }
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $stdout = $process.StandardOutput.ReadToEnd()
        $process.WaitForExit()
        return [pscustomobject]@{ exitCode = [int]$process.ExitCode; stdout = $stdout; stderr = $stderrTask.Result }
    }
    finally { $process.Dispose() }
}
function Invoke-GitFixture([string]$GitPath, [string]$RepoRoot, [string[]]$Arguments) {
    $result = Invoke-CapturedProcess $GitPath (@('-C', $RepoRoot, '-c', 'core.quotepath=false') + $Arguments) $RepoRoot
    if ($result.exitCode -ne 0) { throw "Git fixture command failed: $($result.stdout.Trim()) $($result.stderr.Trim())" }
}

$receipt = $null
$failure = $null
$preflightPath = $null
$probeOutputPath = $null
try {
    $gitCommand = Get-Command git.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1
    $pythonCommand = Get-Command py.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1
    $powershellPath = [IO.Path]::GetFullPath((Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'))
    $runRoot = Join-Path $AuditRoot ('source-scope-contract-fixture-' + [Guid]::NewGuid().ToString('N'))
    $repoRoot = Join-Path $runRoot 'repo'
    [void][IO.Directory]::CreateDirectory((Join-Path $repoRoot 'scripts\ci'))
    [void][IO.Directory]::CreateDirectory((Join-Path $repoRoot 'src\build'))
    [void][IO.Directory]::CreateDirectory((Join-Path $repoRoot 'build'))
    $ctestPath = Join-Path $repoRoot 'scripts\ci\required-ctest-tests.json'
    [IO.File]::WriteAllText($ctestPath, '{"required_test_groups":{"fixture":["fixture_test"]}}' + "`n", (New-Object Text.UTF8Encoding($false)))
    $nestedSourcePath = Join-Path $repoRoot 'src\build\retained.cpp'
    [IO.File]::WriteAllText($nestedSourcePath, 'int v43_nested_source_scope_fixture() { return 43; }' + "`n", (New-Object Text.UTF8Encoding($false)))
    $rootBuildPath = Join-Path $repoRoot 'build\generated.cpp'
    [IO.File]::WriteAllText($rootBuildPath, 'int v43_root_build_exclusion_fixture() { return 0; }' + "`n", (New-Object Text.UTF8Encoding($false)))

    Invoke-GitFixture $gitCommand.Source $repoRoot @('init', '--quiet')
    Invoke-GitFixture $gitCommand.Source $repoRoot @('config', 'user.name', 'v43 source-scope test')
    Invoke-GitFixture $gitCommand.Source $repoRoot @('config', 'user.email', 'v43-source-scope@example.invalid')
    Invoke-GitFixture $gitCommand.Source $repoRoot @('add', '-f', '--', 'scripts/ci/required-ctest-tests.json', 'src/build/retained.cpp', 'build/generated.cpp')
    Invoke-GitFixture $gitCommand.Source $repoRoot @('commit', '--quiet', '-m', 'source scope integration fixture')

    $preflightPath = Join-Path $runRoot 'source-preflight-v43.json'
    $sourcePreflightPath = Join-Path $script:PrepRoot 'source-preflight-v43.ps1'
    $preflightArguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $sourcePreflightPath,
        '-RepoRoot', $repoRoot,
        '-RequiredCTestManifestPath', $ctestPath,
        '-AuditRoot', $AuditRoot,
        '-RunId', ('scope-fixture-' + [Guid]::NewGuid().ToString('N')),
        '-OutputPath', $preflightPath
    )
    $preflightResult = Invoke-CapturedProcess $powershellPath $preflightArguments $repoRoot
    if ($preflightResult.exitCode -ne 0) { throw "Windows PowerShell 5.1 source preflight failed: $($preflightResult.stdout.Trim()) $($preflightResult.stderr.Trim())" }
    if (-not [IO.File]::Exists($preflightPath)) { throw 'Producer did not write the requested source-preflight receipt.' }
    $preflight = (New-Object Text.UTF8Encoding($false, $true)).GetString([IO.File]::ReadAllBytes($preflightPath)) | ConvertFrom-Json -ErrorAction Stop
    if ($preflight.schema -cne 'range.pc11-v43-source-preflight/v1' -or $preflight.status -cne 'PASS_SOURCE_PREFLIGHT') { throw 'Producer returned a non-PASS source-preflight receipt.' }

    $probePath = Join-Path $PSScriptRoot 'admission-source-scope-probe-v43.py'
    $probeOutputPath = Join-Path $runRoot 'admission-source-scope-probe.json'
    $probeResult = Invoke-CapturedProcess $pythonCommand.Source @('-3', $probePath, $repoRoot) $repoRoot
    if ($probeResult.exitCode -ne 0) { throw "Admission source-scope enumeration failed: $($probeResult.stdout.Trim()) $($probeResult.stderr.Trim())" }
    [IO.File]::WriteAllText($probeOutputPath, $probeResult.stdout.Trim() + "`n", (New-Object Text.UTF8Encoding($false)))
    $probe = $probeResult.stdout.Trim() | ConvertFrom-Json -ErrorAction Stop
    if ($probe.sourceScopeSchema -cne $preflight.sourceScopeSchema) { throw 'Producer and admission source-scope schema IDs differ.' }
    foreach ($field in @('includePrefixes', 'includeBasenamesAnywhere', 'includeExtensionsAnywhere', 'includeExactPaths', 'excludePrefixes')) {
        if ([string]::Join("`n", [string[]]@($probe.sourceScope.$field)) -cne [string]::Join("`n", [string[]]@($preflight.sourceScope.$field))) { throw "Producer and admission source-scope field differs: $field" }
    }
    foreach ($field in @('enumeration', 'pathOrder', 'treeDigest')) {
        if ([string]$probe.sourceScope.$field -cne [string]$preflight.sourceScope.$field) { throw "Producer and admission source-scope field differs: $field" }
    }
    if ([string]$probe.rootCommit -cne [string]$preflight.rootCommit -or
        [string]$probe.sourceTreeSha256 -cne [string]$preflight.sourceTreeSha256) { throw 'Producer and admission root commit/tree digest differ.' }
    $producerRows = @($preflight.sourceFiles)
    $admissionRows = @($probe.sourceFiles)
    if ($producerRows.Count -ne $admissionRows.Count) { throw 'Producer and admission source row counts differ.' }
    $admissionByPath = @{}
    foreach ($row in $admissionRows) { $admissionByPath[[string]$row.path] = $row }
    foreach ($row in $producerRows) {
        $path = [string]$row.path
        if (-not $admissionByPath.ContainsKey($path)) { throw "Admission enumeration omitted producer source path: $path" }
        $other = $admissionByPath[$path]
        if ([long]$row.sizeBytes -ne [long]$other.sizeBytes -or [string]$row.sha256 -cne [string]$other.sha256) { throw "Producer/admission source row differs: $path" }
    }
    if ($producerRows.path -cnotcontains 'src/build/retained.cpp') { throw 'Producer omitted an in-scope nested src/build source file.' }
    if ($producerRows.path -ccontains 'build/generated.cpp') { throw 'Producer included the root build/ exclusion.' }
    if ($probe.sourceFiles.path -cnotcontains 'src/build/retained.cpp' -or $probe.sourceFiles.path -ccontains 'build/generated.cpp') { throw 'Admission did not apply the nested/root build scope rules identically.' }

    $receipt = [ordered]@{
        schema = 'range.pc11-v43-source-scope-producer-admission-test/v1'
        status = 'PASS_SOURCE_SCOPE_CONTRACT'
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        testOnly = $true
        sourceScopeSchema = [string]$preflight.sourceScopeSchema
        sourceRootPath = $repoRoot
        sourcePreflightReceiptPath = $preflightPath
        sourcePreflightReceiptSha256 = Get-Sha256File $preflightPath
        admissionProbePath = $probeOutputPath
        admissionProbeSha256 = Get-Sha256File $probeOutputPath
        rootCommit = [string]$preflight.rootCommit
        sourceTreeSha256 = [string]$preflight.sourceTreeSha256
        sourceFileCount = $producerRows.Count
        nestedSourcePath = 'src/build/retained.cpp'
        excludedRootBuildPath = 'build/generated.cpp'
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
}
catch { $failure = $_.Exception.Message }

if ($null -eq $receipt) {
    $receipt = [ordered]@{
        schema = 'range.pc11-v43-source-scope-producer-admission-test/v1'
        status = 'FAIL'
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        testOnly = $true
        failure = $failure
        sourceFileCount = 0
        sourcePreflightReceiptPath = $preflightPath
        admissionProbePath = $probeOutputPath
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
}
$parent = [IO.Path]::GetDirectoryName($OutputPath)
if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
$json = $receipt | ConvertTo-Json -Depth 12
[IO.File]::WriteAllText($OutputPath, $json + "`n", (New-Object Text.UTF8Encoding($false)))
Write-Output ("{0} sourceFiles={1} nestedPath=src/build/retained.cpp goPermitted=false output={2}" -f $receipt.status, $receipt.sourceFileCount, $OutputPath)
if ($receipt.status -cne 'PASS_SOURCE_SCOPE_CONTRACT') { exit 1 }
