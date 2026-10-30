[CmdletBinding()]
param([string]$OutputPath = '')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'This v43 supervisor/worker contract test requires Windows PowerShell 5.1.'
}

$prepRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$runtimeRoot = Join-Path $prepRoot 'runtime'
. (Join-Path $runtimeRoot 'contract-common-v43.ps1')
. (Join-Path $runtimeRoot 'supervisor-core-v43.ps1')
. (Join-Path $runtimeRoot 'watchdog-contract-v43.ps1')
. (Join-Path $runtimeRoot 'native-launch-boundary-v43.ps1')

function Invoke-V43ChildProcess([string]$FileName, [string[]]$Arguments, [int]$TimeoutSeconds = 30) {
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FileName
    $startInfo.Arguments = (($Arguments | ForEach-Object { Quote-V43WindowsArgument ([string]$_) }) -join ' ')
    $startInfo.WorkingDirectory = $prepRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) { throw "Could not start isolated test child: $FileName" }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill()
            [void]$process.WaitForExit(5000)
            throw "Isolated test child timed out: $FileName"
        }
        $process.WaitForExit()
        [System.Threading.Tasks.Task]::WaitAll([System.Threading.Tasks.Task[]]@($stdoutTask, $stderrTask))
        return [pscustomobject]@{ exitCode = [int]$process.ExitCode; stdout = [string]$stdoutTask.Result; stderr = [string]$stderrTask.Result }
    }
    finally {
        if (-not $process.HasExited) { try { $process.Kill(); [void]$process.WaitForExit(5000) } catch { } }
        $process.Dispose()
    }
}

function Write-V43FixtureJson([string]$Path, $Value) {
    $null = Write-V43JsonNewFile $Path $Value 20
}

function Assert-V43ChildNoGo($Receipt, [string]$ExpectedStatus, [string]$Label) {
    if ($Receipt.status -cne $ExpectedStatus -or $Receipt.goPermitted -ne $false) { throw "$Label did not preserve its expected no-GO status." }
    foreach ($name in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        if ($Receipt.productActions.$name -ne $false) { throw "$Label permits product action $name." }
    }
}

function Get-V43PythonIdentity([string]$ManifestPath) {
    $pythonLauncher = (Get-Command 'py.exe' -ErrorAction Stop).Source
    $code = 'import json,sys; sys.path.insert(0,sys.argv[1]); from admission_v43 import _candidate_manifest_identity_sha256; print(_candidate_manifest_identity_sha256(json.load(open(sys.argv[2],encoding="utf-8"))))'
    $result = Invoke-V43ChildProcess $pythonLauncher @('-3', '-B', '-c', $code, $prepRoot, $ManifestPath) 30
    if ($result.exitCode -ne 0) { throw "v43 identity cross-check failed: $($result.stderr)" }
    $identity = $result.stdout.Trim()
    if ($identity -notmatch '^[0-9a-f]{64}$') { throw 'admission_v43.py returned a malformed fixture identity.' }
    return $identity
}

