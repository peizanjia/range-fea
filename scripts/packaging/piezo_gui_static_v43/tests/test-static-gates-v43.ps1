[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$AuditRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'The v43 static-gate assessment requires Windows PowerShell 5.1.'
}

$prepRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $AuditRoot) { $AuditRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..\build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit')) }
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$auditPrefix = $AuditRoot.TrimEnd('\') + '\'
if (-not $OutputPath.StartsWith($auditPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The v43 static-gate assessment receipt must remain under the external audit root.'
}
if ([IO.File]::Exists($OutputPath)) { throw "Refusing to overwrite v43 assessment evidence: $OutputPath" }

function Get-Sha256File([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
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

function Invoke-V43Child([string]$ScriptPath, [hashtable]$Environment) {
    $powerShellPath = [IO.Path]::GetFullPath((Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'))
    $arguments = @('-NoProfile', '-NoLogo', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $ScriptPath)
    $quoted = New-Object 'System.Collections.Generic.List[string]'
    foreach ($argument in $arguments) { [void]$quoted.Add((Quote-WindowsArgument ([string]$argument))) }
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $powerShellPath
    $startInfo.Arguments = [string]::Join(' ', $quoted.ToArray())
    $startInfo.WorkingDirectory = $prepRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($name in $Environment.Keys) { $startInfo.EnvironmentVariables[[string]$name] = [string]$Environment[$name] }
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) { throw 'Could not start an isolated Windows PowerShell v43 module-boundary case.' }
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $stdout = $process.StandardOutput.ReadToEnd()
        $process.WaitForExit()
        return [pscustomobject]@{ exitCode = [int]$process.ExitCode; stdout = $stdout; stderr = [string]$stderrTask.Result }
    }
    finally { $process.Dispose() }
}

function New-V43ChildScript([string]$Path, [string]$Body) {
    $utf8 = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($Path, $Body, $utf8)
}

function Read-V43ChildResult([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { throw "Isolated v43 module-boundary child did not write its result: $Path" }
    $text = (New-Object Text.UTF8Encoding($false, $true)).GetString([IO.File]::ReadAllBytes($Path))
    return ($text | ConvertFrom-Json -ErrorAction Stop)
}

function New-FakeModule([string]$Root, [string]$ModuleName, [string]$ExportName, [string]$ModuleMarker, [string]$CommandMarker) {
    $moduleDirectory = Join-Path $Root $ModuleName
    [void][IO.Directory]::CreateDirectory($moduleDirectory)
    $manifestPath = Join-Path $moduleDirectory ($ModuleName + '.psd1')
    $modulePath = Join-Path $moduleDirectory ($ModuleName + '.psm1')
    $guid = [Guid]::NewGuid().ToString()
    $manifest = "@{ RootModule='$ModuleName.psm1'; ModuleVersion='1.0.0'; GUID='$guid'; FunctionsToExport=@('$ExportName') }`n"
    $module = @"
[IO.File]::WriteAllText(`$env:V43_FAKE_MODULE_MARKER, 'loaded')
function $ExportName {
    [IO.File]::WriteAllText(`$env:V43_FAKE_COMMAND_MARKER, 'executed')
    return 'forged'
}
Export-ModuleMember -Function $ExportName
"@
    [IO.File]::WriteAllText($manifestPath, $manifest, (New-Object Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText($modulePath, $module, (New-Object Text.UTF8Encoding($false)))
    return $manifestPath
}

function Get-ContractIds([string]$Path, [string]$Pattern, [string]$Label) {
    if (-not [IO.File]::Exists($Path)) { throw "$Label source is missing: $Path" }
    $text = (New-Object Text.UTF8Encoding($false, $true)).GetString([IO.File]::ReadAllBytes($Path))
    $section = [regex]::Match($text, $Pattern, [Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $section.Success) { throw "$Label does not declare its required static-test IDs." }
    return ,@([regex]::Matches($section.Groups['items'].Value, '(?<quote>[''"])(?<id>[a-z_]+)\k<quote>') | ForEach-Object { [string]$_.Groups['id'].Value })
}

$requiredIds = @(
    'module_boundary_build', 'module_boundary_source', 'watchdog_preflight', 'save_predicate_preflight',
    'path_shadow', 'root_seal', 'save_predicate', 'watchdog_contract', 'sampler_arguments', 'supervisor',
    'worker_handshake', 'native_launch_boundary', 'parser', 'finalize_integrity', 'root_bootstrap'
)
$testedResults = @{}
$testCases = New-Object 'System.Collections.Generic.List[object]'
$failure = $null
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('range-fea-v43-module-boundary-' + [Guid]::NewGuid().ToString('N'))

try {
    [void][IO.Directory]::CreateDirectory($tempRoot)
    $admissionIds = Get-ContractIds (Join-Path $prepRoot 'admission_v43.py') 'REQUIRED_STATIC_TEST_IDS\s*=\s*\((?<items>.*?)\)' 'v43 admission'
    $runnerIds = Get-ContractIds (Join-Path $PSScriptRoot 'test-static-chain-v43.ps1') '\$script:RequiredTestIds\s*=\s*@\((?<items>.*?)\)' 'v43 static-chain runner'
    if (($admissionIds -join "`n") -cne ($runnerIds -join "`n")) { throw 'Admission and static-chain runner required gate IDs differ.' }
    $alreadySatisfiedIds = @('source_preflight', 'source_freeze', 'package_tree', 'source_static_tests')
    $pendingAdmissionIds = @($admissionIds | Where-Object { $_ -notin $alreadySatisfiedIds })
    if (($pendingAdmissionIds -join "`n") -cne ($requiredIds -join "`n")) { throw 'The 15 assessment IDs are not the exact remaining v43 static contract IDs.' }
    [void]$testCases.Add([pscustomobject]@{ case = 'admission-and-runner-id-parity'; status = 'PASS'; gateCount = $admissionIds.Count; assessedGateCount = $pendingAdmissionIds.Count })

    $helperPath = Join-Path $prepRoot 'tools\system-module-boundary-v43.ps1'
    $helperLiteral = "'" + $helperPath.Replace("'", "''") + "'"
    $positiveScriptPath = Join-Path $tempRoot 'positive.ps1'
    $positiveOutputPath = Join-Path $tempRoot 'positive.json'
    $positiveBody = @"
`$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. $helperLiteral
`$result = Assert-V43SystemModuleBoundary
[IO.File]::WriteAllText(`$env:V43_CHILD_OUTPUT, (`$result | ConvertTo-Json -Depth 8 -Compress), (New-Object Text.UTF8Encoding(`$false)))
"@
    New-V43ChildScript $positiveScriptPath $positiveBody
    $baseModulePath = Join-Path $PSHOME 'Modules'
    $cleanEnvironment = @{
        V43_CHILD_OUTPUT = $positiveOutputPath
        PSModulePath = $baseModulePath
    }
    $positive = Invoke-V43Child $positiveScriptPath $cleanEnvironment
    if ($positive.exitCode -ne 0) { throw "Clean system-module case failed: $($positive.stderr) $($positive.stdout)" }
    $positiveResult = Read-V43ChildResult $positiveOutputPath
    if ($positiveResult.status -cne 'PASS' -or $positiveResult.modules.Count -ne 2) {
        throw 'Clean system-module case did not load the current Utility and Management modules.'
    }
    [void]$testCases.Add([pscustomobject]@{
        case = 'clean-system-module-path'
        status = 'PASS'
        powershellVersion = [string]$positiveResult.powershellVersion
        loadedModulePaths = @($positiveResult.modules | ForEach-Object { [string]$_.loadedPath })
    })

    $shadowRoot = Join-Path $tempRoot 'shadow-modules'
    [void][IO.Directory]::CreateDirectory($shadowRoot)
    $moduleMarker = Join-Path $tempRoot 'shadow-module-loaded.marker'
    $commandMarker = Join-Path $tempRoot 'shadow-command-executed.marker'
    [void](New-FakeModule $shadowRoot 'Microsoft.PowerShell.Utility' 'Get-FileHash' $moduleMarker $commandMarker)
    [void](New-FakeModule $shadowRoot 'Microsoft.PowerShell.Management' 'Get-Content' $moduleMarker $commandMarker)
    $pathShadowScriptPath = Join-Path $tempRoot 'path-shadow.ps1'
    $pathShadowOutputPath = Join-Path $tempRoot 'path-shadow.json'
    $pathShadowBody = $positiveBody
    New-V43ChildScript $pathShadowScriptPath $pathShadowBody
    $shadowEnvironment = @{
        V43_CHILD_OUTPUT = $pathShadowOutputPath
        V43_FAKE_MODULE_MARKER = $moduleMarker
        V43_FAKE_COMMAND_MARKER = $commandMarker
        PSModulePath = $shadowRoot + ';' + $baseModulePath
    }
    $pathShadow = Invoke-V43Child $pathShadowScriptPath $shadowEnvironment
    if ($pathShadow.exitCode -ne 0 -or [IO.File]::Exists($moduleMarker) -or [IO.File]::Exists($commandMarker)) {
        throw "Fake-first PSModulePath shadow was loaded or invoked: $($pathShadow.stderr) $($pathShadow.stdout)"
    }
    $pathShadowResult = Read-V43ChildResult $pathShadowOutputPath
    if ($pathShadowResult.status -cne 'PASS') { throw 'The fake-first PSModulePath case did not return a positive system-module receipt.' }
    [void]$testCases.Add([pscustomobject]@{ case = 'fake-first-PSModulePath'; status = 'PASS'; shadowLoaded = $false; shadowInvoked = $false })

    $functionMarker = Join-Path $tempRoot 'same-name-function-invoked.marker'
    $functionShadowPath = Join-Path $tempRoot 'function-shadow.ps1'
    $functionShadowBody = @"
`$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
function global:Get-FileHash { [IO.File]::WriteAllText(`$env:V43_FAKE_COMMAND_MARKER, 'executed'); return 'forged' }
. $helperLiteral
try { `$null = Assert-V43SystemModuleBoundary; exit 41 }
catch {
if (`$_.Exception.Message -notmatch 'same-name PowerShell alias/function') { [Console]::Error.WriteLine(`$_.Exception.ToString()); exit 42 }
    if ([IO.File]::Exists(`$env:V43_FAKE_COMMAND_MARKER)) { exit 43 }
    [Console]::WriteLine('FUNCTION_SHADOW_REJECTED_WITHOUT_INVOCATION')
    exit 0
}
"@
    New-V43ChildScript $functionShadowPath $functionShadowBody
    $functionShadow = Invoke-V43Child $functionShadowPath @{ V43_FAKE_COMMAND_MARKER = $functionMarker; PSModulePath = $baseModulePath }
    if ($functionShadow.exitCode -ne 0 -or $functionShadow.stdout -notmatch 'FUNCTION_SHADOW_REJECTED_WITHOUT_INVOCATION' -or [IO.File]::Exists($functionMarker)) {
        throw "Preloaded same-name function was not rejected before invocation: $($functionShadow.stderr) $($functionShadow.stdout)"
    }
    [void]$testCases.Add([pscustomobject]@{ case = 'preloaded-same-name-function'; status = 'PASS'; rejected = $true; functionInvoked = $false })

    $preloadedModuleMarker = Join-Path $tempRoot 'preloaded-module-loaded.marker'
    $preloadedCommandMarker = Join-Path $tempRoot 'preloaded-module-command.marker'
    $preloadedRoot = Join-Path $tempRoot 'preloaded-modules'
    [void][IO.Directory]::CreateDirectory($preloadedRoot)
    $preloadedUtility = New-FakeModule $preloadedRoot 'Microsoft.PowerShell.Utility' 'Get-FileHash' $preloadedModuleMarker $preloadedCommandMarker
    $preloadedModulePath = Join-Path $tempRoot 'preloaded-module.ps1'
    $preloadedModuleBody = @"
`$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module -Name '$($preloadedUtility.Replace("'", "''"))' -Global -ErrorAction Stop
. $helperLiteral
try { `$null = Assert-V43SystemModuleBoundary; exit 51 }
catch {
    if (`$_.Exception.Message -notmatch 'preloaded from a non-system path') { [Console]::Error.WriteLine(`$_.Exception.ToString()); exit 52 }
    if ([IO.File]::Exists(`$env:V43_FAKE_COMMAND_MARKER)) { exit 53 }
    [Console]::WriteLine('PRELOADED_MODULE_SHADOW_REJECTED_BEFORE_COMMAND')
    exit 0
}
"@
    New-V43ChildScript $preloadedModulePath $preloadedModuleBody
    $preloadedModule = Invoke-V43Child $preloadedModulePath @{
        V43_FAKE_MODULE_MARKER = $preloadedModuleMarker
        V43_FAKE_COMMAND_MARKER = $preloadedCommandMarker
        PSModulePath = $preloadedRoot + ';' + $baseModulePath
    }
    if ($preloadedModule.exitCode -ne 0 -or $preloadedModule.stdout -notmatch 'PRELOADED_MODULE_SHADOW_REJECTED_BEFORE_COMMAND' -or
        -not [IO.File]::Exists($preloadedModuleMarker) -or [IO.File]::Exists($preloadedCommandMarker)) {
        throw "Preloaded module shadow did not fail before calling its exported command: $($preloadedModule.stderr) $($preloadedModule.stdout)"
    }
    [void]$testCases.Add([pscustomobject]@{ case = 'preloaded-module'; status = 'PASS'; moduleLoadedForNegativeCase = $true; exportedCommandInvoked = $false; rejected = $true })

    $parsePaths = @(
        Get-ChildItem -LiteralPath $prepRoot -Recurse -File -Filter '*.ps1' |
            Sort-Object -Property FullName |
            ForEach-Object { [IO.Path]::GetFullPath($_.FullName) }
    )
    if ($parsePaths.Count -eq 0) { throw 'No v43 PowerShell sources were found for parser validation.' }
    foreach ($scriptPath in $parsePaths) {
        if (-not [IO.File]::Exists($scriptPath)) { throw "Required v43 PowerShell source is missing: $scriptPath" }
        $tokens = $null
        $parseErrors = $null
        [void][Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$tokens, [ref]$parseErrors)
        if (@($parseErrors).Count -gt 0) { throw "Windows PowerShell parser rejected $scriptPath : $($parseErrors[0].Message)" }
    }

    $testedResults['module_boundary_build'] = [pscustomobject]@{
        testId = 'module_boundary_build'; status = 'PASS'
        reason = 'Windows PowerShell 5.1 loaded the current system Utility and Management modules from the PSHOME module root and resolved the required commands from those modules.'
        powershellVersion = [string]$positiveResult.powershellVersion
        evidenceCases = @('clean-system-module-path', 'fake-first-PSModulePath')
    }
    $testedResults['module_boundary_source'] = [pscustomobject]@{
        testId = 'module_boundary_source'; status = 'PASS'
        reason = 'The new v43 helper passed parser, preloaded function, and preloaded module shadow tests; negative command bodies were not invoked.'
        evidenceCases = @('preloaded-same-name-function', 'preloaded-module')
    }
    $testedResults['parser'] = [pscustomobject]@{
        testId = 'parser'; status = 'PASS'
        reason = 'Every PowerShell script under the v43 prep directory parsed under Windows PowerShell 5.1.'
        parsedFiles = @($parsePaths | ForEach-Object {
            [pscustomobject][ordered]@{ path = $_; sha256 = Get-Sha256File $_ }
        })
    }
    $testedResults['module_boundary_build'] | Add-Member -NotePropertyName evidencePath -NotePropertyValue ([IO.Path]::GetFullPath($helperPath)) -Force
    $testedResults['module_boundary_build'] | Add-Member -NotePropertyName evidenceSha256 -NotePropertyValue (Get-Sha256File $helperPath) -Force
    $testedResults['module_boundary_source'] | Add-Member -NotePropertyName evidencePath -NotePropertyValue ([IO.Path]::GetFullPath($helperPath)) -Force
    $testedResults['module_boundary_source'] | Add-Member -NotePropertyName evidenceSha256 -NotePropertyValue (Get-Sha256File $helperPath) -Force
    $testedResults['parser'] | Add-Member -NotePropertyName evidencePath -NotePropertyValue ([IO.Path]::GetFullPath($MyInvocation.MyCommand.Path)) -Force
    $testedResults['parser'] | Add-Member -NotePropertyName evidenceSha256 -NotePropertyValue (Get-Sha256File $MyInvocation.MyCommand.Path) -Force
}
catch {
    $failure = $_.Exception.Message
}
finally {
    if ([IO.Directory]::Exists($tempRoot)) { Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue }
}

$pendingReasons = @{
    watchdog_preflight = 'No v43 watchdog preflight or v43 watchdog source is present; requires a candidate-bound no-product-action preflight before static implementation.'
    save_predicate_preflight = 'No v43 Save-predicate preflight is present; requires a source-bound preflight contract and negative fixture suite.'
    path_shadow = 'The PSModulePath shadow cases above are covered, but the v43 executable PATH-shadow contract has no v43-specific preflight or fixture yet.'
    root_seal = 'The external root GO seal and independent review are outside this static chain and are not supplied by this harness.'
    save_predicate = 'No candidate-bound, no-product-action Save predicate fixture has proved clean/adversarial decisions. Actual product Save is a separate post-GO acceptance action.'
    watchdog_contract = 'No v43 watchdog implementation and isolated supervisor-result fixtures are present.'
    sampler_arguments = 'No v43 sampler argument builder/parser contract is present.'
    supervisor = 'No v43 supervisor/worker implementation is bound to this candidate; product launch was not attempted.'
    worker_handshake = 'No v43 worker handshake implementation or isolated token-bound worker fixtures are present.'
    native_launch_boundary = 'No v43 native launch-boundary implementation/receipt is present; this harness starts only isolated PowerShell test children.'
    finalize_integrity = 'No candidate-bound v43 finalizer integrity clone/tamper suite has run against a complete static receipt.'
    root_bootstrap = 'No external v43 root bootstrap, independently pinned root seal, or root bootstrap receipt is supplied.'
}

$results = New-Object 'System.Collections.Generic.List[object]'
foreach ($testId in $requiredIds) {
    if ($testedResults.ContainsKey($testId)) {
        [void]$results.Add($testedResults[$testId])
    }
    elseif ($null -ne $failure -and $testId -in @('module_boundary_build', 'module_boundary_source', 'parser')) {
        [void]$results.Add([pscustomobject][ordered]@{ testId = $testId; status = 'FAIL'; reason = $failure })
    }
    else {
        [void]$results.Add([pscustomobject][ordered]@{ testId = $testId; status = 'PENDING'; reason = [string]$pendingReasons[$testId] })
    }
}
$passedCount = @($results | Where-Object { $_.status -ceq 'PASS' }).Count
$pendingIds = @($results | Where-Object { $_.status -ceq 'PENDING' } | ForEach-Object { [string]$_.testId })
$failCount = @($results | Where-Object { $_.status -ceq 'FAIL' }).Count
$status = if ($failCount -gt 0) { 'FAIL' } elseif ($pendingIds.Count -gt 0) { 'STATIC_INCOMPLETE' } else { 'PASS' }
$receipt = [ordered]@{
    schema = 'range.pc11-v43-static-gate-assessment/v1'
    status = $status
    candidateVersion = 'v43'
    variant = 'OFF'
    staticOnly = $true
    assessmentScope = '15_REMAINING_STATIC_GATES_ONLY; OUTER_STATIC_RECEIPT_MUST_BIND_THIS_FILE_TO_CANDIDATE_IDENTITY'
    candidateManifestIdentitySha256 = $null
    sourceTreeSha256 = $null
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    requiredGateIds = $requiredIds
    results = $results.ToArray()
    passedGateCount = $passedCount
    pendingGateIds = $pendingIds
    failure = $failure
    moduleBoundaryCases = $testCases.ToArray()
    productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
    goPermitted = $false
}
$parent = [IO.Path]::GetDirectoryName($OutputPath)
if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
[IO.File]::WriteAllText($OutputPath, ($receipt | ConvertTo-Json -Depth 20) + "`n", (New-Object Text.UTF8Encoding($false)))
Write-Output ("{0} passed={1} pending={2} failed={3} output={4}" -f $status, $passedCount, $pendingIds.Count, $failCount, $OutputPath)
if ($status -eq 'FAIL') { exit 1 }
