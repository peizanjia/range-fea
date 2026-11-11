# Close only an explicitly identified, owned audit GUI by its normal quit UI.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Manifest,[Parameter(Mandatory=$true)][int]$GuiProcessId)
$ErrorActionPreference='Stop'
$spec=Get-Content -LiteralPath $Manifest -Raw -Encoding UTF8 | ConvertFrom-Json
$gui=Get-Process -Id $GuiProcessId
$created=$gui.StartTime.ToUniversalTime().ToString('o')
$launch=@(Get-Content -LiteralPath (Join-Path $spec.output 'uia-actions.jsonl') -Encoding UTF8 | ForEach-Object{$_|ConvertFrom-Json} | Where-Object action -eq 'verified-gui')
if($launch.Count -ne 1 -or $launch[0].guiPid -ne $GuiProcessId -or $launch[0].detail.creationUtc -ne $created){throw 'Owned audit GUI lifetime mismatch.'}
if ([IO.Path]::GetFullPath($gui.MainModule.FileName) -ine [IO.Path]::GetFullPath($spec.executable)) { throw 'Live executable mismatch.' }
if ((Get-FileHash -LiteralPath $spec.executable).Hash -ine $spec.executableSha256) { throw 'Executable SHA mismatch.' }
$wpf=Join-Path ([Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory()) 'WPF'
Add-Type -Path (Join-Path $PSScriptRoot 'pc11_gui_uia.cs') -ReferencedAssemblies @((Join-Path $wpf 'UIAutomationClient.dll'),(Join-Path $wpf 'UIAutomationTypes.dll'),(Join-Path $wpf 'WindowsBase.dll'))
[Pc11GuiUia]::EnableDpiAwareness()
$main=[Pc11GuiUia]::Find($GuiProcessId,'Application.MainWindow','','Window',$true)
if($null -eq $main){throw 'Exact owned main unavailable; cancel any owned file picker first.'}
$closeProof=[Pc11GuiUia]::CloseWindow($main,$GuiProcessId)
$deadline=[DateTime]::UtcNow.AddSeconds(20); $dialog=$null
while([DateTime]::UtcNow -lt $deadline){
    $dialog=[Pc11GuiUia]::Find($GuiProcessId,'','Quit?','Window',$true)
    if($null -ne $dialog){break}
    if($gui.HasExited){throw 'GUI exited without observed Quit confirmation; preserve evidence.'}
    Start-Sleep -Milliseconds 100
}
if($null -eq $dialog){throw 'Own Quit confirmation unavailable; no process was killed.'}
[Pc11GuiUia]::CheckDialog($dialog,$GuiProcessId,'Quit?')
$quit=[Pc11GuiUia]::Descendant($dialog,'','Quit','Button')
if($null -eq $quit){throw 'Own Quit button unavailable.'}
$quitProof=[Pc11GuiUia]::PostControl($quit,$GuiProcessId)
$dockProof=$null
if(!$gui.WaitForExit(5000)){
    $remainingMain=[Pc11GuiUia]::Find($GuiProcessId,'Application.MainWindow','','Window',$true)
    $dock=[Pc11GuiUia]::Find($GuiProcessId,'Application.controlDock','','Window',$true)
    $windows=@([Pc11GuiUia]::Elements($GuiProcessId)|Where-Object{$_.Current.ControlType -eq [Windows.Automation.ControlType]::Window -and !$_.Current.IsOffscreen}|ForEach-Object{$_.Current.NativeWindowHandle}|Select-Object -Unique)
    if($null -ne $remainingMain -or $null -eq $dock -or $windows.Count -ne 1 -or $windows[0] -ne $dock.Current.NativeWindowHandle){throw 'Unexpected remaining main/modal; no window or process was killed.'}
    $dockProof=[Pc11GuiUia]::CloseWindow($dock,$GuiProcessId)
    if(!$gui.WaitForExit(15000)){throw 'Owned GUI remains after normal dock Close; no process was killed.'}
}
$receipt=@{status='OWNED_GUI_CLOSED_NORMALLY';pid=$GuiProcessId;creationUtc=$created;executable=$spec.executable;executableSha256=$spec.executableSha256;utc=[DateTime]::UtcNow.ToString('o')}
$receipt.normalCloseGesture=$closeProof; $receipt.quitGesture=$quitProof; $receipt.remainingDockCloseGesture=$dockProof
if([IO.Directory]::Exists($spec.output)){
    [IO.File]::WriteAllText((Join-Path $spec.output ('normal-close-'+$GuiProcessId+'.json')),($receipt|ConvertTo-Json),(New-Object Text.UTF8Encoding($false)))
}
$receipt|ConvertTo-Json
