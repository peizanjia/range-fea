[CmdletBinding()]
param(
    [string]$OutputPath,
    [string]$AuditRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'The v43 Save-predicate fixture requires Windows PowerShell 5.1.'
}

$prepRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoRoot = [IO.Path]::GetFullPath((Join-Path $prepRoot '..\..\..'))
$predicatePath = [IO.Path]::GetFullPath((Join-Path $prepRoot 'tools\save-predicate-v43.ps1'))
if (-not [IO.File]::Exists($predicatePath)) { throw "The v43 Save-predicate source is missing: $predicatePath" }
if (-not $AuditRoot) {
    $AuditRoot = Join-Path $repoRoot 'build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit'
}
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)
$prepPrefix = $prepRoot.TrimEnd('\') + '\'
if ($AuditRoot.StartsWith($prepPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The Save-predicate fixture receipt must be external to the v43 source package.'
}
if (-not $OutputPath) {
    $OutputPath = Join-Path $AuditRoot ('save-predicate-fixture-v43-' + [Guid]::NewGuid().ToString('N') + '.json')
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$auditPrefix = $AuditRoot.TrimEnd('\') + '\'
if (-not $OutputPath.StartsWith($auditPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The Save-predicate fixture receipt must remain under the external v43 output-audit directory.'
}
if ([IO.File]::Exists($OutputPath)) { throw "Refusing to overwrite single-use Save-predicate fixture evidence: $OutputPath" }

. $predicatePath

function Get-V43TestSha256Bytes([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-V43TestSha256File([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}

function Get-V43TestSha256Text([string]$Text) {
    return Get-V43TestSha256Bytes ((New-Object Text.UTF8Encoding($false, $true)).GetBytes($Text))
}

function Quote-V43TestArgument([string]$Value) {
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

function New-V43SaveFixture {
    $candidateIdentity = Get-V43TestSha256Text 'v43 synthetic fixture candidate identity'
    $sourceSha = Get-V43TestSha256Text 'v43 synthetic source bytes'
    $uia = [ordered]@{
        schema = 'range.pc11-v43-save-uia-evidence/v1'
        candidateManifestIdentitySha256 = $candidateIdentity
        observationId = 'uia-observation-fixture'
        observedAtUtc = '2026-10-14T02:58:00.0000000Z'
        projectId = 'project-fixture'
        documentId = 'document-fixture'
        modelId = 'model-fixture'
        sourceRevision = 'revision-fixture'
        modelSha256 = $sourceSha
        documentState = 'SAVED'
        dirty = $false
    }
    $publication = [ordered]@{
        eventId = 'event-publication-fixture'
        eventType = 'artifact.published'
        operationId = 'operation-fixture'
        candidateManifestIdentitySha256 = $candidateIdentity
        projectId = 'project-fixture'
        documentId = 'document-fixture'
        modelId = 'model-fixture'
        sourceRevision = 'revision-fixture'
        sourceSha256 = $sourceSha
        occurredAtUtc = '2026-10-14T02:59:00.0000000Z'
    }
    $close = [ordered]@{
        eventId = 'event-close-fixture'
        eventType = 'session.closed'
        operationId = 'operation-fixture'
        candidateManifestIdentitySha256 = $candidateIdentity
        projectId = 'project-fixture'
        documentId = 'document-fixture'
        modelId = 'model-fixture'
        sourceRevision = 'revision-fixture'
        sourceSha256 = $sourceSha
        occurredAtUtc = '2026-10-14T02:59:30.0000000Z'
    }
    $log = [ordered]@{
        schema = 'range.pc11-v43-save-operation-log/v1'
        candidateManifestIdentitySha256 = $candidateIdentity
        records = @($publication, $close)
    }
    $authority = [ordered]@{
        schema = 'range.pc11-v43-save-authority/v1'
        authorityId = 'authority-fixture'
        candidateManifestIdentitySha256 = $candidateIdentity
        projectId = 'project-fixture'
        documentId = 'document-fixture'
        sourceRevision = 'revision-fixture'
        sourceSha256 = $sourceSha
        scope = 'save-evidence-review'
        validFromUtc = '2026-10-14T02:00:00.0000000Z'
        validUntilUtc = '2026-10-14T04:00:00.0000000Z'
    }
    return [pscustomobject]@{ uia = $uia; log = $log; authority = $authority; candidateIdentity = $candidateIdentity; sourceSha = $sourceSha }
}

function Assert-V43Decision($Decision, [string]$ExpectedDecision, [string]$ExpectedReason, [string]$CaseName) {
    if ($Decision.schema -cne 'range.pc11-v43-save-predicate-decision/v1' -or
        $Decision.decision -cne $ExpectedDecision -or $Decision.reasonCode -cne $ExpectedReason) {
        throw "$CaseName expected $ExpectedDecision/$ExpectedReason; observed $($Decision.decision)/$($Decision.reasonCode)."
    }
    if ($Decision.fixtureOnlyDecision -ne $true -or $Decision.productActionDecision -cne 'NO_ACTION' -or $Decision.goPermitted -ne $false) {
        throw "$CaseName returned an action-capable or GO-capable decision."
    }
    foreach ($actionName in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        if ($Decision.productActions.$actionName -ne $false) { throw "$CaseName permits product action $actionName." }
    }
}

function Invoke-V43FakeFirstEnvironmentCase([string]$Root, [string]$PredicateScriptPath) {
    $fakeModuleRoot = Join-Path $Root 'fake-first-modules'
    $fakeModuleDirectory = Join-Path $fakeModuleRoot 'Microsoft.PowerShell.Utility'
    $fakePathRoot = Join-Path $Root 'fake-first-path'
    [void][IO.Directory]::CreateDirectory($fakeModuleDirectory)
    [void][IO.Directory]::CreateDirectory($fakePathRoot)
    $moduleMarker = Join-Path $Root 'fake-module-loaded.marker'
    $commandMarker = Join-Path $Root 'fake-command-invoked.marker'
    $resultPath = Join-Path $Root 'fake-environment-result.txt'
    $manifestPath = Join-Path $fakeModuleDirectory 'Microsoft.PowerShell.Utility.psd1'
    $modulePath = Join-Path $fakeModuleDirectory 'Microsoft.PowerShell.Utility.psm1'
    $manifest = "@{ RootModule='Microsoft.PowerShell.Utility.psm1'; ModuleVersion='1.0.0'; GUID='$([Guid]::NewGuid().ToString())'; FunctionsToExport=@('Get-FileHash','ConvertFrom-Json','ConvertTo-Json') }`n"
    $module = @"
[IO.File]::WriteAllText(`$env:V43_SAVE_FAKE_MODULE_MARKER, 'loaded')
function Get-FileHash { [IO.File]::WriteAllText(`$env:V43_SAVE_FAKE_COMMAND_MARKER, 'Get-FileHash'); return 'forged' }
function ConvertFrom-Json { [IO.File]::WriteAllText(`$env:V43_SAVE_FAKE_COMMAND_MARKER, 'ConvertFrom-Json'); return 'forged' }
function ConvertTo-Json { [IO.File]::WriteAllText(`$env:V43_SAVE_FAKE_COMMAND_MARKER, 'ConvertTo-Json'); return 'forged' }
Export-ModuleMember -Function Get-FileHash,ConvertFrom-Json,ConvertTo-Json
"@
    $utf8 = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($manifestPath, $manifest, $utf8)
    [IO.File]::WriteAllText($modulePath, $module, $utf8)
    $shadowCommand = "@echo off`r`necho fake-command-invoked>%V43_SAVE_FAKE_COMMAND_MARKER%`r`nexit /b 0`r`n"
    foreach ($commandName in @('Get-FileHash', 'ConvertFrom-Json', 'ConvertTo-Json')) {
        [IO.File]::WriteAllText((Join-Path $fakePathRoot ($commandName + '.cmd')), $shadowCommand, [Text.Encoding]::ASCII)
    }

    $predicateLiteral = "'" + $PredicateScriptPath.Replace("'", "''") + "'"
    $candidateIdentity = Get-V43TestSha256Text 'v43 synthetic fixture candidate identity'
    $sourceSha = Get-V43TestSha256Text 'v43 synthetic source bytes'
    $childBody = @"
`$ErrorActionPreference = 'Stop'
`$PSModuleAutoLoadingPreference = 'None'
. $predicateLiteral
`$candidate = '$candidateIdentity'
`$source = '$sourceSha'
`$uia = [ordered]@{ schema='range.pc11-v43-save-uia-evidence/v1'; candidateManifestIdentitySha256=`$candidate; observationId='uia-observation-fixture'; observedAtUtc='2026-10-14T02:58:00.0000000Z'; projectId='project-fixture'; documentId='document-fixture'; modelId='model-fixture'; sourceRevision='revision-fixture'; modelSha256=`$source; documentState='SAVED'; dirty=`$false }
`$publication = [ordered]@{ eventId='event-publication-fixture'; eventType='artifact.published'; operationId='operation-fixture'; candidateManifestIdentitySha256=`$candidate; projectId='project-fixture'; documentId='document-fixture'; modelId='model-fixture'; sourceRevision='revision-fixture'; sourceSha256=`$source; occurredAtUtc='2026-10-14T02:59:00.0000000Z' }
`$close = [ordered]@{ eventId='event-close-fixture'; eventType='session.closed'; operationId='operation-fixture'; candidateManifestIdentitySha256=`$candidate; projectId='project-fixture'; documentId='document-fixture'; modelId='model-fixture'; sourceRevision='revision-fixture'; sourceSha256=`$source; occurredAtUtc='2026-10-14T02:59:30.0000000Z' }
`$log = [ordered]@{ schema='range.pc11-v43-save-operation-log/v1'; candidateManifestIdentitySha256=`$candidate; records=@(`$publication,`$close) }
`$authority = [ordered]@{ schema='range.pc11-v43-save-authority/v1'; authorityId='authority-fixture'; candidateManifestIdentitySha256=`$candidate; projectId='project-fixture'; documentId='document-fixture'; sourceRevision='revision-fixture'; sourceSha256=`$source; scope='save-evidence-review'; validFromUtc='2026-10-14T02:00:00.0000000Z'; validUntilUtc='2026-10-14T04:00:00.0000000Z' }
`$decision = Get-V43SavePredicateDecision -UiaEvidence `$uia -OperationLog `$log -AuthorityEvidence `$authority -EvaluationTimeUtc '2026-10-14T03:00:00.0000000Z'
if (`$decision.decision -cne 'EVIDENCE_COMPLETE' -or `$decision.productActions.savePerformed -ne `$false -or `$decision.goPermitted -ne `$false) { exit 71 }
[IO.File]::WriteAllText(`$env:V43_SAVE_CHILD_RESULT, ([string]`$decision.decision + '|' + [string]`$decision.productActionDecision), [Text.Encoding]::ASCII)
"@
    $childScriptPath = Join-Path $Root 'fake-first-environment-child.ps1'
    [IO.File]::WriteAllText($childScriptPath, $childBody, $utf8)

    $arguments = @('-NoProfile', '-NoLogo', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $childScriptPath)
    $quoted = New-Object 'System.Collections.Generic.List[string]'
    foreach ($argument in $arguments) { [void]$quoted.Add((Quote-V43TestArgument ([string]$argument))) }
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = [IO.Path]::GetFullPath((Join-Path $PSHOME 'powershell.exe'))
    $startInfo.Arguments = [string]::Join(' ', $quoted.ToArray())
    $startInfo.WorkingDirectory = $Root
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.EnvironmentVariables['PATH'] = $fakePathRoot
    $startInfo.EnvironmentVariables['PATHEXT'] = '.COM;.EXE;.BAT;.CMD'
    $startInfo.EnvironmentVariables['PSModulePath'] = $fakeModuleRoot + ';' + [IO.Path]::Combine($PSHOME, 'Modules')
    $startInfo.EnvironmentVariables['V43_SAVE_FAKE_MODULE_MARKER'] = $moduleMarker
    $startInfo.EnvironmentVariables['V43_SAVE_FAKE_COMMAND_MARKER'] = $commandMarker
    $startInfo.EnvironmentVariables['V43_SAVE_CHILD_RESULT'] = $resultPath
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) { throw 'Could not start the isolated fake-first v43 Save-predicate process.' }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(20000)) {
            $process.Kill()
            [void]$process.WaitForExit(5000)
            throw 'The fake-first v43 Save-predicate process timed out.'
        }
        $process.WaitForExit()
        [System.Threading.Tasks.Task]::WaitAll([System.Threading.Tasks.Task[]]@($stdoutTask, $stderrTask))
        if ($process.ExitCode -ne 0 -or -not [IO.File]::Exists($resultPath)) {
            throw "Fake-first module/PATH case failed: exit=$($process.ExitCode); stdout=$($stdoutTask.Result); stderr=$($stderrTask.Result)"
        }
        if ([IO.File]::ReadAllText($resultPath) -cne 'EVIDENCE_COMPLETE|NO_ACTION') {
            throw 'Fake-first module/PATH case returned an unexpected predicate decision.'
        }
        if ([IO.File]::Exists($moduleMarker) -or [IO.File]::Exists($commandMarker)) {
            throw 'The fake-first module or exported command loaded during pure predicate evaluation.'
        }
    }
    finally { $process.Dispose() }

    return [pscustomobject][ordered]@{
        case = 'fake-first-PSModulePath-and-PATH'
        status = 'PASS'
        decision = 'EVIDENCE_COMPLETE'
        fakeModuleLoaded = [IO.File]::Exists($moduleMarker)
        fakeCommandInvoked = [IO.File]::Exists($commandMarker)
        predicateProcessPathWasAbsolute = $true
        fakeFirstPathCommandsPresent = $true
        fakePathEntryPrecedesUntrustedExecutables = $true
    }
}

function Write-V43SingleUseJson([string]$Path, $Value) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $parent = [IO.Path]::GetDirectoryName($fullPath)
    if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
    $tempPath = $fullPath + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    $bytes = (New-Object Text.UTF8Encoding($false, $true)).GetBytes(($Value | ConvertTo-Json -Depth 16))
    $stream = $null
    try {
        $stream = New-Object IO.FileStream($tempPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    }
    finally { if ($null -ne $stream) { $stream.Dispose() } }
    try {
        if ([IO.File]::Exists($fullPath)) { throw "Refusing to overwrite single-use evidence: $fullPath" }
        [IO.File]::Move($tempPath, $fullPath)
    }
    finally { if ([IO.File]::Exists($tempPath)) { [IO.File]::Delete($tempPath) } }
    return $fullPath
}

$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('range-fea-v43-save-predicate-' + [Guid]::NewGuid().ToString('N'))
$results = New-Object 'System.Collections.Generic.List[object]'
$failure = $null
$receipt = $null
$clock = [Diagnostics.Stopwatch]::StartNew()
try {
    [void][IO.Directory]::CreateDirectory($tempRoot)
    $fixture = New-V43SaveFixture
    $positive = Get-V43SavePredicateDecision -UiaEvidence $fixture.uia -OperationLog $fixture.log -AuthorityEvidence $fixture.authority -EvaluationTimeUtc '2026-10-14T03:00:00.0000000Z'
    Assert-V43Decision $positive 'EVIDENCE_COMPLETE' 'UNIQUE_CURRENT_AUTHORITY_UIA_AND_OPERATION_CHAIN' 'positive-complete-provenance'
    [void]$results.Add([pscustomobject][ordered]@{ case = 'positive-complete-provenance'; status = 'PASS'; decision = $positive.decision; reasonCode = $positive.reasonCode })

    $missingCases = @(
        [pscustomobject]@{ name = 'missing-uia'; uia = $null; log = $fixture.log; authority = $fixture.authority },
        [pscustomobject]@{ name = 'missing-operation-log'; uia = $fixture.uia; log = $null; authority = $fixture.authority },
        [pscustomobject]@{ name = 'missing-authority'; uia = $fixture.uia; log = $fixture.log; authority = $null }
    )
    foreach ($case in $missingCases) {
        $decision = Get-V43SavePredicateDecision -UiaEvidence $case.uia -OperationLog $case.log -AuthorityEvidence $case.authority -EvaluationTimeUtc '2026-10-14T03:00:00.0000000Z'
        Assert-V43Decision $decision 'EVIDENCE_INCOMPLETE' 'MISSING_INPUT' $case.name
        [void]$results.Add([pscustomobject][ordered]@{ case = $case.name; status = 'PASS'; decision = $decision.decision; reasonCode = $decision.reasonCode })
    }

    $partial = New-V43SaveFixture
    $partial.log.records = @($partial.log.records | Where-Object { $_.eventType -ceq 'artifact.published' })
    $partialDecision = Get-V43SavePredicateDecision -UiaEvidence $partial.uia -OperationLog $partial.log -AuthorityEvidence $partial.authority -EvaluationTimeUtc '2026-10-14T03:00:00.0000000Z'
    Assert-V43Decision $partialDecision 'EVIDENCE_INCOMPLETE' 'PARTIAL_OPERATION_PROVENANCE' 'partial-operation-provenance'
    [void]$results.Add([pscustomobject][ordered]@{ case = 'partial-operation-provenance'; status = 'PASS'; decision = $partialDecision.decision; reasonCode = $partialDecision.reasonCode })

    $duplicate = New-V43SaveFixture
    $duplicate.log.records = @($duplicate.log.records) + @([ordered]@{
        eventId = 'event-publication-duplicate-fixture'
        eventType = 'artifact.published'
        operationId = 'operation-fixture'
        candidateManifestIdentitySha256 = $duplicate.candidateIdentity
        projectId = 'project-fixture'
        documentId = 'document-fixture'
        modelId = 'model-fixture'
        sourceRevision = 'revision-fixture'
        sourceSha256 = $duplicate.sourceSha
        occurredAtUtc = '2026-10-14T02:59:05.0000000Z'
    })
    $duplicateDecision = Get-V43SavePredicateDecision -UiaEvidence $duplicate.uia -OperationLog $duplicate.log -AuthorityEvidence $duplicate.authority -EvaluationTimeUtc '2026-10-14T03:00:00.0000000Z'
    Assert-V43Decision $duplicateDecision 'EVIDENCE_AMBIGUOUS' 'DUPLICATE_PROVENANCE_RECORDS' 'duplicate-operation-provenance'
    [void]$results.Add([pscustomobject][ordered]@{ case = 'duplicate-operation-provenance'; status = 'PASS'; decision = $duplicateDecision.decision; reasonCode = $duplicateDecision.reasonCode })

    $duplicateId = New-V43SaveFixture
    $duplicateId.log.records[1].eventId = $duplicateId.log.records[0].eventId
    $duplicateIdDecision = Get-V43SavePredicateDecision -UiaEvidence $duplicateId.uia -OperationLog $duplicateId.log -AuthorityEvidence $duplicateId.authority -EvaluationTimeUtc '2026-10-14T03:00:00.0000000Z'
    Assert-V43Decision $duplicateIdDecision 'EVIDENCE_AMBIGUOUS' 'DUPLICATE_PROVENANCE_RECORD_ID' 'duplicate-event-id'
    [void]$results.Add([pscustomobject][ordered]@{ case = 'duplicate-event-id'; status = 'PASS'; decision = $duplicateIdDecision.decision; reasonCode = $duplicateIdDecision.reasonCode })

    $staleAuthority = New-V43SaveFixture
    $staleDecision = Get-V43SavePredicateDecision -UiaEvidence $staleAuthority.uia -OperationLog $staleAuthority.log -AuthorityEvidence $staleAuthority.authority -EvaluationTimeUtc '2026-10-14T04:00:00.0000001Z'
    Assert-V43Decision $staleDecision 'EVIDENCE_STALE' 'AUTHORITY_OUTSIDE_VALIDITY_WINDOW' 'expired-authority'
    [void]$results.Add([pscustomobject][ordered]@{ case = 'expired-authority'; status = 'PASS'; decision = $staleDecision.decision; reasonCode = $staleDecision.reasonCode })

    $staleRevision = New-V43SaveFixture
    $staleRevision.uia.sourceRevision = 'older-fixture-revision'
    $staleRevisionDecision = Get-V43SavePredicateDecision -UiaEvidence $staleRevision.uia -OperationLog $staleRevision.log -AuthorityEvidence $staleRevision.authority -EvaluationTimeUtc '2026-10-14T03:00:00.0000000Z'
    Assert-V43Decision $staleRevisionDecision 'EVIDENCE_STALE' 'UIA_AUTHORITY_SOURCEREVISION_MISMATCH' 'stale-uia-revision'
    [void]$results.Add([pscustomobject][ordered]@{ case = 'stale-uia-revision'; status = 'PASS'; decision = $staleRevisionDecision.decision; reasonCode = $staleRevisionDecision.reasonCode })

    $futureObservation = New-V43SaveFixture
    $futureObservation.uia.observedAtUtc = '2026-10-14T03:01:00.0000000Z'
    $futureObservationDecision = Get-V43SavePredicateDecision -UiaEvidence $futureObservation.uia -OperationLog $futureObservation.log -AuthorityEvidence $futureObservation.authority -EvaluationTimeUtc '2026-10-14T03:00:00.0000000Z'
    Assert-V43Decision $futureObservationDecision 'EVIDENCE_STALE' 'UIA_OBSERVATION_FROM_FUTURE' 'future-uia-observation'
    [void]$results.Add([pscustomobject][ordered]@{ case = 'future-uia-observation'; status = 'PASS'; decision = $futureObservationDecision.decision; reasonCode = $futureObservationDecision.reasonCode })

    $staleCandidate = New-V43SaveFixture
    $staleCandidate.log.candidateManifestIdentitySha256 = Get-V43TestSha256Text 'different synthetic candidate identity'
    $staleCandidateDecision = Get-V43SavePredicateDecision -UiaEvidence $staleCandidate.uia -OperationLog $staleCandidate.log -AuthorityEvidence $staleCandidate.authority -EvaluationTimeUtc '2026-10-14T03:00:00.0000000Z'
    Assert-V43Decision $staleCandidateDecision 'EVIDENCE_STALE' 'CANDIDATE_PROVENANCE_MISMATCH' 'stale-candidate-identity'
    [void]$results.Add([pscustomobject][ordered]@{ case = 'stale-candidate-identity'; status = 'PASS'; decision = $staleCandidateDecision.decision; reasonCode = $staleCandidateDecision.reasonCode })

    $dirty = New-V43SaveFixture
    $dirty.uia.dirty = $true
    $dirtyDecision = Get-V43SavePredicateDecision -UiaEvidence $dirty.uia -OperationLog $dirty.log -AuthorityEvidence $dirty.authority -EvaluationTimeUtc '2026-10-14T03:00:00.0000000Z'
    Assert-V43Decision $dirtyDecision 'EVIDENCE_INCOMPLETE' 'UIA_DOCUMENT_NOT_QUIESCENT' 'dirty-uia-document'
    [void]$results.Add([pscustomobject][ordered]@{ case = 'dirty-uia-document'; status = 'PASS'; decision = $dirtyDecision.decision; reasonCode = $dirtyDecision.reasonCode })

    $numericDirty = New-V43SaveFixture
    $numericDirty.uia.dirty = 0
    $numericDirtyDecision = Get-V43SavePredicateDecision -UiaEvidence $numericDirty.uia -OperationLog $numericDirty.log -AuthorityEvidence $numericDirty.authority -EvaluationTimeUtc '2026-10-14T03:00:00.0000000Z'
    Assert-V43Decision $numericDirtyDecision 'EVIDENCE_INVALID' 'UIA_DIRTY_FLAG_NOT_BOOLEAN' 'numeric-zero-dirty-flag'
    [void]$results.Add([pscustomobject][ordered]@{ case = 'numeric-zero-dirty-flag'; status = 'PASS'; decision = $numericDirtyDecision.decision; reasonCode = $numericDirtyDecision.reasonCode })

    $stringDirty = New-V43SaveFixture
    $stringDirty.uia.dirty = 'False'
    $stringDirtyDecision = Get-V43SavePredicateDecision -UiaEvidence $stringDirty.uia -OperationLog $stringDirty.log -AuthorityEvidence $stringDirty.authority -EvaluationTimeUtc '2026-10-14T03:00:00.0000000Z'
    Assert-V43Decision $stringDirtyDecision 'EVIDENCE_INVALID' 'UIA_DIRTY_FLAG_NOT_BOOLEAN' 'string-false-dirty-flag'
    [void]$results.Add([pscustomobject][ordered]@{ case = 'string-false-dirty-flag'; status = 'PASS'; decision = $stringDirtyDecision.decision; reasonCode = $stringDirtyDecision.reasonCode })

    foreach ($arrayCase in @(
        [pscustomobject]@{ name = 'singleton-array-dirty'; field = 'dirty'; value = @($false); decision = 'EVIDENCE_INVALID'; reason = 'UIA_DIRTY_FLAG_NOT_BOOLEAN' },
        [pscustomobject]@{ name = 'empty-array-dirty'; field = 'dirty'; value = @(); decision = 'EVIDENCE_INVALID'; reason = 'UIA_DIRTY_FLAG_NOT_BOOLEAN' },
        [pscustomobject]@{ name = 'singleton-array-document-state'; field = 'documentState'; value = @('SAVED'); decision = 'EVIDENCE_INCOMPLETE'; reason = 'MISSING_FIELD_DOCUMENTSTATE' },
        [pscustomobject]@{ name = 'singleton-array-schema'; field = 'schema'; value = @('range.pc11-v43-save-uia-evidence/v1'); decision = 'EVIDENCE_INVALID'; reason = 'SCHEMA_MISMATCH_UIA_EVIDENCE' },
        [pscustomobject]@{ name = 'singleton-array-uia-timestamp'; field = 'observedAtUtc'; value = @('2026-10-14T02:58:00.0000000Z'); decision = 'EVIDENCE_INCOMPLETE'; reason = 'MISSING_FIELD_OBSERVEDATUTC' }
    )) {
        $arrayFixture = New-V43SaveFixture
        $arrayFixture.uia[$arrayCase.field] = $arrayCase.value
        $arrayDecision = Get-V43SavePredicateDecision -UiaEvidence $arrayFixture.uia -OperationLog $arrayFixture.log -AuthorityEvidence $arrayFixture.authority -EvaluationTimeUtc '2026-10-14T03:00:00.0000000Z'
        Assert-V43Decision $arrayDecision $arrayCase.decision $arrayCase.reason $arrayCase.name
        [void]$results.Add([pscustomobject][ordered]@{ case = $arrayCase.name; status = 'PASS'; decision = $arrayDecision.decision; reasonCode = $arrayDecision.reasonCode })
    }

    $operationIdentityArray = New-V43SaveFixture
    $operationIdentityArray.log.candidateManifestIdentitySha256 = @($operationIdentityArray.candidateIdentity)
    $operationIdentityArrayDecision = Get-V43SavePredicateDecision -UiaEvidence $operationIdentityArray.uia -OperationLog $operationIdentityArray.log -AuthorityEvidence $operationIdentityArray.authority -EvaluationTimeUtc '2026-10-14T03:00:00.0000000Z'
    Assert-V43Decision $operationIdentityArrayDecision 'EVIDENCE_INCOMPLETE' 'MISSING_FIELD_CANDIDATEMANIFESTIDENTITYSHA256' 'singleton-array-operation-identity'
    [void]$results.Add([pscustomobject][ordered]@{ case = 'singleton-array-operation-identity'; status = 'PASS'; decision = $operationIdentityArrayDecision.decision; reasonCode = $operationIdentityArrayDecision.reasonCode })

    $timestampArray = New-V43SaveFixture
    $timestampArray.authority.validFromUtc = @('2026-10-14T02:00:00.0000000Z')
    $timestampArrayDecision = Get-V43SavePredicateDecision -UiaEvidence $timestampArray.uia -OperationLog $timestampArray.log -AuthorityEvidence $timestampArray.authority -EvaluationTimeUtc '2026-10-14T03:00:00.0000000Z'
    Assert-V43Decision $timestampArrayDecision 'EVIDENCE_INCOMPLETE' 'MISSING_FIELD_VALIDFROMUTC' 'singleton-array-authority-timestamp'
    [void]$results.Add([pscustomobject][ordered]@{ case = 'singleton-array-authority-timestamp'; status = 'PASS'; decision = $timestampArrayDecision.decision; reasonCode = $timestampArrayDecision.reasonCode })

    $fieldArray = @('identity-preserved')
    $fieldHolder = [pscustomobject]@{ value = $fieldArray }
    $dictField = Get-V43SaveField $fieldHolder 'value'
    $objectField = Get-V43SaveField @{ value = $fieldArray } 'value'
    if ($dictField -isnot [array] -or $objectField -isnot [array] -or
        -not [object]::ReferenceEquals($dictField, $fieldArray) -or -not [object]::ReferenceEquals($objectField, $fieldArray)) {
        throw 'Get-V43SaveField did not preserve singleton-array identity for dictionary and object properties.'
    }
    [void]$results.Add([pscustomobject][ordered]@{ case = 'singleton-array-identity'; status = 'PASS'; dictionaryArrayPreserved = $true; objectArrayPreserved = $true })

    $emptyField = Get-V43SaveField @{ value = @() } 'value'
    if ($emptyField -isnot [array] -or $emptyField.Count -ne 0) { throw 'Get-V43SaveField did not preserve an empty array.' }
    [void]$results.Add([pscustomobject][ordered]@{ case = 'empty-array-identity'; status = 'PASS'; count = $emptyField.Count })

    $environmentResult = Invoke-V43FakeFirstEnvironmentCase $tempRoot $predicatePath
    [void]$results.Add($environmentResult)
}
catch { $failure = $_.Exception.Message }
finally {
    $clock.Stop()
    if ([IO.Directory]::Exists($tempRoot)) {
        $resolvedTemp = [IO.Path]::GetFullPath($tempRoot)
        $name = [IO.Path]::GetFileName($resolvedTemp)
        $attributes = [IO.File]::GetAttributes($resolvedTemp)
        if (-not $resolvedTemp.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -or
            $name -notmatch '^range-fea-v43-save-predicate-[0-9a-f]{32}$' -or
            (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw 'Refusing to remove a temporary Save-predicate fixture tree outside its verified system-temp root.'
        }
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}

$sourceHashes = @(
    [pscustomobject][ordered]@{ path = 'tools/save-predicate-v43.ps1'; sha256 = Get-V43TestSha256File $predicatePath },
    [pscustomobject][ordered]@{ path = 'tests/test-save-predicate-v43.ps1'; sha256 = Get-V43TestSha256File $MyInvocation.MyCommand.Path }
)
$status = if ($null -eq $failure) { 'STATIC_INCOMPLETE' } else { 'FAIL' }
$receipt = [ordered]@{
    schema = 'range.pc11-v43-save-predicate-fixture-receipt/v1'
    status = $status
    staticContractSuiteStatus = if ($null -eq $failure) { 'PASS' } else { 'FAIL' }
    candidateGateStatus = 'PENDING'
    candidateVersion = 'v43'
    variant = 'OFF'
    staticOnly = $true
    fixtureOnly = $true
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    candidateManifestIdentitySha256 = $null
    sourceTreeSha256 = $null
    requiredStaticTestIdsCovered = @('save_predicate')
    caseCount = $results.Count
    cases = $results.ToArray()
    sourceSha256 = $sourceHashes
    failure = $failure
    productReadiness = 'PENDING_FINAL_CANDIDATE_BINDING_AND_POST_GO_PRODUCT_ACCEPTANCE'
    productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
    goPermitted = $false
    interpretation = 'Synthetic evidence decisions only. This fixture neither performs nor authorizes product Save; actual product Save remains a separate post-GO acceptance action.'
}

$writtenPath = Write-V43SingleUseJson $OutputPath $receipt
Write-Output ("{0} fixtureCases={1} candidateGate={2} productActions=NONE output={3}" -f $receipt.status, $receipt.caseCount, $receipt.candidateGateStatus, $writtenPath)
if ($null -ne $failure) {
    [Console]::Error.WriteLine($failure)
    exit 1
}
