[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PreBuildSourcePreflightPath,
    [Parameter(Mandatory = $true)][string]$PreBuildSourcePreflightSha256,
    [Parameter(Mandatory = $true)][string]$ReleaseBuildReceiptPath,
    [Parameter(Mandatory = $true)][string]$ReleaseBuildReceiptSha256,
    [Parameter(Mandatory = $true)][string]$CMakeCachePath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$RepoRoot,
    [string]$AuditRoot,
    [string]$RunId
)

$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'This source-freeze requires Windows PowerShell 5.1.'
}
$producerPackageRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$producerToolsRoot = [IO.Path]::GetFullPath([IO.Path]::Combine($producerPackageRoot, 'tools'))
$moduleBoundaryHelperPath = [IO.Path]::GetFullPath([IO.Path]::Combine($producerToolsRoot, 'system-module-boundary-v43.ps1'))
$producerPackagePrefix = $producerPackageRoot.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if (-not $moduleBoundaryHelperPath.StartsWith($producerPackagePrefix, [StringComparison]::OrdinalIgnoreCase) -or
    -not [IO.File]::Exists($moduleBoundaryHelperPath)) { throw 'The canonical package-local v43 system-module helper is missing or outside its package.' }
foreach ($producerPath in @($producerPackageRoot, $producerToolsRoot, $moduleBoundaryHelperPath)) {
    if (([IO.File]::GetAttributes($producerPath) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "A v43 module-boundary path is a reparse point: $producerPath" }
}
$global:PSModuleAutoLoadingPreference = 'None'
$preloadedBoundaryCommands = @(& Microsoft.PowerShell.Core\Get-Command -Name 'Assert-V43SystemModuleBoundary' -All -ListImported -ErrorAction SilentlyContinue)
if ($preloadedBoundaryCommands.Count -gt 0) { throw 'A preloaded Assert-V43SystemModuleBoundary command cannot bypass the trusted v43 helper.' }
. $moduleBoundaryHelperPath
$trustedBoundaryCommand = $null
foreach ($boundaryCommand in @(& Microsoft.PowerShell.Core\Get-Command -Name 'Assert-V43SystemModuleBoundary' -All -ListImported -ErrorAction SilentlyContinue)) {
    if ($boundaryCommand.CommandType -eq 'Function' -and
        [IO.Path]::GetFullPath([string]$boundaryCommand.ScriptBlock.Ast.Extent.File) -ceq $moduleBoundaryHelperPath) {
        $trustedBoundaryCommand = $boundaryCommand
        break
    }
}
if ($null -eq $trustedBoundaryCommand) { throw 'Assert-V43SystemModuleBoundary was not loaded from the canonical package-local helper.' }
$moduleBoundaryResult = & $trustedBoundaryCommand.ScriptBlock
if ($moduleBoundaryResult.status -cne 'PASS') { throw 'The v43 system-module boundary helper did not return PASS.' }
Set-StrictMode -Version Latest
if (-not $PSBoundParameters.ContainsKey('RunId')) { $RunId = 'source-freeze-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8) }
if (-not $RepoRoot) { $RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..')) }
if (-not $AuditRoot) { $AuditRoot = Join-Path $RepoRoot 'build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit' }
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)
$PreBuildSourcePreflightPath = [IO.Path]::GetFullPath($PreBuildSourcePreflightPath)
$ReleaseBuildReceiptPath = [IO.Path]::GetFullPath($ReleaseBuildReceiptPath)
$CMakeCachePath = [IO.Path]::GetFullPath($CMakeCachePath)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)

function Get-Sha256File([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}
function Get-Sha256Bytes([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Assert-Hash([string]$Actual, [string]$Expected, [string]$Label) {
    if ($Expected -notmatch '^[0-9a-fA-F]{64}$' -or $Actual -cne $Expected.ToLowerInvariant()) { throw "$Label SHA256 differs from its required pin." }
}
function Read-JsonSnapshot([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { throw "Required JSON file is missing: $Path" }
    $bytes = [IO.File]::ReadAllBytes($Path)
    $sha256 = Get-Sha256Bytes $bytes
    $text = (New-Object Text.UTF8Encoding($false, $true)).GetString($bytes)
    $value = $text | ConvertFrom-Json -ErrorAction Stop
    Assert-JsonObjectValue $value "JSON root in $Path"
    return [pscustomobject]@{ value = $value; sha256 = $sha256 }
}
function Read-Json([string]$Path) {
    return (Read-JsonSnapshot $Path).value
}
function Read-PinnedJson([string]$Path, [string]$ExpectedSha256, [string]$Label) {
    $snapshot = Read-JsonSnapshot $Path
    Assert-Hash $snapshot.sha256 $ExpectedSha256 $Label
    return $snapshot
}
function Assert-JsonObjectValue($Value, [string]$Label) {
    if ($null -eq $Value -or $Value -isnot [System.Management.Automation.PSCustomObject]) { throw "$Label must be a JSON object." }
}
function Assert-JsonProperty($Object, [string]$Name, [string]$Label) {
    Assert-JsonObjectValue $Object $Label
    if ($Object.PSObject.Properties.Name -notcontains $Name) { throw "$Label.$Name is required." }
}
function Assert-JsonString($Object, [string]$Name, [string]$Label) {
    Assert-JsonProperty $Object $Name $Label
    if ($Object.$Name -isnot [string]) { throw "$Label.$Name must be a JSON string." }
}
function Assert-JsonBoolean($Object, [string]$Name, [string]$Label) {
    Assert-JsonProperty $Object $Name $Label
    if ($Object.$Name -isnot [bool]) { throw "$Label.$Name must be a JSON boolean." }
}
function Assert-JsonArray($Object, [string]$Name, [string]$Label) {
    Assert-JsonProperty $Object $Name $Label
    if ($Object.$Name -isnot [array]) { throw "$Label.$Name must be a JSON array." }
}
function Assert-JsonNumber($Object, [string]$Name, [string]$Label) {
    Assert-JsonProperty $Object $Name $Label
    if ($Object.$Name -isnot [ValueType] -or $Object.$Name -is [bool]) { throw "$Label.$Name must be a JSON number." }
}
function Assert-Sha256String([string]$Value, [string]$Label) {
    if ($Value -notmatch '^[0-9a-fA-F]{64}$') { throw "$Label must be a 64-character SHA256 string." }
}
function Assert-CommitString([string]$Value, [string]$Label) {
    if ($Value -notmatch '^(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})$') { throw "$Label must be a 40- or 64-character commit string." }
}
function Assert-ProductActions($Receipt, [string]$Label) {
    Assert-JsonProperty $Receipt 'productActions' $Label
    Assert-JsonObjectValue $Receipt.productActions "$Label.productActions"
    foreach ($actionName in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        Assert-JsonBoolean $Receipt.productActions $actionName "$Label.productActions"
        if ($Receipt.productActions.$actionName -ne $false) { throw "$Label permits product action $actionName." }
    }
}
function Assert-SubmoduleRows($Receipt, [string]$Label) {
    Assert-JsonArray $Receipt 'submoduleCommits' $Label
    foreach ($row in $Receipt.submoduleCommits) {
        Assert-JsonObjectValue $row "$Label.submoduleCommits row"
        foreach ($field in @('path', 'registeredCommit', 'headCommit', 'worktreeStatus', 'sourceTreeSha256')) {
            Assert-JsonString $row $field "$Label.submoduleCommits row"
        }
        Assert-CommitString $row.registeredCommit "$Label.submoduleCommits.registeredCommit"
        Assert-CommitString $row.headCommit "$Label.submoduleCommits.headCommit"
        Assert-Sha256String $row.sourceTreeSha256 "$Label.submoduleCommits.sourceTreeSha256"
    }
}
function Assert-PreflightReceipt($Receipt, [string]$Label, [string]$ExpectedRoot) {
    Assert-JsonObjectValue $Receipt $Label
    foreach ($field in @('schema', 'status', 'sourceRootPath', 'sourceScopeSchema', 'requiredCTestManifestPath', 'requiredCTestManifestSha256', 'rootCommit', 'rootWorktreeStatus', 'rootWorktreeStatusSha256', 'sourceTreeSha256')) {
        Assert-JsonString $Receipt $field $Label
    }
    Assert-JsonBoolean $Receipt 'goPermitted' $Label
    Assert-ProductActions $Receipt $Label
    Assert-SubmoduleRows $Receipt $Label
    Assert-JsonArray $Receipt 'sourceFiles' $Label
    Assert-JsonArray $Receipt 'checks' $Label
    Assert-JsonNumber $Receipt 'sourceFileCount' $Label
    Assert-JsonNumber $Receipt 'sourceTotalBytes' $Label
    Assert-JsonProperty $Receipt 'sourceScope' $Label
    Assert-JsonObjectValue $Receipt.sourceScope "$Label.sourceScope"
    Assert-Sha256String $Receipt.requiredCTestManifestSha256 "$Label.requiredCTestManifestSha256"
    Assert-Sha256String $Receipt.rootWorktreeStatusSha256 "$Label.rootWorktreeStatusSha256"
    Assert-Sha256String $Receipt.sourceTreeSha256 "$Label.sourceTreeSha256"
    Assert-CommitString $Receipt.rootCommit "$Label.rootCommit"
    if ($Receipt.schema -cne 'range.pc11-v43-source-preflight/v1' -or $Receipt.status -cne 'PASS_SOURCE_PREFLIGHT' -or
        $Receipt.goPermitted -ne $false -or $Receipt.sourceRootPath -cne $ExpectedRoot) {
        throw "$Label schema/status/root or no-GO identity is invalid."
    }
}
function Compare-SubmodulePins($Left, $Right) {
    $leftRows = New-Object 'System.Collections.Generic.List[object]'
    foreach ($row in $Left) {
        [void]$leftRows.Add([pscustomobject][ordered]@{
            path = [string]$row.path
            registeredCommit = ([string]$row.registeredCommit).ToLowerInvariant()
            headCommit = ([string]$row.headCommit).ToLowerInvariant()
            worktreeStatus = [string]$row.worktreeStatus
            sourceTreeSha256 = ([string]$row.sourceTreeSha256).ToLowerInvariant()
        })
    }
    $rightRows = New-Object 'System.Collections.Generic.List[object]'
    foreach ($row in $Right) {
        [void]$rightRows.Add([pscustomobject][ordered]@{
            path = [string]$row.path
            registeredCommit = ([string]$row.registeredCommit).ToLowerInvariant()
            headCommit = ([string]$row.headCommit).ToLowerInvariant()
            worktreeStatus = [string]$row.worktreeStatus
            sourceTreeSha256 = ([string]$row.sourceTreeSha256).ToLowerInvariant()
        })
    }
    $leftJson = ConvertTo-Json -InputObject $leftRows.ToArray() -Depth 10 -Compress
    $rightJson = ConvertTo-Json -InputObject $rightRows.ToArray() -Depth 10 -Compress
    return ($leftJson -ceq $rightJson)
}
function Get-NormalizedSubmodulePins($Rows) {
    $result = New-Object 'System.Collections.Generic.List[object]'
    foreach ($row in $Rows) {
        [void]$result.Add([pscustomobject][ordered]@{
            path = [string]$row.path
            registeredCommit = ([string]$row.registeredCommit).ToLowerInvariant()
            headCommit = ([string]$row.headCommit).ToLowerInvariant()
            worktreeStatus = [string]$row.worktreeStatus
            sourceTreeSha256 = ([string]$row.sourceTreeSha256).ToLowerInvariant()
        })
    }
    return $result.ToArray()
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
function Invoke-SourcePreflight([string]$OutputReceiptPath, [string]$RequiredManifestPath) {
    $powerShellPath = [IO.Path]::GetFullPath((Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'))
    $scriptPath = Join-Path $PSScriptRoot 'source-preflight-v43.ps1'
    $runIdForRescan = 'freeze-rescan-' + $RunId
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $scriptPath, '-RepoRoot', $RepoRoot,
        '-RequiredCTestManifestPath', $RequiredManifestPath,
        '-AuditRoot', $AuditRoot, '-RunId', $runIdForRescan, '-OutputPath', $OutputReceiptPath)
    $quoted = New-Object 'System.Collections.Generic.List[string]'
    foreach ($argument in $arguments) { [void]$quoted.Add((Quote-WindowsArgument ([string]$argument))) }
    $processInfo = New-Object Diagnostics.ProcessStartInfo
    $processInfo.FileName = $powerShellPath
    $processInfo.Arguments = [string]::Join(' ', $quoted.ToArray())
    $processInfo.WorkingDirectory = $RepoRoot
    $processInfo.UseShellExecute = $false
    $processInfo.CreateNoWindow = $true
    $processInfo.RedirectStandardOutput = $true
    $processInfo.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $processInfo
    try {
        if (-not $process.Start()) { throw 'Could not start the Windows PowerShell 5.1 source-preflight process.' }
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $stdout = $process.StandardOutput.ReadToEnd()
        $process.WaitForExit()
        $stderr = $stderrTask.Result
        if ($process.ExitCode -ne 0) { throw ("Post-build source-preflight exited {0}: {1} {2}" -f $process.ExitCode, $stdout.Trim(), $stderr.Trim()) }
    }
    finally { $process.Dispose() }
    return Read-JsonSnapshot $OutputReceiptPath
}

$outputRoot = [IO.Path]::GetFullPath((Join-Path $AuditRoot ''))
if (-not $OutputPath.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Freeze output must remain under the external v43 audit root.' }
if ([IO.File]::Exists($OutputPath)) { throw "Refusing to overwrite existing evidence: $OutputPath" }
if ($RunId -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$') { throw 'RunId must contain only bounded ASCII letters, digits, dot, underscore, or hyphen.' }

$receipt = $null
$failure = $null
try {
    $preBuildSnapshot = Read-PinnedJson $PreBuildSourcePreflightPath $PreBuildSourcePreflightSha256 'Pre-build source-preflight receipt'
    $preBuildSha = $preBuildSnapshot.sha256
    $preBuild = $preBuildSnapshot.value
    Assert-PreflightReceipt $preBuild 'Pre-build source-preflight receipt' $RepoRoot
    $requiredCTestPath = [IO.Path]::GetFullPath($preBuild.requiredCTestManifestPath)

    $buildSnapshot = Read-PinnedJson $ReleaseBuildReceiptPath $ReleaseBuildReceiptSha256 'OFF Release build receipt'
    $buildSha = $buildSnapshot.sha256
    $build = $buildSnapshot.value
    foreach ($field in @('schema', 'status', 'candidateVersion', 'variant', 'configuration', 'rootCommit', 'sourceTreeSha256', 'requiredCTestManifestSha256', 'cmakeCacheSha256')) {
        Assert-JsonString $build $field 'Release-build receipt'
    }
    Assert-JsonBoolean $build 'goPermitted' 'Release-build receipt'
    Assert-JsonBoolean $build 'staticOnly' 'Release-build receipt'
    Assert-SubmoduleRows $build 'Release-build receipt'
    Assert-ProductActions $build 'Release-build receipt'
    Assert-CommitString $build.rootCommit 'Release-build receipt.rootCommit'
    foreach ($field in @('sourceTreeSha256', 'requiredCTestManifestSha256', 'cmakeCacheSha256')) {
        Assert-Sha256String $build.$field "Release-build receipt.$field"
    }
    if ($build.schema -cne 'range.pc11-v43-release-build-receipt/v1' -or $build.status -cne 'PASS' -or
        $build.candidateVersion -cne 'v43' -or $build.variant -cne 'OFF' -or $build.configuration -cne 'Release' -or
        $build.goPermitted -ne $false -or $build.staticOnly -ne $true) { throw 'Release-build receipt schema/status/identity or no-GO boundary is invalid.' }
    if ($build.PSObject.Properties.Name -contains 'requiredCTestManifestPath') {
        Assert-JsonString $build 'requiredCTestManifestPath' 'Release-build receipt'
        if ([IO.Path]::GetFullPath($build.requiredCTestManifestPath) -cne $requiredCTestPath) { throw 'Release-build receipt required CTest manifest path differs from the source-preflight path.' }
    }
    $manifestPath = $requiredCTestPath
    $manifestSha = Get-Sha256File $manifestPath
    Assert-Hash $manifestSha ([string]$build.requiredCTestManifestSha256) 'Required CTest manifest'
    $cacheSha = Get-Sha256File $CMakeCachePath
    Assert-Hash $cacheSha ([string]$build.cmakeCacheSha256) 'OFF Release CMakeCache'
    if ($build.PSObject.Properties.Name -contains 'cmakeCachePath') {
        Assert-JsonString $build 'cmakeCachePath' 'Release-build receipt'
        if ([IO.Path]::GetFullPath($build.cmakeCachePath) -cne $CMakeCachePath) { throw 'Release-build receipt CMakeCache path differs from the supplied path.' }
    }

    $postBuildPath = Join-Path ([IO.Path]::GetDirectoryName($OutputPath)) ('post-build-source-preflight-' + $RunId + '.json')
    if (-not $postBuildPath.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Post-build source-preflight output escapes the external v43 audit root.' }
    $postBuildSnapshot = Invoke-SourcePreflight $postBuildPath $requiredCTestPath
    $postBuild = $postBuildSnapshot.value
    $postBuildSha = $postBuildSnapshot.sha256
    Assert-PreflightReceipt $postBuild 'Post-build source-preflight receipt' $RepoRoot

    foreach ($pair in @(
        @{ label = 'rootCommit'; before = [string]$preBuild.rootCommit; build = [string]$build.rootCommit; after = [string]$postBuild.rootCommit },
        @{ label = 'sourceTreeSha256'; before = [string]$preBuild.sourceTreeSha256; build = [string]$build.sourceTreeSha256; after = [string]$postBuild.sourceTreeSha256 },
        @{ label = 'requiredCTestManifestSha256'; before = [string]$preBuild.requiredCTestManifestSha256; build = [string]$build.requiredCTestManifestSha256; after = [string]$postBuild.requiredCTestManifestSha256 }
    )) {
        if ($pair.before -cne $pair.build -or $pair.build -cne $pair.after) { throw "Pre-build, OFF build, and post-build identities differ for $($pair.label)." }
    }
    if (-not (Compare-SubmodulePins $preBuild.submoduleCommits $build.submoduleCommits) -or
        -not (Compare-SubmodulePins $build.submoduleCommits $postBuild.submoduleCommits)) { throw 'Pre-build, OFF build, and post-build submodule commit/source-tree pins differ.' }
    if ($preBuild.sourceScopeSchema -cne 'range.pc11-v43-source-scope/v1' -or
        (ConvertTo-Json -InputObject $preBuild.sourceScope -Depth 10 -Compress) -cne
        (ConvertTo-Json -InputObject $postBuild.sourceScope -Depth 10 -Compress)) { throw 'Source scope schema or serialized rules changed across the build freeze.' }

    $receipt = [ordered]@{
        schema = 'range.pc11-v43-source-freeze/v1'
        status = 'PASS'
        candidateVersion = 'v43'
        variant = 'OFF'
        configuration = 'Release'
        generatedAtUtc = [DateTime]::UtcNow.ToString('o')
        sourceRootPath = $RepoRoot
        sourceScopeSchema = [string]$postBuild.sourceScopeSchema
        sourceScope = $postBuild.sourceScope
        rootCommit = [string]$postBuild.rootCommit
        rootWorktreeStatus = [string]$postBuild.rootWorktreeStatus
        rootWorktreeStatusSha256 = [string]$postBuild.rootWorktreeStatusSha256
        submoduleCommits = @(Get-NormalizedSubmodulePins $postBuild.submoduleCommits)
        sourceFiles = @($postBuild.sourceFiles)
        sourceFileCount = [int]$postBuild.sourceFileCount
        sourceTotalBytes = [long]$postBuild.sourceTotalBytes
        sourceTreeSha256 = [string]$postBuild.sourceTreeSha256
        preBuildSourcePreflightReceiptPath = $PreBuildSourcePreflightPath
        preBuildSourcePreflightReceiptSha256 = $preBuildSha
        postBuildSourcePreflightReceiptPath = $postBuildPath
        postBuildSourcePreflightReceiptSha256 = $postBuildSha
        requiredCTestManifestPath = $manifestPath
        requiredCTestManifestSha256 = $manifestSha
        cmakeCachePath = $CMakeCachePath
        cmakeCacheSha256 = $cacheSha
        releaseBuildReceiptPath = $ReleaseBuildReceiptPath
        releaseBuildReceiptSha256 = $buildSha
        staticOnly = $true
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
}
catch { $failure = $_.Exception.Message }

if ($null -eq $receipt) {
    $receipt = [ordered]@{
        schema = 'range.pc11-v43-source-freeze/v1'
        status = 'SOURCE_FREEZE_FAIL'
        candidateVersion = 'v43'
        variant = 'OFF'
        generatedAtUtc = [DateTime]::UtcNow.ToString('o')
        sourceRootPath = $RepoRoot
        failure = $failure
        staticOnly = $true
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
}
$parent = [IO.Path]::GetDirectoryName($OutputPath)
if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
$json = $receipt | ConvertTo-Json -Depth 20
[IO.File]::WriteAllText($OutputPath, $json + "`n", (New-Object Text.UTF8Encoding($false)))
if ($receipt.PSObject.Properties.Name -contains 'sourceTreeSha256') { $reportedSourceSha = [string]$receipt.sourceTreeSha256 }
else { $reportedSourceSha = 'unavailable' }
Write-Output ("{0} sourceTreeSha256={1} output={2}" -f $receipt.status, $reportedSourceSha, $OutputPath)
if ($receipt.status -cne 'PASS') { exit 1 }
