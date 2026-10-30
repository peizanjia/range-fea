$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-V43BytesSha256([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-V43FileSha256([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}

function ConvertTo-V43JsonString([string]$Value) {
    return [string]($Value | ConvertTo-Json -Compress -ErrorAction Stop)
}

function Quote-V43WindowsArgument([string]$Value) {
    if ($null -eq $Value) { $Value = '' }
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

function Get-V43CandidateManifestIdentitySha256($Manifest) {
    if ($Manifest.schema -cne 'range.pc11-fresh-run-v43-admission-candidate/v1' -or $Manifest.candidateVersion -cne 'v43') {
        throw 'Candidate identity requires the v43 admission-candidate schema.'
    }
    $binding = $Manifest.installBinding
    $source = $Manifest.sourceAndAuthorityPins
    if ($null -eq $binding -or $null -eq $source) { throw 'Candidate identity pins are incomplete.' }
    $installRoot = [string]$binding.installRoot
    $inventorySha = ([string]$binding.inventoryReportSha256).ToLowerInvariant()
    $freezeSha = ([string]$source.sourceFreezeReceiptSha256).ToLowerInvariant()
    $rootCommit = ([string]$source.rootCommit).ToLowerInvariant()
    $sourceTreeSha = ([string]$source.sourceTreeSha256).ToLowerInvariant()
    $buildSha = ([string]$source.releaseBuildReceiptSha256).ToLowerInvariant()
    if ([string]::IsNullOrWhiteSpace($installRoot) -or $installRoot -match '^PENDING_' -or
        $inventorySha -notmatch '^[0-9a-f]{64}$' -or $freezeSha -notmatch '^[0-9a-f]{64}$' -or
        $sourceTreeSha -notmatch '^[0-9a-f]{64}$' -or $buildSha -notmatch '^[0-9a-f]{64}$' -or
        $rootCommit -notmatch '^(?:[0-9a-f]{40}|[0-9a-f]{64})$') {
        throw 'Candidate identity contains a pending or malformed v43 source/install pin.'
    }
    $parts = @(
        '{"candidateVersion":"v43"',
        ',"installRoot":' + (ConvertTo-V43JsonString $installRoot),
        ',"inventoryReportSha256":' + (ConvertTo-V43JsonString $inventorySha),
        ',"releaseBuildReceiptSha256":' + (ConvertTo-V43JsonString $buildSha),
        ',"rootCommit":' + (ConvertTo-V43JsonString $rootCommit),
        ',"schema":"range.pc11-fresh-run-v43-admission-candidate/v1"',
        ',"sourceFreezeReceiptSha256":' + (ConvertTo-V43JsonString $freezeSha),
        ',"sourceTreeSha256":' + (ConvertTo-V43JsonString $sourceTreeSha) + '}'
    )
    return Get-V43BytesSha256 ((New-Object Text.UTF8Encoding($false, $true)).GetBytes([string]::Join('', $parts)))
}

function Assert-V43CandidateManifest(
    [string]$ManifestPath,
    [string]$ExpectedManifestSha256,
    [string]$ExpectedCandidateIdentitySha256
) {
    $path = [IO.Path]::GetFullPath($ManifestPath)
    if (-not [IO.File]::Exists($path)) { throw 'Pinned v43 candidate manifest is missing.' }
    $attributes = [IO.File]::GetAttributes($path)
    if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Candidate manifest must not be a reparse point.' }
    if ($ExpectedManifestSha256 -notmatch '^[0-9a-fA-F]{64}$') {
        throw 'Candidate manifest bytes differ from the externally supplied v43 SHA256.'
    }

    $maximumManifestBytes = 4MB
    $stream = $null
    try {
        $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        $attributes = [IO.File]::GetAttributes($path)
        if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Candidate manifest must not be a reparse point.' }
        $length = [Int64]$stream.Length
        if ($length -lt 1 -or $length -gt $maximumManifestBytes) {
            throw "Candidate manifest size must be between 1 and $maximumManifestBytes bytes."
        }
        $bytes = New-Object byte[] ([int]$length)
        $offset = 0
        while ($offset -lt $bytes.Length) {
            $count = $stream.Read($bytes, $offset, $bytes.Length - $offset)
            if ($count -le 0) { throw 'Candidate manifest ended while its pinned bytes were being read.' }
            $offset += $count
        }
        if ($stream.Length -ne $length -or $stream.ReadByte() -ne -1) {
            throw 'Candidate manifest changed while its pinned bytes were being read.'
        }
    }
    finally {
        if ($null -ne $stream) { $stream.Dispose() }
    }

    $actualManifestSha256 = Get-V43BytesSha256 $bytes
    if ($actualManifestSha256 -cne $ExpectedManifestSha256.ToLowerInvariant()) {
        throw 'Candidate manifest bytes differ from the externally supplied v43 SHA256.'
    }
    $text = (New-Object Text.UTF8Encoding($false, $true)).GetString($bytes)
    $manifest = $text | ConvertFrom-Json -ErrorAction Stop
    if (-not ($manifest.schema -is [string]) -or $manifest.schema -cne 'range.pc11-fresh-run-v43-admission-candidate/v1' -or
        -not ($manifest.candidateVersion -is [string]) -or $manifest.candidateVersion -cne 'v43' -or
        -not ($manifest.goPermitted -is [bool]) -or $manifest.goPermitted -ne $false) {
        throw 'Candidate manifest schema/version or no-GO boundary is invalid.'
    }
    $actions = $manifest.productActions
    $requiredActions = @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')
    if ($null -eq $actions -or @($actions.PSObject.Properties.Name).Count -ne $requiredActions.Count) {
        throw 'Candidate manifest must declare the complete v43 product-action boundary.'
    }
    foreach ($name in $requiredActions) {
        $action = $actions.$name
        if (-not ($action -is [bool]) -or $action -ne $false) { throw "Candidate manifest permits product action $name." }
    }
    foreach ($name in @('staticReadinessDoesNotGrantProductGo', 'externalIndependentReviewRequired', 'separateRootTechnicalDecisionRequired', 'goSealRequiredBeforeAnyProductAction')) {
        $boundary = $manifest.workflowBoundary.$name
        if (-not ($boundary -is [bool]) -or $boundary -ne $true) { throw "Candidate workflow boundary is missing $name." }
    }
    $identity = Get-V43CandidateManifestIdentitySha256 $manifest
    if ($ExpectedCandidateIdentitySha256 -notmatch '^[0-9a-fA-F]{64}$' -or
        $identity -cne $ExpectedCandidateIdentitySha256.ToLowerInvariant()) {
        throw 'Candidate manifest identity differs from the externally supplied v43 identity.'
    }
    return [pscustomobject]@{
        path = $path
        sha256 = $ExpectedManifestSha256.ToLowerInvariant()
        candidateManifestIdentitySha256 = $identity
        manifest = $manifest
    }
}

function Assert-V43NoGoEnvelope($Receipt, [string]$Label, [string]$CandidateIdentitySha256) {
    if ($CandidateIdentitySha256 -notmatch '^[0-9a-fA-F]{64}$') { throw "$Label has an invalid candidate identity pin." }
    if ($null -eq $Receipt -or -not ($Receipt.candidateVersion -is [string]) -or $Receipt.candidateVersion -cne 'v43' -or
        -not ($Receipt.variant -is [string]) -or $Receipt.variant -cne 'OFF' -or
        -not ($Receipt.staticOnly -is [bool]) -or $Receipt.staticOnly -ne $true -or
        -not ($Receipt.goPermitted -is [bool]) -or $Receipt.goPermitted -ne $false -or
        -not ($Receipt.candidateManifestIdentitySha256 -is [string]) -or
        $Receipt.candidateManifestIdentitySha256 -cne $CandidateIdentitySha256.ToLowerInvariant()) {
        throw "$Label does not preserve the candidate-bound v43 OFF static-only no-GO envelope."
    }
    $actions = $Receipt.productActions
    if ($null -eq $actions) {
        throw "$Label must declare the complete v43 product-action boundary."
    }
    if ($actions -is [System.Collections.IDictionary]) {
        $actionNames = @($actions.Keys | ForEach-Object { [string]$_ })
    }
    else {
        $actionNames = @($actions.PSObject.Properties.Name | ForEach-Object { [string]$_ })
    }
    $requiredActions = @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')
    if ($actionNames.Count -ne $requiredActions.Count) {
        throw "$Label must declare the complete v43 product-action boundary."
    }
    foreach ($name in $requiredActions) {
        if ($name -notin $actionNames) { throw "$Label omits product-action field $name." }
        $action = $actions.$name
        if (-not ($action -is [bool]) -or $action -ne $false) { throw "$Label permits product action $name." }
    }
}

function Write-V43JsonNewFile([string]$Path, $Value, [int]$Depth = 12) {
    $full = [IO.Path]::GetFullPath($Path)
    $parent = [IO.Path]::GetDirectoryName($full)
    if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
    $temp = $full + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    $bytes = (New-Object Text.UTF8Encoding($false, $true)).GetBytes(($Value | ConvertTo-Json -Depth $Depth))
    $stream = $null
    try {
        $stream = New-Object IO.FileStream($temp, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    }
    finally { if ($null -ne $stream) { $stream.Dispose() } }
    try {
        if ([IO.File]::Exists($full)) { throw "Refusing to overwrite v43 evidence: $full" }
        [IO.File]::Move($temp, $full)
    }
    finally { if ([IO.File]::Exists($temp)) { [IO.File]::Delete($temp) } }
    return $full
}

function Test-V43PathUnderRoot([string]$Path, [string]$Root) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $prefix = $fullRoot + [IO.Path]::DirectorySeparatorChar
    return $fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
}
