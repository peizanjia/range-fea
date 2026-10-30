[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CandidateManifestPath,
    [Parameter(Mandatory = $true)][string]$CandidateManifestSha256,
    [Parameter(Mandatory = $true)][string]$SourceFreezeReceiptPath,
    [Parameter(Mandatory = $true)][string]$SourceFreezeReceiptSha256
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'The v43 root-seal boundary fixture requires Windows PowerShell 5.1.'
}

function Get-FileSha256([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}
function Read-PinnedJson([string]$Path, [string]$Pin, [string]$Label) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not [IO.File]::Exists($fullPath)) { throw "$Label does not exist: $fullPath" }
    $bytes = [IO.File]::ReadAllBytes($fullPath)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $actualPin = ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
    if ($Pin -notmatch '^[0-9a-fA-F]{64}$' -or $actualPin -cne $Pin.ToLowerInvariant()) {
        throw "$Label does not match its externally supplied SHA256 pin."
    }
    return (New-Object Text.UTF8Encoding($false, $true)).GetString($bytes) | ConvertFrom-Json -ErrorAction Stop
}
function Get-CanonicalSourceTreeSha256($Rows) {
    if ($null -eq $Rows -or @($Rows).Count -eq 0) { throw 'Source-freeze sourceFiles must be a non-empty array.' }
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $canonicalRows = New-Object 'System.Collections.Generic.List[object]'
    $previousPath = $null
    foreach ($row in @($Rows)) {
        if ($null -eq $row -or $row -is [System.Array] -or $row -isnot [PSCustomObject]) { throw 'Source-freeze sourceFiles contains a malformed row.' }
        if (@($row.PSObject.Properties).Count -ne 3) { throw 'Source-freeze source row has unexpected or duplicate fields.' }
        foreach ($name in @('path', 'sha256', 'sizeBytes')) { if ($null -eq $row.PSObject.Properties[$name]) { throw "Source-freeze source row lacks $name." } }
        $path = [string]$row.path
        $digest = [string]$row.sha256
        if ([string]::IsNullOrWhiteSpace($path) -or $path.StartsWith('/') -or $path.Contains('\') -or $path -match '(^|/)\.\.?(/|$)' -or
            $digest -cnotmatch '^[0-9a-f]{64}$' -or $row.sizeBytes -isnot [long] -and $row.sizeBytes -isnot [int] -and $row.sizeBytes -isnot [double] -or
            [double]$row.sizeBytes -lt 0 -or [double]$row.sizeBytes -ne [Math]::Truncate([double]$row.sizeBytes) -or -not $seen.Add($path) -or
            ($null -ne $previousPath -and [StringComparer]::Ordinal.Compare($previousPath, $path) -ge 0)) {
            throw 'Source-freeze sourceFiles contains a duplicate or malformed source row.'
        }
        $previousPath = $path
        [void]$canonicalRows.Add([pscustomobject]@{ path = $path; sha256 = $digest; sizeBytes = [long]$row.sizeBytes })
    }
    # The row sequence was validated above; preserve it exactly as supplied by the freeze producer.
    $jsonRows = New-Object 'System.Collections.Generic.List[string]'
    foreach ($row in $canonicalRows) {
        [void]$jsonRows.Add('{"path":' + (ConvertTo-Json -InputObject ([string]$row.path) -Compress) + ',"sha256":' +
            (ConvertTo-Json -InputObject ([string]$row.sha256) -Compress) + ',"sizeBytes":' + [string]$row.sizeBytes + '}')
    }
    $json = '[' + [string]::Join(',', $jsonRows.ToArray()) + ']'
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash((New-Object Text.UTF8Encoding($false, $true)).GetBytes($json)))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Invoke-PinnedBootstrapFixture([string]$Path) {
    $powerShellPath = [IO.Path]::GetFullPath((Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'))
    if (-not [IO.File]::Exists($powerShellPath)) { throw 'Windows PowerShell 5.1 executable is unavailable.' }
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $powerShellPath
    $info.Arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + $Path.Replace('"', '\"') + '"'
    $info.WorkingDirectory = [IO.Path]::GetTempPath()
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    try {
        if (-not $process.Start()) { throw 'Could not start the isolated root-bootstrap fixture.' }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(180000)) { try { $process.Kill() } catch {}; throw 'Root-bootstrap fixture timed out.' }
        $process.WaitForExit()
        return [pscustomobject]@{ exitCode = $process.ExitCode; stdout = $stdoutTask.Result; stderr = $stderrTask.Result }
    }
    finally { $process.Dispose() }
}
function Assert-NoProductActions($Document, [string]$Label) {
    if ($Document.goPermitted -isnot [bool] -or $Document.goPermitted -ne $false) { throw "$Label must keep goPermitted=false." }
    if ($null -eq $Document.productActions -or $Document.productActions -is [System.Array] -or
        ($Document.productActions -isnot [PSCustomObject] -and $Document.productActions -isnot [System.Collections.IDictionary])) {
        throw "$Label.productActions must be a JSON object."
    }
    foreach ($action in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        if ($Document.productActions -is [System.Collections.IDictionary]) {
            $hasAction = $Document.productActions.Contains($action)
            $actionValue = if ($hasAction) { $Document.productActions[$action] } else { $null }
        }
        else {
            $property = $Document.productActions.PSObject.Properties[$action]
            $hasAction = $null -ne $property
            $actionValue = if ($hasAction) { $property.Value } else { $null }
        }
        if (-not $hasAction -or $actionValue -isnot [bool] -or $actionValue -ne $false) {
            throw "$Label must keep productActions.$action=false."
        }
    }
}
function Assert-StringField($Document, [string]$Name, [string]$Label) {
    if ($null -eq $Document -or $Document -is [System.Array] -or $Document -isnot [PSCustomObject]) { throw "$Label must be a JSON object." }
    $property = $Document.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -isnot [string] -or [string]::IsNullOrWhiteSpace([string]$property.Value)) {
        throw "$Label.$Name must be a non-empty JSON string."
    }
}
function Assert-HashField($Document, [string]$Name, [string]$Label) {
    Assert-StringField $Document $Name $Label
    if ([string]$Document.$Name -cnotmatch '^[0-9a-fA-F]{64}$') { throw "$Label.$Name must be a 64-digit SHA256." }
}
function Assert-CompleteCandidate($Manifest, $Freeze, [string]$FreezePath, [string]$FreezeSha256, [string]$RepoRoot) {
    Assert-StringField $Manifest 'schema' 'Candidate manifest'
    Assert-StringField $Manifest 'candidateVersion' 'Candidate manifest'
    Assert-StringField $Manifest 'candidateState' 'Candidate manifest'
    if ($Manifest.schema -cne 'range.pc11-fresh-run-v43-admission-candidate/v1' -or
        $Manifest.candidateVersion -cne 'v43' -or $Manifest.goPermitted -isnot [bool] -or $Manifest.goPermitted -ne $false) {
        throw 'Candidate manifest schema/version or no-GO boundary is invalid.'
    }
    Assert-NoProductActions $Manifest 'Candidate manifest'
    foreach ($group in @('installBinding', 'installedPathLayout', 'sourceAndAuthorityPins', 'candidateEvidencePins', 'workflowBoundary')) {
        $property = $Manifest.PSObject.Properties[$group]
        if ($null -eq $property -or $property.Value -is [System.Array] -or $property.Value -isnot [PSCustomObject]) {
            throw "Candidate manifest.$group must be a JSON object."
        }
    }
    $install = $Manifest.installBinding
    Assert-StringField $install 'requiredVariant' 'Candidate manifest.installBinding'
    Assert-StringField $install 'installRoot' 'Candidate manifest.installBinding'
    Assert-StringField $install 'inventoryReportPath' 'Candidate manifest.installBinding'
    Assert-HashField $install 'inventoryReportSha256' 'Candidate manifest.installBinding'
    Assert-StringField $install 'releaseQualificationStatus' 'Candidate manifest.installBinding'
    $qualificationStatus = [string]$install.releaseQualificationStatus
    if ($install.requiredVariant -cne 'OFF' -or $install.inventorySchemaVersion -ne 2 -or
        $install.inventoryStatus -cne 'PASS' -or [string]::IsNullOrWhiteSpace($qualificationStatus) -or
        $qualificationStatus.Trim() -match '^(?i:PASS|GO)$' -or $qualificationStatus -match '(?i)PENDING_') {
        throw 'Candidate install binding must be OFF schema-v2 with a PASS inventory and a non-PASS/non-GO qualification value.'
    }
    $layout = $Manifest.installedPathLayout
    foreach ($field in @('guiExecutableRelativePath', 'privatePythonRelativePath', 'solverExecutableRelativePath',
        'providerRootRelativePath', 'productionControlRelativePath', 'nativeBackendRelativePath')) {
        Assert-StringField $layout $field 'Candidate manifest.installedPathLayout'
    }
    foreach ($field in @('guiExecutableSha256', 'privatePythonSha256', 'solverExecutableSha256', 'productionControlSha256', 'nativeBackendSha256')) {
        Assert-HashField $layout $field 'Candidate manifest.installedPathLayout'
    }
    $source = $Manifest.sourceAndAuthorityPins
    foreach ($field in @('sourceRootPath', 'sourceFreezeReceiptPath', 'requiredCTestManifestPath', 'cmakeCachePath', 'releaseBuildReceiptPath')) {
        Assert-StringField $source $field 'Candidate manifest.sourceAndAuthorityPins'
    }
    foreach ($field in @('sourceFreezeReceiptSha256', 'sourceTreeSha256', 'releaseBuildReceiptSha256', 'requiredCTestManifestSha256', 'cmakeCacheSha256')) {
        Assert-HashField $source $field 'Candidate manifest.sourceAndAuthorityPins'
    }
    Assert-StringField $source 'rootCommit' 'Candidate manifest.sourceAndAuthorityPins'
    if ($source.rootCommit -notmatch '^(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})$') { throw 'Candidate rootCommit is malformed.' }
    if (-not [IO.Path]::GetFullPath($source.sourceFreezeReceiptPath).Equals([IO.Path]::GetFullPath($FreezePath), [StringComparison]::OrdinalIgnoreCase) -or
        $source.sourceFreezeReceiptSha256 -cne $FreezeSha256.ToLowerInvariant() -or
        -not [IO.Path]::GetFullPath($source.sourceRootPath).Equals([IO.Path]::GetFullPath($RepoRoot), [StringComparison]::OrdinalIgnoreCase) -or
        $source.rootCommit -cne $Freeze.rootCommit -or $source.sourceTreeSha256 -cne $Freeze.sourceTreeSha256) {
        throw 'Candidate manifest and externally pinned source-freeze receipt do not bind the same root, commit, tree, and bytes.'
    }
    $authority = $source.authorityBinding
    Assert-StringField $authority 'authorityReceiptPath' 'Candidate manifest.sourceAndAuthorityPins.authorityBinding'
    Assert-HashField $authority 'authorityReceiptSha256' 'Candidate manifest.sourceAndAuthorityPins.authorityBinding'
    if ($authority.additionalOriginalAuthorities -isnot [System.Array] -or @($authority.additionalOriginalAuthorities).Count -eq 0) {
        throw 'Candidate authority binding must contain at least one additional original authority.'
    }
    foreach ($row in @($authority.additionalOriginalAuthorities)) {
        Assert-StringField $row 'path' 'Candidate additional original authority'
        Assert-HashField $row 'sha256' 'Candidate additional original authority'
    }
    $evidence = $Manifest.candidateEvidencePins
    foreach ($field in @('offLinkMapReceiptPath', 'packageTreeReceiptPath')) { Assert-StringField $evidence $field 'Candidate manifest.candidateEvidencePins' }
    foreach ($field in @('offLinkMapReceiptSha256', 'packageTreeReceiptSha256')) { Assert-HashField $evidence $field 'Candidate manifest.candidateEvidencePins' }
    $requiredIds = @('source_preflight','source_freeze','package_tree','module_boundary_build','module_boundary_source','watchdog_preflight',
        'save_predicate_preflight','path_shadow','root_seal','save_predicate','watchdog_contract','sampler_arguments','supervisor',
        'worker_handshake','native_launch_boundary','parser','finalize_integrity','source_static_tests','root_bootstrap')
    if ($evidence.requiredStaticTestIds -isnot [System.Array] -or
        (ConvertTo-Json -InputObject @($evidence.requiredStaticTestIds) -Compress) -cne (ConvertTo-Json -InputObject $requiredIds -Compress) -or
        $evidence.staticTestReceipts -isnot [System.Array] -or @($evidence.staticTestReceipts).Count -ne 1) {
        throw 'Candidate static evidence pins do not contain the fixed 19-ID contract and one aggregate receipt.'
    }
    $staticPin = @($evidence.staticTestReceipts)[0]
    Assert-StringField $staticPin 'name' 'Candidate static-test receipt pin'
    Assert-StringField $staticPin 'path' 'Candidate static-test receipt pin'
    Assert-HashField $staticPin 'sha256' 'Candidate static-test receipt pin'
    if ($staticPin.name -cne 'v43-static-suite') { throw 'Candidate static-test receipt pin must name v43-static-suite.' }
    $workflow = $Manifest.workflowBoundary
    foreach ($field in @('staticReadinessDoesNotGrantProductGo', 'externalIndependentReviewRequired', 'separateRootTechnicalDecisionRequired', 'goSealRequiredBeforeAnyProductAction')) {
        if ($workflow.PSObject.Properties[$field] -eq $null -or $workflow.$field -isnot [bool] -or $workflow.$field -ne $true) {
            throw "Candidate workflow boundary must keep $field=true."
        }
    }
    foreach ($field in @('independentReviewReceiptPath', 'rootDecisionReceiptPath')) { Assert-StringField $workflow $field 'Candidate manifest.workflowBoundary' }
    foreach ($field in @('independentReviewReceiptSha256', 'rootDecisionReceiptSha256')) { Assert-HashField $workflow $field 'Candidate manifest.workflowBoundary' }

    foreach ($field in @('candidateVersion', 'variant', 'configuration', 'sourceRootPath', 'sourceScopeSchema', 'rootCommit',
        'rootWorktreeStatus', 'sourceTreeSha256', 'preBuildSourcePreflightReceiptPath', 'postBuildSourcePreflightReceiptPath',
        'requiredCTestManifestPath', 'cmakeCachePath', 'releaseBuildReceiptPath')) {
        Assert-StringField $Freeze $field 'Source-freeze receipt'
    }
    foreach ($field in @('schema', 'status')) { Assert-StringField $Freeze $field 'Source-freeze receipt' }
    foreach ($field in @('rootWorktreeStatusSha256', 'sourceTreeSha256', 'preBuildSourcePreflightReceiptSha256',
        'postBuildSourcePreflightReceiptSha256', 'requiredCTestManifestSha256', 'cmakeCacheSha256', 'releaseBuildReceiptSha256')) {
        Assert-HashField $Freeze $field 'Source-freeze receipt'
    }
    if ($Freeze.schema -cne 'range.pc11-v43-source-freeze/v1' -or $Freeze.status -cne 'PASS' -or
        $Freeze.candidateVersion -cne 'v43' -or $Freeze.variant -cne 'OFF' -or $Freeze.configuration -cne 'Release' -or
        $Freeze.staticOnly -isnot [bool] -or $Freeze.staticOnly -ne $true -or
        -not [IO.Path]::GetFullPath($Freeze.sourceRootPath).Equals([IO.Path]::GetFullPath($RepoRoot), [StringComparison]::OrdinalIgnoreCase) -or
        $Freeze.rootCommit -notmatch '^(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})$' -or
        $Freeze.sourceFiles -isnot [System.Array] -or @($Freeze.sourceFiles).Count -eq 0) {
        throw 'Source-freeze receipt is not a complete v43 OFF Release PASS snapshot.'
    }
    Assert-NoProductActions $Freeze 'Source-freeze receipt'
}

$manifest = Read-PinnedJson $CandidateManifestPath $CandidateManifestSha256 'Candidate manifest'
$freeze = Read-PinnedJson $SourceFreezeReceiptPath $SourceFreezeReceiptSha256 'Source-freeze receipt'
Assert-CompleteCandidate $manifest $freeze $SourceFreezeReceiptPath $SourceFreezeReceiptSha256 ([string]$freeze.sourceRootPath)

$repoRoot = [IO.Path]::GetFullPath([string]$freeze.sourceRootPath)
if (-not [IO.Directory]::Exists($repoRoot) -or $freeze.rootCommit -notmatch '^(?:[0-9a-f]{40}|[0-9a-f]{64})$' -or
    $manifest.sourceAndAuthorityPins.sourceTreeSha256 -notmatch '^[0-9a-f]{64}$' -or
    (Get-CanonicalSourceTreeSha256 $freeze.sourceFiles) -cne ([string]$freeze.sourceTreeSha256).ToLowerInvariant()) {
    throw 'The pinned source-freeze receipt lacks a valid source root, root commit, or tree digest.'
}
$packageRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$bootstrapPath = [IO.Path]::GetFullPath((Join-Path $packageRoot 'runtime\root-bootstrap-v43.ps1'))
$finalizerPath = [IO.Path]::GetFullPath((Join-Path $packageRoot 'finalize-v43.ps1'))
$bootstrapTestPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'test-root-bootstrap-v43.ps1'))
$boundaryTestPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'test-root-seal-boundary-v43-fixture.ps1'))
$pinnedPaths = @($bootstrapPath, $finalizerPath, $bootstrapTestPath, $boundaryTestPath)
$sourceRows = @{}
foreach ($row in @($freeze.sourceFiles)) { $sourceRows[[string]$row.path] = [string]$row.sha256 }
foreach ($path in $pinnedPaths) {
    if (-not [IO.File]::Exists($path)) { throw "Production boundary script is missing: $path" }
    $repoPrefix = $repoRoot.TrimEnd([char[]]@('\', '/')) + [IO.Path]::DirectorySeparatorChar
    if (-not $path.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Pinned boundary source escaped the frozen source root: $path" }
    $relative = $path.Substring($repoPrefix.Length).Replace('\', '/')
    if (-not $sourceRows.ContainsKey($relative) -or $sourceRows[$relative] -cnotmatch '^[0-9a-f]{64}$' -or
        (Get-FileSha256 $path) -cne $sourceRows[$relative]) {
        throw "Candidate source freeze does not pin the current boundary source bytes: $relative"
    }
}

function Get-ScriptAst([string]$Path) {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    if (@($errors).Count -gt 0) { throw "PowerShell parser rejected $Path" }
    return $ast
}
$bootstrapAst = Get-ScriptAst $bootstrapPath
$finalizerAst = Get-ScriptAst $finalizerPath
$bootstrapParams = @($bootstrapAst.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath })
$finalizerParams = @($finalizerAst.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath })
$sealParameters = @($bootstrapParams + $finalizerParams | Where-Object { $_ -match '(?i)seal' })

$bootstrapRun = Invoke-PinnedBootstrapFixture $bootstrapTestPath
if ($bootstrapRun.exitCode -ne 0 -or
    $bootstrapRun.stdout -notmatch 'PASS_ROOT_BOOTSTRAP_V43_SYNTHETIC_FIXTURES scenarioCount=31 positive=6 rejected=24 .*actualReviewOrDecisionPerformed=false' -or
    $bootstrapRun.stderr -match 'goPermitted=true|guiLaunched=true|savePerformed=true|runPerformed=true|confirmPerformed=true') {
    throw "Pinned root-bootstrap synthetic suite failed or violated the no-GO boundary. exit=$($bootstrapRun.exitCode); stdout=$($bootstrapRun.stdout); stderr=$($bootstrapRun.stderr)"
}

$result = [ordered]@{
    schema = 'range.pc11-v43-root-seal-boundary-static-fixture/v1'
    status = 'PASS'
    candidateGateStatus = 'PENDING'
    fixtureOnly = $true
    staticOnly = $true
    candidateManifestPath = [IO.Path]::GetFullPath($CandidateManifestPath)
    candidateManifestSha256 = $CandidateManifestSha256.ToLowerInvariant()
    sourceFreezeReceiptPath = [IO.Path]::GetFullPath($SourceFreezeReceiptPath)
    sourceFreezeReceiptSha256 = $SourceFreezeReceiptSha256.ToLowerInvariant()
    rootSealProducerParameters = $sealParameters
    candidateSourceTreeSha256 = $manifest.sourceAndAuthorityPins.sourceTreeSha256
    candidateRootCommit = $manifest.sourceAndAuthorityPins.rootCommit
    candidatePinnedFiles = @($pinnedPaths | ForEach-Object { $_.Substring(($repoRoot.TrimEnd('\') + '\').Length).Replace('\', '/') })
    bootstrapFixture = [ordered]@{ status = 'PASS'; summary = $bootstrapRun.stdout.Trim(); actualReviewOrDecisionPerformed = $false }
    syntheticCompleteCandidateValidated = $true
    finalizerNoGoRuntimeProof = 'PENDING_OUTER_RUNTIME_RECEIPT_REQUIRED'
    candidateManifestIdentitySha256 = $null
    sourceTreeSha256 = [string]$manifest.sourceAndAuthorityPins.sourceTreeSha256
    externalRootGoSealPerformed = $false
    productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
    goPermitted = $false
}
Assert-NoProductActions ([pscustomobject]$result) 'Boundary fixture result'
Write-Output (($result | ConvertTo-Json -Depth 8 -Compress))
