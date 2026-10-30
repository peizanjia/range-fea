[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CandidateManifestPath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$AuditRoot,
    [string]$SavePredicateReceiptPath,
    [string]$SavePredicateReceiptSha256,
    [string]$SamplerArgumentsReceiptPath,
    [string]$SamplerArgumentsReceiptSha256,
    [string]$SupervisorWorkerReceiptPath,
    [string]$SupervisorWorkerReceiptSha256,
    [switch]$VerifyProducerPathsOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'This v43 static-chain test requires Windows PowerShell 5.1.'
}
$script:PrepRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$script:SelfPath = [IO.Path]::GetFullPath($MyInvocation.MyCommand.Path)
$script:ProducerScripts = [ordered]@{
    sourcePreflight = Join-Path $script:PrepRoot 'source-preflight-v43.ps1'
    sourceFreeze = Join-Path $script:PrepRoot 'freeze-v43.ps1'
    packageTree = Join-Path $script:PrepRoot 'package-tree-v43.ps1'
    finalize = Join-Path $script:PrepRoot 'finalize-v43.ps1'
}
foreach ($producerPath in $script:ProducerScripts.Values) {
    if (-not [IO.File]::Exists([string]$producerPath)) { throw "Harness producer path does not resolve under prep root: $producerPath" }
}
if ($VerifyProducerPathsOnly) {
    Write-Output ("PASS_HARNESS_PATHS producerCount={0} prepRoot={1}" -f $script:ProducerScripts.Count, $script:PrepRoot)
    exit 0
}
$CandidateManifestPath = [IO.Path]::GetFullPath($CandidateManifestPath)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if (-not $AuditRoot) { $AuditRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..\build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit')) }
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)

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
function Get-Sha256Bytes([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Read-JsonSnapshot([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { throw "Required evidence file is missing: $Path" }
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
    if ($ExpectedSha256 -notmatch '^[0-9a-fA-F]{64}$' -or $snapshot.sha256 -cne $ExpectedSha256.ToLowerInvariant()) {
        throw "$Label bytes do not match the pinned SHA256."
    }
    return $snapshot.value
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
function Assert-Sha256String([string]$Value, [string]$Label) {
    if ($Value -notmatch '^[0-9a-fA-F]{64}$') { throw "$Label must be a 64-character SHA256 string." }
}
function Assert-CommitString([string]$Value, [string]$Label) {
    if ($Value -notmatch '^(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})$') { throw "$Label must be a 40- or 64-character commit string." }
}
function Assert-JsonObjectProperty($Object, [string]$Name, [string]$Label) {
    Assert-JsonProperty $Object $Name $Label
    Assert-JsonObjectValue $Object.$Name "$Label.$Name"
}
function Assert-ProductActions($Receipt, [string]$Label) {
    Assert-JsonObjectProperty $Receipt 'productActions' $Label
    foreach ($actionName in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        Assert-JsonBoolean $Receipt.productActions $actionName "$Label.productActions"
        if ($Receipt.productActions.$actionName -ne $false) { throw "$Label permits product action $actionName." }
    }
}
function Assert-Hash([string]$Path, [string]$Expected, [string]$Label) {
    if (-not [IO.File]::Exists($Path) -or $Expected -notmatch '^[0-9a-fA-F]{64}$' -or (Get-Sha256File $Path) -cne $Expected.ToLowerInvariant()) {
        throw "$Label bytes do not match the pinned SHA256."
    }
}
function ConvertTo-JsonString([string]$Value) { return [string]($Value | ConvertTo-Json -Compress -ErrorAction Stop) }
function Get-ManifestIdentitySha256($Manifest) {
    Assert-JsonObjectProperty $Manifest 'installBinding' 'Candidate manifest'
    Assert-JsonObjectProperty $Manifest 'sourceAndAuthorityPins' 'Candidate manifest'
    $binding = $Manifest.installBinding
    $source = $Manifest.sourceAndAuthorityPins
    foreach ($field in @('installRoot', 'inventoryReportSha256')) { Assert-JsonString $binding $field 'Candidate manifest.installBinding' }
    foreach ($field in @('rootCommit', 'releaseBuildReceiptSha256', 'sourceFreezeReceiptSha256', 'sourceTreeSha256')) {
        Assert-JsonString $source $field 'Candidate manifest.sourceAndAuthorityPins'
    }
    $rootCommit = $source.rootCommit.ToLowerInvariant()
    Assert-CommitString $rootCommit 'Candidate manifest.sourceAndAuthorityPins.rootCommit'
    foreach ($field in @('releaseBuildReceiptSha256', 'sourceFreezeReceiptSha256', 'sourceTreeSha256')) {
        Assert-Sha256String $source.$field "Candidate manifest.sourceAndAuthorityPins.$field"
    }
    Assert-Sha256String $binding.inventoryReportSha256 'Candidate manifest.installBinding.inventoryReportSha256'
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
    Assert-JsonObjectValue $Receipt $Label
    foreach ($field in @('candidateVersion', 'variant')) { Assert-JsonString $Receipt $field $Label }
    Assert-JsonBoolean $Receipt 'staticOnly' $Label
    Assert-JsonBoolean $Receipt 'goPermitted' $Label
    Assert-ProductActions $Receipt $Label
    if ($Receipt.candidateVersion -cne 'v43' -or $Receipt.variant -cne 'OFF' -or $Receipt.staticOnly -ne $true -or $Receipt.goPermitted -ne $false) {
        throw "$Label does not preserve v43 OFF static-only no-GO identity."
    }
}
function Get-ParseErrors([string]$Path) {
    $tokens = $null
    $parseErrors = $null
    [void][Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$parseErrors)
    return @($parseErrors)
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
function Invoke-ChildPowerShell([string[]]$ScriptArguments) {
    $powerShellPath = [IO.Path]::GetFullPath((Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'))
    $allArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass') + $ScriptArguments
    $quoted = New-Object 'System.Collections.Generic.List[string]'
    foreach ($argument in $allArguments) { [void]$quoted.Add((Quote-WindowsArgument ([string]$argument))) }
    $processInfo = New-Object Diagnostics.ProcessStartInfo
    $processInfo.FileName = $powerShellPath
    $processInfo.Arguments = [string]::Join(' ', $quoted.ToArray())
    $processInfo.WorkingDirectory = [IO.Path]::GetDirectoryName($CandidateManifestPath)
    $processInfo.UseShellExecute = $false
    $processInfo.CreateNoWindow = $true
    $processInfo.RedirectStandardOutput = $true
    $processInfo.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $processInfo
    try {
        if (-not $process.Start()) { throw 'Could not start a static-chain negative-test child.' }
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $stdout = $process.StandardOutput.ReadToEnd()
        $process.WaitForExit()
        return [pscustomobject]@{ exitCode = [int]$process.ExitCode; stdout = $stdout; stderr = $stderrTask.Result }
    }
    finally { $process.Dispose() }
}
function Assert-FailClosedChild([string]$Label, [string]$ScriptPath, [string[]]$Arguments, [string]$ExpectedStatus) {
    $child = Invoke-ChildPowerShell (@('-File', $ScriptPath) + $Arguments)
    if ($child.exitCode -eq 0) { throw "$Label accepted a deliberately malformed payload or pin." }
    $outputPathIndex = [Array]::IndexOf($Arguments, '-OutputPath')
    if ($outputPathIndex -lt 0 -or $outputPathIndex + 1 -ge $Arguments.Length) { throw "$Label test has no output receipt path." }
    $receiptPath = [IO.Path]::GetFullPath($Arguments[$outputPathIndex + 1])
    $receipt = Read-Json $receiptPath
    if ($receipt.status -cne $ExpectedStatus -or $receipt.goPermitted -ne $false) { throw "$Label did not write the expected fail-closed no-GO receipt." }
    foreach ($actionName in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        if ($receipt.productActions.$actionName -ne $false) { throw "$Label failure receipt permits product action $actionName." }
    }
    return [pscustomobject]@{ id = $Label; status = 'PASS'; expectedRejectedStatus = $ExpectedStatus; childExitCode = $child.exitCode; receiptPath = $receiptPath; receiptSha256 = Get-Sha256File $receiptPath }
}
function Write-JsonFixture([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path, (($Value | ConvertTo-Json -Depth 20) + "`n"), (New-Object Text.UTF8Encoding($false)))
}
function Assert-MalformedFreezePayloadRejected([string]$Label, [string]$ReceiptKind, $Payload, [string]$OutputPath, [string]$PinnedSha256 = '') {
    $payloadPath = Join-Path $negativeRoot ($Label + '.json')
    Write-JsonFixture $payloadPath $Payload
    $preflightPath = [string]$freeze.preBuildSourcePreflightReceiptPath
    $preflightSha = [string]$freeze.preBuildSourcePreflightReceiptSha256
    $buildPath = [string]$freeze.releaseBuildReceiptPath
    $buildSha = [string]$freeze.releaseBuildReceiptSha256
    if ($ReceiptKind -ceq 'preflight') {
        $preflightPath = $payloadPath
        if ($PinnedSha256) { $preflightSha = $PinnedSha256 } else { $preflightSha = Get-Sha256File $payloadPath }
    }
    elseif ($ReceiptKind -ceq 'build') {
        $buildPath = $payloadPath
        if ($PinnedSha256) { $buildSha = $PinnedSha256 } else { $buildSha = Get-Sha256File $payloadPath }
    }
    else { throw "Unknown freeze fixture kind: $ReceiptKind" }
    return Assert-FailClosedChild $Label $script:ProducerScripts.sourceFreeze @(
        '-PreBuildSourcePreflightPath', $preflightPath,
        '-PreBuildSourcePreflightSha256', $preflightSha,
        '-ReleaseBuildReceiptPath', $buildPath,
        '-ReleaseBuildReceiptSha256', $buildSha,
        '-CMakeCachePath', [string]$freeze.cmakeCachePath,
        '-RepoRoot', [string]$freeze.sourceRootPath,
        '-AuditRoot', $AuditRoot,
        '-RunId', ('negative-freeze-' + [Guid]::NewGuid().ToString('N')),
        '-OutputPath', $OutputPath
    ) 'SOURCE_FREEZE_FAIL'
}
function Assert-MalformedAggregateManifestRejected([string]$Label, [scriptblock]$Mutate, [string]$OutputPath) {
    $payload = Read-Json $CandidateManifestPath
    & $Mutate $payload
    $payloadPath = Join-Path $negativeRoot ($Label + '.json')
    Write-JsonFixture $payloadPath $payload
    return Assert-FailClosedChild $Label $script:SelfPath @(
        '-CandidateManifestPath', $payloadPath,
        '-AuditRoot', $AuditRoot,
        '-OutputPath', $OutputPath
    ) 'FAIL'
}
function Assert-AggregateRejectsMutatedPinnedFreeze([string]$OutputPath) {
    $originalFreezePath = [string]$sourcePins.sourceFreezeReceiptPath
    $mutatedFreeze = Read-Json $originalFreezePath
    $mutatedFreeze.status = 'SOURCE_FREEZE_FAIL'
    $mutatedFreezePath = Join-Path $negativeRoot 'mutated-pinned-source-freeze.json'
    Write-JsonFixture $mutatedFreezePath $mutatedFreeze
    $manifest = Read-Json $CandidateManifestPath
    $manifest.sourceAndAuthorityPins.sourceFreezeReceiptPath = $mutatedFreezePath
    $manifestPath = Join-Path $negativeRoot 'aggregate-mutated-pinned-freeze-manifest.json'
    Write-JsonFixture $manifestPath $manifest
    $negativeOutput = $OutputPath
    return Assert-FailClosedChild 'aggregateRejectsMutatedPinnedFreezeBytes' $script:SelfPath @(
        '-CandidateManifestPath', $manifestPath,
        '-AuditRoot', $AuditRoot,
        '-OutputPath', $negativeOutput
    ) 'FAIL'
}
function Assert-NormalizedSubmodulePinsRoundTrip([string]$FreezeScriptPath) {
    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($FreezeScriptPath, [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count -gt 0) { throw "Could not parse source-freeze helper for array-shape test: $($parseErrors[0].Message)" }
    $functionAst = $ast.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Get-NormalizedSubmodulePins'
    }, $true)
    if ($null -eq $functionAst) { throw 'Source-freeze Get-NormalizedSubmodulePins function was not found.' }
    . ([scriptblock]::Create($functionAst.Extent.Text))
    foreach ($rowCount in @(0, 1, 2)) {
        $inputRows = @()
        for ($index = 0; $index -lt $rowCount; $index++) {
            $inputRows += [pscustomobject][ordered]@{
                path = 'module-' + $index
                registeredCommit = ('a' * 40)
                headCommit = ('b' * 40)
                worktreeStatus = ''
                sourceTreeSha256 = ('c' * 64)
            }
        }
        $json = ConvertTo-Json -InputObject ([ordered]@{ submoduleCommits = @(Get-NormalizedSubmodulePins $inputRows) }) -Depth 10 -Compress
        $roundTrip = $json | ConvertFrom-Json -ErrorAction Stop
        if ($roundTrip.submoduleCommits -isnot [array] -or $roundTrip.submoduleCommits.Count -ne $rowCount) {
            throw "Normalized submodule array did not round-trip as a flat $rowCount-row array."
        }
        for ($index = 0; $index -lt $rowCount; $index++) {
            if ($roundTrip.submoduleCommits[$index] -isnot [System.Management.Automation.PSCustomObject] -or
                $roundTrip.submoduleCommits[$index].path -cne ('module-' + $index)) {
                throw "Normalized submodule row $index was nested, missing, or out of order in the JSON round trip."
            }
        }
    }
    return [pscustomobject]@{ id = 'normalizedSubmodulePinsFlatArrayRoundTrip'; status = 'PASS'; coveredRowCounts = @(0, 1, 2) }
}
function Assert-CandidateGateReceipt($Receipt, [string]$Schema, [string]$ManifestSha, [string]$FreezeSha, [string]$TreeSha, [string]$IdentitySha) {
    foreach ($field in @('schema','status','candidateGateStatus','candidateManifestSha256','sourceFreezeReceiptSha256','sourceTreeSha256')) {
        Assert-JsonString $Receipt $field 'Candidate gate receipt'
    }
    Assert-NoGoBoundary $Receipt 'Candidate gate receipt'
    Assert-JsonBoolean $Receipt 'staticOnly' 'Candidate gate receipt'
    if ($Receipt.schema -cne $Schema -or $Receipt.status -cne 'PASS' -or $Receipt.staticOnly -ne $true -or
        $Receipt.candidateGateStatus -cne 'PASS' -or $Receipt.candidateManifestSha256 -cne $ManifestSha -or
        $Receipt.sourceFreezeReceiptSha256 -cne $FreezeSha -or $Receipt.sourceTreeSha256 -cne $TreeSha) {
        throw 'Candidate gate receipt does not prove this pinned candidate and frozen source.'
    }
    if (-not [string]::IsNullOrWhiteSpace($IdentitySha)) {
        Assert-JsonString $Receipt 'candidateManifestIdentitySha256' 'Candidate gate receipt'
        if ($Receipt.candidateManifestIdentitySha256 -cne $IdentitySha) { throw 'Candidate gate canonical identity differs.' }
    }
}
function Invoke-FrozenCandidateGate([string]$ScriptPath, [string[]]$Arguments, [string[]]$Sources, [string]$ReceiptPath, [switch]$DiscoverReceipt) {
    $handles = New-Object 'System.Collections.Generic.List[System.IO.FileStream]'
    try {
        foreach ($path in @($ScriptPath) + $Sources) {
            $full = [IO.Path]::GetFullPath($path)
            if (-not $full.StartsWith($sourceRootPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Candidate gate source escaped the frozen root.' }
            $relative = $full.Substring($sourceRootPrefix.Length).Replace('\','/')
            $stream = [IO.File]::Open($full, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
            [void]$handles.Add($stream)
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $actual = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-','').ToLowerInvariant() }
            finally { $sha.Dispose() }
            if (-not $snapshotRows.ContainsKey($relative) -or $snapshotRows[$relative] -cne $actual) { throw "Candidate gate source differs from freeze: $relative" }
        }
        $childArgs = @('-File',$ScriptPath) + $Arguments
        if (-not $DiscoverReceipt) { $childArgs += @('-OutputPath',$ReceiptPath) }
        $run = Invoke-ChildPowerShell $childArgs
        if ($run.exitCode -ne 0) { throw "Candidate gate failed: $($run.stdout.Trim()) $($run.stderr.Trim())" }
        if ($DiscoverReceipt) {
            if ($run.stdout -notmatch '(?m)receipt=([^\r\n]+)\r?$') { throw 'Fixture did not report its receipt path.' }
            $found = [IO.Path]::GetFullPath($Matches[1])
            if (-not $found.StartsWith($AuditRoot.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture receipt escaped the audit root.' }
            $receiptBytes = [IO.File]::ReadAllBytes($found)
            $copyStream = [IO.File]::Open($ReceiptPath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
            try { $copyStream.Write($receiptBytes,0,$receiptBytes.Length) } finally { $copyStream.Dispose() }
        }
        return Read-Json $ReceiptPath
    }
    finally { foreach ($handle in $handles) { $handle.Dispose() } }
}
$outputRoot = $AuditRoot.TrimEnd('\') + '\'
if (-not $OutputPath.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Static-test receipt output must remain under the external v43 audit root.' }
if ([IO.File]::Exists($OutputPath)) { throw "Refusing to overwrite existing evidence: $OutputPath" }

$receipt = $null
$failure = $null
$negativeTests = @()
$harnessPathCheck = $null
$sourceScopeContractCheck = $null
$staticGateAssessment = $null
$preflightGateAssessment = $null
$producerModuleBoundary = $null
$candidateGateEvidence = @{}
try {
    $manifestBytes = [IO.File]::ReadAllBytes($CandidateManifestPath)
    $manifest = (New-Object Text.UTF8Encoding($false, $true)).GetString($manifestBytes) | ConvertFrom-Json -ErrorAction Stop
    Assert-JsonObjectValue $manifest 'Candidate manifest'
    foreach ($field in @('schema', 'candidateVersion')) { Assert-JsonString $manifest $field 'Candidate manifest' }
    Assert-JsonBoolean $manifest 'goPermitted' 'Candidate manifest'
    Assert-ProductActions $manifest 'Candidate manifest'
    if ($manifest.schema -cne 'range.pc11-fresh-run-v43-admission-candidate/v1' -or $manifest.candidateVersion -cne 'v43' -or $manifest.goPermitted -ne $false) {
        throw 'Candidate manifest schema/version or no-GO boundary is invalid.'
    }
    Assert-JsonObjectProperty $manifest 'candidateEvidencePins' 'Candidate manifest'
    Assert-JsonObjectProperty $manifest 'sourceAndAuthorityPins' 'Candidate manifest'
    Assert-JsonObjectProperty $manifest 'installBinding' 'Candidate manifest'
    $evidence = $manifest.candidateEvidencePins
    $sourcePins = $manifest.sourceAndAuthorityPins
    $identitySha = Get-ManifestIdentitySha256 $manifest
    Assert-JsonArray $evidence 'requiredStaticTestIds' 'Candidate manifest.candidateEvidencePins'
    foreach ($testId in $evidence.requiredStaticTestIds) {
        if ($testId -isnot [string]) { throw 'Candidate manifest requiredStaticTestIds entries must be JSON strings.' }
    }
    if (@($evidence.requiredStaticTestIds) -cnotcontains 'root_bootstrap' -or
        (ConvertTo-Json -InputObject @($evidence.requiredStaticTestIds) -Compress) -cne (ConvertTo-Json -InputObject $script:RequiredTestIds -Compress)) {
        throw 'Candidate manifest requiredStaticTestIds differ from the frozen v43 static contract.'
    }

    foreach ($field in @('sourceFreezeReceiptPath', 'sourceFreezeReceiptSha256', 'sourceTreeSha256')) { Assert-JsonString $sourcePins $field 'Candidate manifest.sourceAndAuthorityPins' }
    Assert-Sha256String $sourcePins.sourceFreezeReceiptSha256 'Candidate manifest.sourceAndAuthorityPins.sourceFreezeReceiptSha256'
    Assert-Sha256String $sourcePins.sourceTreeSha256 'Candidate manifest.sourceAndAuthorityPins.sourceTreeSha256'
    $freezePath = [IO.Path]::GetFullPath($sourcePins.sourceFreezeReceiptPath)
    $freeze = Read-PinnedJson $freezePath $sourcePins.sourceFreezeReceiptSha256 'Source freeze receipt'
    foreach ($field in @('schema', 'status')) { Assert-JsonString $freeze $field 'Source freeze receipt' }
    if ($freeze.schema -cne 'range.pc11-v43-source-freeze/v1' -or $freeze.status -cne 'PASS') { throw 'Source freeze is not a PASS receipt.' }
    Assert-NoGoBoundary $freeze 'Source freeze receipt'
    foreach ($field in @('sourceRootPath', 'rootCommit', 'releaseBuildReceiptPath', 'releaseBuildReceiptSha256', 'postBuildSourcePreflightReceiptPath', 'postBuildSourcePreflightReceiptSha256', 'sourceTreeSha256')) {
        Assert-JsonString $freeze $field 'Source freeze receipt'
    }
    Assert-CommitString $freeze.rootCommit 'Source freeze receipt.rootCommit'
    foreach ($field in @('releaseBuildReceiptSha256', 'postBuildSourcePreflightReceiptSha256', 'sourceTreeSha256')) {
        Assert-Sha256String $freeze.$field "Source freeze receipt.$field"
    }
    Assert-Sha256String $freeze.sourceTreeSha256 'Source freeze receipt.sourceTreeSha256'

    $preflightPath = [IO.Path]::GetFullPath($freeze.postBuildSourcePreflightReceiptPath)
    $preflight = Read-PinnedJson $preflightPath $freeze.postBuildSourcePreflightReceiptSha256 'Post-build source-preflight receipt'
    foreach ($field in @('schema', 'status', 'sourceRootPath', 'rootCommit', 'sourceTreeSha256')) { Assert-JsonString $preflight $field 'Post-build source-preflight receipt' }
    Assert-CommitString $preflight.rootCommit 'Post-build source-preflight receipt.rootCommit'
    Assert-Sha256String $preflight.sourceTreeSha256 'Post-build source-preflight receipt.sourceTreeSha256'
    if ($preflight.schema -cne 'range.pc11-v43-source-preflight/v1' -or $preflight.status -cne 'PASS_SOURCE_PREFLIGHT' -or
        $preflight.sourceRootPath -cne $freeze.sourceRootPath -or $preflight.rootCommit -cne $freeze.rootCommit -or
        $preflight.sourceTreeSha256 -cne $freeze.sourceTreeSha256) { throw 'Post-build source-preflight identity differs from the source freeze.' }

    foreach ($field in @('packageTreeReceiptPath', 'packageTreeReceiptSha256')) { Assert-JsonString $evidence $field 'Candidate manifest.candidateEvidencePins' }
    Assert-Sha256String $evidence.packageTreeReceiptSha256 'Candidate manifest.candidateEvidencePins.packageTreeReceiptSha256'
    $packagePath = [IO.Path]::GetFullPath($evidence.packageTreeReceiptPath)
    $package = Read-PinnedJson $packagePath $evidence.packageTreeReceiptSha256 'Package-tree receipt'
    foreach ($field in @('schema', 'status', 'candidateManifestIdentitySha256', 'sourceFreezeReceiptSha256', 'sourceTreeSha256', 'inventoryReportSha256')) {
        Assert-JsonString $package $field 'Package-tree receipt'
    }
    foreach ($field in @('candidateManifestIdentitySha256', 'sourceFreezeReceiptSha256', 'sourceTreeSha256', 'inventoryReportSha256')) {
        Assert-Sha256String $package.$field "Package-tree receipt.$field"
    }
    if ($package.schema -cne 'range.pc11-v43-package-tree-receipt/v1' -or $package.status -cne 'PASS' -or
        $package.candidateManifestIdentitySha256 -cne $identitySha -or
        $package.sourceFreezeReceiptSha256 -cne ([string]$sourcePins.sourceFreezeReceiptSha256).ToLowerInvariant() -or
        $package.sourceTreeSha256 -cne ([string]$sourcePins.sourceTreeSha256).ToLowerInvariant() -or
        $package.inventoryReportSha256 -cne ([string]$manifest.installBinding.inventoryReportSha256).ToLowerInvariant()) {
        throw 'Package-tree receipt does not bind the candidate identity/source/inventory pins.'
    }
    Assert-NoGoBoundary $package 'Package-tree receipt'

    $parsedScripts = New-Object 'System.Collections.Generic.List[object]'
    foreach ($scriptPath in @(
        $script:ProducerScripts.sourcePreflight,
        $script:ProducerScripts.sourceFreeze,
        $script:ProducerScripts.packageTree,
        $script:ProducerScripts.finalize,
        (Join-Path $PSScriptRoot 'test-source-scope-v43.ps1'),
        (Join-Path $PSScriptRoot 'test-harness-path-v43.ps1'),
        (Join-Path $PSScriptRoot 'test-static-gates-v43.ps1'),
        (Join-Path $PSScriptRoot 'test-preflight-gates-v43.ps1'),
        (Join-Path $PSScriptRoot 'test-producer-module-boundary-v43.ps1'),
        $MyInvocation.MyCommand.Path
    )) {
        if (-not [IO.File]::Exists($scriptPath)) { throw "Static chain script is missing: $scriptPath" }
        $parseErrors = @(Get-ParseErrors $scriptPath)
        if ($parseErrors.Count -gt 0) { throw "PowerShell parser rejected $scriptPath : $($parseErrors[0].Message)" }
        [void]$parsedScripts.Add([pscustomobject]@{ path = [IO.Path]::GetFullPath($scriptPath); sha256 = Get-Sha256File $scriptPath })
    }
    $negativeRoot = Join-Path $AuditRoot ('negative-static-chain-' + [Guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($negativeRoot)
    $negativeTests += Assert-NormalizedSubmodulePinsRoundTrip $script:ProducerScripts.sourceFreeze

    $harnessPathTestPath = Join-Path $PSScriptRoot 'test-harness-path-v43.ps1'
    $harnessPathResult = Invoke-ChildPowerShell @('-File', $harnessPathTestPath, '-AuditRoot', $AuditRoot)
    if ($harnessPathResult.exitCode -ne 0 -or
        $harnessPathResult.stdout -notmatch '(?m)^PASS_HARNESS_PATH_TEST\b' -or
        $harnessPathResult.stdout -notmatch '\bproducerCount=4\b') {
        throw "Static harness producer-path test failed: $($harnessPathResult.stdout.Trim()) $($harnessPathResult.stderr.Trim())"
    }
    $harnessPathCheck = [ordered]@{
        status = 'PASS'
        scriptPath = [IO.Path]::GetFullPath($harnessPathTestPath)
        scriptSha256 = Get-Sha256File $harnessPathTestPath
        stdout = $harnessPathResult.stdout.Trim()
    }

    $scopeTestPath = Join-Path $PSScriptRoot 'test-source-scope-v43.ps1'
    $scopeTestReceiptPath = Join-Path $negativeRoot 'source-scope-producer-admission-test.json'
    $scopeTestResult = Invoke-ChildPowerShell @('-File', $scopeTestPath, '-AuditRoot', $AuditRoot, '-OutputPath', $scopeTestReceiptPath)
    if ($scopeTestResult.exitCode -ne 0 -or -not [IO.File]::Exists($scopeTestReceiptPath)) {
        throw "Producer-to-admission source-scope test failed: $($scopeTestResult.stdout.Trim()) $($scopeTestResult.stderr.Trim())"
    }
    $scopeTestReceipt = Read-Json $scopeTestReceiptPath
    if ($scopeTestReceipt.schema -cne 'range.pc11-v43-source-scope-producer-admission-test/v1' -or
        $scopeTestReceipt.status -cne 'PASS_SOURCE_SCOPE_CONTRACT') { throw 'Producer-to-admission source-scope test receipt is invalid.' }
    Assert-NoGoBoundary $scopeTestReceipt 'Producer-to-admission source-scope test'
    $sourceScopeContractCheck = [ordered]@{
        status = 'PASS'
        receiptPath = $scopeTestReceiptPath
        receiptSha256 = Get-Sha256File $scopeTestReceiptPath
        sourceTreeSha256 = [string]$scopeTestReceipt.sourceTreeSha256
        sourceFileCount = [int]$scopeTestReceipt.sourceFileCount
        nestedSourcePath = [string]$scopeTestReceipt.nestedSourcePath
        excludedRootBuildPath = [string]$scopeTestReceipt.excludedRootBuildPath
    }

    $badBuildPath = Join-Path $negativeRoot 'malformed-release-build-receipt.json'
    $badBuild = [ordered]@{ schema = 'range.pc11-v43-release-build-receipt/invalid-test'; status = 'FAIL'; candidateVersion = 'v43'; variant = 'OFF'; configuration = 'Release'; staticOnly = $true; productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }; goPermitted = $false }
    [IO.File]::WriteAllText($badBuildPath, (($badBuild | ConvertTo-Json -Depth 8) + "`n"), (New-Object Text.UTF8Encoding($false)))
    $badBuildSha = Get-Sha256File $badBuildPath
    $freezeScriptPath = $script:ProducerScripts.sourceFreeze
    $negativeTests += Assert-FailClosedChild 'freezeRejectsMalformedBuildReceipt' $freezeScriptPath @(
        '-PreBuildSourcePreflightPath', [string]$freeze.preBuildSourcePreflightReceiptPath,
        '-PreBuildSourcePreflightSha256', ([string]$freeze.preBuildSourcePreflightReceiptSha256),
        '-ReleaseBuildReceiptPath', $badBuildPath,
        '-ReleaseBuildReceiptSha256', $badBuildSha,
        '-CMakeCachePath', (Join-Path $negativeRoot 'unused-CMakeCache.txt'),
        '-RepoRoot', [string]$freeze.sourceRootPath,
        '-AuditRoot', $AuditRoot,
        '-RunId', ('negative-freeze-' + [Guid]::NewGuid().ToString('N')),
        '-OutputPath', (Join-Path $negativeRoot 'freeze-failure.json')
    ) 'SOURCE_FREEZE_FAIL'
    $preBuildWrongSchema = Read-Json ([string]$freeze.preBuildSourcePreflightReceiptPath)
    $preBuildWrongSchema.schema = @()
    $negativeTests += Assert-MalformedFreezePayloadRejected 'freezeRejectsArrayPrebuildSchema' 'preflight' $preBuildWrongSchema (Join-Path $negativeRoot 'freeze-array-prebuild-schema-result.json')
    $preBuildWrongStatus = Read-Json ([string]$freeze.preBuildSourcePreflightReceiptPath)
    $preBuildWrongStatus.status = @()
    $negativeTests += Assert-MalformedFreezePayloadRejected 'freezeRejectsArrayPrebuildStatus' 'preflight' $preBuildWrongStatus (Join-Path $negativeRoot 'freeze-array-prebuild-status-result.json')
    $preBuildWrongGo = Read-Json ([string]$freeze.preBuildSourcePreflightReceiptPath)
    $preBuildWrongGo.goPermitted = @()
    $negativeTests += Assert-MalformedFreezePayloadRejected 'freezeRejectsArrayPrebuildNoGoFlag' 'preflight' $preBuildWrongGo (Join-Path $negativeRoot 'freeze-array-prebuild-nogo-result.json')
    $preBuildWrongActions = Read-Json ([string]$freeze.preBuildSourcePreflightReceiptPath)
    $preBuildWrongActions.productActions = @()
    $negativeTests += Assert-MalformedFreezePayloadRejected 'freezeRejectsArrayPrebuildProductActions' 'preflight' $preBuildWrongActions (Join-Path $negativeRoot 'freeze-array-prebuild-actions-result.json')
    $preBuildWrongRoot = Read-Json ([string]$freeze.preBuildSourcePreflightReceiptPath)
    $preBuildWrongRoot.rootCommit = @()
    $negativeTests += Assert-MalformedFreezePayloadRejected 'freezeRejectsArrayPrebuildRootCommit' 'preflight' $preBuildWrongRoot (Join-Path $negativeRoot 'freeze-array-prebuild-root-result.json')
    $preBuildWrongSha = Read-Json ([string]$freeze.preBuildSourcePreflightReceiptPath)
    $preBuildWrongSha.sourceTreeSha256 = @()
    $negativeTests += Assert-MalformedFreezePayloadRejected 'freezeRejectsArrayPrebuildSourceSha' 'preflight' $preBuildWrongSha (Join-Path $negativeRoot 'freeze-array-prebuild-sha-result.json')
    $preBuildMutatedBytes = Read-Json ([string]$freeze.preBuildSourcePreflightReceiptPath)
    $preBuildMutatedBytes.status = 'SOURCE_PREFLIGHT_FAIL'
    $negativeTests += Assert-MalformedFreezePayloadRejected 'freezeRejectsMutatedPrebuildPinnedBytes' 'preflight' $preBuildMutatedBytes (Join-Path $negativeRoot 'freeze-mutated-prebuild-bytes-result.json') ([string]$freeze.preBuildSourcePreflightReceiptSha256)
    $buildWrongSchema = Read-Json ([string]$freeze.releaseBuildReceiptPath)
    $buildWrongSchema.schema = @()
    $negativeTests += Assert-MalformedFreezePayloadRejected 'freezeRejectsArrayBuildSchema' 'build' $buildWrongSchema (Join-Path $negativeRoot 'freeze-array-build-schema-result.json')
    $buildWrongStatus = Read-Json ([string]$freeze.releaseBuildReceiptPath)
    $buildWrongStatus.status = @()
    $negativeTests += Assert-MalformedFreezePayloadRejected 'freezeRejectsArrayBuildStatus' 'build' $buildWrongStatus (Join-Path $negativeRoot 'freeze-array-build-status-result.json')
    $buildWrongVersion = Read-Json ([string]$freeze.releaseBuildReceiptPath)
    $buildWrongVersion.candidateVersion = @()
    $negativeTests += Assert-MalformedFreezePayloadRejected 'freezeRejectsArrayBuildVersion' 'build' $buildWrongVersion (Join-Path $negativeRoot 'freeze-array-build-version-result.json')
    $buildWrongVariant = Read-Json ([string]$freeze.releaseBuildReceiptPath)
    $buildWrongVariant.variant = @()
    $negativeTests += Assert-MalformedFreezePayloadRejected 'freezeRejectsArrayBuildVariant' 'build' $buildWrongVariant (Join-Path $negativeRoot 'freeze-array-build-variant-result.json')
    $buildWrongConfiguration = Read-Json ([string]$freeze.releaseBuildReceiptPath)
    $buildWrongConfiguration.configuration = @()
    $negativeTests += Assert-MalformedFreezePayloadRejected 'freezeRejectsArrayBuildConfiguration' 'build' $buildWrongConfiguration (Join-Path $negativeRoot 'freeze-array-build-configuration-result.json')
    $buildWrongStaticOnly = Read-Json ([string]$freeze.releaseBuildReceiptPath)
    $buildWrongStaticOnly.staticOnly = @()
    $negativeTests += Assert-MalformedFreezePayloadRejected 'freezeRejectsArrayBuildStaticOnlyFlag' 'build' $buildWrongStaticOnly (Join-Path $negativeRoot 'freeze-array-build-static-result.json')
    $buildWrongNoGo = Read-Json ([string]$freeze.releaseBuildReceiptPath)
    $buildWrongNoGo.goPermitted = @()
    $negativeTests += Assert-MalformedFreezePayloadRejected 'freezeRejectsArrayBuildNoGoFlag' 'build' $buildWrongNoGo (Join-Path $negativeRoot 'freeze-array-build-nogo-result.json')
    $buildWrongActions = Read-Json ([string]$freeze.releaseBuildReceiptPath)
    $buildWrongActions.productActions = @()
    $negativeTests += Assert-MalformedFreezePayloadRejected 'freezeRejectsArrayBuildProductActions' 'build' $buildWrongActions (Join-Path $negativeRoot 'freeze-array-build-actions-result.json')
    $buildWrongRoot = Read-Json ([string]$freeze.releaseBuildReceiptPath)
    $buildWrongRoot.rootCommit = @()
    $negativeTests += Assert-MalformedFreezePayloadRejected 'freezeRejectsArrayBuildRootCommit' 'build' $buildWrongRoot (Join-Path $negativeRoot 'freeze-array-build-root-result.json')
    $buildWrongSha = Read-Json ([string]$freeze.releaseBuildReceiptPath)
    $buildWrongSha.sourceTreeSha256 = @()
    $negativeTests += Assert-MalformedFreezePayloadRejected 'freezeRejectsArrayBuildSourceSha' 'build' $buildWrongSha (Join-Path $negativeRoot 'freeze-array-build-sha-result.json')
    $buildMutatedBytes = Read-Json ([string]$freeze.releaseBuildReceiptPath)
    $buildMutatedBytes.status = 'FAIL'
    $negativeTests += Assert-MalformedFreezePayloadRejected 'freezeRejectsMutatedBuildPinnedBytes' 'build' $buildMutatedBytes (Join-Path $negativeRoot 'freeze-mutated-build-bytes-result.json') ([string]$freeze.releaseBuildReceiptSha256)
    $aggregateMalformedSchema = Assert-MalformedAggregateManifestRejected 'aggregateRejectsArrayManifestSchema' { param($payload) $payload.schema = @() } (Join-Path $negativeRoot 'aggregate-array-schema-result.json')
    $negativeTests += $aggregateMalformedSchema
    $aggregateMalformedActions = Assert-MalformedAggregateManifestRejected 'aggregateRejectsArrayManifestProductActions' { param($payload) $payload.productActions = @() } (Join-Path $negativeRoot 'aggregate-array-actions-result.json')
    $negativeTests += $aggregateMalformedActions
    $aggregateMalformedVersion = Assert-MalformedAggregateManifestRejected 'aggregateRejectsArrayManifestVersion' { param($payload) $payload.candidateVersion = @() } (Join-Path $negativeRoot 'aggregate-array-version-result.json')
    $negativeTests += $aggregateMalformedVersion
    $aggregateMalformedNoGo = Assert-MalformedAggregateManifestRejected 'aggregateRejectsArrayManifestNoGoFlag' { param($payload) $payload.goPermitted = @() } (Join-Path $negativeRoot 'aggregate-array-nogo-result.json')
    $negativeTests += $aggregateMalformedNoGo
    $aggregateMalformedRoot = Assert-MalformedAggregateManifestRejected 'aggregateRejectsArraySourceRootCommit' { param($payload) $payload.sourceAndAuthorityPins.rootCommit = @() } (Join-Path $negativeRoot 'aggregate-array-root-result.json')
    $negativeTests += $aggregateMalformedRoot
    $aggregateMalformedSha = Assert-MalformedAggregateManifestRejected 'aggregateRejectsArraySourceTreeSha' { param($payload) $payload.sourceAndAuthorityPins.sourceTreeSha256 = @() } (Join-Path $negativeRoot 'aggregate-array-source-sha-result.json')
    $negativeTests += $aggregateMalformedSha
    $aggregateMalformedBuildSha = Assert-MalformedAggregateManifestRejected 'aggregateRejectsArrayBuildReceiptSha' { param($payload) $payload.sourceAndAuthorityPins.releaseBuildReceiptSha256 = @() } (Join-Path $negativeRoot 'aggregate-array-build-sha-result.json')
    $negativeTests += $aggregateMalformedBuildSha
    $negativeTests += Assert-AggregateRejectsMutatedPinnedFreeze (Join-Path $negativeRoot 'aggregate-mutated-pinned-freeze-result.json')
    $packageScriptPath = $script:ProducerScripts.packageTree
    $negativeTests += Assert-FailClosedChild 'packageTreeRejectsWrongManifestSha' $packageScriptPath @(
        '-CandidateManifestPath', $CandidateManifestPath,
        '-CandidateManifestSha256', ('0' * 64),
        '-AuditRoot', $AuditRoot,
        '-RunId', ('negative-package-' + [Guid]::NewGuid().ToString('N')),
        '-OutputPath', (Join-Path $negativeRoot 'package-failure.json')
    ) 'FAIL'
    $finalizeScriptPath = $script:ProducerScripts.finalize
    $negativeTests += Assert-FailClosedChild 'finalizeRejectsWrongManifestSha' $finalizeScriptPath @(
        '-CandidateManifestPath', $CandidateManifestPath,
        '-CandidateManifestSha256', ('0' * 64),
        '-AuditRoot', $AuditRoot,
        '-RunId', ('negative-finalize-' + [Guid]::NewGuid().ToString('N')),
        '-OutputPath', (Join-Path $negativeRoot 'finalize-failure.json')
    ) 'FINALIZE_FAIL'
    $assessmentPath = Join-Path $negativeRoot 'static-gate-assessment.json'
    $assessmentScript = Join-Path $PSScriptRoot 'test-static-gates-v43.ps1'
    $assessmentRun = Invoke-ChildPowerShell @('-File', $assessmentScript, '-AuditRoot', $AuditRoot, '-OutputPath', $assessmentPath)
    if ($assessmentRun.exitCode -ne 0) { throw "Static gate assessment failed: $($assessmentRun.stdout.Trim()) $($assessmentRun.stderr.Trim())" }
    $assessment = Read-Json $assessmentPath
    $remainingIds = @($script:RequiredTestIds | Where-Object { $_ -notin @('source_preflight', 'source_freeze', 'package_tree', 'source_static_tests') })
    if ($assessment.schema -cne 'range.pc11-v43-static-gate-assessment/v1' -or $assessment.status -cne 'STATIC_INCOMPLETE' -or
        ($assessment.requiredGateIds -join "`n") -cne ($remainingIds -join "`n") -or
        @($assessment.results).Count -ne $remainingIds.Count -or
        $assessment.candidateManifestIdentitySha256 -ne $null -or $assessment.sourceTreeSha256 -ne $null) {
        throw 'Static gate assessment does not match the bounded v43 local contract.'
    }
    Assert-NoGoBoundary $assessment 'Static gate assessment'
    $assessmentById = @{}
    foreach ($entry in $assessment.results) {
        $entryId = [string]$entry.testId
        if ($entryId -cnotin $remainingIds -or $assessmentById.ContainsKey($entryId) -or [string]$entry.status -cnotin @('PASS', 'PENDING')) {
            throw 'Static gate assessment has duplicate, unknown, or failed result rows.'
        }
        $assessmentById[$entryId] = $entry
    }
    if ($assessmentById.Count -ne $remainingIds.Count -or
        [int]$assessment.passedGateCount -ne @($assessment.results | Where-Object { $_.status -ceq 'PASS' }).Count -or
        ($assessment.pendingGateIds -join "`n") -cne ((@($assessment.results | Where-Object { $_.status -ceq 'PENDING' } | ForEach-Object { $_.testId })) -join "`n")) {
        throw 'Static gate assessment result counts or pending IDs are inconsistent.'
    }
    foreach ($entry in $assessment.results) {
        if ($entry.status -ceq 'PASS') {
            if ([string]$entry.testId -cnotin @('module_boundary_build', 'module_boundary_source', 'parser')) {
                throw 'A local static assessment attempted to promote a candidate-dependent gate.'
            }
            Assert-Hash ([string]$entry.evidencePath) ([string]$entry.evidenceSha256) ('Static gate ' + $entry.testId)
        }
    }
    $staticGateAssessment = [ordered]@{ receiptPath = $assessmentPath; receiptSha256 = Get-Sha256File $assessmentPath; scriptPath = $assessmentScript; scriptSha256 = Get-Sha256File $assessmentScript }

    $preflightPathLocal = Join-Path $negativeRoot 'preflight-gate-assessment.json'
    $preflightScript = Join-Path $PSScriptRoot 'test-preflight-gates-v43.ps1'
    $preflightRun = Invoke-ChildPowerShell @('-File', $preflightScript, '-AuditRoot', $AuditRoot, '-OutputPath', $preflightPathLocal)
    if ($preflightRun.exitCode -ne 0) { throw "Preflight gate assessment failed: $($preflightRun.stdout.Trim()) $($preflightRun.stderr.Trim())" }
    $preflightAssessment = Read-Json $preflightPathLocal
    $preflightIds = @('watchdog_preflight', 'save_predicate_preflight', 'path_shadow', 'sampler_arguments')
    if ($preflightAssessment.schema -cne 'range.pc11-v43-static-preflight-assessment/v1' -or
        $preflightAssessment.status -cne 'STATIC_INCOMPLETE' -or
        ($preflightAssessment.requiredGateIds -join "`n") -cne ($preflightIds -join "`n") -or
        @($preflightAssessment.results).Count -ne $preflightIds.Count -or
        [int]$preflightAssessment.passedGateCount -ne 0 -or [int]$preflightAssessment.failedGateCount -ne 0 -or
        ($preflightAssessment.pendingGateIds -join "`n") -cne ($preflightIds -join "`n") -or
        $preflightAssessment.candidateManifestIdentitySha256 -ne $null -or $preflightAssessment.sourceTreeSha256 -ne $null) {
        throw 'Preflight assessment attempted to promote an unproven candidate gate.'
    }
    Assert-NoGoBoundary $preflightAssessment 'Preflight gate assessment'
    foreach ($index in 0..($preflightIds.Count - 1)) {
        if ([string]$preflightAssessment.results[$index].testId -cne $preflightIds[$index] -or
            [string]$preflightAssessment.results[$index].status -cne 'PENDING') {
            throw 'Preflight assessment rows are not the four pending v43 gates.'
        }
    }
    $preflightGateAssessment = [ordered]@{ receiptPath = $preflightPathLocal; receiptSha256 = Get-Sha256File $preflightPathLocal; scriptPath = $preflightScript; scriptSha256 = Get-Sha256File $preflightScript; componentCaseCount = [int]$preflightAssessment.componentCaseCount }
    $producerBoundaryScript = Join-Path $PSScriptRoot 'test-producer-module-boundary-v43.ps1'
    $producerBoundaryHelper = Join-Path $script:PrepRoot 'tools\system-module-boundary-v43.ps1'
    $snapshotRows = @{}
    foreach ($row in $preflight.sourceFiles) { $snapshotRows[[string]$row.path] = [string]$row.sha256 }
    $producerBoundarySources = New-Object 'System.Collections.Generic.List[object]'
    $producerBoundaryLocks = New-Object 'System.Collections.Generic.List[System.IO.FileStream]'
    $sourceRootPrefix = [IO.Path]::GetFullPath([string]$freeze.sourceRootPath).TrimEnd('\') + '\'
    try {
        foreach ($sourcePath in @($producerBoundaryScript, $producerBoundaryHelper) + @($script:ProducerScripts.Values)) {
            $fullPath = [IO.Path]::GetFullPath([string]$sourcePath)
            if (-not $fullPath.StartsWith($sourceRootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Actual producer module-boundary source escaped the frozen source root.'
            }
            $relative = $fullPath.Substring($sourceRootPrefix.Length).Replace('\', '/')
            $lockedStream = [IO.File]::Open($fullPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
            [void]$producerBoundaryLocks.Add($lockedStream)
            $actualSha = Get-Sha256File $fullPath
            if (-not $snapshotRows.ContainsKey($relative) -or $snapshotRows[$relative] -cne $actualSha) {
                throw "Actual producer module-boundary source differs from the frozen source snapshot: $relative"
            }
            [void]$producerBoundarySources.Add([pscustomobject][ordered]@{ path = $fullPath; sourceRelativePath = $relative; sha256 = $actualSha })
        }
        $producerBoundaryRun = Invoke-ChildPowerShell @('-File', $producerBoundaryScript)
        if ($producerBoundaryRun.exitCode -ne 0 -or $producerBoundaryRun.stdout.Trim() -cne 'PASS producer-module-boundary cases=24') {
            throw "Actual producer module-boundary test failed: $($producerBoundaryRun.stdout.Trim()) $($producerBoundaryRun.stderr.Trim())"
        }
        foreach ($sourceRow in $producerBoundarySources) {
            Assert-Hash ([string]$sourceRow.path) ([string]$sourceRow.sha256) ('Actual producer module-boundary source ' + [string]$sourceRow.sourceRelativePath)
        }
    }
    finally {
        foreach ($lockedStream in $producerBoundaryLocks) { $lockedStream.Dispose() }
    }
    $producerBoundaryReceiptPath = Join-Path $negativeRoot 'producer-module-boundary.json'
    $producerBoundaryReceipt = [pscustomobject][ordered]@{
        schema = 'range.pc11-v43-producer-module-boundary/v1'
        status = 'PASS_PRODUCER_MODULE_BOUNDARY'
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        candidateManifestIdentitySha256 = $identitySha
        sourceTreeSha256 = ([string]$sourcePins.sourceTreeSha256).ToLowerInvariant()
        caseCount = 24
        sources = $producerBoundarySources.ToArray()
        testStdout = $producerBoundaryRun.stdout.Trim()
        productActions = [pscustomobject][ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
    Assert-NoGoBoundary $producerBoundaryReceipt 'Actual producer module-boundary receipt'
    if ([IO.File]::Exists($producerBoundaryReceiptPath)) { throw 'Refusing to overwrite producer module-boundary evidence.' }
    [IO.File]::WriteAllText($producerBoundaryReceiptPath, (($producerBoundaryReceipt | ConvertTo-Json -Depth 12) + "`n"), (New-Object Text.UTF8Encoding($false)))
    $producerModuleBoundary = [ordered]@{ receiptPath = $producerBoundaryReceiptPath; receiptSha256 = Get-Sha256File $producerBoundaryReceiptPath; caseCount = 24 }
    $manifestSha = Get-Sha256Bytes $manifestBytes
    $freezeSha = ([string]$sourcePins.sourceFreezeReceiptSha256).ToLowerInvariant()
    $treeSha = ([string]$sourcePins.sourceTreeSha256).ToLowerInvariant()
    $saveEnvironmentRoot = [IO.Path]::GetFullPath((Join-Path ([string]$freeze.sourceRootPath) 'build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit'))
    $saveEnvironmentPath = Join-Path $saveEnvironmentRoot ('save-environment-candidate-' + [Guid]::NewGuid().ToString('N') + '.json')
    $saveEnvironmentScript = Join-Path $PSScriptRoot 'test-save-environment-preflight-v43.ps1'
    $saveEnvironment = Invoke-FrozenCandidateGate $saveEnvironmentScript @(
        '-CandidateManifestPath',$CandidateManifestPath,'-CandidateManifestSha256',$manifestSha,
        '-SourceFreezePath',$freezePath,'-SourceFreezeSha256',$freezeSha,
        '-SystemModuleBoundarySha256',(Get-Sha256File $producerBoundaryHelper)
    ) @($producerBoundaryHelper) $saveEnvironmentPath
    Assert-JsonBoolean $saveEnvironment 'staticOnly' 'Save environment receipt'
    Assert-JsonBoolean $saveEnvironment 'goPermitted' 'Save environment receipt'
    Assert-ProductActions $saveEnvironment 'Save environment receipt'
    foreach ($field in @('schema','status','candidateGateStatus','candidateManifestSha256','sourceFreezeSha256','validatedInventorySha256','validatedBuildReceiptSha256')) {
        Assert-JsonString $saveEnvironment $field 'Save environment receipt'
    }
    if ($saveEnvironment.schema -cne 'range.pc11-v43-save-environment-preflight/v1' -or
        $saveEnvironment.status -cne 'PASS' -or $saveEnvironment.candidateGateStatus -cne 'PASS' -or
        $saveEnvironment.staticOnly -ne $true -or $saveEnvironment.goPermitted -ne $false -or
        $saveEnvironment.candidateManifestSha256 -cne $manifestSha -or $saveEnvironment.sourceFreezeSha256 -cne $freezeSha -or
        $saveEnvironment.validatedInventorySha256 -cne ([string]$manifest.installBinding.inventoryReportSha256).ToLowerInvariant() -or
        $saveEnvironment.validatedBuildReceiptSha256 -cne ([string]$sourcePins.releaseBuildReceiptSha256).ToLowerInvariant()) { throw 'Save environment preflight did not qualify this frozen candidate.' }
    $candidateGateEvidence['save_predicate_preflight'] = [pscustomobject]@{ status='PASS'; path=$saveEnvironmentPath; sha256=(Get-Sha256File $saveEnvironmentPath) }
    foreach ($gate in @(
        [pscustomobject]@{ id='root_seal'; script='test-root-seal-boundary-v43.ps1'; fixture='test-root-seal-boundary-v43-fixture.ps1'; schema='range.pc11-v43-root-seal-boundary-static-fixture/v1' },
        [pscustomobject]@{ id='finalize_integrity'; script='test-finalize-integrity-candidate-v43.ps1'; fixture='test-finalize-integrity-candidate-fixture-v43.ps1'; schema='range.pc11-v43-finalize-integrity-candidate-receipt/v1' }
    )) {
        $gateScript = Join-Path $PSScriptRoot $gate.script
        $gatePath = Join-Path $negativeRoot ($gate.id + '-candidate.json')
        $gateArgs = @('-CandidateManifestPath',$CandidateManifestPath,'-CandidateManifestSha256',$manifestSha,
            '-SourceFreezeReceiptPath',$freezePath,'-SourceFreezeReceiptSha256',$freezeSha)
        if ($gate.id -ceq 'finalize_integrity') {
            $gateArgs += @('-FinalizerSha256',(Get-Sha256File $script:ProducerScripts.finalize),'-SourceTestSha256',(Get-Sha256File $gateScript))
        }
        $gateSources = @((Join-Path $PSScriptRoot $gate.fixture),$script:ProducerScripts.finalize,
            (Join-Path $script:PrepRoot 'candidate-manifest-v43.admission-template.json'))
        if ($gate.id -ceq 'root_seal') {
            $gateSources += @((Join-Path $script:PrepRoot 'runtime\root-bootstrap-v43.ps1'),(Join-Path $PSScriptRoot 'test-root-bootstrap-v43.ps1'))
        }
        $gateReceipt = Invoke-FrozenCandidateGate $gateScript $gateArgs $gateSources $gatePath
        Assert-CandidateGateReceipt $gateReceipt $gate.schema $manifestSha $freezeSha $treeSha $identitySha
        $candidateGateEvidence[$gate.id] = [pscustomobject]@{ status='PASS'; path=$gatePath; sha256=(Get-Sha256File $gatePath) }
    }
    $controlScript = Join-Path $PSScriptRoot 'test-candidate-control-contract-v43.ps1'
    $controlPath = Join-Path $negativeRoot 'candidate-control-contract.json'
    $controlArgs = @('-CandidateManifestPath',$CandidateManifestPath,'-CandidateManifestSha256',$manifestSha,'-AuditRoot',$AuditRoot)
    foreach ($component in @(
        [pscustomobject]@{ id='save_predicate'; pathParam='SavePredicateReceiptPath'; shaParam='SavePredicateReceiptSha256'; runner='test-save-predicate-v43.ps1'; discover=$false; sources=@('tools/save-predicate-v43.ps1','tests/test-save-predicate-v43.ps1') },
        [pscustomobject]@{ id='sampler_arguments'; pathParam='SamplerArgumentsReceiptPath'; shaParam='SamplerArgumentsReceiptSha256'; runner='test-sampler-arguments-v43.ps1'; discover=$true; sources=@('runtime/sampler-arguments-v43.ps1','../../../scripts/benchmark/pc11_working_set.py','tests/test-sampler-arguments-v43.ps1') },
        [pscustomobject]@{ id='watchdog_contract'; pathParam='SupervisorWorkerReceiptPath'; shaParam='SupervisorWorkerReceiptSha256'; runner='test-supervisor-worker-contract-v43.ps1'; discover=$false; sources=@('runtime/contract-common-v43.ps1','runtime/supervisor-core-v43.ps1','runtime/worker-handshake-v43.ps1','runtime/watchdog-contract-v43.ps1','runtime/native-launch-boundary-v43.ps1','tests/fixtures/worker-fixture-v43.ps1','tests/test-supervisor-worker-contract-v43.ps1','finalize-v43.ps1') }
    )) {
        $pathValue = Get-Variable -Name $component.pathParam -ValueOnly
        $shaValue = Get-Variable -Name $component.shaParam -ValueOnly
        if ([string]::IsNullOrWhiteSpace($pathValue) -ne [string]::IsNullOrWhiteSpace($shaValue)) { throw 'Component receipt paths and SHA256 pins must be supplied together.' }
        if ([string]::IsNullOrWhiteSpace($pathValue)) {
            $componentRoot = Join-Path $negativeRoot $component.id
            [void][IO.Directory]::CreateDirectory($componentRoot)
            $originalPath = Join-Path $componentRoot 'original-receipt.json'
            if ($component.id -ceq 'watchdog_contract') {
                $originalPath = Join-Path $saveEnvironmentRoot ('watchdog-original-' + [Guid]::NewGuid().ToString('N') + '.json')
            }
            $componentSources = @($component.sources | ForEach-Object { [IO.Path]::GetFullPath((Join-Path $script:PrepRoot $_)) })
            $fixtureArgs = @()
            if ($component.id -cne 'watchdog_contract') { $fixtureArgs = @('-AuditRoot',$componentRoot) }
            $original = Invoke-FrozenCandidateGate (Join-Path $PSScriptRoot $component.runner) $fixtureArgs $componentSources $originalPath -DiscoverReceipt:$component.discover
            Assert-JsonBoolean $original 'staticOnly' 'Executed component fixture'
            Assert-JsonBoolean $original 'goPermitted' 'Executed component fixture'
            Assert-ProductActions $original 'Executed component fixture'
            if ($original.staticOnly -ne $true -or $original.goPermitted -ne $false) { throw 'Executed fixture violates static no-GO boundary.' }
            $sourceRows = @($component.sources | ForEach-Object {
                [pscustomobject]@{ path=$_; sha256=(Get-Sha256File ([IO.Path]::GetFullPath((Join-Path $script:PrepRoot $_)))) }
            })
            $envelope = [ordered]@{
                schema='range.pc11-v43-frozen-fixture-execution/v1'; componentId=$component.id; candidateVersion='v43'; variant='OFF'
                originalReceipt=[ordered]@{ path=$originalPath; sha256=(Get-Sha256File $originalPath) }
                candidateManifestIdentitySha256=$identitySha; sourceTreeSha256=$treeSha
                sourceBindingMethod='FROZEN_SOURCE_EXECUTION'; fixtureOnly=$true; staticOnly=$true
                sources=$sourceRows; productActions=[ordered]@{ guiLaunched=$false; savePerformed=$false; runPerformed=$false; confirmPerformed=$false }; goPermitted=$false
            }
            $pathValue = Join-Path $componentRoot 'frozen-execution.json'
            [IO.File]::WriteAllText($pathValue,(ConvertTo-Json -InputObject $envelope -Depth 12)+"`n",(New-Object Text.UTF8Encoding($false)))
            $shaValue = Get-Sha256File $pathValue
            if ($component.id -ceq 'watchdog_contract') { $watchdogExecution = $original; $watchdogExecutionPath = $pathValue }
        }
        $controlArgs += @(('-' + $component.pathParam),$pathValue,('-' + $component.shaParam),$shaValue)
        if ($component.id -ceq 'watchdog_contract') { $watchdogExecutionEnvelopePath = $pathValue }
    }
    $controlReceipt = Invoke-FrozenCandidateGate $controlScript $controlArgs @() $controlPath
    Assert-NoGoBoundary $controlReceipt 'Candidate control receipt'
    foreach ($field in @('schema','candidateManifestIdentitySha256','sourceTreeSha256')) { Assert-JsonString $controlReceipt $field 'Candidate control receipt' }
    if ($controlReceipt.schema -cne 'range.pc11-v43-candidate-control-contract-receipt/v1' -or
        $controlReceipt.candidateManifestIdentitySha256 -cne $identitySha -or $controlReceipt.sourceTreeSha256 -cne $treeSha) { throw 'Candidate control receipt identity differs.' }
    Assert-JsonArray $controlReceipt 'results' 'Candidate control receipt'
    $controlIds = @('save_predicate','sampler_arguments','watchdog_contract')
    if ($controlReceipt.results.Count -ne $controlIds.Count) { throw 'Candidate control receipt row count differs.' }
    for ($index=0; $index -lt $controlIds.Count; $index++) {
        $row = $controlReceipt.results[$index]
        foreach ($field in @('testId','status')) { Assert-JsonString $row $field 'Candidate control row' }
        if ($row.testId -cne $controlIds[$index] -or $row.status -cnotin @('PASS','PENDING')) { throw 'Candidate control row order or status differs.' }
        if ($row.status -ceq 'PASS') {
            if ($row.sourcePathBinding -cne 'RECEIPT_AND_FROZEN_SOURCE_HASHES_MATCH' -or
                $row.candidateIdentityBinding -cne 'EXTERNAL_MANIFEST_IDENTITY_AND_SOURCE_TREE_MATCH') { throw 'Candidate control PASS lacks frozen source binding.' }
            $candidateGateEvidence[$row.testId] = [pscustomobject]@{ status='PASS'; path=$controlPath; sha256=(Get-Sha256File $controlPath) }
            if ($row.testId -ceq 'watchdog_contract' -and $null -ne (Get-Variable -Name watchdogExecution -ErrorAction SilentlyContinue)) {
                foreach ($coveredId in @('supervisor','worker_handshake','native_launch_boundary')) {
                    $coveredRows = @($watchdogExecution.results | Where-Object { $_.testId -ceq $coveredId })
                    if ($coveredRows.Count -ne 1 -or $coveredRows[0].status -isnot [string] -or $coveredRows[0].status -cne 'PASS') { throw 'Frozen supervisor fixture is missing tested contract coverage.' }
                    $candidateGateEvidence[$coveredId] = [pscustomobject]@{ status='PASS'; path=$watchdogExecutionPath; sha256=(Get-Sha256File $watchdogExecutionPath) }
                }
            }
        }
    }
    $readinessScript = Join-Path $PSScriptRoot 'test-final-readiness-candidate-v43.ps1'
    $readinessPath = Join-Path $negativeRoot 'final-readiness-candidate.json'
    $readiness = Invoke-FrozenCandidateGate $readinessScript @(
        '-CandidateManifestPath',$CandidateManifestPath,'-CandidateManifestSha256',$manifestSha,
        '-SourceFreezeReceiptPath',$freezePath,'-SourceFreezeReceiptSha256',$freezeSha,
        '-CandidateControlReceiptPath',$controlPath,'-CandidateControlReceiptSha256',(Get-Sha256File $controlPath),
        '-WatchdogExecutionEnvelopePath',$watchdogExecutionEnvelopePath,'-WatchdogExecutionEnvelopeSha256',(Get-Sha256File $watchdogExecutionEnvelopePath)
    ) @() $readinessPath
    Assert-CandidateGateReceipt $readiness 'range.pc11-v43-final-readiness-receipt/v1' $manifestSha $freezeSha $treeSha $identitySha
    Assert-JsonArray $readiness 'results' 'Final readiness receipt'
    $readinessIds = @('watchdog_preflight','path_shadow','root_bootstrap')
    if ($readiness.results.Count -ne $readinessIds.Count) { throw 'Final readiness receipt coverage differs.' }
    for ($index=0; $index -lt $readinessIds.Count; $index++) {
        $row = $readiness.results[$index]
        foreach ($field in @('testId','status')) { Assert-JsonString $row $field 'Final readiness row' }
        if ($row.testId -cne $readinessIds[$index] -or $row.status -cne 'PASS') { throw 'Final readiness row did not qualify its static contract.' }
        $candidateGateEvidence[$row.testId] = [pscustomobject]@{ status='PASS'; path=$readinessPath; sha256=(Get-Sha256File $readinessPath) }
    }
    $sourceStaticStatus = 'PASS'
    $sourceStaticReason = 'Owned v43 scripts parse under Windows PowerShell 5.1; producer paths resolve from tests to prep; producer/admission nested source-scope and wrong-pin tests pass with no-GO receipts.'
}
catch { $failure = $_.Exception.Message }

if ($null -eq $failure) {
    $statuses = @{}
    foreach ($testId in $script:RequiredTestIds) { $statuses[$testId] = 'PENDING' }
    $statuses['source_preflight'] = 'PASS'
    $statuses['source_freeze'] = 'PASS'
    $statuses['package_tree'] = 'PASS'
    $statuses['source_static_tests'] = $sourceStaticStatus
    # Both local helper fixtures and the actual producer shadow tests are
    # required before the two module-boundary IDs can be promoted.
    $statuses['module_boundary_build'] = [string]$assessmentById['module_boundary_build'].status
    $statuses['module_boundary_source'] = [string]$assessmentById['module_boundary_source'].status
    $statuses['parser'] = [string]$assessmentById['parser'].status
    foreach ($entryId in $candidateGateEvidence.Keys) { $statuses[$entryId] = $candidateGateEvidence[$entryId].status }
    $results = New-Object 'System.Collections.Generic.List[object]'
    foreach ($testId in $script:RequiredTestIds) {
        $result = [ordered]@{ testId = $testId; status = $statuses[$testId] }
        if ($testId -eq 'source_preflight') { $result.evidencePath = $preflightPath; $result.evidenceSha256 = [string]$freeze.postBuildSourcePreflightReceiptSha256 }
        elseif ($testId -eq 'source_freeze') { $result.evidencePath = $freezePath; $result.evidenceSha256 = [string]$sourcePins.sourceFreezeReceiptSha256 }
        elseif ($testId -eq 'package_tree') { $result.evidencePath = $packagePath; $result.evidenceSha256 = ([string]$evidence.packageTreeReceiptSha256).ToLowerInvariant() }
        elseif ($testId -eq 'source_static_tests') { $result.evidencePath = [IO.Path]::GetFullPath($MyInvocation.MyCommand.Path); $result.evidenceSha256 = Get-Sha256File $MyInvocation.MyCommand.Path }
        elseif ($candidateGateEvidence.ContainsKey($testId)) { $result.evidencePath = $candidateGateEvidence[$testId].path; $result.evidenceSha256 = $candidateGateEvidence[$testId].sha256 }
        elseif ($testId -in @('module_boundary_build', 'module_boundary_source') -and $statuses[$testId] -ceq 'PASS') { $result.evidencePath = $producerModuleBoundary.receiptPath; $result.evidenceSha256 = $producerModuleBoundary.receiptSha256; $result.assessmentReceiptPath = $staticGateAssessment.receiptPath; $result.assessmentReceiptSha256 = $staticGateAssessment.receiptSha256 }
        elseif ($statuses[$testId] -ceq 'PASS') { $result.evidencePath = [string]$assessmentById[$testId].evidencePath; $result.evidenceSha256 = [string]$assessmentById[$testId].evidenceSha256; $result.assessmentReceiptPath = $staticGateAssessment.receiptPath; $result.assessmentReceiptSha256 = $staticGateAssessment.receiptSha256 }
        else { $result.reason = 'Required v39-equivalent static gate is not implemented or has not been run for this v43 candidate.' }
        [void]$results.Add([pscustomobject]$result)
    }
    $pending = @($results | Where-Object { $_.status -cne 'PASS' } | ForEach-Object { [string]$_.testId })
    $receipt = [ordered]@{
        schema = 'range.pc11-v43-static-test-receipt/v1'
        status = if ($pending.Count -eq 0) { 'PASS' } else { 'STATIC_INCOMPLETE' }
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        generatedAtUtc = [DateTime]::UtcNow.ToString('o')
        candidateManifestIdentitySha256 = $identitySha
        sourceTreeSha256 = ([string]$sourcePins.sourceTreeSha256).ToLowerInvariant()
        inventoryReportSha256 = ([string]$manifest.installBinding.inventoryReportSha256).ToLowerInvariant()
        packageTreeReceiptSha256 = ([string]$evidence.packageTreeReceiptSha256).ToLowerInvariant()
        requiredTestIds = $script:RequiredTestIds
        results = $results.ToArray()
        passedTestCount = @($results | Where-Object { $_.status -ceq 'PASS' }).Count
        pendingTestIds = $pending
        sourceStaticPowerShellFiles = $parsedScripts.ToArray()
        sourceScopeProbePath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'admission-source-scope-probe-v43.py'))
        sourceScopeProbeSha256 = Get-Sha256File (Join-Path $PSScriptRoot 'admission-source-scope-probe-v43.py')
        harnessPathTest = $harnessPathCheck
        producerAdmissionScopeTest = $sourceScopeContractCheck
        staticGateAssessment = $staticGateAssessment
        preflightGateAssessment = $preflightGateAssessment
        producerModuleBoundary = $producerModuleBoundary
        negativeTests = $negativeTests
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
}
else {
    $receipt = [ordered]@{
        schema = 'range.pc11-v43-static-test-receipt/v1'
        status = 'FAIL'
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        generatedAtUtc = [DateTime]::UtcNow.ToString('o')
        failure = $failure
        requiredTestIds = $script:RequiredTestIds
        results = @()
        passedTestCount = 0
        pendingTestIds = $script:RequiredTestIds
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
}
$parent = [IO.Path]::GetDirectoryName($OutputPath)
if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
$json = $receipt | ConvertTo-Json -Depth 20
[IO.File]::WriteAllText($OutputPath, $json + "`n", (New-Object Text.UTF8Encoding($false)))
Write-Output ("{0} passed={1} pending={2} output={3}" -f $receipt.status, $receipt.passedTestCount, @($receipt.pendingTestIds).Count, $OutputPath)
if ($receipt.status -ceq 'FAIL') { exit 1 }
