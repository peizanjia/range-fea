[CmdletBinding()]
param([string]$AuditRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) { throw 'This test requires Windows PowerShell 5.1.' }
$checkout=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..'))
$prep=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$cmake=Get-Command cmake.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1
$testRoot=Join-Path $checkout ('build\audit-20261014\piezo-delivery\off-build-receipt-fixture-' + [Guid]::NewGuid().ToString('N'))
$repo=$testRoot; $source=Join-Path $repo 'src'; $build=Join-Path $repo 'build\off-release'
$audit=Join-Path $repo 'build\audit'
[void][IO.Directory]::CreateDirectory($source)
[void][IO.Directory]::CreateDirectory((Join-Path $repo 'scripts\ci'))
[void][IO.Directory]::CreateDirectory($audit)
function Invoke-ProducerChild([string[]]$ChildArgs, [string]$Tag) {
    $stdoutPath=Join-Path $audit ($Tag+'.stdout'); $stderrPath=Join-Path $audit ($Tag+'.stderr')
    $quoted=@($ChildArgs | ForEach-Object { '"'+([string]$_).Replace('"','\"')+'"' })
    $child=Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -ArgumentList ([string]::Join(' ',$quoted)) -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -NoNewWindow -Wait -PassThru
    return [int]$child.ExitCode
}
function Get-TestBytesSha256([byte[]]$Bytes) {
    $sha=[Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Quote-TestArgument([string]$Value) {
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $builder=New-Object Text.StringBuilder; [void]$builder.Append('"'); $slashes=0
    foreach($character in $Value.ToCharArray()) {
        if($character -eq '\'){$slashes++;continue}
        if($character -eq '"'){[void]$builder.Append(('\'*(2*$slashes+1)));[void]$builder.Append('"');$slashes=0;continue}
        if($slashes -gt 0){[void]$builder.Append(('\'*$slashes));$slashes=0};[void]$builder.Append($character)
    }
    if($slashes -gt 0){[void]$builder.Append(('\'*(2*$slashes)))};[void]$builder.Append('"');return $builder.ToString()
}
function Invoke-ModuleBoundaryCase([string[]]$ChildArgs,[string]$Tag,[string]$Mode,[string]$ModulePath,[string]$ProducerPath) {
    $argsPath=Join-Path $audit ($Tag+'.args.json'); $wrapperPath=Join-Path $audit ($Tag+'.wrapper.ps1')
    $stdoutPath=Join-Path $audit ($Tag+'.stdout'); $stderrPath=Join-Path $audit ($Tag+'.stderr'); $marker=Join-Path $audit ($Tag+'.marker')
    [IO.File]::WriteAllText($argsPath,(ConvertTo-Json -InputObject @($ChildArgs) -Compress),(New-Object Text.UTF8Encoding($false)))
    $wrapper=@'
if ($env:V43_TEST_MODE -ceq 'preloaded-join-path') {
    function global:Join-Path { [IO.File]::WriteAllText($env:V43_TEST_MARKER,'invoked'); return 'SHADOWED' }
}
$argBytes=[IO.File]::ReadAllBytes($env:V43_TEST_ARGS_PATH)
$childArgs=(New-Object Text.UTF8Encoding($false,$true)).GetString($argBytes) | ConvertFrom-Json
$producerParameters=@{}
for($index=0;$index -lt $childArgs.Count;$index+=2){$producerParameters[([string]$childArgs[$index]).TrimStart('-')]=[string]$childArgs[$index+1]}
& $env:V43_TEST_PRODUCER_PATH @producerParameters
'@
    [IO.File]::WriteAllText($wrapperPath,$wrapper,(New-Object Text.UTF8Encoding($false)))
    $powershell=Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $arguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',$wrapperPath);$quoted=@($arguments|ForEach-Object{Quote-TestArgument ([string]$_)})
    $psi=New-Object Diagnostics.ProcessStartInfo;$psi.FileName=$powershell;$psi.Arguments=[string]::Join(' ',$quoted);$psi.WorkingDirectory=$checkout
    $psi.UseShellExecute=$false;$psi.CreateNoWindow=$true;$psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
    $psi.EnvironmentVariables['V43_TEST_MODE']=$Mode;$psi.EnvironmentVariables['V43_TEST_ARGS_PATH']=$argsPath;$psi.EnvironmentVariables['V43_TEST_PRODUCER_PATH']=$ProducerPath;$psi.EnvironmentVariables['V43_TEST_MARKER']=$marker
    $psi.EnvironmentVariables['PSModulePath']=$ModulePath
    $process=New-Object Diagnostics.Process;$process.StartInfo=$psi
    try {if(-not $process.Start()){throw 'Could not start module-boundary child.'};$errTask=$process.StandardError.ReadToEndAsync();$out=$process.StandardOutput.ReadToEnd();$process.WaitForExit();return [pscustomobject]@{exitCode=[int]$process.ExitCode;stdout=$out;stderr=[string]$errTask.Result;marker=$marker}}
    finally{$process.Dispose()}
}
try {
    $producerPath=Join-Path $prep 'release-build-v43.ps1'
    $tokens=$null; $parseErrors=$null
    $producerAst=[System.Management.Automation.Language.Parser]::ParseFile($producerPath,[ref]$tokens,[ref]$parseErrors)
    if ($parseErrors.Count -gt 0) { throw 'Could not parse receipt producer for byte-snapshot function fixture.' }
    $shaFunction=$null; $snapshotFunction=$null
    $validatorFunctionNames=@('Get-RequiredProperty','Assert-JsonObject','Assert-StringField','Assert-ShaField','Assert-JsonInteger','Assert-SourcePreflight')
    $validatorFunctions=New-Object 'System.Collections.Generic.List[object]'
    foreach ($functionAst in $producerAst.FindAll({param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst]},$true)) {
        if ($functionAst.Name -ceq 'Get-Sha256Bytes') { $shaFunction=$functionAst }
        if ($functionAst.Name -ceq 'Get-DocumentSnapshot') { $snapshotFunction=$functionAst }
        if ($validatorFunctionNames -contains $functionAst.Name) { [void]$validatorFunctions.Add($functionAst) }
    }
    if ($null -eq $shaFunction -or $null -eq $snapshotFunction -or $validatorFunctions.Count -ne $validatorFunctionNames.Count) { throw 'Producer snapshot or typed-validator functions were not found.' }
    . ([scriptblock]::Create($shaFunction.Extent.Text))
    . ([scriptblock]::Create($snapshotFunction.Extent.Text))
    foreach ($validatorFunction in $validatorFunctions) { . ([scriptblock]::Create($validatorFunction.Extent.Text)) }
    $swapPath=Join-Path $audit 'snapshot-swap.json'
    $bytesA=(New-Object Text.UTF8Encoding($false)).GetBytes('{"identity":"A"}')
    $bytesB=(New-Object Text.UTF8Encoding($false)).GetBytes('{"identity":"B"}')
    [IO.File]::WriteAllBytes($swapPath,$bytesA)
    $capturedA=[IO.File]::ReadAllBytes($swapPath)
    [IO.File]::WriteAllBytes($swapPath,$bytesB)
    $snapshotA=Get-DocumentSnapshot $capturedA 'A/B replacement fixture'
    if ($snapshotA.document.identity -cne 'A' -or $snapshotA.sha256 -cne (Get-TestBytesSha256 $bytesA) -or $snapshotA.sha256 -ceq (Get-FileHash $swapPath -Algorithm SHA256).Hash.ToLowerInvariant()) { throw 'A/B replacement let parsed document A pair with hash B.' }
    $capturedB=[IO.File]::ReadAllBytes($swapPath)
    [IO.File]::WriteAllBytes($swapPath,$bytesA)
    $snapshotB=Get-DocumentSnapshot $capturedB 'B/A replacement fixture'
    if ($snapshotB.document.identity -cne 'B' -or $snapshotB.sha256 -cne (Get-TestBytesSha256 $bytesB) -or $snapshotB.sha256 -ceq (Get-FileHash $swapPath -Algorithm SHA256).Hash.ToLowerInvariant()) { throw 'B/A replacement let parsed document B pair with hash A.' }

    [IO.File]::WriteAllText((Join-Path $repo '.gitignore'),"build/`n",(New-Object Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText((Join-Path $repo 'scripts\ci\required-ctest-tests.json'),'{"schema_version":1,"required_test_groups":{"fixture":["fixture_test"]}}'+"`n",(New-Object Text.UTF8Encoding($false)))
    $cmakeLists=@'
cmake_minimum_required(VERSION 3.20)
project(RangeFea NONE)
option(RANGE_ENABLE_AGENT "fixture OFF setting" OFF)
set(CMAKE_CONFIGURATION_TYPES "Debug;Release;MinSizeRel;RelWithDebInfo" CACHE STRING "" FORCE)
option(FIXTURE_FAIL "make the synthetic build fail" OFF)
if(FIXTURE_FAIL)
  add_custom_target(fixture ALL COMMAND "${CMAKE_COMMAND}" -E false VERBATIM)
else()
  add_custom_target(fixture ALL COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_SOURCE_DIR}/write_release.cmake" VERBATIM)
endif()
'@
    [IO.File]::WriteAllText((Join-Path $source 'CMakeLists.txt'),$cmakeLists,(New-Object Text.UTF8Encoding($false)))
    $writeScript=@'
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/Release" "${CMAKE_BINARY_DIR}/fea/Release")
file(WRITE "${CMAKE_BINARY_DIR}/Release/fea.exe" "synthetic output for receipt-producer test")
file(WRITE "${CMAKE_BINARY_DIR}/fea/Release/fea-link.map" "synthetic map for receipt-producer test")
'@
    [IO.File]::WriteAllText((Join-Path $source 'write_release.cmake'),$writeScript,(New-Object Text.UTF8Encoding($false)))
    $git=Get-Command git.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1
    & $git.Source -C $repo init -q
    if ($LASTEXITCODE -ne 0) { throw 'Could not initialize isolated miniature Git repository.' }
    & $git.Source -C $repo config user.name 'v43 fixture'
    & $git.Source -C $repo config user.email 'v43-fixture@example.invalid'
    $submoduleSeed=$testRoot+'.submodule-seed'
    [void][IO.Directory]::CreateDirectory($submoduleSeed)
    & $git.Source -C $submoduleSeed init -q
    & $git.Source -C $submoduleSeed config user.name 'v43 fixture'
    & $git.Source -C $submoduleSeed config user.email 'v43-fixture@example.invalid'
    [IO.File]::WriteAllText((Join-Path $submoduleSeed 'README.txt'),'isolated submodule fixture'+"`n",(New-Object Text.UTF8Encoding($false)))
    & $git.Source -C $submoduleSeed add README.txt
    & $git.Source -C $submoduleSeed commit -q -m 'isolated submodule fixture'
    if ($LASTEXITCODE -ne 0) { throw 'Could not create isolated fixture submodule commit.' }
    & $git.Source -C $repo -c protocol.file.allow=always submodule add $submoduleSeed 'deps\fixture'
    if ($LASTEXITCODE -ne 0) { throw 'Could not register isolated fixture submodule.' }
    & $git.Source -C $repo -c protocol.file.allow=always submodule add $submoduleSeed 'deps\fixture-two'
    if ($LASTEXITCODE -ne 0) { throw 'Could not register second isolated fixture submodule.' }
    & $git.Source -C $repo add .
    & $git.Source -C $repo commit -q -m 'isolated OFF receipt fixture'
    if ($LASTEXITCODE -ne 0) { throw 'Could not commit isolated miniature Git repository.' }

    $generatorArgs=@('-G','Visual Studio 18 2026','-A','x64')
    $configureOutput=& $cmake.Source -S $source -B $build @generatorArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { Write-Output ("SKIP: CMake could not configure the isolated fixture with Visual Studio 18 2026 -A x64: {0}" -f $configureOutput.Trim()); exit 0 }
    $sourceReceipt=Join-Path $audit 'preflight.json'
    $sourceArgs=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $prep 'source-preflight-v43.ps1'),'-RepoRoot',$repo,'-AuditRoot',$audit,'-RunId','pre','-OutputPath',$sourceReceipt)
    & (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') @sourceArgs | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Isolated fixture could not obtain its pinned source-preflight.' }
    $pre=Get-Content -LiteralPath $sourceReceipt -Raw | ConvertFrom-Json
    $preSha=(Get-FileHash -LiteralPath $sourceReceipt -Algorithm SHA256).Hash.ToLowerInvariant()
    $exe=Join-Path $build 'Release\fea.exe'; $map=Join-Path $build 'fea\Release\fea-link.map'; $cache=Join-Path $build 'CMakeCache.txt'
    foreach ($rowCount in @(0,1,7)) {
        $typedDocument=(($pre | ConvertTo-Json -Depth 20) | ConvertFrom-Json)
        $moduleRow=$pre.submoduleCommits[0]; $fileRow=$pre.sourceFiles[0]
        $moduleRows=New-Object 'System.Collections.Generic.List[object]'; $fileRows=New-Object 'System.Collections.Generic.List[object]'
        for($i=0;$i -lt $rowCount;$i++){[void]$moduleRows.Add($moduleRow);[void]$fileRows.Add($fileRow)}
        $typedDocument.submoduleCommits=[object[]]$moduleRows.ToArray()
        $typedDocument.sourceFiles=[object[]]$fileRows.ToArray()
        $typedDocument.sourceFileCount=$rowCount
        $requiredModules=Get-RequiredProperty $typedDocument 'submoduleCommits' 'array-shape fixture'
        $requiredFiles=Get-RequiredProperty $typedDocument 'sourceFiles' 'array-shape fixture'
        if($requiredModules -isnot [array] -or $requiredModules.Count -ne $rowCount -or $requiredFiles -isnot [array] -or $requiredFiles.Count -ne $rowCount){throw "Required-property lookup unwrapped a JSON array with $rowCount row(s)."}
        Assert-SourcePreflight $typedDocument $repo
    }
    foreach ($postCase in @('empty-source-hash-array','singleton-root-commit-array')) {
        $malformedPost=(($pre | ConvertTo-Json -Depth 20) | ConvertFrom-Json)
        if($postCase -ceq 'empty-source-hash-array'){$malformedPost.sourceTreeSha256=@()}else{$malformedPost.rootCommit=@([string]$pre.rootCommit)}
        $postAccepted=$false
        try { Assert-SourcePreflight $malformedPost $repo; $postAccepted=$true } catch { }
        if($postAccepted){throw "Malformed post-build preflight '$postCase' passed strict scalar validation."}
    }

    $receipt=Join-Path $audit 'success.json'; $log=Join-Path $audit 'success.log'
    $run=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $prep 'release-build-v43.ps1'),'-PreBuildSourcePreflightPath',$sourceReceipt,'-PreBuildSourcePreflightSha256',$preSha,'-BuildDirectory',$build,'-CMakeCachePath',$cache,'-ExecutablePath',$exe,'-MapPath',$map,'-OutputPath',$receipt,'-BuildLogPath',$log,'-RepoRoot',$repo,'-AuditRoot',$audit,'-RunId','success')
    $moduleArgs=@('-PreBuildSourcePreflightPath',$sourceReceipt,'-PreBuildSourcePreflightSha256','bad-pin','-BuildDirectory',$build,'-CMakeCachePath',$cache,'-ExecutablePath',$exe,'-MapPath',$map,'-OutputPath',(Join-Path $audit 'module-boundary-receipt.json'),'-BuildLogPath',(Join-Path $audit 'module-boundary-build.log'),'-RepoRoot',$repo,'-AuditRoot',$audit,'-RunId','module-boundary')
    $producerPath=Join-Path $prep 'release-build-v43.ps1'
    $systemModules=Join-Path $PSHOME 'Modules'
    $preload=Invoke-ModuleBoundaryCase $moduleArgs 'preloaded-shadow' 'preloaded-join-path' $systemModules $producerPath
    if ($preload.exitCode -eq 0 -or $preload.stderr -notmatch 'same-name PowerShell alias/function was preloaded' -or [IO.File]::Exists($preload.marker)) { throw ("Preloaded Join-Path shadow was not rejected before invocation: exit={0}; stderr={1}; stdout={2}; marker={3}" -f $preload.exitCode,$preload.stderr,$preload.stdout,[IO.File]::Exists($preload.marker)) }
    $fakeModules=Join-Path $audit 'fake-modules';$fakeManagement=Join-Path $fakeModules 'Microsoft.PowerShell.Management'
    [void][IO.Directory]::CreateDirectory($fakeManagement)
    $fakeGuid=[Guid]::NewGuid().ToString()
    $fakeManifest="@{ RootModule='Microsoft.PowerShell.Management.psm1'; ModuleVersion='1.0.0'; GUID='$fakeGuid'; FunctionsToExport=@('Join-Path') }`n"
    $fakeModule="[IO.File]::WriteAllText(`$env:V43_TEST_MARKER,'loaded')`nfunction Join-Path { [IO.File]::WriteAllText(`$env:V43_TEST_MARKER,'invoked'); return 'SHADOWED' }`nExport-ModuleMember -Function Join-Path`n"
    [IO.File]::WriteAllText((Join-Path $fakeManagement 'Microsoft.PowerShell.Management.psd1'),$fakeManifest,(New-Object Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText((Join-Path $fakeManagement 'Microsoft.PowerShell.Management.psm1'),$fakeModule,(New-Object Text.UTF8Encoding($false)))
    $fakeFirstModulePath=$fakeModules+';'+$systemModules
    $fakeFirst=Invoke-ModuleBoundaryCase $moduleArgs 'fake-modulepath' 'fake-modulepath' $fakeFirstModulePath $producerPath
    if ($fakeFirst.exitCode -eq 0 -or $fakeFirst.stderr -notmatch 'Pre-build source-preflight SHA256 pin is malformed' -or [IO.File]::Exists($fakeFirst.marker)) { throw 'Fake-first PSModulePath was loaded or did not reach the expected trusted validation failure.' }
    foreach ($caseName in @('schema-single-valid','status-single-valid','go-single-false','action-single-false','go-numeric','action-numeric')) {
        $malformed=(($pre | ConvertTo-Json -Depth 20) | ConvertFrom-Json)
        switch ($caseName) {
            'schema-single-valid' { $malformed.schema=@('range.pc11-v43-source-preflight/v1') }
            'status-single-valid' { $malformed.status=@('PASS_SOURCE_PREFLIGHT') }
            'go-single-false' { $malformed.goPermitted=@($false) }
            'action-single-false' { $malformed.productActions.guiLaunched=@($false) }
            'go-numeric' { $malformed.goPermitted=0 }
            'action-numeric' { $malformed.productActions.guiLaunched=0 }
        }
        $malformedPath=Join-Path $audit ($caseName+'.json')
        $malformedBytes=(New-Object Text.UTF8Encoding($false)).GetBytes(($malformed | ConvertTo-Json -Depth 20)+"`n")
        [IO.File]::WriteAllBytes($malformedPath,$malformedBytes)
        $malformedSha=Get-TestBytesSha256 $malformedBytes
        $malformedOutput=Join-Path $audit ($caseName+'-receipt.json'); $malformedLog=Join-Path $audit ($caseName+'.log')
        $malformedRun=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $prep 'release-build-v43.ps1'),'-PreBuildSourcePreflightPath',$malformedPath,'-PreBuildSourcePreflightSha256',$malformedSha,'-BuildDirectory',$build,'-CMakeCachePath',$cache,'-ExecutablePath',$exe,'-MapPath',$map,'-OutputPath',$malformedOutput,'-BuildLogPath',$malformedLog,'-RepoRoot',$repo,'-AuditRoot',$audit,'-RunId',$caseName)
        $malformedExit=Invoke-ProducerChild $malformedRun ($caseName+'-child')
        if ($malformedExit -eq 0 -or [IO.File]::Exists($malformedOutput) -or [IO.File]::Exists($malformedLog)) { throw "Malformed preflight case '$caseName' reached build or emitted a receipt." }
    }
    $producerExit=Invoke-ProducerChild $run 'success-child'
    if ($producerExit -ne 0) { throw 'Successful isolated synthetic build did not produce a PASS receipt.' }
    $built=Get-Content -LiteralPath $receipt -Raw | ConvertFrom-Json
    if ($built.schema -cne 'range.pc11-v43-release-build-receipt/v1' -or $built.status -cne 'PASS' -or $built.buildExitCode -ne 0 -or $built.cmakeGenerator -cne 'Visual Studio 18 2026' -or $built.cmakeGeneratorPlatform -cne 'x64' -or $built.rangeEnableAgent -cne 'OFF' -or $built.goPermitted -ne $false -or $built.productActions.guiLaunched -ne $false) { throw 'Success receipt identity or no-GO/configuration contract is invalid.' }
    $replayExit=Invoke-ProducerChild $run 'replay-child'
    if ($replayExit -eq 0) { throw 'Single-use receipt/log replay unexpectedly succeeded.' }

    $failedBuild=Join-Path $repo 'build\off-release-fail'
    $failConfigure=& $cmake.Source -S $source -B $failedBuild @generatorArgs '-DFIXTURE_FAIL=ON' 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw ("Could not configure failing fixture: {0}" -f $failConfigure.Trim()) }
    $failedReceipt=Join-Path $audit 'failed.json'; $failedLog=Join-Path $audit 'failed.log'
    $failedRun=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $prep 'release-build-v43.ps1'),'-PreBuildSourcePreflightPath',$sourceReceipt,'-PreBuildSourcePreflightSha256',$preSha,'-BuildDirectory',$failedBuild,'-CMakeCachePath',(Join-Path $failedBuild 'CMakeCache.txt'),'-ExecutablePath',(Join-Path $failedBuild 'Release\fea.exe'),'-MapPath',(Join-Path $failedBuild 'fea\Release\fea-link.map'),'-OutputPath',$failedReceipt,'-BuildLogPath',$failedLog,'-RepoRoot',$repo,'-AuditRoot',$audit,'-RunId','failed')
    $failedExit=Invoke-ProducerChild $failedRun 'failed-child'
    if ($failedExit -eq 0 -or [IO.File]::Exists($failedReceipt) -or -not [IO.File]::Exists($failedLog)) { throw 'Failed build did not fail closed with a captured log and no receipt.' }

    $badCache=Join-Path $repo 'build\off-release-bad-cache'
    $badConfigure=& $cmake.Source -S $source -B $badCache @generatorArgs '-DRANGE_ENABLE_AGENT=ON' 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw ("Could not configure bad-cache fixture: {0}" -f $badConfigure.Trim()) }
    $badCacheReceipt=Join-Path $audit 'bad-cache.json'; $badCacheLog=Join-Path $audit 'bad-cache.log'
    $badCacheRun=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $prep 'release-build-v43.ps1'),'-PreBuildSourcePreflightPath',$sourceReceipt,'-PreBuildSourcePreflightSha256',$preSha,'-BuildDirectory',$badCache,'-CMakeCachePath',(Join-Path $badCache 'CMakeCache.txt'),'-ExecutablePath',(Join-Path $badCache 'Release\fea.exe'),'-MapPath',(Join-Path $badCache 'fea\Release\fea-link.map'),'-OutputPath',$badCacheReceipt,'-BuildLogPath',$badCacheLog,'-RepoRoot',$repo,'-AuditRoot',$audit,'-RunId','badcache')
    $badCacheExit=Invoke-ProducerChild $badCacheRun 'bad-cache-child'
    if ($badCacheExit -eq 0 -or [IO.File]::Exists($badCacheReceipt) -or [IO.File]::Exists($badCacheLog)) { throw 'RANGE_ENABLE_AGENT=ON cache unexpectedly reached the build or emitted a receipt.' }

    [IO.File]::AppendAllText((Join-Path $source 'write_release.cmake'),"`n# deliberate pinned-source mutation`n")
    $tamperedReceipt=Join-Path $audit 'tampered.json'; $tamperedLog=Join-Path $audit 'tampered.log'
    $tamperedRun=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $prep 'release-build-v43.ps1'),'-PreBuildSourcePreflightPath',$sourceReceipt,'-PreBuildSourcePreflightSha256',$preSha,'-BuildDirectory',$build,'-CMakeCachePath',$cache,'-ExecutablePath',$exe,'-MapPath',$map,'-OutputPath',$tamperedReceipt,'-BuildLogPath',$tamperedLog,'-RepoRoot',$repo,'-AuditRoot',$audit,'-RunId','tampered')
    $tamperedExit=Invoke-ProducerChild $tamperedRun 'tampered-child'
    if ($tamperedExit -eq 0 -or [IO.File]::Exists($tamperedReceipt) -or -not [IO.File]::Exists($tamperedLog)) { throw 'Changed included source unexpectedly produced a receipt.' }
    Write-Output 'PASS_BUILD_OFF_V43_SYNTHETIC success=1 failedBuildNoReceipt=1 badOffConfigNoBuild=1 sourceTamperNoReceipt=1 singleUse=1 sameByteSwap=1 typedPreflightRejects=6 arrayShapeRows=0,1,7 postArrayRejects=2'
} finally {
    # Keep isolated fixture evidence under the checkout audit path for inspection.
}
