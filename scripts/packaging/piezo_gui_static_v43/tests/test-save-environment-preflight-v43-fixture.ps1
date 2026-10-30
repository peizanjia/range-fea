[CmdletBinding()]
param([string]$OutputPath)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) { throw 'Fixture requires Windows PowerShell 5.1.' }
$packageRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoRoot = [IO.Path]::GetFullPath((Join-Path $packageRoot '..\..\..'))
$auditRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit'))
$fixtureRoot = Join-Path $auditRoot ('save-environment-fixture-v43-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($fixtureRoot)
if (-not $OutputPath) { $OutputPath = Join-Path $fixtureRoot 'fixture-receipt.json' }
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if (-not $OutputPath.StartsWith($auditRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture receipt must remain under external audit root.' }

function Hash-Bytes([byte[]]$Bytes) { $s=[Security.Cryptography.SHA256]::Create(); try { return ([BitConverter]::ToString($s.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant() } finally { $s.Dispose() } }
function Hash-File([string]$Path) { Hash-Bytes ([IO.File]::ReadAllBytes($Path)) }
function Write-Json([string]$Path, $Value) { [IO.File]::WriteAllText($Path, ($Value | Microsoft.PowerShell.Utility\ConvertTo-Json -Depth 20 -Compress), (New-Object Text.UTF8Encoding($false))) }
function Quote-Arg([string]$Value) { return '"' + $Value.Replace('"','\"') + '"' }
function Invoke-Preflight([string]$Manifest, [string]$ManifestHash, [string]$Freeze, [string]$FreezeHash, [string]$Out, [string]$PathValue, [string]$ModulesValue) {
    $exe = Join-Path $PSHOME 'powershell.exe'
    $script = Join-Path $packageRoot 'tests\test-save-environment-preflight-v43.ps1'
    $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Quote-Arg $script),'-CandidateManifestPath',(Quote-Arg $Manifest),'-CandidateManifestSha256',$ManifestHash,'-SourceFreezePath',(Quote-Arg $Freeze),'-SourceFreezeSha256',$FreezeHash,'-SystemModuleBoundarySha256',(Hash-File $helperScript),'-OutputPath',(Quote-Arg $Out)) -join ' '
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName=$exe; $start.Arguments=$arguments; $start.WorkingDirectory=$repoRoot; $start.UseShellExecute=$false
    $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true; $start.CreateNoWindow=$true
    $start.EnvironmentVariables['PATH']=$PathValue; $start.EnvironmentVariables['PSModulePath']=$ModulesValue
    $p=New-Object Diagnostics.Process; $p.StartInfo=$start
    try { [void]$p.Start(); $o=$p.StandardOutput.ReadToEnd(); $e=$p.StandardError.ReadToEnd(); $p.WaitForExit(); return [pscustomobject]@{ code=$p.ExitCode; stdout=$o; stderr=$e } } finally { $p.Dispose() }
}

$sourceScript = Join-Path $packageRoot 'tests\test-save-environment-preflight-v43.ps1'
$helperScript = Join-Path $packageRoot 'tools\system-module-boundary-v43.ps1'
$sourceText = [IO.File]::ReadAllText($sourceScript)
$sourceTokens = $null
$sourceParseErrors = $null
$sourceAst = [Management.Automation.Language.Parser]::ParseFile($sourceScript, [ref]$sourceTokens, [ref]$sourceParseErrors)
if ($sourceParseErrors.Count -ne 0) { throw 'Production preflight source did not parse cleanly.' }
$lockFunctionAsts = @($sourceAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Get-FileSha256' }, $true))
if ($lockFunctionAsts.Count -ne 1) { throw 'Expected one production Get-FileSha256 function for the isolated lock probe.' }
$lockFunctionText = $lockFunctionAsts[0].Extent.Text
$lockOpenAt = $lockFunctionText.IndexOf('[IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)', [StringComparison]::Ordinal)
$handleRetainAt = $lockFunctionText.IndexOf('$script:installedFileHandles.Add($stream)', [StringComparison]::Ordinal)
$streamTransferAt = $lockFunctionText.IndexOf('$stream = $null', [StringComparison]::Ordinal)
$installedHashAt = $sourceText.IndexOf('Get-FileSha256 $full $true', [StringComparison]::Ordinal)
$receiptWriteAt = $sourceText.IndexOf('$stream.Write($receiptBytes', [StringComparison]::Ordinal)
$handleReleaseAt = $sourceText.IndexOf('foreach ($installedHandle in $installedFileHandles)', [StringComparison]::Ordinal)
$installedDisposeAt = $sourceText.IndexOf('$installedHandle.Dispose()', [StringComparison]::Ordinal)
$handleClearAt = $sourceText.IndexOf('$installedFileHandles.Clear()', [StringComparison]::Ordinal)
if ($lockOpenAt -lt 0 -or $handleRetainAt -le $lockOpenAt -or $streamTransferAt -le $handleRetainAt -or
    $installedHashAt -lt 0 -or $receiptWriteAt -le $installedHashAt -or $handleReleaseAt -le $receiptWriteAt -or
    $installedDisposeAt -le $handleReleaseAt -or $handleClearAt -le $installedDisposeAt) {
    throw 'Production installed-file lock function or receipt lifetime ordering is invalid.'
}
$script:installedFileHandles = New-Object 'System.Collections.Generic.List[IO.FileStream]'
. ([scriptblock]::Create($lockFunctionText))
$lockProbePath = Join-Path $fixtureRoot 'installed-file-lock-probe.bin'
[IO.File]::WriteAllText($lockProbePath, 'isolated installed-file lock probe', (New-Object Text.UTF8Encoding($false)))
$lockProbeExpectedHash = Hash-File $lockProbePath
$lockProbeActualHash = Get-FileSha256 $lockProbePath $true
if ($lockProbeActualHash -cne $lockProbeExpectedHash -or $script:installedFileHandles.Count -ne 1) {
    throw 'Extracted production hash function did not retain exactly one read handle.'
}
function Assert-SharingViolation([scriptblock]$Operation, [string]$Label) {
    $sharingViolation = $false
    $opened = $null
    try { $opened = & $Operation }
    catch [IO.IOException] {
        if (($_.Exception.HResult -band 0xFFFF) -ne 32) { throw "$Label raised an I/O error other than ERROR_SHARING_VIOLATION: $($_.Exception.Message)" }
        $sharingViolation = $true
    }
    finally { if ($opened -is [IDisposable]) { $opened.Dispose() } }
    if (-not $sharingViolation) { throw "$Label was not blocked by the retained production read handle." }
}
Assert-SharingViolation { [IO.File]::Open($lockProbePath, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite) } 'Competing writer'
Assert-SharingViolation { [IO.File]::Delete($lockProbePath) } 'Competing delete'
if (-not [IO.File]::Exists($lockProbePath)) { throw 'Competing delete removed the file despite the retained production read handle.' }
foreach ($installedHandle in $script:installedFileHandles) { $installedHandle.Dispose() }
$script:installedFileHandles.Clear()
$writer = [IO.File]::Open($lockProbePath, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
$writer.Dispose()
if ((Hash-File $lockProbePath) -cne $lockProbeExpectedHash) { throw 'Post-release writer probe changed the synthetic file bytes.' }
[IO.File]::Delete($lockProbePath)
if ([IO.File]::Exists($lockProbePath)) { throw 'Writer or delete remained blocked after retained production handles were disposed.' }
$readShareToken = '[IO.FileShare]::Read'
if ($lockFunctionText.IndexOf($readShareToken, [StringComparison]::Ordinal) -lt 0 -or
    $lockFunctionText.IndexOf($readShareToken, [StringComparison]::Ordinal) -ne $lockFunctionText.LastIndexOf($readShareToken, [StringComparison]::Ordinal)) {
    throw 'Expected one production read-share token for the isolated mutation control.'
}
$mutatedLockFunctionText = $lockFunctionText.Replace($readShareToken, '[IO.FileShare]::ReadWrite')
if ($mutatedLockFunctionText -ceq $lockFunctionText) { throw 'Mutation control did not change the production file-sharing mode.' }
. ([scriptblock]::Create($mutatedLockFunctionText))
$mutationProbePath = Join-Path $fixtureRoot 'installed-file-lock-mutation-control.bin'
[IO.File]::WriteAllText($mutationProbePath, 'isolated mutated lock control', (New-Object Text.UTF8Encoding($false)))
$mutationProbeExpectedHash = Hash-File $mutationProbePath
$mutationProbeActualHash = Get-FileSha256 $mutationProbePath $true
if ($mutationProbeActualHash -cne $mutationProbeExpectedHash -or $script:installedFileHandles.Count -ne 1) {
    throw 'Mutated production function did not retain exactly one read handle on the isolated control path.'
}
$mutationWriter = $null
try { $mutationWriter = [IO.File]::Open($mutationProbePath, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite) }
catch { throw "FileShare.ReadWrite mutation unexpectedly blocked a competing writer: $($_.Exception.Message)" }
finally { if ($null -ne $mutationWriter) { $mutationWriter.Dispose() } }
if ((Hash-File $mutationProbePath) -cne $mutationProbeExpectedHash) { throw 'Mutation control wrote to the isolated synthetic file.' }
foreach ($installedHandle in $script:installedFileHandles) { $installedHandle.Dispose() }
$script:installedFileHandles.Clear()
[IO.File]::Delete($mutationProbePath)
if ([IO.File]::Exists($mutationProbePath)) { throw 'Mutation-control cleanup could not delete its isolated synthetic file.' }
$lockProbeScope = 'unit-level AST-extracted production function plus isolated FileShare.ReadWrite mutation control; no end-to-end preflight concurrency claim'
$installRoot = Join-Path $fixtureRoot 'install'
$files = @{
    'bin/fea.exe'='fixture gui executable';
    'runtime/python/python.exe'='fixture private python';
    'bin/fea-solver.exe'='fixture solver';
    'share/range-workflow/range_workflow/runtime_control.py'='fixture control';
    'share/range-workflow/range_workflow/piezo_native_backend.py'='fixture backend'
}
$hashes = @{}
foreach ($relative in $files.Keys) {
    $full = Join-Path $installRoot ($relative.Replace('/','\'))
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($full))
    [IO.File]::WriteAllText($full, $files[$relative], (New-Object Text.UTF8Encoding($false)))
    $hashes[$relative]=Hash-File $full
}
$sourceRows = @(
    [pscustomobject]@{path='scripts/packaging/piezo_gui_static_v43/tests/test-save-environment-preflight-v43.ps1';sizeBytes=(Get-Item $sourceScript).Length;sha256=(Hash-File $sourceScript)},
    [pscustomobject]@{path='scripts/packaging/piezo_gui_static_v43/tools/system-module-boundary-v43.ps1';sizeBytes=(Get-Item $helperScript).Length;sha256=(Hash-File $helperScript)}
)
$buildRoot = Join-Path $fixtureRoot 'build-off-release'
[void][IO.Directory]::CreateDirectory($buildRoot)
$build = [ordered]@{
    schema='range.pc11-v43-release-build-receipt/v1'; status='PASS'; candidateVersion='v43'; variant='OFF'; configuration='Release';
    rootCommit=('a'*40); sourceTreeSha256=('b'*64); buildDirectory=$buildRoot; buildExitCode=0; staticOnly=$true;
    productActions=[ordered]@{guiLaunched=$false;savePerformed=$false;runPerformed=$false;confirmPerformed=$false}; goPermitted=$false
}
$buildPath=Join-Path $fixtureRoot 'release-build.json'; Write-Json $buildPath $build
$buildHash=Hash-File $buildPath
$inventory = [ordered]@{
    schemaVersion=2; inventoryStatus='PASS'; variant='OFF'; installRoot=$installRoot; buildRoot=$buildRoot;
    fileCount=$files.Count; files=@($files.Keys | Sort-Object | ForEach-Object { [ordered]@{path=$_;sha256=$hashes[$_];sizeBytes=([IO.FileInfo](Join-Path $installRoot ($_.Replace('/','\')))).Length} });
    directories=@('bin','runtime','runtime/python','share','share/range-workflow','share/range-workflow/range_workflow')
}
$inventoryPath=Join-Path $fixtureRoot 'inventory.json'; Write-Json $inventoryPath $inventory
$inventoryHash=Hash-File $inventoryPath
$freeze = [ordered]@{
    schema='range.pc11-v43-source-freeze/v1'; status='PASS'; candidateVersion='v43'; variant='OFF'; configuration='Release';
    rootCommit=('a'*40); sourceTreeSha256=('b'*64); sourceRootPath=$repoRoot; sourceFiles=$sourceRows; releaseBuildReceiptSha256=$buildHash; staticOnly=$true;
    productActions=[ordered]@{guiLaunched=$false;savePerformed=$false;runPerformed=$false;confirmPerformed=$false}; goPermitted=$false
}
$freezePath=Join-Path $fixtureRoot 'source-freeze.json'; Write-Json $freezePath $freeze
$freezeHash=Hash-File $freezePath
$manifest=[ordered]@{
    schema='range.pc11-fresh-run-v43-admission-candidate/v1'; candidateVersion='v43'; candidateState='READY_FOR_STATIC_PREFLIGHT'; goPermitted=$false;
    productActions=[ordered]@{guiLaunched=$false;savePerformed=$false;runPerformed=$false;confirmPerformed=$false};
    installBinding=[ordered]@{requiredVariant='OFF';installRoot=$installRoot;inventoryReportPath=$inventoryPath;inventoryReportSha256=$inventoryHash;inventorySchemaVersion=2;inventoryStatus='PASS'};
    installedPathLayout=[ordered]@{
        guiExecutableRelativePath='bin/fea.exe';guiExecutableSha256=$hashes['bin/fea.exe']; privatePythonRelativePath='runtime/python/python.exe';privatePythonSha256=$hashes['runtime/python/python.exe'];
        solverExecutableRelativePath='bin/fea-solver.exe';solverExecutableSha256=$hashes['bin/fea-solver.exe'];
        productionControlRelativePath='share/range-workflow/range_workflow/runtime_control.py';productionControlSha256=$hashes['share/range-workflow/range_workflow/runtime_control.py'];
        nativeBackendRelativePath='share/range-workflow/range_workflow/piezo_native_backend.py';nativeBackendSha256=$hashes['share/range-workflow/range_workflow/piezo_native_backend.py']
    };
    sourceAndAuthorityPins=[ordered]@{sourceRootPath=$repoRoot;sourceTreeSha256=$freeze.sourceTreeSha256;rootCommit=$freeze.rootCommit;sourceFreezeReceiptPath=$freezePath;sourceFreezeReceiptSha256=$freezeHash;releaseBuildReceiptPath=$buildPath;releaseBuildReceiptSha256=$buildHash}
}
$manifestPath=Join-Path $fixtureRoot 'candidate.json'; Write-Json $manifestPath $manifest
$manifestHash=Hash-File $manifestPath

$fakeModules=Join-Path $fixtureRoot 'fake-modules'; $moduleDir=Join-Path $fakeModules 'Microsoft.PowerShell.Utility'; [void][IO.Directory]::CreateDirectory($moduleDir)
$marker=Join-Path $fixtureRoot 'shadow-module.marker'; $called=Join-Path $fixtureRoot 'shadow-command.marker'
$module="[IO.File]::WriteAllText('$marker','loaded'); function ConvertFrom-Json { [IO.File]::WriteAllText('$called','called'); return @{} }; Export-ModuleMember -Function ConvertFrom-Json"
[IO.File]::WriteAllText((Join-Path $moduleDir 'Microsoft.PowerShell.Utility.psm1'),$module,(New-Object Text.UTF8Encoding($false)))
[IO.File]::WriteAllText((Join-Path $moduleDir 'Microsoft.PowerShell.Utility.psd1'),"@{RootModule='Microsoft.PowerShell.Utility.psm1';ModuleVersion='1.0.0';GUID='$([Guid]::NewGuid())';FunctionsToExport=@('ConvertFrom-Json')}",(New-Object Text.UTF8Encoding($false)))
$fakePath=Join-Path $fixtureRoot 'fake-path'; [void][IO.Directory]::CreateDirectory($fakePath)
foreach($n in @('python','powershell')) { [IO.File]::WriteAllText((Join-Path $fakePath ($n+'.cmd')),"@echo off`r`necho called>$called`r`nexit /b 0`r`n",[Text.Encoding]::ASCII) }
$goodOut=Join-Path $fixtureRoot 'positive.json'
$good=Invoke-Preflight $manifestPath $manifestHash $freezePath $freezeHash $goodOut ($fakePath+';'+$env:PATH) ($fakeModules+';'+(Join-Path $PSHOME 'Modules'))
if($good.code -ne 0 -or -not (Test-Path $goodOut)) { throw "Positive fixture failed: $($good.stderr) $($good.stdout)" }
$goodReceipt=([IO.File]::ReadAllText($goodOut) | Microsoft.PowerShell.Utility\ConvertFrom-Json)
if($goodReceipt.status -cne 'PASS' -or $goodReceipt.candidateGateStatus -cne 'PASS' -or $goodReceipt.goPermitted -ne $false) { throw 'Positive synthetic candidate did not produce bounded PASS.' }
if((Test-Path $marker) -or (Test-Path $called)) { throw 'Fake-first module shadow was loaded or invoked.' }

$templatePath=Join-Path $packageRoot 'candidate-manifest-v43.admission-template.json'
$templateOut=Join-Path $fixtureRoot 'template-pending.json'
$template=Invoke-Preflight $templatePath (Hash-File $templatePath) (Join-Path $fixtureRoot 'missing-freeze.json') ('0'*64) $templateOut $env:PATH $env:PSModulePath
if($template.code -ne 0 -or -not (Test-Path $templateOut)) { throw "Pending template fixture failed: $($template.stderr) $($template.stdout)" }
$templateReceipt=([IO.File]::ReadAllText($templateOut) | Microsoft.PowerShell.Utility\ConvertFrom-Json)
if($templateReceipt.status -cne 'PENDING' -or $templateReceipt.candidateGateStatus -cne 'PENDING' -or $templateReceipt.goPermitted -ne $false) { throw 'Template or absent freeze was not retained as PENDING.' }

$tampered=Join-Path $fixtureRoot 'candidate-tampered.json'; [IO.File]::WriteAllText($tampered,([IO.File]::ReadAllText($manifestPath)+' '),(New-Object Text.UTF8Encoding($false)))
$badOut=Join-Path $fixtureRoot 'tamper.json'
$bad=Invoke-Preflight $tampered $manifestHash $freezePath $freezeHash $badOut $env:PATH $env:PSModulePath
if($bad.code -eq 0 -or -not (Test-Path $badOut)) { throw 'Manifest tamper was not rejected.' }
$badReceipt=([IO.File]::ReadAllText($badOut) | Microsoft.PowerShell.Utility\ConvertFrom-Json)
if($badReceipt.status -cne 'FAIL' -or $badReceipt.candidateGateStatus -cne 'PENDING' -or $badReceipt.goPermitted -ne $false) { throw 'Manifest tamper receipt did not fail closed.' }

$tamperedFreeze=Join-Path $fixtureRoot 'source-freeze-tampered.json'
[IO.File]::WriteAllText($tamperedFreeze,([IO.File]::ReadAllText($freezePath)+' '),(New-Object Text.UTF8Encoding($false)))
$freezeBadOut=Join-Path $fixtureRoot 'freeze-tamper.json'
$freezeBad=Invoke-Preflight $manifestPath $manifestHash $tamperedFreeze $freezeHash $freezeBadOut $env:PATH $env:PSModulePath
if($freezeBad.code -eq 0 -or -not (Test-Path $freezeBadOut)) { throw 'Source-freeze tamper was not rejected.' }
$freezeBadReceipt=([IO.File]::ReadAllText($freezeBadOut) | Microsoft.PowerShell.Utility\ConvertFrom-Json)
if($freezeBadReceipt.status -cne 'FAIL' -or $freezeBadReceipt.candidateGateStatus -cne 'PENDING') { throw 'Source-freeze tamper receipt did not fail closed.' }

foreach ($negative in @(
    [pscustomobject]@{name='on-variant'; mutate={param($m) $m.installBinding.requiredVariant='ON'}},
    [pscustomobject]@{name='failed-inventory'; mutate={param($m) $m.installBinding.inventoryStatus='FAIL'}},
    [pscustomobject]@{name='array-go'; mutate={param($m) $m.goPermitted=@($false)}}
)) {
    $changed = Microsoft.PowerShell.Utility\ConvertFrom-Json -InputObject ([IO.File]::ReadAllText($manifestPath))
    & $negative.mutate $changed
    $changedPath = Join-Path $fixtureRoot ($negative.name + '-manifest.json')
    Write-Json $changedPath $changed
    $changedOut = Join-Path $fixtureRoot ($negative.name + '-result.json')
    $run = Invoke-Preflight $changedPath (Hash-File $changedPath) $freezePath $freezeHash $changedOut $env:PATH $env:PSModulePath
    if ($run.code -eq 0 -or -not [IO.File]::Exists($changedOut)) { throw "Invalid candidate $($negative.name) did not fail closed." }
    $changedReceipt = Microsoft.PowerShell.Utility\ConvertFrom-Json -InputObject ([IO.File]::ReadAllText($changedOut))
    if ($changedReceipt.status -cne 'FAIL' -or $changedReceipt.candidateGateStatus -cne 'PENDING') { throw "Invalid candidate $($negative.name) produced an unexpected status." }
}

$summary=[ordered]@{schema='range.pc11-v43-save-environment-preflight-fixture/v1';status='PASS';cases=@('synthetic-candidate-positive','installed-file-lock-compatible-writer-and-delete-sharing-violations','installed-file-lock-readwrite-mutation-control','installed-file-hash-receipt-release-source-order','template-remains-pending-without-freeze','externally-pinned-manifest-tamper','externally-pinned-source-freeze-tamper','fake-first-PSModulePath','fake-first-PATH','on-variant-rejected','failed-inventory-rejected','array-go-rejected');lockProbeScope=$lockProbeScope;productActions=[ordered]@{guiLaunched=$false;savePerformed=$false;runPerformed=$false;confirmPerformed=$false};goPermitted=$false}
if([IO.File]::Exists($OutputPath)){throw "Refusing to overwrite fixture receipt: $OutputPath"}
[IO.File]::WriteAllText($OutputPath,($summary|Microsoft.PowerShell.Utility\ConvertTo-Json -Depth 8)+"`n",(New-Object Text.UTF8Encoding($false)))
Write-Output ("PASS cases={0} output={1}" -f $summary.cases.Count,$OutputPath)