function New-V43FinalizeFixture([string]$Root) {
    $audit = Join-Path $Root 'audit'
    [void][IO.Directory]::CreateDirectory($audit)
    $install = Join-Path $Root 'off-install'
    [void][IO.Directory]::CreateDirectory($install)
    $freezePath = Join-Path $Root 'source-freeze-fixture-v43.json'
    $freeze = [ordered]@{
        schema = 'range.pc11-v43-source-freeze/v1'
        status = 'PASS'
        candidateVersion = 'v43'
        variant = 'OFF'
        configuration = 'Release'
        staticOnly = $true
        sourceTreeSha256 = ('3' * 64)
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
    Write-V43FixtureJson $freezePath $freeze
    $freezeSha = Get-V43FileSha256 $freezePath

    $packagePath = Join-Path $Root 'package-tree-fixture-v43.json'
    $staticPath = Join-Path $Root 'static-suite-fixture-v43.json'
    $required = @(
        'source_preflight', 'source_freeze', 'package_tree', 'module_boundary_build', 'module_boundary_source',
        'watchdog_preflight', 'save_predicate_preflight', 'path_shadow', 'root_seal', 'save_predicate',
        'watchdog_contract', 'sampler_arguments', 'supervisor', 'worker_handshake', 'native_launch_boundary',
        'parser', 'finalize_integrity', 'source_static_tests', 'root_bootstrap'
    )
    $inventorySha = '1' * 64
    $sourceTreeSha = '3' * 64
    $manifest = [ordered]@{
        schema = 'range.pc11-fresh-run-v43-admission-candidate/v1'
        candidateVersion = 'v43'
        goPermitted = $false
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        workflowBoundary = [ordered]@{
            staticReadinessDoesNotGrantProductGo = $true
            externalIndependentReviewRequired = $true
            separateRootTechnicalDecisionRequired = $true
            goSealRequiredBeforeAnyProductAction = $true
        }
        installBinding = [ordered]@{
            requiredVariant = 'OFF'
            installRoot = $install
            inventoryReportSha256 = $inventorySha
        }
        sourceAndAuthorityPins = [ordered]@{
            rootCommit = 'a' * 40
            sourceTreeSha256 = $sourceTreeSha
            sourceFreezeReceiptPath = $freezePath
            sourceFreezeReceiptSha256 = $freezeSha
            releaseBuildReceiptSha256 = '4' * 64
        }
        candidateEvidencePins = [ordered]@{
            requiredStaticTestIds = $required
            packageTreeReceiptPath = $packagePath
            packageTreeReceiptSha256 = ''
            staticTestReceipts = @()
        }
    }
    $identity = Get-V43CandidateManifestIdentitySha256 $manifest
    $noGo = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
    $package = [ordered]@{
        schema = 'range.pc11-v43-package-tree-receipt/v1'
        status = 'PASS'
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        candidateManifestIdentitySha256 = $identity
        sourceFreezeReceiptSha256 = $freezeSha
        sourceTreeSha256 = $sourceTreeSha
        inventoryReportSha256 = $inventorySha
        productActions = $noGo
        goPermitted = $false
    }
    Write-V43FixtureJson $packagePath $package
    $packageSha = Get-V43FileSha256 $packagePath
    $pendingRows = @($required | ForEach-Object { [ordered]@{ testId = [string]$_; status = 'PENDING' } })
    $static = [ordered]@{
        schema = 'range.pc11-v43-static-test-receipt/v1'
        status = 'STATIC_INCOMPLETE'
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        candidateManifestIdentitySha256 = $identity
        sourceTreeSha256 = $sourceTreeSha
        inventoryReportSha256 = $inventorySha
        packageTreeReceiptSha256 = $packageSha
        requiredTestIds = $required
        results = $pendingRows
        productActions = $noGo
        goPermitted = $false
    }
    Write-V43FixtureJson $staticPath $static
    $staticSha = Get-V43FileSha256 $staticPath
    $manifest.candidateEvidencePins.packageTreeReceiptSha256 = $packageSha
    $manifest.candidateEvidencePins.staticTestReceipts = @([ordered]@{ name = 'v43-static-suite'; path = $staticPath; sha256 = $staticSha })
    $manifestPath = Join-Path $Root 'candidate-manifest-fixture-v43.json'
    Write-V43FixtureJson $manifestPath $manifest
    return [pscustomobject]@{
        auditRoot = $audit
        manifestPath = $manifestPath
        manifestSha = Get-V43FileSha256 $manifestPath
        candidateIdentitySha = Get-V43CandidateManifestIdentitySha256 $manifest
        staticPath = $staticPath
        staticSha = $staticSha
    }
}

function Invoke-V43Finalizer($Fixture, [string]$OutputPath) {
    $finalizer = Join-Path $prepRoot 'finalize-v43.ps1'
    $child = Invoke-V43ChildProcess (Join-Path $PSHOME 'powershell.exe') @(
        '-NoProfile', '-NoLogo', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $finalizer,
        '-CandidateManifestPath', $Fixture.manifestPath,
        '-CandidateManifestSha256', $Fixture.manifestSha,
        '-OutputPath', $OutputPath,
        '-AuditRoot', $Fixture.auditRoot
    ) 30
    if (-not [IO.File]::Exists($OutputPath)) { throw "v43 finalizer did not publish its bounded receipt: $($child.stderr)" }
    $receipt = [IO.File]::ReadAllText($OutputPath, (New-Object Text.UTF8Encoding($false, $true))) | ConvertFrom-Json -ErrorAction Stop
    return [pscustomobject]@{ child = $child; receipt = $receipt }
}

$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
$tempRoot = Join-Path $tempBase ('range-fea-v43-contract-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($tempRoot)
$results = New-Object 'System.Collections.Generic.List[object]'
$templatePath = Join-Path $prepRoot 'candidate-manifest-v43.admission-template.json'
$templateSha = Get-V43FileSha256 $templatePath
$fixture = $null
try {
    $template = [IO.File]::ReadAllText($templatePath, (New-Object Text.UTF8Encoding($false, $true))) | ConvertFrom-Json -ErrorAction Stop
    if ($template.candidateVersion -cne 'v43' -or $template.goPermitted -ne $false) { throw 'Current v43 template lost its no-GO identity.' }
    foreach ($name in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        if ($template.productActions.$name -ne $false) { throw "Current v43 template permits product action $name." }
    }
    if ([string]$template.workflowBoundary.independentReviewReceiptPath -notmatch '^PENDING_' -or
        [string]$template.workflowBoundary.rootDecisionReceiptPath -notmatch '^PENDING_') {
        throw 'External review or root decision unexpectedly has a v43 pin; this fixture must not infer bootstrap readiness.'
    }

    $fixture = New-V43FinalizeFixture $tempRoot
    $binding = Assert-V43CandidateManifest $fixture.manifestPath $fixture.manifestSha $fixture.candidateIdentitySha
    $pythonIdentity = Get-V43PythonIdentity $fixture.manifestPath
    if ($pythonIdentity -cne $fixture.candidateIdentitySha) { throw 'PowerShell candidate identity differs from admission_v43.py.' }
    $manifestTypeMutations = @(
        [pscustomobject]@{ label = 'numeric-no-go'; apply = { param($value) $value.goPermitted = 0 } },
        [pscustomobject]@{ label = 'numeric-action'; apply = { param($value) $value.productActions.guiLaunched = 0 } },
        [pscustomobject]@{ label = 'numeric-workflow-boundary'; apply = { param($value) $value.workflowBoundary.goSealRequiredBeforeAnyProductAction = 1 } }
    )
    foreach ($mutation in $manifestTypeMutations) {
        $mutant = ($binding.manifest | ConvertTo-Json -Depth 30 | ConvertFrom-Json -ErrorAction Stop)
        & $mutation.apply $mutant
        $mutantPath = Join-Path $tempRoot ('candidate-type-' + $mutation.label + '.json')
        [IO.File]::WriteAllText($mutantPath, (($mutant | ConvertTo-Json -Depth 30) + "`n"), (New-Object Text.UTF8Encoding($false)))
        $rejected = $false
        try { $null = Assert-V43CandidateManifest $mutantPath (Get-V43FileSha256 $mutantPath) $fixture.candidateIdentitySha }
        catch { $rejected = $true }
        if (-not $rejected) { throw "Candidate manifest accepted $($mutation.label)." }
    }
    $envelopeTemplate = [pscustomobject]@{
        candidateVersion = 'v43'; variant = 'OFF'; staticOnly = $true; goPermitted = $false
        candidateManifestIdentitySha256 = $fixture.candidateIdentitySha
        productActions = [pscustomobject]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
    }
    Assert-V43NoGoEnvelope $envelopeTemplate 'Typed envelope fixture' $fixture.candidateIdentitySha
    foreach ($mutation in @(
        [pscustomobject]@{ label = 'numeric-static-only'; apply = { param($value) $value.staticOnly = 1 } },
        [pscustomobject]@{ label = 'numeric-envelope-no-go'; apply = { param($value) $value.goPermitted = 0 } },
        [pscustomobject]@{ label = 'numeric-envelope-action'; apply = { param($value) $value.productActions.confirmPerformed = 0 } }
    )) {
        $mutant = ($envelopeTemplate | ConvertTo-Json -Depth 10 | ConvertFrom-Json -ErrorAction Stop)
        & $mutation.apply $mutant
        $rejected = $false
        try { Assert-V43NoGoEnvelope $mutant ('Typed envelope ' + $mutation.label) $fixture.candidateIdentitySha }
        catch { $rejected = $true }
        if (-not $rejected) { throw "No-GO envelope accepted $($mutation.label)." }
    }
    [void]$results.Add([pscustomobject]@{
        testId = 'candidate-manifest-and-envelope-boolean-types'; status = 'PASS'; productReadiness = 'PENDING'
        malformedManifestCasesRejected = $manifestTypeMutations.Count; malformedEnvelopeCasesRejected = 3
    })

    foreach ($path in @(
        (Join-Path $runtimeRoot 'contract-common-v43.ps1'),
        (Join-Path $runtimeRoot 'supervisor-core-v43.ps1'),
        (Join-Path $runtimeRoot 'worker-handshake-v43.ps1'),
        (Join-Path $runtimeRoot 'watchdog-contract-v43.ps1'),
        (Join-Path $runtimeRoot 'native-launch-boundary-v43.ps1'),
        (Join-Path $PSScriptRoot 'fixtures\worker-fixture-v43.ps1'),
        (Join-Path $prepRoot 'finalize-v43.ps1'),
        $PSCommandPath
    )) {
        $tokens = $null
        $parseErrors = $null
        [void][Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$parseErrors)
        if (@($parseErrors).Count -gt 0) { throw "Windows PowerShell parser rejected $path : $($parseErrors[0].Message)" }
    }

    $workerFixture = Join-Path $PSScriptRoot 'fixtures\worker-fixture-v43.ps1'
    $badEvidence = Join-Path $tempRoot 'worker-negative'
    [void][IO.Directory]::CreateDirectory($badEvidence)
    $badWorker = Invoke-V43ChildProcess (Join-Path $PSHOME 'powershell.exe') @(
        '-NoProfile', '-NoLogo', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $workerFixture,
        '-CandidateManifestPath', $fixture.manifestPath,
        '-CandidateManifestSha256', $fixture.manifestSha,
        '-CandidateManifestIdentitySha256', ('0' * 64),
        '-EvidenceRoot', $badEvidence,
        '-SupervisorReadyPath', (Join-Path $badEvidence 'ready.json'),
        '-SupervisorTerminalPath', (Join-Path $badEvidence 'terminal.json'),
        '-SupervisorToken', ([Guid]::NewGuid().ToString('N')),
        '-SupervisorPid', [string]$PID,
        '-FixtureScriptSha256', (Get-V43FileSha256 $workerFixture),
        '-HandshakeScriptSha256', (Get-V43FileSha256 (Join-Path $runtimeRoot 'worker-handshake-v43.ps1')),
        '-WorkerErrorPath', (Join-Path $badEvidence 'worker.error')
    ) 15
    if ($badWorker.exitCode -eq 0 -or [IO.File]::Exists((Join-Path $badEvidence 'terminal.json'))) {
        throw 'Worker accepted a candidate identity mismatch or published a terminal receipt.'
    }
    [void]$results.Add([pscustomobject]@{
        testId = 'worker_handshake'; status = 'PASS'; productReadiness = 'PENDING'
        positiveFixtureAccepted = $true; candidateIdentityMismatchRejected = $true
        fixtureCandidateManifestIdentitySha256 = $fixture.candidateIdentitySha
    })

    $supervisorRoot = Join-Path $tempRoot 'supervisor-fixtures'
    [void][IO.Directory]::CreateDirectory($supervisorRoot)
    $supervisor = Invoke-V43StaticFixtureSupervisor -CandidateManifestPath $fixture.manifestPath `
        -CandidateManifestSha256 $fixture.manifestSha -CandidateManifestIdentitySha256 $fixture.candidateIdentitySha `
        -FixtureEvidenceRoot $supervisorRoot
    if (-not $supervisor.success -or $supervisor.status -cne 'FIXTURE_COMPLETE' -or
        $supervisor.candidateManifestIdentitySha256 -cne $fixture.candidateIdentitySha -or $supervisor.goPermitted -ne $false) {
        throw 'v43 fixture supervisor did not return a candidate-bound no-GO success receipt.'
    }
    $beforeMismatch = @(Get-ChildItem -LiteralPath $supervisorRoot -Force).Count
    $manifestMismatchRejected = $false
    try {
        $null = Invoke-V43StaticFixtureSupervisor -CandidateManifestPath $fixture.manifestPath `
            -CandidateManifestSha256 ('f' * 64) -CandidateManifestIdentitySha256 $fixture.candidateIdentitySha `
            -FixtureEvidenceRoot $supervisorRoot
    }
    catch { $manifestMismatchRejected = $true }
    $afterMismatch = @(Get-ChildItem -LiteralPath $supervisorRoot -Force).Count
    if (-not $manifestMismatchRejected -or $beforeMismatch -ne $afterMismatch) { throw 'Supervisor accepted a bad external manifest hash or created evidence before rejecting it.' }

    $assignmentFailureRoot = Join-Path $tempRoot 'supervisor-assignment-failure'
    [void][IO.Directory]::CreateDirectory($assignmentFailureRoot)
    $assignmentFailureError = $null
    try {
        $null = Invoke-V43StaticFixtureSupervisor -CandidateManifestPath $fixture.manifestPath `
            -CandidateManifestSha256 $fixture.manifestSha -CandidateManifestIdentitySha256 $fixture.candidateIdentitySha `
            -FixtureEvidenceRoot $assignmentFailureRoot -TestOnlySimulateAssignmentFailure
    }
    catch { $assignmentFailureError = $_ }
    if ($null -eq $assignmentFailureError -or
        $assignmentFailureError.Exception.Message -cne 'Suspended fixture worker could not be assigned to the supervisor Job.') {
        throw 'Supervisor assignment-failure injection did not preserve its original failure.'
    }
    $assignmentCleanup = $assignmentFailureError.Exception.Data['v43SupervisorCleanup']
    if ($null -eq $assignmentCleanup -or $assignmentCleanup.workerAssigned -ne $false -or
        $assignmentCleanup.terminationAttempted -ne $true -or $assignmentCleanup.terminationAccepted -ne $true -or
        $assignmentCleanup.processWaitStatus -ne 0 -or $assignmentCleanup.processExitCode -ne 125 -or
        $assignmentCleanup.processDrained -ne $true) {
        throw 'Supervisor did not prove bounded termination and a terminal exit code for its unassigned worker.'
    }
    if (Get-Process -Id ([int]$assignmentCleanup.workerPid) -ErrorAction SilentlyContinue) {
        throw 'Supervisor assignment-failure cleanup left its unassigned worker process alive.'
    }
    $assignmentRunRoots = @(Get-ChildItem -LiteralPath $assignmentFailureRoot -Directory -Force)
    if ($assignmentRunRoots.Count -ne 1 -or
        [IO.File]::Exists((Join-Path $assignmentRunRoots[0].FullName 'ready.json')) -or
        [IO.File]::Exists((Join-Path $assignmentRunRoots[0].FullName 'terminal.json')) -or
        [IO.File]::Exists((Join-Path $assignmentRunRoots[0].FullName 'supervisor-receipt.json'))) {
        throw 'Assignment failure unexpectedly published worker or supervisor readiness evidence.'
    }
    $assignmentFailureNoGo = [pscustomobject]@{
        candidateVersion = 'v43'; variant = 'OFF'; staticOnly = $true
        candidateManifestIdentitySha256 = $fixture.candidateIdentitySha
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
    Assert-V43NoGoEnvelope $assignmentFailureNoGo 'v43 supervisor assignment-failure fixture' $fixture.candidateIdentitySha
    [void]$results.Add([pscustomobject]@{
        testId = 'supervisor'; status = 'PASS'; productReadiness = 'PENDING'
        fixtureOnlyWorkerPathEnforced = $true; candidateManifestHashRequired = $true
        badManifestHashRejectedBeforeLaunch = $true; assignmentFailureInjectionReachedSupervisor = $true
        unassignedWorkerDrainedWithExitCode = [int]$assignmentCleanup.processExitCode
        assignmentFailureOriginalExceptionPreserved = $true; assignmentFailureStayedNoGo = $true
        supervisorReceiptSha256 = $supervisor.receiptSha256
        fixtureCandidateManifestIdentitySha256 = $fixture.candidateIdentitySha
    })

    $watchdogRoot = Join-Path $tempRoot 'watchdog-fixtures'
    [void][IO.Directory]::CreateDirectory($watchdogRoot)
    $watched = Invoke-V43StaticFixtureWatchdog -CandidateManifestPath $fixture.manifestPath `
        -CandidateManifestSha256 $fixture.manifestSha -CandidateManifestIdentitySha256 $fixture.candidateIdentitySha `
        -FixtureEvidenceRoot $watchdogRoot
    $badWatchdogResult = [pscustomobject]@{
        success = $false; status = 'SUPERVISOR_FAILURE'; candidateManifestIdentitySha256 = $fixture.candidateIdentitySha
        goPermitted = $false; productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
    }
    $watchdogFailureRejected = $false
    try { $null = Assert-V43WatchdogSupervisorResult $badWatchdogResult $fixture.candidateIdentitySha }
    catch { $watchdogFailureRejected = $true }
    if ($watched.status -cne 'FIXTURE_COMPLETE' -or -not $watchdogFailureRejected) { throw 'Watchdog failed to accept the bound success or reject a failed supervisor result.' }
    $watchdogMalformed = @(
        [pscustomobject]@{ label = 'numeric-success'; success = 1; goPermitted = $false; action = $false },
        [pscustomobject]@{ label = 'string-success'; success = 'True'; goPermitted = $false; action = $false },
        [pscustomobject]@{ label = 'numeric-no-go'; success = $true; goPermitted = 0; action = $false },
        [pscustomobject]@{ label = 'string-no-go'; success = $true; goPermitted = 'False'; action = $false },
        [pscustomobject]@{ label = 'numeric-product-action'; success = $true; goPermitted = $false; action = 0 }
    )
    foreach ($malformed in $watchdogMalformed) {
        $malformedResult = [pscustomobject]@{
            success = $malformed.success; status = 'FIXTURE_COMPLETE'; candidateManifestIdentitySha256 = $fixture.candidateIdentitySha
            goPermitted = $malformed.goPermitted
            productActions = [pscustomobject]@{ guiLaunched = $malformed.action; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        }
        $rejected = $false
        try { $null = Assert-V43WatchdogSupervisorResult $malformedResult $fixture.candidateIdentitySha }
        catch { $rejected = $true }
        if (-not $rejected) { throw "Watchdog accepted a coerced Boolean in $($malformed.label)." }
    }
    foreach ($malformedShape in @(
        [pscustomobject]@{ label = 'empty-status'; status = @(); identity = $fixture.candidateIdentitySha },
        [pscustomobject]@{ label = 'repeated-status'; status = @('FIXTURE_COMPLETE', 'FIXTURE_COMPLETE'); identity = $fixture.candidateIdentitySha },
        [pscustomobject]@{ label = 'empty-identity'; status = 'FIXTURE_COMPLETE'; identity = @() },
        [pscustomobject]@{ label = 'repeated-identity'; status = 'FIXTURE_COMPLETE'; identity = @($fixture.candidateIdentitySha, $fixture.candidateIdentitySha) }
    )) {
        $malformedResult = [pscustomobject]@{
            success = $true; status = $malformedShape.status; candidateManifestIdentitySha256 = $malformedShape.identity
            goPermitted = $false
            productActions = [pscustomobject]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        }
        $rejected = $false
        try { $null = Assert-V43WatchdogSupervisorResult $malformedResult $fixture.candidateIdentitySha }
        catch { $rejected = $true }
        if (-not $rejected) { throw "Watchdog accepted a collection in $($malformedShape.label)." }
    }
    [void]$results.Add([pscustomobject]@{
        testId = 'watchdog_contract'; status = 'PASS'; productReadiness = 'PENDING'
        boundSupervisorSuccessAccepted = $true; supervisorFailureRejected = $true; malformedBooleanCasesRejected = $watchdogMalformed.Count; malformedCollectionCasesRejected = 4
        fixtureCandidateManifestIdentitySha256 = $fixture.candidateIdentitySha
    })

    $nativeRoot = Join-Path $tempRoot 'native-fixtures'
    [void][IO.Directory]::CreateDirectory($nativeRoot)
    $native = Invoke-V43NativeLaunchBoundaryFixture $nativeRoot $fixture.candidateIdentitySha
    if ($native.status -cne 'PASS' -or @($native.cases).Count -ne 3 -or
        @($native.cases | Where-Object { $_.status -cne 'PASS' }).Count -ne 0 -or
        $native.cases[0].executed -ne $false -or -not $native.cases[1].exactJobMembershipVerified -or
        -not $native.cases[2].killOnJobClose -or -not $native.cases[2].drained) {
        throw 'v43 native launch boundary fixture did not prove suspended assignment, exact Job membership, and kill-on-close.'
    }
    [void]$results.Add([pscustomobject]@{
        testId = 'native_launch_boundary'; status = 'PASS'; productReadiness = 'PENDING'
        createSuspendedBeforeAssignment = $true; exactJobMembershipBeforeResume = $true
        withheldAssignmentStayedInert = $true; killOnJobCloseDrainedFixture = $true
        fixtureCandidateManifestIdentitySha256 = $fixture.candidateIdentitySha
    })

    $positiveFinalizerPath = Join-Path $fixture.auditRoot 'final-static-fixture-v43.json'
    $positiveFinalize = Invoke-V43Finalizer $fixture $positiveFinalizerPath
    Assert-V43ChildNoGo $positiveFinalize.receipt 'STATIC_INCOMPLETE' 'positive v43 finalizer fixture'
    if ($positiveFinalize.child.exitCode -ne 0 -or $positiveFinalize.receipt.allRequiredStaticTestsPass -ne $false -or
        @($positiveFinalize.receipt.pendingTestIds).Count -ne 19) {
        throw 'v43 finalizer did not preserve an incomplete static receipt as no-GO.'
    }
    [IO.File]::WriteAllText($fixture.staticPath, '{"tampered":true}', (New-Object Text.UTF8Encoding($false)))
    $tamperOutput = Join-Path $fixture.auditRoot 'final-static-tampered-fixture-v43.json'
    $tamperedFinalize = Invoke-V43Finalizer $fixture $tamperOutput
    Assert-V43ChildNoGo $tamperedFinalize.receipt 'FINALIZE_FAIL' 'tampered v43 finalizer fixture'
    if ($tamperedFinalize.child.exitCode -eq 0 -or $tamperedFinalize.receipt.failure -notmatch 'SHA256') {
        throw 'v43 finalizer accepted an aggregate static-receipt byte change.'
    }
    [void]$results.Add([pscustomobject]@{
        testId = 'finalize_integrity'; status = 'PASS'; productReadiness = 'PENDING'
        incompleteStaticChainStayedNoGo = $true; externallyPinnedReceiptTamperRejected = $true
        finalizerPositiveStatus = [string]$positiveFinalize.receipt.status
        finalizerTamperStatus = [string]$tamperedFinalize.receipt.status
    })

    [void]$results.Add([pscustomobject]@{
        testId = 'root_bootstrap'; status = 'PENDING'; productReadiness = 'PENDING'
        reason = 'The v43 candidate template still has PENDING_EXTERNAL_BOOTSTRAP_PATH and PENDING_EXTERNAL_BOOTSTRAP_SHA256; no external root-bootstrap receipt was supplied or synthesized.'
        externalBootstrapReceiptPinned = $false
    })

    foreach ($resultRow in $results) {
        $fixtureStatus = if ($resultRow.status -ceq 'PASS') { 'PASS' } else { 'PENDING' }
        $resultRow | Add-Member -NotePropertyName fixtureStatus -NotePropertyValue $fixtureStatus
        $resultRow | Add-Member -NotePropertyName candidateGateStatus -NotePropertyValue 'PENDING'
    }

    $sourceHashes = [ordered]@{}
    foreach ($path in @(
        (Join-Path $runtimeRoot 'contract-common-v43.ps1'),
        (Join-Path $runtimeRoot 'supervisor-core-v43.ps1'),
        (Join-Path $runtimeRoot 'worker-handshake-v43.ps1'),
        (Join-Path $runtimeRoot 'watchdog-contract-v43.ps1'),
        (Join-Path $runtimeRoot 'native-launch-boundary-v43.ps1'),
        (Join-Path $PSScriptRoot 'fixtures\worker-fixture-v43.ps1'),
        $PSCommandPath,
        (Join-Path $prepRoot 'finalize-v43.ps1')
    )) {
        $relativePath = $path.Substring($prepRoot.TrimEnd('\').Length + 1)
        $sourceHashes[$relativePath.Replace('\', '/')] = Get-V43FileSha256 $path
    }
    $receipt = [ordered]@{
        schema = 'range.pc11-v43-supervisor-worker-contract-static-receipt/v1'
        status = 'STATIC_INCOMPLETE'
        staticContractSuiteStatus = 'PASS'
        fixtureContractStatus = 'PASS'
        candidateGateStatus = 'PENDING'
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        fixtureOnly = $true
        generatedAtUtc = [DateTime]::UtcNow.ToString('o')
        currentCandidateTemplatePath = $templatePath
        currentCandidateTemplateSha256 = $templateSha
        fixtureCandidateManifestSha256 = $fixture.manifestSha
        fixtureCandidateManifestIdentitySha256 = $fixture.candidateIdentitySha
        admissionIdentityCrossCheckPassed = $true
        requiredStaticTestIdsCovered = @('supervisor', 'worker_handshake', 'watchdog_contract', 'native_launch_boundary', 'finalize_integrity', 'root_bootstrap')
        results = @($results.ToArray())
        sourceSha256 = $sourceHashes
        productReadiness = 'PENDING_OFF_INSTALL_SOURCE_FREEZE_BUILD_AND_EXTERNAL_REVIEW'
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
        integrationNote = 'These rows are fixture contract evidence. The fixture identity is synthetic and must not be presented as the live OFF candidate identity or admitted as the full v43 static suite.'
    }
    Assert-V43NoGoEnvelope ([pscustomobject]@{ candidateVersion = 'v43'; variant = 'OFF'; staticOnly = $true; candidateManifestIdentitySha256 = $fixture.candidateIdentitySha; productActions = $receipt.productActions; goPermitted = $false }) 'v43 static supervisor contract receipt' $fixture.candidateIdentitySha
    $auditRoot = [IO.Path]::GetFullPath((Join-Path $prepRoot '..\..\..\build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit'))
    if ([string]::IsNullOrWhiteSpace($OutputPath)) {
        $OutputPath = Join-Path $auditRoot ('supervisor-worker-contract-receipt-v43-' + [Guid]::NewGuid().ToString('N') + '.json')
    }
    $OutputPath = [IO.Path]::GetFullPath($OutputPath)
    if (-not (Test-V43PathUnderRoot $OutputPath $auditRoot)) { throw 'Static contract receipt must stay under the external audit directory.' }
    $written = Write-V43JsonNewFile $OutputPath $receipt 20
    Write-Output ('PASS v43 supervisor/worker fixture contract; root_bootstrap=PENDING; product readiness and GO remain false; receipt=' + $written)
}
finally {
    if ([IO.Directory]::Exists($tempRoot)) {
        $resolvedTemp = [IO.Path]::GetFullPath($tempRoot)
        $tempPrefix = $tempBase + [IO.Path]::DirectorySeparatorChar
        if (-not $resolvedTemp.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or
            [IO.Path]::GetFileName($resolvedTemp) -notmatch '^range-fea-v43-contract-[0-9a-f]{32}$' -or
            (([IO.File]::GetAttributes($resolvedTemp) -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw 'Refusing to remove a temporary test tree outside its verified unique system-temp root.'
        }
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}
