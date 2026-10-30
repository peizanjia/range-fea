<#
Runs UIA against the actual installed fea.exe. Windows PowerShell 5.1 is required.
Preflight does not launch or mutate a product project. Inspect dumps a verified
process's accessible controls. Run performs saved-experiment Run + Confirm only.
Measure observes the product-bound accepted native result and exports complete
CSV through the real Save picker. Full combines Run and Measure. No mode creates
a model, registry or trace.
The JSON manifest and output directory are fresh, explicit audit inputs.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Manifest,
    [ValidateSet('Preflight','Inspect','PickerProbe','Setup','Run','Measure','Full')][string]$Mode='Preflight',
    [int]$GuiProcessId=0,
    [switch]$Launch,
    [switch]$DiscoveryOnly
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop') { throw 'Run with Windows PowerShell 5.1 (powershell.exe), not pwsh.' }
$spec=Get-Content -LiteralPath $Manifest -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($field in @('installedRoot','executable','executableSha256','workspace','sourceModel','sourceSha256','family','output','python')) {
    if (!$spec.PSObject.Properties[$field] -or [string]::IsNullOrWhiteSpace([string]$spec.$field)) { throw "Missing manifest field: $field" }
}
function Absolute([string]$path) {
    if (![IO.Path]::IsPathRooted($path)) { throw "Absolute path required: $path" }
    return [IO.Path]::GetFullPath($path)
}
$exe=Absolute $spec.executable; $install=Absolute $spec.installedRoot
$workspace=Absolute $spec.workspace; $source=Absolute $spec.sourceModel; $out=Absolute $spec.output
$python=Absolute $spec.python
$readRelease=$null
if($spec.PSObject.Properties['timedReadReleaseFile']) {
    $readRelease=Absolute $spec.timedReadReleaseFile
    if([IO.File]::Exists($readRelease) -or [IO.Path]::GetDirectoryName($readRelease) -ine $out){throw 'Timed-read release must be a fresh coordination file in this audit output.'}
}
$profile=Join-Path $out 'application-profile'
if ($spec.PSObject.Properties['profile']) { $profile=Absolute $spec.profile }
if (![IO.File]::Exists($python)) { throw 'Sampler Python executable is unavailable.' }
if (![IO.File]::Exists($exe) -or [IO.Path]::GetFileName($exe) -ine 'fea.exe') { throw 'Actual fea.exe required.' }
if (!$exe.StartsWith($install.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Executable escaped installed root.' }
if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ine $spec.executableSha256) { throw 'Executable SHA-256 mismatch.' }
if (@('axialRod','planarBeam','flatPlate') -notcontains $spec.family) { throw 'Unknown family.' }
if (![IO.Directory]::Exists($workspace)) { throw 'A real existing project workspace is required.' }
if (!$DiscoveryOnly -and ![IO.File]::Exists($source)) { throw 'Saved native source MODEL is required.' }
if (!$DiscoveryOnly -and @('.rbm','.ram') -notcontains [IO.Path]::GetExtension($source).ToLowerInvariant()) { throw 'Native source MODEL suffix required.' }
if ([IO.File]::Exists($source) -and (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ine $spec.sourceSha256) { throw 'Approved source MODEL SHA mismatch.' }
if ($DiscoveryOnly -and $Mode -notin @('Preflight','Inspect')) { throw 'DiscoveryOnly cannot qualify or run an experiment.' }
if ($Mode -in @('Measure','Full')) {
    if (!$spec.PSObject.Properties['csv']) { throw 'Missing csv' }
    $result=$null; $resultSearch=$null; $profileUpgradeResult=$null
    if ($spec.PSObject.Properties['resultModel']) { $result=Absolute $spec.resultModel }
    elseif ($Mode -eq 'Full' -and $spec.PSObject.Properties['resultSearchRoot']) {
        $resultSearch=Absolute $spec.resultSearchRoot
        if (![IO.Directory]::Exists($resultSearch) -or $resultSearch -eq [IO.Path]::GetPathRoot($resultSearch)) { throw 'Bounded existing resultSearchRoot required.' }
    } else { throw 'Provide resultModel, or resultSearchRoot for Full mode.' }
    $csv=Absolute $spec.csv
    if ($csv -eq $source -or $csv -eq $result -or [IO.File]::Exists($csv)) { throw 'CSV must be a fresh separate file.' }
    if ($Mode -eq 'Measure' -and ![IO.File]::Exists($result)) { throw 'Real committed registered native result required.' }
    if ($null -ne $resultSearch) {
        $familyTag=@{axialRod='axial';planarBeam='layered';flatPlate='layered'}[$spec.family]
        $resultFilename='native-'+$familyTag+'.rbm'
        $priorResults=@(Get-ChildItem -LiteralPath $resultSearch -Filter $resultFilename -Recurse -File | ForEach-Object FullName)
    }
}
if ($Mode -eq 'Preflight') {
    [pscustomobject]@{status='PREFLIGHT_ONLY';executable=$exe;sha256=$spec.executableSha256;workspace=$workspace;profile=$profile;family=$spec.family;guiAcceptanceVerified=$false} | ConvertTo-Json
    return
}
if ([IO.Directory]::Exists($out)) { throw 'Output directory already exists; evidence must be fresh.' }
[void][IO.Directory]::CreateDirectory($out)
Copy-Item -LiteralPath $Manifest -Destination (Join-Path $out 'manifest.json')
$utf8=New-Object Text.UTF8Encoding($false)
$events=Join-Path $out 'gui-performance.jsonl'; $stop=Join-Path $out 'sampler.stop'
$liveEvents=$events
if ($spec.PSObject.Properties['performanceEvents']) {
    $liveEvents=Absolute $spec.performanceEvents
    if ([IO.File]::Exists($liveEvents) -or !$liveEvents.StartsWith($workspace.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Performance events must be a fresh file inside the actual project workspace.' }
}
$script:actions=Join-Path $out 'uia-actions.jsonl'; $script:guiPid=0
$script:activeNativePicker=0
function Log([string]$action, $detail) {
    $entry=@{utc=[DateTime]::UtcNow.ToString('o');action=$action;guiPid=$script:guiPid;detail=$detail}
    [IO.File]::AppendAllText($script:actions,($entry | ConvertTo-Json -Depth 12 -Compress)+"`n",$utf8)
}
$wpf=Join-Path ([Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory()) 'WPF'
Add-Type -Path (Join-Path $wpf 'UIAutomationClient.dll')
Add-Type -Path (Join-Path $wpf 'UIAutomationTypes.dll')
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -Path (Join-Path $PSScriptRoot 'pc11_gui_uia.cs') -ReferencedAssemblies @((Join-Path $wpf 'UIAutomationClient.dll'),(Join-Path $wpf 'UIAutomationTypes.dll'),(Join-Path $wpf 'WindowsBase.dll'))
Add-Type -Path (Join-Path $PSScriptRoot 'pc11_gui_native.cs') -ReferencedAssemblies @([Drawing.Bitmap].Assembly.Location)
Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class Pc11PhysicalRamSampler {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Auto)]
    private class MemoryStatusEx {
        public uint dwLength = (uint)Marshal.SizeOf(typeof(MemoryStatusEx));
        public uint dwMemoryLoad;
        public ulong ullTotalPhys;
        public ulong ullAvailPhys;
        public ulong ullTotalPageFile;
        public ulong ullAvailPageFile;
        public ulong ullTotalVirtual;
        public ulong ullAvailVirtual;
        public ulong ullAvailExtendedVirtual;
    }

    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Auto)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GlobalMemoryStatusEx([In, Out] MemoryStatusEx buffer);

    private static readonly object Gate = new object();
    private static Timer timer;
    private static StreamWriter writer;
    private static string directory;
    private static long startedTicks;
    private static long firstNs;
    private static long lastNs;
    private static long previousNs;
    private static long totalPhysicalBytes;
    private static long sampleCount;
    private static double maximumUsedPercent;
    private static double maximumGapMilliseconds;
    private static string error;
    private const long NanosecondsPerSecond = 1000000000L;

    private static long MonotonicNs(long ticks) {
        long frequency = Stopwatch.Frequency;
        return (ticks / frequency) * NanosecondsPerSecond + ((ticks % frequency) * NanosecondsPerSecond) / frequency;
    }

    public static void Start(string outputDirectory, int intervalMilliseconds) {
        lock (Gate) {
            if (timer != null || writer != null) throw new InvalidOperationException("Physical RAM sampler already started.");
            directory = outputDirectory;
            Directory.CreateDirectory(directory);
            writer = new StreamWriter(Path.Combine(directory, "physical-ram.csv"), false, new UTF8Encoding(false));
            writer.WriteLine("monotonic_ns,elapsed_seconds,total_physical_bytes,available_physical_bytes,used_percent");
            writer.Flush();
            startedTicks = Stopwatch.GetTimestamp();
            firstNs = 0; lastNs = 0; previousNs = 0; totalPhysicalBytes = 0; sampleCount = 0;
            maximumUsedPercent = 0.0; maximumGapMilliseconds = 0.0; error = null;
            Capture(null);
            timer = new Timer(Capture, null, intervalMilliseconds, intervalMilliseconds);
        }
    }

    private static void Capture(object state) {
        lock (Gate) {
            if (writer == null || error != null) return;
            MemoryStatusEx memory = new MemoryStatusEx();
            if (!GlobalMemoryStatusEx(memory) || memory.ullTotalPhys == 0 || memory.ullAvailPhys > memory.ullTotalPhys) {
                error = "GlobalMemoryStatusEx failed or returned invalid physical memory values (Win32=" + Marshal.GetLastWin32Error().ToString(CultureInfo.InvariantCulture) + ").";
                return;
            }
            long now = MonotonicNs(Stopwatch.GetTimestamp());
            double elapsed = (Stopwatch.GetTimestamp() - startedTicks) / (double)Stopwatch.Frequency;
            double usedPercent = 100.0 * (1.0 - ((double)memory.ullAvailPhys / (double)memory.ullTotalPhys));
            if (sampleCount > 0) {
                double gap = (now - previousNs) / 1000000.0;
                if (gap > maximumGapMilliseconds) maximumGapMilliseconds = gap;
            } else {
                firstNs = now;
                totalPhysicalBytes = (long)memory.ullTotalPhys;
            }
            writer.WriteLine(string.Format(CultureInfo.InvariantCulture, "{0},{1:F6},{2},{3},{4:F6}", now, elapsed,
                memory.ullTotalPhys, memory.ullAvailPhys, usedPercent));
            if (usedPercent > maximumUsedPercent) maximumUsedPercent = usedPercent;
            previousNs = now;
            lastNs = now;
            sampleCount++;
        }
    }

    public static string Stop() {
        lock (Gate) {
            if (timer != null) {
                timer.Change(Timeout.Infinite, Timeout.Infinite);
                timer.Dispose();
                timer = null;
            }
            Capture(null);
            if (writer != null) {
                writer.Flush();
                writer.Dispose();
                writer = null;
            }
            long coveredNs = (sampleCount > 0) ? lastNs - firstNs : 0;
            string status = (error == null && sampleCount > 0) ? "MEASURED" : "FAILED";
            string summary = "{\"schema\":\"range.pc11-physical-ram/v1\",\"status\":\"" + status +
                "\",\"source\":\"GlobalMemoryStatusEx\",\"samplingIntervalMilliseconds\":50,\"sampleCount\":" + sampleCount.ToString(CultureInfo.InvariantCulture) +
                ",\"totalPhysicalBytes\":" + totalPhysicalBytes.ToString(CultureInfo.InvariantCulture) +
                ",\"maximumUsedPercent\":" + maximumUsedPercent.ToString("R", CultureInfo.InvariantCulture) +
                ",\"maximumSampleGapMilliseconds\":" + maximumGapMilliseconds.ToString("R", CultureInfo.InvariantCulture) +
                ",\"firstSampleMonotonicNs\":" + firstNs.ToString(CultureInfo.InvariantCulture) +
                ",\"lastSampleMonotonicNs\":" + lastNs.ToString(CultureInfo.InvariantCulture) +
                ",\"coveredDurationMilliseconds\":" + (coveredNs / 1000000.0).ToString("R", CultureInfo.InvariantCulture) +
                ((error == null) ? "" : ",\"error\":\"" + error.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"") + "}";
            File.WriteAllText(Path.Combine(directory, "summary.json"), summary, new UTF8Encoding(false));
            return summary;
        }
    }
}
'@
[Pc11GuiUia]::EnableDpiAwareness()
function Dump([string]$name) {
    $rows=@([Pc11GuiUia]::Elements($script:guiPid) | ForEach-Object {
        try { $c=$_.Current; [pscustomobject]@{name=$c.Name;id=$c.AutomationId;type=$c.ControlType.ProgrammaticName;enabled=$c.IsEnabled;offscreen=$c.IsOffscreen;handle=$c.NativeWindowHandle;bounds=$c.BoundingRectangle.ToString()} } catch { }
    })
    [IO.File]::WriteAllText((Join-Path $out ($name+'.json')),($rows | ConvertTo-Json -Depth 5),$utf8)
}
function Wait-Control([string]$id,[string]$name,[string]$type='',[int]$seconds=30,[bool]$visible=$true) {
    $clock=[Diagnostics.Stopwatch]::StartNew()
    while ($clock.Elapsed.TotalSeconds -lt $seconds) {
        if ($script:gui.HasExited) { throw 'Verified GUI exited.' }
        if ([Pc11GuiUia]::LastActionError) { throw [Pc11GuiUia]::LastActionError }
        $element=[Pc11GuiUia]::Find($script:guiPid,$id,$name,$type,$visible)
        if ($null -ne $element) { return $element }
        Start-Sleep -Milliseconds 100
    }
    Dump 'control-timeout'; throw "Control unavailable: $id / $name"
}
function Action([string]$id,[string]$name,[string]$type='',[int]$seconds=30,[bool]$posted=$false) {
    $element=Wait-Control $id $name $type $seconds $false
    $clock=[Diagnostics.Stopwatch]::StartNew()
    while (!$element.Current.IsEnabled -and $clock.Elapsed.TotalSeconds -lt $seconds) { Start-Sleep -Milliseconds 100 }
    if($id -in @('piezoRun','piezoConfirm')){Observe-BaselineMemory}
    # Cache metadata before Invoke: Qt's provider can stay in a modal picker
    # until that picker closes, so querying the QAction afterwards blocks UIA.
    $metadata=@{id=$element.Current.AutomationId;name=$element.Current.Name;type=$element.Current.ControlType.ProgrammaticName}
    if($posted){$metadata.postedGesture=[Pc11GuiUia]::PostControl($element,$script:guiPid)}
    else{[Pc11GuiUia]::Act($element)}
    Log 'accessible-action' $metadata
}
function Wait-PreviewPanel([int]$seconds=120) {
    $clock=[Diagnostics.Stopwatch]::StartNew()
    while($clock.Elapsed.TotalSeconds -lt $seconds) {
        if($script:gui.HasExited){throw 'Verified GUI exited before the experiment preview appeared.'}
        $panel=[Pc11GuiUia]::Find($script:guiPid,'piezoPreviewPanel','','',$true)
        if($null -ne $panel -and $panel.Current.IsEnabled) { return $panel }
        Start-Sleep -Milliseconds 100
    }
    Dump 'preview-panel-timeout'
    throw 'Visible, enabled piezoPreviewPanel did not appear after Run.'
}
function Get-ModelTreeRoots {
    $items=@([Pc11GuiUia]::Elements($script:guiPid) | Where-Object {
        try {
            $c=$_.Current
            $c.ControlType.ProgrammaticName -eq 'ControlType.TreeItem' -and $c.AutomationId -match '\.ModelTree$'
        } catch { $false }
    })
    if (!$items.Count) { return @() }
    $left=($items | ForEach-Object { $_.Current.BoundingRectangle.Left } | Measure-Object -Minimum).Minimum
    @($items | Where-Object { [Math]::Abs($_.Current.BoundingRectangle.Left-$left) -lt 1 } | Sort-Object { $_.Current.BoundingRectangle.Top })
}
function Handle-FirstRun {
    $firstRun=[Pc11GuiUia]::Find($script:guiPid,'','First run','Window',$true)
    if ($null -eq $firstRun) { return }
    Dump 'first-run-controls'
    $usage=Wait-Control '' 'Send usage information' 'CheckBox'
    [Pc11GuiUia]::ToggleOff($usage); Log 'first-run-usage-off' @{}
    Action '' 'Start' 'Button'; Start-Sleep -Milliseconds 500
}
function Picker([string]$title,[string]$path,[bool]$folder=$false,[bool]$save=$false,[bool]$cancel=$false) {
    # Native Qt file dialogs can keep the Qt UIA Invoke provider inside modal
    # exec(). Read and operate the exact owned native controls without querying
    # that blocked Qt owner and without sending global keyboard/pointer input.
    $clock=[Diagnostics.Stopwatch]::StartNew(); $native=0; $dialog=$null
    $nativePreference=Get-Variable -Name nativePickersOnly -Scope Script -ErrorAction SilentlyContinue
    $nativeOnly=($null -ne $nativePreference -and $nativePreference.Value)
    while ($clock.Elapsed.TotalSeconds -lt 30) {
        $native=[Pc11GuiNative]::Picker($script:guiPid,$title)
        if ($native -ne 0) { break }
        if(!$nativeOnly){$dialog=[Pc11GuiUia]::Find($script:guiPid,'',$title,'Window',$true)}
        if ($null -ne $dialog) { break }
        Start-Sleep -Milliseconds 100
    }
    if ($native -ne 0) {
        $script:activeNativePicker=$native
        if($cancel){throw 'Qt-picker cancellation probe encountered a native dialog; preserve its actual controls.'}
        $nativeControls=[Pc11GuiNative]::Controls($native,$script:guiPid)
        [IO.File]::WriteAllText((Join-Path $out ('picker-'+$native+'-controls.json')),($nativeControls | ConvertTo-Json -Depth 6),$utf8)
        # PrintWindow can hold the shell dialog's UI thread during its next
        # native text query. Retain the control snapshot without capturing here.
        [Pc11GuiNative]::CompletePicker($native,$script:guiPid,$title,$path,$folder,$save)
        $script:activeNativePicker=0
        Log 'picker-commit' @{title=$title;path=$path;method='owned-native-controls';windowHandle=$native}
        return
    }
    if ($null -eq $dialog) { throw 'Owned native or Qt file picker unavailable.' }
    [Pc11GuiUia]::CheckDialog($dialog,$script:guiPid,$title)
    Dump ('qt-picker-'+$dialog.Current.NativeWindowHandle+'-controls')
    $edit=[Pc11GuiUia]::Descendant($dialog,'fileNameEdit','','Edit')
    if ($null -ne $edit) {
        [Pc11GuiUia]::CheckDialog($dialog,$script:guiPid,$title)
        if($edit.Current.ProcessId -ne $script:guiPid -or !$edit.Current.IsEnabled -or $edit.Current.IsOffscreen){throw 'Owned Qt filename edit is not enabled/visible.'}
        [Pc11GuiUia]::SetText($edit,$path)
        if([Pc11GuiUia]::Value($edit) -ne $path){throw 'Owned Qt filename edit readback differs.'}
    }
    else { Dump 'picker-unsupported'; throw 'File picker has no known accessible filename edit.' }
    if ($cancel) { $buttonNames=@('Cancel','&Cancel') }
    elseif ($folder) { $buttonNames=@('Choose','&Choose','Select Folder','Open','&Open') }
    elseif ($save) { $buttonNames=@('Save','&Save') } else { $buttonNames=@('Open','&Open') }
    $button=$null
    foreach ($label in $buttonNames) {
        $button=[Pc11GuiUia]::Descendant($dialog,'',$label,'Button')
        if ($null -ne $button) { break }
    }
    if ($null -eq $button) { Dump 'picker-button-unsupported'; throw 'Picker commit button unavailable.' }
    [Pc11GuiUia]::CheckDialog($dialog,$script:guiPid,$title)
    if($button.Current.ProcessId -ne $script:guiPid -or !$button.Current.IsEnabled -or $button.Current.IsOffscreen){throw 'Owned Qt picker commit button is not enabled/visible.'}
    $proof=[Pc11GuiUia]::PostControl($button,$script:guiPid)
    $pickerAction=if($cancel){'picker-cancel'}else{'picker-commit'}
    Log $pickerAction @{title=$title;path=$path;method='owned-qt-uia';valueReadbackVerified=$true;postedGesture=$proof}
}
function Capture([string]$name) {
    $visibleWindow=$script:main
    if ($null -ne $script:measurementWindow) { $visibleWindow=$script:measurementWindow }
    if ($visibleWindow.Current.IsOffscreen) { throw 'Measured window is not visible.' }
    $exposure=Assert-WindowExposure $name
    [Pc11GuiNative]::Capture($visibleWindow.Current.NativeWindowHandle,$script:guiPid,(Join-Path $out ($name+'.png')))
    Log 'owned-window-capture' @{method='WindowFromPoint-exposure-then-PrintWindow';windowHandle=$visibleWindow.Current.NativeWindowHandle;name=$name;timingEndOverride=$false;exposure=$exposure}
}
function Assert-WindowExposure([string]$name) {
    $visibleWindow=$script:main
    if ($null -ne $script:measurementWindow) { $visibleWindow=$script:measurementWindow }
    $exposure=[Pc11GuiUia]::WindowExposure($visibleWindow.Current.NativeWindowHandle,$script:main.Current.NativeWindowHandle,$script:guiPid)
    Log 'owned-window-exposure' @{name=$name;targetHandle=$exposure.TargetHandle;targetProcessId=$exposure.TargetProcessId;targetTitle=$exposure.TargetTitle;targetClass=$exposure.TargetClass;mainWindowHandle=$exposure.MainWindowHandle;mainOwnsTarget=$exposure.MainOwnsTarget;allSamplePointsOwned=$exposure.AllSamplePointsOwned;hitTargets=$exposure.Hits}
    if (!$exposure.AllSamplePointsOwned) { throw "Measured window is covered or not exposed at one or more sampled points: $name" }
    return $exposure
}
function Arrange-MeasurementWindows([string]$reason) {
    $mainHandle=$script:main.Current.NativeWindowHandle
    $workbenchHandle=$script:measurementWindow.Current.NativeWindowHandle
    Log 'measurement-window-layout-before' @{reason=$reason;main=[Pc11GuiNative]::Details($mainHandle,$script:guiPid);workbench=[Pc11GuiNative]::Details($workbenchHandle,$script:guiPid)}
    $layout=[Pc11GuiNative]::ArrangeMeasurementWindows($mainHandle,$workbenchHandle,$script:guiPid)
    Log 'measurement-window-layout' @{reason=$reason;targetIsMain=$layout.TargetIsMain;mainHandle=$layout.MainHandle;mainBounds=$layout.MainBounds;mainWidth=$layout.MainWidth;workbenchHandle=$layout.WorkbenchHandle;workbenchBounds=$layout.WorkbenchBounds;workbenchWidth=$layout.WorkbenchWidth;workbenchHeight=$layout.WorkbenchHeight;workAreaBounds=$layout.WorkAreaBounds;totalWidth=$layout.TotalWidth;noOverlap=$layout.NoOverlap;withinWorkArea=$layout.WithinWorkArea;sameMonitor=$layout.SameMonitor;layoutValid=$layout.LayoutValid;noActivate=$true}
    if (!$layout.LayoutValid) { throw "Separate measurement windows did not fit side-by-side in the monitor work area: $reason" }
}
function Wait-Event([string]$operation,[string]$terminal,[int]$seconds) {
    $clock=[Diagnostics.Stopwatch]::StartNew()
    while ($clock.Elapsed.TotalSeconds -lt $seconds) {
        if ($script:gui.HasExited) { throw 'GUI exited during measurement.' }
        Assert-SamplerAlive
        if ([IO.File]::Exists($liveEvents)) {
            $lines=Get-Content -LiteralPath $liveEvents
            foreach ($line in $lines) {
                try { $event=$line | ConvertFrom-Json } catch { continue }
                if ($null -ne $event -and $null -ne $event.PSObject.Properties['operation'] -and $null -ne $event.PSObject.Properties['event'] -and $event.operation -eq $operation -and $event.event -eq $terminal) { return $event }
            }
        }
        Start-Sleep -Milliseconds 100
    }
    Dump 'event-timeout'; throw "No product $operation/$terminal event after $seconds seconds."
}
function Assert-SamplerAlive {
    if ($null -ne $sampler) {
        $sampler.Refresh()
        if ($sampler.HasExited) { throw 'Whole-tree sampler exited before measurement completion; preserve this audit as invalid.' }
    }
}
function Observe-BaselineMemory {
    Assert-SamplerAlive
    $rows=@(Import-Csv -LiteralPath (Join-Path $out 'working-set\working-set.csv'))
    $peak=($rows | Measure-Object -Property working_set_bytes -Maximum).Maximum
    Log 'observed-baseline-memory' @{maximumWorkingSetBytes=[long]$peak;sampleCount=$rows.Count;informational=$true}
}
function Reveal-WorkflowControls {
    $controls=@('piezoRun')|ForEach-Object{Wait-Control $_ '' 'Button' 30 $false}
    # New production layouts have an outer scroll area around the whole
    # workbench. Keep the saved Run control visible; Qt IsOffscreen alone does
    # not account for this clipping. Confirm only exists after Run preview.
    $viewport=[Pc11GuiUia]::Find($script:guiPid,'Application.controlDock.controlWorkflowScroll.qt_scrollarea_viewport','','',$true)
    $bar=[Pc11GuiUia]::Find($script:guiPid,'Application.controlDock.controlWorkflowScroll.qt_scrollarea_vcontainer.QScrollBar','','ScrollBar',$true)
    if($null -ne $viewport -and $null -ne $bar) {
        $range=[Windows.Automation.RangeValuePattern]$bar.GetCurrentPattern([Windows.Automation.RangeValuePattern]::Pattern)
        $top=($controls|ForEach-Object{$_.Current.BoundingRectangle.Top}|Measure-Object -Minimum).Minimum
        $bottom=($controls|ForEach-Object{$_.Current.BoundingRectangle.Bottom}|Measure-Object -Maximum).Maximum
        $clip=$viewport.Current.BoundingRectangle
        $dpi=[Pc11GuiNative]::Details($script:measurementWindow.Current.NativeWindowHandle,$script:guiPid).Dpi
        $scale=[Math]::Max(1,$dpi/96.0)
        $delta=(($top+$bottom)/2-($clip.Top+$clip.Height/2))/$scale
        $range.SetValue([Math]::Max($range.Current.Minimum,[Math]::Min($range.Current.Maximum,$range.Current.Value+$delta)))
        Start-Sleep -Milliseconds 100
    }
    $visibility=@($controls|ForEach-Object{@{id=$_.Current.AutomationId;enabled=$_.Current.IsEnabled;fullyVisible=[Pc11GuiUia]::FullyVisible($_);physicalBounds=$_.Current.BoundingRectangle.ToString()}})
    Log 'saved-run-physical-visibility' $visibility
    if(@($visibility|Where-Object{!$_.fullyVisible}).Count){throw 'Saved Run control is not fully visible in the actual owned viewport.'}
}
$sampler=$null; $script:gui=$null; $script:measurementWindow=$null; $crossVersionProfileUpgrade=$false
$script:physicalRamSamplerStarted=$false
try {
    if ($Launch) {
        if ($GuiProcessId -ne 0) { throw 'Launch and attach PID are mutually exclusive.' }
        $info=New-Object Diagnostics.ProcessStartInfo
        $info.FileName=$exe; $info.WorkingDirectory=$install; $info.UseShellExecute=$false
        $info.RedirectStandardOutput=$true; $info.RedirectStandardError=$true
        $info.EnvironmentVariables['QT_QPA_PLATFORM']='windows'
        $info.EnvironmentVariables['QT_PLUGIN_PATH']=Join-Path $install 'bin\plugins'
        if (![IO.Directory]::Exists($info.EnvironmentVariables['QT_PLUGIN_PATH'])) { $info.EnvironmentVariables['QT_PLUGIN_PATH']=Join-Path $install 'bin' }
        $info.EnvironmentVariables['RANGE_PROJECT_WORKSPACE']=$workspace
        $reusedProfile=$false
        if ($spec.PSObject.Properties['reuseProfileFrom']) {
            $upgradeAuthority=$null
            $priorPath=Absolute $spec.reuseProfileFrom; $prior=Get-Content -LiteralPath $priorPath -Raw | ConvertFrom-Json
            $priorProfile=if($prior.PSObject.Properties['profile']){Absolute $prior.profile}else{Join-Path (Absolute $prior.output) 'application-profile'}
            if ($profile -ine $priorProfile -or $prior.workspace -ine $workspace) { throw 'Owned prior profile/workspace mismatch.' }
            $priorExe=Absolute $prior.executable
            $priorSha=[string]$prior.executableSha256
            $sameBinary=($priorExe -ieq $exe -and $priorSha -ieq $spec.executableSha256)
            $upgradeProfile=$false
            if ($spec.PSObject.Properties['allowProfileBinaryUpgrade']) {
                if ($spec.allowProfileBinaryUpgrade -isnot [bool]) { throw 'allowProfileBinaryUpgrade must be a JSON boolean.' }
                $upgradeProfile=($spec.allowProfileBinaryUpgrade -eq $true)
            }
            if (!$sameBinary -and !$upgradeProfile) { throw 'Owned prior profile/executable/SHA mismatch; cross-version reuse requires allowProfileBinaryUpgrade.' }
            if (![IO.File]::Exists($priorExe) -or (Get-FileHash -LiteralPath $priorExe -Algorithm SHA256).Hash -ine $priorSha) { throw 'Prior manifest executable is missing or its SHA-256 changed.' }
            if (!$sameBinary) {
                foreach ($field in @('sourceModel','sourceSha256','sourceFingerprint','sourceManifest','sourceManifestSha256','approvedRecipe','approvedRecipeSha256','approvedInputDate','family','steps','expectedAcceptedRecords','dtS')) {
                    if (!$prior.PSObject.Properties[$field] -or !$spec.PSObject.Properties[$field]) { throw "Profile upgrade identity field missing: $field" }
                    if (@('sourceModel','sourceManifest','approvedRecipe') -contains $field) {
                        if ((Absolute ([string]$prior.$field)) -ine (Absolute ([string]$spec.$field))) { throw "Profile upgrade identity path mismatch: $field" }
                    } elseif ([string]$prior.$field -ine [string]$spec.$field) { throw "Profile upgrade identity mismatch: $field" }
                }
                if ($Mode -eq 'Measure' -and $null -ne $result) {
                    $profileUpgradeResult=$result
                } elseif ($Mode -eq 'Full' -and $null -eq $result -and $null -ne $resultSearch -and $spec.PSObject.Properties['profileUpgradeResultModel']) {
                    $profileUpgradeResult=Absolute ([string]$spec.profileUpgradeResultModel)
                } else {
                    throw 'Cross-version profile reuse requires Measure with its explicit registered result, or Full with resultSearchRoot and a separate profileUpgradeResultModel authority.'
                }
                $jobsRoot=Join-Path $profile 'application-data\local-data\native-jobs'
                if (![IO.Directory]::Exists($jobsRoot)) { throw 'Prior profile has no native job registry.' }
                $matchingJobs=@()
                foreach ($jobFile in @(Get-ChildItem -LiteralPath $jobsRoot -Filter 'job.json' -Recurse -File)) {
                    try { $jobRecord=Get-Content -LiteralPath $jobFile.FullName -Raw -Encoding UTF8 | ConvertFrom-Json } catch { continue }
                    if ($jobRecord.resultArtifactRef -and (Absolute ([string]$jobRecord.resultArtifactRef)) -ieq $profileUpgradeResult) {
                        $matchingJobs+=@([pscustomobject]@{path=$jobFile.FullName;record=$jobRecord})
                    }
                }
                if ($matchingJobs.Count -ne 1) { throw 'Profile upgrade requires exactly one registered job for the explicit result.' }
                $jobPath=Absolute $matchingJobs[0].path; $jobRecord=$matchingJobs[0].record
                if ((Absolute ([string]$jobRecord.nativeJobRecordRef)) -ine $jobPath -or $jobRecord.state -ne 'succeeded' -or !$jobRecord.provenanceFrozen -or !$jobRecord.resultArtifactVerified -or !$jobRecord.resultManifestVerified -or !$jobRecord.resultSourceBindingVerified -or !$jobRecord.resultReadbackComplete -or !$jobRecord.nativeResultCatalogAvailable) { throw 'Registered result lacks frozen, verified terminal authority.' }
                if ($jobRecord.nativeCheckpointContext.sourceSha256 -ine $spec.sourceSha256 -or $jobRecord.nativeCheckpointContext.modelSha256 -ine $spec.sourceSha256) { throw 'Registered result source/model binding differs from the approved source.' }
                $trace=$jobRecord.nativeResultProvenance.executionContext.nativeAcceptedTrace
                if ($null -eq $trace -or $trace.checkpointDirectory -ine $jobRecord.nativeCheckpointDirectory -or $trace.context.sourceSha256 -ine $spec.sourceSha256) { throw 'Registered result lacks a matching native accepted-trace context.' }
                if (!(Test-Path -LiteralPath $profileUpgradeResult -PathType Leaf) -or (Get-FileHash -LiteralPath $profileUpgradeResult -Algorithm SHA256).Hash -ine $jobRecord.resultArtifactSha256) { throw 'Registered result artifact SHA-256 verification failed.' }
                $resultManifest=Absolute ([string]$jobRecord.resultManifestRef)
                if (!(Test-Path -LiteralPath $resultManifest -PathType Leaf) -or (Get-FileHash -LiteralPath $resultManifest -Algorithm SHA256).Hash -ine $jobRecord.resultManifestSha256) { throw 'Registered result manifest SHA-256 verification failed.' }
                $boundArtifact=Absolute ([string]$jobRecord.resultSourceBinding.resolvedSourcePath)
                if (!(Test-Path -LiteralPath $boundArtifact -PathType Leaf) -or (Get-FileHash -LiteralPath $boundArtifact -Algorithm SHA256).Hash -ine $jobRecord.resultSourceBinding.savedArtifactHash) { throw 'Registered resolved result payload SHA-256 verification failed.' }
                $recipePath=Absolute ([string]$spec.approvedRecipe)
                if (!(Test-Path -LiteralPath $recipePath -PathType Leaf) -or (Get-FileHash -LiteralPath $recipePath -Algorithm SHA256).Hash -ine $spec.approvedRecipeSha256 -or $prior.approvedRecipeSha256 -ine $spec.approvedRecipeSha256) { throw 'Approved recipe file/hash is not bound to both manifests.' }
                $recipe=Get-Content -LiteralPath $recipePath -Raw -Encoding UTF8 | ConvertFrom-Json
                if ($recipe.family -ine $spec.family -or [string]$recipe.steps -ine [string]$spec.steps -or [double]$recipe.dt -ne [double]$spec.dtS) { throw 'Approved recipe family/step/time-grid identity mismatch.' }
                $registeredManifest=Get-Content -LiteralPath $resultManifest -Raw -Encoding UTF8 | ConvertFrom-Json
                $identity=$registeredManifest.nativeAcceptedIdentity
                $expectedResultFamily=if($spec.family -eq 'axialRod'){'axial'}else{'layered'}
                if ($identity.family -ine $expectedResultFamily -or [int]$identity.targetSteps -ne [int]$recipe.steps -or [double]$identity.dtS -ne [double]$recipe.dt -or $identity.sourceFingerprint -ine $spec.sourceFingerprint -or $identity.backendControlSha256 -ine $jobRecord.nativeCheckpointContext.controlSha256) { throw 'Registered native result identity differs from the approved family, source, control backend, or time grid.' }
                if ($registeredManifest.inputPackageSha256 -ine $jobRecord.inputPackageSha256 -or $jobRecord.nativeCheckpointContext.packageSha256 -ine $jobRecord.inputPackageSha256 -or $registeredManifest.control.controllerId -ine ("internal-native-piezo/"+[string]$recipe.controller.kind)) { throw 'Registered package/controller identity differs from the approved run configuration.' }
                if ([string]::IsNullOrWhiteSpace([string]$identity.configurationIdentity)) { throw 'Registered native result lacks a configuration identity.' }
                $admissionText=& $python '-B' (Join-Path $PSScriptRoot 'pc11_gui_terminal.py') '--profile' $profile '--gui-pid' ([string]$jobRecord.ownerPid)
                $admissionCode=$LASTEXITCODE
                try { $admission=$admissionText | ConvertFrom-Json } catch { throw 'Prior profile native registry/checkpoint admission did not return valid JSON.' }
                if ($admissionCode -ne 0 -or $admission.status -ne 'TERMINAL_NATIVE_20000' -or $admission.jobId -ne $jobRecord.jobId -or [int]$admission.acceptedSeq -ne [int]$recipe.steps -or (Absolute ([string]$admission.resultModel)) -ine $profileUpgradeResult) { throw 'Prior profile native registry chain or terminal checkpoint admission failed.' }
                $upgradeAuthority=@{priorManifest=$priorPath;profile=$profile;priorExecutable=$priorExe;priorExecutableSha256=$priorSha;currentExecutable=$exe;currentExecutableSha256=$spec.executableSha256;jobRecord=$jobPath;jobId=$jobRecord.jobId;registryAdmission=$admission;result=$profileUpgradeResult;resultSha256=$jobRecord.resultArtifactSha256;resultManifest=$resultManifest;resultSearchRoot=$resultSearch;approvedRecipe=$recipePath;approvedRecipeSha256=$spec.approvedRecipeSha256;noRegistryCopies=$true;runSubmitted=$false}
                $crossVersionProfileUpgrade=$true
            }
            $priorActions=Get-Content -LiteralPath (Join-Path $prior.output 'uia-actions.jsonl') -Encoding UTF8 | ForEach-Object { $_|ConvertFrom-Json }
            $old=@($priorActions|Where-Object action -eq 'verified-gui')
            if($old.Count -ne 1){throw 'Prior profile lacks one verified owned GUI launch.'}
            if ($old[0].detail.image -ine $priorExe -or $old[0].detail.sha256 -ine $priorSha) { throw 'Prior verified GUI identity differs from its manifest.' }
            $profileLaunch=@($priorActions|Where-Object{$_.action -eq 'isolated-application-profile' -and [int]$_.guiPid -eq [int]$old[0].guiPid -and $_.detail.path -ieq $priorProfile -and $_.detail.environment -eq 'RANGE_APPLICATION_PROFILE_DIR' -and $_.detail.launch -eq $true})
            if($profileLaunch.Count -ne 1){throw 'Prior verified GUI lacks one matching profile/environment launch receipt.'}
            $quitFile=Join-Path $prior.output ('normal-close-'+$old[0].guiPid+'.json')
            if(![IO.File]::Exists($quitFile)){throw 'Prior owned GUI lacks its normal Quit receipt.'}
            $quit=Get-Content -LiteralPath $quitFile -Raw|ConvertFrom-Json
            if($quit.status -ne 'OWNED_GUI_CLOSED_NORMALLY' -or [int]$quit.pid -ne [int]$old[0].guiPid -or $quit.creationUtc -ne $old[0].detail.creationUtc -or $quit.executableSha256 -ine $priorSha){throw 'Prior normal Quit receipt identity mismatch.'}
            $oldProcess=Get-Process -Id ([int]$old[0].guiPid) -ErrorAction SilentlyContinue
            if($null -ne $oldProcess -and $oldProcess.StartTime.ToUniversalTime().ToString('o') -eq $old[0].detail.creationUtc){throw 'Prior owned GUI has not closed; profile reuse is forbidden.'}
            if(![IO.Directory]::Exists($profile)){throw 'Owned existing profile missing.'}
            if ($null -ne $upgradeAuthority) { Log 'owned-profile-upgrade-reuse-authority' $upgradeAuthority }
            $reusedProfile=$true
            if ($sameBinary) { Log 'owned-profile-reuse-authority' @{priorManifest=$priorPath;profile=$profile;priorGuiPid=$old[0].guiPid;noRegistryCopies=$true} }
        }
        if (!$reusedProfile -and [IO.Directory]::Exists($profile) -and @(Get-ChildItem -LiteralPath $profile -Force).Count -gt 0) { throw 'Launch requires a fresh empty application profile or verified own-profile reuse.' }
        [void][IO.Directory]::CreateDirectory($profile)
        $info.EnvironmentVariables['RANGE_APPLICATION_PROFILE_DIR']=$profile
        $info.EnvironmentVariables['RANGE_PIEZO_PERFORMANCE_WORKSPACE']=if ($liveEvents -eq $events) {$out} else {$workspace}
        $info.EnvironmentVariables['RANGE_PIEZO_PERFORMANCE_EVENTS']=$liveEvents
        $script:gui=[Diagnostics.Process]::Start($info)
        [Pc11GuiUia]::RedirectLogs($script:gui,(Join-Path $out 'product.stdout.txt'),(Join-Path $out 'product.stderr.txt'))
    } else {
        if ($GuiProcessId -le 0) { throw 'Provide -Launch or a verified -GuiProcessId.' }
        if ($Mode -notin @('Inspect','Setup')) { throw 'Measurement must launch with the explicit performance environment of this run.' }
        $script:gui=Get-Process -Id $GuiProcessId
    }
    $script:guiPid=$script:gui.Id
    if ([IO.Path]::GetFullPath($script:gui.MainModule.FileName) -ine $exe) { throw 'Live GUI executable mismatch.' }
    Log 'verified-gui' @{image=$exe;sha256=$spec.executableSha256;discoveryOnly=[bool]$DiscoveryOnly;creationUtc=$script:gui.StartTime.ToUniversalTime().ToString('o')}
    Log 'isolated-application-profile' @{path=$profile;environment='RANGE_APPLICATION_PROFILE_DIR';launch=[bool]$Launch}
    # Launch sampler immediately, before opening the project/source or requesting providers.
    if (!$DiscoveryOnly -and $Mode -ne 'Inspect') {
        [Pc11PhysicalRamSampler]::Start((Join-Path $out 'physical-ram'),50)
        $script:physicalRamSamplerStarted=$true
        $samplerArgs=@('-B',(Join-Path $PSScriptRoot 'pc11_working_set.py'),'--root-pid',[string]$script:guiPid,'--expected-root-image',$exe,'--allowed-root',$install,'--allowed-root',(Split-Path $python),'--output',(Join-Path $out 'working-set'),'--stop-file',$stop)
        if ($spec.PSObject.Properties['allowedDescendantRoots']) {
            foreach ($root in $spec.allowedDescendantRoots) {
                $allowed=Absolute $root
                if (![IO.Directory]::Exists($allowed) -or $allowed -eq [IO.Path]::GetPathRoot($allowed)) { throw 'Allowed compute image directory is missing or excessively broad.' }
                $samplerArgs+=@('--allowed-root',$allowed)
            }
        }
        if ($spec.PSObject.Properties['allowedDescendantImages']) {
            foreach ($item in $spec.allowedDescendantImages) {
                $allowed=Absolute $item.path
                if (![IO.File]::Exists($allowed) -or (Get-FileHash -LiteralPath $allowed).Hash -ine $item.sha256) { throw 'Exact approved descendant image SHA mismatch.' }
                $samplerArgs+=@('--allowed-root',$allowed)
                Log 'approved-exact-descendant-image' @{path=$allowed;sha256=$item.sha256}
            }
        }
        foreach ($arg in $samplerArgs) { if ($arg.Contains('"')) { throw 'Quotes in sampler arguments are unsupported.' } }
        $sampler=Start-Process -FilePath $python -ArgumentList (($samplerArgs | ForEach-Object {'"'+$_+'"'}) -join ' ') -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $out 'sampler.stdout.txt') -RedirectStandardError (Join-Path $out 'sampler.stderr.txt')
        $deadline=[DateTime]::UtcNow.AddSeconds(10)
        $samples=Join-Path $out 'working-set\working-set.csv'
        while (![IO.File]::Exists($samples) -or @(Get-Content -LiteralPath $samples).Count -lt 3) {
            if ($sampler.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw 'Baseline sampler did not become ready.' }
            Start-Sleep -Milliseconds 100
        }
        Log 'baseline-sampler-ready' @{samplerPid=$sampler.Id}
    }
    $clock=[Diagnostics.Stopwatch]::StartNew()
    do {
        Handle-FirstRun
        $script:gui.Refresh()
        if ($script:gui.MainWindowHandle -ne [IntPtr]::Zero -and $script:gui.MainWindowTitle -match 'Range FEA') {
            $candidate=[Windows.Automation.AutomationElement]::FromHandle($script:gui.MainWindowHandle)
            $menu=$candidate.FindFirst([Windows.Automation.TreeScope]::Descendants,(New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::NameProperty,'File')))
            if ($null -ne $menu) { break }
        }
        Start-Sleep -Milliseconds 100
    } while ($clock.Elapsed.TotalSeconds -lt 60)
    if ($script:gui.MainWindowHandle -eq [IntPtr]::Zero) { throw 'GUI has no actual top-level window.' }
    $script:main=[Windows.Automation.AutomationElement]::FromHandle($script:gui.MainWindowHandle)
    [Pc11GuiNative]::FitWindow($script:gui.MainWindowHandle,$script:guiPid)
    $mainBounds=@{kind='main';physicalBounds=[Pc11GuiNative]::Bounds($script:gui.MainWindowHandle,$script:guiPid);noActivate=$true}
    Log 'owned-window-bounds' $mainBounds
    $spec | Add-Member -MemberType NoteProperty -Name physicalWindowBounds -Value @($mainBounds) -Force
    Start-Sleep -Milliseconds 500
    Handle-FirstRun
    Start-Sleep -Milliseconds 500
    Dump 'startup-controls'; Capture 'startup'
    if ($Mode -eq 'Inspect') { Log 'inspect-complete' @{guiAcceptanceVerified=$false}; return }
    Action '' 'File' 'MenuItem'; Action '' 'Project' 'MenuItem'; Action 'openProjectWorkspace' 'Open project workspace…' 'MenuItem' 30 $true
    Picker 'Open project workspace' $workspace $true
    if($Mode -eq 'PickerProbe') {
        Action '' 'Open model' 'Button' 30 $true
        Picker 'Open model' $source $false $false $true
        Observe-BaselineMemory
        Log 'qt-picker-probe-complete' @{sourceReadbackOnly=$source;sourceSha256=(Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant();runSubmitted=$false;guiAcceptanceVerified=$false}
        Capture 'qt-picker-cancelled'; return
    }
    Action '' 'Piezo control' 'TabItem' 90
    # Float the workbench when Qt exposes its dock control. Some supported
    # layouts already present it as a separate, full-height visible window.
    $workbenchLayoutKind='floating-workbench'
    $floatButton=[Pc11GuiUia]::Find($script:guiPid,'Application.MainWindow.controlDock.qt_dockwidget_floatbutton','','Button',$false)
    if($null -ne $floatButton){
        Action 'Application.MainWindow.controlDock.qt_dockwidget_floatbutton' '' 'Button'
    }else{
        $visibleDock=[Pc11GuiUia]::Find($script:guiPid,'Application.controlDock','','Window',$false)
        if($null -eq $visibleDock -or ![Pc11GuiUia]::FullyVisible($visibleDock)){throw 'Qt dock float control is absent and no fully visible workbench window is available.'}
        $dockRect=$visibleDock.Current.BoundingRectangle
        if($dockRect.Width -lt 1000 -or $dockRect.Height -lt 700){throw 'Qt dock float control is absent and the existing workbench window is too small for measurement.'}
        $workbenchLayoutKind='already-visible-workbench'
        Log 'workbench-already-visible' @{physicalBounds=[Pc11GuiNative]::Bounds($visibleDock.Current.NativeWindowHandle,$script:guiPid);details=[Pc11GuiNative]::Details($visibleDock.Current.NativeWindowHandle,$script:guiPid);floatControlAvailable=$false}
    }
    Start-Sleep -Milliseconds 500
    $run=Wait-Control 'piezoRun' 'Run saved experiment' 'Button' 30 $false
    $script:measurementWindow=[Pc11GuiUia]::NativeWindowFor($run)
    Arrange-MeasurementWindows 'workbench-discovered'
    $mainAfterFloat=@{kind='main-after-float';physicalBounds=[Pc11GuiNative]::Bounds($script:main.Current.NativeWindowHandle,$script:guiPid);details=[Pc11GuiNative]::Details($script:main.Current.NativeWindowHandle,$script:guiPid);noActivate=$true}
    Log 'owned-window-bounds' $mainAfterFloat
    $spec.physicalWindowBounds+=@($mainAfterFloat)
    $workbenchBounds=@{kind=$workbenchLayoutKind;physicalBounds=[Pc11GuiNative]::Bounds($script:measurementWindow.Current.NativeWindowHandle,$script:guiPid);noActivate=$true}
    Log 'owned-window-bounds' $workbenchBounds
    $spec.physicalWindowBounds+=@($workbenchBounds)
    Log 'visible-layout-before-source-open' @{main=[Pc11GuiNative]::Details($script:main.Current.NativeWindowHandle,$script:guiPid);workbench=[Pc11GuiNative]::Details($script:measurementWindow.Current.NativeWindowHandle,$script:guiPid);sourceOpened=$false;hidden=$false}
    $existingModelRoots=@(Get-ModelTreeRoots)
    $existingModelRootSnapshot=@($existingModelRoots | ForEach-Object { $c=$_.Current; @{name=$c.Name;bounds=$c.BoundingRectangle.ToString()} })
    if($existingModelRoots.Count -gt 1){throw "Multiple model roots already exist in the saved profile; refusing to append another source. roots=$($existingModelRootSnapshot | ConvertTo-Json -Compress)"}
    $sourceOpened=$false
    if($existingModelRoots.Count -eq 0){
        Action '' 'Open model' 'Button' 30 $true
        Picker 'Open model' $source
        Action '' 'Piezo control' 'TabItem' 90
        # Qt may replace the native main HWND when the first OpenGL model opens.
        $script:main=Wait-Control 'Application.MainWindow' '' 'Window' 30 $true
        Arrange-MeasurementWindows 'main-after-source-open'
        $rootDeadline=[DateTime]::UtcNow.AddSeconds(10); $allRoots=@()
        do {
            $allRoots=@(Get-ModelTreeRoots)
            if($allRoots.Count -gt 1){$rootSnapshot=@($allRoots | ForEach-Object { $c=$_.Current; @{name=$c.Name;bounds=$c.BoundingRectangle.ToString()} }); throw "Opening the approved source produced multiple model roots; roots=$($rootSnapshot | ConvertTo-Json -Compress)"}
            if($allRoots.Count -eq 1){break}
            Start-Sleep -Milliseconds 100
        } while([DateTime]::UtcNow -lt $rootDeadline)
        if($allRoots.Count -ne 1){throw 'Opening the approved source did not produce exactly one model root.'}
        $sourceOpened=$true
    }else{
        $allRoots=@($existingModelRoots)
    }
    if($allRoots.Count -ne 1){throw "Expected one model root for the approved source; count=$($allRoots.Count)."}
    $sourceRootName=$allRoots[0].Current.Name
    $sourceRootTop=$allRoots[0].Current.BoundingRectangle.Top
    if([string]::IsNullOrWhiteSpace($sourceRootName)){throw 'Saved approved source model root has no readable name.'}
    switch($spec.family){
        'axialRod' {$expectedFamilyText='axial rod / cable'}
        'planarBeam' {$expectedFamilyText='planar layered beam'}
        'flatPlate' {$expectedFamilyText='flat layered plate'}
        default {throw "Unknown model family for exact workbench status: $($spec.family)"}
    }
    $sourceStatusPattern='^Current model: '+[regex]::Escape($sourceRootName)+' \| revision [0-9]+ \| '+[regex]::Escape($expectedFamilyText)+'$'
    $currentModelRootSnapshot=@($allRoots | ForEach-Object { $c=$_.Current; @{name=$c.Name;bounds=$c.BoundingRectangle.ToString()} })
    $selectionMethod='already-selected'
    if(![Pc11GuiUia]::IsSelected($allRoots[0])) {
        $selectionMethod='selected-by-harness'
        [Pc11GuiUia]::Act($allRoots[0])
    }
    $selectionClock=[Diagnostics.Stopwatch]::StartNew(); $selectedRoot=$null; $sourceStatus=$null
    do {
        $selectedRoots=@(Get-ModelTreeRoots)
        if($selectedRoots.Count -ne 1){throw "Model root count changed during selection; count=$($selectedRoots.Count)."}
        $selectedRoot=$selectedRoots[0]
        if($selectedRoot.Current.Name -cne $sourceRootName){throw "Selected model root changed during selection; expected='$sourceRootName', actual='$($selectedRoot.Current.Name)'."}
        $sourceStatus=[Pc11GuiUia]::Find($script:guiPid,'piezoSource','','Text',$false)
        if($null -ne $sourceStatus -and [Pc11GuiUia]::IsSelected($selectedRoot) -and $sourceStatus.Current.Name -cmatch $sourceStatusPattern){break}
        Start-Sleep -Milliseconds 100
    } while($selectionClock.Elapsed.TotalSeconds -lt 10)
    $selectedRoots=@(Get-ModelTreeRoots)
    if($selectedRoots.Count -ne 1 -or $selectedRoots[0].Current.Name -cne $sourceRootName -or ![Pc11GuiUia]::IsSelected($selectedRoots[0])){throw 'Approved saved model root is not uniquely selected in the Model tree.'}
    $sourceStatus=[Pc11GuiUia]::Find($script:guiPid,'piezoSource','','Text',$false)
    $sourceStatusText=if($null -ne $sourceStatus){[string]$sourceStatus.Current.Name}else{'<missing>'}
    if($sourceStatusText -cnotmatch $sourceStatusPattern){throw "Selecting the approved source did not produce the exact model/revision/family workbench status. expectedPattern='$sourceStatusPattern'; actual='$sourceStatusText'"}
    Log 'accepted-source-model-selected' @{source=$source;sourceSha256=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant();sourceOpened=$sourceOpened;existingRootCount=$existingModelRootSnapshot.Count;existingRoots=$existingModelRootSnapshot;currentRootCount=$currentModelRootSnapshot.Count;currentRoots=$currentModelRootSnapshot;selectedRootName=$sourceRootName;selectedRootIndex=0;selectedRootTop=$sourceRootTop;selectionItemIsSelected=$true;selectionMethod=$selectionMethod;selectedRootAutomationId=$selectedRoots[0].Current.AutomationId;selectedRootControlType=$selectedRoots[0].Current.ControlType.ProgrammaticName;workbenchReadbackPattern=$sourceStatusPattern;workbenchReadback=$sourceStatus.Current.Name;runSubmitted=$false}
    $sourceBounds=@{kind='main-with-source';physicalBounds=[Pc11GuiNative]::Bounds($script:main.Current.NativeWindowHandle,$script:guiPid);details=[Pc11GuiNative]::Details($script:main.Current.NativeWindowHandle,$script:guiPid);noActivate=$true}
    Log 'owned-window-bounds' $sourceBounds
    $spec.physicalWindowBounds+=@($sourceBounds)
    [IO.File]::WriteAllText((Join-Path $out 'manifest.json'),($spec | ConvertTo-Json -Depth 12),$utf8)
    if ($Mode -eq 'Setup') {
        Reveal-WorkflowControls
        Dump 'source-open-controls'; Capture 'source-open'
        Log 'installed-source-setup-complete' @{source=$source;sourceSha256=(Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant();guiAcceptanceVerified=$false;runSubmitted=$false}
        return
    }
    if ($Mode -in @('Run','Full')) {
        if ($Mode -eq 'Full') {
            Action '' 'Excitation / initial state' 'TabItem'
            $stepsControl=Wait-Control 'piezoSteps' '' '' 30 $false
            if([int]([Pc11GuiUia]::Value($stepsControl)) -ne 20000){throw 'Actual saved GUI experiment does not target20000 FE steps.'}
            if((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ine $spec.sourceSha256){throw 'Approved source changed before Run.'}
            Log 'approved-source-and-target-before-run' @{path=$source;sha256=(Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant();actualGuiSteps=20000}
            Reveal-WorkflowControls
            Log 'run-viewport-prepared-before-run' @{runVisible=[Pc11GuiUia]::FullyVisible((Wait-Control 'piezoRun' '' 'Button' 30 $false))}
        }
        Observe-BaselineMemory
        Action 'piezoRun' 'Run saved experiment' 'Button' 120
        Observe-BaselineMemory
        $preview=Wait-PreviewPanel 120
        Log 'run-preview-visible-enabled' @{automationId=$preview.Current.AutomationId;name=$preview.Current.Name;enabled=$preview.Current.IsEnabled;offscreen=$preview.Current.IsOffscreen}
        Action 'piezoConfirm' 'Confirm preview' 'Button' 120
        Log 'saved-run-plus-confirm' @{activeOperations=2}; Capture 'run-confirmed'; Dump 'run-controls'
        if ($Mode -eq 'Run') { return }
        $waitSeconds=3600
        if ($spec.PSObject.Properties['runWaitSeconds']) { $waitSeconds=[int]$spec.runWaitSeconds }
        $deadline=[DateTime]::UtcNow.AddSeconds($waitSeconds)
        while ($true) {
            if ($script:gui.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw 'Registered native result did not appear after actual Run + Confirm.' }
            Assert-SamplerAlive
            $terminalText=& $python '-B' (Join-Path $PSScriptRoot 'pc11_gui_terminal.py') '--profile' $profile '--gui-pid' ([string]$script:guiPid)
            $terminalCode=$LASTEXITCODE; $terminal=$terminalText | ConvertFrom-Json
            if ($terminalCode -eq 0) {
                if ($null -ne $result -and $result -ine $terminal.resultModel) { throw 'Observed terminal result differs from explicit expected resultModel.' }
                if($crossVersionProfileUpgrade -and $Mode -eq 'Full'){
                    $newResult=Absolute ([string]$terminal.resultModel)
                    $oldResult=Absolute ([string]$upgradeAuthority.result)
                    if($terminal.jobId -eq $upgradeAuthority.jobId -or $newResult -ieq $oldResult){throw 'Cross-version Full did not produce a new registered job/result distinct from its profile-upgrade authority.'}
                    $resultPrefix=$resultSearch.TrimEnd('\')+'\'
                    if(!$newResult.StartsWith($resultPrefix,[StringComparison]::OrdinalIgnoreCase)){throw 'New current-binary result escaped the bounded resultSearchRoot.'}
                    $priorResultMatches=@($priorResults | Where-Object { [IO.Path]::GetFullPath($_) -ieq $newResult })
                    if($priorResultMatches.Count -ne 0){throw 'New current-binary result path existed before Run + Confirm.'}
                    Log 'cross-version-full-new-result' @{priorJobId=$upgradeAuthority.jobId;priorAuthorityResult=$oldResult;newJobId=$terminal.jobId;newResult=$newResult;resultSearchRoot=$resultSearch;priorPathMatchCount=$priorResultMatches.Count}
                }
                $result=$terminal.resultModel
                $spec | Add-Member -MemberType NoteProperty -Name resultModel -Value $result -Force
                [IO.File]::WriteAllText((Join-Path $out 'manifest.json'),($spec | ConvertTo-Json -Depth 12),$utf8)
                [IO.File]::WriteAllText((Join-Path $out 'terminal-native-job.json'),($terminal | ConvertTo-Json -Depth 20),$utf8)
                Log 'terminal-native-20000' $terminal
                if($Mode -eq 'Full') {
                    Log 'native-result-auto-bound' @{jobId=$terminal.jobId;projectId=$terminal.projectId;resultModel=$terminal.resultModel;resultArtifactSha256=$terminal.resultArtifactSha256;acceptedSeq=$terminal.acceptedSeq;stateFingerprintSha256=$terminal.stateFingerprintSha256;sourceFingerprint=$spec.sourceFingerprint;binding='product-automatic-native-job-result'}
                }
                break
            }
            if ($terminalCode -ne 1 -or $null -eq $terminal -or $terminal.status -ne 'WAITING') { throw "Native terminal admission failed: $terminalText" }
            Start-Sleep -Milliseconds 1000
        }
        # No provider or record replay occurs here; the product's upcoming cold
        # GUI request independently certifies the complete committed trace.
    }
    if($null -ne $readRelease) {
        Log 'timed-read-awaiting-coordinator' @{releaseFile=$readRelease;providerOpened=$false;reason='serialize actual plot/CSV after competing science/retest load'}
        while(![IO.File]::Exists($readRelease)) {
            if($script:gui.HasExited){throw 'GUI exited while awaiting timed-read coordination.'}
            Assert-SamplerAlive
            Start-Sleep -Milliseconds 1000
        }
        if([IO.File]::ReadAllText($readRelease).Trim() -ne 'RELEASE_ACTUAL_TIMED_READ'){throw 'Invalid timed-read coordination token.'}
        Log 'timed-read-coordinator-released' @{releaseFile=$readRelease;providerOpened=$false}
    }
    $paint=Wait-Event 'plot' 'canvas_painted' 180
    $paintExposure=Assert-WindowExposure 'certified-plot'
    Log 'product-canvas-painted' @{event=$paint;windowExposure=$paintExposure}; Capture 'certified-plot'; Dump 'plot-controls'
    Action 'piezoExportAcceptedCsv' 'Export selected accepted native records to CSV…' 'Button' 30 $true
    Picker 'Export certified accepted samples' $csv $false $true
    $commit=Wait-Event 'csv' 'csv_committed' 180
    $commitExposure=Assert-WindowExposure 'csv-committed'
    Log 'product-csv-committed' @{event=$commit;windowExposure=$commitExposure}; Capture 'csv-committed'
} catch {
    Log 'harness-failed' @{error=$_.Exception.ToString();guiAcceptanceVerified=$false}
    if ($script:guiPid -gt 0) {
        try {
            if ($script:activeNativePicker -ne 0) {
                [Pc11GuiNative]::Capture($script:activeNativePicker,$script:guiPid,(Join-Path $out 'failure-native-picker.png'))
            } else { Dump 'failure-controls'; Capture 'failure' }
        } catch { }
    }
    throw
} finally {
    if ($script:physicalRamSamplerStarted) {
        try {
            $physicalRamSummary=[Pc11PhysicalRamSampler]::Stop() | ConvertFrom-Json
            Log 'physical-ram-sampler-stopped' $physicalRamSummary
        } catch {
            Log 'physical-ram-sampler-stop-failed' @{error=$_.Exception.ToString()}
        }
    }
    # Stop only this read-only sampler through its documented stop-file protocol.
    if ($null -ne $sampler) {
        [IO.File]::WriteAllText($stop,'stop',$utf8)
        if (!$sampler.WaitForExit(10000)) { Log 'sampler-stop-pending' @{samplerPid=$sampler.Id} }
    }
    if ($liveEvents -ne $events -and [IO.File]::Exists($liveEvents)) { Copy-Item -LiteralPath $liveEvents -Destination $events }
    # Leave the product open for inspection; do not kill processes or discard dialogs.
    if ($script:guiPid -gt 0) { Log 'gui-left-open' @{pid=$script:guiPid} }
}
if ($Mode -in @('Measure','Full')) {
    & $python '-B' (Join-Path $PSScriptRoot 'pc11_gui_verify.py') '--evidence' $out
    if ($LASTEXITCODE -ne 0) { throw 'PC11 evidence did not meet the frozen contract. See qualification.json.' }
}
