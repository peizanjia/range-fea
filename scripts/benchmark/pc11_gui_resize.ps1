param([Parameter(Mandatory=$true)][string]$Manifest,[Parameter(Mandatory=$true)][int]$GuiProcessId,[Parameter(Mandatory=$true)][long[]]$WindowHandles)
$ErrorActionPreference='Stop'
$spec=Get-Content -LiteralPath $Manifest -Raw|ConvertFrom-Json
$gui=Get-Process -Id $GuiProcessId
if($gui.MainModule.FileName -ine $spec.executable -or (Get-FileHash -LiteralPath $spec.executable).Hash -ine $spec.executableSha256){throw 'Owned executable identity mismatch.'}
Add-Type -AssemblyName System.Drawing
$wpf=Join-Path ([Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory()) 'WPF'
Add-Type -Path (Join-Path $PSScriptRoot 'pc11_gui_uia.cs') -ReferencedAssemblies @((Join-Path $wpf 'UIAutomationClient.dll'),(Join-Path $wpf 'UIAutomationTypes.dll'),(Join-Path $wpf 'WindowsBase.dll'))
Add-Type -Path (Join-Path $PSScriptRoot 'pc11_gui_native.cs') -ReferencedAssemblies @([Drawing.Bitmap].Assembly.Location)
[Pc11GuiUia]::EnableDpiAwareness()
function Snapshot {
    $rows=Get-CimInstance Win32_Process|Select-Object ProcessId,ParentProcessId,ExecutablePath
    $owned=@($GuiProcessId)
    do{$new=@($rows|Where-Object{$owned -contains [int]$_.ParentProcessId -and $owned -notcontains [int]$_.ProcessId}|ForEach-Object{[int]$_.ProcessId});$owned+=$new}while($new.Count)
    $values=@($rows|Where-Object{$owned -contains [int]$_.ProcessId}|ForEach-Object{
        $p=Get-Process -Id $_.ProcessId -ErrorAction Stop
        [pscustomobject]@{pid=$_.ProcessId;creationUtc=$p.StartTime.ToUniversalTime().ToString('o');parentPid=$_.ParentProcessId;image=$_.ExecutablePath;workingSetBytes=$p.WorkingSet64}
    })
    [pscustomobject]@{utc=[DateTime]::UtcNow.ToString('o');processes=$values;sumWorkingSetBytes=($values|Measure-Object workingSetBytes -Sum).Sum;windows=@($WindowHandles|ForEach-Object{[Pc11GuiNative]::Details($_,$GuiProcessId)})}
}
$before=Snapshot
foreach($handle in $WindowHandles){[Pc11GuiNative]::FitWindow($handle,$GuiProcessId,1600,900)}
Start-Sleep -Seconds 5
$after=Snapshot
Start-Sleep -Seconds 25
$delayed=Snapshot
$result=@{schema='range.pc11-owned-viewport-diagnostic/v1';guiPid=$GuiProcessId;noActivate=$true;hide=$false;workingSetTrim=$false;before=$before;after5seconds=$after;after30seconds=$delayed;guiAcceptanceVerified=$false}
[IO.File]::WriteAllText((Join-Path $spec.output 'viewport-resize-diagnostic.json'),($result|ConvertTo-Json -Depth 12),(New-Object Text.UTF8Encoding($false)))
$result|ConvertTo-Json -Depth 12
