[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'The candidate-gate aggregate unit test requires Windows PowerShell 5.1.'
}

$packageRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoRoot = [IO.Path]::GetFullPath((Join-Path $packageRoot '..\..\..'))
$auditRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit'))
$fixtureRoot = Join-Path $auditRoot ('candidate-gate-aggregate-v43-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($fixtureRoot)
$receiptPath = Join-Path $fixtureRoot 'aggregate-test-receipt.json'
if ([IO.File]::Exists($receiptPath)) { throw "Refusing to overwrite test receipt: $receiptPath" }

function Get-FixtureSha256Bytes([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Get-FixtureSha256File([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}
function Copy-FixtureJsonObject($Value) {
    $json = $Value | Microsoft.PowerShell.Utility\ConvertTo-Json -Depth 12 -Compress
    return Microsoft.PowerShell.Utility\ConvertFrom-Json -InputObject $json -ErrorAction Stop
}
function Assert-ForgedCandidateGateReceiptRejected($Receipt, [string]$Label, [string]$Schema, [string]$ManifestSha, [string]$FreezeSha, [string]$TreeSha, [string]$IdentitySha) {
    $rejected = $false
    try { Assert-CandidateGateReceipt $Receipt $Schema $ManifestSha $FreezeSha $TreeSha $IdentitySha }
    catch { $rejected = $true }
    if (-not $rejected) { throw "Candidate gate accepted forged receipt case: $Label" }
}
function Assert-FrozenSourceRejectedBeforeChild([string]$Label, [scriptblock]$Invocation) {
    $before = $script:childInvocationCount
    $rejected = $false
    try { $null = & $Invocation }
    catch { $rejected = $true }
    if (-not $rejected) { throw "Frozen candidate gate accepted source case: $Label" }
    if ($script:childInvocationCount -ne $before) { throw "Frozen candidate gate invoked its child before rejecting source case: $Label" }
}

$productionScript = Join-Path $PSScriptRoot 'test-static-chain-v43.ps1'
$tokens = $null
$parseErrors = $null
$productionAst = [Management.Automation.Language.Parser]::ParseFile($productionScript, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw 'The production static-chain script did not parse cleanly.' }
$functionNames = @(
    'Assert-JsonObjectValue', 'Assert-JsonProperty', 'Assert-JsonString', 'Assert-JsonBoolean',
    'Assert-JsonObjectProperty', 'Assert-ProductActions', 'Assert-NoGoBoundary',
    'Get-Sha256Bytes', 'Read-JsonSnapshot', 'Read-Json',
    'Quote-WindowsArgument', 'Invoke-ChildPowerShell',
    'Assert-CandidateGateReceipt', 'Invoke-FrozenCandidateGate'
)
foreach ($functionName in $functionNames) {
    $definitions = @($productionAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $functionName }, $true))
    if ($definitions.Count -eq 0) { throw "Production function is missing from AST: $functionName" }
    $functionText = $definitions[$definitions.Count - 1].Extent.Text
    foreach ($definition in $definitions) {
        if ($definition.Extent.Text -cne $functionText) { throw "Production helper has conflicting definitions: $functionName" }
    }
    . ([scriptblock]::Create($functionText))
}

$schema = 'range.pc11-v43-aggregate-candidate-gate/v1'
$manifestSha = '1' * 64
$freezeSha = '2' * 64
$treeSha = '3' * 64
$identitySha = '8' * 64
$validReceipt = Copy-FixtureJsonObject ([ordered]@{
    schema = $schema
    status = 'PASS'
    candidateGateStatus = 'PASS'
    candidateManifestSha256 = $manifestSha
    sourceFreezeReceiptSha256 = $freezeSha
    sourceTreeSha256 = $treeSha
    candidateManifestIdentitySha256 = $identitySha
    candidateVersion = 'v43'
    variant = 'OFF'
    staticOnly = $true
    productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
    goPermitted = $false
})
$caseNames = New-Object 'System.Collections.Generic.List[string]'
$controlArgumentAssignments = @($productionAst.FindAll({ param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and
    $node.Left.Extent.Text -ceq '$controlArgs' -and
    $node.Operator -ceq [Management.Automation.Language.TokenKind]::PlusEquals
}, $true))
if ($controlArgumentAssignments.Count -ne 1) { throw 'Expected one production candidate-control argument append expression.' }
$componentArguments = @(
    [pscustomobject]@{ pathParam='SavePredicateReceiptPath'; shaParam='SavePredicateReceiptSha256'; path=(Join-Path $fixtureRoot 'save predicate receipt.json'); sha=('a' * 64) },
    [pscustomobject]@{ pathParam='SamplerArgumentsReceiptPath'; shaParam='SamplerArgumentsReceiptSha256'; path=(Join-Path $fixtureRoot 'sampler arguments receipt.json'); sha=('b' * 64) },
    [pscustomobject]@{ pathParam='SupervisorWorkerReceiptPath'; shaParam='SupervisorWorkerReceiptSha256'; path=(Join-Path $fixtureRoot 'watchdog contract receipt.json'); sha=('c' * 64) }
)
$controlArgs = @()
foreach ($component in $componentArguments) {
    $pathValue = [string]$component.path
    $shaValue = [string]$component.sha
    . ([scriptblock]::Create($controlArgumentAssignments[0].Extent.Text))
}
$expectedControlArgs = @(
    '-SavePredicateReceiptPath', [string]$componentArguments[0].path,
    '-SavePredicateReceiptSha256', [string]$componentArguments[0].sha,
    '-SamplerArgumentsReceiptPath', [string]$componentArguments[1].path,
    '-SamplerArgumentsReceiptSha256', [string]$componentArguments[1].sha,
    '-SupervisorWorkerReceiptPath', [string]$componentArguments[2].path,
    '-SupervisorWorkerReceiptSha256', [string]$componentArguments[2].sha
)
if ($controlArgs.Count -ne 12 -or $expectedControlArgs.Count -ne 12) { throw 'Production candidate-control assembly did not yield exactly twelve tokens for three components.' }
for ($argumentIndex = 0; $argumentIndex -lt $expectedControlArgs.Count; $argumentIndex++) {
    if ([string]$controlArgs[$argumentIndex] -cne [string]$expectedControlArgs[$argumentIndex]) {
        throw "Production candidate-control argument token $argumentIndex differs from the exact expected token."
    }
}
[void]$caseNames.Add('production-candidate-control-ast-assembles-twelve-exact-component-tokens')
$candidateManifestPath = Join-Path $fixtureRoot 'invoker-context\candidate.json'
[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($candidateManifestPath))
$CandidateManifestPath = $candidateManifestPath
$childStubPath = Join-Path $fixtureRoot 'bind-component-parameters.ps1'
$childStub = @'
param(
    [Parameter(Mandatory = $true)][string]$SavePredicateReceiptPath,
    [Parameter(Mandatory = $true)][string]$SavePredicateReceiptSha256,
    [Parameter(Mandatory = $true)][string]$SamplerArgumentsReceiptPath,
    [Parameter(Mandatory = $true)][string]$SamplerArgumentsReceiptSha256,
    [Parameter(Mandatory = $true)][string]$SupervisorWorkerReceiptPath,
    [Parameter(Mandatory = $true)][string]$SupervisorWorkerReceiptSha256
)
[ordered]@{
    SavePredicateReceiptPath = $SavePredicateReceiptPath
    SavePredicateReceiptSha256 = $SavePredicateReceiptSha256
    SamplerArgumentsReceiptPath = $SamplerArgumentsReceiptPath
    SamplerArgumentsReceiptSha256 = $SamplerArgumentsReceiptSha256
    SupervisorWorkerReceiptPath = $SupervisorWorkerReceiptPath
    SupervisorWorkerReceiptSha256 = $SupervisorWorkerReceiptSha256
} | ConvertTo-Json -Compress
'@
[IO.File]::WriteAllText($childStubPath, $childStub, (New-Object Text.UTF8Encoding($false)))
$boundChild = Invoke-ChildPowerShell (@('-File', $childStubPath) + $controlArgs)
if ($boundChild.exitCode -ne 0) { throw "Windows PowerShell 5.1 child stub rejected production argument tokens: $($boundChild.stderr.Trim())" }
$boundParameters = $boundChild.stdout | ConvertFrom-Json -ErrorAction Stop
$childBoundNames = @('SavePredicateReceiptPath','SavePredicateReceiptSha256','SamplerArgumentsReceiptPath','SamplerArgumentsReceiptSha256','SupervisorWorkerReceiptPath','SupervisorWorkerReceiptSha256')
$childBoundExpectedValues = @(
    [string]$componentArguments[0].path, [string]$componentArguments[0].sha,
    [string]$componentArguments[1].path, [string]$componentArguments[1].sha,
    [string]$componentArguments[2].path, [string]$componentArguments[2].sha
)
for ($parameterIndex = 0; $parameterIndex -lt $childBoundNames.Count; $parameterIndex++) {
    $parameterName = $childBoundNames[$parameterIndex]
    $actualValue = [string]$boundParameters.PSObject.Properties[$parameterName].Value
    if ($actualValue -cne [string]$childBoundExpectedValues[$parameterIndex]) {
        throw "Windows PowerShell 5.1 child bound an unexpected value for $parameterName."
    }
}
[void]$caseNames.Add('windows-powershell-51-child-binds-six-required-component-parameters')
$pathShadowScript = Join-Path $PSScriptRoot 'test-path-shadow-v43.ps1'
$pathShadowTokens = $null
$pathShadowParseErrors = $null
$pathShadowAst = [Management.Automation.Language.Parser]::ParseFile($pathShadowScript, [ref]$pathShadowTokens, [ref]$pathShadowParseErrors)
if ($pathShadowParseErrors.Count -ne 0) { throw 'The production path-shadow script did not parse cleanly.' }
$pathShadowReceiptAssignments = @($pathShadowAst.FindAll({ param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -ceq '$receipt'
}, $true))
if ($pathShadowReceiptAssignments.Count -ne 1) { throw 'Expected one production path-shadow receipt constructor.' }
$readinessScript = Join-Path $PSScriptRoot 'test-final-readiness-candidate-v43.ps1'
$readinessTokens = $null
$readinessParseErrors = $null
$readinessAst = [Management.Automation.Language.Parser]::ParseFile($readinessScript, [ref]$readinessTokens, [ref]$readinessParseErrors)
if ($readinessParseErrors.Count -ne 0) { throw 'The production final-readiness script did not parse cleanly.' }
$actionAssignments = @($readinessAst.FindAll({ param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -ceq '$script:Actions'
}, $true))
if ($actionAssignments.Count -ne 1) { throw 'Expected one production final-readiness product-action list.' }
. ([scriptblock]::Create($actionAssignments[0].Extent.Text))
foreach ($functionName in @('Assert-JsonObject','Assert-String','Assert-Boolean','Assert-ProductActions','Assert-NoGo')) {
    $definitions = @($readinessAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $functionName }, $true))
    if ($definitions.Count -ne 1) { throw "Expected one production final-readiness helper: $functionName" }
    . ([scriptblock]::Create($definitions[0].Extent.Text))
}
$status = 'STATIC_INCOMPLETE'
$failure = $null
$finalSourceHashes = @([pscustomobject][ordered]@{ route='synthetic'; path='synthetic-source.ps1'; sha256=('d' * 64) })
$rows = New-Object 'System.Collections.Generic.List[object]'
for ($routeIndex = 1; $routeIndex -le 5; $routeIndex++) {
    $rows.Add([pscustomobject][ordered]@{ route=('synthetic_route_' + $routeIndex); status='PASS'; fixtureOnly=$true })
}
$pending = @([pscustomobject][ordered]@{ route='root_bootstrap_external_entry'; status='PENDING'; reason='Synthetic route retained pending.' })
. ([scriptblock]::Create($pathShadowReceiptAssignments[0].Extent.Text))
$pathShadowReceipt = $receipt
Assert-NoGo $pathShadowReceipt 'Production path-shadow receipt constructor'
if ($pathShadowReceipt.schema -cne 'range.pc11-v43-path-shadow-receipt/v1' -or
    $pathShadowReceipt.status -cne 'STATIC_INCOMPLETE' -or @($pathShadowReceipt.testedRoutes).Count -ne 5 -or
    @($pathShadowReceipt.testedRoutes | Where-Object { $_.status -cne 'PASS' }).Count -ne 0 -or
    @($pathShadowReceipt.pendingRoutes).Count -ne 1 -or
    $pathShadowReceipt.pendingRoutes[0].route -cne 'root_bootstrap_external_entry' -or
    $pathShadowReceipt.pendingRoutes[0].status -cne 'PENDING') {
    throw 'Production path-shadow constructor did not preserve five PASS routes and the one explicit pending route.'
}
[void]$caseNames.Add('production-path-shadow-receipt-constructor-passes-real-final-readiness-no-go-guard')
$missingVariantPathShadow = Copy-FixtureJsonObject $pathShadowReceipt
$missingVariantPathShadow.PSObject.Properties.Remove('variant')
$missingVariantRejected = $false
try { Assert-NoGo $missingVariantPathShadow 'Path-shadow missing-variant regression' }
catch { $missingVariantRejected = $_.Exception.Message -ceq 'Path-shadow missing-variant regression.variant must be a non-empty JSON string.' }
if (-not $missingVariantRejected) { throw 'Production final-readiness Assert-NoGo did not reject a missing path-shadow variant.' }
[void]$caseNames.Add('production-final-readiness-no-go-guard-rejects-missing-path-shadow-variant')
$boundaryConstructors = @($productionAst.FindAll({ param($node)
    $node -is [Management.Automation.Language.AssignmentStatementAst] -and
    $node.Left.Extent.Text -ceq '$producerBoundaryReceipt'
}, $true))
if ($boundaryConstructors.Count -ne 1) { throw 'Expected one production module-boundary receipt constructor.' }
$sourcePins = [pscustomobject]@{ sourceTreeSha256 = $treeSha }
$producerBoundarySources = New-Object 'System.Collections.Generic.List[object]'
$producerBoundaryRun = [pscustomobject]@{ stdout = 'PASS producer-module-boundary cases=24' }
. ([scriptblock]::Create($boundaryConstructors[0].Extent.Text))
Assert-NoGoBoundary $producerBoundaryReceipt 'Production module-boundary receipt constructor'
[void]$caseNames.Add('production-module-boundary-constructor-preserves-strict-json-types')
$negativeRoot = $fixtureRoot
$freeze = [pscustomobject]@{ sourceRootPath = $repoRoot }
$savedUnitRepoRoot = $repoRoot
Remove-Variable -Name repoRoot -Scope Script
foreach ($assignmentName in @('$saveEnvironmentRoot', '$saveEnvironmentPath')) {
    $assignmentNodes = @($productionAst.FindAll({ param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left.Extent.Text -ceq $assignmentName
    }, $true))
    if ($assignmentNodes.Count -ne 1) { throw "Expected one production assignment for $assignmentName." }
    . ([scriptblock]::Create($assignmentNodes[0].Extent.Text))
}
$repoRoot = $savedUnitRepoRoot
if (-not $saveEnvironmentPath.StartsWith($auditRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Production Save-environment output escaped its canonical external audit root.'
}
[void]$caseNames.Add('save-environment-output-respects-canonical-root-with-short-caller-audit')
Assert-CandidateGateReceipt $validReceipt $schema $manifestSha $freezeSha $treeSha $identitySha
[void]$caseNames.Add('valid-pass-no-go-receipt-accepted')

$forgedCases = @(
    [pscustomobject]@{ name = 'wrong-canonical-identity'; mutate = { param($value) $value.candidateManifestIdentitySha256 = ('9' * 64) } },
    [pscustomobject]@{ name = 'array-canonical-identity'; mutate = { param($value) $value.candidateManifestIdentitySha256 = @($identitySha) } },
    [pscustomobject]@{ name = 'wrong-schema'; mutate = { param($value) $value.schema = 'range.wrong/v1' } },
    [pscustomobject]@{ name = 'wrong-manifest-pin'; mutate = { param($value) $value.candidateManifestSha256 = ('4' * 64) } },
    [pscustomobject]@{ name = 'wrong-freeze-pin'; mutate = { param($value) $value.sourceFreezeReceiptSha256 = ('4' * 64) } },
    [pscustomobject]@{ name = 'wrong-tree-pin'; mutate = { param($value) $value.sourceTreeSha256 = ('4' * 64) } },
    [pscustomobject]@{ name = 'fixture-pending'; mutate = { param($value) $value.status = 'PENDING'; $value.candidateGateStatus = 'PENDING' } },
    [pscustomobject]@{ name = 'array-go-boolean'; mutate = { param($value) $value.goPermitted = [object[]]@($false) } },
    [pscustomobject]@{ name = 'string-go-boolean'; mutate = { param($value) $value.goPermitted = 'false' } },
    [pscustomobject]@{ name = 'array-static-only-boolean'; mutate = { param($value) $value.staticOnly = [object[]]@($true) } },
    [pscustomobject]@{ name = 'array-schema-string'; mutate = { param($value) $value.schema = [object[]]@($schema) } },
    [pscustomobject]@{ name = 'product-action-enabled'; mutate = { param($value) $value.productActions.savePerformed = $true } },
    [pscustomobject]@{ name = 'product-action-string-boolean'; mutate = { param($value) $value.productActions.guiLaunched = 'false' } },
    [pscustomobject]@{ name = 'go-enabled'; mutate = { param($value) $value.goPermitted = $true } }
)
foreach ($forgedCase in $forgedCases) {
    $candidate = Copy-FixtureJsonObject $validReceipt
    & $forgedCase.mutate $candidate
    Assert-ForgedCandidateGateReceiptRejected $candidate $forgedCase.name $schema $manifestSha $freezeSha $treeSha $identitySha
    [void]$caseNames.Add(('forged-' + $forgedCase.name + '-rejected'))
}

$script:childInvocationCount = 0
$script:stubGateReceipt = $validReceipt
$script:discoveredFixturePath = $null
$script:receiptLineEnding = "`n"
function Invoke-ChildPowerShell([string[]]$ScriptArguments) {
    $script:childInvocationCount++
    $outputIndex = [Array]::IndexOf($ScriptArguments, '-OutputPath')
    if ($outputIndex -lt 0 -and $null -ne $script:discoveredFixturePath) {
        [IO.File]::WriteAllText($script:discoveredFixturePath, (($script:stubGateReceipt | Microsoft.PowerShell.Utility\ConvertTo-Json -Depth 12) + "`n"), (New-Object Text.UTF8Encoding($false)))
        return [pscustomobject]@{ exitCode=0; stdout=('PASS receipt=' + $script:discoveredFixturePath + $script:receiptLineEnding); stderr='' }
    }
    if ($outputIndex -lt 0 -or $outputIndex + 1 -ge $ScriptArguments.Length) { throw 'Stub child did not receive an OutputPath argument.' }
    $childReceiptPath = [string]$ScriptArguments[$outputIndex + 1]
    [IO.File]::WriteAllText($childReceiptPath, (($script:stubGateReceipt | Microsoft.PowerShell.Utility\ConvertTo-Json -Depth 12) + "`n"), (New-Object Text.UTF8Encoding($false)))
    return [pscustomobject]@{ exitCode = 0; stdout = 'PASS stubbed child'; stderr = '' }
}

$sourceRoot = Join-Path $fixtureRoot 'synthetic-frozen-source'
$gateScriptPath = Join-Path $sourceRoot 'tests\gate.ps1'
$dependencyPath = Join-Path $sourceRoot 'lib\frozen-dependency.ps1'
[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($gateScriptPath))
[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($dependencyPath))
[IO.File]::WriteAllText($gateScriptPath, '# synthetic gate source', (New-Object Text.UTF8Encoding($false)))
[IO.File]::WriteAllText($dependencyPath, '# frozen dependency v1', (New-Object Text.UTF8Encoding($false)))
$script:sourceRootPrefix = [IO.Path]::GetFullPath($sourceRoot).TrimEnd('\') + '\'
$script:snapshotRows = @{
    'tests/gate.ps1' = Get-FixtureSha256File $gateScriptPath
    'lib/frozen-dependency.ps1' = Get-FixtureSha256File $dependencyPath
}
$invokeReceiptPath = Join-Path $fixtureRoot 'valid-gate-receipt.json'
$invokedReceipt = Invoke-FrozenCandidateGate $gateScriptPath @('-SyntheticMode') @($dependencyPath) $invokeReceiptPath
Assert-CandidateGateReceipt $invokedReceipt $schema $manifestSha $freezeSha $treeSha $identitySha
if ($script:childInvocationCount -ne 1) { throw 'Valid frozen source did not invoke the child runner exactly once.' }
[void]$caseNames.Add('valid-frozen-sources-reach-stubbed-child-and-return-pinned-receipt')
foreach ($lineCase in @([pscustomobject]@{name='lf'; ending="`n"}, [pscustomobject]@{name='crlf'; ending="`r`n"})) {
    $script:discoveredFixturePath = Join-Path $fixtureRoot ('original-' + $lineCase.name + '.json')
    $script:receiptLineEnding = $lineCase.ending
    $discoveredCopy = Join-Path $fixtureRoot ('discovered-' + $lineCase.name + '.json')
    $discoveredReceipt = Invoke-FrozenCandidateGate $gateScriptPath @('-SyntheticMode') @($dependencyPath) $discoveredCopy -DiscoverReceipt
    Assert-CandidateGateReceipt $discoveredReceipt $schema $manifestSha $freezeSha $treeSha $identitySha
    if ((Get-FixtureSha256File $discoveredCopy) -cne (Get-FixtureSha256File $script:discoveredFixturePath)) { throw 'Receipt discovery changed original bytes.' }
    [void]$caseNames.Add(('receipt-discovery-' + $lineCase.name + '-preserves-original-bytes'))
}
$script:discoveredFixturePath = $null

$unpinnedPath = Join-Path $sourceRoot 'lib\not-in-freeze.ps1'
[IO.File]::WriteAllText($unpinnedPath, '# source exists but has no frozen row', (New-Object Text.UTF8Encoding($false)))
Assert-FrozenSourceRejectedBeforeChild 'missing-frozen-row' {
    Invoke-FrozenCandidateGate $gateScriptPath @('-SyntheticMode') @($unpinnedPath) (Join-Path $fixtureRoot 'missing-row-receipt.json')
}
[void]$caseNames.Add('missing-frozen-source-row-rejected-before-child')

$missingPath = Join-Path $sourceRoot 'lib\missing-on-disk.ps1'
$script:snapshotRows['lib/missing-on-disk.ps1'] = Get-FixtureSha256Bytes ([Text.Encoding]::UTF8.GetBytes('# expected but absent'))
Assert-FrozenSourceRejectedBeforeChild 'missing-file' {
    Invoke-FrozenCandidateGate $gateScriptPath @('-SyntheticMode') @($missingPath) (Join-Path $fixtureRoot 'missing-file-receipt.json')
}
[void]$caseNames.Add('missing-source-file-rejected-before-child')

[IO.File]::WriteAllText($dependencyPath, '# frozen dependency mutated after snapshot', (New-Object Text.UTF8Encoding($false)))
Assert-FrozenSourceRejectedBeforeChild 'mutated-source' {
    Invoke-FrozenCandidateGate $gateScriptPath @('-SyntheticMode') @($dependencyPath) (Join-Path $fixtureRoot 'mutated-source-receipt.json')
}
[void]$caseNames.Add('mutated-frozen-source-rejected-before-child')
if ($script:childInvocationCount -ne 3) { throw 'A missing or mutated source reached the stubbed child runner.' }

$fixtureReceipt = [ordered]@{
    schema = 'range.pc11-v43-candidate-gate-aggregate-unit-test/v1'
    status = 'PASS'
    cases = $caseNames.ToArray()
    candidateGateProbeScope = 'AST-extracted production helpers, candidate-control and path-shadow receipt constructors; actual final-readiness no-GO guard; Windows PowerShell 5.1 childstub binds six component parameters; child runner is stubbed only for frozen-source unit checks'
    productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
    goPermitted = $false
}
[IO.File]::WriteAllText($receiptPath, (($fixtureReceipt | Microsoft.PowerShell.Utility\ConvertTo-Json -Depth 12) + "`n"), (New-Object Text.UTF8Encoding($false)))
Write-Output ("PASS cases={0} childInvocations={1} receipt={2}" -f $caseNames.Count, $script:childInvocationCount, $receiptPath)
