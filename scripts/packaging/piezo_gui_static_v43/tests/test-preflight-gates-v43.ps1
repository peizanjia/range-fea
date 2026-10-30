[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$AuditRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'This v43 static preflight requires Windows PowerShell 5.1.'
}

$script:PrepRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $AuditRoot) {
    $AuditRoot = [IO.Path]::GetFullPath((Join-Path $script:PrepRoot '..\..\..\build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit'))
}
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$auditPrefix = $AuditRoot.TrimEnd('\') + '\'
if (-not $OutputPath.StartsWith($auditPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Preflight receipt must remain under the external v43 output-audit directory.'
}
if ([IO.File]::Exists($OutputPath)) { throw "Refusing to overwrite existing preflight evidence: $OutputPath" }

function Get-Sha256File([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}

function Read-StrictUtf8([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { throw "Required v43 source is missing: $Path" }
    return (New-Object Text.UTF8Encoding($false, $true)).GetString([IO.File]::ReadAllBytes($Path))
}

function Get-QuotedIds([string]$Source, [string]$Pattern, [string]$Label) {
    $section = [regex]::Match($Source, $Pattern, [Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $section.Success) { throw "$Label does not expose the expected static gate ID list." }
    $ids = @([regex]::Matches($section.Groups['items'].Value, '(?<quote>[''"])(?<id>[a-z_]+)\k<quote>') |
        ForEach-Object { [string]$_.Groups['id'].Value })
    if ($ids.Count -eq 0) { throw "$Label static gate ID list is empty." }
    return ,$ids
}

function Test-IdParity([string[]]$AdmissionIds, [string[]]$RunnerIds) {
    return (($AdmissionIds -join "`n") -ceq ($RunnerIds -join "`n"))
}

function Get-FunctionSource([string]$Path, [string]$Name) {
    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$parseErrors)
    if (@($parseErrors).Count -gt 0) { throw "Windows PowerShell parser rejected $Path : $($parseErrors[0].Message)" }
    $matches = @($ast.FindAll({
        param($node)
        return ($node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $Name)
    }, $true))
    if ($matches.Count -ne 1) { throw "Expected exactly one $Name definition in $Path; found $($matches.Count)." }
    return [string]$matches[0].Extent.Text
}

function Test-ExplicitPrivatePythonBinding([string]$FunctionText) {
    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput($FunctionText, [ref]$tokens, [ref]$parseErrors)
    if (@($parseErrors).Count -gt 0) { return $false }
    $functions = @($ast.FindAll({
        param($node)
        return ($node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Invoke-InventoryReview')
    }, $true))
    if ($functions.Count -ne 1) { return $false }
    $assignments = @($functions[0].Body.FindAll({
        param($node)
        return ($node -is [Management.Automation.Language.AssignmentStatementAst] -and
            $node.Left.Extent.Text -match '^\s*\$processInfo\.FileName\s*$')
    }, $true))
    if ($assignments.Count -ne 1 -or $assignments[0].Right.Extent.Text -cne '$PythonPath') { return $false }
    $parameters = @($functions[0].Parameters | Where-Object { $_.Name.VariablePath.UserPath -ieq 'PythonPath' })
    return ($parameters.Count -eq 1)
}

function Assert-ArgumentCapture([string]$MarkerPath, [string[]]$ExpectedArguments, [string]$ExpectedExecutable) {
    if (-not [IO.File]::Exists($MarkerPath)) { throw "Fixture executable did not write its marker: $MarkerPath" }
    $lines = [IO.File]::ReadAllLines($MarkerPath, [Text.Encoding]::UTF8)
    if ($lines.Count -lt 1 -or [string]$lines[0] -ine $ExpectedExecutable) {
        throw "Unexpected fixture executable identity: $MarkerPath"
    }
    $captured = [string[]]@($lines | Select-Object -Skip 1)
    if ($captured.Count -ne $ExpectedArguments.Count) { throw 'Inventory helper changed the fixture argument count.' }
    for ($index = 0; $index -lt $ExpectedArguments.Count; $index++) {
        if ($captured[$index] -cne $ExpectedArguments[$index]) { throw "Inventory helper changed fixture argument $index." }
    }
}

$admissionPath = Join-Path $script:PrepRoot 'admission_v43.py'
$runnerPath = Join-Path $PSScriptRoot 'test-static-chain-v43.ps1'
$packageTreePath = Join-Path $script:PrepRoot 'package-tree-v43.ps1'
$testCases = New-Object 'System.Collections.Generic.List[object]'
$admissionText = Read-StrictUtf8 $admissionPath
$runnerText = Read-StrictUtf8 $runnerPath
$packageTreeText = Read-StrictUtf8 $packageTreePath

$admissionIds = Get-QuotedIds $admissionText 'REQUIRED_STATIC_TEST_IDS\s*=\s*\((?<items>.*?)\)' 'v43 admission'
$runnerIds = Get-QuotedIds $runnerText '\$script:RequiredTestIds\s*=\s*@\((?<items>.*?)\)' 'v43 static-chain runner'
$requestedIds = @('watchdog_preflight', 'save_predicate_preflight', 'path_shadow', 'sampler_arguments')
if (-not (Test-IdParity $admissionIds $runnerIds)) { throw 'Admission and static-chain runner gate IDs differ.' }
foreach ($id in $requestedIds) {
    if (@($admissionIds | Where-Object { $_ -ceq $id }).Count -ne 1) { throw "Expected exactly one required gate ID: $id" }
}

# Negative control: the contract checker must notice a missing gate in a mutated runner source.
$idMutation = New-Object Text.RegularExpressions.Regex("'path_shadow'\s*,?")
$mutatedRunnerText = $idMutation.Replace($runnerText, '', 1)
$mutatedRunnerIds = Get-QuotedIds $mutatedRunnerText '\$script:RequiredTestIds\s*=\s*@\((?<items>.*?)\)' 'mutated v43 static-chain runner'
if (Test-IdParity $admissionIds $mutatedRunnerIds) { throw 'ID-parity negative fixture failed to detect a removed path_shadow ID.' }
[void]$testCases.Add([pscustomobject][ordered]@{
    case = 'admission-runner-gate-id-parity-positive-and-negative'
    status = 'PASS'
    requiredGateIdsPresentOnce = $true
    identicalOrderedLists = $true
    removedPathShadowDetected = $true
    fixtureOnly = $true
})

$inventoryFunction = Get-FunctionSource $packageTreePath 'Invoke-InventoryReview'
$quoteFunction = Get-FunctionSource $packageTreePath 'Quote-WindowsArgument'
if (-not (Test-ExplicitPrivatePythonBinding $inventoryFunction)) {
    throw 'Current package-tree inventory invocation does not bind ProcessStartInfo.FileName to its explicit PythonPath parameter.'
}

$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('range-fea v43 preflight ' + [Guid]::NewGuid().ToString('N'))
$tempFull = [IO.Path]::GetFullPath($tempRoot)
if (-not $tempFull.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($tempFull) -notlike 'range-fea v43 preflight *') {
    throw 'Fixture root is outside the Windows temporary directory contract.'
}

$oldPath = $env:PATH
$oldMarker = $env:V43_PREFLIGHT_EXEC_MARKER
$runtimeInventory = @()
$failure = $null
$receipt = $null

try {
    [void][IO.Directory]::CreateDirectory($tempRoot)
    $fixtureRoot = Join-Path $tempRoot 'fixture inputs'
    $privateRoot = Join-Path $fixtureRoot 'private interpreter'
    $shadowRoot = Join-Path $fixtureRoot 'path shadow'
    $workingRoot = Join-Path $fixtureRoot 'inventory inputs'
    foreach ($directory in @($privateRoot, $shadowRoot, $workingRoot)) { [void][IO.Directory]::CreateDirectory($directory) }

    $adapterPath = Join-Path $workingRoot 'inventory adapter.py'
    $installRoot = Join-Path $workingRoot 'synthetic install'
    $reportPath = Join-Path $workingRoot 'inventory report.json'
    [IO.File]::WriteAllText($adapterPath, '# fixture adapter only' + [Environment]::NewLine, (New-Object Text.UTF8Encoding($false)))
    [void][IO.Directory]::CreateDirectory($installRoot)
    [IO.File]::WriteAllText($reportPath, '{}', (New-Object Text.UTF8Encoding($false)))

    $shimSource = @'
using System;
using System.IO;
using System.Text;
public static class RangeV43PreflightPythonShim {
    public static int Main(string[] args) {
        string marker = Environment.GetEnvironmentVariable("V43_PREFLIGHT_EXEC_MARKER");
        if (String.IsNullOrEmpty(marker)) return 71;
        string executable = System.Reflection.Assembly.GetExecutingAssembly().Location;
        StringBuilder output = new StringBuilder();
        output.AppendLine(executable);
        foreach (string arg in args) output.AppendLine(arg);
        File.WriteAllText(marker, output.ToString(), new UTF8Encoding(false));
        Console.Out.WriteLine("{\"fixture\":true,\"status\":\"INVENTORY_TREE_MATCH\",\"goPermitted\":false}");
        return 0;
    }
}
'@
    $privatePythonPath = Join-Path $privateRoot 'python.exe'
    $shadowPythonPath = Join-Path $shadowRoot 'python.exe'
    Add-Type -TypeDefinition $shimSource -Language CSharp -OutputAssembly $privatePythonPath -OutputType ConsoleApplication -ErrorAction Stop
    Copy-Item -LiteralPath $privatePythonPath -Destination $shadowPythonPath -ErrorAction Stop

    $env:PATH = $shadowRoot + ';' + $oldPath
    $shadowCommands = @(Get-Command python.exe -CommandType Application -ErrorAction Stop)
    $shadowCommand = $shadowCommands[0]
    if ([string]$shadowCommand.Source -ine $shadowPythonPath) {
        throw "The negative PATH fixture did not resolve the fake python.exe first; observed='$($shadowCommand.Source)' expected='$shadowPythonPath'."
    }

    Invoke-Expression $quoteFunction
    Invoke-Expression $inventoryFunction
    $expectedArguments = @('-B', $adapterPath, '--install-root', $installRoot, '--inventory-report', $reportPath,
        '--inventory-sha256', ('a' * 64), '--variant', 'OFF')

    $privateMarker = Join-Path $fixtureRoot 'private-python-executed.marker'
    $shadowMarker = Join-Path $fixtureRoot 'path-python-executed.marker'
    $env:V43_PREFLIGHT_EXEC_MARKER = $privateMarker
    $positive = Invoke-InventoryReview $privatePythonPath $adapterPath $installRoot $reportPath ('a' * 64)
    Assert-ArgumentCapture $privateMarker $expectedArguments $privatePythonPath
    if ([IO.File]::Exists($shadowMarker) -or $positive.status -cne 'INVENTORY_TREE_MATCH') {
        throw 'The source-bound package-tree helper did not use its explicit private Python path under fake-first PATH.'
    }
    [void]$testCases.Add([pscustomobject][ordered]@{
        case = 'package-tree-explicit-private-python-under-fake-first-PATH'
        status = 'PASS'
        privatePythonExecuted = $true
        shadowPythonExecuted = $false
        argumentVectorPreserved = $true
        fixtureOnly = $true
    })

    $mutationPattern = '(?m)^(\s*\$processInfo\.FileName\s*=\s*)\$PythonPath\s*$'
    $mutationCount = [regex]::Matches($inventoryFunction, $mutationPattern).Count
    $mutatedFunction = [regex]::Replace($inventoryFunction, $mutationPattern, [Text.RegularExpressions.MatchEvaluator]{
        param($match)
        return $match.Groups[1].Value + "'python.exe'"
    })
    if ($mutationCount -ne 1 -or (Test-ExplicitPrivatePythonBinding $mutatedFunction)) {
        throw 'The path-shadow negative source mutation was not rejected by the AST binding check.'
    }
    [void]$testCases.Add([pscustomobject][ordered]@{
        case = 'bare-python-source-mutation-rejected'
        status = 'PASS'
        mutationCount = $mutationCount
        rejectedBeforeMutationExecution = $true
        fixtureOnly = $true
    })

    # Execute the deliberately mutated helper only against the fake fixture executable.
    if ([IO.File]::Exists($privateMarker)) { [IO.File]::Delete($privateMarker) }
    if ([IO.File]::Exists($shadowMarker)) { [IO.File]::Delete($shadowMarker) }
    Invoke-Expression $mutatedFunction
    $env:V43_PREFLIGHT_EXEC_MARKER = $shadowMarker
    $mutatedResult = Invoke-InventoryReview $privatePythonPath $adapterPath $installRoot $reportPath ('a' * 64)
    Assert-ArgumentCapture $shadowMarker $expectedArguments $shadowPythonPath
    if ([IO.File]::Exists($privateMarker) -or $mutatedResult.status -cne 'INVENTORY_TREE_MATCH') {
        throw 'The negative control did not demonstrate that bare python.exe resolves to the fake PATH entry.'
    }
    [void]$testCases.Add([pscustomobject][ordered]@{
        case = 'bare-python-mutant-demonstrates-shadow-execution'
        status = 'PASS'
        shadowPythonExecuted = $true
        fixtureOnly = $true
    })
    Invoke-Expression $inventoryFunction

    # The inventory source surface is candidate packaging code only. These searches avoid tests and docs.
    $sourceFiles = @(Get-ChildItem -LiteralPath $script:PrepRoot -Recurse -File -ErrorAction Stop |
        Where-Object {
            $_.FullName -notmatch '[\\/]tests[\\/]' -and
            [IO.Path]::GetExtension($_.Name).ToLowerInvariant() -in @('.ps1', '.py', '.cs')
        } | Sort-Object FullName)
    $runtimeInventory = @($sourceFiles | ForEach-Object {
        $name = [string]$_.Name
        $role = if ($name -match '(?i)watchdog|supervisor|worker|launch') { 'watchdog-or-launcher' }
            elseif ($name -match '(?i)save.*predicate|predicate.*save|save.*io') { 'save-predicate' }
            elseif ($name -match '(?i)sampler|working[-_]?set|sampler.*argument') { 'sampler-arguments' }
            else { $null }
        if ($null -ne $role) {
            [pscustomobject][ordered]@{ path = $_.FullName; role = $role }
        }
    })
    $watchdogSources = @($runtimeInventory | Where-Object role -eq 'watchdog-or-launcher')
    $saveSources = @($runtimeInventory | Where-Object role -eq 'save-predicate')
    $samplerSources = @($runtimeInventory | Where-Object role -eq 'sampler-arguments')

    $sourceEvidence = @(
        [pscustomobject][ordered]@{ role = 'admission-contract'; path = $admissionPath; sha256 = Get-Sha256File $admissionPath },
        [pscustomobject][ordered]@{ role = 'outer-runner-contract'; path = $runnerPath; sha256 = Get-Sha256File $runnerPath },
        [pscustomobject][ordered]@{ role = 'package-tree-interpreter-call'; path = $packageTreePath; sha256 = Get-Sha256File $packageTreePath }
    )

    $gateResults = @(
        [pscustomobject][ordered]@{
            testId = 'watchdog_preflight'; status = 'PENDING'
            reason = if ($watchdogSources.Count -gt 0) { 'V43 watchdog/supervisor source paths are present, but this receipt did not test their current bytes; the source owner has not declared a stable implementation snapshot.' } else { 'No candidate-bound v43 watchdog/supervisor implementation is present in the prep source scope.' }
            prerequisite = 'After the v43 runtime source is stable and candidate-pinned, run source-driven successful and failed supervisor-result fixtures in an isolated package clone.'
        },
        [pscustomobject][ordered]@{
            testId = 'save_predicate_preflight'; status = 'PENDING'
            reason = if ($saveSources.Count -gt 0) { 'Save-predicate-related source paths are present, but this receipt did not test their current bytes or environment behavior.' } else { 'No candidate-bound v43 Save-predicate/Save-I/O implementation or environment preflight fixture is present in the prep source scope.' }
            prerequisite = 'After the v43 Save predicate and I/O helper are stable and candidate-pinned, test clean and adversarial module/path environments without product Save.'
        },
        [pscustomobject][ordered]@{
            testId = 'path_shadow'; status = 'PENDING'
            reason = 'The v43 package-tree private-Python callsite passed its fake-first PATH positive/negative component fixture; candidate runtime launch paths were not exercised by this component test.'
            prerequisite = 'After supervisor, worker, and GUI launch sources are stable and candidate-pinned, repeat fake-first PATH tests across every launch route before the aggregate gate can pass.'
            componentEvidence = 'package-tree-private-python-callsite'
        },
        [pscustomobject][ordered]@{
            testId = 'sampler_arguments'; status = 'PENDING'
            reason = if ($samplerSources.Count -gt 0) { 'Sampler-related v43 source paths are present, but this receipt did not test their current bytes or emitted arguments.' } else { 'No candidate-bound v43 supervisor/UIA sampler argument builders or matching sampler parser are present in the prep source scope.' }
            prerequisite = 'After each v43 sampler builder and parser is stable and candidate-pinned, capture both emitted vectors and prove positive plus malformed/missing-pin rejection cases.'
        }
    )

    $receipt = [ordered]@{
        schema = 'range.pc11-v43-static-preflight-assessment/v1'
        status = 'STATIC_INCOMPLETE'
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        candidateManifestIdentitySha256 = $null
        sourceTreeSha256 = $null
        evidenceScope = 'SOURCE_BOUND_PREFLIGHT_COMPONENTS_ONLY; RUNTIME GATES REMAIN PENDING UNTIL STABLE CANDIDATE SOURCES HAVE SOURCE-SPECIFIC FIXTURES'
        requiredGateIds = $requestedIds
        results = $gateResults
        passedGateCount = 0
        pendingGateIds = $requestedIds
        failedGateCount = 0
        contractGateCount = $admissionIds.Count
        requestedGateIdsPresentOnceInAdmissionAndRunner = $true
        componentCases = $testCases.ToArray()
        componentCaseCount = $testCases.Count
        runtimeSourceInventory = $runtimeInventory
        runtimeSourceInventoryStatus = 'DISCOVERY_ONLY_CURRENT_BYTES_NOT_TESTED'
        inspectedSources = $sourceEvidence
        outerHarnessBinding = [ordered]@{
            bindReceiptPathAndSha256 = $true
            bindCandidateManifestIdentityAfterCandidateFreeze = $true
            preserveAllFourPendingStatuses = $true
            promotePathShadowOnlyAfterSupervisorWorkerAndGuiCoverage = $true
        }
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
}
catch {
    $failure = $_.Exception.Message + ' ' + $_.ScriptStackTrace
}
finally {
    $env:PATH = $oldPath
    if ($null -eq $oldMarker) { Remove-Item Env:V43_PREFLIGHT_EXEC_MARKER -ErrorAction SilentlyContinue }
    else { $env:V43_PREFLIGHT_EXEC_MARKER = $oldMarker }
    if ([IO.Directory]::Exists($tempRoot)) {
        $resolvedTemp = [IO.Path]::GetFullPath($tempRoot)
        if ($resolvedTemp.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolvedTemp) -like 'range-fea v43 preflight *') {
            Remove-Item -LiteralPath $resolvedTemp -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

if ($null -eq $receipt) {
    throw "v43 static preflight failed: $failure"
}
$parent = [IO.Path]::GetDirectoryName($OutputPath)
if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
[IO.File]::WriteAllText($OutputPath, ($receipt | ConvertTo-Json -Depth 16) + "`n", (New-Object Text.UTF8Encoding($false)))
Write-Output ("{0} passed={1} pending={2} failed={3} componentCases={4} output={5}" -f $receipt.status, $receipt.passedGateCount, $receipt.pendingGateIds.Count, $receipt.failedGateCount, $receipt.componentCaseCount, $OutputPath)
