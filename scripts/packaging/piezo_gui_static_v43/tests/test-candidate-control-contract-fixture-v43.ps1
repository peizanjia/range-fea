[CmdletBinding()]
param(
    [string]$AuditRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'The candidate control synthetic fixture requires Windows PowerShell 5.1.'
}

$prepRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoRoot = [IO.Path]::GetFullPath((Join-Path $prepRoot '..\..\..'))
$historicalAuditRoot = Join-Path $repoRoot 'build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit'
if ([string]::IsNullOrWhiteSpace($AuditRoot)) {
    $AuditRoot = Join-Path $repoRoot ('build\audit-20261014\piezo-delivery\candidate-control-contract-' + [Guid]::NewGuid().ToString('N'))
}
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)
[void][IO.Directory]::CreateDirectory($AuditRoot)
[void][IO.Directory]::CreateDirectory($historicalAuditRoot)

$gatePath = Join-Path $PSScriptRoot 'test-candidate-control-contract-v43.ps1'
$saveOutput = & (Join-Path $PSScriptRoot 'test-save-predicate-v43.ps1') -AuditRoot $AuditRoot
if ([string]$saveOutput -cnotmatch 'output=(.+)$') { throw 'Current Save-predicate fixture did not produce a parseable receipt.' }
$savePath = [string]$Matches[1]
$samplerOutput = & (Join-Path $PSScriptRoot 'test-sampler-arguments-v43.ps1') -AuditRoot $AuditRoot
if ([string]$samplerOutput -cnotmatch 'receipt=(.+)$') { throw 'Current sampler fixture did not produce a parseable receipt.' }
$samplerPath = [string]$Matches[1]
$watchdogPath = Join-Path $historicalAuditRoot ('supervisor-worker-contract-receipt-v43-fresh-' + [Guid]::NewGuid().ToString('N') + '.json')
$null = & (Join-Path $PSScriptRoot 'test-supervisor-worker-contract-v43.ps1') -OutputPath $watchdogPath
foreach ($path in @($gatePath, $savePath, $samplerPath, $watchdogPath)) {
    if (-not [IO.File]::Exists($path)) { throw "Required v43 candidate-control fixture input is missing: $path" }
}

