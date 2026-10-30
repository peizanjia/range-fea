[CmdletBinding()]
param(
    [string]$RepoRoot,
    [string]$RequiredCTestManifestPath,
    [string]$AuditRoot,
    [string]$RunId,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'This source-preflight requires Windows PowerShell 5.1.'
}
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
if (-not $PSBoundParameters.ContainsKey('RunId')) { $RunId = 'source-preflight-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8) }
if (-not $RepoRoot) { $RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..')) }
if (-not $AuditRoot) { $AuditRoot = Join-Path $RepoRoot 'build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit' }

function Get-Sha256Bytes([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-Sha256File([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}

function ConvertTo-JsonString([string]$Value) {
    return [string]($Value | ConvertTo-Json -Compress -ErrorAction Stop)
}

function Compare-Utf8Text([string]$Left, [string]$Right) {
    $leftBytes = [Text.Encoding]::UTF8.GetBytes($Left)
    $rightBytes = [Text.Encoding]::UTF8.GetBytes($Right)
    $limit = [Math]::Min($leftBytes.Length, $rightBytes.Length)
    for ($i = 0; $i -lt $limit; $i++) {
        if ($leftBytes[$i] -lt $rightBytes[$i]) { return -1 }
        if ($leftBytes[$i] -gt $rightBytes[$i]) { return 1 }
    }
    if ($leftBytes.Length -lt $rightBytes.Length) { return -1 }
    if ($leftBytes.Length -gt $rightBytes.Length) { return 1 }
    return 0
}

function Get-CanonicalTreeSha256($Rows) {
    $parts = New-Object 'System.Collections.Generic.List[string]'
    foreach ($row in $Rows) {
        [void]$parts.Add('{"path":' + (ConvertTo-JsonString ([string]$row.path)) +
            ',"sha256":' + (ConvertTo-JsonString ([string]$row.sha256)) +
            ',"sizeBytes":' + ([string]([long]$row.sizeBytes)) + '}')
    }
    $json = '[' + [string]::Join(',', $parts.ToArray()) + ']'
    return Get-Sha256Bytes ((New-Object Text.UTF8Encoding($false, $true)).GetBytes($json))
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

function Invoke-GitBytes([string]$WorkingDirectory, [string[]]$Arguments) {
    $processInfo = New-Object Diagnostics.ProcessStartInfo
    $processInfo.FileName = $script:GitExecutable
    $processInfo.WorkingDirectory = $WorkingDirectory
    $quoted = New-Object 'System.Collections.Generic.List[string]'
    foreach ($argument in $Arguments) { [void]$quoted.Add((Quote-WindowsArgument ([string]$argument))) }
    $processInfo.Arguments = [string]::Join(' ', $quoted.ToArray())
    $processInfo.UseShellExecute = $false
    $processInfo.CreateNoWindow = $true
    $processInfo.RedirectStandardOutput = $true
    $processInfo.RedirectStandardError = $true
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $processInfo
    try {
        if (-not $process.Start()) { throw 'Could not start the pinned Git executable.' }
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $memory = New-Object IO.MemoryStream
        try { $process.StandardOutput.BaseStream.CopyTo($memory); $process.WaitForExit(); $bytes = $memory.ToArray() }
        finally { $memory.Dispose() }
        $stderr = $stderrTask.Result
        if ($process.ExitCode -ne 0) { throw ("Git command failed (exit {0}): {1}" -f $process.ExitCode, $stderr.Trim()) }
        return ,$bytes
    }
    finally { $process.Dispose() }
}

function Convert-GitUtf8([byte[]]$Bytes) {
    return (New-Object Text.UTF8Encoding($false, $true)).GetString($Bytes)
}

function Invoke-GitText([string]$WorkingDirectory, [string[]]$Arguments) {
    return Convert-GitUtf8 (Invoke-GitBytes $WorkingDirectory $Arguments)
}

function Get-GitZeroPaths([string]$WorkingDirectory) {
    $bytes = Invoke-GitBytes $WorkingDirectory @('-c', 'core.quotepath=false', 'ls-files', '-co', '--exclude-standard', '-z')
    $text = Convert-GitUtf8 $bytes
    $paths = New-Object 'System.Collections.Generic.List[string]'
    if ($text.Length -gt 0 -and $text[$text.Length - 1] -ne [char]0) { throw 'Git -z path output is missing its terminating NUL.' }
    $tokens = $text.Split([char]0, [StringSplitOptions]::None)
    for ($tokenIndex = 0; $tokenIndex -lt $tokens.Length; $tokenIndex++) {
        $path = [string]$tokens[$tokenIndex]
        if ($path.Length -eq 0) {
            if ($tokenIndex -eq ($tokens.Length - 1) -and $text.Length -gt 0) { continue }
            throw 'Git -z path output contains an empty interior path.'
        }
        [void]$paths.Add($path.Replace('\', '/'))
    }
    return ,$paths.ToArray()
}

function Get-RepoStatusBytes([string]$WorkingDirectory) {
    return Invoke-GitBytes $WorkingDirectory @('-c', 'core.quotepath=false', 'status', '--porcelain=v1', '-z', '--untracked-files=all', '--ignore-submodules=none')
}

function Get-RepoCommit([string]$WorkingDirectory) {
    $value = (Invoke-GitText $WorkingDirectory @('rev-parse', 'HEAD')).Trim()
    if ($value -notmatch '^[0-9a-f]{40,64}$') { throw "Git returned a malformed HEAD identity for $WorkingDirectory." }
    return $value.ToLowerInvariant()
}

function Get-SubmoduleRows([string]$RepositoryRoot, [string]$RelativePrefix) {
    $output = Invoke-GitText $RepositoryRoot @('submodule', 'status', '--recursive')
    $rows = New-Object 'System.Collections.Generic.List[object]'
    foreach ($line in ($output -split "`r?`n")) {
        if ([string]::IsNullOrEmpty($line)) { continue }
        if ($line.Length -lt 43 -or $line[0] -notin @(' ', '+', '-', 'U')) { throw "Unrecognized recursive submodule status row: $line" }
        $marker = [string]$line[0]
        $commit = $line.Substring(1, 40).ToLowerInvariant()
        if ($commit -notmatch '^[0-9a-f]{40}$') { throw "Malformed submodule commit in row: $line" }
        $tail = $line.Substring(42)
        $path = $tail -replace ' \([^)]*\)$', ''
        if ([string]::IsNullOrWhiteSpace($path) -or $path -match '[\r\n\x00]') { throw "Malformed submodule path in row: $line" }
        $path = $path.Replace('\', '/').Trim('/')
        if ($path -match '(^|/)\.{1,2}(/|$)' -or [IO.Path]::IsPathRooted($path)) { throw "Unsafe submodule path: $path" }
        if ($marker -eq '-') { throw "Uninitialized submodule cannot be frozen: $path" }
        if ($marker -eq 'U') { throw "Conflicted submodule cannot be frozen: $path" }
        $fullPrefix = if ([string]::IsNullOrEmpty($RelativePrefix)) { $path } else { $RelativePrefix.TrimEnd('/') + '/' + $path }
        [void]$rows.Add([pscustomobject]@{ path = $fullPrefix; headFromStatus = $commit; marker = $marker })
    }
    return ,$rows.ToArray()
}

function Test-IsBuildInput([string]$RelativePath) {
    $path = $RelativePath.Replace('\', '/')
    if ($path.StartsWith('src/', [StringComparison]::OrdinalIgnoreCase)) { return $true }
    if ($path.StartsWith('scripts/packaging/', [StringComparison]::OrdinalIgnoreCase)) { return $true }
    if ($path.StartsWith('scripts/benchmark/', [StringComparison]::OrdinalIgnoreCase)) { return $true }
    if ($path.StartsWith('scripts/ci/', [StringComparison]::OrdinalIgnoreCase)) { return $true }
    if ($path.StartsWith('cmake/', [StringComparison]::OrdinalIgnoreCase)) { return $true }
    if ($path -match '(^|/)(CMakeLists\.txt|[^/]+\.cmake(\.in)?|CMakePresets\.json|CMakeSettings\.json|vcpkg\.json|vcpkg-configuration\.json)$') { return $true }
    if ($path -ceq '.github/workflows/windows-regression.yml') { return $true }
    if ($path -ceq 'docs/PIEZO_CONTROL_DELIVERY.md' -or $path -ceq 'docs/piezo_control_plan.json') { return $true }
    return $false
}

function Test-UnclassifiedBuildInput([string]$RelativePath) {
    $extension = [IO.Path]::GetExtension($RelativePath).ToLowerInvariant()
    return ($extension -in @('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.ui', '.qrc', '.rc', '.py', '.ps1', '.cmake', '.in'))
}

function Get-RelativePathForSource([string]$Root, [string]$Path) {
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) { throw "Source path escapes repository root: $Path" }
    return $full.Substring($rootFull.Length).Replace('\', '/')
}

function Assert-NoReparseChain([string]$Root, [string]$RelativePath) {
    $cursor = [IO.Path]::GetFullPath($Root)
    $rootItem = Get-Item -LiteralPath $cursor -Force -ErrorAction Stop
    if (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Repository root is a reparse point.' }
    foreach ($segment in $RelativePath.Split('/')) {
        $cursor = Join-Path $cursor $segment
        $item = Get-Item -LiteralPath $cursor -Force -ErrorAction Stop
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Reparse point is in source path: $RelativePath" }
    }
}

function Read-SourceFileRow([string]$RepositoryRoot, [string]$RelativePath) {
    if ($RelativePath.Length -gt 1024 -or $RelativePath -match '[\x00-\x1f\x7f]') { throw "Source path is outside the supported path contract: $RelativePath" }
    if ([IO.Path]::IsPathRooted($RelativePath) -or $RelativePath -match '(^|/)\.{1,2}(/|$)') { throw "Unsafe source path: $RelativePath" }
    Assert-NoReparseChain $RepositoryRoot $RelativePath
    $full = Join-Path $RepositoryRoot ($RelativePath.Replace('/', '\'))
    $itemBefore = Get-Item -LiteralPath $full -Force -ErrorAction Stop
    if ($itemBefore.PSIsContainer) { throw "A source file path resolves to a directory: $RelativePath" }
    if (($itemBefore.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "A source file is a reparse point: $RelativePath" }
    if ($itemBefore.Length -gt 2147483648) { throw "Source file exceeds the per-file bound: $RelativePath" }
    $sha = Get-Sha256File $full
    $itemAfter = Get-Item -LiteralPath $full -Force -ErrorAction Stop
    if ($itemAfter.PSIsContainer -or $itemBefore.Length -ne $itemAfter.Length -or $itemBefore.LastWriteTimeUtc -ne $itemAfter.LastWriteTimeUtc) {
        throw "Source file changed while it was being hashed: $RelativePath"
    }
    return [pscustomobject]@{ path = $RelativePath; sizeBytes = [long]$itemAfter.Length; sha256 = $sha }
}

function Get-SourceSnapshot([string]$Root) {
    $rootFull = [IO.Path]::GetFullPath($Root)
    $actualRoot = [IO.Path]::GetFullPath((Invoke-GitText $rootFull @('rev-parse', '--show-toplevel')).Trim())
    if ($actualRoot -cne $rootFull) { throw "RepoRoot is not the Git top-level: $actualRoot" }
    $rootCommit = Get-RepoCommit $rootFull
    $rootStatus = Get-RepoStatusBytes $rootFull
    $submoduleRows = New-Object 'System.Collections.Generic.List[object]'
    $submoduleRows.AddRange([object[]](Get-SubmoduleRows $rootFull ''))

    $repoRows = New-Object 'System.Collections.Generic.List[object]'
    [void]$repoRows.Add([pscustomobject]@{ root = $rootFull; prefix = '' })
    foreach ($submodule in $submoduleRows) {
        $full = [IO.Path]::GetFullPath((Join-Path $rootFull ($submodule.path.Replace('/', '\'))))
        if (-not [IO.Directory]::Exists($full)) { throw "Submodule working tree is missing: $($submodule.path)" }
        [void]$repoRows.Add([pscustomobject]@{ root = $full; prefix = [string]$submodule.path })
    }

    $allRows = New-Object 'System.Collections.Generic.List[object]'
    $submodulePins = New-Object 'System.Collections.Generic.List[object]'
    $pathOwners = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([StringComparer]::OrdinalIgnoreCase)
    $totalBytes = [long]0
    $submoduleParentPrefixes = @{}
    foreach ($submodule in $submoduleRows) {
        $parentPrefix = ''
        foreach ($candidate in $submoduleRows) {
            if ($submodule.path.StartsWith([string]$candidate.path + '/', [StringComparison]::OrdinalIgnoreCase) -and $candidate.path.Length -gt $parentPrefix.Length) {
                $parentPrefix = [string]$candidate.path
            }
        }
        $submoduleParentPrefixes[[string]$submodule.path] = $parentPrefix
    }
    foreach ($repo in $repoRows) {
        $nestedPrefixes = New-Object 'System.Collections.Generic.List[string]'
        foreach ($submodule in $submoduleRows) {
            if ([string]$submoduleParentPrefixes[[string]$submodule.path] -ceq [string]$repo.prefix) {
                $relativeNested = if ([string]::IsNullOrEmpty([string]$repo.prefix)) { [string]$submodule.path } else { $submodule.path.Substring(([string]$repo.prefix).Length).TrimStart('/') }
                if ($relativeNested) { [void]$nestedPrefixes.Add($relativeNested) }
            }
        }
        $relativePaths = Get-GitZeroPaths ([string]$repo.root)
        for ($relativePathIndex = 0; $relativePathIndex -lt $relativePaths.Count; $relativePathIndex++) {
            $relativePathValue = $relativePaths[$relativePathIndex]
            $relativePath = [string]$relativePathValue
            if ([string]::IsNullOrEmpty($relativePath)) { throw "Git returned an empty path for repository '$($repo.root)' (prefix '$($repo.prefix)'); index=$relativePathIndex count=$($relativePaths.Count) type=$($relativePathValue.GetType().FullName)." }
            if ($relativePath -ceq '.git' -or $relativePath.StartsWith('.git/', [StringComparison]::OrdinalIgnoreCase)) { continue }
            $isNestedGitlink = $false
            foreach ($nestedPrefix in $nestedPrefixes) {
                if ($relativePath -ceq $nestedPrefix -or $relativePath.StartsWith($nestedPrefix + '/', [StringComparison]::OrdinalIgnoreCase)) { $isNestedGitlink = $true; break }
            }
            if ($isNestedGitlink) { continue }
            $globalPath = if ([string]::IsNullOrEmpty([string]$repo.prefix)) { $relativePath } else { ([string]$repo.prefix).TrimEnd('/') + '/' + $relativePath }
            if ($globalPath.StartsWith('build/', [StringComparison]::OrdinalIgnoreCase) -or
                $globalPath -ceq '.git' -or $globalPath.StartsWith('.git/', [StringComparison]::OrdinalIgnoreCase)) { continue }
            if (-not (Test-IsBuildInput $globalPath)) {
                if (Test-UnclassifiedBuildInput $globalPath) { throw "Build-like source file is outside the frozen input scope: $globalPath" }
                continue
            }
            $existing = $null
            if ($pathOwners.TryGetValue($globalPath, [ref]$existing)) {
                if ($existing -cne $globalPath) { throw "Case-insensitive source path collision: $existing / $globalPath" }
                throw "Duplicate source path across repositories: $globalPath"
            }
            $pathOwners.Add($globalPath, $globalPath)
            $row = Read-SourceFileRow ([string]$repo.root) $relativePath
            [void]$allRows.Add([pscustomobject]@{ path = $globalPath; sizeBytes = [long]$row.sizeBytes; sha256 = [string]$row.sha256 })
            $totalBytes += [long]$row.sizeBytes
            if ($allRows.Count -gt 100000 -or $totalBytes -gt 21474836480) { throw 'Source input tree exceeds its count or total-byte bound.' }
        }
    }

    $compare = [System.Comparison[object]]{
        param($left, $right)
        return Compare-Utf8Text ([string]$left.path) ([string]$right.path)
    }
    $comparer = [System.Collections.Generic.Comparer[object]]::Create($compare)
    $allRows.Sort($comparer)
    for ($i = 1; $i -lt $allRows.Count; $i++) {
        if ((Compare-Utf8Text ([string]$allRows[$i - 1].path) ([string]$allRows[$i].path)) -eq 0) { throw 'Duplicate source path after canonical ordering.' }
    }
    $sourceTreeSha = Get-CanonicalTreeSha256 $allRows.ToArray()

    foreach ($submodule in $submoduleRows) {
        $full = [IO.Path]::GetFullPath((Join-Path $rootFull ($submodule.path.Replace('/', '\'))))
        $head = Get-RepoCommit $full
        if ($head -cne [string]$submodule.headFromStatus) { throw "Submodule HEAD changed during snapshot: $($submodule.path)" }
        $parentPrefix = ''
        foreach ($candidate in $submoduleRows) {
            if ($submodule.path.StartsWith([string]$candidate.path + '/', [StringComparison]::OrdinalIgnoreCase) -and $candidate.path.Length -gt $parentPrefix.Length) { $parentPrefix = [string]$candidate.path }
        }
        $parentRoot = if ($parentPrefix) { Join-Path $rootFull ($parentPrefix.Replace('/', '\')) } else { $rootFull }
        $relativeLink = if ($parentPrefix) { $submodule.path.Substring($parentPrefix.Length).TrimStart('/') } else { [string]$submodule.path }
        $stageOutput = Invoke-GitText $parentRoot @('ls-files', '--stage', '--', $relativeLink)
        if ($stageOutput -notmatch '(?m)^160000 (?<sha>[0-9a-f]{40}) 0\t') { throw "Submodule gitlink identity is missing from its parent index: $($submodule.path)" }
        $registered = $Matches.sha.ToLowerInvariant()
        $statusBytes = Get-RepoStatusBytes $full
        $subRows = @($allRows | Where-Object { ([string]$_.path).StartsWith([string]$submodule.path + '/', [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object {
            [pscustomobject]@{ path = ([string]$_.path).Substring(([string]$submodule.path).Length + 1); sizeBytes = [long]$_.sizeBytes; sha256 = [string]$_.sha256 }
        })
        [void]$submodulePins.Add([pscustomobject][ordered]@{
            path = [string]$submodule.path
            registeredCommit = $registered
            headCommit = $head
            worktreeStatus = if ($statusBytes.Length -eq 0) { 'CLEAN' } else { 'DIRTY' }
            worktreeStatusSha256 = Get-Sha256Bytes $statusBytes
            sourceTreeSha256 = Get-CanonicalTreeSha256 $subRows
        })
    }
    $submodulePins.Sort([System.Collections.Generic.Comparer[object]]::Create([System.Comparison[object]]{ param($left, $right) Compare-Utf8Text ([string]$left.path) ([string]$right.path) }))
    return [pscustomobject][ordered]@{
        repoRoot = $rootFull
        rootCommit = $rootCommit
        rootWorktreeStatus = if ($rootStatus.Length -eq 0) { 'CLEAN' } else { 'DIRTY' }
        rootWorktreeStatusSha256 = Get-Sha256Bytes $rootStatus
        submoduleCommits = $submodulePins.ToArray()
        sourceFiles = $allRows.ToArray()
        sourceTreeSha256 = $sourceTreeSha
        sourceFileCount = $allRows.Count
        sourceTotalBytes = $totalBytes
    }
}

function Write-JsonNoOverwrite([string]$Path, $Value) {
    if ([IO.File]::Exists($Path)) { throw "Refusing to overwrite existing audit evidence: $Path" }
    $parent = [IO.Path]::GetDirectoryName($Path)
    if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
    $json = $Value | ConvertTo-Json -Depth 20
    [IO.File]::WriteAllText($Path, $json + "`n", (New-Object Text.UTF8Encoding($false)))
}

$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)
if ($RunId -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$') { throw 'RunId must contain only bounded ASCII letters, digits, dot, underscore, or hyphen.' }
$gitCommand = Get-Command git.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1
$script:GitExecutable = [IO.Path]::GetFullPath($gitCommand.Source)
if (-not $RequiredCTestManifestPath) { $RequiredCTestManifestPath = Join-Path $RepoRoot 'scripts\ci\required-ctest-tests.json' }
$RequiredCTestManifestPath = [IO.Path]::GetFullPath($RequiredCTestManifestPath)
$defaultOutput = Join-Path (Join-Path $AuditRoot $RunId) 'source-preflight-v43.json'
if (-not $OutputPath) { $OutputPath = $defaultOutput }
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$allowedOutputRoot = $AuditRoot.TrimEnd('\') + '\'
if (-not $OutputPath.StartsWith($allowedOutputRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Source-preflight output must remain under the external v43 audit root.' }

$checks = New-Object 'System.Collections.Generic.List[object]'
$snapshot = $null
$failure = $null
$failureDetails = $null
$manifestSha = $null
$manifestGroupCount = $null
$manifestTestCount = $null
try {
    $gitVersion = (Invoke-GitText $RepoRoot @('--version')).Trim()
    if ($gitVersion -notmatch '^git version ') { throw 'Git executable did not return a recognizable version.' }
    [void]$checks.Add([pscustomobject]@{ id = 'gitExecutable'; status = 'PASS'; path = $script:GitExecutable; sha256 = Get-Sha256File $script:GitExecutable; version = $gitVersion })

    if (-not [IO.File]::Exists($RequiredCTestManifestPath)) { throw "Required CTest manifest is missing: $RequiredCTestManifestPath" }
    $manifestBytes = [IO.File]::ReadAllBytes($RequiredCTestManifestPath)
    $manifestText = (New-Object Text.UTF8Encoding($false, $true)).GetString($manifestBytes)
    $ctestManifest = $manifestText | ConvertFrom-Json -ErrorAction Stop
    if ($null -eq $ctestManifest.required_test_groups) { throw 'Required CTest manifest lacks required_test_groups.' }
    $testNames = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    $manifestGroupCount = 0
    $manifestTestCount = 0
    foreach ($property in $ctestManifest.required_test_groups.PSObject.Properties) {
        $manifestGroupCount++
        $groupNames = @($property.Value)
        if ($groupNames.Count -eq 0) { throw "Required CTest group is empty: $($property.Name)" }
        foreach ($testNameValue in $groupNames) {
            $testName = [string]$testNameValue
            if ($testName -notmatch '^[A-Za-z0-9_.-]+$' -or -not $testNames.Add($testName)) { throw "Malformed or duplicate required CTest name: $testName" }
            $manifestTestCount++
        }
    }
    if ($manifestGroupCount -eq 0 -or $manifestTestCount -eq 0) { throw 'Required CTest manifest contains no required groups/tests.' }
    $manifestSha = Get-Sha256Bytes $manifestBytes
    $relativeCTest = Get-RelativePathForSource $RepoRoot $RequiredCTestManifestPath
    if (-not (Test-IsBuildInput $relativeCTest)) { throw 'Required CTest manifest is outside the declared build-input scope.' }
    [void]$checks.Add([pscustomobject]@{ id = 'requiredCTestManifest'; status = 'PASS'; path = $RequiredCTestManifestPath; relativePath = $relativeCTest; sha256 = $manifestSha; groupCount = $manifestGroupCount; testCount = $manifestTestCount })

    $first = Get-SourceSnapshot $RepoRoot
    $second = Get-SourceSnapshot $RepoRoot
    if ($first.sourceTreeSha256 -cne $second.sourceTreeSha256 -or $first.rootCommit -cne $second.rootCommit -or
        $first.rootWorktreeStatusSha256 -cne $second.rootWorktreeStatusSha256 -or
        ($first.submoduleCommits | ConvertTo-Json -Depth 10 -Compress) -cne ($second.submoduleCommits | ConvertTo-Json -Depth 10 -Compress)) {
        throw 'Source/build inputs or Git identities changed between the two bounded scans.'
    }
    $snapshot = $second
    [void]$checks.Add([pscustomobject]@{ id = 'sourceTreeStable'; status = 'PASS'; sourceFileCount = $snapshot.sourceFileCount; sourceTreeSha256 = $snapshot.sourceTreeSha256; rootWorktreeStatus = $snapshot.rootWorktreeStatus; submoduleCount = $snapshot.submoduleCommits.Count })
}
catch { $failure = $_.Exception.Message; $failureDetails = $_.ScriptStackTrace }

$receipt = [ordered]@{
    schema = 'range.pc11-v43-source-preflight/v1'
    status = if ($null -eq $failure) { 'PASS_SOURCE_PREFLIGHT' } else { 'SOURCE_PREFLIGHT_FAIL' }
    evidenceClass = 'BUILD_INPUT_SNAPSHOT_ONLY_NOT_RELEASE_OR_PRODUCT_QUALIFICATION'
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    repoRoot = $RepoRoot
    gitExecutablePath = $script:GitExecutable
    gitExecutableSha256 = if ($checks.Count -gt 0 -and $checks[0].id -eq 'gitExecutable') { [string]$checks[0].sha256 } else { $null }
    sourceRootPath = $RepoRoot
    sourceScopeSchema = 'range.pc11-v43-source-scope/v1'
    sourceScope = [ordered]@{
        includePrefixes = @('src/', 'scripts/packaging/', 'scripts/benchmark/', 'scripts/ci/', 'cmake/')
        includeBasenamesAnywhere = @('CMakeLists.txt', 'CMakePresets.json', 'CMakeSettings.json', 'vcpkg.json', 'vcpkg-configuration.json')
        includeExtensionsAnywhere = @('.cmake', '.cmake.in')
        includeExactPaths = @('.github/workflows/windows-regression.yml', 'docs/PIEZO_CONTROL_DELIVERY.md', 'docs/piezo_control_plan.json')
        excludePrefixes = @('build/', '.git/')
        enumeration = 'git ls-files -co --exclude-standard -z per repo root and recursive submodule; omit gitlink entries; hash all included tracked+nonignored untracked regular files; reject build-like extension outside include rules; reparse/case collision/control chars reject'
        pathOrder = 'UTF8_BYTE_LEXICOGRAPHIC'
        treeDigest = "SHA256(UTF8(JSON(sourceFiles, sort_keys=true, separators=(',',':'), ensure_ascii=false)))"
    }
    sourceExclusions = @('build/** generated/configured output', '.git/** metadata', 'other narrative docs and evidence; docs/PIEZO_DEPENDENCY_PROVENANCE.md is pinned by install/release evidence')
    requiredCTestManifestPath = $RequiredCTestManifestPath
    requiredCTestManifestSha256 = $manifestSha
    requiredCTestGroupCount = $manifestGroupCount
    requiredCTestNameCount = $manifestTestCount
    rootCommit = if ($null -ne $snapshot) { $snapshot.rootCommit } else { $null }
    rootWorktreeStatus = if ($null -ne $snapshot) { $snapshot.rootWorktreeStatus } else { $null }
    rootWorktreeStatusSha256 = if ($null -ne $snapshot) { $snapshot.rootWorktreeStatusSha256 } else { $null }
    submoduleCommits = if ($null -ne $snapshot) { $snapshot.submoduleCommits } else { @() }
    sourceFiles = if ($null -ne $snapshot) { $snapshot.sourceFiles } else { @() }
    sourceFileCount = if ($null -ne $snapshot) { $snapshot.sourceFileCount } else { 0 }
    sourceTotalBytes = if ($null -ne $snapshot) { $snapshot.sourceTotalBytes } else { 0 }
    sourceTreeSha256 = if ($null -ne $snapshot) { $snapshot.sourceTreeSha256 } else { $null }
    checks = $checks.ToArray()
    failure = $failure
    failureDetails = $failureDetails
    productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
    goPermitted = $false
}

Write-JsonNoOverwrite $OutputPath $receipt
Write-Output ("{0} sourceTreeSha256={1} output={2}" -f $receipt.status, $receipt.sourceTreeSha256, $OutputPath)
if ($receipt.status -cne 'PASS_SOURCE_PREFLIGHT') { exit 1 }
