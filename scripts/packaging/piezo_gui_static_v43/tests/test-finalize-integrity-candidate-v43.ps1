[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$CandidateManifestPath,
    [string]$CandidateManifestSha256,
    [string]$SourceFreezeReceiptPath,
    [string]$SourceFreezeReceiptSha256,
    [string]$FinalizerSha256,
    [string]$SourceTestSha256
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'The v43 candidate finalize-integrity gate requires Windows PowerShell 5.1.'
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if ([IO.File]::Exists($OutputPath)) { throw "Refusing to overwrite single-use finalize-integrity evidence: $OutputPath" }
$prepRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$finalizerPath = [IO.Path]::GetFullPath((Join-Path $prepRoot 'finalize-v43.ps1'))
$fixturePath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'test-finalize-integrity-candidate-fixture-v43.ps1'))
$templatePath = [IO.Path]::GetFullPath((Join-Path $prepRoot 'candidate-manifest-v43.admission-template.json'))
$repoRoot = [IO.Path]::GetFullPath((Join-Path $prepRoot '..\..\..'))
$requiredIds = @(
    'source_preflight', 'source_freeze', 'package_tree', 'module_boundary_build', 'module_boundary_source',
    'watchdog_preflight', 'save_predicate_preflight', 'path_shadow', 'root_seal', 'save_predicate',
    'watchdog_contract', 'sampler_arguments', 'supervisor', 'worker_handshake', 'native_launch_boundary',
    'parser', 'finalize_integrity', 'source_static_tests', 'root_bootstrap'
)

