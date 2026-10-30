$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'contract-common-v43.ps1')
. (Join-Path $PSScriptRoot 'native-launch-boundary-v43.ps1')

function Invoke-V43StaticFixtureSupervisor {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$CandidateManifestPath,
        [Parameter(Mandatory = $true)][string]$CandidateManifestSha256,
        [Parameter(Mandatory = $true)][string]$CandidateManifestIdentitySha256,
        [Parameter(Mandatory = $true)][string]$FixtureEvidenceRoot,
        [int]$TimeoutSeconds = 20,
        [switch]$TestOnlySimulateAssignmentFailure
    )

    if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
        throw 'The v43 static fixture supervisor requires Windows PowerShell 5.1.'
    }
    if ($TimeoutSeconds -lt 2 -or $TimeoutSeconds -gt 60) { throw 'Fixture supervisor timeout must be between 2 and 60 seconds.' }
    $candidate = Assert-V43CandidateManifest $CandidateManifestPath $CandidateManifestSha256 $CandidateManifestIdentitySha256
    $fixtureRoot = [IO.Path]::GetFullPath($FixtureEvidenceRoot)
    if (-not [IO.Directory]::Exists($fixtureRoot)) { throw 'FixtureEvidenceRoot must already exist and be isolated by the caller.' }

    $workerPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\tests\fixtures\worker-fixture-v43.ps1'))
    $expectedWorker = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $PSScriptRoot) 'tests\fixtures\worker-fixture-v43.ps1'))
    $handshakePath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'worker-handshake-v43.ps1'))
    if ($workerPath -ine $expectedWorker -or -not [IO.File]::Exists($workerPath) -or -not [IO.File]::Exists($handshakePath)) {
        throw 'The v43 fixture supervisor accepts only its package-local worker and handshake implementation.'
    }
    $workerSha = Get-V43FileSha256 $workerPath
    $handshakeSha = Get-V43FileSha256 $handshakePath
    foreach ($sourcePath in @($workerPath, $handshakePath)) {
        $sourceText = (New-Object Text.UTF8Encoding($false, $true)).GetString([IO.File]::ReadAllBytes($sourcePath))
        $sourceTokens = $null
        $sourceErrors = $null
        [void][Management.Automation.Language.Parser]::ParseInput($sourceText, [ref]$sourceTokens, [ref]$sourceErrors)
        if (@($sourceErrors).Count -gt 0) { throw "v43 fixture source failed PowerShell parsing: $sourcePath" }
    }

    $runRoot = Join-Path $fixtureRoot ('supervisor-v43-' + [Guid]::NewGuid().ToString('N'))
    if (-not (Test-V43PathUnderRoot $runRoot $fixtureRoot)) { throw 'Supervisor fixture run path escaped FixtureEvidenceRoot.' }
    [void][IO.Directory]::CreateDirectory($runRoot)
    $readyPath = Join-Path $runRoot 'ready.json'
    $terminalPath = Join-Path $runRoot 'terminal.json'
    $workerErrorPath = Join-Path $runRoot 'worker.error'
    $token = [Guid]::NewGuid().ToString('N')
    $powerShell = [IO.Path]::GetFullPath((Join-Path $PSHOME 'powershell.exe'))
    if (-not [IO.File]::Exists($powerShell)) { throw 'Windows PowerShell 5.1 executable was not found under PSHOME.' }

    $payload = [ordered]@{
        CandidateManifestPath = $candidate.path
        CandidateManifestSha256 = $candidate.sha256
        CandidateManifestIdentitySha256 = $candidate.candidateManifestIdentitySha256
        EvidenceRoot = $runRoot
        SupervisorReadyPath = $readyPath
        SupervisorTerminalPath = $terminalPath
        SupervisorToken = $token
        SupervisorPid = [int]$PID
        FixtureScriptSha256 = $workerSha
        HandshakeScriptSha256 = $handshakeSha
        WorkerErrorPath = $workerErrorPath
    }
    $payloadJson = ConvertTo-Json -InputObject $payload -Compress -Depth 8
    $payloadBase64 = [Convert]::ToBase64String((New-Object Text.UTF8Encoding($false, $true)).GetBytes($payloadJson))
    $workerPathBase64 = [Convert]::ToBase64String((New-Object Text.UTF8Encoding($false, $true)).GetBytes($workerPath))
    $childText = @'
