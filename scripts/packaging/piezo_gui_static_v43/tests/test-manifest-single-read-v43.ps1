[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'This v43 manifest single-read test requires Windows PowerShell 5.1.'
}

$runtimeRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'runtime'
. (Join-Path $runtimeRoot 'contract-common-v43.ps1')

$tempRoot = Join-Path ([IO.Path]::GetFullPath([IO.Path]::GetTempPath())) ('range-fea-v43-single-read-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($tempRoot)
try {
    $manifestPath = Join-Path $tempRoot 'candidate-manifest-v43.json'
    $originalManifestBytes = $null
    $replacementManifestBytes = $null
    $expectedManifestSha256 = $null
    $manifestMutatedAfterHash = $false
    $legacyFileHashUsed = $false
    $originalManifestBytesSha256 = (Get-Command Get-V43BytesSha256 -CommandType Function).ScriptBlock

    $manifest = [ordered]@{
        schema = 'range.pc11-fresh-run-v43-admission-candidate/v1'
        candidateVersion = 'v43'
        goPermitted = $false
        productActions = [ordered]@{
            guiLaunched = $false
            savePerformed = $false
            runPerformed = $false
            confirmPerformed = $false
        }
        workflowBoundary = [ordered]@{
            staticReadinessDoesNotGrantProductGo = $true
            externalIndependentReviewRequired = $true
            separateRootTechnicalDecisionRequired = $true
            goSealRequiredBeforeAnyProductAction = $true
        }
        installBinding = [ordered]@{
            installRoot = Join-Path $tempRoot 'off-install'
            inventoryReportSha256 = '1' * 64
        }
        sourceAndAuthorityPins = [ordered]@{
            rootCommit = 'a' * 40
            sourceTreeSha256 = '2' * 64
            sourceFreezeReceiptSha256 = '3' * 64
            releaseBuildReceiptSha256 = '4' * 64
        }
    }
    $identity = Get-V43CandidateManifestIdentitySha256 $manifest
    $utf8 = New-Object Text.UTF8Encoding($false, $true)
    $originalManifestBytes = $utf8.GetBytes(($manifest | ConvertTo-Json -Depth 8 -Compress))
    $expectedManifestSha256 = Get-V43BytesSha256 $originalManifestBytes
    [IO.File]::WriteAllBytes($manifestPath, $originalManifestBytes)

    $replacementManifest = [ordered]@{}
    foreach ($name in $manifest.Keys) { $replacementManifest[$name] = $manifest[$name] }
    $replacementManifest['raceMarker'] = 'replacement-bytes-after-hash'
    $replacementManifestBytes = $utf8.GetBytes(($replacementManifest | ConvertTo-Json -Depth 8 -Compress))

    function Get-V43BytesSha256([byte[]]$Bytes) {
        $digest = & $script:originalManifestBytesSha256 $Bytes
        if (-not $script:manifestMutatedAfterHash -and $Bytes.Length -eq $script:originalManifestBytes.Length) {
            [IO.File]::WriteAllBytes($script:manifestPath, $script:replacementManifestBytes)
            $script:manifestMutatedAfterHash = $true
        }
        return $digest
    }

    function Get-V43FileSha256([string]$Path) {
        $script:legacyFileHashUsed = $true
        [IO.File]::WriteAllBytes($Path, $script:replacementManifestBytes)
        return $script:expectedManifestSha256
    }

    $binding = Assert-V43CandidateManifest $manifestPath $expectedManifestSha256 $identity
    if (-not $manifestMutatedAfterHash) { throw 'The deterministic mutation hook did not run after hashing the pinned manifest bytes.' }
    if ($legacyFileHashUsed) { throw 'Candidate validation called the separate file-hash helper.' }
    if ($binding.sha256 -cne $expectedManifestSha256.ToLowerInvariant() -or
        $binding.candidateManifestIdentitySha256 -cne $identity -or
        $binding.manifest.candidateVersion -cne 'v43' -or
        $binding.manifest.goPermitted -ne $false) {
        throw 'Candidate validation did not preserve the externally pinned v43 identity and no-GO boundary.'
    }
    foreach ($name in @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')) {
        if ($binding.manifest.productActions.$name -ne $false) { throw "Candidate validation permits product action $name." }
    }
    if ($null -ne $binding.manifest.PSObject.Properties['raceMarker']) {
        throw 'Candidate validation parsed replacement bytes instead of the bytes covered by the external SHA256.'
    }

    $oversizedPath = Join-Path $tempRoot 'oversized-manifest-v43.json'
    [IO.File]::WriteAllBytes($oversizedPath, (New-Object byte[] (4MB + 1)))
    $oversizedRejected = $false
    try {
        $null = Assert-V43CandidateManifest $oversizedPath ('0' * 64) $identity
    }
    catch {
        if ($_.Exception.Message -like '*size*') { $oversizedRejected = $true }
        else { throw }
    }
    if (-not $oversizedRejected) { throw 'Candidate validation accepted a manifest larger than the configured byte bound.' }

    Write-Output 'PASS: v43 candidate manifest hashing/parsing uses the same single-read bytes and preserves no-GO.'
}
finally {
    if ([IO.Directory]::Exists($tempRoot)) { [IO.Directory]::Delete($tempRoot, $true) }
}