function Get-Sha256File([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { throw "Required pinned file is missing: $Path" }
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}
function Read-Json([string]$Path) {
    $text = (New-Object Text.UTF8Encoding($false, $true)).GetString([IO.File]::ReadAllBytes($Path))
    return ($text | ConvertFrom-Json -ErrorAction Stop)
}
function Assert-Sha([string]$Value, [string]$Name) {
    if ($Value -cnotmatch '^[0-9a-fA-F]{64}$') { throw "$Name must be an externally supplied 64-digit SHA256." }
}
function Assert-String($Object, [string]$Name, [string]$Label) {
    if ($null -eq $Object -or $Object -is [System.Array] -or $Object -isnot [PSCustomObject]) { throw "$Label must be a JSON object." }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -isnot [string] -or [string]::IsNullOrWhiteSpace([string]$property.Value)) {
        throw "$Label.$Name must be a non-empty JSON string."
    }
}
function Assert-Boolean($Object, [string]$Name, [string]$Label, [bool]$Expected) {
    if ($null -eq $Object -or $Object -is [System.Array] -or $Object -isnot [PSCustomObject]) { throw "$Label must be a JSON object." }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -isnot [bool] -or $property.Value -ne $Expected) {
        throw "$Label.$Name must be JSON boolean $Expected."
    }
}
function Assert-ProductActions($Document, [string]$Label) {
    if ($null -eq $Document.productActions -or $Document.productActions -is [System.Array] -or $Document.productActions -isnot [PSCustomObject]) {
        throw "$Label.productActions must be a JSON object."
    }
    foreach ($name in @('guiLaunched','savePerformed','runPerformed','confirmPerformed')) { Assert-Boolean $Document.productActions $name "$Label.productActions" $false }
}
function Assert-CompleteCandidatePair($Manifest, $Freeze, [string]$FreezePath, [string]$FreezeSha) {
    Assert-String $Manifest 'schema' 'Candidate manifest'
    Assert-String $Manifest 'candidateVersion' 'Candidate manifest'
    if ($Manifest.schema -cne 'range.pc11-fresh-run-v43-admission-candidate/v1' -or
        $Manifest.candidateVersion -cne 'v43') { throw 'Candidate manifest schema/version is not v43.' }
    Assert-Boolean $Manifest 'goPermitted' 'Candidate manifest' $false
    Assert-ProductActions $Manifest 'Candidate manifest'
    $source = $Manifest.sourceAndAuthorityPins
    if ($null -eq $source -or $source -is [System.Array] -or $source -isnot [PSCustomObject]) { throw 'Candidate sourceAndAuthorityPins must be a JSON object.' }
    foreach ($field in @('sourceRootPath','sourceFreezeReceiptPath','rootCommit','sourceTreeSha256','sourceFreezeReceiptSha256','releaseBuildReceiptSha256')) {
        Assert-String $source $field 'Candidate manifest.sourceAndAuthorityPins'
    }
    foreach ($field in @('sourceTreeSha256','sourceFreezeReceiptSha256','releaseBuildReceiptSha256')) { Assert-Sha ([string]$source.$field) "Candidate source pin $field" }
    if (-not [IO.Path]::GetFullPath([string]$source.sourceFreezeReceiptPath).Equals($FreezePath, [StringComparison]::OrdinalIgnoreCase) -or
        ([string]$source.sourceFreezeReceiptSha256).ToLowerInvariant() -cne $FreezeSha.ToLowerInvariant() -or
        $source.rootCommit -notmatch '^(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})$' -or
        -not [IO.Path]::GetFullPath([string]$source.sourceRootPath).Equals($repoRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Candidate manifest does not bind the externally pinned freeze path, bytes, and root commit.'
    }
    $evidence = $Manifest.candidateEvidencePins
    if ($null -eq $evidence -or $evidence -is [System.Array] -or $evidence -isnot [PSCustomObject] -or
        $evidence.requiredStaticTestIds -isnot [System.Array] -or
        (ConvertTo-Json -InputObject @($evidence.requiredStaticTestIds) -Compress) -cne (ConvertTo-Json -InputObject $script:requiredIds -Compress) -or
        $evidence.staticTestReceipts -isnot [System.Array] -or @($evidence.staticTestReceipts).Count -ne 1) {
        throw 'Candidate manifest is a template or lacks the exact 19-ID aggregate static receipt binding.'
    }
    $staticPin = @($evidence.staticTestReceipts)[0]
    Assert-String $staticPin 'name' 'Candidate aggregate static receipt pin'
    Assert-String $staticPin 'path' 'Candidate aggregate static receipt pin'
    Assert-String $staticPin 'sha256' 'Candidate aggregate static receipt pin'
    Assert-Sha ([string]$staticPin.sha256) 'Candidate aggregate static receipt pin'
    if ($staticPin.name -cne 'v43-static-suite') { throw 'Candidate must pin exactly the v43-static-suite aggregate.' }
    foreach ($field in @('schema','status','candidateVersion','variant','configuration')) { Assert-String $Freeze $field 'Source-freeze receipt' }
    if ($Freeze.schema -cne 'range.pc11-v43-source-freeze/v1' -or $Freeze.status -cne 'PASS' -or
        $Freeze.candidateVersion -cne 'v43' -or $Freeze.variant -cne 'OFF' -or $Freeze.configuration -cne 'Release') {
        throw 'Source-freeze receipt is not a v43 OFF Release PASS receipt.'
    }
    Assert-Boolean $Freeze 'staticOnly' 'Source-freeze receipt' $true
    Assert-Boolean $Freeze 'goPermitted' 'Source-freeze receipt' $false
    Assert-ProductActions $Freeze 'Source-freeze receipt'
    foreach ($field in @('sourceRootPath','rootCommit','sourceTreeSha256')) { Assert-String $Freeze $field 'Source-freeze receipt' }
    if (-not [IO.Path]::GetFullPath([string]$Freeze.sourceRootPath).Equals([IO.Path]::GetFullPath([string]$source.sourceRootPath), [StringComparison]::OrdinalIgnoreCase) -or
        -not [IO.Path]::GetFullPath([string]$Freeze.sourceRootPath).Equals($repoRoot, [StringComparison]::OrdinalIgnoreCase) -or
        [string]$Freeze.rootCommit -cne [string]$source.rootCommit -or
        ([string]$Freeze.sourceTreeSha256).ToLowerInvariant() -cne ([string]$source.sourceTreeSha256).ToLowerInvariant()) {
        throw 'Source-freeze root, commit, or source-tree identity differs from candidate pins.'
    }
    if ($Freeze.sourceFiles -isnot [System.Array] -or @($Freeze.sourceFiles).Count -eq 0) { throw 'Source-freeze sourceFiles must be a non-empty JSON array.' }
    $sourceRows = @{}
    foreach ($row in @($Freeze.sourceFiles)) {
        if ($null -eq $row -or $row -is [System.Array] -or $row -isnot [PSCustomObject]) { throw 'Source-freeze sourceFiles contains a malformed row.' }
        foreach ($field in @('path','sha256')) { Assert-String $row $field 'Source-freeze source row' }
        if ($row.path.StartsWith('/') -or $row.path.Contains('\') -or $row.path -match '(^|/)\.\.?(/|$)' -or $row.sha256 -cnotmatch '^[0-9a-fA-F]{64}$') {
            throw 'Source-freeze sourceFiles contains an unsafe path or malformed SHA256.'
        }
        if ($sourceRows.ContainsKey([string]$row.path)) { throw "Source-freeze sourceFiles duplicates path: $($row.path)" }
        $sourceRows[[string]$row.path] = ([string]$row.sha256).ToLowerInvariant()
    }
    $repoPrefix = $repoRoot.TrimEnd([char[]]@('\','/')) + [IO.Path]::DirectorySeparatorChar
    foreach ($path in @($finalizerPath, $PSCommandPath, $fixturePath, $templatePath)) {
        $fullPath = [IO.Path]::GetFullPath($path)
        if (-not $fullPath.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Required candidate-bound source escaped the repository root: $fullPath" }
        $relative = $fullPath.Substring($repoPrefix.Length).Replace('\','/')
        if (-not $sourceRows.ContainsKey($relative)) { throw "Source freeze does not pin the current finalizer-integrity input: $relative" }
        if ((Get-Sha256File $fullPath) -cne $sourceRows[$relative]) { throw "Source freeze bytes differ for finalizer-integrity input: $relative" }
    }
}

$provided = @($CandidateManifestPath, $CandidateManifestSha256, $SourceFreezeReceiptPath, $SourceFreezeReceiptSha256, $FinalizerSha256, $SourceTestSha256 |
    Where-Object { -not [string]::IsNullOrWhiteSpace($_) }).Count
if ($provided -ne 0 -and $provided -ne 6) { throw 'Candidate binding requires all six external candidate and source pins together.' }

$parent = [IO.Path]::GetDirectoryName($OutputPath)
if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
$fixtureRoot = Join-Path $parent ('finalize-integrity-fixture-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($fixtureRoot)
$fixtureReceipt = Join-Path $fixtureRoot 'fixture-receipt.json'
& $fixturePath -ReceiptPath $fixtureReceipt
$fixture = Read-Json $fixtureReceipt
if ($fixture.schema -cne 'range.pc11-v43-finalize-integrity-fixture/v1' -or $fixture.status -cne 'PASS' -or
    $fixture.fixtureOnly -ne $true -or $fixture.syntheticCompleteCandidate.status -cne 'PASS' -or
    $fixture.syntheticCompleteCandidate.staticReceiptStatus -cne 'STATIC_INCOMPLETE' -or
    $fixture.syntheticCompleteCandidate.allRequiredStaticTestsPass -ne $false -or
    $fixture.syntheticCompleteCandidate.goPermitted -ne $false -or
    @($fixture.rejectedCases).Count -lt 5) {
    throw 'Synthetic complete-candidate finalizer fixture did not produce its expected incomplete/no-GO runtime and tamper rejections.'
}
foreach ($name in @('guiLaunched','savePerformed','runPerformed','confirmPerformed')) {
    if ($fixture.syntheticCompleteCandidate.productActions.$name -ne $false -or $fixture.productActions.$name -ne $false) {
        throw "Synthetic finalize-integrity fixture permits product action $name."
    }
}

$candidateGateStatus = 'PENDING'
$candidateBinding = $null
$candidateManifestIdentitySha256 = $null
$sourceTreeSha256 = $null
if ($provided -eq 6) {
    Assert-Sha $CandidateManifestSha256 'Candidate manifest pin'
    Assert-Sha $SourceFreezeReceiptSha256 'Source-freeze receipt pin'
    Assert-Sha $FinalizerSha256 'Finalizer source pin'
    Assert-Sha $SourceTestSha256 'Source-test pin'
    $CandidateManifestPath = [IO.Path]::GetFullPath($CandidateManifestPath)
    $SourceFreezeReceiptPath = [IO.Path]::GetFullPath($SourceFreezeReceiptPath)
    if ((Get-Sha256File $CandidateManifestPath) -cne $CandidateManifestSha256.ToLowerInvariant()) { throw 'Candidate manifest bytes do not match the external SHA256 pin.' }
    if ((Get-Sha256File $SourceFreezeReceiptPath) -cne $SourceFreezeReceiptSha256.ToLowerInvariant()) { throw 'Source-freeze receipt bytes do not match the external SHA256 pin.' }
    if ((Get-Sha256File $finalizerPath) -cne $FinalizerSha256.ToLowerInvariant()) { throw 'Current finalize-v43.ps1 bytes differ from the external source pin.' }
    if ((Get-Sha256File $PSCommandPath) -cne $SourceTestSha256.ToLowerInvariant()) { throw 'Current candidate source-test bytes differ from the external source pin.' }
    $manifest = Read-Json $CandidateManifestPath
    $freeze = Read-Json $SourceFreezeReceiptPath
    Assert-CompleteCandidatePair $manifest $freeze $SourceFreezeReceiptPath $SourceFreezeReceiptSha256

    # Use the production finalizer's identity calculation so the receipt carries the same binding digest.
    $tokens = $null; $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($finalizerPath, [ref]$tokens, [ref]$parseErrors)
    if (@($parseErrors).Count -gt 0) { throw 'The production v43 finalizer does not parse.' }
    $functionText = New-Object 'System.Collections.Generic.List[string]'
    foreach ($name in @('Get-Sha256Bytes','ConvertTo-JsonString','Get-ManifestIdentitySha256','Assert-JsonObject','Assert-JsonString','Assert-Sha256Field')) {
        $node = @($ast.FindAll({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq $name }, $true)) | Select-Object -First 1
        if ($null -eq $node) { throw "Production finalizer identity helper missing: $name" }
        [void]$functionText.Add($node.Extent.Text)
    }
    Invoke-Expression ([string]::Join([Environment]::NewLine, $functionText.ToArray()))
    $candidateManifestIdentitySha256 = Get-ManifestIdentitySha256 $manifest
    $sourceTreeSha256 = ([string]$manifest.sourceAndAuthorityPins.sourceTreeSha256).ToLowerInvariant()
    $candidateGateStatus = 'PASS'
    $candidateBinding = [ordered]@{
        candidateManifestPath = $CandidateManifestPath
        candidateManifestSha256 = $CandidateManifestSha256.ToLowerInvariant()
        sourceFreezeReceiptPath = $SourceFreezeReceiptPath
        sourceFreezeReceiptSha256 = $SourceFreezeReceiptSha256.ToLowerInvariant()
        candidateManifestIdentitySha256 = $candidateManifestIdentitySha256
        sourceTreeSha256 = $sourceTreeSha256
        finalizerPath = $finalizerPath
        finalizerSha256 = $FinalizerSha256.ToLowerInvariant()
        sourceTestPath = [IO.Path]::GetFullPath($PSCommandPath)
        sourceTestSha256 = $SourceTestSha256.ToLowerInvariant()
    }
}

$receipt = [ordered]@{
    schema = 'range.pc11-v43-finalize-integrity-candidate-receipt/v1'
    status = 'PASS'
    candidateVersion = 'v43'
    variant = 'OFF'
    candidateGateStatus = $candidateGateStatus
    fixtureStatus = 'PASS'
    fixtureReceiptSchema = [string]$fixture.schema
    fixtureReceiptPath = [IO.Path]::GetFullPath($fixtureReceipt)
    fixtureReceiptSha256 = Get-Sha256File $fixtureReceipt
    syntheticCompleteCandidate = $fixture.syntheticCompleteCandidate
    rejectedCases = @($fixture.rejectedCases)
    finalizerNegativeCases = @($fixture.finalizerNegativeCases)
    candidateBinding = $candidateBinding
    candidateManifestSha256 = if ($null -ne $candidateBinding) { $candidateBinding.candidateManifestSha256 } else { $null }
    sourceFreezeReceiptSha256 = if ($null -ne $candidateBinding) { $candidateBinding.sourceFreezeReceiptSha256 } else { $null }
    candidateManifestIdentitySha256 = $candidateManifestIdentitySha256
    sourceTreeSha256 = $sourceTreeSha256
    staticOnly = $true
    candidatePromotionPermitted = $false
    productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
    goPermitted = $false
}
[IO.File]::WriteAllText($OutputPath, (($receipt | ConvertTo-Json -Depth 24) + "`n"), (New-Object Text.UTF8Encoding($false)))
Write-Output ("PASS finalize_integrity candidateGate={0} syntheticCases={1} runtimeStatus={2} goPermitted=false output={3}" -f
    $candidateGateStatus, @($fixture.rejectedCases).Count, $fixture.syntheticCompleteCandidate.staticReceiptStatus, $OutputPath)
