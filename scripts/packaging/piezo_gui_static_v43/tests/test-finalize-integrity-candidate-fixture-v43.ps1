[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$ReceiptPath)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'The v43 candidate finalize-integrity fixture requires Windows PowerShell 5.1.'
}
$ReceiptPath = [IO.Path]::GetFullPath($ReceiptPath)
if ([IO.File]::Exists($ReceiptPath)) { throw "Refusing to overwrite single-use fixture receipt: $ReceiptPath" }
$finalizerPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\finalize-v43.ps1'))
$templatePath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\candidate-manifest-v43.admission-template.json'))
$fixtureRoot = [IO.Path]::GetDirectoryName($ReceiptPath)
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..'))
[void][IO.Directory]::CreateDirectory($fixtureRoot)

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
function Write-Json([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path, (($Value | ConvertTo-Json -Depth 30) + "`n"), (New-Object Text.UTF8Encoding($false)))
    return Get-Sha256File $Path
}
function Sort-SourceRowsOrdinal($Rows) {
    $comparer = [System.Collections.Generic.Comparer[object]]::Create([System.Comparison[object]]{
        param($left, $right)
        return [StringComparer]::Ordinal.Compare([string]$left.path, [string]$right.path)
    })
    $ordered = New-Object 'System.Collections.Generic.List[object]'
    foreach ($row in @($Rows)) { [void]$ordered.Add($row) }
    $ordered.Sort($comparer)
    return $ordered.ToArray()
}
function Get-CanonicalSourceTreeSha256($Rows) {
    # Hash the supplied canonical row sequence without culture-sensitive reordering.
    $parts = New-Object 'System.Collections.Generic.List[string]'
    foreach ($row in @($Rows)) {
        $pathJson = ConvertTo-Json -InputObject ([string]$row.path) -Compress
        $shaJson = ConvertTo-Json -InputObject ([string]$row.sha256) -Compress
        $size = ([long]$row.sizeBytes).ToString([Globalization.CultureInfo]::InvariantCulture)
        [void]$parts.Add('{"path":' + $pathJson + ',"sha256":' + $shaJson + ',"sizeBytes":' + $size + '}')
    }
    return Get-Sha256Bytes ((New-Object Text.UTF8Encoding($false, $true)).GetBytes('[' + [string]::Join(',', $parts.ToArray()) + ']'))
}
function Quote-Arg([string]$Value) {
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
function Invoke-Finalizer([string]$ManifestPath, [string]$ManifestSha, [string]$AuditRoot, [string]$OutputPath, [string]$RunId, [string]$MutatePath = '', [string]$MutateText = '') {
    if ($MutatePath) { [IO.File]::WriteAllText($MutatePath, $MutateText, (New-Object Text.UTF8Encoding($false))) }
    $args = @('-NoProfile','-NoLogo','-NonInteractive','-ExecutionPolicy','Bypass','-File',$finalizerPath,
        '-CandidateManifestPath',$ManifestPath,'-CandidateManifestSha256',$ManifestSha,'-AuditRoot',$AuditRoot,'-OutputPath',$OutputPath,'-RunId',$RunId)
    $quoted = @($args | ForEach-Object { Quote-Arg ([string]$_) })
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = [IO.Path]::GetFullPath((Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'))
    $psi.Arguments = [string]::Join(' ', $quoted)
    $psi.WorkingDirectory = $fixtureRoot
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $psi
    try {
        if (-not $process.Start()) { throw 'Could not launch the genuine v43 finalizer fixture child.' }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(180000)) { try { $process.Kill() } catch {}; throw "Genuine finalizer case timed out: $RunId" }
        $process.WaitForExit()
        if (-not [IO.File]::Exists($OutputPath)) { throw "Finalizer emitted no receipt for $RunId; stdout=$($stdoutTask.Result) stderr=$($stderrTask.Result)" }
        $receipt = ((New-Object Text.UTF8Encoding($false, $true)).GetString([IO.File]::ReadAllBytes($OutputPath)) | ConvertFrom-Json -ErrorAction Stop)
        return [pscustomobject]@{ exitCode = [int]$process.ExitCode; stdout = $stdoutTask.Result; stderr = $stderrTask.Result; receipt = $receipt; receiptPath = $OutputPath }
    }
    finally { $process.Dispose() }
}
function Assert-NoGoRuntimeReceipt($Receipt, [string]$Label) {
    if ($Receipt.schema -cne 'range.pc11-v43-final-static-receipt/v1' -or $Receipt.candidateVersion -cne 'v43' -or
        $Receipt.variant -cne 'OFF' -or $Receipt.staticOnly -isnot [bool] -or $Receipt.staticOnly -ne $true -or
        $Receipt.goPermitted -isnot [bool] -or $Receipt.goPermitted -ne $false) {
        throw "$Label is not a v43 OFF static-only no-GO runtime receipt."
    }
    foreach ($name in @('guiLaunched','savePerformed','runPerformed','confirmPerformed')) {
        if ($Receipt.productActions.PSObject.Properties[$name] -eq $null -or $Receipt.productActions.$name -isnot [bool] -or $Receipt.productActions.$name -ne $false) {
            throw "$Label permits product action $name."
        }
    }
}
function Assert-FinalizerRejected([string]$Name, [string]$ManifestPath, [string]$ManifestSha, [string]$AuditRoot, [string]$OutputPath, [string]$MutatePath = '', [string]$MutateText = '') {
    $run = Invoke-Finalizer $ManifestPath $ManifestSha $AuditRoot $OutputPath ('fixture-' + $Name) $MutatePath $MutateText
    Assert-NoGoRuntimeReceipt $run.receipt "Genuine finalizer negative case '$Name'"
    if ($run.exitCode -eq 0 -or $run.receipt.status -cne 'FINALIZE_FAIL') { throw "Genuine finalizer accepted tampered synthetic case: $Name" }
    return [pscustomobject]@{ name = $Name; status = 'PASS'; exitCode = $run.exitCode; failure = [string]$run.receipt.failure; receiptPath = $run.receiptPath; receiptSha256 = Get-Sha256File $run.receiptPath }
}
function New-FixtureEvidence([string]$Root, [string]$Name, $Value) {
    $path = Join-Path $Root ($Name + '.json')
    $sha = Write-Json $path $Value
    return [pscustomobject]@{ path = $path; sha256 = $sha }
}
function New-CompleteSyntheticClosure([string]$Root) {
    [void][IO.Directory]::CreateDirectory($Root)
    $support = [ordered]@{ schema = 'range.pc11-v43-finalizer-synthetic-evidence/v1'; fixtureOnly = $true; goPermitted = $false }
    $inventory = New-FixtureEvidence $Root 'inventory' $support
    $offMap = New-FixtureEvidence $Root 'off-link-map' $support
    $authority = New-FixtureEvidence $Root 'authority' $support
    $review = New-FixtureEvidence $Root 'independent-review' $support
    $decision = New-FixtureEvidence $Root 'root-decision' $support
    $build = New-FixtureEvidence $Root 'release-build' $support
    $ctest = New-FixtureEvidence $Root 'required-ctest' $support
    $preflight = New-FixtureEvidence $Root 'source-preflight' $support
    $cachePath = Join-Path $Root 'CMakeCache.txt'
    [IO.File]::WriteAllText($cachePath, 'fixture-only CMake cache' + [Environment]::NewLine, (New-Object Text.UTF8Encoding($false)))
    $cacheSha = Get-Sha256File $cachePath

    $sourceRows = @(Sort-SourceRowsOrdinal @([pscustomobject][ordered]@{ path = 'src/synthetic.cpp'; sha256 = '4' * 64; sizeBytes = [long]1 }))
    $sourceTree = Get-CanonicalSourceTreeSha256 $sourceRows
    $rootCommit = 'a' * 40
    $freezePath = Join-Path $Root 'source-freeze.json'
    $freeze = [ordered]@{
        schema = 'range.pc11-v43-source-freeze/v1'; status = 'PASS'; candidateVersion = 'v43'; variant = 'OFF'; configuration = 'Release'
        generatedAtUtc = [DateTime]::UtcNow.ToString('o'); sourceRootPath = $repoRoot; sourceScopeSchema = 'range.pc11-v43-source-scope/v1'
        sourceScope = [ordered]@{ enumeration = 'SYNTHETIC_FIXTURE_ONLY'; pathOrder = 'UTF8_BYTE_LEXICOGRAPHIC'; treeDigest = 'SYNTHETIC_FIXTURE_ONLY' }
        rootCommit = $rootCommit; rootWorktreeStatus = 'fixture-only'; rootWorktreeStatusSha256 = (Get-Sha256Bytes ([Text.Encoding]::UTF8.GetBytes('fixture-only')))
        submoduleCommits = @(); sourceFiles = $sourceRows; sourceFileCount = 1; sourceTotalBytes = [long]1; sourceTreeSha256 = $sourceTree
        preBuildSourcePreflightReceiptPath = $preflight.path; preBuildSourcePreflightReceiptSha256 = $preflight.sha256
        postBuildSourcePreflightReceiptPath = $preflight.path; postBuildSourcePreflightReceiptSha256 = $preflight.sha256
        requiredCTestManifestPath = $ctest.path; requiredCTestManifestSha256 = $ctest.sha256
        cmakeCachePath = $cachePath; cmakeCacheSha256 = $cacheSha
        releaseBuildReceiptPath = $build.path; releaseBuildReceiptSha256 = $build.sha256
        staticOnly = $true; productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false; fixtureOnly = $true
    }
    $freezeSha = Write-Json $freezePath $freeze

    $manifest = Get-Content -Raw -LiteralPath $templatePath | ConvertFrom-Json -ErrorAction Stop
    $manifest.candidateState = 'STATIC_CANDIDATE_PENDING_REVIEW_AND_ROOT_DECISION'
    $manifest.installBinding.requiredVariant = 'OFF'
    $manifest.installBinding.installRoot = Join-Path $Root 'synthetic-off-install'
    $manifest.installBinding.inventoryReportPath = $inventory.path
    $manifest.installBinding.inventoryReportSha256 = $inventory.sha256
    $manifest.installBinding.inventorySchemaVersion = 2
    $manifest.installBinding.inventoryStatus = 'PASS'
    $manifest.installBinding.releaseQualificationStatus = 'INCOMPLETE_LINK_MAP_NOT_SUPPLIED'
    $manifest.installedPathLayout.guiExecutableSha256 = '1' * 64
    $manifest.installedPathLayout.privatePythonSha256 = '2' * 64
    $manifest.installedPathLayout.solverExecutableSha256 = '3' * 64
    $manifest.installedPathLayout.productionControlSha256 = '4' * 64
    $manifest.installedPathLayout.nativeBackendSha256 = '5' * 64
    $manifest.sourceAndAuthorityPins.sourceRootPath = $repoRoot
    $manifest.sourceAndAuthorityPins.sourceFreezeReceiptPath = $freezePath
    $manifest.sourceAndAuthorityPins.sourceFreezeReceiptSha256 = $freezeSha
    $manifest.sourceAndAuthorityPins.rootCommit = $rootCommit
    $manifest.sourceAndAuthorityPins.submoduleCommits = @()
    $manifest.sourceAndAuthorityPins.sourceTreeSha256 = $sourceTree
    $manifest.sourceAndAuthorityPins.requiredCTestManifestPath = $ctest.path
    $manifest.sourceAndAuthorityPins.requiredCTestManifestSha256 = $ctest.sha256
    $manifest.sourceAndAuthorityPins.cmakeCachePath = $cachePath
    $manifest.sourceAndAuthorityPins.cmakeCacheSha256 = $cacheSha
    $manifest.sourceAndAuthorityPins.releaseBuildReceiptPath = $build.path
    $manifest.sourceAndAuthorityPins.releaseBuildReceiptSha256 = $build.sha256
    $manifest.sourceAndAuthorityPins.authorityBinding.authorityReceiptPath = $authority.path
    $manifest.sourceAndAuthorityPins.authorityBinding.authorityReceiptSha256 = $authority.sha256
    $manifest.sourceAndAuthorityPins.authorityBinding.additionalOriginalAuthorities = @([ordered]@{ path = $authority.path; sha256 = $authority.sha256 })
    $manifest.candidateEvidencePins.offLinkMapReceiptPath = $offMap.path
    $manifest.candidateEvidencePins.offLinkMapReceiptSha256 = $offMap.sha256

    $packagePath = Join-Path $Root 'package-tree-receipt.json'
    $staticPath = Join-Path $Root 'aggregate-static-receipt.json'
    $manifest.candidateEvidencePins.packageTreeReceiptPath = $packagePath
    $manifest.candidateEvidencePins.packageTreeReceiptSha256 = '0' * 64
    $manifest.candidateEvidencePins.staticTestReceipts = @([ordered]@{ name = 'v43-static-suite'; path = $staticPath; sha256 = ('0' * 64) })
    $manifest.workflowBoundary.independentReviewReceiptPath = $review.path
    $manifest.workflowBoundary.independentReviewReceiptSha256 = $review.sha256
    $manifest.workflowBoundary.rootDecisionReceiptPath = $decision.path
    $manifest.workflowBoundary.rootDecisionReceiptSha256 = $decision.sha256

    $identity = Get-ManifestIdentitySha256 $manifest
    $actions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
    $package = [ordered]@{
        schema = 'range.pc11-v43-package-tree-receipt/v1'; status = 'PASS'; candidateVersion = 'v43'; variant = 'OFF'; staticOnly = $true
        candidateManifestIdentitySha256 = $identity; sourceFreezeReceiptSha256 = $freezeSha; sourceTreeSha256 = $sourceTree
        inventoryReportSha256 = $inventory.sha256; productActions = $actions; goPermitted = $false
    }
    $packageSha = Write-Json $packagePath $package
    $manifest.candidateEvidencePins.packageTreeReceiptSha256 = $packageSha

    $requiredIds = @(
        'source_preflight', 'source_freeze', 'package_tree', 'module_boundary_build', 'module_boundary_source',
        'watchdog_preflight', 'save_predicate_preflight', 'path_shadow', 'root_seal', 'save_predicate',
        'watchdog_contract', 'sampler_arguments', 'supervisor', 'worker_handshake', 'native_launch_boundary',
        'parser', 'finalize_integrity', 'source_static_tests', 'root_bootstrap'
    )
    $results = @($requiredIds | ForEach-Object { [ordered]@{ testId = $_; status = 'PENDING' } })
    $static = [ordered]@{
        schema = 'range.pc11-v43-static-test-receipt/v1'; status = 'STATIC_INCOMPLETE'; candidateVersion = 'v43'; variant = 'OFF'; staticOnly = $true
        candidateManifestIdentitySha256 = $identity; sourceTreeSha256 = $sourceTree; inventoryReportSha256 = $inventory.sha256
        packageTreeReceiptSha256 = $packageSha; requiredTestIds = $requiredIds; results = $results
        productActions = $actions; goPermitted = $false
    }
    $staticSha = Write-Json $staticPath $static
    $manifest.candidateEvidencePins.staticTestReceipts = @([ordered]@{ name = 'v43-static-suite'; path = $staticPath; sha256 = $staticSha })
    $manifestPath = Join-Path $Root 'candidate-manifest.json'
    $manifestSha = Write-Json $manifestPath $manifest
    return [pscustomobject]@{
        root = $Root; manifest = $manifest; manifestPath = $manifestPath; manifestSha256 = $manifestSha
        freezePath = $freezePath; freezeSha256 = $freezeSha; packagePath = $packagePath; packageSha256 = $packageSha
        staticPath = $staticPath; staticSha256 = $staticSha; sourceTreeSha256 = $sourceTree
    }
}

# Load the production finalizer's own identity/hash and no-GO validators into this isolated fixture.
$tokens = $null; $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($finalizerPath, [ref]$tokens, [ref]$parseErrors)
if (@($parseErrors).Count -gt 0) { throw 'The production v43 finalizer does not parse.' }
$extract = @('Get-Sha256Bytes','ConvertTo-JsonString','Get-ManifestIdentitySha256','Assert-JsonObject','Assert-JsonString','Assert-JsonBoolean','Assert-JsonStringArray','Assert-JsonObjectArray','Assert-Sha256Field','Assert-NoGoBoundary')
$functionText = New-Object 'System.Collections.Generic.List[string]'
foreach ($name in $extract) {
    $node = @($ast.FindAll({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq $name }, $true)) | Select-Object -First 1
    if ($null -eq $node) { throw "Production finalizer validator missing: $name" }
    [void]$functionText.Add($node.Extent.Text)
}
Invoke-Expression ([string]::Join([Environment]::NewLine, $functionText.ToArray()))

$positive = New-CompleteSyntheticClosure (Join-Path $fixtureRoot 'synthetic-complete-candidate')
$positiveAuditRoot = Join-Path $positive.root 'audit'
$positiveOutputPath = Join-Path $positiveAuditRoot 'final-static.json'
$positiveRun = Invoke-Finalizer $positive.manifestPath $positive.manifestSha256 $positiveAuditRoot $positiveOutputPath 'fixture-complete-pending-static'
Assert-NoGoRuntimeReceipt $positiveRun.receipt 'Complete synthetic candidate positive case'
if ($positiveRun.exitCode -ne 0 -or $positiveRun.receipt.status -cne 'STATIC_INCOMPLETE' -or
    $positiveRun.receipt.allRequiredStaticTestsPass -ne $false -or @($positiveRun.receipt.pendingTestIds).Count -ne 19) {
    throw "Genuine finalizer did not accept the structurally complete, pending synthetic closure as STATIC_INCOMPLETE. stdout=$($positiveRun.stdout) stderr=$($positiveRun.stderr)"
}

$cases = New-Object 'System.Collections.Generic.List[object]'
[void]$cases.Add((Assert-FinalizerRejected 'incorrect-external-candidate-pin' $positive.manifestPath ('0' * 64) (Join-Path $fixtureRoot 'negative-wrong-pin\audit') (Join-Path $fixtureRoot 'negative-wrong-pin\audit\receipt.json')))

$replayRoot = Join-Path $fixtureRoot 'negative-replayed-candidate'
[void][IO.Directory]::CreateDirectory($replayRoot)
$replayManifest = Join-Path $replayRoot 'candidate.json'
$replaySha = Write-Json $replayManifest $positive.manifest
[void]$cases.Add((Assert-FinalizerRejected 'candidate-bytes-mutated-after-pin' $replayManifest $replaySha (Join-Path $replayRoot 'audit') (Join-Path $replayRoot 'audit\receipt.json') $replayManifest '{"fixture":"replayed-before-use"}'))

$freezeCaseRoot = Join-Path $fixtureRoot 'negative-freeze-mutation'
[void][IO.Directory]::CreateDirectory($freezeCaseRoot)
$freezeCase = New-CompleteSyntheticClosure $freezeCaseRoot
[void]$cases.Add((Assert-FinalizerRejected 'source-freeze-bytes-mutated-after-pin' $freezeCase.manifestPath $freezeCase.manifestSha256 (Join-Path $freezeCaseRoot 'audit') (Join-Path $freezeCaseRoot 'audit\receipt.json') $freezeCase.freezePath '{"schema":"range.pc11-v43-source-freeze/v1","status":"PASS","goPermitted":true}'))

$packageCaseRoot = Join-Path $fixtureRoot 'negative-package-mutation'
[void][IO.Directory]::CreateDirectory($packageCaseRoot)
$packageCase = New-CompleteSyntheticClosure $packageCaseRoot
[void]$cases.Add((Assert-FinalizerRejected 'package-tree-bytes-mutated-after-pin' $packageCase.manifestPath $packageCase.manifestSha256 (Join-Path $packageCaseRoot 'audit') (Join-Path $packageCaseRoot 'audit\receipt.json') $packageCase.packagePath '{"fixture":"tampered-package"}'))

$identityCaseRoot = Join-Path $fixtureRoot 'negative-package-identity'
$identityCase = New-CompleteSyntheticClosure $identityCaseRoot
$badPackage = Get-Content -Raw -LiteralPath $identityCase.packagePath | ConvertFrom-Json -ErrorAction Stop
$badPackage.candidateManifestIdentitySha256 = 'f' * 64
$identityCase.packageSha256 = Write-Json $identityCase.packagePath $badPackage
$identityCase.manifest.candidateEvidencePins.packageTreeReceiptSha256 = $identityCase.packageSha256
$identityCase.manifestSha256 = Write-Json $identityCase.manifestPath $identityCase.manifest
[void]$cases.Add((Assert-FinalizerRejected 'package-candidate-identity-mismatch' $identityCase.manifestPath $identityCase.manifestSha256 (Join-Path $identityCaseRoot 'audit') (Join-Path $identityCaseRoot 'audit\receipt.json')))

$staticCaseRoot = Join-Path $fixtureRoot 'negative-static-duplicate'
$staticCase = New-CompleteSyntheticClosure $staticCaseRoot
$badStatic = Get-Content -Raw -LiteralPath $staticCase.staticPath | ConvertFrom-Json -ErrorAction Stop
$badStatic.results[1].testId = $badStatic.results[0].testId
$staticCase.staticSha256 = Write-Json $staticCase.staticPath $badStatic
$staticCase.manifest.candidateEvidencePins.staticTestReceipts[0].sha256 = $staticCase.staticSha256
$staticCase.manifestSha256 = Write-Json $staticCase.manifestPath $staticCase.manifest
[void]$cases.Add((Assert-FinalizerRejected 'aggregate-static-duplicate-id' $staticCase.manifestPath $staticCase.manifestSha256 (Join-Path $staticCaseRoot 'audit') (Join-Path $staticCaseRoot 'audit\receipt.json')))

$fixtureReceipt = [ordered]@{
    schema = 'range.pc11-v43-finalize-integrity-fixture/v1'
    status = 'PASS'
    testedFinalizerPath = $finalizerPath
    syntheticCompleteCandidate = [ordered]@{
        status = 'PASS'; staticReceiptStatus = [string]$positiveRun.receipt.status
        allRequiredStaticTestsPass = [bool]$positiveRun.receipt.allRequiredStaticTestsPass
        pendingTestCount = @($positiveRun.receipt.pendingTestIds).Count
        finalizerReceiptPath = [IO.Path]::GetFullPath($positiveRun.receiptPath)
        finalizerReceiptSha256 = Get-Sha256File $positiveRun.receiptPath
        candidateManifestSha256 = $positive.manifestSha256; sourceFreezeReceiptSha256 = $positive.freezeSha256
        sourceTreeSha256 = $positive.sourceTreeSha256; staticOnly = $true; goPermitted = $false
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
    }
    rejectedCases = @($cases.ToArray() | ForEach-Object { $_.name })
    finalizerNegativeCases = @($cases.ToArray())
    productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
    goPermitted = $false
    fixtureOnly = $true
}
[void](Write-Json $ReceiptPath $fixtureReceipt)
Write-Output ("PASS genuine finalize-v43 completeSynthetic={0} rejectedCases={1} runtimeStatus={2} goPermitted=false" -f
    $fixtureReceipt.syntheticCompleteCandidate.status, $cases.Count, $fixtureReceipt.syntheticCompleteCandidate.staticReceiptStatus)
