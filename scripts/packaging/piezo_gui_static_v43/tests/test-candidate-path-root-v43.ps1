[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CandidateManifestPath,
    [Parameter(Mandatory = $true)][string]$CandidateManifestSha256,
    [Parameter(Mandatory = $true)][string]$AuditRoot,
    [Parameter(Mandatory = $true)][string]$OutputPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'Candidate path/root evidence requires Windows PowerShell 5.1.'
}

function Get-Sha256File([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}
function Assert-NoReparseAncestors([string]$Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    if ([IO.File]::Exists($fullPath) -or [IO.Directory]::Exists($fullPath)) {
        if (([IO.File]::GetAttributes($fullPath) -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing reparse-point path: $fullPath"
        }
    }
    $directoryPath = if ([IO.Directory]::Exists($fullPath)) { $fullPath } else { [IO.Path]::GetDirectoryName($fullPath) }
    $cursor = [IO.DirectoryInfo]$directoryPath
    while ($null -ne $cursor) {
        if ($cursor.Exists -and (($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw "Refusing reparse-point path ancestor: $($cursor.FullName)"
        }
        $cursor = $cursor.Parent
    }
}
function Assert-Object($Value, [string]$Label) {
    if ($null -eq $Value -or $Value -isnot [Management.Automation.PSCustomObject]) { throw "$Label must be a JSON object, not an array or scalar." }
}
function Assert-StringProperty($Object, [string]$Name, [string]$Label) {
    Assert-Object $Object $Label
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -isnot [string]) { throw "$Label.$Name must be a scalar JSON string." }
    return [string]$property.Value
}
function Assert-NoGoBoundary($Object, [string]$Label, [bool]$RequireStaticOnly = $true) {
    Assert-Object $Object $Label
    $go = $Object.PSObject.Properties['goPermitted']
    $static = $Object.PSObject.Properties['staticOnly']
    $actions = $Object.PSObject.Properties['productActions']
    if ($null -eq $go -or $go.Value -isnot [bool] -or $go.Value -or
        ($RequireStaticOnly -and ($null -eq $static -or $static.Value -isnot [bool] -or -not $static.Value))) {
        throw "$Label must preserve its no-GO product-action boundary."
    }
    Assert-Object $(if ($null -eq $actions) { $null } else { $actions.Value }) "$Label.productActions"
    foreach ($name in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        $action = $actions.Value.PSObject.Properties[$name]
        if ($null -eq $action -or $action.Value -isnot [bool] -or $action.Value) { throw "$Label.productActions.$name must be boolean false." }
    }
}
function Get-JsonSnapshot([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { throw "Required candidate evidence is missing: $Path" }
    $bytes = [IO.File]::ReadAllBytes($Path)
    $text = (New-Object Text.UTF8Encoding($false, $true)).GetString($bytes)
    $value = ConvertFrom-Json -InputObject $text -ErrorAction Stop
    Assert-Object $value "JSON root in $Path"
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
    return [pscustomobject]@{ value = $value; sha256 = $hash }
}
function Get-PinnedJsonSnapshot([string]$Path, [string]$ExpectedSha256, [string]$Label) {
    if ($ExpectedSha256 -notmatch '^[0-9a-fA-F]{64}$') { throw "$Label needs an external 64-character SHA256 pin." }
    $snapshot = Get-JsonSnapshot $Path
    if ($snapshot.sha256 -cne $ExpectedSha256.ToLowerInvariant()) { throw "$Label bytes differ from the external SHA256 pin." }
    return $snapshot
}
function Test-PendingValue([string]$Value) {
    return [string]::IsNullOrWhiteSpace($Value) -or $Value -match '^(?i)PENDING(?:_|$)'
}
function Resolve-CandidatePath([string]$Value) {
    if (Test-PendingValue $Value) { return $Value }
    if ([IO.Path]::IsPathRooted($Value)) { return [IO.Path]::GetFullPath($Value) }
    return [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetDirectoryName($CandidateManifestPath)) $Value))
}
function Assert-PostBuildSourceRows($Document, [string]$Root) {
    $rowsProperty = $Document.PSObject.Properties['sourceFiles']
    $countProperty = $Document.PSObject.Properties['sourceFileCount']
    if ($null -eq $rowsProperty -or $rowsProperty.Value -isnot [array] -or $null -eq $countProperty -or
        ($countProperty.Value -isnot [byte] -and $countProperty.Value -isnot [int16] -and $countProperty.Value -isnot [int32] -and $countProperty.Value -isnot [int64]) -or
        [long]$countProperty.Value -ne @($rowsProperty.Value).Count -or @($rowsProperty.Value).Count -eq 0) {
        throw 'Post-build source receipt sourceFiles/sourceFileCount fields are malformed.'
    }
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    foreach ($row in $rowsProperty.Value) {
        Assert-Object $row 'Post-build source file row'
        $relative = Assert-StringProperty $row 'path' 'Post-build source file row'
        $expectedHash = Assert-StringProperty $row 'sha256' 'Post-build source file row'
        $size = $row.PSObject.Properties['sizeBytes']
        if ($expectedHash -notmatch '^[0-9a-fA-F]{64}$' -or $null -eq $size -or
            ($size.Value -isnot [byte] -and $size.Value -isnot [int16] -and $size.Value -isnot [int32] -and $size.Value -isnot [int64])) { throw 'Post-build source file row hash or size is malformed.' }
        if ([IO.Path]::IsPathRooted($relative)) { throw 'Post-build source file row path must be relative to sourceRootPath.' }
        $full = [IO.Path]::GetFullPath((Join-Path $Root ($relative.Replace('/', '\'))))
        if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Post-build source file row escaped sourceRootPath.' }
        Assert-NoReparseAncestors $full
        if (-not [IO.File]::Exists($full)) { throw "Frozen source file is missing: $relative" }
        if ((Get-Sha256File $full) -cne $expectedHash.ToLowerInvariant()) { throw "Frozen source file hash changed: $relative" }
        if ((New-Object IO.FileInfo($full)).Length -ne [long]$size.Value) { throw "Frozen source file size changed: $relative" }
    }
}
function Quote-Arg([string]$Value) {
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    return '"' + $Value.Replace('\\', '\\\\').Replace('"', '\\"') + '"'
}
function Invoke-Component([string]$ScriptPath, [string[]]$Arguments) {
    $exe = [IO.Path]::GetFullPath((Join-Path $PSHOME 'powershell.exe'))
    $lineArgs = @('-NoProfile', '-NoLogo', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $ScriptPath) + $Arguments
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $exe
    $info.Arguments = [string]::Join(' ', @($lineArgs | ForEach-Object { Quote-Arg ([string]$_) }))
    $info.WorkingDirectory = $PSScriptRoot
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    try {
        if (-not $process.Start()) { throw "Could not start static component: $ScriptPath" }
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $stdout = $process.StandardOutput.ReadToEnd()
        $process.WaitForExit()
        return [pscustomobject]@{ exitCode = [int]$process.ExitCode; stdout = $stdout.Trim(); stderr = $stderrTask.Result.Trim() }
    }
    finally { $process.Dispose() }
}

$CandidateManifestPath = [IO.Path]::GetFullPath($CandidateManifestPath)
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
Assert-NoReparseAncestors $CandidateManifestPath
Assert-NoReparseAncestors $AuditRoot
Assert-NoReparseAncestors ([IO.Path]::GetDirectoryName($OutputPath))
if ($CandidateManifestSha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'CandidateManifestSha256 must be a pinned 64-character SHA256.' }
if (-not $OutputPath.StartsWith($AuditRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Receipt must be written beneath the external audit root.' }
if (-not [IO.Directory]::Exists($AuditRoot) -or -not [IO.Directory]::Exists([IO.Path]::GetDirectoryName($OutputPath))) { throw 'External audit root and receipt parent must already exist.' }
if ([IO.File]::Exists($OutputPath)) { throw "Refusing to overwrite single-use candidate path/root receipt: $OutputPath" }

$missing = New-Object 'System.Collections.Generic.List[string]'
$postBuildHash = $null
$postBuild = $null
$snapshot = Get-PinnedJsonSnapshot $CandidateManifestPath $CandidateManifestSha256 'Candidate manifest'
$candidateHash = $snapshot.sha256
$manifest = $snapshot.value
if ((Assert-StringProperty $manifest 'schema' 'Candidate manifest') -cne 'range.pc11-fresh-run-v43-admission-candidate/v1') { throw 'Candidate manifest schema mismatch.' }
if ((Assert-StringProperty $manifest 'candidateVersion' 'Candidate manifest') -cne 'v43') { throw 'Candidate manifest candidateVersion must be v43.' }
$candidateState = Assert-StringProperty $manifest 'candidateState' 'Candidate manifest'
$goPermitted = $manifest.PSObject.Properties['goPermitted']
if ($null -eq $goPermitted -or $goPermitted.Value -isnot [bool] -or $goPermitted.Value) { throw 'Candidate manifest must preserve goPermitted=false.' }
$actions = $manifest.PSObject.Properties['productActions']
Assert-Object $(if ($null -eq $actions) { $null } else { $actions.Value }) 'Candidate manifest.productActions'
foreach ($name in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
    $action = $actions.Value.PSObject.Properties[$name]
    if ($null -eq $action -or $action.Value -isnot [bool] -or $action.Value) { throw "Candidate manifest productActions.$name must be boolean false." }
}

$binding = $manifest.PSObject.Properties['installBinding']
Assert-Object $(if ($null -eq $binding) { $null } else { $binding.Value }) 'Candidate manifest.installBinding'
$source = $manifest.PSObject.Properties['sourceAndAuthorityPins']
Assert-Object $(if ($null -eq $source) { $null } else { $source.Value }) 'Candidate manifest.sourceAndAuthorityPins'
$evidence = $manifest.PSObject.Properties['candidateEvidencePins']
Assert-Object $(if ($null -eq $evidence) { $null } else { $evidence.Value }) 'Candidate manifest.candidateEvidencePins'
$requiredIdsProperty = $evidence.Value.PSObject.Properties['requiredStaticTestIds']
if ($null -eq $requiredIdsProperty -or $requiredIdsProperty.Value -isnot [array] -or
    @($requiredIdsProperty.Value | Where-Object { $_ -isnot [string] }).Count -gt 0 -or
    @($requiredIdsProperty.Value) -cnotcontains 'path_shadow' -or @($requiredIdsProperty.Value) -cnotcontains 'root_seal') {
    throw 'Candidate manifest must pin scalar requiredStaticTestIds entries including path_shadow and root_seal.'
}

$installRootValue = Assert-StringProperty $binding.Value 'installRoot' 'Candidate manifest.installBinding'
$inventoryPathValue = Assert-StringProperty $binding.Value 'inventoryReportPath' 'Candidate manifest.installBinding'
$requiredVariant = Assert-StringProperty $binding.Value 'requiredVariant' 'Candidate manifest.installBinding'
$inventorySha = Assert-StringProperty $binding.Value 'inventoryReportSha256' 'Candidate manifest.installBinding'
$inventoryStatus = Assert-StringProperty $binding.Value 'inventoryStatus' 'Candidate manifest.installBinding'
$inventorySchema = $binding.Value.PSObject.Properties['inventorySchemaVersion']
$sourceRoot = Assert-StringProperty $source.Value 'sourceRootPath' 'Candidate manifest.sourceAndAuthorityPins'
$freezePathValue = Assert-StringProperty $source.Value 'sourceFreezeReceiptPath' 'Candidate manifest.sourceAndAuthorityPins'
$freezeSha = Assert-StringProperty $source.Value 'sourceFreezeReceiptSha256' 'Candidate manifest.sourceAndAuthorityPins'
$sourceTreeSha = Assert-StringProperty $source.Value 'sourceTreeSha256' 'Candidate manifest.sourceAndAuthorityPins'
$buildPathValue = Assert-StringProperty $source.Value 'releaseBuildReceiptPath' 'Candidate manifest.sourceAndAuthorityPins'
$buildSha = Assert-StringProperty $source.Value 'releaseBuildReceiptSha256' 'Candidate manifest.sourceAndAuthorityPins'
$authority = $source.Value.PSObject.Properties['authorityBinding']
if ($null -eq $authority) { $missing.Add('fresh original authority receipt and ten authority rows are absent') }
else {
    Assert-Object $authority.Value 'Candidate manifest.sourceAndAuthorityPins.authorityBinding'
    $authorityPath = Assert-StringProperty $authority.Value 'authorityReceiptPath' 'Candidate manifest.sourceAndAuthorityPins.authorityBinding'
    $authoritySha = Assert-StringProperty $authority.Value 'authorityReceiptSha256' 'Candidate manifest.sourceAndAuthorityPins.authorityBinding'
    if ((Test-PendingValue $authorityPath) -or (Test-PendingValue $authoritySha)) { $missing.Add('fresh original authority receipt and ten authority rows are absent') }
}
$installRootValue = Resolve-CandidatePath $installRootValue
$inventoryPathValue = Resolve-CandidatePath $inventoryPathValue
$sourceRoot = Resolve-CandidatePath $sourceRoot
$freezePathValue = Resolve-CandidatePath $freezePathValue
$buildPathValue = Resolve-CandidatePath $buildPathValue
if ($requiredVariant -cne 'OFF') { throw 'Candidate installBinding.requiredVariant must be OFF.' }
if ($null -eq $inventorySchema -or $inventorySchema.Value -isnot [int] -or [int]$inventorySchema.Value -ne 2) { throw 'Candidate installBinding.inventorySchemaVersion must be integer 2.' }

foreach ($pair in @(@('installed root', $installRootValue), @('OFF inventory report path', $inventoryPathValue), @('OFF inventory SHA256', $inventorySha), @('source root', $sourceRoot), @('source freeze receipt path', $freezePathValue), @('source freeze receipt SHA256', $freezeSha), @('source tree SHA256', $sourceTreeSha), @('OFF release build receipt path', $buildPathValue), @('OFF release build receipt SHA256', $buildSha))) {
    if (Test-PendingValue ([string]$pair[1])) { $missing.Add("$($pair[0]) is pending") }
}

if ($inventorySha -notmatch '^[0-9a-fA-F]{64}$') { $missing.Add('OFF inventory SHA256 is not available as a valid pin') }
elseif (-not (Test-PendingValue $inventoryPathValue) -and [IO.File]::Exists($inventoryPathValue)) {
    Assert-NoReparseAncestors $inventoryPathValue
    $inventory = (Get-PinnedJsonSnapshot $inventoryPathValue $inventorySha 'OFF inventory').value
    if ((Assert-StringProperty $inventory 'variant' 'OFF inventory') -cne 'OFF') { throw 'Installed inventory variant must be OFF.' }
    if ((Assert-StringProperty $inventory 'inventoryStatus' 'OFF inventory') -cne 'PASS') { $missing.Add('OFF inventory report does not have PASS status') }
    if ($inventoryStatus -cne 'PASS') { $missing.Add('candidate OFF inventoryStatus is not PASS') }
    $reportRoot = Assert-StringProperty $inventory 'installRoot' 'OFF inventory'
    if (([IO.Path]::GetFullPath($reportRoot)).TrimEnd('\') -ine ([IO.Path]::GetFullPath($installRootValue)).TrimEnd('\')) { throw 'OFF inventory installRoot differs from the candidate install root.' }
    $schemaVersion = $inventory.PSObject.Properties['schemaVersion']; $fileCount = $inventory.PSObject.Properties['fileCount']; $files = $inventory.PSObject.Properties['files']
    if ($null -eq $schemaVersion -or $schemaVersion.Value -isnot [int] -or [int]$schemaVersion.Value -ne 2 -or
        $null -eq $fileCount -or ($fileCount.Value -isnot [int] -and $fileCount.Value -isnot [long]) -or
        $null -eq $files -or $files.Value -isnot [array] -or [long]$fileCount.Value -ne @($files.Value).Count) {
        throw 'OFF inventory schemaVersion/fileCount/files fields are malformed or inconsistent.'
    }
} elseif (-not (Test-PendingValue $inventoryPathValue)) { $missing.Add('pinned OFF inventory report file is missing') }
if (-not (Test-PendingValue $installRootValue)) {
    if (-not [IO.Directory]::Exists($installRootValue)) { $missing.Add('candidate installed root directory is missing') }
    else { Assert-NoReparseAncestors $installRootValue }
}
if (-not (Test-PendingValue $sourceRoot)) {
    if (-not [IO.Directory]::Exists($sourceRoot)) { $missing.Add('candidate frozen source root directory is missing') }
    else { Assert-NoReparseAncestors $sourceRoot }
}

if ($freezeSha -notmatch '^[0-9a-fA-F]{64}$') { $missing.Add('source freeze receipt SHA256 is not available as a valid pin') }
elseif (-not (Test-PendingValue $freezePathValue) -and [IO.File]::Exists($freezePathValue)) {
    Assert-NoReparseAncestors $freezePathValue
    $freeze = (Get-PinnedJsonSnapshot $freezePathValue $freezeSha 'Source freeze receipt').value
    Assert-NoGoBoundary $freeze 'Source freeze receipt'
    if ((Assert-StringProperty $freeze 'schema' 'Source freeze receipt') -cne 'range.pc11-v43-source-freeze/v1' -or
        (Assert-StringProperty $freeze 'variant' 'Source freeze receipt') -cne 'OFF' -or
        (Assert-StringProperty $freeze 'candidateVersion' 'Source freeze receipt') -cne 'v43') { throw 'Source freeze receipt schema or candidate identity differs from v43 OFF.' }
    if ((Assert-StringProperty $freeze 'status' 'Source freeze receipt') -cne 'PASS') { $missing.Add('source freeze receipt does not have PASS status') }
    if ((Assert-StringProperty $freeze 'sourceTreeSha256' 'Source freeze receipt') -cne $sourceTreeSha) { throw 'Source freeze sourceTreeSha256 differs from the candidate pin.' }
    if ((Assert-StringProperty $freeze 'sourceRootPath' 'Source freeze receipt') -ine $sourceRoot) { throw 'Source freeze sourceRootPath differs from the candidate pin.' }
    if ((Assert-StringProperty $freeze 'releaseBuildReceiptSha256' 'Source freeze receipt') -cne $buildSha) { throw 'Source freeze release build SHA256 differs from the candidate pin.' }
    $postBuildHash = $freeze.PSObject.Properties['postBuildSourcePreflightReceiptSha256']
    $postBuildPath = $freeze.PSObject.Properties['postBuildSourcePreflightReceiptPath']
    if ($null -eq $postBuildHash -or $postBuildHash.Value -isnot [string] -or $postBuildHash.Value -notmatch '^[0-9a-fA-F]{64}$' -or
        $null -eq $postBuildPath -or $postBuildPath.Value -isnot [string] -or (Test-PendingValue ([string]$postBuildPath.Value))) {
        $missing.Add('post-build source hash receipt is absent or malformed')
    } elseif (-not [IO.File]::Exists([string]$postBuildPath.Value)) {
        $missing.Add('pinned post-build source hash receipt file is missing')
    } else {
        Assert-NoReparseAncestors ([string]$postBuildPath.Value)
        $postBuild = (Get-PinnedJsonSnapshot ([string]$postBuildPath.Value) ([string]$postBuildHash.Value) 'Post-build source receipt').value
        Assert-NoGoBoundary $postBuild 'Post-build source receipt' $false
        if ((Assert-StringProperty $postBuild 'schema' 'Post-build source receipt') -cne 'range.pc11-v43-source-preflight/v1' -or
            (Assert-StringProperty $postBuild 'status' 'Post-build source receipt') -cne 'PASS_SOURCE_PREFLIGHT' -or
            (Assert-StringProperty $postBuild 'sourceTreeSha256' 'Post-build source receipt') -cne $sourceTreeSha) {
            throw 'Post-build source receipt does not match the frozen source tree.'
        }
        Assert-PostBuildSourceRows $postBuild $sourceRoot
    }
} elseif (-not (Test-PendingValue $freezePathValue)) { $missing.Add('pinned source freeze receipt file is missing') }
if ($buildSha -notmatch '^[0-9a-fA-F]{64}$') { $missing.Add('OFF Release build SHA256 is not available as a valid pin') }
elseif (-not (Test-PendingValue $buildPathValue) -and [IO.File]::Exists($buildPathValue)) {
    Assert-NoReparseAncestors $buildPathValue
    $build = (Get-PinnedJsonSnapshot $buildPathValue $buildSha 'OFF Release build receipt').value
    Assert-NoGoBoundary $build 'OFF Release build receipt'
    if ((Assert-StringProperty $build 'schema' 'OFF Release build receipt') -cne 'range.pc11-v43-release-build-receipt/v1' -or
        (Assert-StringProperty $build 'status' 'OFF Release build receipt') -cne 'PASS' -or
        (Assert-StringProperty $build 'candidateVersion' 'OFF Release build receipt') -cne 'v43' -or
        (Assert-StringProperty $build 'variant' 'OFF Release build receipt') -cne 'OFF' -or
        (Assert-StringProperty $build 'configuration' 'OFF Release build receipt') -cne 'Release' -or
        (Assert-StringProperty $build 'sourceTreeSha256' 'OFF Release build receipt') -cne $sourceTreeSha) {
        throw 'OFF Release build receipt does not match the candidate source identity.'
    }
} elseif (-not (Test-PendingValue $buildPathValue)) { $missing.Add('pinned OFF Release build receipt file is missing') }

# Run the extant synthetic component suites in isolated outputs. Their PASS result is fixture evidence only.
$pathShadowPath = Join-Path ([IO.Path]::GetDirectoryName($OutputPath)) (([IO.Path]::GetFileNameWithoutExtension($OutputPath)) + '.path-shadow-fixture.json')
$componentPath = Join-Path $PSScriptRoot 'test-path-shadow-v43.ps1'
$bootstrapPath = Join-Path $PSScriptRoot 'test-root-bootstrap-v43.ps1'
foreach ($component in @($componentPath, $bootstrapPath)) { if (-not [IO.File]::Exists($component)) { throw "Required static component is missing: $component" } }
$pathShadowRun = Invoke-Component $componentPath @('-OutputPath', $pathShadowPath, '-AuditRoot', $AuditRoot)
$bootstrapRun = Invoke-Component $bootstrapPath @()
$componentResults = @(
    [ordered]@{ testId = 'path_shadow'; component = 'test-path-shadow-v43.ps1'; status = if ($pathShadowRun.exitCode -eq 0) { 'PASS_FIXTURE_ONLY' } else { 'FAIL' }; exitCode = $pathShadowRun.exitCode; output = $pathShadowRun.stdout; error = $pathShadowRun.stderr; receiptPath = $pathShadowPath; receiptSha256 = if ([IO.File]::Exists($pathShadowPath)) { Get-Sha256File $pathShadowPath } else { $null } },
    [ordered]@{ testId = 'root_seal'; component = 'test-root-bootstrap-v43.ps1'; status = if ($bootstrapRun.exitCode -eq 0) { 'PASS_FIXTURE_ONLY' } else { 'FAIL' }; exitCode = $bootstrapRun.exitCode; output = $bootstrapRun.stdout; error = $bootstrapRun.stderr; receiptPath = $null; receiptSha256 = $null }
)
if (@($componentResults | Where-Object { $_.status -ceq 'FAIL' }).Count -gt 0) { throw 'One or more synthetic path/root component suites failed.' }
if ($candidateState -notmatch '^(?i)PREPARATION_ONLY_' -and $missing.Count -eq 0) {
    $missing.Add('candidate-bound source, installed-root and independent-authority path proof is not implemented by the synthetic fixtures')
}
$status = 'PENDING'
$receipt = [ordered]@{
    schema = 'range.pc11-v43-candidate-path-root-receipt/v1'
    status = $status
    candidateVersion = 'v43'
    variant = 'OFF'
    staticOnly = $true
    goPermitted = $false
    candidateManifestPath = $CandidateManifestPath
    candidateManifestSha256 = $candidateHash
    candidateState = $candidateState
    sourceFreezeReceiptPath = if (Test-PendingValue $freezePathValue) { $null } else { $freezePathValue }
    sourceFreezeReceiptSha256 = if ($freezeSha -match '^[0-9a-fA-F]{64}$') { $freezeSha.ToLowerInvariant() } else { $null }
    postBuildSourcePreflightReceiptSha256 = if ($null -ne $postBuildHash -and $postBuildHash.Value -is [string]) { $postBuildHash.Value } else { $null }
    sourceTreeSha256 = if ($sourceTreeSha -match '^[0-9a-fA-F]{64}$') { $sourceTreeSha.ToLowerInvariant() } else { $null }
    installedRoot = if (Test-PendingValue $installRootValue) { $null } else { $installRootValue }
    inventoryReportPath = if (Test-PendingValue $inventoryPathValue) { $null } else { $inventoryPathValue }
    inventoryReportSha256 = if ($inventorySha -match '^[0-9a-fA-F]{64}$') { $inventorySha.ToLowerInvariant() } else { $null }
    requiredTestIds = @('path_shadow', 'root_seal')
    componentResults = $componentResults
    missingPrerequisites = @($missing | Select-Object -Unique)
    productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
}
Assert-NoReparseAncestors $AuditRoot
Assert-NoReparseAncestors ([IO.Path]::GetDirectoryName($OutputPath))
if (-not $OutputPath.StartsWith($AuditRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Receipt escaped the external audit root after revalidation.' }
$receiptBytes = (New-Object Text.UTF8Encoding($false)).GetBytes(($receipt | ConvertTo-Json -Depth 12))
$receiptStream = [IO.File]::Open($OutputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $receiptStream.Write($receiptBytes, 0, $receiptBytes.Length); $receiptStream.Flush($true) }
finally { $receiptStream.Dispose() }
Write-Output ("{0} path_shadow={1} root_seal={2} candidateSha256={3} receipt={4}" -f $status, $componentResults[0].status, $componentResults[1].status, $candidateHash, $OutputPath)
exit 0
