<# Normally stop one verified audit GUI's sole native solver and preserve evidence.
No process kill, registry mutation, lock removal, global input or focus change.
#>
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Manifest,
      [Parameter(Mandatory=$true)][int]$GuiProcessId,
      [Parameter(Mandatory=$true)][string]$JobId)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if($PSVersionTable.PSEdition -ne 'Desktop'){throw 'Windows PowerShell5.1 STA required.'}
$spec=Get-Content -LiteralPath $Manifest -Raw -Encoding UTF8|ConvertFrom-Json
$out=[IO.Path]::GetFullPath($spec.output)
$audit=Join-Path $out ('normal-cancel-'+$JobId)
if([IO.Directory]::Exists($audit)){throw 'Cancellation evidence directory must be fresh.'}
$profile=if($spec.PSObject.Properties['profile']){$spec.profile}else{Join-Path $out 'application-profile'}
$gui=Get-Process -Id $GuiProcessId
$created=$gui.StartTime.ToUniversalTime().ToString('o')
$launch=@(Get-Content (Join-Path $out 'uia-actions.jsonl') -Encoding UTF8|ForEach-Object{$_|ConvertFrom-Json}|Where-Object action -eq 'verified-gui')
if($launch.Count -ne 1 -or $launch[0].guiPid -ne $GuiProcessId -or $launch[0].detail.creationUtc -ne $created){throw 'Owned GUI launch/lifetime mismatch.'}
if($gui.MainModule.FileName -ine $spec.executable -or (Get-FileHash -LiteralPath $spec.executable).Hash -ine $spec.executableSha256){throw 'Owned installed executable/SHA mismatch.'}
[void][IO.Directory]::CreateDirectory($audit)
$utf8=New-Object Text.UTF8Encoding($false)
function Write-Json([string]$name,$value){[IO.File]::WriteAllText((Join-Path $audit $name),($value|ConvertTo-Json -Depth 30),$utf8)}
function Read-OwnedJob {
    $text=& $spec.python -B (Join-Path $PSScriptRoot 'pc11_gui_registry.py') --profile $profile --gui-pid $GuiProcessId --job-id $JobId
    if($LASTEXITCODE -ne 0){throw "Owned registry proof failed: $text"}
    $proof=$text|ConvertFrom-Json
    if($proof.record.sourceModelFileRef -ine $spec.sourceModel.Replace('\','/') -and [IO.Path]::GetFullPath($proof.record.sourceModelFileRef) -ine [IO.Path]::GetFullPath($spec.sourceModel)){throw 'Registered job original source path mismatch.'}
    if($proof.record.nativeCheckpointContext.sourceSha256 -ine $spec.sourceSha256){throw 'Registered original source SHA mismatch.'}
    return $proof
}
function Own-Processes {
    $rows=Get-CimInstance Win32_Process
    $owned=@($GuiProcessId)
    do{$new=@($rows|Where-Object{$owned -contains [int]$_.ParentProcessId -and $owned -notcontains [int]$_.ProcessId}|ForEach-Object{[int]$_.ProcessId});$owned+=$new}while($new.Count)
    return @($rows|Where-Object{$owned -contains [int]$_.ProcessId}|ForEach-Object{
        $p=Get-Process -Id $_.ProcessId -ErrorAction Stop
        [pscustomobject]@{pid=$_.ProcessId;parentPid=$_.ParentProcessId;creationUtc=$p.StartTime.ToUniversalTime().ToString('o');image=$_.ExecutablePath;commandLine=$_.CommandLine;workingSetBytes=$p.WorkingSet64}
    })
}
$before=Read-OwnedJob
if($before.record.state -notin @('running','queued','cancelling')){throw 'Expected active owned native job before cancellation.'}
$processes=Own-Processes
$solvers=@($processes|Where-Object{[IO.Path]::GetFileName($_.image) -ieq 'fea-solver.exe'})
if($solvers.Count -ne 1 -or $solvers[0].parentPid -ne $GuiProcessId -or [string]$solvers[0].pid -ne $before.record.nativeProcessId -or $solvers[0].commandLine -notmatch ('--task-id='+[regex]::Escape($JobId)+'(?:\s|$)')){throw 'Stop-all UI requires exactly the expected sole owned solver child.'}
Write-Json 'before-registry-proof.json' $before
Write-Json 'before-process-identities.json' @{utc=[DateTime]::UtcNow.ToString('o');guiCreationUtc=$created;processes=$processes}
Copy-Item -LiteralPath $before.record.nativeJobRecordRef -Destination (Join-Path $audit 'before-job-record-raw.json')
Copy-Item -LiteralPath $before.acceptedPrefix.envelopeFile -Destination (Join-Path $audit 'before-accepted-envelope-raw.json')
foreach($relative in @('working-set','resize-diagnostic-working-set')){
    $directory=Join-Path $out $relative
    foreach($file in @('summary.json','ownership-events.jsonl','working-set.csv')){
        $path=Join-Path $directory $file
        if([IO.File]::Exists($path)){Copy-Item -LiteralPath $path -Destination (Join-Path $audit ($relative+'-before-'+$file))}
    }
}
$wpf=Join-Path ([Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory()) 'WPF'
Add-Type -Path (Join-Path $PSScriptRoot 'pc11_gui_uia.cs') -ReferencedAssemblies @((Join-Path $wpf 'UIAutomationClient.dll'),(Join-Path $wpf 'UIAutomationTypes.dll'),(Join-Path $wpf 'WindowsBase.dll'))
Add-Type -AssemblyName System.Drawing
Add-Type -Path (Join-Path $PSScriptRoot 'pc11_gui_native.cs') -ReferencedAssemblies @([Drawing.Bitmap].Assembly.Location)
[Pc11GuiUia]::EnableDpiAwareness()
function Capture-Own([string]$stage){
    $seen=New-Object 'Collections.Generic.HashSet[string]'
    $controls=@([Pc11GuiUia]::Elements($GuiProcessId)|ForEach-Object{
        $c=$_.Current
        if($c.ProcessId -eq $GuiProcessId -and $seen.Add(($_.GetRuntimeId()-join ','))){[pscustomobject]@{name=$c.Name;id=$c.AutomationId;type=$c.ControlType.ProgrammaticName;enabled=$c.IsEnabled;offscreen=$c.IsOffscreen;handle=$c.NativeWindowHandle;bounds=$c.BoundingRectangle.ToString()}}
    })
    Write-Json ($stage+'-controls.json') $controls
    foreach($id in @('Application.MainWindow','Application.controlDock')){
        $window=[Pc11GuiUia]::Find($GuiProcessId,$id,'','Window',$true)
        if($null -ne $window){[Pc11GuiNative]::Capture($window.Current.NativeWindowHandle,$GuiProcessId,(Join-Path $audit ($stage+'-'+$id+'.png')))}
    }
}
Capture-Own 'before'
$stop=[Pc11GuiUia]::Find($GuiProcessId,'','Stop solver','Button',$true)
if($null -eq $stop -or !$stop.Current.IsEnabled){throw 'Explicit own visible enabled normal Stop solver unavailable; job kept running.'}
# Recheck identity and sole running solver immediately before opening Stop.
$check=Read-OwnedJob
if($check.record.nativeProcessId -ne $before.record.nativeProcessId -or $check.record.state -ne 'running'){throw 'Owned job changed before Stop gesture.'}
Write-Json 'stop-gesture.json' @{utc=[DateTime]::UtcNow.ToString('o');jobId=$JobId;proof=[Pc11GuiUia]::PostControl($stop,$GuiProcessId);normalProductStop=$true}
$deadline=[DateTime]::UtcNow.AddSeconds(30); $dialog=$null
while([DateTime]::UtcNow -lt $deadline){$dialog=[Pc11GuiUia]::Find($GuiProcessId,'','Stop solver','Window',$true);if($null -ne $dialog){break};Start-Sleep -Milliseconds 100}
if($null -eq $dialog){throw 'Owned Stop solver confirmation unavailable; no affirmative input was sent.'}
[Pc11GuiUia]::CheckDialog($dialog,$GuiProcessId,'Stop solver')
$question=[Pc11GuiUia]::Descendant($dialog,'','Are you sure you want to stop all running solvers?','Text')
if($null -eq $question){throw 'Unexpected owned normal Stop confirmation text.'}
$yes=[Pc11GuiUia]::Descendant($dialog,'','Yes','Button')
if($null -eq $yes){$yes=[Pc11GuiUia]::Descendant($dialog,'','&Yes','Button')}
if($null -eq $yes -or !$yes.Current.IsEnabled -or $yes.Current.IsOffscreen){throw 'Owned Stop confirmation lacks visible enabled Yes.'}
[Pc11GuiUia]::CheckDialog($dialog,$GuiProcessId,'Stop solver')
Write-Json 'normal-stop-confirmed.json' @{utc=[DateTime]::UtcNow.ToString('o');jobId=$JobId;proof=[Pc11GuiUia]::PostControl($yes,$GuiProcessId)}
$deadline=[DateTime]::UtcNow.AddSeconds(120)
while($true){
    $after=Read-OwnedJob
    if($after.record.state -in @('cancelled','failed','succeeded','interrupted')){break}
    if([DateTime]::UtcNow -gt $deadline){Write-Json 'terminal-wait-timeout.json' $after;throw 'Owned normal Stop did not persist terminal state; GUI remains open.'}
    Start-Sleep -Milliseconds 500
}
if(!$after.record.processExited){Write-Json 'terminal-without-exit.json' $after;throw 'Durable terminal record lacks actual process exit proof.'}
$deadline=[DateTime]::UtcNow.AddSeconds(60)
while($true){
    $remaining=@(Own-Processes|Where-Object{$_.pid -ne $GuiProcessId})
    if($remaining.Count -eq 0){break}
    if([DateTime]::UtcNow -gt $deadline){Write-Json 'remaining-own-children.json' $remaining;throw 'Owned compute descendants remain; GUI remains open.'}
    Start-Sleep -Milliseconds 250
}
Write-Json 'after-terminal-registry-proof.json' $after
Copy-Item -LiteralPath $after.record.nativeJobRecordRef -Destination (Join-Path $audit 'after-terminal-job-record-raw.json')
Capture-Own 'after-terminal'
# Documented stop-file protocols, never terminate the sampler processes.
foreach($name in @('sampler.stop','resize-diagnostic-sampler.stop')){[IO.File]::WriteAllText((Join-Path $out $name),'stop',$utf8)}
Start-Sleep -Seconds 2
foreach($relative in @('working-set','resize-diagnostic-working-set')){
    $summary=Join-Path (Join-Path $out $relative) 'summary.json'
    if([IO.File]::Exists($summary)){Copy-Item -LiteralPath $summary -Destination (Join-Path $audit ($relative+'-after-summary.json'))}
}
& (Join-Path $PSScriptRoot 'pc11_gui_close.ps1') -Manifest $Manifest -GuiProcessId $GuiProcessId
if(Get-Process -Id $GuiProcessId -ErrorAction SilentlyContinue){throw 'Owned GUI still exists after normal Quit.'}
Write-Json 'normal-cancellation-receipt.json' @{status='NORMAL_PRODUCT_STOP_THEN_QUIT';guiPid=$GuiProcessId;creationUtc=$created;jobId=$JobId;terminalState=$after.record.state;processExited=$after.record.processExited;acceptedPrefix=$after.acceptedPrefix;complete20000=$false;qualification='NONE';utc=[DateTime]::UtcNow.ToString('o')}
[pscustomobject]@{status='NORMAL_PRODUCT_STOP_THEN_QUIT';guiPid=$GuiProcessId;jobId=$JobId;terminalState=$after.record.state;acceptedPrefix=$after.acceptedPrefix.acceptedSeq;evidence=$audit}|ConvertTo-Json
