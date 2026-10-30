[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$ReceiptPath)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'This v43 finalizer scalar fixture requires Windows PowerShell 5.1.'
}
$ReceiptPath = [IO.Path]::GetFullPath($ReceiptPath)
if ([IO.File]::Exists($ReceiptPath)) { throw "Refusing to overwrite single-use fixture receipt: $ReceiptPath" }

# Load only the actual validation/identity functions from the finalizer AST.
# This keeps the fixture synthetic: it cannot run product actions or finalize
# external product evidence, while exercising the code used by the finalizer.
$finalizerPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\finalize-v43.ps1'))
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($finalizerPath, [ref]$tokens, [ref]$parseErrors)
if (@($parseErrors).Count -gt 0) { throw 'The actual v43 finalizer does not parse.' }
$needed = @('Get-Sha256Bytes', 'ConvertTo-JsonString', 'Assert-JsonObject', 'Assert-JsonString', 'Assert-JsonBoolean', 'Assert-JsonStringArray', 'Assert-JsonObjectArray', 'Assert-Sha256Field', 'Get-ManifestIdentitySha256', 'Assert-NoGoBoundary')
$functionText = New-Object 'System.Collections.Generic.List[string]'
foreach ($name in $needed) {
    $functionAst = @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name }, $true)) | Select-Object -First 1
    if ($null -eq $functionAst) { throw "Required finalizer function was not found: $name" }
    [void]$functionText.Add($functionAst.Extent.Text)
}
Invoke-Expression ([string]::Join("`n", $functionText.ToArray()))

$script:rejected = New-Object 'System.Collections.Generic.List[string]'
function Assert-Rejected([string]$Case, [scriptblock]$Action) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    if (-not $failed) { throw "Expected malformed scalar fixture to be rejected: $Case" }
    [void]$script:rejected.Add($Case)
}
function Copy-Fixture($Value) {
    return ([string]($Value | ConvertTo-Json -Depth 20) | ConvertFrom-Json -ErrorAction Stop)
}

$sha = 'a' * 64
$validManifest = [pscustomobject]@{
    installBinding = [pscustomobject]@{ installRoot = 'C:\fixture\install'; inventoryReportSha256 = $sha }
    sourceAndAuthorityPins = [pscustomobject]@{
        rootCommit = 'b' * 40
        releaseBuildReceiptSha256 = $sha
        sourceFreezeReceiptSha256 = $sha
        sourceTreeSha256 = $sha
    }
}
$identity = Get-ManifestIdentitySha256 $validManifest
if ($identity -notmatch '^[0-9a-f]{64}$') { throw 'Valid scalar manifest identity control did not produce a SHA256.' }

Assert-Rejected 'schema-array' { Assert-JsonString ([pscustomobject]@{ schema = @('one', 'two') }) 'schema' 'fixture' }
Assert-Rejected 'status-array' { Assert-JsonString ([pscustomobject]@{ status = @('PASS', 'PASS') }) 'status' 'fixture' }
Assert-Rejected 'status-integer' { Assert-JsonString ([pscustomobject]@{ status = 1 }) 'status' 'fixture' }
Assert-Rejected 'root-commit-array' { $m = Copy-Fixture $validManifest; $m.sourceAndAuthorityPins.rootCommit = @('b' * 40); Get-ManifestIdentitySha256 $m }
Assert-Rejected 'empty-source-tree-sha-array' { $m = Copy-Fixture $validManifest; $m.sourceAndAuthorityPins.sourceTreeSha256 = @(); Get-ManifestIdentitySha256 $m }
Assert-Rejected 'empty-inventory-sha-array' { $m = Copy-Fixture $validManifest; $m.installBinding.inventoryReportSha256 = @(); Get-ManifestIdentitySha256 $m }
Assert-Rejected 'package-receipt-sha-array' { Assert-Sha256Field ([pscustomobject]@{ packageTreeReceiptSha256 = @() }) 'packageTreeReceiptSha256' 'fixture' }
Assert-Rejected 'package-receipt-sha-integer' { Assert-Sha256Field ([pscustomobject]@{ packageTreeReceiptSha256 = 64 }) 'packageTreeReceiptSha256' 'fixture' }
Assert-Rejected 'package-receipt-path-array' { Assert-JsonString ([pscustomobject]@{ packageTreeReceiptPath = @('receipt.json') }) 'packageTreeReceiptPath' 'fixture' }
Assert-Rejected 'source-freeze-receipt-sha-array' { $m = Copy-Fixture $validManifest; $m.sourceAndAuthorityPins.sourceFreezeReceiptSha256 = @($sha); Get-ManifestIdentitySha256 $m }
Assert-Rejected 'required-test-ids-not-array' { Assert-JsonStringArray ([pscustomobject]@{ requiredTestIds = 'parser' }) 'requiredTestIds' 'fixture' }

$validNoGo = [pscustomobject]@{
    candidateVersion = 'v43'; variant = 'OFF'; staticOnly = $true; goPermitted = $false
    productActions = [pscustomobject]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
}
Assert-NoGoBoundary $validNoGo 'valid fixture no-GO'
Assert-Rejected 'no-go-envelope-candidate-version-array' { $x = Copy-Fixture $validNoGo; $x.candidateVersion = @('v43'); Assert-NoGoBoundary $x 'fixture' }
Assert-Rejected 'no-go-envelope-static-only-array' { $x = Copy-Fixture $validNoGo; $x.staticOnly = @($true); Assert-NoGoBoundary $x 'fixture' }
Assert-Rejected 'no-go-envelope-go-permitted-array' { $x = Copy-Fixture $validNoGo; $x.goPermitted = @($false); Assert-NoGoBoundary $x 'fixture' }
Assert-Rejected 'no-go-action-array' { $x = Copy-Fixture $validNoGo; $x.productActions.guiLaunched = @($false); Assert-NoGoBoundary $x 'fixture' }
Assert-Rejected 'result-test-id-array' { Assert-JsonString ([pscustomobject]@{ testId = @('parser') }) 'testId' 'fixture result' }
Assert-Rejected 'result-status-array' { Assert-JsonString ([pscustomobject]@{ status = @('PASS') }) 'status' 'fixture result' }

$parent = [IO.Path]::GetDirectoryName($ReceiptPath)
if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
$fixtureReceipt = [ordered]@{
    schema = 'range.pc11-v43-finalizer-scalar-fixture/v1'
    status = 'PASS'
    candidateVersion = 'v43'
    variant = 'OFF'
    staticOnly = $true
    testedFinalizerPath = $finalizerPath
    acceptedValidIdentityControl = $true
    acceptedValidNoGoControl = $true
    rejectedCases = $script:rejected.ToArray()
    caseCount = $script:rejected.Count + 2
    productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
    goPermitted = $false
}
[IO.File]::WriteAllText($ReceiptPath, (($fixtureReceipt | ConvertTo-Json -Depth 10) + "`n"), (New-Object Text.UTF8Encoding($false)))
Write-Output ("PASS v43 scalar finalizer fixture rejected={0} receipt={1} goPermitted=false" -f $script:rejected.Count, $ReceiptPath)
