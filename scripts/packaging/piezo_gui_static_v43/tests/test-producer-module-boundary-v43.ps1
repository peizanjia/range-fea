[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'The v43 producer module-boundary test requires Windows PowerShell 5.1.'
}

$script:PrepRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$script:RepoRoot = [IO.Path]::GetFullPath((Join-Path $script:PrepRoot '..\..\..'))
$script:PowerShellPath = [IO.Path]::GetFullPath((Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'))
$script:ProducerCases = @(
    [pscustomobject]@{ name = 'source-preflight'; path = (Join-Path $script:PrepRoot 'source-preflight-v43.ps1'); failure = 'Required CTest manifest lacks required_test_groups.'; schema = 'range.pc11-v43-source-preflight/v1'; status = 'SOURCE_PREFLIGHT_FAIL' },
    [pscustomobject]@{ name = 'freeze'; path = (Join-Path $script:PrepRoot 'freeze-v43.ps1'); failure = 'Pre-build source-preflight receipt.sourceRootPath is required.'; schema = 'range.pc11-v43-source-freeze/v1'; status = 'SOURCE_FREEZE_FAIL' },
    [pscustomobject]@{ name = 'package-tree'; path = (Join-Path $script:PrepRoot 'package-tree-v43.ps1'); failure = 'Candidate manifest schema/version is invalid.'; schema = 'range.pc11-v43-package-tree-receipt/v1'; status = 'FAIL' },
    [pscustomobject]@{ name = 'finalize'; path = (Join-Path $script:PrepRoot 'finalize-v43.ps1'); failure = "The property 'candidateEvidencePins' cannot be found on this object. Verify that the property exists."; schema = 'range.pc11-v43-final-static-receipt/v1'; status = 'FINALIZE_FAIL' }
)

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
    $arguments = @('-NoProfile', '-NoLogo', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $ScriptPath)
    $quoted = New-Object 'System.Collections.Generic.List[string]'
    foreach ($argument in $arguments) { [void]$quoted.Add((Quote-WindowsArgument ([string]$argument))) }
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $script:PowerShellPath
    $startInfo.Arguments = [string]::Join(' ', $quoted.ToArray())
    $startInfo.WorkingDirectory = $script:RepoRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($name in $Environment.Keys) { $startInfo.EnvironmentVariables[[string]$name] = [string]$Environment[$name] }
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) { throw 'Could not start an isolated Windows PowerShell 5.1 producer case.' }
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $stdout = $process.StandardOutput.ReadToEnd()
        $process.WaitForExit()
        return [pscustomobject]@{ exitCode = [int]$process.ExitCode; stdout = $stdout; stderr = [string]$stderrTask.Result }
    }
    finally { $process.Dispose() }
}

function New-V43ChildScript([string]$Path, [string]$Body) {
    [IO.File]::WriteAllText($Path, $Body, (New-Object Text.UTF8Encoding($false)))
}

