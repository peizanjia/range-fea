[CmdletBinding()]
param([string]$AuditRoot)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'This candidate snapshot regression requires Windows PowerShell 5.1.'
}
if (-not $AuditRoot) {
    $AuditRoot = Join-Path ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..'))) 'build\audit-20261014\piezo-delivery\gui-fresh-run-v43-output-audit'
}
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)
if (-not [IO.Directory]::Exists($AuditRoot)) { throw "External audit root is missing: $AuditRoot" }

$producer = Join-Path $PSScriptRoot 'test-candidate-path-root-v43.ps1'
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($producer, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw 'Candidate path/root producer has parse errors.' }
$names = @('Assert-Object','Assert-NoGoBoundary','Get-JsonSnapshot','Get-PinnedJsonSnapshot')
foreach ($name in $names) {
    $matches = @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name }, $true))
    if ($matches.Count -ne 1) { throw "Expected one real $name function in candidate path/root producer." }
    Invoke-Expression $matches[0].Extent.Text
}

function Get-Sha([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Write-Sample([string]$Path, $Value) {
    $json = $Value | ConvertTo-Json -Depth 8 -Compress
    $bytes = (New-Object Text.UTF8Encoding($false, $true)).GetBytes($json)
    [IO.File]::WriteAllBytes($Path, $bytes)
    return Get-Sha $bytes
}
function Assert-Rejected([scriptblock]$Body, [string]$Name) {
    $rejected = $false
    try { & $Body | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw "Candidate path/root snapshot accepted invalid case $Name." }
}

$caseRoot = Join-Path $AuditRoot ('candidate-path-root-snapshot-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($caseRoot)
$samplePath = Join-Path $caseRoot 'receipt.json'
$base = [ordered]@{
    schema = 'range.pc11-v43-source-preflight/v1'
    status = 'PASS_SOURCE_PREFLIGHT'
    goPermitted = $false
    productActions = [ordered]@{ guiLaunched=$false; savePerformed=$false; runPerformed=$false; confirmPerformed=$false }
}
$pinA = Write-Sample $samplePath $base
$snapA = Get-PinnedJsonSnapshot $samplePath $pinA 'Normal preflight'
if ($snapA.sha256 -cne $pinA -or $snapA.value.schema -cne $base.schema) { throw 'Normal snapshot did not preserve the pinned bytes and parsed value.' }
Assert-NoGoBoundary $snapA.value 'Normal preflight' $false
Assert-Rejected { Assert-NoGoBoundary $snapA.value 'Normal preflight' $true } 'preflight-absent-staticOnly-required'

$base.productActions.runPerformed = $true
$pinB = Write-Sample $samplePath $base
if ($pinA -ceq $pinB -or $snapA.sha256 -cne $pinA -or $snapA.value.productActions.runPerformed -ne $false) {
    throw 'Parsed snapshot changed after the on-disk receipt was replaced.'
}
Assert-Rejected { Get-PinnedJsonSnapshot $samplePath $pinA 'Changed preflight' } 'changed-bytes-old-pin'
$snapB = Get-PinnedJsonSnapshot $samplePath $pinB 'Changed preflight'
Assert-Rejected { Assert-NoGoBoundary $snapB.value 'Changed preflight' $false } 'product-action-true'

$base.productActions.runPerformed = $false
$base.goPermitted = @($false)
$arrayPin = Write-Sample $samplePath $base
$arraySnap = Get-PinnedJsonSnapshot $samplePath $arrayPin 'Array preflight'
Assert-Rejected { Assert-NoGoBoundary $arraySnap.value 'Array preflight' $false } 'array-boolean'

$base.goPermitted = $false
$base.staticOnly = $true
$staticPin = Write-Sample $samplePath $base
$staticSnap = Get-PinnedJsonSnapshot $samplePath $staticPin 'Static receipt'
Assert-NoGoBoundary $staticSnap.value 'Static receipt' $true

Write-Output "PASS_CANDIDATE_PATH_ROOT_SNAPSHOT_V43 cases=7 audit=$caseRoot"
