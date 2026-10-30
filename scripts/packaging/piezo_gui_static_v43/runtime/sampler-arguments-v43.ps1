$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-V43SamplerPin([string]$Value, [string]$Label) {
    if ([string]::IsNullOrWhiteSpace($Value) -or $Value -notmatch '^[0-9a-fA-F]{64}$') {
        throw "$Label must be a pinned SHA256 hex value."
    }
}

function Test-V43SamplerFullyQualifiedPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    # Path.IsPathRooted accepts drive-relative values such as C:folder on Windows.
    return ($Path -match '^[A-Za-z]:\\' -or $Path -match '^\\\\[^\\]+\\[^\\]+(?:\\|$)')
}

function Quote-V43SamplerArgument([string]$Value) {
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $builder = New-Object Text.StringBuilder
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') { $slashes++; continue }
        if ($character -eq '"') {
            [void]$builder.Append(('\' * (2 * $slashes + 1)))
            [void]$builder.Append('"'); $slashes = 0; continue
        }
        if ($slashes -gt 0) { [void]$builder.Append(('\' * $slashes)); $slashes = 0 }
        [void]$builder.Append($character)
    }
    if ($slashes -gt 0) { [void]$builder.Append(('\' * (2 * $slashes))) }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function New-V43SamplerArgumentPlan {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][string]$CandidateManifestSha256,
        [Parameter(Mandatory=$true)][string]$CandidateManifestIdentitySha256,
        [Parameter(Mandatory=$true)][string]$PythonExe,
        [Parameter(Mandatory=$true)][string]$SamplerScript,
        [Parameter(Mandatory=$true)][int]$RootPid,
        [Parameter(Mandatory=$true)][string]$ExpectedRootImage,
        [Parameter(Mandatory=$true)][string[]]$AllowedRoots,
        [Parameter(Mandatory=$true)][string]$Output,
        [Parameter(Mandatory=$true)][string]$StopFile,
        [int]$IntervalMilliseconds = 50,
        [int]$MinimumSamples = 10,
        [double]$MinimumSeconds = 0.5
    )
    Assert-V43SamplerPin $CandidateManifestSha256 'Candidate manifest SHA256'
    Assert-V43SamplerPin $CandidateManifestIdentitySha256 'Candidate manifest identity SHA256'
    if ($RootPid -le 0) { throw 'Sampler root PID must be positive.' }
    if ($IntervalMilliseconds -ne 50 -or $MinimumSamples -lt 10 -or [double]::IsNaN($MinimumSeconds) -or
        [double]::IsInfinity($MinimumSeconds) -or $MinimumSeconds -lt 0.5) { throw 'PC-11 sampler bounds are invalid.' }
    if ($AllowedRoots.Count -lt 1) { throw 'At least one allowed descendant image root is required.' }
    foreach ($path in @($PythonExe, $SamplerScript, $ExpectedRootImage, $Output, $StopFile) + $AllowedRoots) {
        if (-not (Test-V43SamplerFullyQualifiedPath $path)) { throw 'Every sampler executable and path must be fully qualified (drive-rooted or UNC).' }
    }
    $program = [IO.Path]::GetFullPath($PythonExe)
    $script = [IO.Path]::GetFullPath($SamplerScript)
    if (-not [IO.File]::Exists($program) -or -not [IO.File]::Exists($script)) { throw 'Pinned Python executable and sampler script must exist.' }
    $args = New-Object 'System.Collections.Generic.List[string]'
    [void]$args.Add($script)
    [void]$args.Add('--root-pid'); [void]$args.Add([string]$RootPid)
    [void]$args.Add('--expected-root-image'); [void]$args.Add([IO.Path]::GetFullPath($ExpectedRootImage))
    foreach ($root in $AllowedRoots) { [void]$args.Add('--allowed-root'); [void]$args.Add([IO.Path]::GetFullPath($root)) }
    [void]$args.Add('--output'); [void]$args.Add([IO.Path]::GetFullPath($Output))
    [void]$args.Add('--stop-file'); [void]$args.Add([IO.Path]::GetFullPath($StopFile))
    [void]$args.Add('--interval-ms'); [void]$args.Add([string]$IntervalMilliseconds)
    [void]$args.Add('--min-samples'); [void]$args.Add([string]$MinimumSamples)
    [void]$args.Add('--min-seconds'); [void]$args.Add(([string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0:0.###}', $MinimumSeconds)))
    $quoted = @($args | ForEach-Object { Quote-V43SamplerArgument ([string]$_) })
    return [pscustomobject]@{
        candidateManifestSha256 = $CandidateManifestSha256.ToLowerInvariant()
        candidateManifestIdentitySha256 = $CandidateManifestIdentitySha256.ToLowerInvariant()
        program = $program
        arguments = [string[]]$args.ToArray()
        commandLine = [string]::Join(' ', $quoted)
        goPermitted = $false
        productActions = [ordered]@{ guiLaunched=$false; savePerformed=$false; runPerformed=$false; confirmPerformed=$false }
    }
}

function Assert-V43SamplerArgumentPlan($Plan, [string]$CandidateManifestSha256, [string]$CandidateManifestIdentitySha256, [string]$ExpectedProgram, [string]$ExpectedSamplerScript) {
    Assert-V43SamplerPin $CandidateManifestSha256 'Candidate manifest SHA256'
    Assert-V43SamplerPin $CandidateManifestIdentitySha256 'Candidate manifest identity SHA256'
    if (-not (Test-V43SamplerFullyQualifiedPath $ExpectedProgram)) { throw 'Expected Python program must be fully qualified.' }
    if (-not (Test-V43SamplerFullyQualifiedPath $ExpectedSamplerScript)) { throw 'Expected sampler script must be fully qualified.' }
    $expectedProgramFull = [IO.Path]::GetFullPath($ExpectedProgram)
    $expectedScriptFull = [IO.Path]::GetFullPath($ExpectedSamplerScript)
    if ($null -eq $Plan -or $Plan.candidateManifestSha256 -cne $CandidateManifestSha256.ToLowerInvariant() -or
        $Plan.candidateManifestIdentitySha256 -cne $CandidateManifestIdentitySha256.ToLowerInvariant() -or
        -not (Test-V43SamplerFullyQualifiedPath ([string]$Plan.program)) -or
        [IO.Path]::GetFullPath([string]$Plan.program) -cne $expectedProgramFull -or
        -not [IO.File]::Exists($expectedProgramFull) -or
        $Plan.goPermitted -isnot [bool] -or $Plan.goPermitted -ne $false -or
        $null -eq $Plan.arguments -or $Plan.arguments.Count -lt 17) { throw 'Sampler argument plan failed candidate or shape validation.' }
    $a = [string[]]$Plan.arguments
    if (-not (Test-V43SamplerFullyQualifiedPath $a[0]) -or [IO.Path]::GetFullPath($a[0]) -cne $expectedScriptFull -or
        -not [IO.File]::Exists($expectedScriptFull)) { throw 'Sampler script argument differs from the existing caller-pinned script.' }
    $i = 1
    if ($a[$i++] -cne '--root-pid' -or $a[$i] -notmatch '^[1-9][0-9]*$') { throw 'Sampler root PID arguments are malformed.' }; $i++
    if ($a[$i++] -cne '--expected-root-image' -or -not (Test-V43SamplerFullyQualifiedPath $a[$i])) { throw 'Sampler expected-image arguments are malformed.' }; $i++
    $allowedCount = 0
    while ($i -lt $a.Count -and $a[$i] -ceq '--allowed-root') {
        $i++; if ($i -ge $a.Count -or -not (Test-V43SamplerFullyQualifiedPath $a[$i])) { throw 'Sampler allowed-root argument is malformed.' }
        $allowedCount++; $i++
    }
    if ($allowedCount -lt 1) { throw 'Sampler requires at least one allowed-root argument.' }
    foreach ($pair in @(@('--output'),@('--stop-file'))) {
        if ($i -ge $a.Count -or $a[$i++] -cne $pair[0] -or $i -ge $a.Count -or -not (Test-V43SamplerFullyQualifiedPath $a[$i])) { throw "Sampler $($pair[0]) arguments are malformed." }; $i++
    }
    if ($i + 6 -ne $a.Count -or $a[$i++] -cne '--interval-ms' -or $a[$i++] -cne '50' -or
        $a[$i++] -cne '--min-samples' -or $a[$i] -notmatch '^[1-9][0-9]*$' -or [int]$a[$i] -lt 10) { throw 'Sampler interval/sample arguments are malformed.' }; $i++
    if ($i -ge $a.Count -or $a[$i++] -cne '--min-seconds' -or $i -ge $a.Count -or $a[$i] -notmatch '^[0-9]+(?:\.[0-9]+)?$' -or [double]::Parse($a[$i], [Globalization.CultureInfo]::InvariantCulture) -lt 0.5) { throw 'Sampler minimum-duration arguments are malformed.' }; $i++
    if ($i -ne $a.Count) { throw 'Sampler argument vector contains trailing values.' }
    $quoted = @($a | ForEach-Object { Quote-V43SamplerArgument $_ })
    if ($Plan.commandLine -cne [string]::Join(' ', $quoted)) { throw 'Sampler command line differs from its exact argument vector.' }
    $actions = $Plan.productActions
    foreach ($name in @('guiLaunched','savePerformed','runPerformed','confirmPerformed')) {
        if ($actions.$name -isnot [bool] -or $actions.$name -ne $false) { throw "Sampler plan permits or mis-types product action $name." }
    }
    return $true
}