function Get-Sha256File([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}

function New-FakeUtilityModule([string]$Root) {
    $moduleName = 'Microsoft.PowerShell.Utility'
    $moduleDirectory = Join-Path $Root $moduleName
    [void][IO.Directory]::CreateDirectory($moduleDirectory)
    $guid = [Guid]::NewGuid().ToString()
    $manifest = "@{ RootModule='$moduleName.psm1'; ModuleVersion='1.0.0'; GUID='$guid'; FunctionsToExport=@('Get-FileHash','ConvertFrom-Json','ConvertTo-Json') }`n"
    $module = @'
[IO.File]::WriteAllText($env:V43_FAKE_MODULE_MARKER, 'loaded')
function Get-FileHash { [IO.File]::WriteAllText($env:V43_FAKE_COMMAND_MARKER, 'Get-FileHash'); return 'forged' }
function ConvertFrom-Json { [IO.File]::WriteAllText($env:V43_FAKE_COMMAND_MARKER, 'ConvertFrom-Json'); return [pscustomobject]@{} }
function ConvertTo-Json { [IO.File]::WriteAllText($env:V43_FAKE_COMMAND_MARKER, 'ConvertTo-Json'); return '{}' }
Export-ModuleMember -Function @('Get-FileHash','ConvertFrom-Json','ConvertTo-Json')
'@
    [IO.File]::WriteAllText((Join-Path $moduleDirectory ($moduleName + '.psd1')), $manifest, (New-Object Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText((Join-Path $moduleDirectory ($moduleName + '.psm1')), $module, (New-Object Text.UTF8Encoding($false)))
}

function New-ProducerInvocationBody {
    return @'
$ErrorActionPreference = 'Stop'
if ($env:V43_PRELOAD_MODE -ceq 'get-file-hash') {
    function global:Get-FileHash { [IO.File]::WriteAllText($env:V43_FUNCTION_MARKER, 'invoked'); return 'forged' }
}
elseif ($env:V43_PRELOAD_MODE -ceq 'boundary') {
    function global:Assert-V43SystemModuleBoundary { [IO.File]::WriteAllText($env:V43_FUNCTION_MARKER, 'invoked'); return [pscustomobject]@{ status = 'PASS' } }
}
elseif ($env:V43_PRELOAD_MODE -ceq 'where-object') {
    function global:Where-Object { [IO.File]::WriteAllText($env:V43_FUNCTION_MARKER, 'invoked'); return $_ }
}
elseif ($env:V43_PRELOAD_MODE -ceq 'for-each-object') {
    function global:ForEach-Object { [IO.File]::WriteAllText($env:V43_FUNCTION_MARKER, 'invoked'); return $_ }
}
elseif ($env:V43_PRELOAD_MODE -ceq 'select-object') {
    function global:Select-Object { [IO.File]::WriteAllText($env:V43_FUNCTION_MARKER, 'invoked'); return $_ }
}
$producerParameters = $null
switch ($env:V43_PRODUCER_KIND) {
    'source-preflight' {
        $producerParameters = @{ RepoRoot = $env:V43_REPO_ROOT; RequiredCTestManifestPath = $env:V43_INPUT_PATH; AuditRoot = $env:V43_AUDIT_ROOT; RunId = 'module-boundary-fixture'; OutputPath = $env:V43_OUTPUT_PATH }
    }
    'freeze' {
        $producerParameters = @{ PreBuildSourcePreflightPath = $env:V43_INPUT_PATH; PreBuildSourcePreflightSha256 = $env:V43_INPUT_SHA; ReleaseBuildReceiptPath = $env:V43_INPUT_PATH; ReleaseBuildReceiptSha256 = $env:V43_INPUT_SHA; CMakeCachePath = $env:V43_INPUT_PATH; RepoRoot = $env:V43_REPO_ROOT; AuditRoot = $env:V43_AUDIT_ROOT; RunId = 'module-boundary-fixture'; OutputPath = $env:V43_OUTPUT_PATH }
    }
    'package-tree' {
        $producerParameters = @{ CandidateManifestPath = $env:V43_INPUT_PATH; CandidateManifestSha256 = $env:V43_INPUT_SHA; AuditRoot = $env:V43_AUDIT_ROOT; RunId = 'module-boundary-fixture'; OutputPath = $env:V43_OUTPUT_PATH }
    }
    'finalize' {
        $producerParameters = @{ CandidateManifestPath = $env:V43_INPUT_PATH; CandidateManifestSha256 = $env:V43_INPUT_SHA; AuditRoot = $env:V43_AUDIT_ROOT; RunId = 'module-boundary-fixture'; OutputPath = $env:V43_OUTPUT_PATH }
    }
    default { [Console]::Error.WriteLine('Unknown producer case.'); exit 61 }
}
try {
    & $env:V43_PRODUCER_PATH @producerParameters
    if ($env:V43_ALLOW_PRODUCER_FAILURE -ceq 'true') {
        [Console]::WriteLine('PRODUCER_RETURNED_WITH_EXPECTED_NEGATIVE_RECEIPT')
        exit 0
    }
    [Console]::Error.WriteLine('Producer returned without the expected module-boundary failure.')
    exit 62
}
catch {
    if ($env:V43_ALLOW_PRODUCER_FAILURE -ceq 'true') {
        [Console]::WriteLine('PRODUCER_RETURNED_WITH_EXPECTED_NEGATIVE_RECEIPT')
        exit 0
    }
    if ($_.Exception.Message -cne $env:V43_EXPECTED_FAILURE) { [Console]::Error.WriteLine($_.Exception.ToString()); exit 63 }
    [Console]::WriteLine('EXPECTED_MODULE_BOUNDARY_FAILURE')
    exit 0
}
'@
}

function Assert-NoGoReceipt([string]$Path, $Case) {
    if (-not [IO.File]::Exists($Path)) { throw "Fake-first module case did not preserve the normal negative receipt: $($Case.name)." }
    $text = (New-Object Text.UTF8Encoding($false, $true)).GetString([IO.File]::ReadAllBytes($Path))
    $receipt = $text | ConvertFrom-Json -ErrorAction Stop
    if ($receipt.schema -cne $Case.schema -or $receipt.status -cne $Case.status -or
        [string]$receipt.failure -cne $Case.failure -or $receipt.goPermitted -ne $false) {
        throw ("Fake-first module case returned an unexpected negative receipt: {0}; schema={1}; status={2}; failure={3}; goPermitted={4}." -f
            $Case.name, [string]$receipt.schema, [string]$receipt.status, [string]$receipt.failure, [string]$receipt.goPermitted)
    }
    foreach ($actionName in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        if ($receipt.productActions.$actionName -ne $false) { throw "Negative receipt permits product action ${actionName}: $($Case.name)." }
    }
    return $receipt
}

$failure = $null
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('range-fea-v43-producer-module-boundary-' + [Guid]::NewGuid().ToString('N'))
$testedCases = New-Object 'System.Collections.Generic.List[string]'
try {
    [void][IO.Directory]::CreateDirectory($tempRoot)
    $invalidInputPath = Join-Path $tempRoot 'invalid-input.json'
    $invalidInput = '{"required_test_groups":null,"schema":"invalid","status":"FAIL","candidateVersion":"v43","goPermitted":false,"productActions":{"guiLaunched":false,"savePerformed":false,"runPerformed":false,"confirmPerformed":false}}'
    [IO.File]::WriteAllText($invalidInputPath, $invalidInput + "`n", (New-Object Text.UTF8Encoding($false)))
    $invalidInputSha = Get-Sha256File $invalidInputPath
    $childPath = Join-Path $tempRoot 'invoke-producer.ps1'
    New-V43ChildScript $childPath (New-ProducerInvocationBody)

    foreach ($case in $script:ProducerCases) {
        foreach ($preloadMode in @('get-file-hash', 'boundary', 'where-object', 'for-each-object', 'select-object')) {
            $caseRoot = Join-Path $tempRoot ($case.name + '-' + $preloadMode)
            $outputPath = Join-Path $caseRoot 'receipt.json'
            $markerPath = Join-Path $caseRoot 'function-invoked.marker'
            switch ($preloadMode) {
                'boundary' { $expectedFailure = 'A preloaded Assert-V43SystemModuleBoundary command cannot bypass the trusted v43 helper.' }
                { $_ -in @('get-file-hash', 'where-object', 'for-each-object', 'select-object') } {
                    $commandName = switch ($preloadMode) {
                        'get-file-hash' { 'Get-FileHash' }
                        'where-object' { 'Where-Object' }
                        'for-each-object' { 'ForEach-Object' }
                        'select-object' { 'Select-Object' }
                    }
                    $expectedFailure = "A same-name PowerShell alias/function was preloaded before the system module boundary: $commandName"
                }
            }
            $result = Invoke-V43Child $childPath @{
                PSModulePath = (Join-Path $PSHOME 'Modules')
                V43_PRODUCER_KIND = $case.name
                V43_PRODUCER_PATH = $case.path
                V43_REPO_ROOT = $tempRoot
                V43_INPUT_PATH = $invalidInputPath
                V43_INPUT_SHA = $invalidInputSha
                V43_AUDIT_ROOT = $caseRoot
                V43_OUTPUT_PATH = $outputPath
                V43_EXPECTED_FAILURE = $expectedFailure
                V43_PRELOAD_MODE = $preloadMode
                V43_FUNCTION_MARKER = $markerPath
                V43_FAKE_MODULE_MARKER = (Join-Path $caseRoot 'fake-module-loaded.marker')
                V43_FAKE_COMMAND_MARKER = (Join-Path $caseRoot 'fake-command-invoked.marker')
            }
            if ($result.exitCode -ne 0 -or $result.stdout -notmatch 'EXPECTED_MODULE_BOUNDARY_FAILURE') {
                throw "Actual producer did not reject $preloadMode before work ($($case.name)): $($result.stderr.Trim()) $($result.stdout.Trim())"
            }
            if ([IO.File]::Exists($markerPath) -or [IO.File]::Exists($outputPath)) {
                throw "Actual producer reached a preloaded function or wrote a receipt before rejection ($($case.name), $preloadMode)."
            }
            [void]$testedCases.Add(($case.name + ':' + $preloadMode))
        }
    }

    $fakeModuleRoot = Join-Path $tempRoot 'fake-modules'
    [void][IO.Directory]::CreateDirectory($fakeModuleRoot)
    New-FakeUtilityModule $fakeModuleRoot
    foreach ($case in $script:ProducerCases) {
        $caseRoot = Join-Path $tempRoot ($case.name + '-fake-first')
        [void][IO.Directory]::CreateDirectory($caseRoot)
        $outputPath = Join-Path $caseRoot 'receipt.json'
        $moduleMarker = Join-Path $caseRoot 'fake-module-loaded.marker'
        $commandMarker = Join-Path $caseRoot 'fake-command-invoked.marker'
        $result = Invoke-V43Child $childPath @{
            PSModulePath = $fakeModuleRoot + ';' + (Join-Path $PSHOME 'Modules')
            V43_PRODUCER_KIND = $case.name
            V43_PRODUCER_PATH = $case.path
            V43_REPO_ROOT = $tempRoot
            V43_INPUT_PATH = $invalidInputPath
            V43_INPUT_SHA = $invalidInputSha
            V43_AUDIT_ROOT = $caseRoot
            V43_OUTPUT_PATH = $outputPath
            V43_EXPECTED_FAILURE = $case.failure
            V43_ALLOW_PRODUCER_FAILURE = 'true'
            V43_PRELOAD_MODE = 'none'
            V43_FUNCTION_MARKER = (Join-Path $caseRoot 'function-invoked.marker')
            V43_FAKE_MODULE_MARKER = $moduleMarker
            V43_FAKE_COMMAND_MARKER = $commandMarker
        }
        if ($result.exitCode -ne 0 -or $result.stdout -notmatch 'PRODUCER_RETURNED_WITH_EXPECTED_NEGATIVE_RECEIPT') {
            throw "Fake-first PSModulePath did not reach the normal producer failure path ($($case.name)): $($result.stderr.Trim()) $($result.stdout.Trim())"
        }
        [void](Assert-NoGoReceipt $outputPath $case)
        if ([IO.File]::Exists($moduleMarker) -or [IO.File]::Exists($commandMarker)) {
            throw "Fake-first PSModulePath loaded or invoked the shadow module ($($case.name))."
        }
        [void]$testedCases.Add(($case.name + ':fake-first-PSModulePath'))
    }

    Write-Output ("PASS producer-module-boundary cases={0}" -f $testedCases.Count)
}
catch {
    $failure = $_.Exception.Message
}
finally {
    if ([IO.Directory]::Exists($tempRoot)) { Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue }
}

if ($null -ne $failure) { throw $failure }
