[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$AuditRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'The v43 path-shadow fixture requires Windows PowerShell 5.1.'
}
$packageRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $AuditRoot) { $AuditRoot = [IO.Path]::GetFullPath((Join-Path $packageRoot '..\..\..\build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit')) }
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if (-not $OutputPath.StartsWith($AuditRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Receipt must be written under the external audit root.' }
if ([IO.File]::Exists($OutputPath)) { throw "Refusing to overwrite single-use path-shadow receipt: $OutputPath" }

function Assert-NoReparseAncestors([string]$Path) {
    $cursor = [IO.DirectoryInfo]([IO.Path]::GetFullPath($Path))
    while ($null -ne $cursor) {
        if ($cursor.Exists -and (($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw "Refusing reparse-point path ancestor: $($cursor.FullName)"
        }
        $cursor = $cursor.Parent
    }
}
Assert-NoReparseAncestors $AuditRoot
Assert-NoReparseAncestors ([IO.Path]::GetDirectoryName($OutputPath))

function Get-Hash([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}
function Get-FunctionText([string]$Path, [string]$Name) {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    if (@($errors).Count -gt 0) { throw "Source parse failed: $Path" }
    $found = @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $Name }, $true))
    if ($found.Count -ne 1) { throw "Expected one $Name function in $Path." }
    return [string]$found[0].Extent.Text
}
function Get-ParsedAst([string]$Path) {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    if (@($errors).Count -gt 0) { throw "Source parse failed: $Path" }
    return $ast
}
function Get-VariableAssignments($Ast, [string]$Name) {
    return ,@($Ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left -is [Management.Automation.Language.VariableExpressionAst] -and
        $node.Left.VariablePath.UserPath -ieq $Name
    }, $true))
}
function Test-SourceBindings([string]$PackagePath, [string]$SupervisorPath, [string]$NativeText, [string]$ModulePath) {
    # The actual package-tree interpreter assignment and the ProcessStartInfo target are AST-bound.
    $packageAst = Get-ParsedAst $PackagePath
    $privatePython = Get-VariableAssignments $packageAst 'pythonPath'
    if ($privatePython.Count -ne 1 -or $privatePython[0].Right.Extent.Text -notmatch '^\[IO\.Path\]::GetFullPath\(\(Join-Path\s+\$installRoot\s+''runtime\\python\\python\.exe''\)\)$') { throw 'Package-tree interpreter path AST did not match.' }
    $inventoryAst = $packageAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Invoke-InventoryReview' }, $true)
    if (@($inventoryAst).Count -ne 1) { throw 'Package-tree invocation function AST did not match.' }
    $fileNameAssignments = @($inventoryAst[0].FindAll({ param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left.Extent.Text -ceq '$processInfo.FileName'
    }, $true))
    if ($fileNameAssignments.Count -ne 1 -or $fileNameAssignments[0].Right.Extent.Text -cne '$PythonPath') { throw 'Package-tree ProcessStartInfo AST did not match.' }

    # Supervisor executable is derived from PSHOME and the call forwards that exact variable as application.
    $supervisorAst = Get-ParsedAst $SupervisorPath
    $powerShellAssignments = Get-VariableAssignments $supervisorAst 'powerShell'
    if ($powerShellAssignments.Count -ne 1 -or $powerShellAssignments[0].Right.Extent.Text -notmatch '^\[IO\.Path\]::GetFullPath\(\(Join-Path\s+\$PSHOME\s+''powershell\.exe''\)\)$') { throw 'Supervisor executable AST did not match.' }
    $launchAssignments = @($supervisorAst.FindAll({ param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left.Extent.Text -ceq '$processInfo' -and
        $node.Right -is [Management.Automation.Language.CommandExpressionAst] -and
        $node.Right.Expression -is [Management.Automation.Language.InvokeMemberExpressionAst] -and
        $node.Right.Expression.Member.Extent.Text -ceq 'StartSuspended'
    }, $true))
    if ($launchAssignments.Count -ne 1 -or $launchAssignments[0].Right.Expression.Arguments.Count -lt 1 -or
        $launchAssignments[0].Right.Expression.Arguments[0].Extent.Text -cne '$powerShell') { throw ("Supervisor StartSuspended AST forwarding did not match: count={0}; right={1}" -f $launchAssignments.Count, $(if($launchAssignments.Count){$launchAssignments[0].Right.Extent.Text}else{'<none>'})) }

    # C# inside Add-Type is checked as a source-level forwarding contract with a negative mutation.
    $dllImport = '\[DllImport\("kernel32\.dll",\s*EntryPoint\s*=\s*"CreateProcessW"[^\]]*\)\]\s*private\s+static\s+extern\s+bool\s+CreateProcess\(string\s+application,'
    $forward = 'bool\s+created\s*=\s*CreateProcess\(application,\s*new\s+StringBuilder\(commandLine\),'
    if ($NativeText -notmatch $dllImport -or $NativeText -notmatch $forward) { throw 'Native CreateProcess forwarding pattern did not match.' }
    $mutatedNative = [regex]::Replace($NativeText, $forward, 'bool created = CreateProcess(null, new StringBuilder(commandLine),', 1)
    if ($mutatedNative -ceq $NativeText -or ($mutatedNative -match $dllImport -and $mutatedNative -match $forward)) { throw 'Native negative mutation did not detect null application.' }

    # Module import AST must carry the computed absolute manifest path as Import-Module's -Name value.
    $moduleAst = Get-ParsedAst $ModulePath
    $imports = @($moduleAst.FindAll({ param($node)
        $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -match '(^|\\)Import-Module$'
    }, $true))
    $pinnedImportFound = $false
    foreach ($import in $imports) {
        $elements = @($import.CommandElements | ForEach-Object { $_.Extent.Text })
        for ($i = 0; $i -lt ($elements.Count - 1); $i++) {
            if ($elements[$i] -ceq '-Name' -and $elements[$i + 1] -ceq '$manifestPath') { $pinnedImportFound = $true }
        }
    }
    if (-not $pinnedImportFound) { throw 'Module Import-Module AST did not forward manifestPath.' }
    return $true
}
function Quote-Arg([string]$Value) {
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    return '"' + $Value.Replace('\\', '\\\\').Replace('"', '\\"') + '"'
}
function Invoke-Child([string]$ScriptPath, [hashtable]$Environment) {
    $exe = [IO.Path]::GetFullPath((Join-Path $PSHOME 'powershell.exe'))
    $args = @('-NoProfile','-NoLogo','-NonInteractive','-ExecutionPolicy','Bypass','-File',$ScriptPath)
    $line = [string]::Join(' ', @($args | ForEach-Object { Quote-Arg ([string]$_) }))
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $exe; $info.Arguments = $line; $info.WorkingDirectory = $packageRoot
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true; $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    foreach ($key in $Environment.Keys) { $info.EnvironmentVariables[[string]$key] = [string]$Environment[$key] }
    $proc = New-Object Diagnostics.Process; $proc.StartInfo = $info
    try {
        if (-not $proc.Start()) { throw 'Could not start isolated PowerShell fixture child.' }
        $err = $proc.StandardError.ReadToEndAsync(); $out = $proc.StandardOutput.ReadToEnd(); $proc.WaitForExit()
        return [pscustomobject]@{ exitCode = [int]$proc.ExitCode; stdout = $out; stderr = [string]$err.Result }
    } finally { $proc.Dispose() }
}
function Write-Utf8([string]$Path, [string]$Text) { [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false))) }

