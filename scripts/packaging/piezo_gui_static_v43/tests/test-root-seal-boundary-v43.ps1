[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$CandidateManifestPath,
    [string]$CandidateManifestSha256,
    [string]$SourceFreezeReceiptPath,
    [string]$SourceFreezeReceiptSha256
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'The v43 root-seal boundary fixture requires Windows PowerShell 5.1.'
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if ([IO.File]::Exists($OutputPath)) { throw "Refusing to overwrite single-use root-seal evidence: $OutputPath" }
$provided = @($CandidateManifestPath, $CandidateManifestSha256, $SourceFreezeReceiptPath, $SourceFreezeReceiptSha256 |
    Where-Object { -not [string]::IsNullOrWhiteSpace($_) }).Count
if ($provided -ne 0 -and $provided -ne 4) { throw 'Candidate binding requires all four manifest and source-freeze path/hash pins together.' }

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..'))
$prepRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$fixturePath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'test-root-seal-boundary-v43-fixture.ps1'))
$bootstrapPath = [IO.Path]::GetFullPath((Join-Path $prepRoot 'runtime\root-bootstrap-v43.ps1'))
$bootstrapTestPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'test-root-bootstrap-v43.ps1'))
$finalizerPath = [IO.Path]::GetFullPath((Join-Path $prepRoot 'finalize-v43.ps1'))
$templatePath = [IO.Path]::GetFullPath((Join-Path $prepRoot 'candidate-manifest-v43.admission-template.json'))
$tempRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('range-fea-v43-root-seal-' + [Guid]::NewGuid().ToString('N'))))
[void][IO.Directory]::CreateDirectory($tempRoot)