$ErrorActionPreference = 'Stop'
try {
    $payloadJson = (New-Object Text.UTF8Encoding($false, $true)).GetString([Convert]::FromBase64String('__PAYLOAD__'))
    $payloadObject = $payloadJson | ConvertFrom-Json -ErrorAction Stop
    $payload = @{}
    foreach ($property in $payloadObject.PSObject.Properties) { $payload[$property.Name] = $property.Value }
    $workerPath = (New-Object Text.UTF8Encoding($false, $true)).GetString([Convert]::FromBase64String('__WORKER__'))
    & $workerPath @payload
    exit $LASTEXITCODE
}
catch {
    try {
        $payloadJson = (New-Object Text.UTF8Encoding($false, $true)).GetString([Convert]::FromBase64String('__PAYLOAD__'))
        $payload = $payloadJson | ConvertFrom-Json -ErrorAction Stop
        [IO.File]::WriteAllText([string]$payload.WorkerErrorPath, $_.Exception.Message, (New-Object Text.UTF8Encoding($false)))
    }
    catch { }
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 73
}
'@
    $childText = $childText.Replace('__PAYLOAD__', $payloadBase64).Replace('__WORKER__', $workerPathBase64)
    $commandLine = New-V43EncodedPowerShellCommand $powerShell $childText
    $job = [IntPtr]::Zero
    $processInfo = $null
    $threadHandle = [IntPtr]::Zero
    $processHandle = [IntPtr]::Zero
    $workerAssigned = $false
    $exactMembership = $false
    $workerResumed = $false
    $terminalAccepted = $false
    $workerExitCode = $null
    try {
        $job = [V43NativeLaunchBoundary]::CreateKillOnCloseJob('Local\RangeV43-Worker-' + $token)
        $processInfo = [V43NativeLaunchBoundary]::StartSuspended($powerShell, $commandLine, $runRoot)
        $processHandle = $processInfo.process
        $threadHandle = $processInfo.thread
        $assignmentSucceeded = $false
        if (-not $TestOnlySimulateAssignmentFailure) {
            $assignmentSucceeded = [V43NativeLaunchBoundary]::AssignProcessToJobObject($job, $processHandle)
        }
        if (-not $assignmentSucceeded) {
            throw 'Suspended fixture worker could not be assigned to the supervisor Job.'
        }
        $workerAssigned = $true
        $member = $false
        if (-not [V43NativeLaunchBoundary]::IsProcessInJob($processHandle, $job, [ref]$member) -or -not $member) {
            throw 'Suspended fixture worker is not proven to belong to the exact supervisor Job.'
        }
        $exactMembership = $true
        $initialSuspendCount = [V43NativeLaunchBoundary]::ResumeThread($threadHandle)
        if ($initialSuspendCount -ne 1) { throw "Fixture worker initial suspend count was not one: $initialSuspendCount" }
        $workerResumed = $true
        [void][V43NativeLaunchBoundary]::CloseHandle($threadHandle)
        $threadHandle = [IntPtr]::Zero

        $readyReceipt = [ordered]@{
            schema = 'range.pc11-v43-worker-handshake/v1'
            issuedBy = 'supervisor-contract-v43'
            candidateVersion = 'v43'
            candidateManifestSha256 = $candidate.sha256
            candidateManifestIdentitySha256 = $candidate.candidateManifestIdentitySha256
            token = $token
            supervisorPid = [int]$PID
            workerPid = [int]$processInfo.processId
            productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
            goPermitted = $false
        }
        $null = Write-V43JsonNewFile $readyPath $readyReceipt 8
        $timer = [Diagnostics.Stopwatch]::StartNew()
        while (-not [IO.File]::Exists($terminalPath)) {
            $wait = [V43NativeLaunchBoundary]::WaitForSingleObject($processHandle, 25)
            if ($wait -eq 0) { break }
            if ($wait -ne 258) { throw 'Native supervisor wait returned an unexpected status.' }
            if ($timer.Elapsed.TotalSeconds -ge $TimeoutSeconds) { throw 'v43 fixture worker did not publish a terminal receipt before its deadline.' }
        }
        if (-not [IO.File]::Exists($terminalPath)) {
            $exitCodeOnMissingTerminal = [uint32]::MaxValue
            [void][V43NativeLaunchBoundary]::GetExitCodeProcess($processHandle, [ref]$exitCodeOnMissingTerminal)
            $workerError = if ([IO.File]::Exists($workerErrorPath)) { [IO.File]::ReadAllText($workerErrorPath) } else { 'no worker diagnostic was published' }
            throw "Fixture worker exited without publishing its terminal receipt (exit=$exitCodeOnMissingTerminal): $workerError"
        }
        $terminalBytes = [IO.File]::ReadAllBytes($terminalPath)
        if ($terminalBytes.Length -gt 65536) { throw 'Fixture worker terminal exceeds the bounded JSON size.' }
        $terminalText = (New-Object Text.UTF8Encoding($false, $true)).GetString($terminalBytes)
        $terminal = $terminalText | ConvertFrom-Json -ErrorAction Stop
        Assert-V43NoGoEnvelope $terminal 'v43 fixture worker terminal' $candidate.candidateManifestIdentitySha256
        if ($terminal.schema -cne 'range.pc11-v43-worker-terminal/v1' -or $terminal.status -cne 'FIXTURE_COMPLETE' -or
            $terminal.fixtureOnly -ne $true -or $terminal.candidateManifestSha256 -cne $candidate.sha256 -or
            $terminal.supervisorToken -cne $token -or [int]$terminal.workerPid -ne [int]$processInfo.processId -or
            [int]$terminal.supervisorPid -ne [int]$PID) {
            throw 'v43 fixture worker terminal did not match the candidate, token, or owned worker process.'
        }
        $terminalAccepted = $true
        if ([V43NativeLaunchBoundary]::WaitForSingleObject($processHandle, 5000) -ne 0) {
            throw 'Fixture worker did not exit after publishing its terminal receipt.'
        }
        $nativeExitCode = [uint32]::MaxValue
        if (-not [V43NativeLaunchBoundary]::GetExitCodeProcess($processHandle, [ref]$nativeExitCode) -or $nativeExitCode -eq 259) {
            throw 'Fixture worker terminal process exit code could not be read after its wait.'
        }
        $workerExitCode = [uint32]$nativeExitCode
        if ($workerExitCode -ne 0) { throw "Fixture worker exit code was $workerExitCode." }

        $receipt = [ordered]@{
            schema = 'range.pc11-v43-supervisor-fixture-receipt/v1'
            status = 'FIXTURE_COMPLETE'
            candidateVersion = 'v43'
            variant = 'OFF'
            staticOnly = $true
            fixtureOnly = $true
            candidateManifestSha256 = $candidate.sha256
            candidateManifestIdentitySha256 = $candidate.candidateManifestIdentitySha256
            workerScriptSha256 = $workerSha
            workerHandshakeScriptSha256 = $handshakeSha
            supervisorPid = [int]$PID
            workerPid = [int]$processInfo.processId
            supervisorToken = $token
            workerCreatedSuspended = $true
            workerAssignedBeforeResume = $workerAssigned
            exactWorkerJobMembershipVerified = $exactMembership
            workerInitialSuspendCount = [int]$initialSuspendCount
            workerResumedAfterExactMembership = $workerResumed
            killOnJobClose = $true
            workerTerminalSha256 = Get-V43FileSha256 $terminalPath
            workerExitCode = $workerExitCode
            terminalReceiptAccepted = $terminalAccepted
            productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
            goPermitted = $false
        }
        Assert-V43NoGoEnvelope $receipt 'v43 fixture supervisor receipt' $candidate.candidateManifestIdentitySha256
        $receiptPath = Join-Path $runRoot 'supervisor-receipt.json'
        $null = Write-V43JsonNewFile $receiptPath $receipt 12
        return [pscustomobject]@{
            success = $true
            status = 'FIXTURE_COMPLETE'
            candidateManifestSha256 = $candidate.sha256
            candidateManifestIdentitySha256 = $candidate.candidateManifestIdentitySha256
            evidenceRoot = $runRoot
            workerPid = [int]$processInfo.processId
            workerCreatedSuspended = $true
            workerAssignedBeforeResume = $workerAssigned
            exactWorkerJobMembershipVerified = $exactMembership
            workerInitialSuspendCount = [int]$initialSuspendCount
            workerResumedAfterExactMembership = $workerResumed
            killOnJobClose = $true
            workerExitCode = $workerExitCode
            receiptSha256 = Get-V43FileSha256 $receiptPath
            productActions = $receipt.productActions
            goPermitted = $false
        }
    }
    catch {
        $primaryError = $_
        if ($processHandle -ne [IntPtr]::Zero) {
            $cleanup = [ordered]@{
                workerPid = if ($null -ne $processInfo) { [int]$processInfo.processId } else { $null }
                workerAssigned = [bool]$workerAssigned
                terminationAttempted = $true
                terminationAccepted = $false
                processWaitStatus = $null
                processExitCode = $null
                processDrained = $false
            }
            if ($workerAssigned -and $job -ne [IntPtr]::Zero) {
                try {
                    if ([V43NativeLaunchBoundary]::TerminateJobObject($job, 125)) { $cleanup.terminationAccepted = $true }
                }
                catch { }
            }
            try {
                if ([V43NativeLaunchBoundary]::TerminateProcess($processHandle, 125)) { $cleanup.terminationAccepted = $true }
            }
            catch { }
            try {
                $waitStatus = [uint32][V43NativeLaunchBoundary]::WaitForSingleObject($processHandle, 5000)
                if ($waitStatus -ne 0) {
                    try {
                        if ([V43NativeLaunchBoundary]::TerminateProcess($processHandle, 125)) { $cleanup.terminationAccepted = $true }
                    }
                    catch { }
                    $waitStatus = [uint32][V43NativeLaunchBoundary]::WaitForSingleObject($processHandle, 5000)
                }
                $cleanup.processWaitStatus = $waitStatus
                if ($waitStatus -eq 0) {
                    $exitCode = [uint32]::MaxValue
                    if ([V43NativeLaunchBoundary]::GetExitCodeProcess($processHandle, [ref]$exitCode)) {
                        $cleanup.processExitCode = [uint32]$exitCode
                        $cleanup.processDrained = ($exitCode -ne 259)
                    }
                }
            }
            catch { }
            try { $primaryError.Exception.Data['v43SupervisorCleanup'] = [pscustomobject]$cleanup }
            catch { }
        }
        throw
    }
    finally {
        if ($threadHandle -ne [IntPtr]::Zero) { [void][V43NativeLaunchBoundary]::CloseHandle($threadHandle) }
        if ($processHandle -ne [IntPtr]::Zero) { [void][V43NativeLaunchBoundary]::CloseHandle($processHandle) }
        if ($job -ne [IntPtr]::Zero) { [void][V43NativeLaunchBoundary]::CloseHandle($job) }
    }
}
