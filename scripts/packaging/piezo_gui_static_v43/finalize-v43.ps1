[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CandidateManifestPath,
    [Parameter(Mandatory = $true)][string]$CandidateManifestSha256,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$AuditRoot,
    [string]$RunId
)

$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'This v43 finalizer requires Windows PowerShell 5.1.'
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
if (-not $PSBoundParameters.ContainsKey('RunId')) { $RunId = 'finalize-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8) }
if (-not $AuditRoot) { $AuditRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit')) }
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)
$CandidateManifestPath = [IO.Path]::GetFullPath($CandidateManifestPath)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)

$script:RequiredTestIds = @(
    'source_preflight', 'source_freeze', 'package_tree', 'module_boundary_build', 'module_boundary_source',
    'watchdog_preflight', 'save_predicate_preflight', 'path_shadow', 'root_seal', 'save_predicate',
    'watchdog_contract', 'sampler_arguments', 'supervisor', 'worker_handshake', 'native_launch_boundary',
    'parser', 'finalize_integrity', 'source_static_tests', 'root_bootstrap'
)
function Get-Sha256Bytes([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Get-Sha256File([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}
function Read-Json([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { throw "Required evidence file is missing: $Path" }
    $bytes = [IO.File]::ReadAllBytes($Path)
    $text = (New-Object Text.UTF8Encoding($false, $true)).GetString($bytes)
    return ($text | ConvertFrom-Json -ErrorAction Stop)
}
function Assert-JsonObject($Value, [string]$Label) {
    if ($null -eq $Value -or $Value -is [System.Array] -or $Value -isnot [System.Management.Automation.PSCustomObject]) {
        throw "$Label must be a JSON object."
    }
}
function Assert-JsonString($Object, [string]$Name, [string]$Label) {
    Assert-JsonObject $Object $Label
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -isnot [string]) { throw "$Label.$Name must be a JSON string scalar." }
}
function Assert-JsonBoolean($Object, [string]$Name, [string]$Label) {
    Assert-JsonObject $Object $Label
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -isnot [bool]) { throw "$Label.$Name must be a JSON boolean scalar." }
}
function Assert-JsonStringArray($Object, [string]$Name, [string]$Label) {
    Assert-JsonObject $Object $Label
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -isnot [System.Array]) { throw "$Label.$Name must be a JSON array." }
    foreach ($item in $property.Value) { if ($item -isnot [string]) { throw "$Label.$Name entries must be JSON string scalars." } }
}
function Assert-JsonObjectArray($Object, [string]$Name, [string]$Label) {
    Assert-JsonObject $Object $Label
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -isnot [System.Array]) { throw "$Label.$Name must be a JSON array." }
    foreach ($item in $property.Value) { Assert-JsonObject $item "$Label.$Name entry" }
}
function Assert-Sha256Field($Object, [string]$Name, [string]$Label) {
    Assert-JsonString $Object $Name $Label
    if ($Object.$Name -cnotmatch '^[0-9a-fA-F]{64}$') { throw "$Label.$Name must be a 64-digit SHA256 string." }
}
function Assert-Hash([string]$Path, [string]$Expected, [string]$Label) {
    if (-not [IO.File]::Exists($Path) -or $Expected -notmatch '^[0-9a-fA-F]{64}$' -or (Get-Sha256File $Path) -cne $Expected.ToLowerInvariant()) {
        throw "$Label bytes do not match the pinned SHA256."
    }
}
function ConvertTo-JsonString([string]$Value) { return [string]($Value | ConvertTo-Json -Compress -ErrorAction Stop) }
function Get-ManifestIdentitySha256($Manifest) {
    Assert-JsonObject $Manifest 'Candidate manifest'
    $binding = $Manifest.installBinding
    $source = $Manifest.sourceAndAuthorityPins
    Assert-JsonObject $binding 'Candidate manifest.installBinding'
    Assert-JsonObject $source 'Candidate manifest.sourceAndAuthorityPins'
    Assert-JsonString $binding 'installRoot' 'Candidate manifest.installBinding'
    Assert-Sha256Field $binding 'inventoryReportSha256' 'Candidate manifest.installBinding'
    Assert-JsonString $source 'rootCommit' 'Candidate manifest.sourceAndAuthorityPins'
    Assert-Sha256Field $source 'releaseBuildReceiptSha256' 'Candidate manifest.sourceAndAuthorityPins'
    Assert-Sha256Field $source 'sourceFreezeReceiptSha256' 'Candidate manifest.sourceAndAuthorityPins'
    Assert-Sha256Field $source 'sourceTreeSha256' 'Candidate manifest.sourceAndAuthorityPins'
    $rootCommit = $source.rootCommit.ToLowerInvariant()
    if ($rootCommit -notmatch '^(?:[0-9a-f]{40}|[0-9a-f]{64})$') { throw 'Candidate manifest rootCommit is malformed.' }
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
function Assert-NoGoBoundary($Receipt, [string]$Label) {
    Assert-JsonString $Receipt 'candidateVersion' $Label
    Assert-JsonString $Receipt 'variant' $Label
    Assert-JsonBoolean $Receipt 'staticOnly' $Label
    Assert-JsonBoolean $Receipt 'goPermitted' $Label
    Assert-JsonObject $Receipt.productActions "$Label.productActions"
    foreach ($actionName in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        Assert-JsonBoolean $Receipt.productActions $actionName "$Label.productActions"
    }
    if ($Receipt.candidateVersion -cne 'v43' -or $Receipt.variant -cne 'OFF' -or $Receipt.staticOnly -ne $true -or $Receipt.goPermitted -ne $false) {
        throw "$Label does not preserve v43 OFF static-only no-GO identity."
    }
    foreach ($actionName in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        if ($Receipt.productActions.$actionName -ne $false) { throw "$Label permits product action $actionName." }
    }
}

$outputRoot = [IO.Path]::GetFullPath((Join-Path $AuditRoot ''))
if (-not $OutputPath.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Final receipt output must remain under the external v43 audit root.' }
if ([IO.File]::Exists($OutputPath)) { throw "Refusing to overwrite existing evidence: $OutputPath" }
if ($RunId -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$') { throw 'RunId must contain only bounded ASCII letters, digits, dot, underscore, or hyphen.' }

$receipt = $null
$failure = $null
try {
    Assert-Hash $CandidateManifestPath $CandidateManifestSha256 'Candidate manifest'
    $manifest = Read-Json $CandidateManifestPath
    Assert-JsonString $manifest 'schema' 'Candidate manifest'
    Assert-JsonString $manifest 'candidateVersion' 'Candidate manifest'
    Assert-JsonBoolean $manifest 'goPermitted' 'Candidate manifest'
    Assert-JsonObject $manifest.productActions 'Candidate manifest.productActions'
    foreach ($actionName in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        Assert-JsonBoolean $manifest.productActions $actionName 'Candidate manifest.productActions'
    }
    Assert-JsonObject $manifest.candidateEvidencePins 'Candidate manifest.candidateEvidencePins'
    Assert-JsonStringArray $manifest.candidateEvidencePins 'requiredStaticTestIds' 'Candidate manifest.candidateEvidencePins'
    Assert-JsonObjectArray $manifest.candidateEvidencePins 'staticTestReceipts' 'Candidate manifest.candidateEvidencePins'
    Assert-JsonObject $manifest.sourceAndAuthorityPins 'Candidate manifest.sourceAndAuthorityPins'
    foreach ($field in @('sourceFreezeReceiptPath')) { Assert-JsonString $manifest.sourceAndAuthorityPins $field 'Candidate manifest.sourceAndAuthorityPins' }
    Assert-JsonObject $manifest.installBinding 'Candidate manifest.installBinding'
    if ($manifest.schema -cne 'range.pc11-fresh-run-v43-admission-candidate/v1' -or $manifest.candidateVersion -cne 'v43' -or $manifest.goPermitted -ne $false) {
        throw 'Candidate manifest schema/version or no-GO boundary is invalid.'
    }
    foreach ($actionName in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        if ($manifest.productActions.$actionName -ne $false) { throw "Candidate manifest permits product action $actionName." }
    }
    $evidence = $manifest.candidateEvidencePins
    $source = $manifest.sourceAndAuthorityPins
    Assert-JsonString $evidence 'packageTreeReceiptPath' 'Candidate manifest.candidateEvidencePins'
    Assert-Sha256Field $evidence 'packageTreeReceiptSha256' 'Candidate manifest.candidateEvidencePins'
    if ((ConvertTo-Json -InputObject @($evidence.requiredStaticTestIds) -Compress) -cne
        (ConvertTo-Json -InputObject $script:RequiredTestIds -Compress)) { throw 'Manifest static test ID contract differs from this finalizer.' }
    $identitySha = Get-ManifestIdentitySha256 $manifest

    $freezePath = [IO.Path]::GetFullPath([string]$source.sourceFreezeReceiptPath)
    Assert-Hash $freezePath ([string]$source.sourceFreezeReceiptSha256) 'Source freeze receipt'
    $freeze = Read-Json $freezePath
    Assert-JsonString $freeze 'schema' 'Source freeze receipt'
    Assert-JsonString $freeze 'status' 'Source freeze receipt'
    Assert-Sha256Field $freeze 'sourceTreeSha256' 'Source freeze receipt'
    if ($freeze.schema -cne 'range.pc11-v43-source-freeze/v1' -or $freeze.status -cne 'PASS' -or
        $freeze.sourceTreeSha256 -cne ([string]$source.sourceTreeSha256).ToLowerInvariant()) { throw 'Source freeze identity differs from candidate pins.' }
    Assert-NoGoBoundary $freeze 'Source freeze receipt'

    $packagePath = [IO.Path]::GetFullPath([string]$evidence.packageTreeReceiptPath)
    Assert-Hash $packagePath ([string]$evidence.packageTreeReceiptSha256) 'Package-tree receipt'
    $package = Read-Json $packagePath
    Assert-JsonString $package 'schema' 'Package-tree receipt'
    Assert-JsonString $package 'status' 'Package-tree receipt'
    foreach ($field in @('candidateManifestIdentitySha256', 'sourceFreezeReceiptSha256', 'sourceTreeSha256', 'inventoryReportSha256')) {
        Assert-Sha256Field $package $field 'Package-tree receipt'
    }
    if ($package.schema -cne 'range.pc11-v43-package-tree-receipt/v1' -or $package.status -cne 'PASS' -or
        $package.candidateManifestIdentitySha256 -cne $identitySha -or
        $package.sourceFreezeReceiptSha256 -cne ([string]$source.sourceFreezeReceiptSha256).ToLowerInvariant() -or
        $package.sourceTreeSha256 -cne ([string]$source.sourceTreeSha256).ToLowerInvariant() -or
        $package.inventoryReportSha256 -cne ([string]$manifest.installBinding.inventoryReportSha256).ToLowerInvariant()) {
        throw 'Package-tree receipt identity differs from candidate pins.'
    }
    Assert-NoGoBoundary $package 'Package-tree receipt'

    $entries = @($evidence.staticTestReceipts)
    if ($entries.Count -gt 0) { Assert-JsonObject $entries[0] 'Candidate manifest.candidateEvidencePins.staticTestReceipts[0]' }
    if ($entries.Count -eq 1) {
        Assert-JsonString $entries[0] 'name' 'Candidate static-test receipt pin'
        Assert-JsonString $entries[0] 'path' 'Candidate static-test receipt pin'
        Assert-Sha256Field $entries[0] 'sha256' 'Candidate static-test receipt pin'
    }
    if ($entries.Count -ne 1 -or $entries[0].name -cne 'v43-static-suite') { throw 'Manifest must pin exactly one aggregate v43-static-suite receipt.' }
    $staticPath = [IO.Path]::GetFullPath([string]$entries[0].path)
    Assert-Hash $staticPath ([string]$entries[0].sha256) 'Aggregate static-test receipt'
    $static = Read-Json $staticPath
    Assert-JsonString $static 'schema' 'Aggregate static-test receipt'
    Assert-JsonString $static 'status' 'Aggregate static-test receipt'
    foreach ($field in @('candidateManifestIdentitySha256', 'sourceTreeSha256', 'inventoryReportSha256', 'packageTreeReceiptSha256')) {
        Assert-Sha256Field $static $field 'Aggregate static-test receipt'
    }
    Assert-JsonStringArray $static 'requiredTestIds' 'Aggregate static-test receipt'
    if ($static.PSObject.Properties['results'] -eq $null -or $static.results -isnot [System.Array]) { throw 'Aggregate static-test receipt.results must be a JSON array.' }
    if ($static.schema -cne 'range.pc11-v43-static-test-receipt/v1' -or
        $static.candidateManifestIdentitySha256 -cne $identitySha -or
        $static.sourceTreeSha256 -cne ([string]$source.sourceTreeSha256).ToLowerInvariant() -or
        $static.inventoryReportSha256 -cne ([string]$manifest.installBinding.inventoryReportSha256).ToLowerInvariant() -or
        $static.packageTreeReceiptSha256 -cne ([string]$evidence.packageTreeReceiptSha256).ToLowerInvariant() -or
        (ConvertTo-Json -InputObject @($static.requiredTestIds) -Compress) -cne (ConvertTo-Json -InputObject $script:RequiredTestIds -Compress)) {
        throw 'Aggregate static-test receipt schema, identity, or required-test contract differs from candidate pins.'
    }
    Assert-NoGoBoundary $static 'Aggregate static-test receipt'
    if (@($static.results).Count -ne $script:RequiredTestIds.Count) { throw 'Aggregate static-test receipt result count differs from the full static contract.' }
    $observed = @{}
    foreach ($row in $static.results) {
        Assert-JsonString $row 'testId' 'Aggregate static-test receipt result'
        Assert-JsonString $row 'status' 'Aggregate static-test receipt result'
        if ($row.testId -notin $script:RequiredTestIds -or $row.status -notin @('PASS', 'PENDING', 'FAIL') -or $observed.ContainsKey([string]$row.testId)) {
            throw 'Aggregate static-test receipt contains an unknown, malformed, or duplicate result.'
        }
        $observed[[string]$row.testId] = [string]$row.status
    }
    if ($observed.Count -ne $script:RequiredTestIds.Count) { throw 'Aggregate static-test receipt omits required results.' }
    $pending = @($script:RequiredTestIds | Where-Object { $observed[$_] -cne 'PASS' })
    $status = if ($pending.Count -gt 0 -or $static.status -cne 'PASS') { 'STATIC_INCOMPLETE' } else { 'STATIC_COMPLETE_NO_GO' }
    $receipt = [ordered]@{
        schema = 'range.pc11-v43-final-static-receipt/v1'
        status = $status
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        generatedAtUtc = [DateTime]::UtcNow.ToString('o')
        candidateManifestSha256 = ([string]$CandidateManifestSha256).ToLowerInvariant()
        candidateManifestIdentitySha256 = $identitySha
        sourceFreezeReceiptPath = $freezePath
        sourceFreezeReceiptSha256 = ([string]$source.sourceFreezeReceiptSha256).ToLowerInvariant()
        sourceTreeSha256 = ([string]$source.sourceTreeSha256).ToLowerInvariant()
        inventoryReportSha256 = ([string]$manifest.installBinding.inventoryReportSha256).ToLowerInvariant()
        packageTreeReceiptPath = $packagePath
        packageTreeReceiptSha256 = ([string]$evidence.packageTreeReceiptSha256).ToLowerInvariant()
        staticTestReceiptPath = $staticPath
        staticTestReceiptSha256 = ([string]$entries[0].sha256).ToLowerInvariant()
        requiredTestIds = $script:RequiredTestIds
        results = @($static.results)
        allRequiredStaticTestsPass = ($pending.Count -eq 0 -and $static.status -ceq 'PASS')
        pendingTestIds = $pending
        missingV39EquivalentGates = $pending
        externalProductGoRequirements = @('independent static review', 'separate root technical decision', 'root GO seal')
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
}
catch { $failure = $_.Exception.Message }

if ($null -eq $receipt) {
    $receipt = [ordered]@{
        schema = 'range.pc11-v43-final-static-receipt/v1'
        status = 'FINALIZE_FAIL'
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        generatedAtUtc = [DateTime]::UtcNow.ToString('o')
        failure = $failure
        allRequiredStaticTestsPass = $false
        requiredTestIds = $script:RequiredTestIds
        pendingTestIds = $script:RequiredTestIds
        missingV39EquivalentGates = $script:RequiredTestIds
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
}
$parent = [IO.Path]::GetDirectoryName($OutputPath)
if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
$json = $receipt | ConvertTo-Json -Depth 20
[IO.File]::WriteAllText($OutputPath, $json + "`n", (New-Object Text.UTF8Encoding($false)))
Write-Output ("{0} allRequiredStaticTestsPass={1} missing={2} goPermitted=false output={3}" -f $receipt.status, $receipt.allRequiredStaticTestsPass, @($receipt.pendingTestIds).Count, $OutputPath)
if ($receipt.status -ceq 'FINALIZE_FAIL') { exit 1 }
