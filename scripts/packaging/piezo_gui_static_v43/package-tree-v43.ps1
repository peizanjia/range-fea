[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CandidateManifestPath,
    [Parameter(Mandatory = $true)][string]$CandidateManifestSha256,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$InventoryAdapterPath,
    [string]$AuditRoot,
    [string]$RunId
)

$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'This package-tree verifier requires Windows PowerShell 5.1.'
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
if (-not $PSBoundParameters.ContainsKey('RunId')) { $RunId = 'package-tree-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8) }
if (-not $InventoryAdapterPath) { $InventoryAdapterPath = Join-Path $PSScriptRoot 'inventory_adapter_v43.py' }
if (-not $AuditRoot) { $AuditRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit')) }
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)
$CandidateManifestPath = [IO.Path]::GetFullPath($CandidateManifestPath)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$InventoryAdapterPath = [IO.Path]::GetFullPath($InventoryAdapterPath)

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
function Read-Json([string]$Path, [string]$ExpectedSha256) {
    if (-not [IO.File]::Exists($Path)) { throw "Required JSON file is missing: $Path" }
    $bytes = [IO.File]::ReadAllBytes($Path)
    Assert-Hash (Get-Sha256Bytes $bytes) $ExpectedSha256 "Pinned JSON file $Path"
    $text = (New-Object Text.UTF8Encoding($false, $true)).GetString($bytes)
    return ($text | ConvertFrom-Json -ErrorAction Stop)
}
function Assert-JsonObject($Value, [string]$Label) {
    if ($null -eq $Value -or -not ($Value -is [pscustomobject])) { throw "$Label must be a JSON object." }
}
function Assert-JsonString($Value, [string]$Field, [string]$Label) {
    Assert-JsonObject $Value $Label
    if (-not ($Value.$Field -is [string]) -or [string]::IsNullOrWhiteSpace($Value.$Field)) {
        throw "$Label.$Field must be a nonempty JSON string scalar."
    }
}
function Assert-JsonBoolean($Value, [string]$Field, [string]$Label, [bool]$Expected) {
    Assert-JsonObject $Value $Label
    if (-not ($Value.$Field -is [bool]) -or $Value.$Field -ne $Expected) {
        throw "$Label.$Field must be the expected JSON Boolean scalar."
    }
}
function Assert-JsonInteger($Value, [string]$Field, [string]$Label, [long]$Expected) {
    Assert-JsonObject $Value $Label
    if ((-not ($Value.$Field -is [int]) -and -not ($Value.$Field -is [long])) -or $Value.$Field -ne $Expected) {
        throw "$Label.$Field must be the expected JSON integer scalar."
    }
}
function Compare-Utf8Text([string]$Left, [string]$Right) {
    $leftBytes = [Text.Encoding]::UTF8.GetBytes($Left)
    $rightBytes = [Text.Encoding]::UTF8.GetBytes($Right)
    $limit = [Math]::Min($leftBytes.Length, $rightBytes.Length)
    for ($i = 0; $i -lt $limit; $i++) {
        if ($leftBytes[$i] -lt $rightBytes[$i]) { return -1 }
        if ($leftBytes[$i] -gt $rightBytes[$i]) { return 1 }
    }
    if ($leftBytes.Length -lt $rightBytes.Length) { return -1 }
    if ($leftBytes.Length -gt $rightBytes.Length) { return 1 }
    return 0
}
function ConvertTo-JsonString([string]$Value) { return [string]($Value | ConvertTo-Json -Compress -ErrorAction Stop) }
function Get-ManifestIdentitySha256($Manifest) {
    Assert-JsonObject $Manifest 'Candidate manifest'
    $binding = $Manifest.installBinding
    $source = $Manifest.sourceAndAuthorityPins
    Assert-JsonObject $binding 'Candidate manifest.installBinding'
    Assert-JsonObject $source 'Candidate manifest.sourceAndAuthorityPins'
    foreach ($field in @('installRoot', 'inventoryReportSha256')) {
        Assert-JsonString $binding $field 'Candidate manifest.installBinding'
    }
    foreach ($field in @('sourceFreezeReceiptSha256', 'rootCommit', 'sourceTreeSha256', 'releaseBuildReceiptSha256')) {
        Assert-JsonString $source $field 'Candidate manifest.sourceAndAuthorityPins'
    }
    $rootCommit = ([string]$source.rootCommit).ToLowerInvariant()
    if ($rootCommit -notmatch '^(?:[0-9a-f]{40}|[0-9a-f]{64})$') { throw 'Candidate rootCommit is malformed.' }
    $parts = @(
        '{"candidateVersion":"v43"',
        ',"installRoot":' + (ConvertTo-JsonString ([string]$binding.installRoot)),
        ',"inventoryReportSha256":' + (ConvertTo-JsonString ([string]$binding.inventoryReportSha256).ToLowerInvariant()),
        ',"releaseBuildReceiptSha256":' + (ConvertTo-JsonString ([string]$source.releaseBuildReceiptSha256).ToLowerInvariant()),
        ',"rootCommit":' + (ConvertTo-JsonString $rootCommit),
        ',"schema":"range.pc11-fresh-run-v43-admission-candidate/v1"',
        ',"sourceFreezeReceiptSha256":' + (ConvertTo-JsonString ([string]$source.sourceFreezeReceiptSha256).ToLowerInvariant()),
        ',"sourceTreeSha256":' + (ConvertTo-JsonString ([string]$source.sourceTreeSha256).ToLowerInvariant()) + '}'
    )
    return Get-Sha256Bytes ((New-Object Text.UTF8Encoding($false, $true)).GetBytes([string]::Join('', $parts)))
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
    return ,$result.ToArray()
}
function Assert-ScalarPinEqual($Left, $Right, [string]$Label, [string]$Pattern = '^[0-9a-fA-F]{64}$') {
    if (-not ($Left -is [string]) -or -not ($Right -is [string]) -or
        $Left -notmatch $Pattern -or $Right -notmatch $Pattern -or
        $Left.ToLowerInvariant() -cne $Right.ToLowerInvariant()) {
        throw "$Label differs across the v43 candidate, freeze, and OFF build."
    }
}
function Assert-StrictNoGo($Value, [string]$Label, [bool]$RequireStaticOnly) {
    Assert-JsonObject $Value $Label
    if (-not ($Value.goPermitted -is [bool]) -or $Value.goPermitted -ne $false -or
        ($RequireStaticOnly -and (-not ($Value.staticOnly -is [bool]) -or $Value.staticOnly -ne $true))) {
        throw "$Label is not a typed no-GO receipt."
    }
    $actions = $Value.productActions
    Assert-JsonObject $actions "$Label.productActions"
    $required = @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')
    if ($null -eq $actions -or @($actions.PSObject.Properties.Name).Count -ne $required.Count) {
        throw "$Label has an incomplete product-action boundary."
    }
    foreach ($name in $required) {
        $action = $actions.$name
        if (-not ($action -is [bool]) -or $action -ne $false) { throw "$Label permits product action $name." }
    }
}
function Assert-SubmodulePinsEqual($Left, $Right, [string]$Label) {
    foreach ($rows in @($Left, $Right)) {
        if (-not ($rows -is [array])) { throw "$Label must contain arrays of submodule pins." }
        foreach ($row in $rows) {
            Assert-JsonObject $row "$Label submodule pin"
            foreach ($name in @('path', 'registeredCommit', 'headCommit', 'worktreeStatus', 'sourceTreeSha256')) {
                if (-not ($row.$name -is [string])) { throw "$Label has a non-scalar $name field." }
            }
        }
    }
    $leftJson = ConvertTo-Json -InputObject (Get-NormalizedSubmodulePins $Left) -Depth 10 -Compress
    $rightJson = ConvertTo-Json -InputObject (Get-NormalizedSubmodulePins $Right) -Depth 10 -Compress
    if ($leftJson -cne $rightJson) { throw "$Label differs across the v43 candidate, freeze, and OFF build." }
}
function Invoke-InventoryReview([string]$PythonPath, [string]$AdapterPath, [string]$InstallRoot, [string]$ReportPath, [string]$ReportSha) {
    $arguments = @('-B', $AdapterPath, '--install-root', $InstallRoot, '--inventory-report', $ReportPath, '--inventory-sha256', $ReportSha, '--variant', 'OFF')
    $quoted = New-Object 'System.Collections.Generic.List[string]'
    foreach ($argument in $arguments) { [void]$quoted.Add((Quote-WindowsArgument ([string]$argument))) }
    $processInfo = New-Object Diagnostics.ProcessStartInfo
    $processInfo.FileName = $PythonPath
    $processInfo.Arguments = [string]::Join(' ', $quoted.ToArray())
    $processInfo.WorkingDirectory = [IO.Path]::GetDirectoryName($AdapterPath)
    $processInfo.UseShellExecute = $false
    $processInfo.CreateNoWindow = $true
    $processInfo.RedirectStandardOutput = $true
    $processInfo.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $processInfo
    try {
        if (-not $process.Start()) { throw 'Could not start the private OFF-install Python inventory verifier.' }
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $stdout = $process.StandardOutput.ReadToEnd()
        $process.WaitForExit()
        $stderr = $stderrTask.Result
        if ($process.ExitCode -ne 0) { throw ("Inventory adapter exited {0}: {1} {2}" -f $process.ExitCode, $stdout.Trim(), $stderr.Trim()) }
        return ($stdout | ConvertFrom-Json -ErrorAction Stop)
    }
    finally { $process.Dispose() }
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
function Get-PackageTreeDigest($Files, [string[]]$Directories) {
    $fileParts = New-Object 'System.Collections.Generic.List[string]'
    foreach ($row in $Files) {
        [void]$fileParts.Add('{"path":' + (ConvertTo-JsonString ([string]$row.path)) +
            ',"sha256":' + (ConvertTo-JsonString ([string]$row.sha256).ToLowerInvariant()) +
            ',"sizeBytes":' + ([string]([long]$row.sizeBytes)) + '}')
    }
    $directoryParts = New-Object 'System.Collections.Generic.List[string]'
    foreach ($directory in $Directories) { [void]$directoryParts.Add((ConvertTo-JsonString ([string]$directory))) }
    $json = '{"directories":[' + [string]::Join(',', $directoryParts.ToArray()) + '],"files":[' + [string]::Join(',', $fileParts.ToArray()) + ']}'
    return Get-Sha256Bytes ((New-Object Text.UTF8Encoding($false, $true)).GetBytes($json))
}

$outputRoot = [IO.Path]::GetFullPath((Join-Path $AuditRoot ''))
if (-not $OutputPath.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Package receipt output must remain under the external v43 audit root.' }
if ([IO.File]::Exists($OutputPath)) { throw "Refusing to overwrite existing evidence: $OutputPath" }
if ($RunId -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$') { throw 'RunId must contain only bounded ASCII letters, digits, dot, underscore, or hyphen.' }

$receipt = $null
$failure = $null
try {
    Assert-Hash (Get-Sha256File $CandidateManifestPath) $CandidateManifestSha256 'Candidate manifest'
    $manifest = Read-Json $CandidateManifestPath $CandidateManifestSha256
    Assert-JsonObject $manifest 'Candidate manifest'
    if (-not ($manifest.schema -is [string]) -or $manifest.schema -cne 'range.pc11-fresh-run-v43-admission-candidate/v1' -or
        -not ($manifest.candidateVersion -is [string]) -or $manifest.candidateVersion -cne 'v43') { throw 'Candidate manifest schema/version is invalid.' }
    Assert-StrictNoGo $manifest 'Candidate manifest' $false
    $binding = $manifest.installBinding
    $sourcePins = $manifest.sourceAndAuthorityPins
    Assert-JsonObject $binding 'Candidate manifest.installBinding'
    Assert-JsonObject $sourcePins 'Candidate manifest.sourceAndAuthorityPins'
    Assert-JsonString $binding 'requiredVariant' 'Candidate manifest.installBinding'
    Assert-JsonString $binding 'inventoryStatus' 'Candidate manifest.installBinding'
    Assert-JsonInteger $binding 'inventorySchemaVersion' 'Candidate manifest.installBinding' 2
    foreach ($field in @('installRoot', 'inventoryReportPath', 'inventoryReportSha256')) {
        Assert-JsonString $binding $field 'Candidate manifest.installBinding'
    }
    foreach ($field in @('sourceFreezeReceiptPath', 'sourceFreezeReceiptSha256', 'releaseBuildReceiptPath', 'releaseBuildReceiptSha256')) {
        Assert-JsonString $sourcePins $field 'Candidate manifest.sourceAndAuthorityPins'
    }
    if ($binding.requiredVariant -cne 'OFF' -or $binding.inventoryStatus -cne 'PASS') { throw 'Candidate manifest does not pin a completed OFF schema-v2 inventory.' }
    $inventoryPath = [IO.Path]::GetFullPath([string]$binding.inventoryReportPath)
    $inventorySha = ([string]$binding.inventoryReportSha256).ToLowerInvariant()
    Assert-Hash (Get-Sha256File $inventoryPath) $inventorySha 'Schema-v2 OFF inventory report'
    $inventory = Read-Json $inventoryPath $inventorySha
    Assert-JsonObject $inventory 'OFF inventory'
    Assert-JsonInteger $inventory 'schemaVersion' 'OFF inventory' 2
    foreach ($field in @('variant', 'inventoryStatus', 'installRoot', 'buildRoot')) { Assert-JsonString $inventory $field 'OFF inventory' }
    if (-not ($inventory.files -is [array]) -or $inventory.files.Count -eq 0 -or
        -not ($inventory.directories -is [array]) -or $inventory.directories.Count -eq 0 -or
        (-not ($inventory.fileCount -is [int]) -and -not ($inventory.fileCount -is [long])) -or
        $inventory.fileCount -ne $inventory.files.Count -or
        $inventory.variant -cne 'OFF' -or $inventory.inventoryStatus -cne 'PASS' -or
        $inventory.installRoot -cne $binding.installRoot) {
        throw 'OFF inventory schema/status/root/dynamic file count or directories are invalid.'
    }

    $freezePath = [IO.Path]::GetFullPath([string]$sourcePins.sourceFreezeReceiptPath)
    $freezeSha = ([string]$sourcePins.sourceFreezeReceiptSha256).ToLowerInvariant()
    Assert-Hash (Get-Sha256File $freezePath) $freezeSha 'Source freeze receipt'
    $freeze = Read-Json $freezePath $freezeSha
    if (-not ($freeze.schema -is [string]) -or $freeze.schema -cne 'range.pc11-v43-source-freeze/v1' -or
        -not ($freeze.status -is [string]) -or $freeze.status -cne 'PASS' -or
        -not ($freeze.candidateVersion -is [string]) -or $freeze.candidateVersion -cne 'v43' -or
        -not ($freeze.variant -is [string]) -or $freeze.variant -cne 'OFF') {
        throw 'Source freeze receipt schema/status/identity/no-GO boundary is invalid.'
    }
    Assert-StrictNoGo $freeze 'Source freeze receipt' $true
    $buildPath = [IO.Path]::GetFullPath([string]$sourcePins.releaseBuildReceiptPath)
    $buildSha = ([string]$sourcePins.releaseBuildReceiptSha256).ToLowerInvariant()
    Assert-Hash (Get-Sha256File $buildPath) $buildSha 'OFF Release build receipt'
    $build = Read-Json $buildPath $buildSha
    if (-not ($build.schema -is [string]) -or $build.schema -cne 'range.pc11-v43-release-build-receipt/v1' -or
        -not ($build.status -is [string]) -or $build.status -cne 'PASS' -or
        -not ($build.candidateVersion -is [string]) -or $build.candidateVersion -cne 'v43' -or
        -not ($build.variant -is [string]) -or $build.variant -cne 'OFF' -or
        -not ($build.configuration -is [string]) -or $build.configuration -cne 'Release') { throw 'OFF Release build receipt is not a PASS Release identity.' }
    Assert-StrictNoGo $build 'OFF Release build receipt' $true
    Assert-ScalarPinEqual $buildSha $freeze.releaseBuildReceiptSha256 'Frozen OFF Release build receipt SHA256'
    Assert-ScalarPinEqual $sourcePins.rootCommit $freeze.rootCommit 'Frozen root commit' '^(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})$'
    Assert-ScalarPinEqual $freeze.rootCommit $build.rootCommit 'OFF build root commit' '^(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})$'
    foreach ($field in @('sourceTreeSha256', 'requiredCTestManifestSha256', 'cmakeCacheSha256')) {
        Assert-ScalarPinEqual $sourcePins.$field $freeze.$field ("Frozen $field")
        Assert-ScalarPinEqual $freeze.$field $build.$field ("OFF build $field")
    }
    Assert-ScalarPinEqual $sourcePins.releaseBuildReceiptSha256 $freeze.releaseBuildReceiptSha256 'Candidate/freeze OFF build receipt SHA256'
    Assert-SubmodulePinsEqual $sourcePins.submoduleCommits $freeze.submoduleCommits 'Candidate/freeze submodule pins'
    Assert-SubmodulePinsEqual $freeze.submoduleCommits $build.submoduleCommits 'Freeze/build submodule pins'
    if (-not ($freeze.releaseBuildReceiptPath -is [string]) -or
        [IO.Path]::GetFullPath($freeze.releaseBuildReceiptPath) -cne $buildPath) { throw 'Frozen OFF build receipt path differs from the candidate build receipt path.' }
    Assert-JsonString $build 'buildDirectory' 'OFF Release build receipt'
    if (-not ($inventory.buildRoot -is [string]) -or -not ($build.buildDirectory -is [string]) -or
        [IO.Path]::GetFullPath($inventory.buildRoot) -cne [IO.Path]::GetFullPath($build.buildDirectory)) {
        throw 'OFF inventory and pinned Release receipt describe different build directories.'
    }
    if (-not ($inventory.executableProvenance -is [array])) { throw 'OFF inventory executableProvenance must be an array.' }
    $guiBuildRows = @($inventory.executableProvenance | Where-Object { $_.installedPath -is [string] -and $_.installedPath -ceq 'bin/fea.exe' })
    if ($guiBuildRows.Count -ne 1 -or -not ($build.executablePath -is [string]) -or
        [IO.Path]::GetFullPath($build.executablePath) -cne
            [IO.Path]::GetFullPath((Join-Path $build.buildDirectory 'Release\fea.exe'))) {
        throw 'OFF build receipt does not bind the GUI executable identified by inventory.'
    }
    Assert-ScalarPinEqual $build.executableSha256 $guiBuildRows[0].releaseBuildSha256 'OFF GUI executable build bytes'

    $installRoot = [IO.Path]::GetFullPath([string]$binding.installRoot)
    $pythonPath = [IO.Path]::GetFullPath((Join-Path $installRoot 'runtime\python\python.exe'))
    $layout = $manifest.installedPathLayout
    Assert-JsonObject $layout 'Candidate installedPathLayout'
    Assert-JsonString $layout 'privatePythonRelativePath' 'Candidate installedPathLayout'
    Assert-JsonString $layout 'privatePythonSha256' 'Candidate installedPathLayout'
    if ($layout.privatePythonRelativePath -cne 'runtime/python/python.exe') { throw 'Private Python path layout differs from the frozen candidate contract.' }
    Assert-Hash (Get-Sha256File $pythonPath) $layout.privatePythonSha256 'Installed private Python'
    if (-not [IO.File]::Exists($InventoryAdapterPath)) { throw "v43 inventory adapter is missing: $InventoryAdapterPath" }
    $adapterSha = Get-Sha256File $InventoryAdapterPath
    $treeReview = Invoke-InventoryReview $pythonPath $InventoryAdapterPath ([string]$binding.installRoot) $inventoryPath $inventorySha
    Assert-JsonObject $treeReview 'Private Python inventory adapter'
    Assert-JsonString $treeReview 'status' 'Private Python inventory adapter'
    Assert-JsonString $treeReview 'inventoryReportSha256' 'Private Python inventory adapter'
    Assert-JsonBoolean $treeReview 'goPermitted' 'Private Python inventory adapter' $false
    Assert-JsonBoolean $treeReview 'guiAdmissionEvaluated' 'Private Python inventory adapter' $false
    Assert-JsonInteger $treeReview 'fileCount' 'Private Python inventory adapter' $inventory.fileCount
    Assert-JsonInteger $treeReview 'directoryCount' 'Private Python inventory adapter' $inventory.directories.Count
    if ($treeReview.status -cne 'INVENTORY_TREE_MATCH' -or $treeReview.inventoryReportSha256 -cne $inventorySha) {
        throw 'Private Python inventory adapter did not confirm the complete OFF install tree.'
    }
    Assert-StrictNoGo $treeReview 'Private Python inventory adapter' $false
    foreach ($actionName in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        if ($treeReview.productActions.$actionName -ne $false) { throw "Inventory adapter permits product action $actionName." }
    }

    $fileList = New-Object 'System.Collections.Generic.List[object]'
    foreach ($row in $inventory.files) {
        [void]$fileList.Add([pscustomobject][ordered]@{ path = [string]$row.path; sizeBytes = [long]$row.sizeBytes; sha256 = ([string]$row.sha256).ToLowerInvariant() })
    }
    $fileCompare = [System.Comparison[object]]{ param($left, $right) Compare-Utf8Text ([string]$left.path) ([string]$right.path) }
    $fileList.Sort([System.Collections.Generic.Comparer[object]]::Create($fileCompare))
    $filesArray = $fileList.ToArray()
    $directories = [string[]]@($inventory.directories | ForEach-Object { [string]$_ })
    $identitySha = Get-ManifestIdentitySha256 $manifest
    $packageTreeSha = Get-PackageTreeDigest $filesArray $directories
    $totalBytes = [long]0
    foreach ($file in $filesArray) { $totalBytes += [long]$file.sizeBytes }
    $receipt = [ordered]@{
        schema = 'range.pc11-v43-package-tree-receipt/v1'
        status = 'PASS'
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        generatedAtUtc = [DateTime]::UtcNow.ToString('o')
        installRoot = [string]$binding.installRoot
        candidateManifestIdentitySha256 = $identitySha
        sourceFreezeReceiptSha256 = $freezeSha
        rootCommit = [string]$freeze.rootCommit
        submoduleCommits = (Get-NormalizedSubmodulePins $freeze.submoduleCommits)
        sourceTreeSha256 = [string]$freeze.sourceTreeSha256
        requiredCTestManifestSha256 = ([string]$freeze.requiredCTestManifestSha256).ToLowerInvariant()
        cmakeCacheSha256 = ([string]$freeze.cmakeCacheSha256).ToLowerInvariant()
        releaseBuildReceiptSha256 = $buildSha
        inventoryReportSha256 = $inventorySha
        inventoryStatus = 'PASS'
        inventoryAdapterSha256 = $adapterSha
        fileCount = $filesArray.Count
        directoryCount = $directories.Count
        totalBytes = $totalBytes
        files = $filesArray
        directories = $directories
        treeDigest = $packageTreeSha
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
}
catch { $failure = $_.Exception.Message }

if ($null -eq $receipt) {
    $receipt = [ordered]@{
        schema = 'range.pc11-v43-package-tree-receipt/v1'
        status = 'FAIL'
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        generatedAtUtc = [DateTime]::UtcNow.ToString('o')
        failure = $failure
        files = @()
        directories = @()
        fileCount = 0
        directoryCount = 0
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
}
$parent = [IO.Path]::GetDirectoryName($OutputPath)
if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
$json = $receipt | ConvertTo-Json -Depth 20
[IO.File]::WriteAllText($OutputPath, $json + "`n", (New-Object Text.UTF8Encoding($false)))
Write-Output ("{0} fileCount={1} directoryCount={2} treeDigest={3} output={4}" -f $receipt.status, $receipt.fileCount, $receipt.directoryCount, $receipt.treeDigest, $OutputPath)
if ($receipt.status -cne 'PASS') { exit 1 }
