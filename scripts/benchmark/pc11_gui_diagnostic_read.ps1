<# First real plot/full CSV on an existing owned installed GUI.
Explicit functional diagnosis under competing load; never performance qualification.
#>
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Manifest,
      [Parameter(Mandatory=$true)][int]$GuiProcessId,
      [Parameter(Mandatory=$true)][string]$JobId,
      [Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if($PSVersionTable.PSEdition -ne 'Desktop'){throw 'Windows PowerShell5.1 STA required.'}
$spec=Get-Content -LiteralPath $Manifest -Raw -Encoding UTF8|ConvertFrom-Json
$priorOut=$spec.output
$out=[IO.Path]::GetFullPath($Output)
if([IO.Directory]::Exists($out) -or !$out.StartsWith([IO.Path]::GetFullPath($priorOut).TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Fresh diagnosis subdirectory inside exact original audit output required.'}
$exe=$spec.executable; $install=$spec.installedRoot; $python=$spec.python
$source=$spec.sourceModel; $workspace=$spec.workspace
$profile=if($spec.PSObject.Properties['profile']){$spec.profile}else{Join-Path $priorOut 'application-profile'}
$liveEvents=$spec.performanceEvents
if([IO.File]::Exists($liveEvents) -and (Get-Item -LiteralPath $liveEvents).Length -gt 0){throw 'Earlier GUI performance requests exist; preserve them rather than claim first read.'}
$script:gui=Get-Process -Id $GuiProcessId
$created=$script:gui.StartTime.ToUniversalTime().ToString('o')
$owned=@(Get-Content (Join-Path $priorOut 'uia-actions.jsonl') -Encoding UTF8|ForEach-Object{$_|ConvertFrom-Json}|Where-Object action -eq 'verified-gui')
if($owned.Count -ne 1 -or $owned[0].guiPid -ne $GuiProcessId -or $owned[0].detail.creationUtc -ne $created -or $script:gui.MainModule.FileName -ine $exe -or (Get-FileHash -LiteralPath $exe).Hash -ine $spec.executableSha256){throw 'Existing owned GUI lifetime/executable/SHA mismatch.'}
$terminalText=& $python -B (Join-Path $PSScriptRoot 'pc11_gui_terminal.py') --profile $profile --gui-pid $GuiProcessId
if($LASTEXITCODE -ne 0){throw "Expected real owned terminal20000: $terminalText"}
$terminal=$terminalText|ConvertFrom-Json
if($terminal.jobId -ne $JobId){throw 'Terminal native job mismatch.'}
$result=$terminal.resultModel
$csv=Join-Path $out 'complete-20001.csv'
[void][IO.Directory]::CreateDirectory($out)
$utf8=New-Object Text.UTF8Encoding($false)
$script:guiPid=$GuiProcessId; $script:actions=Join-Path $out 'uia-actions.jsonl'
$script:activeNativePicker=0; $script:measurementWindow=$null
$script:nativePickersOnly=$true # This bounded attachment targets the approved v7 native-dialog installation.
$events=Join-Path $out 'gui-performance.jsonl'; $stop=Join-Path $out 'sampler.stop'
$sampler=$null
$wpf=Join-Path ([Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory()) 'WPF'
Add-Type -Path (Join-Path $wpf 'UIAutomationClient.dll')
Add-Type -Path (Join-Path $wpf 'UIAutomationTypes.dll')
Add-Type -AssemblyName System.Drawing
Add-Type -Path (Join-Path $PSScriptRoot 'pc11_gui_uia.cs') -ReferencedAssemblies @((Join-Path $wpf 'UIAutomationClient.dll'),(Join-Path $wpf 'UIAutomationTypes.dll'),(Join-Path $wpf 'WindowsBase.dll'))
Add-Type -Path (Join-Path $PSScriptRoot 'pc11_gui_native.cs') -ReferencedAssemblies @([Drawing.Bitmap].Assembly.Location)
[Pc11GuiUia]::EnableDpiAwareness()
# Reuse only the owned harness function definitions, never its launch/run body.
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'pc11_gui_uia.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Owned UIA harness parser failure.'}
$functionNames=@('Log','Dump','Wait-Control','Action','Picker','Capture','Wait-Event','Assert-SamplerAlive')
foreach($node in $ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$true)){
    if($node.Name -in $functionNames){Invoke-Expression $node.Extent.Text}
}
$spec.output=$out
$spec|Add-Member resultModel $result -Force
$spec|Add-Member csv $csv -Force
$spec|Add-Member diagnosticOnly $true -Force
$spec|Add-Member originalFailedAudit $priorOut -Force
[IO.File]::WriteAllText((Join-Path $out 'manifest.json'),($spec|ConvertTo-Json -Depth 12),$utf8)
[IO.File]::WriteAllText((Join-Path $out 'terminal-native-job.json'),($terminal|ConvertTo-Json -Depth 15),$utf8)
Log 'verified-gui' @{image=$exe;sha256=$spec.executableSha256;creationUtc=$created;existingOwnedLaunch=$true;discoveryOnly=$false}
Log 'functional-diagnostic-only' @{performanceQualification=$false;underCompetingScienceAndRegressionLoad=$true;originalFullAuditRemainsInvalid=$priorOut;providerOpened=$false}
try{
    $conhost='C:\Windows\System32\conhost.exe'
    if((Get-FileHash -LiteralPath $conhost).Hash -ine 'c86146188b78e484f32cc29e2331689659943b47689a85570342044f499938ac'){throw 'Approved exact conhost SHA changed.'}
    $args=@('-B',(Join-Path $PSScriptRoot 'pc11_working_set.py'),'--root-pid',[string]$GuiProcessId,'--expected-root-image',$exe,'--allowed-root',$install,'--allowed-root',$conhost,'--output',(Join-Path $out 'working-set'),'--stop-file',$stop)
    foreach($arg in $args){if($arg.Contains('"')){throw 'Unsupported quotes in sampler argument.'}}
    $sampler=Start-Process -FilePath $python -ArgumentList (($args|ForEach-Object{'"'+$_+'"'})-join ' ') -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $out 'sampler.stdout.txt') -RedirectStandardError (Join-Path $out 'sampler.stderr.txt')
    $deadline=[DateTime]::UtcNow.AddSeconds(10);$samples=Join-Path $out 'working-set\working-set.csv'
    while(![IO.File]::Exists($samples) -or @(Get-Content -LiteralPath $samples).Count -lt 3){if($sampler.HasExited -or [DateTime]::UtcNow -gt $deadline){throw 'Diagnostic baseline sampler unavailable.'};Start-Sleep -Milliseconds 100}
    Log 'baseline-sampler-ready' @{samplerPid=$sampler.Id;qualification='NONE';wholeComputeCoverage=$false}
    $script:main=Wait-Control 'Application.MainWindow' '' 'Window' 30 $true
    Action '' 'Piezo control' 'TabItem'
    Action '' 'Excitation / initial state / results' 'TabItem'
    $load=Wait-Control 'piezoShowAcceptedSeries' '' 'Button' 30 $false
    $script:measurementWindow=[Pc11GuiUia]::NativeWindowFor($load)
    [Pc11GuiUia]::Reveal($load)
    Log 'diagnostic-before-first-reader' @{main=[Pc11GuiNative]::Details($script:main.Current.NativeWindowHandle,$GuiProcessId);workbench=[Pc11GuiNative]::Details($script:measurementWindow.Current.NativeWindowHandle,$GuiProcessId);providerOpened=$false}
    Capture 'before-first-reader'
    Action 'piezoShowAcceptedSeries' 'Open native result / accepted records and show the selected view…' 'Button' 30 $true
    Picker 'Select certified accepted records for named-port time series' $result
    $paint=Wait-Event 'plot' 'canvas_painted' 240
    Log 'product-canvas-painted' $paint
    Capture 'complete-certified-plot'
    Dump 'complete-plot-controls'
    Action 'piezoExportAcceptedCsv' 'Export selected accepted native records to CSV…' 'Button' 30 $true
    Picker 'Select actual accepted native model records' $result
    Picker 'Export certified accepted samples' $csv $false $true
    $commit=Wait-Event 'csv' 'csv_committed' 240
    Log 'product-csv-committed' $commit
    Capture 'complete-csv-committed'
}catch{
    Log 'harness-failed' @{error=$_.Exception.ToString();performanceQualification=$false}
    try{Dump 'diagnostic-failure-controls';Capture 'diagnostic-failure'}catch{}
    throw
}finally{
    if($null -ne $sampler){[IO.File]::WriteAllText($stop,'stop',$utf8);[void]$sampler.WaitForExit(10000)}
    if([IO.File]::Exists($liveEvents)){Copy-Item -LiteralPath $liveEvents -Destination $events}
    Log 'gui-left-open' @{pid=$GuiProcessId;diagnosticOnly=$true}
}
[pscustomobject]@{status='FUNCTIONAL_DIAGNOSTIC_CAPTURED';performanceQualification=$false;guiPid=$GuiProcessId;jobId=$JobId;plotSeconds=[long]$paint.elapsedNanoseconds/1e9;csvSeconds=[long]$commit.elapsedNanoseconds/1e9;csv=$csv;evidence=$out}|ConvertTo-Json
