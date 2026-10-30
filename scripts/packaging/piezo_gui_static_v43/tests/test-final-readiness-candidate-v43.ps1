[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$CandidateManifestPath,
    [string]$CandidateManifestSha256,
    [string]$SourceFreezeReceiptPath,
    [string]$SourceFreezeReceiptSha256,
    [string]$CandidateControlReceiptPath,
    [string]$CandidateControlReceiptSha256,
    [string]$WatchdogExecutionEnvelopePath,
    [string]$WatchdogExecutionEnvelopeSha256
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'The v43 final-readiness gate requires Windows PowerShell 5.1.'
}
$script:PrepRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$script:RepoRoot = [IO.Path]::GetFullPath((Join-Path $script:PrepRoot '..\..\..'))
$script:RequiredIds = @(
    'source_preflight', 'source_freeze', 'package_tree', 'module_boundary_build', 'module_boundary_source',
    'watchdog_preflight', 'save_predicate_preflight', 'path_shadow', 'root_seal', 'save_predicate',
    'watchdog_contract', 'sampler_arguments', 'supervisor', 'worker_handshake', 'native_launch_boundary',
    'parser', 'finalize_integrity', 'source_static_tests', 'root_bootstrap'
)
$script:Actions = @('guiLaunched','savePerformed','runPerformed','confirmPerformed')
$script:OpenStreams = New-Object 'System.Collections.Generic.List[System.IO.FileStream]'
$script:LockedSources = New-Object 'System.Collections.Generic.List[object]'
$script:LockedExternal = New-Object 'System.Collections.Generic.List[object]'
$script:LockedFullPaths = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$script:FreezeRows = @()
$script:Failure = $null
$script:PendingReason = $null
$script:ActiveTestId = $null
$script:TestEvidence = [ordered]@{}
$script:TestResults = [ordered]@{
    watchdog_preflight = [ordered]@{ testId='watchdog_preflight'; status='PENDING'; reason=$null }
    path_shadow = [ordered]@{ testId='path_shadow'; status='PENDING'; reason=$null }
    root_bootstrap = [ordered]@{ testId='root_bootstrap'; status='PENDING'; reason=$null }
}
$script:OutputPath = [IO.Path]::GetFullPath($OutputPath)
$script:OutputParent = [IO.Path]::GetDirectoryName($script:OutputPath)
$script:AuditBase = [IO.Path]::GetFullPath((Join-Path $script:RepoRoot 'build\audit-20261014\piezo-delivery'))
$script:AuditPrefix = $script:AuditBase.TrimEnd('\') + '\'
if (-not $script:OutputPath.StartsWith($script:AuditPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Final-readiness receipt must remain below build/audit-20261014/piezo-delivery.'
}
if ([IO.File]::Exists($script:OutputPath)) { throw "Refusing to overwrite single-use final-readiness evidence: $script:OutputPath" }
if (-not [IO.Directory]::Exists($script:OutputParent)) { [void][IO.Directory]::CreateDirectory($script:OutputParent) }

function Assert-Sha([string]$Value, [string]$Label) {
    if ($Value -cnotmatch '^[0-9a-fA-F]{64}$') { throw "$Label must be a 64-digit SHA256." }
}
function Assert-JsonObject($Value, [string]$Label) {
    if ($null -eq $Value -or $Value -is [Array] -or
        ($Value -isnot [PSCustomObject] -and $Value -isnot [System.Collections.IDictionary])) {
        throw "$Label must be a JSON object."
    }
}
function Assert-String($Object, [string]$Name, [string]$Label) {
    Assert-JsonObject $Object $Label
    if ($Object -is [System.Collections.IDictionary]) {
        if (-not $Object.Contains($Name) -or $Object[$Name] -isnot [string] -or [string]::IsNullOrWhiteSpace([string]$Object[$Name])) {
            throw "$Label.$Name must be a non-empty JSON string."
        }
    } else {
        $property = $Object.PSObject.Properties[$Name]
        if ($null -eq $property -or $property.Value -isnot [string] -or [string]::IsNullOrWhiteSpace([string]$property.Value)) {
            throw "$Label.$Name must be a non-empty JSON string."
        }
    }
}
function Assert-Boolean($Object, [string]$Name, [string]$Label, [bool]$Expected) {
    Assert-JsonObject $Object $Label
    if ($Object -is [System.Collections.IDictionary]) {
        if (-not $Object.Contains($Name) -or $Object[$Name] -isnot [bool] -or $Object[$Name] -ne $Expected) {
            throw "$Label.$Name must be JSON boolean $Expected."
        }
    } else {
        $property = $Object.PSObject.Properties[$Name]
        if ($null -eq $property -or $property.Value -isnot [bool] -or $property.Value -ne $Expected) {
            throw "$Label.$Name must be JSON boolean $Expected."
        }
    }
}
function Assert-ObjectArray($Object, [string]$Name, [string]$Label) {
    Assert-JsonObject $Object $Label
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -isnot [Array]) { throw "$Label.$Name must be a JSON array." }
    foreach ($item in $property.Value) { Assert-JsonObject $item "$Label.$Name entry" }
}
function Assert-StringArray($Object, [string]$Name, [string]$Label) {
    Assert-JsonObject $Object $Label
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -isnot [Array]) { throw "$Label.$Name must be a JSON array." }
    foreach ($item in $property.Value) {
        if ($item -isnot [string]) { throw "$Label.$Name entries must be JSON strings." }
    }
}
function Assert-ProductActions($Object, [string]$Label) {
    Assert-JsonObject $Object.productActions "$Label.productActions"
    foreach ($action in $script:Actions) { Assert-Boolean $Object.productActions $action "$Label.productActions" $false }
}
function Assert-NoGo($Object, [string]$Label) {
    Assert-String $Object 'candidateVersion' $Label
    Assert-String $Object 'variant' $Label
    Assert-Boolean $Object 'staticOnly' $Label $true
    Assert-Boolean $Object 'goPermitted' $Label $false
    Assert-ProductActions $Object $Label
    if ($Object.candidateVersion -cne 'v43' -or $Object.variant -cne 'OFF') {
        throw "$Label does not carry the exact v43/OFF no-GO identity."
    }
}
function Get-Sha256Bytes([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Get-StreamSha256([IO.Stream]$Stream) {
    $Stream.Position = 0
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $Stream.Position = 0 }
}
function Get-BytesFromStream([IO.Stream]$Stream) {
    $Stream.Position = 0
    $memory = New-Object IO.MemoryStream
    try { $Stream.CopyTo($memory); return $memory.ToArray() }
    finally { $memory.Dispose(); $Stream.Position = 0 }
}
function Open-LockedInput([string]$Path, [string]$ExpectedSha, [string]$Label) {
    Assert-Sha $ExpectedSha "$Label external pin"
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not [IO.File]::Exists($fullPath)) { throw "$Label is missing: $fullPath" }
    if (-not $script:LockedFullPaths.Add($fullPath)) { throw "Duplicate locked input path: $fullPath" }
    $stream = [IO.File]::Open($fullPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    [void]$script:OpenStreams.Add($stream)
    $actualSha = Get-StreamSha256 $stream
    if ($actualSha -cne $ExpectedSha.ToLowerInvariant()) { throw "$Label bytes differ from the external SHA256 pin." }
    $bytes = Get-BytesFromStream $stream
    $text = (New-Object Text.UTF8Encoding($false, $true)).GetString($bytes)
    $value = $text | ConvertFrom-Json -ErrorAction Stop
    Assert-JsonObject $value $Label
    $pin = [pscustomobject]@{ path=$fullPath; sha256=$actualSha; value=$value; bytes=$bytes }
    [void]$script:LockedExternal.Add($pin)
    return $pin
}
function Get-SourceRelativePath([string]$FullPath) {
    $full = [IO.Path]::GetFullPath($FullPath)
    $prefix = $script:RepoRoot.TrimEnd('\') + '\'
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Source escaped the frozen repository root: $full" }
    return $full.Substring($prefix.Length).Replace('\','/')
}
function Lock-FrozenSource([string]$Path, [string]$Label) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not [IO.File]::Exists($fullPath)) { throw "$Label source is missing: $fullPath" }
    if (-not $script:LockedFullPaths.Add($fullPath)) { return }
    $relative = Get-SourceRelativePath $fullPath
    $matches = @($script:FreezeRows | Where-Object { [string]$_.path -ceq $relative })
    if ($matches.Count -ne 1) { throw "$Label source must have exactly one current freeze row: $relative" }
    $stream = [IO.File]::Open($fullPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    [void]$script:OpenStreams.Add($stream)
    $actualSha = Get-StreamSha256 $stream
    if ([string]$matches[0].sha256 -cne $actualSha) { throw "$Label source differs from its current freeze row: $relative" }
    [void]$script:LockedSources.Add([pscustomobject]@{ path=$fullPath; relativePath=$relative; sha256=$actualSha; stream=$stream })
}
function ConvertTo-JsonString([string]$Value) { return [string]($Value | ConvertTo-Json -Compress -ErrorAction Stop) }
function Get-ManifestIdentity($Manifest) {
    Assert-JsonObject $Manifest 'Candidate manifest'
    Assert-JsonObject $Manifest.installBinding 'Candidate manifest.installBinding'
    Assert-JsonObject $Manifest.sourceAndAuthorityPins 'Candidate manifest.sourceAndAuthorityPins'
    $binding = $Manifest.installBinding
    $source = $Manifest.sourceAndAuthorityPins
    $rootCommit = ([string]$source.rootCommit).ToLowerInvariant()
    if ($rootCommit -notmatch '^(?:[0-9a-f]{40}|[0-9a-f]{64})$') { throw 'Candidate rootCommit is malformed.' }
    foreach ($pair in @(
        [pscustomobject]@{ value=[string]$binding.inventoryReportSha256; name='inventoryReportSha256' },
        [pscustomobject]@{ value=[string]$source.releaseBuildReceiptSha256; name='releaseBuildReceiptSha256' },
        [pscustomobject]@{ value=[string]$source.sourceFreezeReceiptSha256; name='sourceFreezeReceiptSha256' },
        [pscustomobject]@{ value=[string]$source.sourceTreeSha256; name='sourceTreeSha256' }
    )) { Assert-Sha $pair.value "Candidate $($pair.name)" }
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
function Test-SamePath([string]$Left, [string]$Right) {
    return [StringComparer]::OrdinalIgnoreCase.Equals([IO.Path]::GetFullPath($Left).TrimEnd('\'), [IO.Path]::GetFullPath($Right).TrimEnd('\'))
}
function Assert-SourceFreeze($Manifest, $Freeze, [string]$FreezePath, [string]$FreezeSha) {
    Assert-JsonObject $Manifest 'Candidate manifest'
    Assert-String $Manifest 'schema' 'Candidate manifest'
    Assert-String $Manifest 'candidateVersion' 'Candidate manifest'
    Assert-Boolean $Manifest 'goPermitted' 'Candidate manifest' $false
    Assert-ProductActions $Manifest 'Candidate manifest'
    if ($Manifest.schema -cne 'range.pc11-fresh-run-v43-admission-candidate/v1' -or $Manifest.candidateVersion -cne 'v43') {
        throw 'Candidate manifest schema/version is not v43.'
    }
    Assert-JsonObject $Manifest.sourceAndAuthorityPins 'Candidate source pins'
    Assert-JsonObject $Manifest.installBinding 'Candidate install binding'
    Assert-String $Manifest.installBinding 'requiredVariant' 'Candidate install binding'
    if ($Manifest.installBinding.requiredVariant -cne 'OFF') { throw 'Candidate install variant is not OFF.' }
    $source = $Manifest.sourceAndAuthorityPins
    foreach ($field in @('sourceRootPath','sourceFreezeReceiptPath','sourceFreezeReceiptSha256','sourceTreeSha256','rootCommit')) {
        Assert-String $source $field 'Candidate source pins'
    }
    if (-not (Test-SamePath ([string]$source.sourceRootPath) $script:RepoRoot) -or
        -not (Test-SamePath ([string]$Freeze.sourceRootPath) $script:RepoRoot) -or
        -not (Test-SamePath ([string]$source.sourceFreezeReceiptPath) $FreezePath) -or
        ([string]$source.sourceFreezeReceiptSha256).ToLowerInvariant() -cne $FreezeSha.ToLowerInvariant() -or
        ([string]$source.sourceTreeSha256).ToLowerInvariant() -cne ([string]$Freeze.sourceTreeSha256).ToLowerInvariant() -or
        [string]$source.rootCommit -cne [string]$Freeze.rootCommit) {
        throw 'Candidate manifest and source-freeze root/path/hash/tree/commit bindings differ.'
    }
    if ($Freeze.schema -cne 'range.pc11-v43-source-freeze/v1' -or $Freeze.status -cne 'PASS' -or
        $Freeze.candidateVersion -cne 'v43' -or $Freeze.variant -cne 'OFF' -or $Freeze.configuration -cne 'Release' -or
        $Freeze.staticOnly -ne $true -or $Freeze.goPermitted -ne $false) {
        throw 'Source freeze must be a v43/OFF/Release PASS, static-only no-GO receipt.'
    }
    Assert-NoGo $Freeze 'Source freeze'
    if ($Freeze.sourceFiles -isnot [Array] -or $Freeze.sourceFiles.Count -eq 0) { throw 'Source-freeze sourceFiles must be a non-empty JSON array.' }
    $paths = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($row in $Freeze.sourceFiles) {
        Assert-String $row 'path' 'Source-freeze row'
        Assert-String $row 'sha256' 'Source-freeze row'
        Assert-Sha ([string]$row.sha256) "Source-freeze row $($row.path)"
        if ($row.path.StartsWith('/') -or $row.path.Contains('\') -or $row.path -match '(^|/)\.\.?(/|$)' -or
            -not $paths.Add([string]$row.path)) { throw "Source-freeze row path is unsafe or duplicated: $($row.path)" }
    }
    if ($Manifest.candidateEvidencePins -isnot [PSCustomObject] -or $Manifest.candidateEvidencePins.requiredStaticTestIds -isnot [Array] -or
        ($Manifest.candidateEvidencePins.requiredStaticTestIds -join [Environment]::NewLine) -cne ($script:RequiredIds -join [Environment]::NewLine)) {
        throw 'Candidate manifest static-test ID contract differs from the frozen ordered 19-ID list.'
    }
}
function Resolve-ProducerSource([string]$ProducerPath) {
    if ([string]::IsNullOrWhiteSpace($ProducerPath) -or $ProducerPath.Contains('\') -or $ProducerPath -match '(^|/)\.\.?(/|$)') {
        throw "Unsafe producer source path: $ProducerPath"
    }
    if ($ProducerPath.StartsWith('../../../scripts/', [StringComparison]::Ordinal)) {
        return [IO.Path]::GetFullPath((Join-Path $script:RepoRoot ($ProducerPath.Substring(9).Replace('/','\'))))
    }
    return [IO.Path]::GetFullPath((Join-Path $script:PrepRoot ($ProducerPath.Replace('/','\'))))
}
function Test-ValidatorNegativeCases {
    $cases = New-Object 'System.Collections.Generic.List[object]'
    $malformedBoolean = '{"value":"true"}' | ConvertFrom-Json -ErrorAction Stop
    $oneElementStatusArray = '{"status":["PASS"]}' | ConvertFrom-Json -ErrorAction Stop
    $objectInsteadOfArray = '{"results":{"testId":"watchdog_contract"}}' | ConvertFrom-Json -ErrorAction Stop
    foreach ($caseName in @('string-boolean','array-status','object-instead-of-results-array')) {
        $rejected = $false
        try {
            switch ($caseName) {
                'string-boolean' { Assert-Boolean $malformedBoolean 'value' 'negative string boolean' $true }
                'array-status' { Assert-String $oneElementStatusArray 'status' 'negative array status' }
                'object-instead-of-results-array' { Assert-ObjectArray $objectInsteadOfArray 'results' 'negative results array' }
            }
        } catch { $rejected = $true }
        if (-not $rejected) { throw "Strict receipt validator accepted malformed $caseName." }
        [void]$cases.Add([pscustomobject][ordered]@{ case=$caseName; status='PASS_REJECTED' })
    }
    return $cases.ToArray()
}
function Assert-WatchdogProof($Control, $Envelope, [string]$EnvelopePath, [string]$EnvelopeSha, [string]$ManifestSha, [string]$FreezeSha, [string]$Identity, [string]$Tree) {
    Assert-NoGo $Control 'Candidate-control receipt'
    Assert-String $Control 'schema' 'Candidate-control receipt'
    Assert-String $Control 'candidateGateStatus' 'Candidate-control receipt'
    Assert-String $Control 'candidateManifestIdentitySha256' 'Candidate-control receipt'
    Assert-String $Control 'sourceTreeSha256' 'Candidate-control receipt'
    Assert-Boolean $Control 'fixtureOnly' 'Candidate-control receipt' $true
    Assert-ObjectArray $Control 'results' 'Candidate-control receipt'
    if ($Control.schema -cne 'range.pc11-v43-candidate-control-contract-receipt/v1' -or
        $Control.candidateGateStatus -cne 'PASS' -or $Control.fixtureOnly -ne $true -or
        [string]$Control.candidateManifestIdentitySha256 -cne $Identity -or
        ([string]$Control.sourceTreeSha256).ToLowerInvariant() -cne $Tree.ToLowerInvariant()) {
        throw 'Candidate-control receipt is not PASS and bound to the pinned candidate identity/tree.'
    }
    Assert-JsonObject $Control.gateProducerSource 'Candidate-control evaluator source binding'
    Assert-String $Control.gateProducerSource 'path' 'Candidate-control evaluator source binding'
    Assert-String $Control.gateProducerSource 'sourceFreezeBinding' 'Candidate-control evaluator source binding'
    if ($Control.gateProducerSource.path -cne 'scripts/packaging/piezo_gui_static_v43/tests/test-candidate-control-contract-v43.ps1' -or
        $Control.gateProducerSource.sourceFreezeBinding -cne 'MATCH') {
        throw 'Candidate-control evaluator is not source-freeze bound.'
    }
    Assert-JsonObject $Control.externalCandidateManifest 'Candidate-control external manifest pin'
    Assert-String $Control.externalCandidateManifest 'path' 'Candidate-control external manifest pin'
    Assert-String $Control.externalCandidateManifest 'sha256' 'Candidate-control external manifest pin'
    if (-not (Test-SamePath ([string]$Control.externalCandidateManifest.path) $CandidateManifestPath) -or
        ([string]$Control.externalCandidateManifest.sha256).ToLowerInvariant() -cne $ManifestSha.ToLowerInvariant()) {
        throw 'Candidate-control receipt does not pin the supplied manifest path and bytes.'
    }
    Assert-JsonObject $Control.sourceFreezeReceipt 'Candidate-control source-freeze pin'
    Assert-String $Control.sourceFreezeReceipt 'path' 'Candidate-control source-freeze pin'
    Assert-String $Control.sourceFreezeReceipt 'sha256' 'Candidate-control source-freeze pin'
    if (-not (Test-SamePath ([string]$Control.sourceFreezeReceipt.path) $SourceFreezeReceiptPath) -or
        ([string]$Control.sourceFreezeReceipt.sha256).ToLowerInvariant() -cne $FreezeSha.ToLowerInvariant()) {
        throw 'Candidate-control receipt does not pin the supplied source-freeze bytes.'
    }
    $controlRows = @($Control.results | Where-Object { [string]$_.testId -ceq 'watchdog_contract' })
    if ($controlRows.Count -ne 1) { throw 'Candidate-control receipt must contain exactly one watchdog_contract result.' }
    $controlRow = $controlRows[0]
    Assert-String $controlRow 'testId' 'Candidate-control watchdog row'
    foreach ($field in @('status','sourceBindingMethod','sourcePathBinding','semanticCoverage','candidateIdentityBinding','fixtureReceiptPath','fixtureReceiptSha256','executionEnvelopePath','executionEnvelopeSha256')) {
        Assert-String $controlRow $field 'Candidate-control watchdog row'
    }
    if ($controlRow.status -cne 'PASS' -or $controlRow.sourceBindingMethod -cne 'FROZEN_SOURCE_EXECUTION' -or
        $controlRow.sourcePathBinding -cne 'RECEIPT_AND_FROZEN_SOURCE_HASHES_MATCH' -or
        $controlRow.semanticCoverage -cne 'FIXTURE_CASES_VALIDATED' -or
        $controlRow.candidateIdentityBinding -cne 'EXTERNAL_MANIFEST_IDENTITY_AND_SOURCE_TREE_MATCH' -or
        -not (Test-SamePath ([string]$controlRow.executionEnvelopePath) $EnvelopePath) -or
        ([string]$controlRow.executionEnvelopeSha256).ToLowerInvariant() -cne $EnvelopeSha.ToLowerInvariant()) {
        throw 'Candidate-control watchdog row lacks the required external envelope and semantic/source PASS bindings.'
    }
    $expectedSources = @(
        'runtime/contract-common-v43.ps1','runtime/supervisor-core-v43.ps1','runtime/worker-handshake-v43.ps1',
        'runtime/watchdog-contract-v43.ps1','runtime/native-launch-boundary-v43.ps1',
        'tests/fixtures/worker-fixture-v43.ps1','tests/test-supervisor-worker-contract-v43.ps1','finalize-v43.ps1'
    )
    Assert-StringArray $controlRow 'producerSourcePaths' 'Candidate-control watchdog row'
    if (
        ($controlRow.producerSourcePaths -join [Environment]::NewLine) -cne ($expectedSources -join [Environment]::NewLine)) {
        throw 'Candidate-control watchdog producer source path set or order differs.'
    }
    foreach ($field in @('schema','componentId','candidateVersion','variant','sourceBindingMethod','candidateManifestIdentitySha256','sourceTreeSha256')) {
        Assert-String $Envelope $field 'Watchdog execution envelope'
    }
    Assert-Boolean $Envelope 'fixtureOnly' 'Watchdog execution envelope' $true
    Assert-Boolean $Envelope 'staticOnly' 'Watchdog execution envelope' $true
    Assert-Boolean $Envelope 'goPermitted' 'Watchdog execution envelope' $false
    if ($Envelope.schema -cne 'range.pc11-v43-frozen-fixture-execution/v1' -or
        $Envelope.componentId -cne 'watchdog_contract' -or $Envelope.candidateVersion -cne 'v43' -or
        $Envelope.variant -cne 'OFF' -or $Envelope.sourceBindingMethod -cne 'FROZEN_SOURCE_EXECUTION' -or
        $Envelope.fixtureOnly -ne $true -or $Envelope.staticOnly -ne $true -or $Envelope.goPermitted -ne $false -or
        [string]$Envelope.candidateManifestIdentitySha256 -cne $Identity -or
        ([string]$Envelope.sourceTreeSha256).ToLowerInvariant() -cne $Tree.ToLowerInvariant()) {
        throw 'Watchdog execution envelope is not a frozen-source, fixture-only no-GO binding for the supplied candidate.'
    }
    Assert-NoGo $Envelope 'Watchdog execution envelope'
    Assert-JsonObject $Envelope.originalReceipt 'Watchdog original receipt pin'
    Assert-String $Envelope.originalReceipt 'path' 'Watchdog original receipt pin'
    Assert-String $Envelope.originalReceipt 'sha256' 'Watchdog original receipt pin'
    if (-not (Test-SamePath ([string]$Envelope.originalReceipt.path) ([string]$controlRow.fixtureReceiptPath)) -or
        ([string]$Envelope.originalReceipt.sha256).ToLowerInvariant() -cne ([string]$controlRow.fixtureReceiptSha256).ToLowerInvariant()) {
        throw 'Watchdog envelope original receipt does not match candidate-control receipt row.'
    }
    Assert-ObjectArray $Envelope 'sources' 'Watchdog execution envelope'
    if ($Envelope.sources.Count -ne $expectedSources.Count) { throw 'Watchdog envelope source rows are incomplete.' }
    for ($i=0; $i -lt $expectedSources.Count; $i++) {
        $sourceRow = $Envelope.sources[$i]
        Assert-String $sourceRow 'path' 'Watchdog envelope source row'
        Assert-String $sourceRow 'sha256' 'Watchdog envelope source row'
        if ($sourceRow.path -cne $expectedSources[$i]) { throw 'Watchdog envelope source row path/order differs.' }
        Assert-Sha ([string]$sourceRow.sha256) "Watchdog envelope source $($sourceRow.path)"
    }
    $receiptPin = Open-LockedInput ([string]$Envelope.originalReceipt.path) ([string]$Envelope.originalReceipt.sha256) 'Original watchdog fixture receipt'
    $original = $receiptPin.value
    Assert-NoGo $original 'Original watchdog fixture receipt'
    foreach ($field in @('schema','status','staticContractSuiteStatus','fixtureContractStatus','candidateGateStatus')) {
        Assert-String $original $field 'Original watchdog fixture receipt'
    }
    Assert-String $original 'fixtureCandidateManifestIdentitySha256' 'Original watchdog fixture receipt'
    Assert-Boolean $original 'fixtureOnly' 'Original watchdog fixture receipt' $true
    Assert-ObjectArray $original 'results' 'Original watchdog fixture receipt'
    if ($original.schema -cne 'range.pc11-v43-supervisor-worker-contract-static-receipt/v1' -or
        $original.fixtureOnly -ne $true -or $original.status -cne 'STATIC_INCOMPLETE' -or
        $original.staticContractSuiteStatus -cne 'PASS' -or $original.fixtureContractStatus -cne 'PASS' -or
        $original.candidateGateStatus -cne 'PENDING') {
        throw 'Original watchdog receipt is not the expected successful fixture-only static-incomplete receipt.'
    }
    $watchdogRows = @($original.results | Where-Object { [string]$_.testId -ceq 'watchdog_contract' })
    if ($watchdogRows.Count -eq 1) {
        foreach ($field in @('testId','status','fixtureStatus','candidateGateStatus','fixtureCandidateManifestIdentitySha256')) {
            Assert-String $watchdogRows[0] $field 'Original watchdog result row'
        }
        Assert-Boolean $watchdogRows[0] 'boundSupervisorSuccessAccepted' 'Original watchdog result row' $true
        Assert-Boolean $watchdogRows[0] 'supervisorFailureRejected' 'Original watchdog result row' $true
    }
    if ($watchdogRows.Count -ne 1 -or $watchdogRows[0].status -cne 'PASS' -or
        $watchdogRows[0].boundSupervisorSuccessAccepted -ne $true -or $watchdogRows[0].supervisorFailureRejected -ne $true -or
        $watchdogRows[0].fixtureStatus -cne 'PASS' -or $watchdogRows[0].candidateGateStatus -cne 'PENDING' -or
        [string]$watchdogRows[0].fixtureCandidateManifestIdentitySha256 -cne [string]$original.fixtureCandidateManifestIdentitySha256) {
        throw 'Original watchdog success/rejection fixture result is incomplete or does not remain pending product qualification.'
    }
    Assert-JsonObject $original.sourceSha256 'Original watchdog source hashes'
    foreach ($path in $expectedSources) {
        $sourceProperty = $original.sourceSha256.PSObject.Properties[$path]
        if ($null -eq $sourceProperty -or $sourceProperty.Value -isnot [string]) { throw "Original watchdog receipt has no source pin for $path" }
        $envelopeRow = @($Envelope.sources | Where-Object { [string]$_.path -ceq $path })
        if ($envelopeRow.Count -ne 1 -or [string]$sourceProperty.Value -cne [string]$envelopeRow[0].sha256) {
            throw "Original watchdog receipt and envelope source hashes differ for $path."
        }
        $relativeSource = Get-SourceRelativePath (Resolve-ProducerSource $path)
        $freezeRow = @($script:FreezeRows | Where-Object { [string]$_.path -ceq $relativeSource })
        $lockedSource = @($script:LockedSources | Where-Object { [string]$_.relativePath -ceq $relativeSource })
        if ($freezeRow.Count -ne 1 -or $lockedSource.Count -ne 1 -or
            ([string]$envelopeRow[0].sha256).ToLowerInvariant() -cne ([string]$freezeRow[0].sha256).ToLowerInvariant() -or
            ([string]$envelopeRow[0].sha256).ToLowerInvariant() -cne ([string]$lockedSource[0].sha256).ToLowerInvariant()) {
            throw "Watchdog receipt/envelope source is not bound to the current frozen implementation: $path"
        }
    }
    return [pscustomobject]@{
        envelopePath=$EnvelopePath; envelopeSha256=$EnvelopeSha
        originalReceiptPath=$receiptPin.path; originalReceiptSha256=$receiptPin.sha256
        successAccepted=$true; supervisorFailureRejected=$true
    }
}
function Quote-Argument([string]$Value) {
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
function Invoke-ChildPowerShell([string[]]$Arguments, [hashtable]$Environment = @{}, [string]$WorkingDirectory = $script:OutputParent) {
    $exe = [IO.Path]::GetFullPath((Join-Path $PSHOME 'powershell.exe'))
    $quoted = New-Object 'System.Collections.Generic.List[string]'
    foreach ($argument in (@('-NoProfile','-ExecutionPolicy','Bypass') + $Arguments)) { [void]$quoted.Add((Quote-Argument ([string]$argument))) }
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $exe
    $info.Arguments = [string]::Join(' ', $quoted.ToArray())
    $info.WorkingDirectory = $WorkingDirectory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($name in $Environment.Keys) { $info.EnvironmentVariables[[string]$name] = [string]$Environment[$name] }
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    try {
        if (-not $process.Start()) { throw 'Could not start isolated Windows PowerShell test process.' }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(600000)) {
            try { $process.Kill() } catch {}
            $process.WaitForExit()
            throw 'Isolated Windows PowerShell test exceeded the ten-minute bound.'
        }
        return [pscustomobject]@{ exitCode=[int]$process.ExitCode; stdout=[string]$stdoutTask.Result; stderr=[string]$stderrTask.Result; executable=$exe }
    }
    finally { $process.Dispose() }
}
function Write-EvidenceText([string]$Path, [string]$Text) {
    if ([IO.File]::Exists($Path)) { throw "Refusing to overwrite runtime output evidence: $Path" }
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false)))
}
function Assert-NoReparseAncestors([string]$Path) {
    $cursor = [IO.DirectoryInfo]([IO.Path]::GetFullPath($Path))
    while ($null -ne $cursor) {
        if ($cursor.Exists -and (($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) { throw "Refusing reparse-point audit ancestor: $($cursor.FullName)" }
        $cursor = $cursor.Parent
    }
}

$script:ValidatorNegativeCases = @(Test-ValidatorNegativeCases)

$pinValues = @($CandidateManifestPath,$CandidateManifestSha256,$SourceFreezeReceiptPath,$SourceFreezeReceiptSha256,
    $CandidateControlReceiptPath,$CandidateControlReceiptSha256,$WatchdogExecutionEnvelopePath,$WatchdogExecutionEnvelopeSha256)
$providedPinCount = @($pinValues | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) }).Count
$manifestPin = $null
$freezePin = $null
$controlPin = $null
$envelopePin = $null
$identitySha = $null
$manifestSha = $null
$freezeSha = $null
$sourceTreeSha = $null
$watchdogProof = $null
$candidateGateStatus = 'PENDING'
$script:EvidenceRoot = $null

try {
    Assert-NoReparseAncestors $script:OutputParent
    if ($providedPinCount -ne 8) {
        $script:PendingReason = if ($providedPinCount -eq 0) {
            'External candidate, freeze, candidate-control, and watchdog-envelope pins were not supplied.'
        } else {
            'Final-readiness requires all eight candidate/freeze/control/envelope path and SHA pins together.'
        }
    } else {
        Assert-Sha $CandidateManifestSha256 'Candidate manifest SHA256'
        Assert-Sha $SourceFreezeReceiptSha256 'Source freeze SHA256'
        Assert-Sha $CandidateControlReceiptSha256 'Candidate-control receipt SHA256'
        Assert-Sha $WatchdogExecutionEnvelopeSha256 'Watchdog execution envelope SHA256'
        $manifestPin = Open-LockedInput $CandidateManifestPath $CandidateManifestSha256 'Candidate manifest'
        $freezePin = Open-LockedInput $SourceFreezeReceiptPath $SourceFreezeReceiptSha256 'Source freeze receipt'
        $controlPin = Open-LockedInput $CandidateControlReceiptPath $CandidateControlReceiptSha256 'Candidate-control receipt'
        $envelopePin = Open-LockedInput $WatchdogExecutionEnvelopePath $WatchdogExecutionEnvelopeSha256 'Watchdog execution envelope'
        $manifestSha = $manifestPin.sha256
        $freezeSha = $freezePin.sha256
        $identitySha = Get-ManifestIdentity $manifestPin.value
        $sourceTreeSha = ([string]$manifestPin.value.sourceAndAuthorityPins.sourceTreeSha256).ToLowerInvariant()
        Assert-SourceFreeze $manifestPin.value $freezePin.value $freezePin.path $freezeSha
        if (-not (Test-SamePath $manifestPin.path $CandidateManifestPath) -or -not (Test-SamePath $freezePin.path $SourceFreezeReceiptPath)) {
            throw 'Pinned source paths changed during normalization.'
        }
        $script:FreezeRows = @($freezePin.value.sourceFiles)
        $sourcePaths = @(
            $PSCommandPath,
            (Join-Path $script:PrepRoot 'tests\test-preflight-gates-v43.ps1'),
            (Join-Path $script:PrepRoot 'admission_v43.py'),
            (Join-Path $PSScriptRoot 'test-static-chain-v43.ps1'),
            (Join-Path $script:PrepRoot 'package-tree-v43.ps1'),
            (Join-Path $PSScriptRoot 'test-path-shadow-v43.ps1'),
            (Join-Path $script:PrepRoot 'tools\system-module-boundary-v43.ps1'),
            (Join-Path $script:PrepRoot 'runtime\contract-common-v43.ps1'),
            (Join-Path $script:PrepRoot 'runtime\supervisor-core-v43.ps1'),
            (Join-Path $script:PrepRoot 'runtime\native-launch-boundary-v43.ps1'),
            (Join-Path $script:PrepRoot 'runtime\worker-handshake-v43.ps1'),
            (Join-Path $script:PrepRoot 'tests\fixtures\worker-fixture-v43.ps1'),
            (Join-Path $PSScriptRoot 'test-root-bootstrap-v43.ps1'),
            (Join-Path $script:PrepRoot 'runtime\root-bootstrap-v43.ps1'),
            (Join-Path $PSScriptRoot 'test-root-bootstrap-relative-create-v43.ps1'),
            (Join-Path $PSScriptRoot 'test-candidate-control-contract-v43.ps1'),
            (Join-Path $script:PrepRoot 'runtime\watchdog-contract-v43.ps1'),
            (Join-Path $PSScriptRoot 'test-supervisor-worker-contract-v43.ps1'),
            (Join-Path $script:PrepRoot 'finalize-v43.ps1'),
            (Join-Path $script:PrepRoot 'candidate-manifest-v43.admission-template.json')
        )
        foreach ($sourcePath in $sourcePaths) { Lock-FrozenSource $sourcePath 'Final-readiness execution' }
        foreach ($sourceRow in $envelopePin.value.sources) {
            Assert-String $sourceRow 'path' 'Watchdog envelope source row'
            Lock-FrozenSource (Resolve-ProducerSource ([string]$sourceRow.path)) 'Watchdog envelope'
        }
        $watchdogProof = Assert-WatchdogProof $controlPin.value $envelopePin.value $envelopePin.path $envelopePin.sha256 $manifestPin.sha256 $freezePin.sha256 $identitySha $sourceTreeSha
        $script:PendingReason = $null
        $candidateGateStatus = 'PASS'
    }
}
catch {
    $script:PendingReason = $_.Exception.Message
    $candidateGateStatus = 'PENDING'
}

if ($null -eq $script:PendingReason) {
    $script:EvidenceRoot = Join-Path $script:OutputParent ('final-readiness-evidence-' + [Guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($script:EvidenceRoot)
    Assert-NoReparseAncestors $script:EvidenceRoot

    $script:ActiveTestId = 'watchdog_preflight'
    try {
        $preflightReceiptPath = Join-Path $script:EvidenceRoot 'preflight-assessment.json'
        $preflightStdoutPath = Join-Path $script:EvidenceRoot 'preflight.stdout.txt'
        $preflightStderrPath = Join-Path $script:EvidenceRoot 'preflight.stderr.txt'
        $preflightPath = Join-Path $script:PrepRoot 'tests\test-preflight-gates-v43.ps1'
        $preflightRun = Invoke-ChildPowerShell @('-File',$preflightPath,'-OutputPath',$preflightReceiptPath,'-AuditRoot',$script:OutputParent)
        Write-EvidenceText $preflightStdoutPath $preflightRun.stdout
        Write-EvidenceText $preflightStderrPath $preflightRun.stderr
        if ($preflightRun.exitCode -ne 0 -or -not [IO.File]::Exists($preflightReceiptPath)) { throw "Current no-product preflight suite failed: $($preflightRun.stderr) $($preflightRun.stdout)" }
        $preflight = (New-Object Text.UTF8Encoding($false, $true)).GetString([IO.File]::ReadAllBytes($preflightReceiptPath)) | ConvertFrom-Json -ErrorAction Stop
        Assert-NoGo $preflight 'Static preflight receipt'
        $preflightIds = @('watchdog_preflight','save_predicate_preflight','path_shadow','sampler_arguments')
        if ($preflight.schema -cne 'range.pc11-v43-static-preflight-assessment/v1' -or
            $preflight.status -cne 'STATIC_INCOMPLETE' -or
            ($preflight.requiredGateIds -join [Environment]::NewLine) -cne ($preflightIds -join [Environment]::NewLine) -or
            @($preflight.results).Count -ne 4 -or [int]$preflight.passedGateCount -ne 0 -or
            [int]$preflight.failedGateCount -ne 0 -or
            ($preflight.pendingGateIds -join [Environment]::NewLine) -cne ($preflightIds -join [Environment]::NewLine) -or
            $null -ne $preflight.candidateManifestIdentitySha256 -or $null -ne $preflight.sourceTreeSha256) {
            throw 'Static preflight receipt did not retain the expected four pending no-product rows.'
        }
        foreach ($row in $preflight.results) { if ($row.status -cne 'PENDING') { throw 'Static preflight promoted a runtime gate without candidate-bound execution.' } }
        if (@($preflight.componentCases | Where-Object { $_.status -cne 'PASS' }).Count -ne 0 -or
            [int]$preflight.componentCaseCount -ne @($preflight.componentCases).Count) { throw 'Static preflight component fixtures did not all pass.' }
        $script:TestResults.watchdog_preflight.status = 'PASS'
        $script:TestResults.watchdog_preflight.reason = 'Current no-product preflight fixtures passed; fresh externally pinned watchdog success and supervisor-failure rejection are bound to this frozen source and candidate identity. Scope is frozen source implementation, not installed GUI preflight.'
        $script:TestEvidence.watchdog_preflight = [ordered]@{
            preflightReceiptPath=$preflightReceiptPath
            preflightReceiptSha256=(Get-Sha256Bytes ([IO.File]::ReadAllBytes($preflightReceiptPath)))
            stdoutPath=$preflightStdoutPath
            stderrPath=$preflightStderrPath
            componentCaseCount=[int]$preflight.componentCaseCount
            preflightGateRowsRemainPending=$true
            watchdogOriginalReceiptPath=$watchdogProof.originalReceiptPath
            watchdogOriginalReceiptSha256=$watchdogProof.originalReceiptSha256
            watchdogExecutionEnvelopePath=$watchdogProof.envelopePath
            watchdogExecutionEnvelopeSha256=$watchdogProof.envelopeSha256
            boundSupervisorSuccessAccepted=$watchdogProof.successAccepted
            supervisorFailureRejected=$watchdogProof.supervisorFailureRejected
            installedGuiPreflightPerformed=$false
        }
    }
    catch {
        $script:Failure = $_.Exception.Message
        $script:TestResults.watchdog_preflight.status = 'FAIL'
        $script:TestResults.watchdog_preflight.reason = $script:Failure
    }

    if ($null -eq $script:Failure) {
        $script:ActiveTestId = 'path_shadow'
        try {
            $pathShadowReceiptPath = Join-Path $script:EvidenceRoot 'path-shadow.json'
            $pathShadowStdoutPath = Join-Path $script:EvidenceRoot 'path-shadow.stdout.txt'
            $pathShadowStderrPath = Join-Path $script:EvidenceRoot 'path-shadow.stderr.txt'
            $pathShadowPath = Join-Path $script:PrepRoot 'tests\test-path-shadow-v43.ps1'
            $pathShadowRun = Invoke-ChildPowerShell @('-File',$pathShadowPath,'-OutputPath',$pathShadowReceiptPath,'-AuditRoot',$script:OutputParent)
            Write-EvidenceText $pathShadowStdoutPath $pathShadowRun.stdout
            Write-EvidenceText $pathShadowStderrPath $pathShadowRun.stderr
            if ($pathShadowRun.exitCode -ne 0 -or -not [IO.File]::Exists($pathShadowReceiptPath)) { throw "Fake-first PATH route suite failed: $($pathShadowRun.stderr) $($pathShadowRun.stdout)" }
            $pathShadow = (New-Object Text.UTF8Encoding($false, $true)).GetString([IO.File]::ReadAllBytes($pathShadowReceiptPath)) | ConvertFrom-Json -ErrorAction Stop
            Assert-NoGo $pathShadow 'Path-shadow receipt'
            if ($pathShadow.schema -cne 'range.pc11-v43-path-shadow-receipt/v1' -or
                $pathShadow.status -cne 'STATIC_INCOMPLETE' -or @($pathShadow.testedRoutes).Count -ne 5 -or
                @($pathShadow.testedRoutes | Where-Object { $_.status -cne 'PASS' }).Count -ne 0 -or
                @($pathShadow.pendingRoutes).Count -ne 1 -or
                $pathShadow.pendingRoutes[0].route -cne 'root_bootstrap_external_entry' -or
                $pathShadow.pendingRoutes[0].status -cne 'PENDING') {
                throw 'Path-shadow suite did not pass five routes while retaining its explicit external-bootstrap pending row.'
            }
            $fakeDir = Join-Path $script:EvidenceRoot 'fake-first-path'
            [void][IO.Directory]::CreateDirectory($fakeDir)
            $fakeExe = Join-Path $fakeDir 'powershell.exe'
            $fakeMarker = Join-Path $script:EvidenceRoot 'fake-powershell-ran.marker'
            $shimType = 'V43FinalReadinessPowerShellShim' + [Guid]::NewGuid().ToString('N')
            $shimCode = @"
using System;
using System.IO;
public static class $shimType {
    public static int Main(string[] args) {
        string marker = Environment.GetEnvironmentVariable("V43_FINAL_READINESS_FAKE_MARKER");
        if (String.IsNullOrEmpty(marker)) return 81;
        File.WriteAllText(marker, "fake powershell.exe executed");
        Console.Error.WriteLine("fake powershell.exe executed");
        return 91;
    }
}
"@
            Microsoft.PowerShell.Utility\Add-Type -TypeDefinition $shimCode -Language CSharp -OutputAssembly $fakeExe -OutputType ConsoleApplication -ErrorAction Stop
            $oldPath = $env:PATH
            $oldMarker = $env:V43_FINAL_READINESS_FAKE_MARKER
            try {
                $env:PATH = $fakeDir + ';' + $oldPath
                $env:V43_FINAL_READINESS_FAKE_MARKER = $fakeMarker
                $resolved = [string](Get-Command powershell.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
                if (-not (Test-SamePath $resolved $fakeExe)) { throw "Fake PowerShell did not resolve first on PATH: $resolved" }
                $bootstrapStdoutPath = Join-Path $script:EvidenceRoot 'root-bootstrap.stdout.txt'
                $bootstrapStderrPath = Join-Path $script:EvidenceRoot 'root-bootstrap.stderr.txt'
                $bootstrapPath = Join-Path $script:PrepRoot 'tests\test-root-bootstrap-v43.ps1'
                $bootstrapRun = Invoke-ChildPowerShell @('-File',$bootstrapPath) @{ PATH=$env:PATH; V43_FINAL_READINESS_FAKE_MARKER=$fakeMarker }
                Write-EvidenceText $bootstrapStdoutPath $bootstrapRun.stdout
                Write-EvidenceText $bootstrapStderrPath $bootstrapRun.stderr
                if ($bootstrapRun.exitCode -ne 0 -or [IO.File]::Exists($fakeMarker)) { throw "VerifyOnly suite failed or fake executable ran: $($bootstrapRun.stderr) $($bootstrapRun.stdout)" }
                $summary = [regex]::Match($bootstrapRun.stdout, '(?m)^PASS_ROOT_BOOTSTRAP_V43_SYNTHETIC_FIXTURES scenarioCount=31 positive=6 rejected=24 .* actualReviewOrDecisionPerformed=false\s*$')
                if (-not $summary.Success) { throw "VerifyOnly suite did not report all 31 expected synthetic cases: $($bootstrapRun.stdout)" }
            }
            finally {
                $env:PATH = $oldPath
                if ($null -eq $oldMarker) { Remove-Item Env:V43_FINAL_READINESS_FAKE_MARKER -ErrorAction SilentlyContinue }
                else { $env:V43_FINAL_READINESS_FAKE_MARKER = $oldMarker }
            }
            if ([IO.File]::Exists($fakeMarker)) { throw 'The fake powershell.exe marker appeared after VerifyOnly completed.' }
            $script:TestResults.path_shadow.status = 'PASS'
            $script:TestResults.path_shadow.reason = 'Five existing fake-first routes passed; production VerifyOnly ran under confirmed fake-first PATH using its pinned PSHOME PowerShell child, and the fake never executed.'
            $script:TestEvidence.path_shadow = [ordered]@{
                pathShadowReceiptPath=$pathShadowReceiptPath
                pathShadowReceiptSha256=(Get-Sha256Bytes ([IO.File]::ReadAllBytes($pathShadowReceiptPath)))
                stdoutPath=$pathShadowStdoutPath
                stderrPath=$pathShadowStderrPath
                testedRouteCount=5
                allTestedRoutesPassed=$true
                externalBootstrapPendingDischargedByExecution=$true
                rootBootstrapStdoutPath=$bootstrapStdoutPath
                rootBootstrapStderrPath=$bootstrapStderrPath
                fakeFirstExecutablePath=$fakeExe
                fakeFirstResolutionConfirmed=$true
                pinnedPowerShellChildUsed=$true
                fakeExecutableMarkerPath=$fakeMarker
                fakeExecutableNeverRan=$true
                verifyOnlyScenarioCount=31
                positiveScenarioCount=6
                rejectedScenarioCount=24
                actualReviewOrDecisionPerformed=$false
            }
        }
        catch {
            $script:Failure = $_.Exception.Message
            $script:TestResults.path_shadow.status = 'FAIL'
            $script:TestResults.path_shadow.reason = $script:Failure
        }
    }

    if ($null -eq $script:Failure) {
        $script:ActiveTestId = 'root_bootstrap'
        try {
            $relativeAuditRoot = Join-Path $script:EvidenceRoot 'relative-create-audit'
            [void][IO.Directory]::CreateDirectory($relativeAuditRoot)
            $relativeStdoutPath = Join-Path $script:EvidenceRoot 'relative-create.stdout.txt'
            $relativeStderrPath = Join-Path $script:EvidenceRoot 'relative-create.stderr.txt'
            $relativePath = Join-Path $script:PrepRoot 'tests\test-root-bootstrap-relative-create-v43.ps1'
            $relativeRun = Invoke-ChildPowerShell @('-File',$relativePath,'-AuditRoot',$relativeAuditRoot)
            Write-EvidenceText $relativeStdoutPath $relativeRun.stdout
            Write-EvidenceText $relativeStderrPath $relativeRun.stderr
            if ($relativeRun.exitCode -ne 0) { throw "Production relative-create fixture failed: $($relativeRun.stderr) $($relativeRun.stdout)" }
            $receiptMatch = [regex]::Match($relativeRun.stdout, '(?m)\breceipt=(?<path>.+?)\s+tempRetained=')
            if (-not $receiptMatch.Success) { throw 'Relative-create fixture did not report its durable receipt path.' }
            $relativeReceiptPath = [IO.Path]::GetFullPath($receiptMatch.Groups['path'].Value.Trim())
            $relativePrefix = [IO.Path]::GetFullPath($relativeAuditRoot).TrimEnd('\') + '\'
            if (-not $relativeReceiptPath.StartsWith($relativePrefix, [StringComparison]::OrdinalIgnoreCase) -or
                -not [IO.File]::Exists($relativeReceiptPath)) { throw 'Relative-create receipt is missing or escaped its durable audit directory.' }
            $relativeReceiptBytes = [IO.File]::ReadAllBytes($relativeReceiptPath)
            $relativeReceipt = (New-Object Text.UTF8Encoding($false, $true)).GetString($relativeReceiptBytes) | ConvertFrom-Json -ErrorAction Stop
            Assert-String $relativeReceipt 'schema' 'Relative-create receipt'
            Assert-String $relativeReceipt 'status' 'Relative-create receipt'
            Assert-String $relativeReceipt 'tempTreeCleanup' 'Relative-create receipt'
            foreach ($field in @('cleanPositiveExpectedPath','cleanPositiveActualPath',
                    'sourceSentinelSha256Before','sourceSentinelSha256After',
                    'targetSentinelSha256Before','targetSentinelSha256After')) {
                Assert-String $relativeReceipt $field 'Relative-create receipt'
            }
            $relativeExpectedBooleans = [ordered]@{
                writeHandleOpenSucceeded=$true
                fsctlSetAttempted=$true
                fsctlSetSucceeded=$true
                relativeCreateAttempted=$true
                relativeCreateHandleReturned=$false
                relativeCreateRefusedByWhitelistedReparseStatus=$true
                relativeCreateContainedIdentity=$false
                redirectedOutputAppearedInTarget=$false
                junctionCleanupAttempted=$true
                junctionCleanupSucceeded=$true
                cleanPositivePassed=$true
                cleanPositiveDeleteSucceeded=$true
                tempTreeRetained=$false
                goPermitted=$false
            }
            foreach ($field in $relativeExpectedBooleans.Keys) {
                Assert-Boolean $relativeReceipt $field 'Relative-create receipt' ([bool]$relativeExpectedBooleans[$field])
            }
            Assert-ProductActions $relativeReceipt 'Relative-create receipt'
            if ($relativeReceipt.schema -cne 'range.pc11-v43-root-bootstrap-relative-create-receipt/v1' -or
                $relativeReceipt.status -cne 'PASS_REPARSE_REFUSED' -or
                $relativeReceipt.relativeCreateRefusedByWhitelistedReparseStatus -ne $true -or
                $relativeReceipt.cleanPositivePassed -ne $true -or $relativeReceipt.cleanPositiveDeleteSucceeded -ne $true -or
                -not (Test-SamePath ([string]$relativeReceipt.cleanPositiveExpectedPath) ([string]$relativeReceipt.cleanPositiveActualPath)) -or
                $relativeReceipt.redirectedOutputAppearedInTarget -ne $false -or $relativeReceipt.tempTreeRetained -ne $false -or
                $relativeReceipt.tempTreeCleanup -cne 'REMOVED_AFTER_DESCENDANT_AND_REPARSE_CHECK' -or
                $relativeReceipt.junctionCleanupAttempted -ne $true -or $relativeReceipt.junctionCleanupSucceeded -ne $true -or
                [string]$relativeReceipt.sourceSentinelSha256Before -cnotmatch '^[0-9a-f]{64}$' -or
                [string]$relativeReceipt.targetSentinelSha256Before -cnotmatch '^[0-9a-f]{64}$' -or
                [string]$relativeReceipt.sourceSentinelSha256After -cne [string]$relativeReceipt.sourceSentinelSha256Before -or
                [string]$relativeReceipt.targetSentinelSha256After -cne [string]$relativeReceipt.targetSentinelSha256Before) {
                throw 'Relative-create receipt did not prove exact refusal, clean identity/deletion, unchanged sentinels, and safe cleanup.'
            }
            $script:TestResults.root_bootstrap.status = 'PASS'
            $script:TestResults.root_bootstrap.reason = 'Production VerifyOnly passed 31 synthetic no-GO cases under fake-first PATH; unchanged production relative-create returned PASS_REPARSE_REFUSED with clean identity, unchanged sentinels, no redirect, and safe cleanup.'
            $script:TestEvidence.root_bootstrap = [ordered]@{
                verifyOnlyStdoutPath=(Join-Path $script:EvidenceRoot 'root-bootstrap.stdout.txt')
                verifyOnlyStderrPath=(Join-Path $script:EvidenceRoot 'root-bootstrap.stderr.txt')
                verifyOnlyScenarioCount=31
                verifyOnlyPositiveCount=6
                verifyOnlyRejectedCount=24
                verifyOnlyFakeFirstPathConfirmed=$true
                verifyOnlyPinnedPowerShellChildUsed=$true
                verifyOnlyFakeExecutableNeverRan=$true
                actualReviewOrDecisionPerformed=$false
                relativeCreateReceiptPath=$relativeReceiptPath
                relativeCreateReceiptSha256=(Get-Sha256Bytes $relativeReceiptBytes)
                relativeCreateStatus=[string]$relativeReceipt.status
                cleanPositiveIdentityMatched=$true
                cleanPositiveDeleted=$true
                sourceSentinelUnchanged=$true
                targetSentinelUnchanged=$true
                redirectedOutputAppearedInTarget=$false
                junctionCleanupSucceeded=$true
                temporaryTreeCleaned=$true
            }
        }
        catch {
            $script:Failure = $_.Exception.Message
            $script:TestResults.root_bootstrap.status = 'FAIL'
            $script:TestResults.root_bootstrap.reason = $script:Failure
        }
    }
}

if ($script:LockedSources.Count -gt 0) {
    foreach ($sourceRow in $script:LockedSources) {
        if ((Get-StreamSha256 $sourceRow.stream) -cne [string]$sourceRow.sha256) {
            if ($null -eq $script:Failure) { $script:Failure = "Source bytes changed during final-readiness execution: $($sourceRow.relativePath)" }
            break
        }
    }
}
$overall = if ($null -ne $script:Failure) { 'FAIL' }
    elseif ($null -ne $script:PendingReason) { 'PENDING' }
    elseif (@($script:TestResults.Values | Where-Object { $_.status -ne 'PASS' }).Count -eq 0) { 'PASS' }
    else { 'FAIL' }
if ($overall -eq 'FAIL' -and $null -eq $script:Failure) { $script:Failure = 'One or more required final-readiness execution checks did not pass.' }
$receipt = [ordered]@{
    schema='range.pc11-v43-final-readiness-receipt/v1'
    status=$overall
    candidateGateStatus=if ($candidateGateStatus -eq 'PASS') { 'PASS' } else { 'PENDING' }
    candidateVersion='v43'
    variant='OFF'
    staticOnly=$true
    candidateManifestSha256=$manifestSha
    sourceFreezeReceiptSha256=$freezeSha
    candidateManifestIdentitySha256=$identitySha
    sourceTreeSha256=$sourceTreeSha
    evidenceScope='FROZEN_SOURCE_IMPLEMENTATION'
    candidateQualification='NOT_ESTABLISHED'
    fixtureInputSemantics='SYNTHETIC_FIXTURE_RECEIPTS; FROZEN_SOURCE_IMPLEMENTATION_EXECUTION_ONLY'
    actualReviewOrDecisionPerformed=$false
    results=@($script:TestResults.watchdog_preflight,$script:TestResults.path_shadow,$script:TestResults.root_bootstrap)
    testEvidence=$script:TestEvidence
    strictValidatorNegativeCases=@($script:ValidatorNegativeCases)
    lockedSourceFiles=@($script:LockedSources | ForEach-Object { [pscustomobject][ordered]@{ path=$_.relativePath; sha256=$_.sha256 } })
    sourceLockCount=$script:LockedSources.Count
    prerequisitePendingReason=$script:PendingReason
    failure=$script:Failure
    productActions=[ordered]@{ guiLaunched=$false; savePerformed=$false; runPerformed=$false; confirmPerformed=$false }
    goPermitted=$false
}
Assert-NoGo $receipt 'Final-readiness receipt'
$receiptBytes = (New-Object Text.UTF8Encoding($false, $true)).GetBytes(($receipt | ConvertTo-Json -Depth 24) + [Environment]::NewLine)
$receiptStream = [IO.File]::Open($script:OutputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $receiptStream.Write($receiptBytes, 0, $receiptBytes.Length); $receiptStream.Flush($true) }
finally {
    $receiptStream.Dispose()
    foreach ($stream in $script:OpenStreams) { $stream.Dispose() }
}
Write-Output ("{0} candidateGate={1} results={2} sourceLocks={3} goPermitted=false output={4}" -f $overall, $receipt.candidateGateStatus, @($receipt.results).Count, $script:LockedSources.Count, $script:OutputPath)
if ($overall -eq 'FAIL') { exit 1 }