$sources = [ordered]@{
    packageTree = Join-Path $packageRoot 'package-tree-v43.ps1'
    moduleBoundary = Join-Path $packageRoot 'tools\system-module-boundary-v43.ps1'
    contractCommon = Join-Path $packageRoot 'runtime\contract-common-v43.ps1'
    supervisor = Join-Path $packageRoot 'runtime\supervisor-core-v43.ps1'
    nativeLaunch = Join-Path $packageRoot 'runtime\native-launch-boundary-v43.ps1'
    handshake = Join-Path $packageRoot 'runtime\worker-handshake-v43.ps1'
    worker = Join-Path $packageRoot 'tests\fixtures\worker-fixture-v43.ps1'
    test = $PSCommandPath
}
foreach ($path in $sources.Values) { if (-not [IO.File]::Exists($path)) { throw "Required v43 route source is missing: $path" } }
$sourceHashes = @($sources.GetEnumerator() | ForEach-Object {
    $hash = Get-Hash $_.Value
    if ($hash -notmatch '^[0-9a-f]{64}$') { throw "Source SHA256 is malformed for route $($_.Key)." }
    [pscustomobject][ordered]@{ route = $_.Key; path = [IO.Path]::GetFullPath($_.Value); sha256 = $hash }
})
$rows = New-Object 'System.Collections.Generic.List[object]'
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('range-fea-v43-path-shadow-' + [Guid]::NewGuid().ToString('N'))
$oldPath = $env:PATH
$oldModulePath = $env:PSModulePath
$failure = $null
try {
    [void][IO.Directory]::CreateDirectory($tempRoot)

    # Dynamic PATH test runs the actual package-tree function with the explicit private executable path.
    $functionText = Get-FunctionText $sources.packageTree 'Invoke-InventoryReview'
    if ($functionText -notmatch '(?m)^\s*\$processInfo\.FileName\s*=\s*\$PythonPath\s*$') { throw 'Package-tree Python callsite is no longer bound to its explicit PythonPath.' }
    $quoteText = Get-FunctionText $sources.packageTree 'Quote-WindowsArgument'
    $privateDir = Join-Path $tempRoot 'private'; $shadowDir = Join-Path $tempRoot 'shadow'; $inputs = Join-Path $tempRoot 'inputs'
    foreach ($dir in @($privateDir,$shadowDir,$inputs)) { [void][IO.Directory]::CreateDirectory($dir) }
    $adapter = Join-Path $inputs 'adapter.py'; $install = Join-Path $inputs 'install'; $report = Join-Path $inputs 'inventory.json'
    [IO.Directory]::CreateDirectory($install) | Out-Null; Write-Utf8 $adapter '# inert adapter'; Write-Utf8 $report '{}'
    $shim = @'
using System; using System.IO; using System.Text;
public static class V43PathShadowPythonShim {
 public static int Main(string[] args) {
  string marker=Environment.GetEnvironmentVariable("V43_PATH_SHADOW_MARKER");
  if(String.IsNullOrEmpty(marker)) return 81;
  string exe=System.Reflection.Assembly.GetExecutingAssembly().Location;
  var b=new StringBuilder(); b.AppendLine(exe); foreach(string a in args)b.AppendLine(a);
  File.WriteAllText(marker,b.ToString(),new UTF8Encoding(false));
  Console.WriteLine("{\"status\":\"INVENTORY_TREE_MATCH\",\"goPermitted\":false}"); return 0;
 }
}
'@
    $privateExe = Join-Path $privateDir 'python.exe'; $shadowExe = Join-Path $shadowDir 'python.exe'
    Add-Type -TypeDefinition $shim -Language CSharp -OutputAssembly $privateExe -OutputType ConsoleApplication -ErrorAction Stop
    Copy-Item -LiteralPath $privateExe -Destination $shadowExe
    $env:PATH = $shadowDir + ';' + $oldPath
    $resolvedShadow = [string](Get-Command python.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    if ($resolvedShadow -ine $shadowExe) { throw "Negative fixture failed to put shadow python.exe first on PATH: '$resolvedShadow' != '$shadowExe'." }
    Invoke-Expression $quoteText; Invoke-Expression $functionText
    $privateMarker = Join-Path $tempRoot 'private-ran.marker'; $shadowMarker = Join-Path $tempRoot 'shadow-ran.marker'
    $env:V43_PATH_SHADOW_MARKER = $privateMarker
    $review = Invoke-InventoryReview $privateExe $adapter $install $report ('a' * 64)
    if (-not [IO.File]::Exists($privateMarker) -or [IO.File]::Exists($shadowMarker) -or $review.status -cne 'INVENTORY_TREE_MATCH') { throw 'Package-tree explicit-path positive case failed under fake-first PATH.' }
    $captured = [IO.File]::ReadAllLines($privateMarker)
    if ($captured[0] -ine $privateExe -or $captured.Count -ne 11 -or $captured[1] -cne '-B' -or $captured[2] -cne $adapter -or
        $captured[3] -cne '--install-root' -or $captured[4] -cne $install -or $captured[5] -cne '--inventory-report' -or
        $captured[6] -cne $report -or $captured[7] -cne '--inventory-sha256' -or $captured[8] -cne ('a' * 64) -or
        $captured[9] -cne '--variant' -or $captured[10] -cne 'OFF') { throw 'Package-tree private executable or arguments were not preserved.' }
    [void]$rows.Add([pscustomobject][ordered]@{ route='package_tree.private_python'; status='PASS'; fakeFirstPathConfirmed=$true; pinnedExecutableRan=$true; shadowExecutableRan=$false; fixtureOnly=$true })

    # Independent child isolates PSModulePath; only the package-local boundary helper is executed.
    $helperLiteral = "'" + $sources.moduleBoundary.Replace("'", "''") + "'"
    $moduleChild = Join-Path $tempRoot 'module-child.ps1'; $moduleOut = Join-Path $tempRoot 'module-result.json'
    $body = @"
`$ErrorActionPreference='Stop'; Set-StrictMode -Version Latest
. $helperLiteral
`$r=Assert-V43SystemModuleBoundary
[IO.File]::WriteAllText(`$env:V43_PATH_SHADOW_MODULE_RESULT, (`$r | ConvertTo-Json -Depth 10 -Compress), (New-Object Text.UTF8Encoding(`$false)))
"@
    Write-Utf8 $moduleChild $body
    $fakeModules = Join-Path $tempRoot 'fake-modules'; [IO.Directory]::CreateDirectory($fakeModules) | Out-Null
    $loadedMarker = Join-Path $tempRoot 'fake-module-loaded.marker'; $calledMarker = Join-Path $tempRoot 'fake-module-called.marker'
    foreach ($name in @('Microsoft.PowerShell.Utility','Microsoft.PowerShell.Management')) {
        $dir = Join-Path $fakeModules $name; [IO.Directory]::CreateDirectory($dir) | Out-Null
        $manifest = "@{ RootModule='$name.psm1'; ModuleVersion='1.0.0'; GUID='$([Guid]::NewGuid())'; FunctionsToExport=@('Get-FileHash') }"
        Write-Utf8 (Join-Path $dir ($name + '.psd1')) $manifest
        Write-Utf8 (Join-Path $dir ($name + '.psm1')) "[IO.File]::WriteAllText(`$env:V43_PATH_SHADOW_MODULE_LOADED,'loaded'); function Get-FileHash { [IO.File]::WriteAllText(`$env:V43_PATH_SHADOW_MODULE_CALLED,'called') }; Export-ModuleMember -Function Get-FileHash"
    }
    $child = Invoke-Child $moduleChild @{ V43_PATH_SHADOW_MODULE_RESULT=$moduleOut; V43_PATH_SHADOW_MODULE_LOADED=$loadedMarker; V43_PATH_SHADOW_MODULE_CALLED=$calledMarker; PSModulePath=($fakeModules + ';' + (Join-Path $PSHOME 'Modules')) }
    if ($child.exitCode -ne 0 -or -not [IO.File]::Exists($moduleOut) -or [IO.File]::Exists($loadedMarker) -or [IO.File]::Exists($calledMarker)) { throw "Fake-first PSModulePath caused shadow load/call or helper failure: $($child.stderr) $($child.stdout)" }
    $moduleResult = (Get-Content -LiteralPath $moduleOut -Raw | ConvertFrom-Json)
    if ($moduleResult.status -cne 'PASS' -or @($moduleResult.modules).Count -ne 2) { throw 'System module boundary did not positively resolve both pinned modules.' }
    [void]$rows.Add([pscustomobject][ordered]@{ route='system_modules.boundary'; status='PASS'; fakeFirstPSModulePathConfirmed=$true; fakeModuleLoaded=$false; fakeCommandCalled=$false; fixtureOnly=$true })

    # Run CreateProcessW with a fake powershell.exe first on PATH. The native
    # boundary receives the real PSHOME executable as its application path.
    $shadowPowerShell = Join-Path $shadowDir 'powershell.exe'
    Copy-Item -LiteralPath $privateExe -Destination $shadowPowerShell
    $nativeChild = Join-Path $tempRoot 'native-path-shadow-child.ps1'
    $nativeResult = Join-Path $tempRoot 'native-path-shadow-result.json'
    $nativeMarker = Join-Path $tempRoot 'native-pinned-child.marker'
    $nativeBody = @'
$ErrorActionPreference='Stop'; Set-StrictMode -Version Latest
. $env:V43_PATH_SHADOW_NATIVE_SOURCE
$fake = [string](Get-Command powershell.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$pinned = [IO.Path]::GetFullPath((Join-Path $PSHOME 'powershell.exe'))
$script = '[IO.File]::WriteAllText(' + (ConvertTo-V43PowerShellLiteral $env:V43_PATH_SHADOW_NATIVE_MARKER) + ', [Diagnostics.Process]::GetCurrentProcess().MainModule.FileName)'
$created = [V43NativeLaunchBoundary]::StartSuspended($pinned, (New-V43EncodedPowerShellCommand $pinned $script), $env:TEMP)
try {
    $resume = [V43NativeLaunchBoundary]::ResumeThread($created.thread)
    if ($resume -ne 1) { throw "Unexpected initial suspend count: $resume" }
    [void][V43NativeLaunchBoundary]::CloseHandle($created.thread)
    if ([V43NativeLaunchBoundary]::WaitForSingleObject($created.process, 10000) -ne 0) { throw 'Pinned native child did not exit.' }
    $exit = [uint32]::MaxValue
    if (-not [V43NativeLaunchBoundary]::GetExitCodeProcess($created.process, [ref]$exit) -or $exit -ne 0) { throw "Pinned native child exit was $exit." }
} finally {
    if ([V43NativeLaunchBoundary]::WaitForSingleObject($created.process, 0) -ne 0) {
        [void][V43NativeLaunchBoundary]::TerminateProcess($created.process, 125)
        [void][V43NativeLaunchBoundary]::WaitForSingleObject($created.process, 5000)
    }
    [void][V43NativeLaunchBoundary]::CloseHandle($created.thread)
    [void][V43NativeLaunchBoundary]::CloseHandle($created.process)
}
[IO.File]::WriteAllText($env:V43_PATH_SHADOW_NATIVE_RESULT, ([ordered]@{ fakePath=$fake; pinnedPath=$pinned; childPath=[IO.File]::ReadAllText($env:V43_PATH_SHADOW_NATIVE_MARKER); exitCode=$exit } | ConvertTo-Json -Compress), (New-Object Text.UTF8Encoding($false)))
'@
    Write-Utf8 $nativeChild $nativeBody
    $nativeRun = Invoke-Child $nativeChild @{
        PATH = ($shadowDir + ';' + $oldPath)
        V43_PATH_SHADOW_MARKER = (Join-Path $tempRoot 'native-shadow-ran.marker')
        V43_PATH_SHADOW_NATIVE_SOURCE = $sources.nativeLaunch
        V43_PATH_SHADOW_NATIVE_MARKER = $nativeMarker
        V43_PATH_SHADOW_NATIVE_RESULT = $nativeResult
    }
    if ($nativeRun.exitCode -ne 0 -or -not [IO.File]::Exists($nativeResult) -or
        [IO.File]::Exists((Join-Path $tempRoot 'native-shadow-ran.marker'))) {
        throw "Native CreateProcessW shadow-path fixture failed: $($nativeRun.stderr) $($nativeRun.stdout)"
    }
    $nativeEvidence = Get-Content -LiteralPath $nativeResult -Raw | ConvertFrom-Json
    if ($nativeEvidence.fakePath -ine $shadowPowerShell -or $nativeEvidence.pinnedPath -ine $nativeEvidence.childPath -or $nativeEvidence.exitCode -ne 0) {
        throw 'Native CreateProcessW did not execute the exact pinned PowerShell path under a fake-first PATH.'
    }
    [void]$rows.Add([pscustomobject][ordered]@{ route='native_launch_boundary.CreateProcessW'; status='PASS'; fakeFirstPathConfirmed=$true; pinnedApplicationPathExecuted=$true; shadowExecutableRan=$false; fixtureOnly=$true })

    # Exercise the real supervisor fixture-worker route with synthetic no-GO
    # pins. The supervisor child launch must remain bound to PSHOME under PATH shadowing.
    $supervisorChild = Join-Path $tempRoot 'supervisor-path-shadow-child.ps1'
    $supervisorResult = Join-Path $tempRoot 'supervisor-path-shadow-result.json'
    $supervisorEvidenceRoot = Join-Path $tempRoot 'supervisor-evidence'
    [void][IO.Directory]::CreateDirectory($supervisorEvidenceRoot)
    $supervisorBody = @'
$ErrorActionPreference='Stop'; Set-StrictMode -Version Latest
. $env:V43_PATH_SHADOW_COMMON_SOURCE
. $env:V43_PATH_SHADOW_SUPERVISOR_SOURCE
$fake = [string](Get-Command powershell.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$manifestPath = Join-Path $env:TEMP ('v43-path-shadow-candidate-' + [Guid]::NewGuid().ToString('N') + '.json')
$manifest = [ordered]@{
    schema='range.pc11-fresh-run-v43-admission-candidate/v1'; candidateVersion='v43'; goPermitted=$false
    productActions=[ordered]@{ guiLaunched=$false; savePerformed=$false; runPerformed=$false; confirmPerformed=$false }
    workflowBoundary=[ordered]@{ staticReadinessDoesNotGrantProductGo=$true; externalIndependentReviewRequired=$true; separateRootTechnicalDecisionRequired=$true; goSealRequiredBeforeAnyProductAction=$true }
    installBinding=[ordered]@{ installRoot=$env:TEMP; inventoryReportSha256=('1'*64) }
    sourceAndAuthorityPins=[ordered]@{ sourceFreezeReceiptSha256=('2'*64); rootCommit=('3'*40); sourceTreeSha256=('4'*64); releaseBuildReceiptSha256=('5'*64) }
}
try {
    [IO.File]::WriteAllText($manifestPath, (ConvertTo-Json -InputObject $manifest -Depth 8), (New-Object Text.UTF8Encoding($false)))
    $sha = Get-V43FileSha256 $manifestPath
    $identity = Get-V43CandidateManifestIdentitySha256 $manifest
    $result = Invoke-V43StaticFixtureSupervisor -CandidateManifestPath $manifestPath -CandidateManifestSha256 $sha -CandidateManifestIdentitySha256 $identity -FixtureEvidenceRoot $env:V43_PATH_SHADOW_SUPERVISOR_ROOT
    if (-not $result.success -or $result.status -cne 'FIXTURE_COMPLETE' -or $result.goPermitted -ne $false) { throw 'Synthetic supervisor fixture did not finish in no-GO state.' }
    [IO.File]::WriteAllText($env:V43_PATH_SHADOW_SUPERVISOR_RESULT, ([ordered]@{ fakePath=$fake; status=$result.status; workerCreatedSuspended=$result.workerCreatedSuspended; workerAssignedBeforeResume=$result.workerAssignedBeforeResume; exactWorkerJobMembershipVerified=$result.exactWorkerJobMembershipVerified; workerExitCode=$result.workerExitCode; goPermitted=$result.goPermitted; productActions=$result.productActions } | ConvertTo-Json -Depth 6 -Compress), (New-Object Text.UTF8Encoding($false)))
} finally { Remove-Item -LiteralPath $manifestPath -Force -ErrorAction SilentlyContinue }
'@
    Write-Utf8 $supervisorChild $supervisorBody
    $supervisorShadowMarker = Join-Path $tempRoot 'supervisor-shadow-ran.marker'
    $supervisorRun = Invoke-Child $supervisorChild @{
        PATH = ($shadowDir + ';' + $oldPath)
        V43_PATH_SHADOW_MARKER = $supervisorShadowMarker
        V43_PATH_SHADOW_COMMON_SOURCE = $sources.contractCommon
        V43_PATH_SHADOW_SUPERVISOR_SOURCE = $sources.supervisor
        V43_PATH_SHADOW_SUPERVISOR_ROOT = $supervisorEvidenceRoot
        V43_PATH_SHADOW_SUPERVISOR_RESULT = $supervisorResult
    }
    if ($supervisorRun.exitCode -ne 0 -or -not [IO.File]::Exists($supervisorResult) -or [IO.File]::Exists($supervisorShadowMarker)) {
        throw "Supervisor fake-first PATH fixture failed: $($supervisorRun.stderr) $($supervisorRun.stdout)"
    }
    $supervisorEvidence = Get-Content -LiteralPath $supervisorResult -Raw | ConvertFrom-Json
    if ($supervisorEvidence.fakePath -ine $shadowPowerShell -or $supervisorEvidence.status -cne 'FIXTURE_COMPLETE' -or
        -not $supervisorEvidence.workerCreatedSuspended -or -not $supervisorEvidence.workerAssignedBeforeResume -or
        -not $supervisorEvidence.exactWorkerJobMembershipVerified -or $supervisorEvidence.workerExitCode -ne 0 -or
        $supervisorEvidence.goPermitted -ne $false -or [IO.File]::Exists($supervisorShadowMarker)) {
        throw 'Supervisor child did not use its fixture-only pinned executable/worker route under fake-first PATH.'
    }
    foreach ($action in @('guiLaunched','savePerformed','runPerformed','confirmPerformed')) {
        if ($supervisorEvidence.productActions.$action -ne $false) { throw "Supervisor fixture permits product action $action." }
    }
    [void]$rows.Add([pscustomobject][ordered]@{ route='supervisor.worker_child_process'; status='PASS'; fakeFirstPathConfirmed=$true; pinnedPowerShellChildUsed=$true; fixtureWorkerRoute=$true; exactJobMembershipVerified=$true; shadowExecutableRan=$false; fixtureOnly=$true })

    # AST/forwarding checks bind source routes without launching candidate code; negative mutations must fail.
    $supervisorText = [IO.File]::ReadAllText($sources.supervisor)
    $nativeText = [IO.File]::ReadAllText($sources.nativeLaunch)
    if (-not (Test-SourceBindings $sources.packageTree $sources.supervisor $nativeText $sources.moduleBoundary)) { throw 'AST/forwarding source-binding contract failed.' }
    $mutatedSupervisor = [regex]::Replace($supervisorText, '(?m)(\[V43NativeLaunchBoundary\]::StartSuspended\()\$powerShell', '$1''powershell.exe''', 1)
    $mutatedSupervisorPath = Join-Path $tempRoot 'mutated-supervisor.ps1'; Write-Utf8 $mutatedSupervisorPath $mutatedSupervisor
    $supervisorMutationRejected = $false
    try { $supervisorMutationRejected = -not (Test-SourceBindings $sources.packageTree $mutatedSupervisorPath $nativeText $sources.moduleBoundary) }
    catch { $supervisorMutationRejected = $true }
    if ($mutatedSupervisor -ceq $supervisorText -or -not $supervisorMutationRejected) {
        throw 'Supervisor forwarding negative mutation did not establish checker sensitivity.'
    }
    $packageText = [IO.File]::ReadAllText($sources.packageTree)
    $mutatedPackage = [regex]::Replace($packageText, '(?m)(\$processInfo\.FileName\s*=\s*)\$PythonPath', '$1''python.exe''', 1)
    $originalPackageText = [IO.File]::ReadAllText($sources.packageTree)
    $mutatedPath = Join-Path $tempRoot 'mutated-package-tree.ps1'; Write-Utf8 $mutatedPath $mutatedPackage
    $mutatedPackageAst = Get-ParsedAst $mutatedPath
    $mutatedInventoryAst = $mutatedPackageAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Invoke-InventoryReview' }, $true)
    $mutatedAssignments = @($mutatedInventoryAst[0].FindAll({ param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -ceq '$processInfo.FileName' }, $true))
    if ($mutatedPackage -ceq $originalPackageText -or $mutatedAssignments.Count -ne 1 -or $mutatedAssignments[0].Right.Extent.Text -ceq '$PythonPath') { throw 'Package-tree bare executable negative mutation was not rejected.' }
    [void]$rows.Add([pscustomobject][ordered]@{ route='source_bindings'; status='PASS'; packageTreeAstPinned=$true; supervisorAstForwarded=$true; nativeCreateProcessForwardedApplication=$true; moduleManifestAstPinned=$true; supervisorAndPackageNegativeMutationsRejected=$true; note='AST/forwarding source checks plus separate fake-first PATH native/supervisor fixture routes.' })
}
catch { $failure = $_.Exception.Message }
finally {
    $env:PATH = $oldPath; $env:PSModulePath = $oldModulePath
    Remove-Item Env:V43_PATH_SHADOW_MARKER -ErrorAction SilentlyContinue
    Remove-Item Env:V43_PATH_SHADOW_MODULE_RESULT -ErrorAction SilentlyContinue
    Remove-Item Env:V43_PATH_SHADOW_MODULE_LOADED -ErrorAction SilentlyContinue
    Remove-Item Env:V43_PATH_SHADOW_MODULE_CALLED -ErrorAction SilentlyContinue
}

$pending = @(
    [pscustomobject][ordered]@{ route='root_bootstrap_external_entry'; status='PENDING'; reason='External bootstrap executable and root seal are supplied at admission time and are not present as a candidate-pinned route in this package tree.' }
)
$status = if ($null -ne $failure) { 'FAIL' } else { 'STATIC_INCOMPLETE' }
$finalSourceHashes = @($sources.GetEnumerator() | ForEach-Object {
    $hash = Get-Hash $_.Value
    if ($hash -notmatch '^[0-9a-f]{64}$') { throw "Final source SHA256 is malformed for route $($_.Key)." }
    [pscustomobject][ordered]@{ route = $_.Key; path = [IO.Path]::GetFullPath($_.Value); sha256 = $hash }
})
if ($sourceHashes.Count -ne $finalSourceHashes.Count) { throw 'Source hash row count changed during path-shadow fixture.' }
foreach ($index in 0..($sourceHashes.Count - 1)) {
    if ($sourceHashes[$index].route -cne $finalSourceHashes[$index].route -or $sourceHashes[$index].sha256 -cne $finalSourceHashes[$index].sha256) {
        throw "Source bytes changed during path-shadow fixture: $($sourceHashes[$index].route)."
    }
}
$receipt = [ordered]@{
    schema='range.pc11-v43-path-shadow-receipt/v1'
    status=$status; candidateVersion='v43'; variant='OFF'; staticOnly=$true; fixtureOnly=$true
    createdUtc=[DateTime]::UtcNow.ToString('o'); sourceHashes=$finalSourceHashes; testedRoutes=@($rows.ToArray()); pendingRoutes=$pending
    failure=$failure
    productActions=[ordered]@{ guiLaunched=$false; savePerformed=$false; runPerformed=$false; confirmPerformed=$false }
    goPermitted=$false
}
$parent = [IO.Path]::GetDirectoryName($OutputPath)
if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
Assert-NoReparseAncestors $AuditRoot
Assert-NoReparseAncestors $parent
if (-not $OutputPath.StartsWith($AuditRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Receipt escaped the audit root after path revalidation.' }
$receiptBytes = (New-Object Text.UTF8Encoding($false)).GetBytes((ConvertTo-Json -InputObject $receipt -Depth 12))
$receiptStream = [IO.File]::Open($OutputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $receiptStream.Write($receiptBytes, 0, $receiptBytes.Length); $receiptStream.Flush($true) }
finally { $receiptStream.Dispose() }
Write-Output ("{0} tested={1} pending={2} receipt={3}" -f $status, $rows.Count, $pending.Count, $OutputPath)
if ($null -ne $failure) { exit 1 }