function Get-FixtureSha([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}
function Get-FixtureShaBytes([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Get-FixtureRepoPath([string]$RelativePath) {
    $relative = $RelativePath.Replace('/', '\')
    if ($relative.StartsWith('..\..\..\scripts\', [StringComparison]::OrdinalIgnoreCase)) {
        $repoRelative = $relative.Substring(9)
    } else {
        $repoRelative = 'scripts\packaging\piezo_gui_static_v43\' + $relative
    }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $repoRelative))
}
function Write-FixtureJson([string]$Path, $Value) {
    $parent = Split-Path -Parent $Path
    if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
    $bytes = (New-Object Text.UTF8Encoding($false, $true)).GetBytes(($Value | ConvertTo-Json -Depth 30) + "`n")
    [IO.File]::WriteAllBytes($Path, $bytes)
    return Get-FixtureShaBytes $bytes
}
function Copy-FixtureJson([string]$Path) {
    return (ConvertFrom-Json -InputObject ([IO.File]::ReadAllText($Path)))
}
function Set-FixtureProperty($Object, [string]$Name, $Value) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value -Force }
    else { $property.Value = $Value }
}
function Get-FixtureManifestIdentity($Manifest) {
    $binding = $Manifest.installBinding
    $source = $Manifest.sourceAndAuthorityPins
    $jsonString = { param([string]$s) [string]($s | ConvertTo-Json -Compress -ErrorAction Stop) }
    $parts = @(
        '{"candidateVersion":"v43"',
        ',"installRoot":' + (& $jsonString ([string]$binding.installRoot)),
        ',"inventoryReportSha256":' + (& $jsonString ([string]$binding.inventoryReportSha256).ToLowerInvariant()),
        ',"releaseBuildReceiptSha256":' + (& $jsonString ([string]$source.releaseBuildReceiptSha256).ToLowerInvariant()),
        ',"rootCommit":' + (& $jsonString ([string]$source.rootCommit).ToLowerInvariant()),
        ',"schema":"range.pc11-fresh-run-v43-admission-candidate/v1"',
        ',"sourceFreezeReceiptSha256":' + (& $jsonString ([string]$source.sourceFreezeReceiptSha256).ToLowerInvariant()),
        ',"sourceTreeSha256":' + (& $jsonString ([string]$source.sourceTreeSha256).ToLowerInvariant()) + '}'
    )
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash((New-Object Text.UTF8Encoding($false, $true)).GetBytes([string]::Join('', $parts))))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function New-FixtureExecutionEnvelope([string]$ComponentId, [string]$ReceiptPath, [string]$Identity, [string]$SourceTree, [string[]]$SourcePaths) {
    $sourceRows = @()
    foreach ($relativePath in $SourcePaths) {
        $sourceRows += [pscustomobject]@{ path = $relativePath; sha256 = Get-FixtureSha (Get-FixtureRepoPath $relativePath) }
    }
    return [pscustomobject][ordered]@{
        schema = 'range.pc11-v43-frozen-fixture-execution/v1'
        componentId = $ComponentId
        originalReceipt = [pscustomobject][ordered]@{ path = [IO.Path]::GetFullPath($ReceiptPath); sha256 = Get-FixtureSha $ReceiptPath }
        candidateManifestIdentitySha256 = $Identity
        sourceTreeSha256 = $SourceTree
        sourceBindingMethod = 'FROZEN_SOURCE_EXECUTION'
        fixtureOnly = $true
        staticOnly = $true
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
        sources = $sourceRows
    }
}
function Invoke-CandidateGate([string]$CaseRoot, [string]$ManifestPath, [string]$SaveEnvelopePath, [string]$SamplerEnvelopePath, [string]$WatchdogEnvelopePath, [string]$OutputName = 'candidate-control-receipt.json', [string]$SaveInputShaOverride = '') {
    $outputPath = Join-Path $CaseRoot $OutputName
    $saveInputSha = if ([string]::IsNullOrWhiteSpace($SaveInputShaOverride)) { Get-FixtureSha $SaveEnvelopePath } else { $SaveInputShaOverride }
    $arguments = @(
        '-NoProfile','-ExecutionPolicy','Bypass','-File',$gatePath,
        '-CandidateManifestPath',$ManifestPath,'-CandidateManifestSha256',(Get-FixtureSha $ManifestPath),
        '-SavePredicateReceiptPath',$SaveEnvelopePath,'-SavePredicateReceiptSha256',$saveInputSha,
        '-SamplerArgumentsReceiptPath',$SamplerEnvelopePath,'-SamplerArgumentsReceiptSha256',(Get-FixtureSha $SamplerEnvelopePath),
        '-SupervisorWorkerReceiptPath',$WatchdogEnvelopePath,'-SupervisorWorkerReceiptSha256',(Get-FixtureSha $WatchdogEnvelopePath),
        '-AuditRoot',$tempRoot,'-OutputPath',$outputPath
    )
    $childOutput = & (Join-Path $PSHOME 'powershell.exe') @arguments 2>&1
    if ($LASTEXITCODE -ne 0 -or -not [IO.File]::Exists($outputPath)) {
        throw "Candidate-control gate did not produce a receipt for synthetic case $([IO.Path]::GetFileName($CaseRoot)): $($childOutput -join ' ')"
    }
    return (Copy-FixtureJson $outputPath)
}
function Assert-FixtureNoGo($Receipt, [string]$Label) {
    if ($Receipt.goPermitted -ne $false -or
        @('guiLaunched','savePerformed','runPerformed','confirmPerformed' | Where-Object { $Receipt.productActions.$_ -ne $false }).Count -gt 0) {
        throw "$Label did not preserve the no-GO/no-product-action boundary."
    }
}

$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$tempRoot = Join-Path $tempBase ('range-fea-v43-candidate-control-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($tempRoot)
try {
    $gitHead = (& git -C $repoRoot rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0 -or [string]$gitHead -notmatch '^(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})$') {
        throw 'Unable to read the current root repository commit for the synthetic source identity.'
    }
    $rootCommit = ([string]$gitHead).Trim().ToLowerInvariant()
    $sourceTree = 'a' * 64
    $buildReceiptSha = 'b' * 64
    $releaseBuildPath = Join-Path $tempRoot 'synthetic-release-build-receipt.json'
    $syntheticInstallRoot = Join-Path $tempRoot 'synthetic-off-install'
    [void][IO.Directory]::CreateDirectory($syntheticInstallRoot)
    $requiredStaticTestIds = @(
        'source_preflight','source_freeze','package_tree','module_boundary_build','module_boundary_source',
        'watchdog_preflight','save_predicate_preflight','path_shadow','root_seal','save_predicate',
        'watchdog_contract','sampler_arguments','supervisor','worker_handshake','native_launch_boundary',
        'parser','finalize_integrity','source_static_tests','root_bootstrap'
    )
    $sourcePaths = @(
        'tools/save-predicate-v43.ps1','tests/test-save-predicate-v43.ps1',
        'runtime/sampler-arguments-v43.ps1','../../../scripts/benchmark/pc11_working_set.py','tests/test-sampler-arguments-v43.ps1',
        'runtime/contract-common-v43.ps1','runtime/supervisor-core-v43.ps1','runtime/worker-handshake-v43.ps1',
        'runtime/watchdog-contract-v43.ps1','runtime/native-launch-boundary-v43.ps1','tests/fixtures/worker-fixture-v43.ps1',
        'tests/test-supervisor-worker-contract-v43.ps1','finalize-v43.ps1',
        'tests/test-candidate-control-contract-v43.ps1'
    ) | Select-Object -Unique
    $sourceRows = @()
    foreach ($relativePath in $sourcePaths) {
        $sourceFile = Get-FixtureRepoPath $relativePath
        $sourceRows += [pscustomobject]@{ path = $sourceFile.Substring($repoRoot.TrimEnd('\').Length + 1).Replace('\', '/'); sha256 = Get-FixtureSha $sourceFile }
    }

    function Invoke-SyntheticCase([string]$CaseName, [string]$Target = '', [string]$Field = '', $Value = $null) {
        $caseRoot = Join-Path $tempRoot $CaseName
        [void][IO.Directory]::CreateDirectory($caseRoot)
        $inventoryPath = Join-Path $caseRoot 'off-inventory.json'
        $freezePath = Join-Path $caseRoot 'source-freeze.json'
        $manifestPath = Join-Path $caseRoot 'candidate-manifest.json'
        $saveReceiptPath = Join-Path $caseRoot 'save-predicate-receipt.json'
        $samplerReceiptPath = Join-Path $caseRoot 'sampler-receipt.json'
        $watchdogReceiptPath = Join-Path $caseRoot 'watchdog-receipt.json'
        $saveEnvelopePath = Join-Path $caseRoot 'save-execution-envelope.json'
        $samplerEnvelopePath = Join-Path $caseRoot 'sampler-execution-envelope.json'
        $watchdogEnvelopePath = Join-Path $caseRoot 'watchdog-execution-envelope.json'

        $inventory = [pscustomobject][ordered]@{
            schemaVersion = 2
            inventoryStatus = 'PASS'
            releaseQualificationStatus = 'INCOMPLETE_LINK_MAP_NOT_SUPPLIED'
            variant = 'OFF'
            installRoot = $syntheticInstallRoot
        }
        $freezeActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        $freeze = [pscustomobject][ordered]@{
            schema = 'range.pc11-v43-source-freeze/v1'
            status = 'PASS'
            candidateVersion = 'v43'
            variant = 'OFF'
            configuration = 'Release'
            sourceRootPath = $repoRoot
            rootCommit = $rootCommit
            sourceTreeSha256 = $sourceTree
            releaseBuildReceiptPath = $releaseBuildPath
            releaseBuildReceiptSha256 = $buildReceiptSha
            sourceFiles = $sourceRows
            staticOnly = $true
            productActions = $freezeActions
            goPermitted = $false
        }
        $manifestActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        $manifest = [pscustomobject][ordered]@{
            schema = 'range.pc11-fresh-run-v43-admission-candidate/v1'
            candidateVersion = 'v43'
            candidateState = 'SYNTHETIC_FIXTURE_ONLY'
            goPermitted = $false
            productActions = $manifestActions
            installBinding = [pscustomobject][ordered]@{
                requiredVariant = 'OFF'; installRoot = $syntheticInstallRoot
                inventoryReportPath = $inventoryPath; inventoryReportSha256 = ''
                inventorySchemaVersion = 2; inventoryStatus = 'PASS'
            }
            sourceAndAuthorityPins = [pscustomobject][ordered]@{
                sourceRootPath = $repoRoot; sourceFreezeReceiptPath = $freezePath; sourceFreezeReceiptSha256 = ''
                rootCommit = $rootCommit; sourceTreeSha256 = $sourceTree
                releaseBuildReceiptPath = $releaseBuildPath; releaseBuildReceiptSha256 = $buildReceiptSha
            }
            candidateEvidencePins = [pscustomobject]@{ requiredStaticTestIds = $requiredStaticTestIds }
        }
        switch ($Target) {
            'inventory' { $inventory.$Field = $Value }
            'freeze' { $freeze.$Field = $Value }
            'manifest' {
                switch ($Field) {
                    'requiredVariant' { $manifest.installBinding.requiredVariant = $Value }
                    'inventorySchemaVersion' { $manifest.installBinding.inventorySchemaVersion = $Value }
                    'inventoryStatus' { $manifest.installBinding.inventoryStatus = $Value }
                    'sourceRootPath' { $manifest.sourceAndAuthorityPins.sourceRootPath = $Value }
                    'goPermitted' { $manifest.goPermitted = $Value }
                    'savePerformed' { $manifest.productActions.savePerformed = $Value }
                }
            }
        }

        $inventorySha = Write-FixtureJson $inventoryPath $inventory
        $manifest.installBinding.inventoryReportSha256 = $inventorySha
        $freezeSha = Write-FixtureJson $freezePath $freeze
        $manifest.sourceAndAuthorityPins.sourceFreezeReceiptSha256 = $freezeSha
        $manifestSha = Write-FixtureJson $manifestPath $manifest
        $identity = Get-FixtureManifestIdentity $manifest
        [IO.File]::Copy($savePath, $saveReceiptPath)
        [IO.File]::Copy($samplerPath, $samplerReceiptPath)
        [IO.File]::Copy($watchdogPath, $watchdogReceiptPath)
        if ($Target -ceq 'saveSources') {
            $save = Copy-FixtureJson $saveReceiptPath
            $rows = @($save.sourceSha256)
            if ($Field -ceq 'missing') { $save.sourceSha256 = @($rows | Select-Object -First 1) }
            elseif ($Field -ceq 'extra') { $save.sourceSha256 = @($rows) + @([pscustomobject]@{ path = 'tools/unexpected-producer.ps1'; sha256 = ('c' * 64) }) }
            elseif ($Field -ceq 'duplicate') { $rows[1].path = $rows[0].path; $save.sourceSha256 = $rows }
            [void](Write-FixtureJson $saveReceiptPath $save)
        }

        $saveEnvelope = New-FixtureExecutionEnvelope 'save_predicate' $saveReceiptPath $identity $sourceTree @('tools/save-predicate-v43.ps1','tests/test-save-predicate-v43.ps1')
        $samplerEnvelope = New-FixtureExecutionEnvelope 'sampler_arguments' $samplerReceiptPath $identity $sourceTree @('runtime/sampler-arguments-v43.ps1','../../../scripts/benchmark/pc11_working_set.py','tests/test-sampler-arguments-v43.ps1')
        $watchdogEnvelope = New-FixtureExecutionEnvelope 'watchdog_contract' $watchdogReceiptPath $identity $sourceTree @('runtime/contract-common-v43.ps1','runtime/supervisor-core-v43.ps1','runtime/worker-handshake-v43.ps1','runtime/watchdog-contract-v43.ps1','runtime/native-launch-boundary-v43.ps1','tests/fixtures/worker-fixture-v43.ps1','tests/test-supervisor-worker-contract-v43.ps1','finalize-v43.ps1')
        if ($Target -ceq 'envelope') {
            switch ($Field) {
                'componentId' { $saveEnvelope.componentId = [string]$Value }
                'candidateManifestIdentitySha256' { $saveEnvelope.candidateManifestIdentitySha256 = [string]$Value }
                'sourceTreeSha256' { $saveEnvelope.sourceTreeSha256 = [string]$Value }
                'sourceBindingMethod' { $saveEnvelope.sourceBindingMethod = [string]$Value }
                'fixtureOnly' { $saveEnvelope.fixtureOnly = [bool]$Value }
                'staticOnly' { $saveEnvelope.staticOnly = [bool]$Value }
                'goPermitted' { $saveEnvelope.goPermitted = [bool]$Value }
                'productActions' { $saveEnvelope.productActions = $Value }
                'sourceMissing' { $saveEnvelope.sources = @($saveEnvelope.sources | Select-Object -Skip 1) }
                'sourceExtra' { $saveEnvelope.sources = @($saveEnvelope.sources) + @([pscustomobject]@{ path='tools/unexpected.ps1'; sha256=('f' * 64) }) }
                'sourceDuplicate' { $saveEnvelope.sources = @($saveEnvelope.sources) + @($saveEnvelope.sources[0]) }
                'sourceHash' { $saveEnvelope.sources[0].sha256 = '0' * 64 }
                'originalReceiptSha256' { $saveEnvelope.originalReceipt.sha256 = '0' * 64 }
            }
        }
        [void](Write-FixtureJson $saveEnvelopePath $saveEnvelope)
        [void](Write-FixtureJson $samplerEnvelopePath $samplerEnvelope)
        [void](Write-FixtureJson $watchdogEnvelopePath $watchdogEnvelope)
        return Invoke-CandidateGate $caseRoot $manifestPath $saveEnvelopePath $samplerEnvelopePath $watchdogEnvelopePath
    }

    $positive = Invoke-SyntheticCase 'positive-candidate'
    Assert-FixtureNoGo $positive 'Synthetic positive candidate'
    if ($positive.candidateGateStatus -cne 'PASS' -or $positive.status -cne 'STATIC_INCOMPLETE' -or
        $positive.results.Count -ne 3 -or @($positive.results | Where-Object { $_.status -cne 'PASS' }).Count -ne 0) {
        throw 'Strict synthetic OFF inventory/source-freeze candidate did not pass the static contract gate.'
    }
    if (@($positive.results | Where-Object { $_.sourcePathBinding -cne 'RECEIPT_AND_FROZEN_SOURCE_HASHES_MATCH' -or
            $_.candidateIdentityBinding -cne 'EXTERNAL_MANIFEST_IDENTITY_AND_SOURCE_TREE_MATCH' }).Count -ne 0 -or
        $positive.gateProducerSource.sourceFreezeBinding -cne 'MATCH') {
        throw 'Positive component and evaluator receipts did not report the required source and candidate identity bindings.'
    }
    $positiveSaveRow = @($positive.results | Where-Object { $_.testId -ceq 'save_predicate' })
    $expectedSavePaths = @('tools/save-predicate-v43.ps1','tests/test-save-predicate-v43.ps1')
    if ($positiveSaveRow.Count -ne 1 -or $positiveSaveRow[0].producerSourcePaths.Count -ne 2 -or
        @($positiveSaveRow[0].producerSourcePaths | Where-Object { $_ -cnotin $expectedSavePaths }).Count -ne 0 -or
        @($expectedSavePaths | Where-Object { $_ -cnotin $positiveSaveRow[0].producerSourcePaths }).Count -ne 0) {
        throw 'Positive Save receipt did not bind exactly its two required production script paths.'
    }
    $positiveCaseRoot = Join-Path $tempRoot 'positive-candidate'
    $positiveManifestPath = Join-Path $positiveCaseRoot 'candidate-manifest.json'
    $positiveSavePath = Join-Path $positiveCaseRoot 'save-predicate-receipt.json'
    $positiveSamplerPath = Join-Path $positiveCaseRoot 'sampler-receipt.json'
    $positiveWatchdogPath = Join-Path $positiveCaseRoot 'watchdog-receipt.json'
    $positiveSaveEnvelopePath = Join-Path $positiveCaseRoot 'save-execution-envelope.json'
    $positiveSamplerEnvelopePath = Join-Path $positiveCaseRoot 'sampler-execution-envelope.json'
    $positiveWatchdogEnvelopePath = Join-Path $positiveCaseRoot 'watchdog-execution-envelope.json'
    if ((Get-FixtureSha $positiveSavePath) -cne (Get-FixtureSha $savePath) -or
        (Get-FixtureSha $positiveSamplerPath) -cne (Get-FixtureSha $samplerPath) -or
        (Get-FixtureSha $positiveWatchdogPath) -cne (Get-FixtureSha $watchdogPath)) {
        throw 'Candidate-bound envelopes changed component producer receipt bytes.'
    }
    if ($positiveSaveRow[0].fixtureReceiptPath -cne [IO.Path]::GetFullPath($positiveSavePath) -or
        $positiveSaveRow[0].executionEnvelopePath -cne [IO.Path]::GetFullPath($positiveSaveEnvelopePath) -or
        $positiveSaveRow[0].sourceBindingMethod -cne 'FROZEN_SOURCE_EXECUTION') {
        throw 'Candidate-control row does not preserve both original receipt and execution-envelope provenance.'
    }
    $rawReceipts = Invoke-CandidateGate $positiveCaseRoot $positiveManifestPath $positiveSavePath $positiveSamplerPath $positiveWatchdogPath 'raw-receipts-pending.json'
    Assert-FixtureNoGo $rawReceipts 'Raw receipt candidate binding'
    if ($rawReceipts.candidateGateStatus -cne 'PENDING' -or @($rawReceipts.results | Where-Object { $_.status -cne 'PENDING' }).Count -ne 0) {
        throw 'Raw component receipts without frozen-execution envelopes were promoted to candidate-bound PASS.'
    }

    $sampler = Copy-FixtureJson $positiveSamplerPath
    $sampler.arguments[4] = 'C:\unrelated\image.exe'
    [void](Write-FixtureJson $positiveSamplerPath $sampler)
    $samplerEnvelope = Copy-FixtureJson $positiveSamplerEnvelopePath
    $samplerEnvelope.originalReceipt.sha256 = Get-FixtureSha $positiveSamplerPath
    [void](Write-FixtureJson $positiveSamplerEnvelopePath $samplerEnvelope)
    $samplerVectorTamper = Invoke-CandidateGate $positiveCaseRoot $positiveManifestPath $positiveSaveEnvelopePath $positiveSamplerEnvelopePath $positiveWatchdogEnvelopePath 'sampler-vector-tamper.json'
    Assert-FixtureNoGo $samplerVectorTamper 'Sampler vector tamper'
    $samplerTamperRows = @($samplerVectorTamper.results | Where-Object { $_.testId -ceq 'sampler_arguments' })
    if ($samplerVectorTamper.candidateGateStatus -cne 'PENDING' -or $samplerTamperRows.Count -ne 1 -or
        $samplerTamperRows[0].semanticCoverage -cne 'PENDING_OR_INCOMPLETE_FIXTURE_COVERAGE') {
        throw 'Candidate-control gate accepted a rehashed sampler receipt with a changed image slot.'
    }

    $saveReceiptShaBeforeTamper = Get-FixtureSha $positiveSavePath
    $saveReceiptBytes = [IO.File]::ReadAllBytes($positiveSavePath)
    [IO.File]::WriteAllBytes($positiveSavePath, $saveReceiptBytes + [byte[]]@(0x20))
    $saveByteTamper = Invoke-CandidateGate $positiveCaseRoot $positiveManifestPath $positiveSaveEnvelopePath $positiveSamplerEnvelopePath $positiveWatchdogEnvelopePath 'save-original-receipt-byte-tamper.json'
    Assert-FixtureNoGo $saveByteTamper 'Save original receipt byte-pin tamper'
    $saveTamperRows = @($saveByteTamper.results | Where-Object { $_.testId -ceq 'save_predicate' })
    if ($saveByteTamper.candidateGateStatus -cne 'PENDING' -or $saveTamperRows.Count -ne 1 -or
        $saveTamperRows[0].status -cne 'PENDING' -or $saveTamperRows[0].reason -notmatch 'original component receipt bytes differ from the external SHA256 pin') {
        throw 'Candidate-control gate did not reject original Save receipt bytes changed after the envelope was written.'
    }

    $negativeCases = @(
        [pscustomobject]@{ name='save-source-missing'; target='saveSources'; field='missing'; value=$null },
        [pscustomobject]@{ name='save-source-extra'; target='saveSources'; field='extra'; value=$null },
        [pscustomobject]@{ name='save-source-duplicate'; target='saveSources'; field='duplicate'; value=$null },
        [pscustomobject]@{ name='inventory-schema'; target='inventory'; field='schemaVersion'; value=1 },
        [pscustomobject]@{ name='inventory-schema-type'; target='inventory'; field='schemaVersion'; value='2' },
        [pscustomobject]@{ name='inventory-variant'; target='inventory'; field='variant'; value='ON' },
        [pscustomobject]@{ name='inventory-status'; target='inventory'; field='inventoryStatus'; value='FAIL' },
        [pscustomobject]@{ name='inventory-install-root'; target='inventory'; field='installRoot'; value=$tempRoot },
        [pscustomobject]@{ name='manifest-required-variant'; target='manifest'; field='requiredVariant'; value='ON' },
        [pscustomobject]@{ name='manifest-schema-version'; target='manifest'; field='inventorySchemaVersion'; value=1 },
        [pscustomobject]@{ name='manifest-schema-version-type'; target='manifest'; field='inventorySchemaVersion'; value='2' },
        [pscustomobject]@{ name='manifest-inventory-status'; target='manifest'; field='inventoryStatus'; value='FAIL' },
        [pscustomobject]@{ name='candidate-source-root'; target='manifest'; field='sourceRootPath'; value=$tempRoot },
        [pscustomobject]@{ name='candidate-go'; target='manifest'; field='goPermitted'; value=$true },
        [pscustomobject]@{ name='candidate-product-action'; target='manifest'; field='savePerformed'; value=$true },
        [pscustomobject]@{ name='envelope-component'; target='envelope'; field='componentId'; value='sampler_arguments' },
        [pscustomobject]@{ name='envelope-identity'; target='envelope'; field='candidateManifestIdentitySha256'; value=('0' * 64) },
        [pscustomobject]@{ name='envelope-source-tree'; target='envelope'; field='sourceTreeSha256'; value=('d' * 64) },
        [pscustomobject]@{ name='envelope-source-method'; target='envelope'; field='sourceBindingMethod'; value='POST_HOC_STAMP' },
        [pscustomobject]@{ name='envelope-fixture-boundary'; target='envelope'; field='fixtureOnly'; value=$false },
        [pscustomobject]@{ name='envelope-static-boundary'; target='envelope'; field='staticOnly'; value=$false },
        [pscustomobject]@{ name='envelope-go'; target='envelope'; field='goPermitted'; value=$true },
        [pscustomobject]@{ name='envelope-product-action'; target='envelope'; field='productActions'; value=[ordered]@{ guiLaunched=$false; savePerformed=$true; runPerformed=$false; confirmPerformed=$false } },
        [pscustomobject]@{ name='envelope-source-missing'; target='envelope'; field='sourceMissing'; value=$null },
        [pscustomobject]@{ name='envelope-source-extra'; target='envelope'; field='sourceExtra'; value=$null },
        [pscustomobject]@{ name='envelope-source-duplicate'; target='envelope'; field='sourceDuplicate'; value=$null },
        [pscustomobject]@{ name='envelope-source-hash'; target='envelope'; field='sourceHash'; value=$null },
        [pscustomobject]@{ name='envelope-original-receipt-pin'; target='envelope'; field='originalReceiptSha256'; value=$null },
        [pscustomobject]@{ name='freeze-schema'; target='freeze'; field='schema'; value='wrong-schema' },
        [pscustomobject]@{ name='freeze-status'; target='freeze'; field='status'; value='FAIL' },
        [pscustomobject]@{ name='freeze-version'; target='freeze'; field='candidateVersion'; value='v42' },
        [pscustomobject]@{ name='freeze-variant'; target='freeze'; field='variant'; value='ON' },
        [pscustomobject]@{ name='freeze-configuration'; target='freeze'; field='configuration'; value='Debug' },
        [pscustomobject]@{ name='freeze-root'; target='freeze'; field='sourceRootPath'; value=$tempRoot },
        [pscustomobject]@{ name='freeze-static-only'; target='freeze'; field='staticOnly'; value=$false },
        [pscustomobject]@{ name='freeze-go'; target='freeze'; field='goPermitted'; value=$true },
        [pscustomobject]@{ name='freeze-product-action'; target='freeze'; field='productActions'; value=[ordered]@{ guiLaunched=$false; savePerformed=$true; runPerformed=$false; confirmPerformed=$false } },
        [pscustomobject]@{ name='freeze-product-action-type'; target='freeze'; field='productActions'; value=[ordered]@{ guiLaunched=$false; savePerformed=0; runPerformed=$false; confirmPerformed=$false } },
        [pscustomobject]@{ name='freeze-root-commit-binding'; target='freeze'; field='rootCommit'; value=('0' * 40) },
        [pscustomobject]@{ name='freeze-tree-binding'; target='freeze'; field='sourceTreeSha256'; value=('d' * 64) },
        [pscustomobject]@{ name='freeze-build-sha-binding'; target='freeze'; field='releaseBuildReceiptSha256'; value=('e' * 64) },
        [pscustomobject]@{ name='freeze-build-path-binding'; target='freeze'; field='releaseBuildReceiptPath'; value=(Join-Path $tempRoot 'other-build.json') }
    )
    foreach ($case in $negativeCases) {
        $result = Invoke-SyntheticCase ([string]$case.name) ([string]$case.target) ([string]$case.field) $case.value
        Assert-FixtureNoGo $result ([string]$case.name)
        if ($result.candidateGateStatus -cne 'PENDING' -or $result.missingPrerequisites.Count -lt 1) {
            throw "Candidate-control gate accepted malformed synthetic case $($case.name)."
        }
        if ($case.target -ceq 'saveSources') {
            $saveRow = @($result.results | Where-Object { $_.testId -ceq 'save_predicate' })
            if ($saveRow.Count -ne 1 -or $saveRow[0].status -cne 'PENDING' -or $saveRow[0].reason -notmatch 'exactly the two required producer paths') {
                throw "Candidate-control gate did not identify the malformed Save sourceSha256 set in $($case.name)."
            }
        }
    }

    $tamperCase = Invoke-SyntheticCase 'inventory-byte-pin-tamper'
    $tamperInventory = Join-Path $tempRoot 'inventory-byte-pin-tamper\off-inventory.json'
    $tamperInventoryBytes = [IO.File]::ReadAllBytes($tamperInventory)
    [IO.File]::WriteAllBytes($tamperInventory, $tamperInventoryBytes + [byte[]]@(0x20))
    if ($tamperCase.candidateGateStatus -cne 'PASS') { throw 'Valid synthetic candidate did not establish a baseline before inventory byte tampering.' }
    $tamperedResult = Invoke-CandidateGate `
        (Join-Path $tempRoot 'inventory-byte-pin-tamper') `
        (Join-Path $tempRoot 'inventory-byte-pin-tamper\candidate-manifest.json') `
        (Join-Path $tempRoot 'inventory-byte-pin-tamper\save-execution-envelope.json') `
        (Join-Path $tempRoot 'inventory-byte-pin-tamper\sampler-execution-envelope.json') `
        (Join-Path $tempRoot 'inventory-byte-pin-tamper\watchdog-execution-envelope.json') `
        'candidate-control-tamper.json'
    Assert-FixtureNoGo $tamperedResult 'Inventory byte-pin tamper'
    if ($tamperedResult.candidateGateStatus -cne 'PENDING' -or
        @($tamperedResult.missingPrerequisites | Where-Object { $_ -match 'Candidate OFF inventory report bytes differ from the external SHA256 pin' }).Count -ne 1) {
        throw 'Candidate-control gate did not reject inventory bytes changed after the manifest pin was written.'
    }

    Write-Output ("PASS candidate-control strict synthetic positive, raw-receipt pending boundary, {0} contract metadata/source negatives, original-receipt and sampler-vector tamper, inventory byte-pin tamper, no-GO; auditRoot={1}" -f $negativeCases.Count, $AuditRoot)
}
finally {
    if ([IO.Directory]::Exists($tempRoot)) {
        $resolved = [IO.Path]::GetFullPath($tempRoot)
        if (-not $resolved.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -or
            [IO.Path]::GetFileName($resolved) -notmatch '^range-fea-v43-candidate-control-[0-9a-f]{32}$' -or
            (([IO.File]::GetAttributes($resolved) -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw 'Refusing to remove the candidate-control fixture directory outside its verified unique system-temp root.'
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
