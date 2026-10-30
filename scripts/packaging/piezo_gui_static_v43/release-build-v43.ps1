[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PreBuildSourcePreflightPath,
    [Parameter(Mandatory = $true)][string]$PreBuildSourcePreflightSha256,
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$CMakeCachePath,
    [Parameter(Mandatory = $true)][string]$ExecutablePath,
    [Parameter(Mandatory = $true)][string]$MapPath,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [Parameter(Mandatory = $true)][string]$BuildLogPath,
    [string]$RepoRoot,
    [string]$AuditRoot,
    [string]$RunId
)

$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) { throw 'This producer requires Windows PowerShell 5.1.' }
$producerPackageRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$producerToolsRoot = [IO.Path]::GetFullPath([IO.Path]::Combine($producerPackageRoot, 'tools'))
$moduleBoundaryHelperPath = [IO.Path]::GetFullPath([IO.Path]::Combine($producerToolsRoot, 'system-module-boundary-v43.ps1'))
$producerPackagePrefix = $producerPackageRoot.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if (-not $moduleBoundaryHelperPath.StartsWith($producerPackagePrefix, [StringComparison]::OrdinalIgnoreCase) -or
    -not [IO.File]::Exists($moduleBoundaryHelperPath)) { throw 'The canonical package-local v43 system-module helper is missing or outside its package.' }
