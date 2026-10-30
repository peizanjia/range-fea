[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$ReceiptPath)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'This v43 package-tree scalar fixture requires Windows PowerShell 5.1.'
}
$ReceiptPath = [IO.Path]::GetFullPath($ReceiptPath)
if ([IO.File]::Exists($ReceiptPath)) { throw "Refusing to overwrite fixture receipt: $ReceiptPath" }

# Exercise the producer's actual validators without starting product actions.
$producerPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\package-tree-v43.ps1'))
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($producerPath, [ref]$tokens, [ref]$parseErrors)
if (@($parseErrors).Count -gt 0) { throw 'The actual package-tree producer does not parse.' }
$needed = @('Get-Sha256Bytes', 'Assert-Hash', 'Read-Json', 'ConvertTo-JsonString', 'Assert-JsonObject', 'Assert-JsonString',
    'Assert-JsonBoolean', 'Assert-JsonInteger', 'Get-ManifestIdentitySha256',
    'Assert-ScalarPinEqual', 'Assert-StrictNoGo', 'Get-NormalizedSubmodulePins', 'Assert-SubmodulePinsEqual')
$functions = New-Object 'System.Collections.Generic.List[string]'
foreach ($name in $needed) {
    $functionAst = @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name }, $true)) | Select-Object -First 1
    if ($null -eq $functionAst) { throw "Missing producer function: $name" }
    [void]$functions.Add($functionAst.Extent.Text)
}
Invoke-Expression ([string]::Join("`n", $functions.ToArray()))

$script:rejected = New-Object 'System.Collections.Generic.List[string]'
function Assert-Rejected([string]$Name, [scriptblock]$Action) {
    $didThrow = $false
    try { & $Action } catch { $didThrow = $true }
    if (-not $didThrow) { throw "Malformed package-tree input was accepted: $Name" }
    [void]$script:rejected.Add($Name)
}
function Copy-Fixture($Value) { return ([string]($Value | ConvertTo-Json -Depth 20) | ConvertFrom-Json -ErrorAction Stop) }

$sha = 'a' * 64
$manifest = [pscustomobject]@{
    installBinding = [pscustomobject]@{ installRoot = 'C:\fixture\install'; inventoryReportSha256 = $sha }
    sourceAndAuthorityPins = [pscustomobject]@{
        rootCommit = 'b' * 40; releaseBuildReceiptSha256 = $sha
        sourceFreezeReceiptSha256 = $sha; sourceTreeSha256 = $sha
    }
}
$identity = Get-ManifestIdentitySha256 $manifest
if ($identity -notmatch '^[0-9a-f]{64}$') { throw 'Valid manifest identity control failed.' }
$noGo = [pscustomobject]@{
    staticOnly = $true; goPermitted = $false
    productActions = [pscustomobject]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
}
Assert-StrictNoGo $noGo 'valid no-GO control' $true
Assert-ScalarPinEqual $sha $sha 'valid matching SHA control'
$pins = @([pscustomobject]@{ path = 'src/vendor'; registeredCommit = 'c' * 40; headCommit = 'c' * 40; worktreeStatus = 'clean'; sourceTreeSha256 = $sha })
Assert-SubmodulePinsEqual $pins $pins 'valid matching submodule control'
foreach ($rows in @(@(), $pins, @($pins[0], (Copy-Fixture $pins[0])))) {
    $flatJson = ConvertTo-Json -InputObject (Get-NormalizedSubmodulePins $rows) -Depth 10 -Compress
    $roundTrip = $flatJson | ConvertFrom-Json -ErrorAction Stop
    if (-not ($roundTrip -is [array]) -or $roundTrip.Count -ne $rows.Count -or
        @($roundTrip | Where-Object { $_ -is [array] }).Count -ne 0) {
        throw 'Submodule pin receipt shape is nested or has a wrong row count.'
    }
}