function Get-Sha256Bytes([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Get-Sha256File([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { throw "Required file is missing: $Path" }
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
    # Freeze rows are already in producer order; only synthetic constructors sort their input.
    $parts = New-Object 'System.Collections.Generic.List[string]'
    foreach ($row in @($Rows)) {
        $pathJson = ConvertTo-Json -InputObject ([string]$row.path) -Compress
        $shaJson = ConvertTo-Json -InputObject ([string]$row.sha256) -Compress
        $size = ([long]$row.sizeBytes).ToString([Globalization.CultureInfo]::InvariantCulture)
        [void]$parts.Add('{"path":' + $pathJson + ',"sha256":' + $shaJson + ',"sizeBytes":' + $size + '}')
    }
    return Get-Sha256Bytes ((New-Object Text.UTF8Encoding($false, $true)).GetBytes('[' + [string]::Join(',', $parts.ToArray()) + ']'))
}
function Assert-CanonicalSourceTreeGoldenVector {
    $paths = @('src/A.cpp', 'src/a-b.cpp', 'src/a.cpp', 'src/a_b.cpp',
        ('src/caf' + [char]0x00e9 + '.cpp'), "src/o'neil.cpp", ('src/' + [char]0x00e9 + 'clair.cpp'))
    $rows = New-Object 'System.Collections.Generic.List[object]'
    for ($index = 0; $index -lt $paths.Count; $index++) {
        $number = $index + 1
        [void]$rows.Add([pscustomobject][ordered]@{
            path = $paths[$index]
            sha256 = ('{0:x2}' -f $number) * 32
            sizeBytes = [long]$number
        })
    }
    $ordered = @(Sort-SourceRowsOrdinal $rows.ToArray())
    $expectedPaths = @('src/A.cpp', 'src/a-b.cpp', 'src/a.cpp', 'src/a_b.cpp',
        ('src/caf' + [char]0x00e9 + '.cpp'), "src/o'neil.cpp", ('src/' + [char]0x00e9 + 'clair.cpp'))
    if ($ordered.Count -ne $expectedPaths.Count) { throw 'Canonical source-tree golden vector count changed.' }
    for ($index = 0; $index -lt $expectedPaths.Count; $index++) {
        if ([string]$ordered[$index].path -cne [string]$expectedPaths[$index]) {
            throw 'Canonical source-tree golden vector is not ordered by ordinal path comparison.'
        }
    }
    # Windows PowerShell 5.1 escapes the apostrophe as \u0027, matching the source-preflight producer.
    if ((Get-CanonicalSourceTreeSha256 $ordered) -cne '4a8f4b687fd7f0d3e5bd8c9577898fa9cc4065c2f918033f5baf05857fabed55') {
        throw 'Canonical source-tree golden vector digest changed.'
    }
}
Assert-CanonicalSourceTreeGoldenVector
function Get-ManifestIdentitySha256($Manifest) {
    $source = $Manifest.sourceAndAuthorityPins
    $binding = $Manifest.installBinding
    $parts = @(
        '{"candidateVersion":"v43"',
        ',"installRoot":' + (ConvertTo-Json -InputObject ([string]$binding.installRoot) -Compress),
        ',"inventoryReportSha256":' + (ConvertTo-Json -InputObject ([string]$binding.inventoryReportSha256).ToLowerInvariant() -Compress),
        ',"releaseBuildReceiptSha256":' + (ConvertTo-Json -InputObject ([string]$source.releaseBuildReceiptSha256).ToLowerInvariant() -Compress),
        ',"rootCommit":' + (ConvertTo-Json -InputObject ([string]$source.rootCommit).ToLowerInvariant() -Compress),
        ',"schema":"range.pc11-fresh-run-v43-admission-candidate/v1"',
        ',"sourceFreezeReceiptSha256":' + (ConvertTo-Json -InputObject ([string]$source.sourceFreezeReceiptSha256).ToLowerInvariant() -Compress),
        ',"sourceTreeSha256":' + (ConvertTo-Json -InputObject ([string]$source.sourceTreeSha256).ToLowerInvariant() -Compress) + '}'
    )
    return Get-Sha256Bytes ((New-Object Text.UTF8Encoding($false, $true)).GetBytes([string]::Join('', $parts)))
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
function Invoke-PowerShellChild([string]$ScriptPath, [string[]]$Arguments, [string]$WorkingDirectory) {
    $powerShellPath = [IO.Path]::GetFullPath((Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'))
    if (-not [IO.File]::Exists($powerShellPath)) { throw 'Windows PowerShell 5.1 executable is unavailable.' }
    $allArguments = @('-NoProfile', '-NoLogo', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $ScriptPath) + $Arguments
    $quoted = @($allArguments | ForEach-Object { Quote-WindowsArgument ([string]$_) })
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $powerShellPath
    $info.Arguments = [string]::Join(' ', $quoted)
    $info.WorkingDirectory = $WorkingDirectory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    try {
        if (-not $process.Start()) { throw "Could not start isolated PowerShell child for $ScriptPath." }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(300000)) { try { $process.Kill() } catch {}; throw "Isolated PowerShell child timed out: $ScriptPath" }
        $process.WaitForExit()
        return [pscustomobject]@{ exitCode = [int]$process.ExitCode; stdout = $stdoutTask.Result; stderr = $stderrTask.Result }
    }
    finally { $process.Dispose() }
}
function Invoke-BoundaryFixture([string]$ManifestPath, [string]$ManifestSha, [string]$FreezePath, [string]$FreezeSha) {
    return Invoke-PowerShellChild $fixturePath @(
        '-CandidateManifestPath', $ManifestPath,
        '-CandidateManifestSha256', $ManifestSha,
        '-SourceFreezeReceiptPath', $FreezePath,
        '-SourceFreezeReceiptSha256', $FreezeSha
    ) $tempRoot
}
function Write-FixtureEvidence([string]$Name, $Value) {
    $path = Join-Path $tempRoot ($Name + '.json')
    $sha = Write-Json $path $Value
    return [pscustomobject]@{ path = $path; sha256 = $sha }
}
function New-SyntheticCandidatePair {
    $support = [ordered]@{ schema = 'range.pc11-v43-root-seal-synthetic-evidence/v1'; fixtureOnly = $true; goPermitted = $false }
    $inventory = Write-FixtureEvidence 'synthetic-inventory' $support
    $linkMap = Write-FixtureEvidence 'synthetic-link-map' $support
    $package = Write-FixtureEvidence 'synthetic-package-tree' $support
    $static = Write-FixtureEvidence 'synthetic-static-suite' $support
    $authority = Write-FixtureEvidence 'synthetic-authority' $support
    $review = Write-FixtureEvidence 'synthetic-review' $support
    $decision = Write-FixtureEvidence 'synthetic-root-decision' $support
    $preflight = Write-FixtureEvidence 'synthetic-preflight' $support
    $release = Write-FixtureEvidence 'synthetic-release-build' $support
    $ctest = Write-FixtureEvidence 'synthetic-ctest-manifest' $support
    $cachePath = Join-Path $tempRoot 'synthetic-CMakeCache.txt'
    [IO.File]::WriteAllText($cachePath, 'fixture-only CMakeCache`n', (New-Object Text.UTF8Encoding($false)))
    $cacheSha = Get-Sha256File $cachePath

    $paths = @($bootstrapPath, $finalizerPath, $bootstrapTestPath, $fixturePath)
    $repoPrefix = $repoRoot.TrimEnd([char[]]@('\', '/')) + [IO.Path]::DirectorySeparatorChar
    $rows = New-Object 'System.Collections.Generic.List[object]'
    foreach ($path in $paths) {
        $relative = [IO.Path]::GetFullPath($path).Substring($repoPrefix.Length).Replace('\', '/')
        $rows.Add([pscustomobject][ordered]@{ path = $relative; sha256 = (Get-Sha256File $path); sizeBytes = [long](Get-Item -LiteralPath $path).Length })
    }
    $sourceRows = @(Sort-SourceRowsOrdinal $rows.ToArray())
    $sourceTree = Get-CanonicalSourceTreeSha256 $sourceRows
    $rootCommit = 'a' * 40
    $sourceScope = [ordered]@{
        includePrefixes = @('src/', 'scripts/packaging/', 'scripts/benchmark/', 'scripts/ci/', 'cmake/')
        includeBasenamesAnywhere = @('CMakeLists.txt', 'CMakePresets.json', 'CMakeSettings.json', 'vcpkg.json', 'vcpkg-configuration.json')
        includeExtensionsAnywhere = @('.cmake', '.cmake.in')
        includeExactPaths = @('.github/workflows/windows-regression.yml', 'docs/PIEZO_CONTROL_DELIVERY.md', 'docs/piezo_control_plan.json')
        excludePrefixes = @('build/', '.git/')
        enumeration = 'SYNTHETIC_FIXTURE_ONLY'
        pathOrder = 'UTF8_BYTE_LEXICOGRAPHIC'
        treeDigest = 'SYNTHETIC_FIXTURE_ONLY'
    }
    $freezePath = Join-Path $tempRoot 'synthetic-source-freeze.json'
    $freeze = [ordered]@{
        schema = 'range.pc11-v43-source-freeze/v1'; status = 'PASS'; candidateVersion = 'v43'; variant = 'OFF'; configuration = 'Release'
        generatedAtUtc = [DateTime]::UtcNow.ToString('o'); sourceRootPath = $repoRoot; sourceScopeSchema = 'range.pc11-v43-source-scope/v1'
        sourceScope = $sourceScope; rootCommit = $rootCommit; rootWorktreeStatus = 'fixture-only'; rootWorktreeStatusSha256 = (Get-Sha256Bytes ([Text.Encoding]::UTF8.GetBytes('fixture-only')))
        submoduleCommits = @(); sourceFiles = $sourceRows; sourceFileCount = [int]$sourceRows.Count
        sourceTotalBytes = [long](($sourceRows | Measure-Object -Property sizeBytes -Sum).Sum); sourceTreeSha256 = $sourceTree
        preBuildSourcePreflightReceiptPath = $preflight.path; preBuildSourcePreflightReceiptSha256 = $preflight.sha256
        postBuildSourcePreflightReceiptPath = $preflight.path; postBuildSourcePreflightReceiptSha256 = $preflight.sha256
        requiredCTestManifestPath = $ctest.path; requiredCTestManifestSha256 = $ctest.sha256
        cmakeCachePath = $cachePath; cmakeCacheSha256 = $cacheSha
        releaseBuildReceiptPath = $release.path; releaseBuildReceiptSha256 = $release.sha256
        staticOnly = $true; productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false; fixtureOnly = $true
    }
    $freezeSha = Write-Json $freezePath $freeze

    $manifest = Get-Content -Raw -LiteralPath $templatePath | ConvertFrom-Json -ErrorAction Stop
    $manifest.candidateState = 'STATIC_CANDIDATE_PENDING_REVIEW_AND_ROOT_DECISION'
    $manifest.installBinding.requiredVariant = 'OFF'
    $manifest.installBinding.installRoot = Join-Path $tempRoot 'synthetic-off-install'
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
    $manifest.sourceAndAuthorityPins.releaseBuildReceiptPath = $release.path
    $manifest.sourceAndAuthorityPins.releaseBuildReceiptSha256 = $release.sha256
    $manifest.sourceAndAuthorityPins.authorityBinding.authorityReceiptPath = $authority.path
    $manifest.sourceAndAuthorityPins.authorityBinding.authorityReceiptSha256 = $authority.sha256
    $manifest.sourceAndAuthorityPins.authorityBinding.additionalOriginalAuthorities = @([ordered]@{ path = $authority.path; sha256 = $authority.sha256 })
    $manifest.candidateEvidencePins.offLinkMapReceiptPath = $linkMap.path
    $manifest.candidateEvidencePins.offLinkMapReceiptSha256 = $linkMap.sha256
    $manifest.candidateEvidencePins.packageTreeReceiptPath = $package.path
    $manifest.candidateEvidencePins.packageTreeReceiptSha256 = $package.sha256
    $manifest.candidateEvidencePins.staticTestReceipts = @([ordered]@{ name = 'v43-static-suite'; path = $static.path; sha256 = $static.sha256 })
    $manifest.workflowBoundary.independentReviewReceiptPath = $review.path
    $manifest.workflowBoundary.independentReviewReceiptSha256 = $review.sha256
    $manifest.workflowBoundary.rootDecisionReceiptPath = $decision.path
    $manifest.workflowBoundary.rootDecisionReceiptSha256 = $decision.sha256
    $manifestPath = Join-Path $tempRoot 'synthetic-complete-candidate.json'
    $manifestSha = Write-Json $manifestPath $manifest
    return [pscustomobject]@{ manifest = $manifest; manifestPath = $manifestPath; manifestSha256 = $manifestSha; freeze = $freeze; freezePath = $freezePath; freezeSha256 = $freezeSha }
}
function Assert-FixtureRejected([string]$Name, [string]$ManifestPath, [string]$ManifestSha, [string]$FreezePath, [string]$FreezeSha) {
    $child = Invoke-BoundaryFixture $ManifestPath $ManifestSha $FreezePath $FreezeSha
    if ($child.exitCode -eq 0) { throw "Root-seal fixture accepted tampered synthetic case '$Name'. stdout=$($child.stdout)" }
    return [pscustomobject]@{ name = $Name; status = 'PASS'; rejected = $true; exitCode = $child.exitCode }
}
function Invoke-GenuineFinalizerNoGo([string]$ValidSyntheticManifestPath, $ValidSyntheticManifest, [string]$AuditOutputRoot) {
    $caseRoot = Join-Path $AuditOutputRoot ('root-seal-finalizer-runtime-' + [Guid]::NewGuid().ToString('N'))
    $auditRoot = Join-Path $caseRoot 'audit'
    $outputPath = Join-Path $auditRoot 'finalize-no-go.json'
    [void][IO.Directory]::CreateDirectory($caseRoot)
    $invalidManifest = $ValidSyntheticManifest | ConvertTo-Json -Depth 30 | ConvertFrom-Json
    $invalidManifest.goPermitted = @($false)
    $badManifestPath = Join-Path $caseRoot 'malformed-candidate.json'
    $badManifestSha = Write-Json $badManifestPath $invalidManifest
    $child = Invoke-PowerShellChild $finalizerPath @(
        '-CandidateManifestPath', $badManifestPath,
        '-CandidateManifestSha256', $badManifestSha,
        '-AuditRoot', $auditRoot,
        '-OutputPath', $outputPath,
        '-RunId', ('root-seal-' + [Guid]::NewGuid().ToString('N'))
    ) $caseRoot
    if (-not [IO.File]::Exists($outputPath)) { throw "Genuine finalizer emitted no runtime receipt. stdout=$($child.stdout) stderr=$($child.stderr)" }
    $receipt = (Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json -ErrorAction Stop)
    if ($child.exitCode -eq 0 -or $receipt.schema -cne 'range.pc11-v43-final-static-receipt/v1' -or
        $receipt.status -cne 'FINALIZE_FAIL' -or $receipt.candidateVersion -cne 'v43' -or $receipt.variant -cne 'OFF' -or
        $receipt.staticOnly -isnot [bool] -or $receipt.staticOnly -ne $true -or
        $receipt.goPermitted -isnot [bool] -or $receipt.goPermitted -ne $false) {
        throw 'Genuine finalizer did not emit a fail-closed v43 OFF static-only no-GO runtime receipt.'
    }
    foreach ($name in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        if ($receipt.productActions.$name -isnot [bool] -or $receipt.productActions.$name -ne $false) { throw "Finalizer runtime receipt permits product action $name." }
    }
    return [ordered]@{
        status = 'PASS'; receiptSchema = [string]$receipt.schema; runtimeReceiptStatus = [string]$receipt.status
        receiptPath = [IO.Path]::GetFullPath($outputPath); receiptSha256 = Get-Sha256File $outputPath; processExitCode = $child.exitCode
        candidateVersion = [string]$receipt.candidateVersion; variant = [string]$receipt.variant; staticOnly = [bool]$receipt.staticOnly
        productActions = $receipt.productActions; goPermitted = [bool]$receipt.goPermitted
    }
}

try {
    $synthetic = New-SyntheticCandidatePair
    $syntheticPositive = Invoke-BoundaryFixture $synthetic.manifestPath $synthetic.manifestSha256 $synthetic.freezePath $synthetic.freezeSha256
    if ($syntheticPositive.exitCode -ne 0) { throw "Complete synthetic candidate pair was rejected. stdout=$($syntheticPositive.stdout) stderr=$($syntheticPositive.stderr)" }
    $syntheticResult = $syntheticPositive.stdout.Trim() | ConvertFrom-Json -ErrorAction Stop
    if ($syntheticResult.schema -cne 'range.pc11-v43-root-seal-boundary-static-fixture/v1' -or
        $syntheticResult.status -cne 'PASS' -or $syntheticResult.candidateGateStatus -cne 'PENDING' -or
        $syntheticResult.syntheticCompleteCandidateValidated -ne $true -or
        $syntheticResult.productActions.guiLaunched -ne $false -or $syntheticResult.goPermitted -ne $false) {
        throw 'Synthetic root-seal positive case failed its PASS/PENDING/no-GO contract.'
    }
    $syntheticCases = New-Object 'System.Collections.Generic.List[object]'
    [void]$syntheticCases.Add([pscustomobject]@{ name = 'complete-candidate-freeze-pair'; status = 'PASS'; accepted = $true; candidateGateStatus = 'PENDING' })

    $qualificationCases = @(
        [pscustomobject]@{ name = 'blank-qualification'; value = '   ' },
        [pscustomobject]@{ name = 'non-string-qualification'; value = 17 },
        [pscustomobject]@{ name = 'pass-qualification'; value = 'PASS' },
        [pscustomobject]@{ name = 'trimmed-pass-qualification'; value = '  PASS  ' },
        [pscustomobject]@{ name = 'go-qualification'; value = 'GO' },
        [pscustomobject]@{ name = 'trimmed-go-qualification'; value = "`tGO " },
        [pscustomobject]@{ name = 'pending-placeholder-qualification'; value = 'PENDING_LINK_MAP' }
    )
    foreach ($case in $qualificationCases) {
        $mutated = $synthetic.manifest | ConvertTo-Json -Depth 30 | ConvertFrom-Json
        $mutated.installBinding.releaseQualificationStatus = $case.value
        $mutatedPath = Join-Path $tempRoot ('tamper-release-qualification-' + $case.name + '.json')
        $mutatedSha = Write-Json $mutatedPath $mutated
        [void]$syntheticCases.Add((Assert-FixtureRejected ('release-qualification-' + $case.name) $mutatedPath $mutatedSha $synthetic.freezePath $synthetic.freezeSha256))
    }

    $treeMismatch = $synthetic.manifest | ConvertTo-Json -Depth 30 | ConvertFrom-Json
    $treeMismatch.sourceAndAuthorityPins.sourceTreeSha256 = 'f' * 64
    $treeMismatchPath = Join-Path $tempRoot 'tamper-source-tree-manifest.json'
    $treeMismatchSha = Write-Json $treeMismatchPath $treeMismatch
    [void]$syntheticCases.Add((Assert-FixtureRejected 'candidate-source-tree-mismatch' $treeMismatchPath $treeMismatchSha $synthetic.freezePath $synthetic.freezeSha256))

    $enabledAction = $synthetic.manifest | ConvertTo-Json -Depth 30 | ConvertFrom-Json
    $enabledAction.productActions.confirmPerformed = $true
    $enabledActionPath = Join-Path $tempRoot 'tamper-enabled-action-manifest.json'
    $enabledActionSha = Write-Json $enabledActionPath $enabledAction
    [void]$syntheticCases.Add((Assert-FixtureRejected 'candidate-enabled-confirm-action' $enabledActionPath $enabledActionSha $synthetic.freezePath $synthetic.freezeSha256))

    $tamperedFreeze = Join-Path $tempRoot 'tamper-freeze-bytes.json'
    [IO.File]::WriteAllText($tamperedFreeze, ((Get-Content -Raw -LiteralPath $synthetic.freezePath).Replace('fixture-only', 'fixture-tampered')), (New-Object Text.UTF8Encoding($false)))
    [void]$syntheticCases.Add((Assert-FixtureRejected 'source-freeze-bytes-differ-from-pin' $synthetic.manifestPath $synthetic.manifestSha256 $tamperedFreeze $synthetic.freezeSha256))

    $enabledFreeze = $synthetic.freeze | ConvertTo-Json -Depth 30 | ConvertFrom-Json
    $enabledFreeze.productActions.runPerformed = $true
    $enabledFreezePath = Join-Path $tempRoot 'tamper-enabled-action-freeze.json'
    $enabledFreezeSha = Write-Json $enabledFreezePath $enabledFreeze
    $enabledFreezeManifest = $synthetic.manifest | ConvertTo-Json -Depth 30 | ConvertFrom-Json
    $enabledFreezeManifest.sourceAndAuthorityPins.sourceFreezeReceiptPath = $enabledFreezePath
    $enabledFreezeManifest.sourceAndAuthorityPins.sourceFreezeReceiptSha256 = $enabledFreezeSha
    $enabledFreezeManifestPath = Join-Path $tempRoot 'tamper-enabled-action-freeze-manifest.json'
    $enabledFreezeManifestSha = Write-Json $enabledFreezeManifestPath $enabledFreezeManifest
    [void]$syntheticCases.Add((Assert-FixtureRejected 'source-freeze-enabled-product-action' $enabledFreezeManifestPath $enabledFreezeManifestSha $enabledFreezePath $enabledFreezeSha))

    $changedRowsFreeze = $synthetic.freeze | ConvertTo-Json -Depth 30 | ConvertFrom-Json
    $changedRowsFreeze.sourceFiles[0].sha256 = 'f' * 64
    $changedRowsFreeze.sourceTreeSha256 = Get-CanonicalSourceTreeSha256 $changedRowsFreeze.sourceFiles
    $changedRowsFreezePath = Join-Path $tempRoot 'tamper-source-row-freeze.json'
    $changedRowsFreezeSha = Write-Json $changedRowsFreezePath $changedRowsFreeze
    $changedRowsManifest = $synthetic.manifest | ConvertTo-Json -Depth 30 | ConvertFrom-Json
    $changedRowsManifest.sourceAndAuthorityPins.sourceFreezeReceiptPath = $changedRowsFreezePath
    $changedRowsManifest.sourceAndAuthorityPins.sourceFreezeReceiptSha256 = $changedRowsFreezeSha
    $changedRowsManifest.sourceAndAuthorityPins.sourceTreeSha256 = $changedRowsFreeze.sourceTreeSha256
    $changedRowsManifestPath = Join-Path $tempRoot 'tamper-source-row-manifest.json'
    $changedRowsManifestSha = Write-Json $changedRowsManifestPath $changedRowsManifest
    [void]$syntheticCases.Add((Assert-FixtureRejected 'source-freeze-source-file-byte-mismatch' $changedRowsManifestPath $changedRowsManifestSha $changedRowsFreezePath $changedRowsFreezeSha))

    $finalizerRuntime = Invoke-GenuineFinalizerNoGo $synthetic.manifestPath $synthetic.manifest ([IO.Path]::GetDirectoryName($OutputPath))
    $candidateGateStatus = 'PENDING'
    $candidateBinding = $null
    if ($provided -eq 4) {
        $CandidateManifestPath = [IO.Path]::GetFullPath($CandidateManifestPath)
        $SourceFreezeReceiptPath = [IO.Path]::GetFullPath($SourceFreezeReceiptPath)
        if ((Get-Sha256File $CandidateManifestPath) -cne $CandidateManifestSha256.ToLowerInvariant() -or
            (Get-Sha256File $SourceFreezeReceiptPath) -cne $SourceFreezeReceiptSha256.ToLowerInvariant()) {
            throw 'Externally supplied candidate or source-freeze bytes do not match their pins.'
        }
        $actual = Invoke-BoundaryFixture $CandidateManifestPath $CandidateManifestSha256 $SourceFreezeReceiptPath $SourceFreezeReceiptSha256
        if ($actual.exitCode -ne 0) { throw "Externally pinned candidate pair failed the root-seal boundary. stdout=$($actual.stdout) stderr=$($actual.stderr)" }
        $actualResult = $actual.stdout.Trim() | ConvertFrom-Json -ErrorAction Stop
        if ($actualResult.status -cne 'PASS' -or $actualResult.goPermitted -ne $false -or $actualResult.productActions.confirmPerformed -ne $false) {
            throw 'Externally pinned candidate boundary result did not preserve PASS/no-GO.'
        }
        $actualManifest = Get-Content -Raw -LiteralPath $CandidateManifestPath | ConvertFrom-Json -ErrorAction Stop
        $candidateGateStatus = 'PASS'
        $candidateBinding = [ordered]@{
            candidateManifestPath = $CandidateManifestPath; candidateManifestSha256 = $CandidateManifestSha256.ToLowerInvariant()
            sourceFreezeReceiptPath = $SourceFreezeReceiptPath; sourceFreezeReceiptSha256 = $SourceFreezeReceiptSha256.ToLowerInvariant()
            candidateManifestIdentitySha256 = Get-ManifestIdentitySha256 $actualManifest
            sourceTreeSha256 = ([string]$actualManifest.sourceAndAuthorityPins.sourceTreeSha256).ToLowerInvariant()
            staticOnly = $true
        }
    }
    $receipt = [ordered]@{
        schema = 'range.pc11-v43-root-seal-boundary-static-fixture/v1'
        status = 'PASS'
        candidateVersion = 'v43'
        variant = 'OFF'
        candidateGateStatus = $candidateGateStatus
        fixtureStatus = 'PASS'
        fixtureOnly = $true
        staticOnly = $true
        candidateBinding = $candidateBinding
        candidateManifestSha256 = if ($null -ne $candidateBinding) { $candidateBinding.candidateManifestSha256 } else { $null }
        sourceFreezeReceiptSha256 = if ($null -ne $candidateBinding) { $candidateBinding.sourceFreezeReceiptSha256 } else { $null }
        candidateManifestIdentitySha256 = if ($null -ne $candidateBinding) { $candidateBinding.candidateManifestIdentitySha256 } else { $null }
        sourceTreeSha256 = if ($null -ne $candidateBinding) { $candidateBinding.sourceTreeSha256 } else { $null }
        syntheticCompleteCandidateValidated = $true
        syntheticCases = @($syntheticCases.ToArray())
        finalizerNoGoRuntimeProof = $finalizerRuntime
        externalRootGoSealPerformed = $false
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
    $parentPath = [IO.Path]::GetDirectoryName($OutputPath)
    if (-not [IO.Directory]::Exists($parentPath)) { [void][IO.Directory]::CreateDirectory($parentPath) }
    [IO.File]::WriteAllText($OutputPath, (($receipt | ConvertTo-Json -Depth 20) + "`n"), (New-Object Text.UTF8Encoding($false)))
    Write-Output ("PASS root_seal candidateGate={0} syntheticCases={1} finalizerRuntime={2} goPermitted=false output={3}" -f
        $candidateGateStatus, $syntheticCases.Count, $finalizerRuntime.runtimeReceiptStatus, $OutputPath)
}
finally {
    if ([IO.Directory]::Exists($tempRoot)) { Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue }
}