foreach ($producerPath in @($producerPackageRoot, $producerToolsRoot, $moduleBoundaryHelperPath)) {
    if (([IO.File]::GetAttributes($producerPath) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "A v43 module-boundary path is a reparse point: $producerPath" }
}
$global:PSModuleAutoLoadingPreference = 'None'
$preloadedBoundaryCommands = @(& Microsoft.PowerShell.Core\Get-Command -Name 'Assert-V43SystemModuleBoundary' -All -ListImported -ErrorAction SilentlyContinue)
if ($preloadedBoundaryCommands.Count -gt 0) { throw 'A preloaded Assert-V43SystemModuleBoundary command cannot bypass the trusted v43 helper.' }
. $moduleBoundaryHelperPath
$trustedBoundaryCommand = $null
foreach ($boundaryCommand in @(& Microsoft.PowerShell.Core\Get-Command -Name 'Assert-V43SystemModuleBoundary' -All -ListImported -ErrorAction SilentlyContinue)) {
    if ($boundaryCommand.CommandType -eq 'Function' -and
        [IO.Path]::GetFullPath([string]$boundaryCommand.ScriptBlock.Ast.Extent.File) -ceq $moduleBoundaryHelperPath) {
        $trustedBoundaryCommand = $boundaryCommand
        break
    }
}
if ($null -eq $trustedBoundaryCommand) { throw 'Assert-V43SystemModuleBoundary was not loaded from the canonical package-local helper.' }
$moduleBoundaryResult = & $trustedBoundaryCommand.ScriptBlock
if ($moduleBoundaryResult.status -cne 'PASS') { throw 'The v43 system-module boundary helper did not return PASS.' }
Set-StrictMode -Version Latest
if (-not $RepoRoot) { $RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..')) }
if (-not $AuditRoot) { $AuditRoot = Join-Path $RepoRoot 'build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit' }
if (-not $RunId) { $RunId = 'off-release-build-' + [Guid]::NewGuid().ToString('N') }
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot).TrimEnd('\') + '\'
$PreBuildSourcePreflightPath = [IO.Path]::GetFullPath($PreBuildSourcePreflightPath)
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$CMakeCachePath = [IO.Path]::GetFullPath($CMakeCachePath)
$ExecutablePath = [IO.Path]::GetFullPath($ExecutablePath)
$MapPath = [IO.Path]::GetFullPath($MapPath)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$BuildLogPath = [IO.Path]::GetFullPath($BuildLogPath)
if ($RunId -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$') { throw 'RunId is malformed.' }
if (-not $OutputPath.StartsWith($AuditRoot, [StringComparison]::OrdinalIgnoreCase) -or -not $BuildLogPath.StartsWith($AuditRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Receipt and build log must be under the external audit root.' }
if ($OutputPath -ceq $BuildLogPath -or [IO.File]::Exists($OutputPath) -or [IO.File]::Exists($BuildLogPath)) { throw 'Receipt and log paths are single-use and must be distinct.' }

function Assert-NoReparse([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetPathRoot($full)
    $cursor = $root
    foreach ($part in $full.Substring($root.Length).Split('\')) {
        if (-not $part) { continue }
        $cursor = Join-Path $cursor $part
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Reparse point in protected path: $cursor" }
        }
    }
}
function Get-Sha256Bytes([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Get-DocumentSnapshot([byte[]]$Bytes, [string]$Label) {
    $sha256 = Get-Sha256Bytes $Bytes
    try { $document = ((New-Object Text.UTF8Encoding($false, $true)).GetString($Bytes) | ConvertFrom-Json -ErrorAction Stop) }
    catch { throw "$Label is not valid UTF-8 JSON: $($_.Exception.Message)" }
    return [pscustomobject]@{ document=$document; sha256=$sha256 }
}
function Get-Hash([string]$Path) {
    return Get-Sha256Bytes ([IO.File]::ReadAllBytes($Path))
}
function Read-Json([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    return (Get-DocumentSnapshot $bytes $Path).document
}
function Assert-JsonObject($Value, [string]$Label) {
    if ($null -eq $Value -or $Value -isnot [pscustomobject]) { throw "$Label must be a JSON object." }
}
function Get-RequiredProperty($Object, [string]$Name, [string]$Label) {
    if ($null -eq $Object -or $Object.PSObject.Properties.Name -notcontains $Name) { throw "$Label is missing required field '$Name'." }
    $value = $Object.PSObject.Properties[$Name].Value
    if ($value -is [array]) { return ,$value }
    return $value
}
function Assert-StringField($Object, [string]$Name, [string]$Label) {
    $value = Get-RequiredProperty $Object $Name $Label
    if ($value -isnot [string] -or [string]::IsNullOrWhiteSpace($value)) { throw "$Label field '$Name' must be a nonempty JSON string." }
    return [string]$value
}
function Assert-ShaField($Object, [string]$Name, [string]$Label) {
    $value = Assert-StringField $Object $Name $Label
    if ($value -notmatch '^[0-9a-fA-F]{64}$') { throw "$Label field '$Name' must be a SHA256 string." }
}
function Assert-JsonInteger($Value, [string]$Label) {
    if ($Value -isnot [byte] -and $Value -isnot [int16] -and $Value -isnot [int32] -and $Value -isnot [int64]) { throw "$Label must be a JSON integer." }
    if ([long]$Value -lt 0) { throw "$Label must be nonnegative." }
}
function Assert-SourcePreflight($Document, [string]$ExpectedRoot) {
    Assert-JsonObject $Document 'Pre-build source-preflight'
    if ((Assert-StringField $Document 'schema' 'Pre-build source-preflight') -cne 'range.pc11-v43-source-preflight/v1' -or
        (Assert-StringField $Document 'status' 'Pre-build source-preflight') -cne 'PASS_SOURCE_PREFLIGHT') { throw 'Pre-build source-preflight schema or status is invalid.' }
    if ([IO.Path]::GetFullPath((Assert-StringField $Document 'sourceRootPath' 'Pre-build source-preflight')) -cne $ExpectedRoot) { throw 'Pre-build source-preflight source root differs from RepoRoot.' }
    $null = Assert-StringField $Document 'rootCommit' 'Pre-build source-preflight'
    if ([string]$Document.rootCommit -notmatch '^(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})$') { throw 'Pre-build rootCommit must be a Git object ID string.' }
    if ((Assert-StringField $Document 'rootWorktreeStatus' 'Pre-build source-preflight') -notin @('CLEAN','DIRTY')) { throw 'Pre-build root worktree status is invalid.' }
    Assert-ShaField $Document 'rootWorktreeStatusSha256' 'Pre-build source-preflight'
    Assert-ShaField $Document 'sourceTreeSha256' 'Pre-build source-preflight'
    $null = Assert-StringField $Document 'requiredCTestManifestPath' 'Pre-build source-preflight'
    Assert-ShaField $Document 'requiredCTestManifestSha256' 'Pre-build source-preflight'
    if ((Assert-StringField $Document 'sourceScopeSchema' 'Pre-build source-preflight') -cne 'range.pc11-v43-source-scope/v1') { throw 'Pre-build source scope schema is invalid.' }
    Assert-JsonObject (Get-RequiredProperty $Document 'sourceScope' 'Pre-build source-preflight') 'Pre-build source scope'
    $files = Get-RequiredProperty $Document 'sourceFiles' 'Pre-build source-preflight'
    $submodules = Get-RequiredProperty $Document 'submoduleCommits' 'Pre-build source-preflight'
    if ($files -isnot [array] -or $submodules -isnot [array]) { throw 'Pre-build sourceFiles and submoduleCommits must be JSON arrays.' }
    Assert-JsonInteger (Get-RequiredProperty $Document 'sourceFileCount' 'Pre-build source-preflight') 'Pre-build sourceFileCount'
    Assert-JsonInteger (Get-RequiredProperty $Document 'sourceTotalBytes' 'Pre-build source-preflight') 'Pre-build sourceTotalBytes'
    if ([long]$Document.sourceFileCount -ne $files.Count) { throw 'Pre-build sourceFileCount differs from sourceFiles array length.' }
    foreach ($row in $files) {
        Assert-JsonObject $row 'Pre-build source file row'
        $null = Assert-StringField $row 'path' 'Pre-build source file row'
        Assert-ShaField $row 'sha256' 'Pre-build source file row'
        Assert-JsonInteger (Get-RequiredProperty $row 'sizeBytes' 'Pre-build source file row') 'Pre-build source file sizeBytes'
    }
    foreach ($row in $submodules) {
        Assert-JsonObject $row 'Pre-build submodule row'
        foreach ($field in @('path','registeredCommit','headCommit','worktreeStatus')) { $null = Assert-StringField $row $field 'Pre-build submodule row' }
        foreach ($field in @('sourceTreeSha256','worktreeStatusSha256')) { Assert-ShaField $row $field 'Pre-build submodule row' }
    }
    $go = Get-RequiredProperty $Document 'goPermitted' 'Pre-build source-preflight'
    if ($go -isnot [bool] -or $go -ne $false) { throw 'Pre-build goPermitted must be the JSON boolean false.' }
    $actions = Get-RequiredProperty $Document 'productActions' 'Pre-build source-preflight'
    Assert-JsonObject $actions 'Pre-build productActions'
    foreach ($name in @('guiLaunched','savePerformed','runPerformed','confirmPerformed')) {
        $value = Get-RequiredProperty $actions $name 'Pre-build productActions'
        if ($value -isnot [bool] -or $value -ne $false) { throw "Pre-build productActions.$name must be the JSON boolean false." }
    }
}
function Get-CMakeCacheValue([string]$Text, [string]$Name) {
    $matches = @([regex]::Matches($Text, '(?m)^' + [regex]::Escape($Name) + ':[^=\r\n]*=(.*)$'))
    if ($matches.Count -ne 1) { throw "CMakeCache must contain exactly one $Name entry." }
    return [string]$matches[0].Groups[1].Value.Trim()
}
function Quote-Arg([string]$Value) {
    if ($Value -notmatch '[\s"]') { return $Value }
    return '"' + ($Value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}
function Invoke-Preflight([string]$Path) {
    $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $script = Join-Path $PSScriptRoot 'source-preflight-v43.ps1'
    $args = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$script,'-RepoRoot',$RepoRoot,'-AuditRoot',$AuditRoot.TrimEnd('\'),'-RunId',('postbuild-'+$RunId),'-OutputPath',$Path)
    $quoted = @($args | ForEach-Object { Quote-Arg ([string]$_) })
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $powershell; $psi.Arguments = [string]::Join(' ', $quoted); $psi.WorkingDirectory = $RepoRoot
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $p = New-Object Diagnostics.Process; $p.StartInfo = $psi
    try { if (-not $p.Start()) { throw 'Could not start post-build source-preflight.' }; $errTask = $p.StandardError.ReadToEndAsync(); $out = $p.StandardOutput.ReadToEnd(); $p.WaitForExit(); $err = $errTask.Result; if ($p.ExitCode -ne 0) { throw "Post-build source-preflight failed: $out $err" } }
    finally { $p.Dispose() }
    $bytes = [IO.File]::ReadAllBytes($Path)
    return Get-DocumentSnapshot $bytes $Path
}

foreach ($path in @($RepoRoot,$BuildDirectory,$CMakeCachePath,$ExecutablePath,$MapPath,$AuditRoot.TrimEnd('\'))) { Assert-NoReparse $path }
foreach ($path in @($OutputPath,$BuildLogPath)) { Assert-NoReparse $path }
if (-not [IO.Directory]::Exists($BuildDirectory) -or -not [IO.File]::Exists($CMakeCachePath)) { throw 'Pinned build directory or CMakeCache is missing.' }
if ([IO.Path]::GetDirectoryName($CMakeCachePath) -cne $BuildDirectory) { throw 'CMakeCache must be directly inside the pinned build directory.' }
$repoBuildPrefix = $RepoRoot.TrimEnd('\') + '\build\'
if (-not $BuildDirectory.StartsWith($repoBuildPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'OFF build directory must remain under the pinned repository build directory.' }
$cacheBytes = [IO.File]::ReadAllBytes($CMakeCachePath)
$cacheSha = Get-Sha256Bytes $cacheBytes
$cacheText = (New-Object Text.UTF8Encoding($false, $true)).GetString($cacheBytes)
$cacheSource = [IO.Path]::GetFullPath((Get-CMakeCacheValue $cacheText 'CMAKE_HOME_DIRECTORY'))
$expectedSource = [IO.Path]::GetFullPath((Join-Path $RepoRoot 'src'))
if ($cacheSource -cne $expectedSource) { throw 'CMakeCache source directory must be the pinned repository src directory.' }
if ((Get-CMakeCacheValue $cacheText 'RANGE_ENABLE_AGENT') -cne 'OFF') { throw 'CMakeCache must configure RANGE_ENABLE_AGENT=OFF.' }
if ((Get-CMakeCacheValue $cacheText 'CMAKE_GENERATOR') -cne 'Visual Studio 18 2026') { throw 'CMakeCache generator must be Visual Studio 18 2026.' }
if ((Get-CMakeCacheValue $cacheText 'CMAKE_GENERATOR_PLATFORM') -cne 'x64') { throw 'CMakeCache generator platform must be x64.' }
if ((Get-CMakeCacheValue $cacheText 'CMAKE_PROJECT_NAME') -cne 'RangeFea') { throw 'CMakeCache project name must be RangeFea.' }
$configurationTypes = (Get-CMakeCacheValue $cacheText 'CMAKE_CONFIGURATION_TYPES').Split(';')
if ($configurationTypes -cnotcontains 'Release') { throw 'CMakeCache must include the Release configuration.' }
if ($ExecutablePath -cne [IO.Path]::GetFullPath((Join-Path $BuildDirectory 'Release\fea.exe'))) { throw 'ExecutablePath must be the configured Release fea.exe output.' }
if ($MapPath -cne [IO.Path]::GetFullPath((Join-Path $BuildDirectory 'fea\Release\fea-link.map'))) { throw 'MapPath must be the configured Release fea-link.map output.' }
$cmakeCommands = @(Get-Command cmake.exe -CommandType Application -All -ErrorAction Stop)
if ($cmakeCommands.Count -ne 1) { throw 'cmake.exe is shadowed or resolves ambiguously on PATH.' }
$cmake = [IO.Path]::GetFullPath($cmakeCommands[0].Source)
Assert-NoReparse $cmake
if (-not [IO.File]::Exists($PreBuildSourcePreflightPath)) { throw 'Pinned pre-build source-preflight is missing.' }
if ($PreBuildSourcePreflightSha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'Pre-build source-preflight SHA256 pin is malformed.' }
$preBuildBytes = [IO.File]::ReadAllBytes($PreBuildSourcePreflightPath)
$preBuildSnapshot = Get-DocumentSnapshot $preBuildBytes $PreBuildSourcePreflightPath
if ($preBuildSnapshot.sha256 -cne $PreBuildSourcePreflightSha256.ToLowerInvariant()) { throw 'Pre-build source-preflight bytes differ from the external SHA256 pin.' }
$before = $preBuildSnapshot.document
Assert-SourcePreflight $before $RepoRoot
$manifest = [IO.Path]::GetFullPath([string]$before.requiredCTestManifestPath)
foreach ($p in @($CMakeCachePath,$manifest)) { Assert-NoReparse $p }
$manifestBytes = [IO.File]::ReadAllBytes($manifest)
$manifestSha = Get-Sha256Bytes $manifestBytes
if ($manifestSha -cne [string]$before.requiredCTestManifestSha256) { throw 'Required CTest manifest bytes differ from the pinned source-preflight snapshot.' }
$parent = [IO.Path]::GetDirectoryName($OutputPath)
$logParent = [IO.Path]::GetDirectoryName($BuildLogPath)
foreach ($directory in @($parent,$logParent)) {
    if (-not [IO.Directory]::Exists($directory)) { [void][IO.Directory]::CreateDirectory($directory) }
    Assert-NoReparse $directory
}

$failure = $null; $exitCode = $null; $logSha = $null; $receipt = $null
try {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $cmake; $psi.Arguments = '--build ' + (Quote-Arg $BuildDirectory) + ' --config Release'; $psi.WorkingDirectory = $RepoRoot
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $p = New-Object Diagnostics.Process; $p.StartInfo = $psi
    try {
        if (-not $p.Start()) { throw 'Could not start pinned cmake.exe.' }
        $errTask = $p.StandardError.ReadToEndAsync(); $stdout = $p.StandardOutput.ReadToEnd(); $p.WaitForExit(); $stderr = $errTask.Result; $exitCode = $p.ExitCode
    } finally { $p.Dispose() }
    $logText = "COMMAND: `"$cmake`" --build `"$BuildDirectory`" --config Release`r`nEXIT_CODE: $exitCode`r`n--- STDOUT ---`r`n$stdout`r`n--- STDERR ---`r`n$stderr"
    $logStream = [IO.File]::Open($BuildLogPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try { $logBytes = (New-Object Text.UTF8Encoding($false)).GetBytes($logText); $logStream.Write($logBytes,0,$logBytes.Length) }
    finally { $logStream.Dispose() }
    $logSha = Get-Hash $BuildLogPath
    if ($exitCode -ne 0) { throw "cmake --build exited $exitCode; no PASS receipt is permitted." }
    if ((Get-Hash $CMakeCachePath) -cne $cacheSha -or (Get-Hash $manifest) -cne $manifestSha) { throw 'CMakeCache or required CTest manifest changed during the build.' }
    foreach ($p in @($ExecutablePath,$MapPath)) { Assert-NoReparse $p; if (-not [IO.File]::Exists($p)) { throw "Expected Release output is missing: $p" } }
    $postPath = Join-Path $parent ('post-build-source-preflight-' + $RunId + '.json')
    if ([IO.File]::Exists($postPath)) { throw 'Post-build preflight output is single-use.' }
    $postSnapshot = Invoke-Preflight $postPath
    $post = $postSnapshot.document
    Assert-SourcePreflight $post $RepoRoot
    if ($post.sourceTreeSha256 -cne $before.sourceTreeSha256 -or $post.rootCommit -cne $before.rootCommit -or
        $post.rootWorktreeStatusSha256 -cne $before.rootWorktreeStatusSha256 -or
        (ConvertTo-Json -InputObject @($post.submoduleCommits) -Depth 10 -Compress) -cne (ConvertTo-Json -InputObject @($before.submoduleCommits) -Depth 10 -Compress) -or
        $post.requiredCTestManifestSha256 -cne $manifestSha) { throw 'Source, root/submodule pins, or manifest changed across the build.' }
    $receipt = [ordered]@{
        schema='range.pc11-v43-release-build-receipt/v1'; status='PASS'; candidateVersion='v43'; variant='OFF'; configuration='Release'
        generatedAtUtc=[DateTime]::UtcNow.ToString('o'); sourceRootPath=$RepoRoot; rootCommit=[string]$before.rootCommit
        rootWorktreeStatus=[string]$before.rootWorktreeStatus; rootWorktreeStatusSha256=[string]$before.rootWorktreeStatusSha256
        submoduleCommits=@($before.submoduleCommits); sourceTreeSha256=[string]$before.sourceTreeSha256
        preBuildSourcePreflightReceiptPath=$PreBuildSourcePreflightPath; preBuildSourcePreflightReceiptSha256=$PreBuildSourcePreflightSha256.ToLowerInvariant()
        postBuildSourcePreflightReceiptPath=$postPath; postBuildSourcePreflightReceiptSha256=$postSnapshot.sha256
        requiredCTestManifestPath=$manifest; requiredCTestManifestSha256=$manifestSha
        cmakeExecutablePath=$cmake; cmakeGenerator=(Get-CMakeCacheValue $cacheText 'CMAKE_GENERATOR'); cmakeGeneratorPlatform=(Get-CMakeCacheValue $cacheText 'CMAKE_GENERATOR_PLATFORM')
        cmakeSourceDirectory=$cacheSource; cmakeProjectName=(Get-CMakeCacheValue $cacheText 'CMAKE_PROJECT_NAME'); rangeEnableAgent='OFF'
        buildDirectory=$BuildDirectory; cmakeCachePath=$CMakeCachePath; cmakeCacheSha256=$cacheSha
        buildCommand='cmake --build <buildDirectory> --config Release'; buildExitCode=[int]$exitCode
        buildLogPath=$BuildLogPath; buildLogSha256=$logSha; executablePath=$ExecutablePath; executableSha256=(Get-Hash $ExecutablePath)
        mapPath=$MapPath; mapSha256=(Get-Hash $MapPath); staticOnly=$true
        productActions=[ordered]@{guiLaunched=$false;savePerformed=$false;runPerformed=$false;confirmPerformed=$false}; goPermitted=$false
    }
} catch { $failure = $_.Exception.Message }

if ($null -eq $receipt) { throw $failure }
$receiptStream = [IO.File]::Open($OutputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
try { $receiptBytes = (New-Object Text.UTF8Encoding($false)).GetBytes(($receipt | ConvertTo-Json -Depth 20) + "`n"); $receiptStream.Write($receiptBytes,0,$receiptBytes.Length) }
finally { $receiptStream.Dispose() }
Write-Output ("{0} buildExitCode={1} output={2}" -f $receipt.status, $exitCode, $OutputPath)
if ($receipt.status -cne 'PASS') { exit 1 }
