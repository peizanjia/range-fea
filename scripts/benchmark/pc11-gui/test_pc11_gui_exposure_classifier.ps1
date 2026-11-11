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

# Exercise the production containment predicate used by both single-window and
# floating-workbench layout. This fixture never queries or moves real windows.
$nativeSource = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'pc11_gui_native.cs'))
$inside = [regex]::Match($nativeSource, '(?s)static bool Inside\(Rect inner,Rect outer\)\s*\{[^{}]*\}')
if (!$inside.Success) { throw 'Could not locate the production work-area containment predicate.' }
Add-Type -TypeDefinition @"
public static class Pc11GuiWorkAreaRegression {
    struct Rect { public int Left, Top, Right, Bottom; }
    $($inside.Value)
    public static bool Check(int left,int top,int right,int bottom) {
        return Inside(new Rect { Left=left,Top=top,Right=right,Bottom=bottom },
                      new Rect { Left=-1920,Top=40,Right=0,Bottom=1080 });
    }
}
"@ -Language CSharp -ErrorAction Stop
$geometryCases = @(
    [pscustomobject]@{ name='inside negative-coordinate monitor'; bounds=@(-1800,60,-100,1000); expected=$true },
    [pscustomobject]@{ name='exact work area'; bounds=@(-1920,40,0,1080); expected=$true },
    [pscustomobject]@{ name='left overflow'; bounds=@(-1921,40,0,1080); expected=$false },
    [pscustomobject]@{ name='taskbar overlap'; bounds=@(-1920,39,0,1080); expected=$false },
    [pscustomobject]@{ name='right overflow'; bounds=@(-1920,40,1,1080); expected=$false },
    [pscustomobject]@{ name='bottom overflow'; bounds=@(-1920,40,0,1081); expected=$false }
)
foreach ($case in $geometryCases) {
    $b = $case.bounds
    $actual = [Pc11GuiWorkAreaRegression]::Check($b[0],$b[1],$b[2],$b[3])
    if ($actual -ne $case.expected) { throw "Work-area regression failed: $($case.name)" }
}
Write-Output "PASS: $($geometryCases.Count) deterministic work-area containment cases"
