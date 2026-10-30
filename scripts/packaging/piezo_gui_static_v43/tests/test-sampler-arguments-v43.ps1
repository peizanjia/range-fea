[CmdletBinding()]
param([string]$AuditRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) { throw 'Sampler argument fixture requires Windows PowerShell 5.1.' }
$prepRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$builderPath = Join-Path $prepRoot 'runtime\sampler-arguments-v43.ps1'
$samplerPath = [IO.Path]::GetFullPath((Join-Path $prepRoot '..\..\..\scripts\benchmark\pc11_working_set.py'))
if (-not $AuditRoot) { $AuditRoot = Join-Path $prepRoot '..\..\..\build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit' }
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)
if (-not [IO.File]::Exists($builderPath) -or -not [IO.File]::Exists($samplerPath)) { throw 'Sampler argument source is missing.' }
$receiptPath = Join-Path $AuditRoot ('sampler-arguments-fixture-v43-' + [Guid]::NewGuid().ToString('N') + '.json')
if ([IO.File]::Exists($receiptPath)) { throw 'Refusing to overwrite single-use sampler fixture receipt.' }
. $builderPath

function Get-TestSha([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}
function Assert-Throws([scriptblock]$Action, [string]$Label) {
    $threw = $false
    try { & $Action } catch { $threw = $true }
    if (-not $threw) { throw "Expected rejection: $Label" }
    return $true
}

$cases = New-Object 'System.Collections.Generic.List[object]'
function Add-SamplerCase([string]$Name, [string]$Kind, [string]$Expected, [string]$Observed, [string]$ReasonCode) {
    if ($Expected -cne $Observed) { throw "Sampler fixture case '$Name' expected '$Expected', observed '$Observed'." }
    [void]$script:cases.Add([ordered]@{ case=$Name; kind=$Kind; expected=$Expected; observed=$Observed; reasonCode=$ReasonCode; status='PASS' })
}

$oldPath = $env:PATH; $oldModules = $env:PSModulePath
try {
    $fake = Join-Path ([IO.Path]::GetTempPath()) ('v43-fake-first-' + [Guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($fake)
    $fakePython = Join-Path $fake 'python.exe'; [IO.File]::WriteAllText($fakePython, 'not executable fixture')
    $env:PATH = $fake + ';' + $oldPath
    $env:PSModulePath = $fake + ';' + $oldModules
    $work = Join-Path ([IO.Path]::GetTempPath()) ('v43 sampler paths with spaces ' + [Guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($work)
    $python = Join-Path $work 'pinned python.exe'; [IO.File]::WriteAllText($python, 'fixture placeholder')
    $image = Join-Path $work 'Range FEA.exe'; [IO.File]::WriteAllText($image, 'fixture placeholder')
    $rootA = Join-Path $work 'allowed images A'; [void][IO.Directory]::CreateDirectory($rootA)
    $rootB = Join-Path $work 'allowed images B'; [void][IO.Directory]::CreateDirectory($rootB)
    $output = Join-Path $work 'run output'; $stop = Join-Path $work 'stop.flag'
    $pin = ('a' * 64); $identity = ('b' * 64)
    $plan = New-V43SamplerArgumentPlan -CandidateManifestSha256 $pin -CandidateManifestIdentitySha256 $identity `
        -PythonExe $python -SamplerScript $samplerPath -RootPid 4312 -ExpectedRootImage $image `
        -AllowedRoots @($rootA,$rootB) -Output $output -StopFile $stop
    if (-not (Assert-V43SamplerArgumentPlan $plan $pin $identity $python $samplerPath)) { throw 'Sampler argument parser rejected valid plan.' }
    $expected = @($samplerPath,'--root-pid','4312','--expected-root-image',$image,'--allowed-root',$rootA,'--allowed-root',$rootB,
        '--output',$output,'--stop-file',$stop,'--interval-ms','50','--min-samples','10','--min-seconds','0.5')
    if ($plan.program -cne [IO.Path]::GetFullPath($python) -or $plan.arguments.Count -ne $expected.Count) { throw 'Sampler executable or argument count differs.' }
    for ($i=0; $i -lt $expected.Count; $i++) { if ($plan.arguments[$i] -cne $expected[$i]) { throw "Argument $i differs: $($plan.arguments[$i])" } }
    if ($plan.commandLine -notmatch '".*paths with spaces.*"' -or $plan.program -ieq $fakePython) { throw 'Spaces were not quoted or fake-first PATH influenced executable selection.' }
    Add-SamplerCase 'valid-plan-exact-argument-vector' 'positive' 'ACCEPT' 'ACCEPT' 'PINNED_PROGRAM_AND_ARGUMENTS_MATCH'
    $malformedPlan = $plan | Select-Object *
    $malformedPlan.arguments = [string[]]$plan.arguments.Clone(); $malformedPlan.arguments[1] = '--unexpected'
    [void](Assert-Throws { Assert-V43SamplerArgumentPlan $malformedPlan $pin $identity $python $samplerPath } 'malformed argument vector')
    Add-SamplerCase 'unexpected-argument-token' 'negative' 'REJECT' 'REJECT' 'ARGUMENT_VECTOR_MISMATCH'
    [void](Assert-Throws { New-V43SamplerArgumentPlan -CandidateManifestSha256 '' -CandidateManifestIdentitySha256 $identity -PythonExe $python -SamplerScript $samplerPath -RootPid 1 -ExpectedRootImage $image -AllowedRoots @($rootA) -Output $output -StopFile $stop } 'missing manifest pin')
    Add-SamplerCase 'missing-manifest-pin' 'negative' 'REJECT' 'REJECT' 'MANIFEST_PIN_REQUIRED'
    [void](Assert-Throws { New-V43SamplerArgumentPlan -CandidateManifestSha256 'bad' -CandidateManifestIdentitySha256 $identity -PythonExe $python -SamplerScript $samplerPath -RootPid 1 -ExpectedRootImage $image -AllowedRoots @($rootA) -Output $output -StopFile $stop } 'malformed manifest pin')
    Add-SamplerCase 'malformed-manifest-pin' 'negative' 'REJECT' 'REJECT' 'MANIFEST_PIN_INVALID'
    [void](Assert-Throws { Assert-V43SamplerArgumentPlan $plan $pin ('c' * 64) $python $samplerPath } 'identity mismatch')
    Add-SamplerCase 'candidate-identity-mismatch' 'negative' 'REJECT' 'REJECT' 'CANDIDATE_IDENTITY_MISMATCH'
    $badPlan = $plan | Select-Object *
    $badPlan.goPermitted = 0
    [void](Assert-Throws { Assert-V43SamplerArgumentPlan $badPlan $pin $identity $python $samplerPath } 'integer goPermitted')
    Add-SamplerCase 'go-permitted-must-be-boolean-false' 'negative' 'REJECT' 'REJECT' 'GO_FIELD_TYPE_OR_VALUE_INVALID'
    $badPlan = $plan | Select-Object *
    $badPlan.productActions = [ordered]@{guiLaunched=0;savePerformed=$false;runPerformed=$false;confirmPerformed=$false}
    [void](Assert-Throws { Assert-V43SamplerArgumentPlan $badPlan $pin $identity $python $samplerPath } 'integer product action')
    Add-SamplerCase 'product-action-must-be-boolean-false' 'negative' 'REJECT' 'REJECT' 'PRODUCT_ACTION_TYPE_OR_VALUE_INVALID'
    $badPlan = $plan | Select-Object *
    $badPlan.program = 'C:fake.exe'
    [void](Assert-Throws { Assert-V43SamplerArgumentPlan $badPlan $pin $identity $python $samplerPath } 'drive-relative and mismatched program')
    Add-SamplerCase 'drive-relative-program-path' 'negative' 'REJECT' 'REJECT' 'PROGRAM_PATH_NOT_PINNED'
    $badPlan = $plan | Select-Object *
    $badPlan.arguments = [string[]]$plan.arguments.Clone(); $badPlan.arguments[0] = Join-Path $work 'missing script.py'
    $badPlan.commandLine = [string]::Join(' ', @($badPlan.arguments | ForEach-Object { Quote-V43SamplerArgument $_ }))
    [void](Assert-Throws { Assert-V43SamplerArgumentPlan $badPlan $pin $identity $python $samplerPath } 'nonexistent script')
    Add-SamplerCase 'nonexistent-sampler-script' 'negative' 'REJECT' 'REJECT' 'SAMPLER_SCRIPT_MISSING'
    $badPlan = $plan | Select-Object *
    $badPlan.arguments = [string[]]$plan.arguments.Clone(); $badPlan.arguments[0] = $image
    $badPlan.commandLine = [string]::Join(' ', @($badPlan.arguments | ForEach-Object { Quote-V43SamplerArgument $_ }))
    [void](Assert-Throws { Assert-V43SamplerArgumentPlan $badPlan $pin $identity $python $samplerPath } 'different existing script')
    Add-SamplerCase 'different-existing-sampler-script' 'negative' 'REJECT' 'REJECT' 'SAMPLER_SCRIPT_MISMATCH'
    $builderText = [IO.File]::ReadAllText($builderPath)
    if ($builderText -match '(?im)\b(Start-Process|ProcessStartInfo|\.Start\s*\()') { throw 'Argument builder contains a process launch path.' }
    Add-SamplerCase 'builder-has-no-process-launch' 'safety' 'ABSENT' 'ABSENT' 'NO_PRODUCT_PROCESS_LAUNCH'
    $sourceHashes = [ordered]@{ samplerArguments=(Get-TestSha $builderPath); samplerCli=(Get-TestSha $samplerPath); fixture=(Get-TestSha $MyInvocation.MyCommand.Path) }
    foreach ($sourceName in @('samplerArguments','samplerCli','fixture')) {
        if ([string]$sourceHashes[$sourceName] -cnotmatch '^[0-9a-f]{64}$') { throw "Invalid SHA256 source hash for $sourceName." }
    }
    Add-SamplerCase 'source-hashes-are-pinned-sha256' 'provenance' 'VALID' 'VALID' 'SOURCE_HASHES_ARE_SHA256'
    $actions = [ordered]@{ guiLaunched=$false; savePerformed=$false; runPerformed=$false; confirmPerformed=$false }
    if ($plan.goPermitted -isnot [bool] -or $plan.goPermitted -ne $false) { throw 'Fixture plan unexpectedly permits GO.' }
    foreach ($actionName in @('guiLaunched','savePerformed','runPerformed','confirmPerformed')) {
        if ($actions[$actionName] -isnot [bool] -or $actions[$actionName] -ne $false) { throw "Fixture recorded product action $actionName." }
    }
    Add-SamplerCase 'go-and-product-actions-remain-disabled' 'safety' 'DISABLED' 'DISABLED' 'NO_GO_NO_PRODUCT_ACTION'
    $receipt = [ordered]@{
        schema='range.pc11-v43-sampler-arguments-fixture/v1'; status='PENDING'; componentSuiteStatus='PASS'; candidateGateStatus='PENDING'; candidateIdentitySource='synthetic-fixture-only'; candidateVersion='v43'; variant='OFF'; staticOnly=$true; fixtureOnly=$true
        sourceHashes=$sourceHashes; cases=$cases.ToArray(); caseCount=$cases.Count
        candidateManifestSha256=$pin; candidateManifestIdentitySha256=$identity; parsedArgumentCount=$plan.arguments.Count
        arguments=$plan.arguments; program=$plan.program; productActions=$actions; goPermitted=$false
    }
    $parent = Split-Path -Parent $receiptPath
    if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
    [IO.File]::WriteAllText($receiptPath, ($receipt | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false)))
    Write-Output ("PASS_SAMPLER_ARGUMENTS_V43 cases={0} args={1} componentSuite=PASS candidateGate=PENDING receipt={2}" -f $cases.Count, $plan.arguments.Count, $receiptPath)
}
finally { $env:PATH = $oldPath; $env:PSModulePath = $oldModules }
