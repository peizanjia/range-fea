[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CandidateManifestPath,
    [Parameter(Mandatory = $true)][string]$CandidateManifestSha256,
    [Parameter(Mandatory = $true)][string]$SourceFreezePath,
    [Parameter(Mandatory = $true)][string]$SourceFreezeSha256,
    [Parameter(Mandatory = $true)][string]$SystemModuleBoundarySha256,
    [Parameter(Mandatory = $true)][string]$OutputPath
)

# Static evidence only. This script never launches the product, its private Python,
# or performs Save, Run, or Confirm. External pins are mandatory to prevent a
# mutable manifest/freeze pair from self-authenticating.
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'The v43 Save-environment preflight requires Windows PowerShell 5.1.'
}
$packageRoot = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, '..'))
$toolsRoot = [IO.Path]::GetFullPath([IO.Path]::Combine($packageRoot, 'tools'))
$boundaryPath = [IO.Path]::GetFullPath([IO.Path]::Combine($toolsRoot, 'system-module-boundary-v43.ps1'))
$packagePrefix = $packageRoot.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if (-not $boundaryPath.StartsWith($packagePrefix, [StringComparison]::OrdinalIgnoreCase) -or -not [IO.File]::Exists($boundaryPath)) {
    throw 'The canonical package-local v43 system-module helper is unavailable.'
}
foreach ($trustedPath in @($packageRoot, $toolsRoot, $boundaryPath)) {
    if (([IO.File]::GetAttributes($trustedPath) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "System-module boundary path is a reparse point: $trustedPath" }
}
if ($SystemModuleBoundarySha256 -cnotmatch '^[0-9a-fA-F]{64}$') { throw 'Canonical system-module helper needs an externally supplied SHA256 pin.' }
$boundaryHandle = [IO.File]::Open($boundaryPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    $boundarySha = [Security.Cryptography.SHA256]::Create()
    try { $boundaryActual = ([BitConverter]::ToString($boundarySha.ComputeHash($boundaryHandle))).Replace('-', '').ToLowerInvariant() }
    finally { $boundarySha.Dispose() }
    if ($boundaryActual -cne $SystemModuleBoundarySha256.ToLowerInvariant()) { throw 'Canonical system-module helper differs from its external SHA256 pin.' }
    $global:PSModuleAutoLoadingPreference = 'None'
    $preloaded = @(& Microsoft.PowerShell.Core\Get-Command -Name 'Assert-V43SystemModuleBoundary' -All -ListImported -ErrorAction SilentlyContinue)
    if ($preloaded.Count -gt 0) { throw 'A preloaded Assert-V43SystemModuleBoundary command is not trusted.' }
    . $boundaryPath
    $trustedBoundary = $null
    foreach ($command in @(& Microsoft.PowerShell.Core\Get-Command -Name 'Assert-V43SystemModuleBoundary' -All -ListImported -ErrorAction SilentlyContinue)) {
        if ($command.CommandType -eq 'Function' -and [IO.Path]::GetFullPath([string]$command.ScriptBlock.Ast.Extent.File) -ceq $boundaryPath) {
            $trustedBoundary = $command
            break
        }
    }
    if ($null -eq $trustedBoundary) { throw 'Canonical package-local system-module helper did not load.' }
    $boundaryResult = & $trustedBoundary.ScriptBlock
    if ($boundaryResult.status -cne 'PASS') { throw 'System-module boundary did not pass.' }
}
finally { $boundaryHandle.Dispose() }
Microsoft.PowerShell.Core\Set-StrictMode -Version Latest

$repoRoot = [IO.Path]::GetFullPath((Join-Path $packageRoot '..\..\..'))
$auditRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit'))
$CandidateManifestPath = [IO.Path]::GetFullPath($CandidateManifestPath)
$SourceFreezePath = [IO.Path]::GetFullPath($SourceFreezePath)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if (-not $OutputPath.StartsWith($auditRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Receipt must remain under the external v43 audit root.' }
if ([IO.File]::Exists($OutputPath)) { throw "Refusing to overwrite single-use environment-preflight receipt: $OutputPath" }

$installedFileHandles = New-Object 'System.Collections.Generic.List[IO.FileStream]'
function Get-FileSha256([string]$Path, [bool]$RetainHandle = $false) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $actual = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
        if ($RetainHandle) { [void]$script:installedFileHandles.Add($stream); $stream = $null }
        return $actual
    }
    finally { $sha.Dispose(); if ($null -ne $stream) { $stream.Dispose() } }
}
function Get-TextSha256([string]$Text) {
    $bytes = (New-Object Text.UTF8Encoding($false, $true)).GetBytes($Text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Read-PinnedJson([string]$Path, [string]$Expected, [string]$Label) {
    if ($Expected -notmatch '^[0-9a-fA-F]{64}$') { throw "$Label external SHA256 pin is malformed." }
    if (-not [IO.File]::Exists($Path)) { throw "$Label is missing." }
    $bytes = [IO.File]::ReadAllBytes($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $actual = ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
    if ($actual -cne $Expected.ToLowerInvariant()) { throw "$Label differs from its external SHA256 pin." }
    $text = (New-Object Text.UTF8Encoding($false, $true)).GetString($bytes)
    $value = Microsoft.PowerShell.Utility\ConvertFrom-Json -InputObject $text -ErrorAction Stop
    if ($null -eq $value -or $value -isnot [Management.Automation.PSCustomObject]) { throw "$Label root must be a JSON object." }
    return [pscustomobject]@{ value = $value; sha256 = $actual }
}
function Assert-NoGoEnvelope($Value, [string]$Label, [bool]$RequireStaticOnly) {
    if ($null -eq $Value -or $Value -isnot [Management.Automation.PSCustomObject]) { throw "$Label must be an object." }
    $go = $Value.PSObject.Properties['goPermitted']
    if ($null -eq $go -or $go.Value -isnot [bool] -or $go.Value) { throw "$Label.goPermitted must be JSON false." }
    if ($RequireStaticOnly) {
        $static = $Value.PSObject.Properties['staticOnly']
        if ($null -eq $static -or $static.Value -isnot [bool] -or -not $static.Value) { throw "$Label.staticOnly must be JSON true." }
    }
    $actions = $Value.PSObject.Properties['productActions']
    if ($null -eq $actions -or $actions.Value -isnot [Management.Automation.PSCustomObject]) { throw "$Label.productActions must be an object." }
    foreach ($action in @('guiLaunched','savePerformed','runPerformed','confirmPerformed')) {
        $field = $actions.Value.PSObject.Properties[$action]
        if ($null -eq $field -or $field.Value -isnot [bool] -or $field.Value) { throw "$Label.productActions.$action must be JSON false." }
    }
}
function Assert-StringField($Value, [string]$Name, [string]$Label) {
    if ($null -eq $Value -or $Value -isnot [Management.Automation.PSCustomObject]) { throw "$Label must be an object." }
    $field = $Value.PSObject.Properties[$Name]
    if ($null -eq $field -or $field.Value -isnot [string]) { throw "$Label.$Name must be a JSON string scalar." }
    return [string]$field.Value
}
function Assert-ObjectField($Value, [string]$Name, [string]$Label) {
    if ($null -eq $Value -or $Value -isnot [Management.Automation.PSCustomObject]) { throw "$Label must be an object." }
    $field = $Value.PSObject.Properties[$Name]
    if ($null -eq $field -or $field.Value -isnot [Management.Automation.PSCustomObject]) { throw "$Label.$Name must be a JSON object." }
    return $field.Value
}
function Assert-NoReparseAncestors([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $cursor = $full
    while (-not [string]::IsNullOrWhiteSpace($cursor)) {
        if ([IO.File]::Exists($cursor) -or [IO.Directory]::Exists($cursor)) {
            if (([IO.File]::GetAttributes($cursor) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Reparse-point path is not trusted: $cursor" }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -ceq $cursor) { break }
        $cursor = $parent
    }
}
function Assert-SourceFilePin($Freeze, [string]$RelativePath, [string]$ActualPath) {
    $sourceFilesProperty = $Freeze.PSObject.Properties['sourceFiles']
    if ($null -eq $sourceFilesProperty -or $sourceFilesProperty.Value -isnot [array]) { throw 'Source freeze sourceFiles must be a JSON array.' }
    $rows = @($Freeze.sourceFiles | Where-Object { [string]$_.path -ceq $RelativePath })
    if ($rows.Count -ne 1) { throw "Source freeze must contain exactly one row for $RelativePath." }
    if ($rows[0] -isnot [Management.Automation.PSCustomObject] -or
        $rows[0].PSObject.Properties['path'].Value -isnot [string] -or
        $rows[0].PSObject.Properties['sha256'].Value -isnot [string]) { throw "Source freeze row for $RelativePath is malformed." }
    $actual = Get-FileSha256 $ActualPath
    if ($actual -cne ([string]$rows[0].sha256).ToLowerInvariant()) { throw "Current source bytes differ from the source freeze: $RelativePath" }
    return $actual
}

$checks = New-Object 'System.Collections.Generic.List[object]'
$failure = $null
$manifest = $null
$freeze = $null
$candidateBound = $false
$validatedInventorySha256 = $null
$validatedBuildReceiptSha256 = $null
try {
    $manifestSnapshot = Read-PinnedJson $CandidateManifestPath $CandidateManifestSha256 'Candidate manifest'
    $manifest = $manifestSnapshot.value
    if ((Assert-StringField $manifest 'schema' 'Candidate manifest') -cne 'range.pc11-fresh-run-v43-admission-candidate/v1' -or
        (Assert-StringField $manifest 'candidateVersion' 'Candidate manifest') -cne 'v43') { throw 'Candidate manifest schema/version is invalid.' }
    Assert-NoGoEnvelope $manifest 'Candidate manifest' $false
    $installBinding = Assert-ObjectField $manifest 'installBinding' 'Candidate manifest'
    $sourcePins = Assert-ObjectField $manifest 'sourceAndAuthorityPins' 'Candidate manifest'
    $candidateState = Assert-StringField $manifest 'candidateState' 'Candidate manifest'
    $installRootValue = Assert-StringField $installBinding 'installRoot' 'Candidate installBinding'
    $sourceTreeValue = Assert-StringField $sourcePins 'sourceTreeSha256' 'Candidate source pins'
    $sourceRootValue = Assert-StringField $sourcePins 'sourceRootPath' 'Candidate source pins'
    $freezePathValue = Assert-StringField $sourcePins 'sourceFreezeReceiptPath' 'Candidate source pins'
    $sourceFreezeHashValue = Assert-StringField $sourcePins 'sourceFreezeReceiptSha256' 'Candidate source pins'
    if ($candidateState -match '^PREPARATION_ONLY_' -or
        $installRootValue -match '^PENDING_' -or
        $sourceTreeValue -match '^PENDING_' -or
        $sourceFreezeHashValue -match '^PENDING_') {
        [void]$checks.Add([pscustomobject]@{ id='candidate_completeness'; status='PENDING'; detail='Manifest contains preparation or pending candidate fields.' })
    } else {
        $freezeSnapshot = Read-PinnedJson $SourceFreezePath $SourceFreezeSha256 'Source freeze'
        $freeze = $freezeSnapshot.value
        if ((Assert-StringField $freeze 'schema' 'Source freeze') -cne 'range.pc11-v43-source-freeze/v1' -or
            (Assert-StringField $freeze 'status' 'Source freeze') -cne 'PASS' -or
            (Assert-StringField $freeze 'candidateVersion' 'Source freeze') -cne 'v43' -or
            (Assert-StringField $freeze 'variant' 'Source freeze') -cne 'OFF' -or
            (Assert-StringField $freeze 'configuration' 'Source freeze') -cne 'Release') { throw 'Source freeze is not a completed v43 OFF Release freeze.' }
        Assert-NoGoEnvelope $freeze 'Source freeze' $true
        if ([IO.Path]::GetFullPath($sourceRootValue) -cne $repoRoot -or
            [IO.Path]::GetFullPath($freezePathValue) -cne $SourceFreezePath -or
            [IO.Path]::GetFullPath((Assert-StringField $freeze 'sourceRootPath' 'Source freeze')) -cne $repoRoot) {
            throw 'Candidate source root or freeze receipt path does not match the current pinned checkout.'
        }
        if ($sourceTreeValue -cne (Assert-StringField $freeze 'sourceTreeSha256' 'Source freeze') -or
            (Assert-StringField $sourcePins 'rootCommit' 'Candidate source pins') -cne (Assert-StringField $freeze 'rootCommit' 'Source freeze') -or
            $sourceFreezeHashValue -cne $freezeSnapshot.sha256) { throw 'Manifest source identity does not match the externally pinned freeze.' }

        $sourceRows = @($freeze.sourceFiles)
        if ($sourceRows.Count -lt 2) { throw 'Freeze source list is incomplete.' }
        $scriptRelative = 'scripts/packaging/piezo_gui_static_v43/tests/test-save-environment-preflight-v43.ps1'
        $helperRelative = 'scripts/packaging/piezo_gui_static_v43/tools/system-module-boundary-v43.ps1'
        $scriptHash = Assert-SourceFilePin $freeze $scriptRelative $PSCommandPath
        $helperHash = Assert-SourceFilePin $freeze $helperRelative (Join-Path $packageRoot 'tools\system-module-boundary-v43.ps1')
        if ($helperHash -cne $boundaryActual) { throw 'Executed system-module helper differs from the frozen source pin.' }
        [void]$checks.Add([pscustomobject]@{ id='source_freeze_bytes'; status='PASS'; sourcePreflightSha256=$scriptHash; systemModuleHelperSha256=$helperHash })

        $root = [IO.Path]::GetFullPath($installRootValue)
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw 'Candidate install root is missing.' }
        Assert-NoReparseAncestors $root
        if ((Assert-StringField $installBinding 'requiredVariant' 'Candidate installBinding') -cne 'OFF' -or
            (Assert-StringField $installBinding 'inventoryStatus' 'Candidate installBinding') -cne 'PASS') {
            throw 'Candidate installation is not a completed OFF PASS inventory.'
        }
        $inventorySchema = $installBinding.PSObject.Properties['inventorySchemaVersion']
        if ($null -eq $inventorySchema -or $inventorySchema.Value -isnot [int] -or $inventorySchema.Value -ne 2) { throw 'Candidate inventorySchemaVersion must be JSON integer 2.' }
        $inventoryPath = [IO.Path]::GetFullPath((Assert-StringField $installBinding 'inventoryReportPath' 'Candidate installBinding'))
        $inventoryPin = Assert-StringField $installBinding 'inventoryReportSha256' 'Candidate installBinding'
        Assert-NoReparseAncestors $inventoryPath
        $inventorySnapshot = Read-PinnedJson $inventoryPath $inventoryPin 'OFF inventory'
        $inventory = $inventorySnapshot.value
        if ((Assert-StringField $inventory 'inventoryStatus' 'OFF inventory') -cne 'PASS' -or
            (Assert-StringField $inventory 'variant' 'OFF inventory') -cne 'OFF' -or
            [IO.Path]::GetFullPath((Assert-StringField $inventory 'installRoot' 'OFF inventory')) -cne $root) {
            throw 'Pinned OFF inventory does not match the candidate installed root and PASS variant.'
        }
        $inventoryVersion = $inventory.PSObject.Properties['schemaVersion']
        $inventoryFileCount = $inventory.PSObject.Properties['fileCount']
        $inventoryFiles = $inventory.PSObject.Properties['files']
        $inventoryDirectories = $inventory.PSObject.Properties['directories']
        if ($null -eq $inventoryVersion -or $inventoryVersion.Value -isnot [int] -or $inventoryVersion.Value -ne 2 -or
            $null -eq $inventoryFileCount -or $inventoryFileCount.Value -isnot [int] -or
            $null -eq $inventoryFiles -or $inventoryFiles.Value -isnot [array] -or
            $null -eq $inventoryDirectories -or $inventoryDirectories.Value -isnot [array] -or
            $inventoryFileCount.Value -ne $inventoryFiles.Value.Count) { throw 'Pinned OFF inventory schema-v2 file and directory arrays are malformed.' }

        $buildPath = [IO.Path]::GetFullPath((Assert-StringField $sourcePins 'releaseBuildReceiptPath' 'Candidate source pins'))
        $buildPin = Assert-StringField $sourcePins 'releaseBuildReceiptSha256' 'Candidate source pins'
        Assert-NoReparseAncestors $buildPath
        $buildSnapshot = Read-PinnedJson $buildPath $buildPin 'OFF Release build receipt'
        $build = $buildSnapshot.value
        Assert-NoGoEnvelope $build 'OFF Release build receipt' $true
        if ((Assert-StringField $build 'schema' 'OFF Release build receipt') -cne 'range.pc11-v43-release-build-receipt/v1' -or
            (Assert-StringField $build 'status' 'OFF Release build receipt') -cne 'PASS' -or
            (Assert-StringField $build 'candidateVersion' 'OFF Release build receipt') -cne 'v43' -or
            (Assert-StringField $build 'variant' 'OFF Release build receipt') -cne 'OFF' -or
            (Assert-StringField $build 'configuration' 'OFF Release build receipt') -cne 'Release' -or
            (Assert-StringField $build 'sourceTreeSha256' 'OFF Release build receipt') -cne $sourceTreeValue -or
            (Assert-StringField $build 'rootCommit' 'OFF Release build receipt') -cne (Assert-StringField $sourcePins 'rootCommit' 'Candidate source pins') -or
            (Assert-StringField $freeze 'releaseBuildReceiptSha256' 'Source freeze') -cne $buildSnapshot.sha256 -or
            [IO.Path]::GetFullPath((Assert-StringField $inventory 'buildRoot' 'OFF inventory')) -cne
                [IO.Path]::GetFullPath((Assert-StringField $build 'buildDirectory' 'OFF Release build receipt'))) {
            throw 'OFF inventory, Release build, source freeze, and candidate source identity do not match.'
        }
        $buildExit = $build.PSObject.Properties['buildExitCode']
        if ($null -eq $buildExit -or $buildExit.Value -isnot [int] -or $buildExit.Value -ne 0) { throw 'OFF Release build did not exit successfully.' }
        $validatedInventorySha256 = $inventorySnapshot.sha256
        $validatedBuildReceiptSha256 = $buildSnapshot.sha256
        [void]$checks.Add([pscustomobject]@{ id='off_inventory_release_build_binding'; status='PASS'; inventorySha256=$validatedInventorySha256; releaseBuildReceiptSha256=$validatedBuildReceiptSha256 })

        $layout = Assert-ObjectField $manifest 'installedPathLayout' 'Candidate manifest'
        if ((Assert-StringField $layout 'privatePythonRelativePath' 'Candidate installedPathLayout') -cne 'runtime/python/python.exe') { throw 'Candidate private Python relative path is not the required install path.' }
        foreach ($pathRow in @(
            @('guiExecutableRelativePath','guiExecutableSha256'), @('privatePythonRelativePath','privatePythonSha256'),
            @('solverExecutableRelativePath','solverExecutableSha256'), @('productionControlRelativePath','productionControlSha256'),
            @('nativeBackendRelativePath','nativeBackendSha256')
        )) {
            $relative = Assert-StringField $layout $pathRow[0] 'Candidate installedPathLayout'
            $expected = Assert-StringField $layout $pathRow[1] 'Candidate installedPathLayout'
            if (-not $relative -or [IO.Path]::IsPathRooted($relative) -or $relative.Replace('\','/') -match '(^|/)\.\.(/|$)' -or $expected -notmatch '^[0-9a-fA-F]{64}$') { throw "Candidate installed path/hash is invalid: $($pathRow[0])" }
            $full = [IO.Path]::GetFullPath((Join-Path $root $relative))
            if (-not $full.StartsWith($root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or -not [IO.File]::Exists($full)) { throw "Candidate installed file is missing or escapes install root: $relative" }
            Assert-NoReparseAncestors $full
            if ((Get-FileSha256 $full $true) -cne $expected.ToLowerInvariant()) { throw "Candidate installed file hash mismatch: $relative" }
            $inventoryRows = @($inventoryFiles.Value | Where-Object { $_.path -ceq $relative.Replace('\','/') })
            if ($inventoryRows.Count -ne 1 -or $inventoryRows[0].sha256 -isnot [string] -or
                [string]$inventoryRows[0].sha256 -cne $expected.ToLowerInvariant()) { throw "Candidate installed file does not match the pinned inventory: $relative" }
        }
        [void]$checks.Add([pscustomobject]@{ id='private_install_paths'; status='PASS'; privatePythonRelativePath=[string]$layout.privatePythonRelativePath; privatePythonSha256=[string]$layout.privatePythonSha256 })
        $candidateBound = $true
    }

    # The canonical helper rejects same-name preloaded aliases/functions and non-system modules.
    [void]$checks.Add([pscustomobject]@{ id='powershell_command_resolution'; status='PASS'; moduleBoundary=$boundaryResult; pathSha256=(Get-TextSha256 ([string]$env:PATH)); psModulePathSha256=(Get-TextSha256 ([string]$env:PSModulePath)) })
} catch { $failure = $_.Exception.Message }

$status = if ($null -ne $failure) { 'FAIL' } elseif ($candidateBound -and @($checks | Where-Object { $_.status -eq 'PENDING' }).Count -eq 0) { 'PASS' } else { 'PENDING' }
$receipt = [ordered]@{
    schema='range.pc11-v43-save-environment-preflight/v1'
    status=$status
    candidateGateStatus=if ($status -eq 'PASS') { 'PASS' } else { 'PENDING' }
    candidateManifestPath=$CandidateManifestPath
    candidateManifestSha256=$CandidateManifestSha256.ToLowerInvariant()
    sourceFreezePath=$SourceFreezePath
    sourceFreezeSha256=$SourceFreezeSha256.ToLowerInvariant()
    systemModuleBoundarySha256=$boundaryActual
    validatedInventorySha256=$validatedInventorySha256
    validatedBuildReceiptSha256=$validatedBuildReceiptSha256
    checks=@($checks.ToArray())
    failure=$failure
    staticOnly=$true
    productActions=[ordered]@{ guiLaunched=$false; savePerformed=$false; runPerformed=$false; confirmPerformed=$false }
    goPermitted=$false
}
$parent = [IO.Path]::GetDirectoryName($OutputPath)
if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
Assert-NoReparseAncestors $parent
$receiptBytes = (New-Object Text.UTF8Encoding($false)).GetBytes(($receipt | Microsoft.PowerShell.Utility\ConvertTo-Json -Depth 12) + "`n")
$stream = $null
try {
    $stream = [IO.File]::Open($OutputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $stream.Write($receiptBytes, 0, $receiptBytes.Length)
}
finally {
    if ($null -ne $stream) { $stream.Dispose() }
    foreach ($installedHandle in $installedFileHandles) { try { $installedHandle.Dispose() } catch { } }
    $installedFileHandles.Clear()
}
Write-Output ("{0} candidateGate={1} productActions=NONE output={2}" -f $status, $receipt.candidateGateStatus, $OutputPath)
if ($status -eq 'FAIL') { exit 1 }
