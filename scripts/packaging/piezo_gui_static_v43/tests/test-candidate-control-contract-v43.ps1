[CmdletBinding()]
param(
    [string]$CandidateManifestPath,
    [string]$CandidateManifestSha256,
    [string]$SavePredicateReceiptPath,
    [string]$SavePredicateReceiptSha256,
    [string]$SamplerArgumentsReceiptPath,
    [string]$SamplerArgumentsReceiptSha256,
    [string]$SupervisorWorkerReceiptPath,
    [string]$SupervisorWorkerReceiptSha256,
    [string]$OutputPath,
    [string]$AuditRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'The v43 candidate control contract gate requires Windows PowerShell 5.1.'
}

$prepRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoRoot = [IO.Path]::GetFullPath((Join-Path $prepRoot '..\..\..'))
$requiredStaticTestIds = @(
    'source_preflight','source_freeze','package_tree','module_boundary_build','module_boundary_source',
    'watchdog_preflight','save_predicate_preflight','path_shadow','root_seal','save_predicate',
    'watchdog_contract','sampler_arguments','supervisor','worker_handshake','native_launch_boundary',
    'parser','finalize_integrity','source_static_tests','root_bootstrap'
)
if (-not $AuditRoot) { $AuditRoot = Join-Path $repoRoot 'build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit' }
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)
if (-not $OutputPath) { $OutputPath = Join-Path $AuditRoot ('candidate-control-contract-v43-' + [Guid]::NewGuid().ToString('N') + '.json') }
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$auditPrefix = $AuditRoot.TrimEnd('\') + '\'
if (-not $OutputPath.StartsWith($auditPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'The candidate control receipt must be written under the external audit root.' }
if ([IO.File]::Exists($OutputPath)) { throw 'Refusing to overwrite a single-use candidate control receipt.' }

function Get-CcSha256File([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { throw "Pinned file is missing: $Path" }
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}
$sourceLocks = New-Object 'System.Collections.Generic.List[System.IO.FileStream]'
function Get-CcLockedSourceSha256([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { throw "Pinned source file is missing: $Path" }
    # Keep a read-only sharing handle alive through receipt creation. Windows then
    # rejects write/delete/replace attempts that could swap A/B source bytes mid-run.
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    [void]$sourceLocks.Add($stream)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Position = 0 }
}
function Get-CcSha256Bytes([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Assert-CcObject($Value, [string]$Label) {
    if ($null -eq $Value -or $Value -is [Array] -or $Value -isnot [Management.Automation.PSCustomObject]) { throw "$Label must be a JSON object." }
}
function Assert-CcString($Object, [string]$Name, [string]$Label) {
    Assert-CcObject $Object $Label
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -isnot [string]) { throw "$Label.$Name must be a JSON string scalar." }
}
function Assert-CcBoolean($Object, [string]$Name, [string]$Label) {
    Assert-CcObject $Object $Label
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -isnot [bool]) { throw "$Label.$Name must be a JSON Boolean scalar." }
}
function Assert-CcObjectArray($Object, [string]$Name, [string]$Label) {
    Assert-CcObject $Object $Label
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -isnot [Array]) { throw "$Label.$Name must be a JSON array." }
    foreach ($item in $property.Value) { Assert-CcObject $item "$Label.$Name entry" }
}
function Assert-CcStringArray($Object, [string]$Name, [string]$Label) {
    Assert-CcObject $Object $Label
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -isnot [Array]) { throw "$Label.$Name must be a JSON array." }
    foreach ($item in $property.Value) { if ($item -isnot [string]) { throw "$Label.$Name entries must be JSON string scalars." } }
}
function Assert-CcSha([string]$Value, [string]$Label) {
    if ($Value -cnotmatch '^[0-9a-fA-F]{64}$') { throw "$Label must be exactly 64 hexadecimal characters." }
}
function Test-CcSamePath([string]$Left, [string]$Right) {
    if ([string]::IsNullOrWhiteSpace($Left) -or [string]::IsNullOrWhiteSpace($Right)) { return $false }
    return [StringComparer]::OrdinalIgnoreCase.Equals([IO.Path]::GetFullPath($Left).TrimEnd('\'), [IO.Path]::GetFullPath($Right).TrimEnd('\'))
}
function Assert-CcNoProductActions($Receipt, [string]$Label) {
    Assert-CcObject $Receipt $Label
    foreach ($action in @('guiLaunched','savePerformed','runPerformed','confirmPerformed')) {
        Assert-CcBoolean $Receipt $action $Label
        if ($Receipt.$action -ne $false) { throw "$Label.$action must be false." }
    }
}
function Find-CcPendingPlaceholder($Value, [string]$Path = '$') {
    if ($Value -is [string]) {
        if ($Value -match '^PENDING_') { return ,@($Path, $Value) }
        return $null
    }
    if ($Value -is [Management.Automation.PSCustomObject]) {
        foreach ($property in $Value.PSObject.Properties) {
            $found = Find-CcPendingPlaceholder $property.Value ($Path + '.' + $property.Name)
            if ($null -ne $found) { return ,$found }
        }
    } elseif ($Value -is [Array]) {
        for ($i = 0; $i -lt $Value.Count; $i++) {
            $found = Find-CcPendingPlaceholder $Value[$i] ($Path + '[' + $i + ']')
            if ($null -ne $found) { return ,$found }
        }
    }
    return $null
}
function Read-CcPinnedJson([string]$Path, [string]$ExpectedSha, [string]$Label) {
    Assert-CcSha $ExpectedSha "$Label external SHA256"
    $Path = [IO.Path]::GetFullPath($Path)
    if (-not [IO.File]::Exists($Path)) { throw "Pinned file is missing: $Path" }
    $bytes = [IO.File]::ReadAllBytes($Path)
    $actual = Get-CcSha256Bytes $bytes
    if ($actual -cne $ExpectedSha.ToLowerInvariant()) { throw "$Label bytes differ from the external SHA256 pin." }
    $text = (New-Object Text.UTF8Encoding($false, $true)).GetString($bytes)
    $value = $text | ConvertFrom-Json -ErrorAction Stop
    Assert-CcObject $value $Label
    return [pscustomobject]@{ path = $Path; sha256 = $actual; value = $value }
}
function Get-CcManifestIdentity($Manifest) {
    Assert-CcObject $Manifest 'Candidate manifest'
    Assert-CcString $Manifest 'schema' 'Candidate manifest'
    Assert-CcString $Manifest 'candidateVersion' 'Candidate manifest'
    Assert-CcObject $Manifest.installBinding 'Candidate manifest.installBinding'
    Assert-CcObject $Manifest.sourceAndAuthorityPins 'Candidate manifest.sourceAndAuthorityPins'
    Assert-CcObject $Manifest.candidateEvidencePins 'Candidate manifest.candidateEvidencePins'
    $binding = $Manifest.installBinding
    $source = $Manifest.sourceAndAuthorityPins
    Assert-CcBoolean $Manifest 'goPermitted' 'Candidate manifest'
    Assert-CcObject $Manifest.productActions 'Candidate manifest.productActions'
    foreach ($action in @('guiLaunched','savePerformed','runPerformed','confirmPerformed')) {
        Assert-CcBoolean $Manifest.productActions $action 'Candidate manifest.productActions'
        if ($Manifest.productActions.$action -ne $false) { throw "Candidate manifest permits product action $action." }
    }
    if ($Manifest.goPermitted -ne $false) { throw 'Candidate manifest permits GO.' }
    Assert-CcStringArray $Manifest.candidateEvidencePins 'requiredStaticTestIds' 'Candidate manifest.candidateEvidencePins'
    if ($Manifest.candidateEvidencePins.requiredStaticTestIds.Count -ne $requiredStaticTestIds.Count) { throw 'Candidate manifest does not carry the frozen 19-ID static contract.' }
    for ($i = 0; $i -lt $requiredStaticTestIds.Count; $i++) {
        if ([string]$Manifest.candidateEvidencePins.requiredStaticTestIds[$i] -cne $requiredStaticTestIds[$i]) { throw 'Candidate manifest static test IDs differ from the frozen ordered 19-ID contract.' }
    }
    foreach ($field in @('requiredVariant','installRoot','inventoryReportPath','inventoryReportSha256','inventoryStatus')) { Assert-CcString $binding $field 'Candidate manifest.installBinding' }
    if (($binding.inventorySchemaVersion -isnot [int] -and $binding.inventorySchemaVersion -isnot [long]) -or $binding.inventorySchemaVersion -ne 2) {
        throw 'Candidate inventory schemaVersion must be the JSON integer 2.'
    }
    if ($binding.requiredVariant -cne 'OFF' -or $binding.inventoryStatus -cne 'PASS') {
        throw 'Candidate install inventory must be OFF, schema-v2, and PASS.'
    }
    foreach ($field in @('rootCommit','releaseBuildReceiptPath','releaseBuildReceiptSha256','sourceFreezeReceiptSha256','sourceTreeSha256','sourceFreezeReceiptPath','sourceRootPath')) {
        Assert-CcString $source $field 'Candidate manifest.sourceAndAuthorityPins'
    }
    Assert-CcSha $binding.inventoryReportSha256 'Candidate inventory SHA256'
    foreach ($field in @('releaseBuildReceiptSha256','sourceFreezeReceiptSha256','sourceTreeSha256')) { Assert-CcSha $source.$field "Candidate $field" }
    if ($Manifest.schema -cne 'range.pc11-fresh-run-v43-admission-candidate/v1' -or $Manifest.candidateVersion -cne 'v43') { throw 'Candidate manifest schema or version differs.' }
    if ($source.rootCommit -cnotmatch '^(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})$') { throw 'Candidate rootCommit is malformed.' }
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
function Get-CcSourcePaths($Receipt, [string]$Kind) {
    $paths = New-Object 'System.Collections.Generic.List[string]'
    if ($Kind -ceq 'save_predicate') {
        Assert-CcObjectArray $Receipt 'sourceSha256' 'Save predicate receipt'
        foreach ($row in $Receipt.sourceSha256) { Assert-CcString $row 'path' 'Save predicate source row'; Assert-CcString $row 'sha256' 'Save predicate source row'; [void]$paths.Add([string]$row.path) }
        $requiredSaveSources = @('tools/save-predicate-v43.ps1','tests/test-save-predicate-v43.ps1')
        $actualSaveSources = $paths.ToArray()
        if ($actualSaveSources.Count -ne $requiredSaveSources.Count -or
            @($actualSaveSources | Select-Object -Unique).Count -ne $requiredSaveSources.Count -or
            @($actualSaveSources | Where-Object { $_ -cnotin $requiredSaveSources }).Count -ne 0 -or
            @($requiredSaveSources | Where-Object { $_ -cnotin $actualSaveSources }).Count -ne 0) {
            throw 'Save-predicate sourceSha256 must contain exactly the two required producer paths with no duplicates.'
        }
    } elseif ($Kind -ceq 'sampler_arguments') {
        Assert-CcObject $Receipt.sourceHashes 'Sampler receipt.sourceHashes'
        foreach ($name in @('samplerArguments','samplerCli','fixture')) {
            Assert-CcString $Receipt.sourceHashes $name 'Sampler receipt.sourceHashes'
        }
        [void]$paths.Add('runtime/sampler-arguments-v43.ps1')
        [void]$paths.Add('../../../scripts/benchmark/pc11_working_set.py')
        [void]$paths.Add('tests/test-sampler-arguments-v43.ps1')
    } elseif ($Kind -ceq 'watchdog_contract') {
        Assert-CcObject $Receipt.sourceSha256 'Supervisor receipt.sourceSha256'
        foreach ($name in @('runtime/contract-common-v43.ps1','runtime/supervisor-core-v43.ps1','runtime/worker-handshake-v43.ps1','runtime/watchdog-contract-v43.ps1','runtime/native-launch-boundary-v43.ps1','tests/fixtures/worker-fixture-v43.ps1','tests/test-supervisor-worker-contract-v43.ps1','finalize-v43.ps1')) {
            Assert-CcString $Receipt.sourceSha256 $name 'Supervisor receipt.sourceSha256'
            [void]$paths.Add($name)
        }
    }
    return $paths.ToArray()
}
function Get-CcReceiptSourceSha($Receipt, [string]$Kind, [string]$ProducerPath) {
    if ($Kind -ceq 'save_predicate') {
        foreach ($item in $Receipt.sourceSha256) { if ([string]$item.path -ceq $ProducerPath) { return [string]$item.sha256 } }
        throw "Save-predicate receipt has no source hash row for $ProducerPath."
    }
    if ($Kind -ceq 'sampler_arguments') {
        switch -CaseSensitive ($ProducerPath) {
            'runtime/sampler-arguments-v43.ps1' { return [string]$Receipt.sourceHashes.samplerArguments }
            '../../../scripts/benchmark/pc11_working_set.py' { return [string]$Receipt.sourceHashes.samplerCli }
            'tests/test-sampler-arguments-v43.ps1' { return [string]$Receipt.sourceHashes.fixture }
        }
    }
    if ($Kind -ceq 'watchdog_contract') {
        $property = $Receipt.sourceSha256.PSObject.Properties[$ProducerPath]
        if ($null -eq $property -or $property.Value -isnot [string]) { throw "Supervisor receipt has no string source hash for $ProducerPath." }
        return [string]$property.Value
    }
    throw "No component source-hash mapping exists for $Kind/$ProducerPath."
}
function Test-CcRequiredStringSet($Actual, [string[]]$Required) {
    if ($Actual -isnot [Array]) { return $false }
    foreach ($item in $Actual) { if ($item -isnot [string]) { return $false } }
    if ($Actual.Count -ne $Required.Count) { return $false }
    for ($i = 0; $i -lt $Required.Count; $i++) { if ($Actual[$i] -cne $Required[$i]) { return $false } }
    return $true
}
function Test-CcExactStringSet($Actual, [string[]]$Required) {
    if ($Actual -isnot [Array] -or $Actual.Count -ne $Required.Count) { return $false }
    $actualSet = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    foreach ($item in $Actual) {
        if ($item -isnot [string] -or -not $actualSet.Add([string]$item)) { return $false }
    }
    foreach ($item in $Required) { if (-not $actualSet.Contains([string]$item)) { return $false } }
    return $true
}
function Get-CcEnvelopeSourceSha($Envelope, [string]$ProducerPath) {
    $matches = @($Envelope.sources | Where-Object { [string]$_.path -ceq $ProducerPath })
    if ($matches.Count -ne 1) { throw "Frozen-execution envelope must contain exactly one source row for $ProducerPath." }
    return [string]$matches[0].sha256
}
function Test-CcComponentCoverage($Receipt, [string]$Kind) {
    # A component receipt's successful fixture assertions are meaningful coverage,
    # but fixture success alone is never candidate qualification.
    if ($Kind -ceq 'save_predicate') {
        Assert-CcString $Receipt 'status' 'Save predicate receipt'
        Assert-CcString $Receipt 'staticContractSuiteStatus' 'Save predicate receipt'
        Assert-CcString $Receipt 'candidateGateStatus' 'Save predicate receipt'
        Assert-CcObjectArray $Receipt 'cases' 'Save predicate receipt'
        if ($Receipt.staticContractSuiteStatus -cne 'PASS' -or $Receipt.status -cne 'STATIC_INCOMPLETE' -or
            $Receipt.candidateGateStatus -cne 'PENDING' -or
            -not (Test-CcRequiredStringSet $Receipt.requiredStaticTestIdsCovered @('save_predicate')) -or
            $Receipt.caseCount -isnot [int] -or $Receipt.caseCount -ne $Receipt.cases.Count) { return $false }
        $names = New-Object 'System.Collections.Generic.List[string]'
        foreach ($case in $Receipt.cases) {
            Assert-CcString $case 'case' 'Save predicate case'
            Assert-CcString $case 'status' 'Save predicate case'
            if ($case.status -cne 'PASS') { return $false }
            [void]$names.Add([string]$case.case)
        }
        $expectedCases = @(
            [pscustomobject]@{ name='positive-complete-provenance'; decision='EVIDENCE_COMPLETE'; reason='UNIQUE_CURRENT_AUTHORITY_UIA_AND_OPERATION_CHAIN' },
            [pscustomobject]@{ name='missing-uia'; decision='EVIDENCE_INCOMPLETE'; reason='MISSING_INPUT' },
            [pscustomobject]@{ name='stale-candidate-identity'; decision='EVIDENCE_STALE'; reason='CANDIDATE_PROVENANCE_MISMATCH' },
            [pscustomobject]@{ name='numeric-zero-dirty-flag'; decision='EVIDENCE_INVALID'; reason='UIA_DIRTY_FLAG_NOT_BOOLEAN' },
            [pscustomobject]@{ name='string-false-dirty-flag'; decision='EVIDENCE_INVALID'; reason='UIA_DIRTY_FLAG_NOT_BOOLEAN' }
        )
        foreach ($expected in $expectedCases) {
            $matching = @()
            foreach ($observed in $Receipt.cases) { if ($observed.case -ceq $expected.name) { $matching += $observed } }
            if ($matching.Count -ne 1 -or $matching[0].decision -cne $expected.decision -or
                $matching[0].reasonCode -cne $expected.reason) { return $false }
        }
        return $true
    }
    if ($Kind -ceq 'sampler_arguments') {
        Assert-CcString $Receipt 'status' 'Sampler receipt'
        Assert-CcString $Receipt 'componentSuiteStatus' 'Sampler receipt'
        Assert-CcString $Receipt 'candidateGateStatus' 'Sampler receipt'
        Assert-CcString $Receipt 'candidateIdentitySource' 'Sampler receipt'
        Assert-CcString $Receipt 'candidateManifestSha256' 'Sampler receipt'
        Assert-CcString $Receipt 'candidateManifestIdentitySha256' 'Sampler receipt'
        Assert-CcString $Receipt 'program' 'Sampler receipt'
        Assert-CcStringArray $Receipt 'arguments' 'Sampler receipt'
        Assert-CcObjectArray $Receipt 'cases' 'Sampler receipt'
        if ($Receipt.status -cne 'PENDING' -or $Receipt.componentSuiteStatus -cne 'PASS' -or
            $Receipt.candidateGateStatus -cne 'PENDING' -or $Receipt.candidateIdentitySource -cne 'synthetic-fixture-only' -or
            $Receipt.caseCount -isnot [int] -or $Receipt.caseCount -ne 13 -or $Receipt.cases.Count -ne 13 -or
            $Receipt.parsedArgumentCount -isnot [int] -or $Receipt.parsedArgumentCount -ne 19 -or
            $Receipt.parsedArgumentCount -ne $Receipt.arguments.Count) { return $false }
        $expectedCases = @{
            'valid-plan-exact-argument-vector' = 'PINNED_PROGRAM_AND_ARGUMENTS_MATCH'
            'unexpected-argument-token' = 'ARGUMENT_VECTOR_MISMATCH'
            'missing-manifest-pin' = 'MANIFEST_PIN_REQUIRED'
            'malformed-manifest-pin' = 'MANIFEST_PIN_INVALID'
            'candidate-identity-mismatch' = 'CANDIDATE_IDENTITY_MISMATCH'
            'go-permitted-must-be-boolean-false' = 'GO_FIELD_TYPE_OR_VALUE_INVALID'
            'product-action-must-be-boolean-false' = 'PRODUCT_ACTION_TYPE_OR_VALUE_INVALID'
            'drive-relative-program-path' = 'PROGRAM_PATH_NOT_PINNED'
            'nonexistent-sampler-script' = 'SAMPLER_SCRIPT_MISSING'
            'different-existing-sampler-script' = 'SAMPLER_SCRIPT_MISMATCH'
            'builder-has-no-process-launch' = 'NO_PRODUCT_PROCESS_LAUNCH'
            'source-hashes-are-pinned-sha256' = 'SOURCE_HASHES_ARE_SHA256'
            'go-and-product-actions-remain-disabled' = 'NO_GO_NO_PRODUCT_ACTION'
        }
        $observedCases = @{}
        foreach ($case in $Receipt.cases) {
            foreach ($field in @('case','kind','expected','observed','reasonCode','status')) { Assert-CcString $case $field 'Sampler case' }
            if (-not $expectedCases.ContainsKey([string]$case.case) -or $observedCases.ContainsKey([string]$case.case) -or
                $case.status -cne 'PASS' -or $case.expected -cne $case.observed -or
                $case.reasonCode -cne $expectedCases[[string]$case.case]) { return $false }
            $observedCases[[string]$case.case] = $true
        }
        if ($observedCases.Count -ne $expectedCases.Count) { return $false }
        $arguments = $Receipt.arguments
        $samplerCliPath = Get-CcSourcePathInRepo '../../../scripts/benchmark/pc11_working_set.py'
        if ([IO.Path]::GetFullPath([string]$arguments[0]) -cne $samplerCliPath -or
            $arguments[1] -cne '--root-pid' -or $arguments[2] -cne '4312' -or
            $arguments[3] -cne '--expected-root-image' -or
            $arguments[5] -cne '--allowed-root' -or $arguments[7] -cne '--allowed-root' -or
            $arguments[9] -cne '--output' -or $arguments[11] -cne '--stop-file' -or
            $arguments[13] -cne '--interval-ms' -or $arguments[14] -cne '50' -or
            $arguments[15] -cne '--min-samples' -or $arguments[16] -cne '10' -or
            $arguments[17] -cne '--min-seconds' -or $arguments[18] -cne '0.5') { return $false }
        if (-not [IO.Path]::IsPathRooted([string]$Receipt.program)) { return $false }
        $fixtureRoot = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath([string]$Receipt.program))
        if ([IO.Path]::GetFileName($fixtureRoot) -cnotmatch '^v43 sampler paths with spaces [0-9a-f]{32}$' -or
            [IO.Path]::GetFullPath([string]$Receipt.program) -cne [IO.Path]::Combine($fixtureRoot, 'pinned python.exe')) { return $false }
        $dynamicSlots = @(
            [pscustomobject]@{ index=4; name='Range FEA.exe' },
            [pscustomobject]@{ index=6; name='allowed images A' },
            [pscustomobject]@{ index=8; name='allowed images B' },
            [pscustomobject]@{ index=10; name='run output' },
            [pscustomobject]@{ index=12; name='stop.flag' }
        )
        foreach ($slot in $dynamicSlots) {
            if (-not [IO.Path]::IsPathRooted([string]$arguments[$slot.index]) -or
                [IO.Path]::GetFullPath([string]$arguments[$slot.index]) -cne [IO.Path]::Combine($fixtureRoot, [string]$slot.name)) { return $false }
        }
        return $true
    }
    if ($Kind -ceq 'watchdog_contract') {
        Assert-CcString $Receipt 'status' 'Supervisor receipt'
        Assert-CcString $Receipt 'staticContractSuiteStatus' 'Supervisor receipt'
        Assert-CcString $Receipt 'fixtureContractStatus' 'Supervisor receipt'
        Assert-CcObjectArray $Receipt 'results' 'Supervisor receipt'
        Assert-CcStringArray $Receipt 'requiredStaticTestIdsCovered' 'Supervisor receipt'
        if ($Receipt.status -cne 'STATIC_INCOMPLETE' -or $Receipt.staticContractSuiteStatus -cne 'PASS' -or
            $Receipt.fixtureContractStatus -cne 'PASS' -or $Receipt.candidateGateStatus -cne 'PENDING' -or
            -not ($Receipt.requiredStaticTestIdsCovered -contains 'watchdog_contract')) { return $false }
        $watchdogRows = @()
        foreach ($result in $Receipt.results) {
            Assert-CcString $result 'testId' 'Supervisor result'
            Assert-CcString $result 'status' 'Supervisor result'
            if ($result.testId -ceq 'watchdog_contract') { $watchdogRows += $result }
        }
        if ($watchdogRows.Count -ne 1 -or $watchdogRows[0].status -cne 'PASS' -or
            $watchdogRows[0].boundSupervisorSuccessAccepted -ne $true -or
            $watchdogRows[0].supervisorFailureRejected -ne $true -or
            $watchdogRows[0].fixtureStatus -cne 'PASS' -or $watchdogRows[0].candidateGateStatus -cne 'PENDING' -or
            $watchdogRows[0].fixtureCandidateManifestIdentitySha256 -cne $Receipt.fixtureCandidateManifestIdentitySha256) { return $false }
        return $true
    }
    return $false
}
function Get-CcSourcePathInRepo([string]$RelativeProducerPath) {
    $relative = $RelativeProducerPath.Replace('/', '\')
    if ($relative.StartsWith('..\..\..\scripts\', [StringComparison]::OrdinalIgnoreCase)) {
        $repoRelative = $relative.Substring(9)
    } else {
        $repoRelative = 'scripts\packaging\piezo_gui_static_v43\' + $relative
    }
    $root = if ([string]::IsNullOrWhiteSpace([string]$sourceRootForCandidate)) { $repoRoot } else { $sourceRootForCandidate }
    return [IO.Path]::GetFullPath((Join-Path $root $repoRelative))
}

$missing = New-Object 'System.Collections.Generic.List[string]'
$rows = New-Object 'System.Collections.Generic.List[object]'
$manifestPin = $null
$identity = $null
$sourceTree = $null
$sourceRootForCandidate = $null
$freezePin = $null
$producerPathRows = New-Object 'System.Collections.Generic.List[object]'
$bindings = @(
    [pscustomobject]@{ id='save_predicate'; path=$SavePredicateReceiptPath; sha=$SavePredicateReceiptSha256; schema='range.pc11-v43-save-predicate-fixture-receipt/v1'; fixtureOnly=$true },
    [pscustomobject]@{ id='sampler_arguments'; path=$SamplerArgumentsReceiptPath; sha=$SamplerArgumentsReceiptSha256; schema='range.pc11-v43-sampler-arguments-fixture/v1'; fixtureOnly=$true },
    [pscustomobject]@{ id='watchdog_contract'; path=$SupervisorWorkerReceiptPath; sha=$SupervisorWorkerReceiptSha256; schema='range.pc11-v43-supervisor-worker-contract-static-receipt/v1'; fixtureOnly=$true }
)

if ([string]::IsNullOrWhiteSpace($CandidateManifestPath) -or [string]::IsNullOrWhiteSpace($CandidateManifestSha256)) {
    [void]$missing.Add('Externally pinned candidate manifest path and SHA256 were not supplied.')
} else {
    $candidate = Read-CcPinnedJson $CandidateManifestPath $CandidateManifestSha256 'Candidate manifest'
    $manifestPin = [pscustomobject]@{ path=$candidate.path; sha256=$candidate.sha256 }
    $templatePath = [IO.Path]::GetFullPath((Join-Path $prepRoot 'candidate-manifest-v43.admission-template.json'))
    if ($candidate.path -ceq $templatePath) { [void]$missing.Add('The externally pinned file is candidate-manifest-v43.admission-template.json, a preparation template rather than a populated OFF candidate manifest.') }
    if ($candidate.value -is [Management.Automation.PSCustomObject]) {
        Assert-CcString $candidate.value 'candidateState' 'Candidate manifest'
        if ($candidate.value.candidateState -match 'PREPARATION|PENDING') {
            [void]$missing.Add('Externally pinned candidate manifest is still in a preparation/pending state.')
        }
        $placeholder = Find-CcPendingPlaceholder $candidate.value
        if ($null -ne $placeholder) {
            [void]$missing.Add("Externally pinned candidate manifest is incomplete: $($placeholder[0])=$($placeholder[1]).")
        }
        $pinChecks = @(
            [pscustomobject]@{ name='installBinding.installRoot'; value=$candidate.value.installBinding.installRoot },
            [pscustomobject]@{ name='installBinding.inventoryReportSha256'; value=$candidate.value.installBinding.inventoryReportSha256 },
            [pscustomobject]@{ name='sourceAndAuthorityPins.sourceRootPath'; value=$candidate.value.sourceAndAuthorityPins.sourceRootPath },
            [pscustomobject]@{ name='sourceAndAuthorityPins.sourceFreezeReceiptPath'; value=$candidate.value.sourceAndAuthorityPins.sourceFreezeReceiptPath },
            [pscustomobject]@{ name='sourceAndAuthorityPins.sourceFreezeReceiptSha256'; value=$candidate.value.sourceAndAuthorityPins.sourceFreezeReceiptSha256 },
            [pscustomobject]@{ name='sourceAndAuthorityPins.sourceTreeSha256'; value=$candidate.value.sourceAndAuthorityPins.sourceTreeSha256 },
            [pscustomobject]@{ name='sourceAndAuthorityPins.rootCommit'; value=$candidate.value.sourceAndAuthorityPins.rootCommit },
            [pscustomobject]@{ name='sourceAndAuthorityPins.releaseBuildReceiptSha256'; value=$candidate.value.sourceAndAuthorityPins.releaseBuildReceiptSha256 }
        )
        foreach ($check in $pinChecks) {
            if ($check.value -isnot [string] -or [string]$check.value -match '^PENDING_') {
                [void]$missing.Add("Candidate prerequisite $($check.name) is absent or still a PENDING placeholder.")
            }
        }
    }
    try {
        $identity = Get-CcManifestIdentity $candidate.value
        $sourceTree = [string]$candidate.value.sourceAndAuthorityPins.sourceTreeSha256
        $source = $candidate.value.sourceAndAuthorityPins
        $sourceRoot = [IO.Path]::GetFullPath([string]$source.sourceRootPath)
        if (-not (Test-CcSamePath $sourceRoot $repoRoot)) { throw 'Candidate sourceRootPath must resolve to the current repository root.' }
        $sourceRootForCandidate = $sourceRoot

        $inventoryPin = Read-CcPinnedJson ([string]$candidate.value.installBinding.inventoryReportPath) ([string]$candidate.value.installBinding.inventoryReportSha256) 'Candidate OFF inventory report'
        $inventory = $inventoryPin.value
        Assert-CcString $inventory 'inventoryStatus' 'Candidate OFF inventory report'
        Assert-CcString $inventory 'variant' 'Candidate OFF inventory report'
        Assert-CcString $inventory 'installRoot' 'Candidate OFF inventory report'
        if (($inventory.schemaVersion -isnot [int] -and $inventory.schemaVersion -isnot [long]) -or $inventory.schemaVersion -ne 2 -or
            $inventory.inventoryStatus -cne 'PASS' -or $inventory.variant -cne 'OFF' -or
            -not (Test-CcSamePath ([string]$inventory.installRoot) ([string]$candidate.value.installBinding.installRoot))) {
            throw 'Pinned candidate inventory must be schema-v2, OFF, PASS, and bound to the manifest installRoot.'
        }

        $freezePath = [IO.Path]::GetFullPath([string]$candidate.value.sourceAndAuthorityPins.sourceFreezeReceiptPath)
        $freezeSha = [string]$source.sourceFreezeReceiptSha256
        $freezePin = Read-CcPinnedJson $freezePath $freezeSha 'Source freeze receipt'
        $freeze = $freezePin.value
        foreach ($field in @('schema','status','candidateVersion','variant','configuration','sourceRootPath','rootCommit','sourceTreeSha256','releaseBuildReceiptPath','releaseBuildReceiptSha256')) {
            Assert-CcString $freeze $field 'Source freeze receipt'
        }
        Assert-CcBoolean $freeze 'staticOnly' 'Source freeze receipt'
        Assert-CcBoolean $freeze 'goPermitted' 'Source freeze receipt'
        Assert-CcObject $freeze.productActions 'Source freeze receipt.productActions'
        Assert-CcNoProductActions $freeze.productActions 'Source freeze receipt.productActions'
        if ($freeze.schema -cne 'range.pc11-v43-source-freeze/v1' -or $freeze.status -cne 'PASS' -or
            $freeze.candidateVersion -cne 'v43' -or $freeze.variant -cne 'OFF' -or $freeze.configuration -cne 'Release' -or
            $freeze.staticOnly -ne $true -or $freeze.goPermitted -ne $false -or
            -not (Test-CcSamePath ([string]$freeze.sourceRootPath) $repoRoot)) {
            throw 'Source freeze receipt must be the current repository v43/OFF/Release static-only no-GO receipt.'
        }
        if (-not (Test-CcSamePath ([string]$source.sourceFreezeReceiptPath) $freezePin.path) -or
            -not (Test-CcSamePath ([string]$source.sourceRootPath) ([string]$freeze.sourceRootPath)) -or
            [string]$source.rootCommit -cne [string]$freeze.rootCommit -or
            [string]$source.sourceTreeSha256 -cne [string]$freeze.sourceTreeSha256 -or
            -not (Test-CcSamePath ([string]$source.releaseBuildReceiptPath) ([string]$freeze.releaseBuildReceiptPath)) -or
            [string]$source.releaseBuildReceiptSha256 -cne [string]$freeze.releaseBuildReceiptSha256) {
            throw 'Candidate source identity fields differ from the pinned source freeze receipt.'
        }
        Assert-CcObjectArray $freeze 'sourceFiles' 'Source freeze receipt'
    } catch {
        [void]$missing.Add(('Candidate/source freeze binding unavailable: ' + $_.Exception.Message))
    }
}

foreach ($binding in $bindings) {
    $row = [ordered]@{ testId=$binding.id; status='PENDING'; fixtureReceiptPath=$null; fixtureReceiptSha256=$null; executionEnvelopePath=$null; executionEnvelopeSha256=$null; sourceBindingMethod='UNBOUND_DIRECT_RECEIPT'; producerSourcePaths=@(); sourcePathBinding='PENDING'; semanticCoverage='PENDING'; candidateIdentityBinding='PENDING'; reason=$null }
    if ([string]::IsNullOrWhiteSpace([string]$binding.path) -or [string]::IsNullOrWhiteSpace([string]$binding.sha)) {
        $row.reason = 'Actual component test receipt path and external SHA256 were not supplied.'
        [void]$missing.Add("$($binding.id): actual component test receipt and external SHA256 are missing.")
        [void]$rows.Add([pscustomobject]$row)
        continue
    }
    try {
        $inputPin = Read-CcPinnedJson ([string]$binding.path) ([string]$binding.sha) "$($binding.id) component input"
        $envelope = $null
        $componentPin = $inputPin
        $envelopeIdentityBound = $false
        if ($inputPin.value.schema -ceq 'range.pc11-v43-frozen-fixture-execution/v1') {
            $envelope = $inputPin.value
            Assert-CcString $envelope 'componentId' "$($binding.id) frozen-execution envelope"
            Assert-CcString $envelope 'sourceBindingMethod' "$($binding.id) frozen-execution envelope"
            Assert-CcString $envelope 'candidateManifestIdentitySha256' "$($binding.id) frozen-execution envelope"
            Assert-CcString $envelope 'sourceTreeSha256' "$($binding.id) frozen-execution envelope"
            Assert-CcBoolean $envelope 'fixtureOnly' "$($binding.id) frozen-execution envelope"
            Assert-CcBoolean $envelope 'staticOnly' "$($binding.id) frozen-execution envelope"
            Assert-CcBoolean $envelope 'goPermitted' "$($binding.id) frozen-execution envelope"
            Assert-CcObject $envelope.productActions "$($binding.id) frozen-execution envelope.productActions"
            Assert-CcNoProductActions $envelope.productActions "$($binding.id) frozen-execution envelope.productActions"
            Assert-CcObject $envelope.originalReceipt "$($binding.id) frozen-execution envelope.originalReceipt"
            Assert-CcString $envelope.originalReceipt 'path' "$($binding.id) frozen-execution originalReceipt"
            Assert-CcString $envelope.originalReceipt 'sha256' "$($binding.id) frozen-execution originalReceipt"
            Assert-CcSha $envelope.originalReceipt.sha256 "$($binding.id) original receipt SHA256"
            Assert-CcObjectArray $envelope 'sources' "$($binding.id) frozen-execution envelope"
            foreach ($sourceRow in $envelope.sources) {
                Assert-CcString $sourceRow 'path' "$($binding.id) frozen-execution source row"
                Assert-CcString $sourceRow 'sha256' "$($binding.id) frozen-execution source row"
                Assert-CcSha $sourceRow.sha256 "$($binding.id) frozen-execution source hash"
            }
            if ($envelope.componentId -cne $binding.id -or $envelope.sourceBindingMethod -cne 'FROZEN_SOURCE_EXECUTION' -or
                $envelope.fixtureOnly -ne $true -or $envelope.staticOnly -ne $true -or $envelope.goPermitted -ne $false) {
                throw 'Frozen-execution envelope component, fixture boundary, source-binding method, or no-GO fields are not exact.'
            }
            $envelopeIdentityBound = ($null -ne $identity -and $envelope.candidateManifestIdentitySha256 -cmatch '^[0-9a-fA-F]{64}$' -and
                $envelope.candidateManifestIdentitySha256.ToLowerInvariant() -ceq $identity -and
                $envelope.sourceTreeSha256 -cmatch '^[0-9a-fA-F]{64}$' -and
                $envelope.sourceTreeSha256.ToLowerInvariant() -ceq ([string]$sourceTree).ToLowerInvariant())
            $componentPin = Read-CcPinnedJson ([string]$envelope.originalReceipt.path) ([string]$envelope.originalReceipt.sha256) "$($binding.id) original component receipt"
            $row.executionEnvelopePath = $inputPin.path
            $row.executionEnvelopeSha256 = $inputPin.sha256
            $row.sourceBindingMethod = [string]$envelope.sourceBindingMethod
        }
        $pin = $componentPin
        $receipt = $componentPin.value
        $row.fixtureReceiptPath = $pin.path
        $row.fixtureReceiptSha256 = $pin.sha256
        Assert-CcString $receipt 'schema' "$($binding.id) component receipt"
        Assert-CcString $receipt 'candidateVersion' "$($binding.id) component receipt"
        Assert-CcBoolean $receipt 'staticOnly' "$($binding.id) component receipt"
        Assert-CcBoolean $receipt 'fixtureOnly' "$($binding.id) component receipt"
        Assert-CcObject $receipt.productActions "$($binding.id) productActions"
        foreach ($action in @('guiLaunched','savePerformed','runPerformed','confirmPerformed')) { Assert-CcBoolean $receipt.productActions $action "$($binding.id) productActions" }
        Assert-CcBoolean $receipt 'goPermitted' "$($binding.id) component receipt"
        if ($receipt.schema -cne $binding.schema -or $receipt.candidateVersion -cne 'v43' -or $receipt.staticOnly -ne $true -or
            $receipt.fixtureOnly -ne $true -or $receipt.goPermitted -ne $false -or
            @('guiLaunched','savePerformed','runPerformed','confirmPerformed' | Where-Object { $receipt.productActions.$_ -ne $false }).Count -gt 0) {
            throw 'Component receipt schema, static fixture boundary, or no-product-action fields are not exact.'
        }
        $coveragePassed = Test-CcComponentCoverage $receipt $binding.id
        $row.semanticCoverage = if ($coveragePassed) { 'FIXTURE_CASES_VALIDATED' } else { 'PENDING_OR_INCOMPLETE_FIXTURE_COVERAGE' }
        $identityBound = ($null -ne $envelope -and $envelopeIdentityBound)
        $row.candidateIdentityBinding = if ($identityBound) { 'EXTERNAL_MANIFEST_IDENTITY_AND_SOURCE_TREE_MATCH' } else { 'PENDING_FROZEN_SOURCE_EXECUTION_ENVELOPE_REQUIRED_OR_MISMATCHED' }
        $sourcePaths = @(Get-CcSourcePaths $receipt $binding.id)
        if ($null -ne $envelope) {
            $envelopePaths = @($envelope.sources | ForEach-Object { [string]$_.path })
            if (-not (Test-CcExactStringSet $envelopePaths $sourcePaths)) {
                throw 'Frozen-execution envelope sources do not exactly match the component receipt source path set.'
            }
        }
        foreach ($producerPath in $sourcePaths) {
            $absolute = Get-CcSourcePathInRepo $producerPath
            $producerSha = Get-CcLockedSourceSha256 $absolute
            $receiptSha = Get-CcReceiptSourceSha $receipt $binding.id $producerPath
            Assert-CcSha $receiptSha "$($binding.id) producer source pin $producerPath"
            if ($receiptSha -cne $producerSha) { throw "Component receipt producer source hash differs from current source: $producerPath" }
            if ($null -ne $envelope) {
                $envelopeSha = Get-CcEnvelopeSourceSha $envelope $producerPath
                if ($envelopeSha -cne $producerSha) { throw "Frozen-execution envelope source hash differs from current source: $producerPath" }
            }
            if ($null -ne $freezePin) {
                $repoRelative = $absolute.Substring($sourceRootForCandidate.TrimEnd('\').Length + 1).Replace('\', '/')
                $freezeRow = @()
                foreach ($sourceRow in $freezePin.value.sourceFiles) {
                    Assert-CcString $sourceRow 'path' 'Source freeze row'
                    Assert-CcString $sourceRow 'sha256' 'Source freeze row'
                    if ($sourceRow.path -ceq $repoRelative) { $freezeRow += $sourceRow }
                }
                if ($freezeRow.Count -ne 1 -or [string]$freezeRow[0].sha256 -cne $producerSha) {
                    throw "Producer source is absent or differs in the pinned source freeze: $repoRelative"
                }
            }
            [void]$producerPathRows.Add([pscustomobject]@{ testId=$binding.id; producerPath=$producerPath; absolutePath=$absolute; sha256=$producerSha })
        }
        $row.producerSourcePaths = $sourcePaths
        $row.sourcePathBinding = if ($null -ne $freezePin) { 'RECEIPT_AND_FROZEN_SOURCE_HASHES_MATCH' } else { 'HASHED_CURRENT_SOURCE; FREEZE_MEMBERSHIP_NOT_PROVEN' }
        if (-not $coveragePassed) {
            $row.reason = 'Component fixture receipt lacks complete, validated positive/negative coverage metadata.'
            [void]$missing.Add("$($binding.id): component fixture coverage is incomplete or not asserted by the receipt.")
        } elseif (-not $identityBound) {
            $row.reason = 'Fixture assertions and frozen source bytes are not bound to the externally pinned candidate manifest identity and source tree.'
            [void]$missing.Add("$($binding.id): component fixture receipt lacks matching external candidate identity/source-tree binding.")
        } else {
            $row.reason = 'Fixture coverage, source pins, and candidate identity are bound; product qualification remains fixture-only.'
            # This PASS is limited to the static candidate-control contract row.
            $row.status = 'PASS'
        }
    } catch {
        $row.reason = $_.Exception.Message
        [void]$missing.Add("$($binding.id): $($_.Exception.Message)")
    }
    [void]$rows.Add([pscustomobject]$row)
}

# Bind this gate's own bytes into the same candidate freeze and lock them until
# its receipt is written. Without this row, the evaluator itself could be swapped.
$gateProducerPath = 'scripts/packaging/piezo_gui_static_v43/tests/test-candidate-control-contract-v43.ps1'
$gateProducerSha = Get-CcLockedSourceSha256 $MyInvocation.MyCommand.Path
$gateProducerFreezeBinding = 'PENDING'
if ($null -ne $freezePin) {
    $gateFreezeRows = @()
    foreach ($sourceRow in $freezePin.value.sourceFiles) {
        if ($sourceRow.path -ceq $gateProducerPath) { $gateFreezeRows += $sourceRow }
    }
    if ($gateFreezeRows.Count -ne 1 -or [string]$gateFreezeRows[0].sha256 -cne $gateProducerSha) {
        [void]$missing.Add('Candidate source freeze does not pin the current candidate-control evaluator bytes.')
    } else {
        $gateProducerFreezeBinding = 'MATCH'
    }
} else {
    [void]$missing.Add('Candidate-control evaluator bytes are not bound to a verified source freeze.')
}

# Local validator negative controls: numeric/string booleans and wrong JSON container shapes must fail closed.
$negativeCases = New-Object 'System.Collections.Generic.List[object]'
foreach ($case in @(
    [pscustomobject]@{ name='numeric-go'; value=[pscustomobject]@{ goPermitted=0 }; validator='boolean' },
    [pscustomobject]@{ name='string-go'; value=[pscustomobject]@{ goPermitted='False' }; validator='boolean' },
    [pscustomobject]@{ name='array-object'; value=@([pscustomobject]@{ x=1 }); validator='object' },
    [pscustomobject]@{ name='object-array'; value=[pscustomobject]@{ rows=[pscustomobject]@{ x=1 } }; validator='array' }
)) {
    $rejected = $false
    try {
        if ($case.validator -ceq 'boolean') { Assert-CcBoolean $case.value 'goPermitted' $case.name }
        elseif ($case.validator -ceq 'object') { Assert-CcObject $case.value $case.name }
        else { Assert-CcObjectArray $case.value 'rows' $case.name }
    } catch { $rejected = $true }
    if (-not $rejected) { throw "Candidate control gate accepted malformed type in negative case $($case.name)." }
    [void]$negativeCases.Add([pscustomobject]@{ case=$case.name; status='PASS_REJECTED' })
}

$gateStatus = if ($missing.Count -eq 0 -and $null -ne $identity -and $null -ne $sourceTree) { 'PASS' } else { 'PENDING' }
$receiptOut = [ordered]@{
    schema = 'range.pc11-v43-candidate-control-contract-receipt/v1'
    status = if ($gateStatus -eq 'PASS') { 'STATIC_INCOMPLETE' } else { 'PENDING' }
    candidateGateStatus = $gateStatus
    candidateVersion = 'v43'
    variant = 'OFF'
    staticOnly = $true
    fixtureOnly = $true
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    externalCandidateManifest = $manifestPin
    candidateManifestIdentitySha256 = $identity
    sourceTreeSha256 = $sourceTree
    sourceFreezeReceipt = if ($null -eq $freezePin) { $null } else { [pscustomobject]@{ path=$freezePin.path; sha256=$freezePin.sha256 } }
    requiredStaticTestIds = $requiredStaticTestIds
    requiredContractIds = @('save_predicate','sampler_arguments','watchdog_contract')
    results = $rows.ToArray()
    producerSourcePaths = $producerPathRows.ToArray()
    gateProducerSource = [pscustomobject]@{ path=$gateProducerPath; sha256=$gateProducerSha; sourceFreezeBinding=$gateProducerFreezeBinding }
    negativeTypeCases = $negativeCases.ToArray()
    missingPrerequisites = $missing.ToArray()
    productReadiness = 'PENDING_EXTERNAL_OFF_INSTALL_SOURCE_FREEZE_AND_POST_GO_ACCEPTANCE'
    productActions = [ordered]@{ guiLaunched=$false; savePerformed=$false; runPerformed=$false; confirmPerformed=$false }
    goPermitted = $false
    interpretation = 'Fixture contract results remain fixture-only. No semantic product qualification, GUI action, Save, Run, Confirm, or GO is inferred.'
}

$parent = Split-Path -Parent $OutputPath
if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
$outBytes = (New-Object Text.UTF8Encoding($false, $true)).GetBytes(($receiptOut | ConvertTo-Json -Depth 20) + "`n")
$outStream = [IO.File]::Open($OutputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $outStream.Write($outBytes, 0, $outBytes.Length) }
finally {
    $outStream.Dispose()
    foreach ($lockedSource in $sourceLocks) { $lockedSource.Dispose() }
}
Write-Output ("{0} candidateGate={1} rows={2} negativeCases={3} productActions=NONE output={4}" -f $receiptOut.status, $receiptOut.candidateGateStatus, $rows.Count, $negativeCases.Count, $OutputPath)