Assert-Rejected 'schema-array' { Assert-JsonString ([pscustomobject]@{ schema = @('x') }) 'schema' 'fixture' }
Assert-Rejected 'status-empty-array' { Assert-JsonString ([pscustomobject]@{ status = @() }) 'status' 'fixture' }
Assert-Rejected 'inventory-version-array' { Assert-JsonInteger ([pscustomobject]@{ inventorySchemaVersion = @(2) }) 'inventorySchemaVersion' 'fixture' 2 }
Assert-Rejected 'inventory-count-boolean' { Assert-JsonInteger ([pscustomobject]@{ fileCount = $true }) 'fileCount' 'fixture' 1 }
Assert-Rejected 'go-permitted-array' { $x = Copy-Fixture $noGo; $x.goPermitted = @($false); Assert-StrictNoGo $x 'fixture' $true }
Assert-Rejected 'static-only-array' { $x = Copy-Fixture $noGo; $x.staticOnly = @($true); Assert-StrictNoGo $x 'fixture' $true }
Assert-Rejected 'action-array' { $x = Copy-Fixture $noGo; $x.productActions.guiLaunched = @($false); Assert-StrictNoGo $x 'fixture' $true }
Assert-Rejected 'actions-not-object' { $x = Copy-Fixture $noGo; $x.productActions = @(); Assert-StrictNoGo $x 'fixture' $true }
Assert-Rejected 'identity-root-commit-array' { $x = Copy-Fixture $manifest; $x.sourceAndAuthorityPins.rootCommit = @('b' * 40); Get-ManifestIdentitySha256 $x }
Assert-Rejected 'identity-build-sha-empty-array' { $x = Copy-Fixture $manifest; $x.sourceAndAuthorityPins.releaseBuildReceiptSha256 = @(); Get-ManifestIdentitySha256 $x }
Assert-Rejected 'different-build-receipt' { Assert-ScalarPinEqual $sha ('d' * 64) 'fixture build SHA' }
Assert-Rejected 'build-sha-array' { Assert-ScalarPinEqual $sha @($sha) 'fixture build SHA' }
Assert-Rejected 'submodule-pin-array-field' { $x = Copy-Fixture $pins; $x[0].sourceTreeSha256 = @($sha); Assert-SubmodulePinsEqual $x $pins 'fixture submodule' }
Assert-Rejected 'submodule-commit-mismatch' { $x = Copy-Fixture $pins; $x[0].headCommit = 'd' * 40; Assert-SubmodulePinsEqual $x $pins 'fixture submodule' }
Assert-Rejected 'nested-submodule-single-row' { $nested = New-Object 'object[]' 1; $nested[0] = $pins; Assert-SubmodulePinsEqual $nested $pins 'fixture submodule' }

$parent = [IO.Path]::GetDirectoryName($ReceiptPath)
if (-not [IO.Directory]::Exists($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
$jsonPath = Join-Path $parent ('package-tree-pinned-input-' + [Guid]::NewGuid().ToString('N') + '.json')
$firstBytes = (New-Object Text.UTF8Encoding($false)).GetBytes('{"flag":true}')
[IO.File]::WriteAllBytes($jsonPath, $firstBytes)
$firstSha = Get-Sha256Bytes $firstBytes
$bound = Read-Json $jsonPath $firstSha
if (-not ($bound.flag -is [bool]) -or $bound.flag -ne $true) { throw 'Valid pinned JSON control failed.' }
[IO.File]::WriteAllText($jsonPath, '{"flag":false}', (New-Object Text.UTF8Encoding($false)))
Assert-Rejected 'pinned-json-changed-between-reads' { Read-Json $jsonPath $firstSha }

$receipt = [ordered]@{
    schema = 'range.pc11-v43-package-tree-scalar-fixture/v1'; status = 'PASS'; testOnly = $true
    producerPath = $producerPath; acceptedControls = 7; rejectedCases = $script:rejected.ToArray()
    caseCount = $script:rejected.Count + 7
    productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
    goPermitted = $false
}
[IO.File]::WriteAllText($ReceiptPath, (($receipt | ConvertTo-Json -Depth 16) + "`n"), (New-Object Text.UTF8Encoding($false)))
Write-Output ("PASS caseCount={0} goPermitted=false receipt={1}" -f $receipt.caseCount, $ReceiptPath)
