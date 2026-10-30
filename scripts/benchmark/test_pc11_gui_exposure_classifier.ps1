$ErrorActionPreference = 'Stop'
$sourcePath = Join-Path $PSScriptRoot 'pc11_gui_uia.cs'
$source = [IO.File]::ReadAllText($sourcePath)
$pattern = '(?s)public static bool ClassifyExposureHit\(bool hitIsTarget,bool hitIsTargetChild,uint hitPid,uint targetPid\)\s*\{(?<body>[^{}]*)\}'
$match = [regex]::Match($source, $pattern)
if (!$match.Success) { throw 'Could not locate the production exposure classifier method.' }

# Compile and exercise the exact pure method body without loading UI Automation
# or querying any desktop windows.
$testSource = "public static class Pc11GuiExposureClassifierRegression {`n$($match.Value)`n}"
Add-Type -TypeDefinition $testSource -Language CSharp -ErrorAction Stop

$samePid = [uint32]731
$otherPid = [uint32]732
$cases = @(
    [pscustomobject]@{ Name = 'same PID target hit'; IsTarget = $true; IsTargetChild = $false; HitPid = $samePid; TargetPid = $samePid; Expected = $true },
    [pscustomobject]@{ Name = 'same PID target child'; IsTarget = $false; IsTargetChild = $true; HitPid = $samePid; TargetPid = $samePid; Expected = $true },
    [pscustomobject]@{ Name = 'same PID main-only hit'; IsTarget = $false; IsTargetChild = $false; HitPid = $samePid; TargetPid = $samePid; Expected = $false },
    [pscustomobject]@{ Name = 'other PID target hit'; IsTarget = $true; IsTargetChild = $false; HitPid = $otherPid; TargetPid = $samePid; Expected = $false }
)

foreach ($case in $cases) {
    $actual = [Pc11GuiExposureClassifierRegression]::ClassifyExposureHit(
        $case.IsTarget, $case.IsTargetChild, $case.HitPid, $case.TargetPid)
    if ([bool]$actual -ne [bool]$case.Expected) {
        throw "Exposure classifier regression failed: $($case.Name); expected=$($case.Expected); actual=$actual"
    }
    Write-Output "PASS: $($case.Name) => $actual"
}

Write-Output "PASS: $($cases.Count) deterministic exposure classifier cases"
