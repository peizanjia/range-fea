$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'contract-common-v43.ps1')

if (-not ('V43NativeLaunchBoundary' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class V43NativeLaunchBoundary {
    [StructLayout(LayoutKind.Sequential)] public struct BasicLimitInformation {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }
    [StructLayout(LayoutKind.Sequential)] public struct IoCounters {
        public ulong ReadOperationCount;
        public ulong WriteOperationCount;
        public ulong OtherOperationCount;
        public ulong ReadTransferCount;
        public ulong WriteTransferCount;
        public ulong OtherTransferCount;
    }
    [StructLayout(LayoutKind.Sequential)] public struct ExtendedLimitInformation {
        public BasicLimitInformation BasicLimitInformation;
        public IoCounters IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] public struct StartupInfo {
        public uint cb;
        public string reserved;
        public string desktop;
        public string title;
        public uint x, y, xSize, ySize, xCountChars, yCountChars, fillAttribute, flags;
        public ushort showWindow;
        public ushort reserved2Size;
        public IntPtr reserved2;
        public IntPtr stdInput, stdOutput, stdError;
    }
    [StructLayout(LayoutKind.Sequential)] public struct ProcessInformation {
        public IntPtr process;
        public IntPtr thread;
        public uint processId;
        public uint threadId;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateJobObject(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetInformationJobObject(IntPtr job, int informationClass, ref ExtendedLimitInformation information, uint length);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool IsProcessInJob(IntPtr process, IntPtr job, out bool result);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool TerminateProcess(IntPtr process, uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool TerminateJobObject(IntPtr job, uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", EntryPoint = "CreateProcessW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateProcess(string application, StringBuilder commandLine, IntPtr processAttributes,
        IntPtr threadAttributes, bool inheritHandles, uint creationFlags, IntPtr environment,
        string currentDirectory, ref StartupInfo startup, out ProcessInformation information);

    public static IntPtr CreateKillOnCloseJob(string name) {
        IntPtr job = CreateJobObject(IntPtr.Zero, name);
        if (job == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateJobObject failed.");
        ExtendedLimitInformation limits = new ExtendedLimitInformation();
        limits.BasicLimitInformation.LimitFlags = 0x2000 | 0x0008;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        if (!SetInformationJobObject(job, 9, ref limits, (uint)Marshal.SizeOf(typeof(ExtendedLimitInformation)))) {
            int error = Marshal.GetLastWin32Error(); CloseHandle(job);
            throw new Win32Exception(error, "KILL_ON_JOB_CLOSE and one-process limit setup failed.");
        }
        return job;
    }

    public static ProcessInformation StartSuspended(string application, string commandLine, string currentDirectory) {
        StartupInfo startup = new StartupInfo();
        startup.cb = (uint)Marshal.SizeOf(typeof(StartupInfo));
        ProcessInformation information;
        bool created = CreateProcess(application, new StringBuilder(commandLine), IntPtr.Zero, IntPtr.Zero,
            false, 0x00000004 | 0x08000000, IntPtr.Zero, currentDirectory, ref startup, out information);
        if (!created) throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateProcessW(CREATE_SUSPENDED|CREATE_NO_WINDOW) failed.");
        return information;
    }
}
'@
}

function ConvertTo-V43PowerShellLiteral([string]$Value) {
    return "'" + $Value.Replace("'", "''") + "'"
}

function New-V43EncodedPowerShellCommand([string]$PowerShellPath, [string]$ScriptText) {
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($ScriptText))
    return '"' + $PowerShellPath + '" -NoProfile -NoLogo -NonInteractive -ExecutionPolicy Bypass -EncodedCommand ' + $encoded
}

function Invoke-V43NativeLaunchBoundaryFixture([string]$FixtureEvidenceRoot, [string]$CandidateIdentitySha256) {
    if ($PSVersionTable.PSEdition -ne 'Desktop' -or $PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
        throw 'The v43 native launch fixture requires Windows PowerShell 5.1.'
    }
    if ($CandidateIdentitySha256 -notmatch '^[0-9a-f]{64}$') { throw 'Native fixture requires the v43 synthetic candidate identity.' }
    $root = [IO.Path]::GetFullPath($FixtureEvidenceRoot)
    if (-not [IO.Directory]::Exists($root)) { throw 'Native fixture evidence root must already exist.' }
    $powerShell = [IO.Path]::GetFullPath((Join-Path $PSHOME 'powershell.exe'))
    if (-not [IO.File]::Exists($powerShell)) { throw 'Windows PowerShell executable was not found under PSHOME.' }
    $runRoot = Join-Path $root ('native-boundary-v43-' + [Guid]::NewGuid().ToString('N'))
    if (-not (Test-V43PathUnderRoot $runRoot $root)) { throw 'Native boundary fixture escaped its evidence root.' }
    [void][IO.Directory]::CreateDirectory($runRoot)
    $rows = New-Object 'System.Collections.Generic.List[object]'

    # The withheld-assignment case must remain suspended until the retained process is terminated.
    $withheldMarker = Join-Path $runRoot 'withheld-assignment.marker'
    $withheldScript = '[IO.File]::WriteAllText(' + (ConvertTo-V43PowerShellLiteral $withheldMarker) + ',' + (ConvertTo-V43PowerShellLiteral $CandidateIdentitySha256) + '); exit 0'
    $withheld = [V43NativeLaunchBoundary]::StartSuspended($powerShell, (New-V43EncodedPowerShellCommand $powerShell $withheldScript), $runRoot)
    try {
        if (-not [V43NativeLaunchBoundary]::TerminateProcess($withheld.process, 125)) { throw 'Could not terminate the unassigned suspended fixture worker.' }
        if ([V43NativeLaunchBoundary]::WaitForSingleObject($withheld.process, 5000) -ne 0) { throw 'Unassigned suspended fixture worker did not terminate.' }
        $exitCode = [uint32]::MaxValue
        if (-not [V43NativeLaunchBoundary]::GetExitCodeProcess($withheld.process, [ref]$exitCode) -or $exitCode -eq 259) {
            throw 'Unassigned worker exit state could not be read after the wait.'
        }
        if ([IO.File]::Exists($withheldMarker)) { throw 'Unassigned worker executed before the Job boundary.' }
        [void]$rows.Add([pscustomobject]@{ case = 'withheld-assignment'; status = 'PASS'; createdSuspended = $true; assigned = $false; resumed = $false; executed = $false; exitCode = $exitCode })
    }
    finally {
        [void][V43NativeLaunchBoundary]::TerminateProcess($withheld.process, 125)
        [void][V43NativeLaunchBoundary]::CloseHandle($withheld.thread)
        [void][V43NativeLaunchBoundary]::CloseHandle($withheld.process)
    }

    # The positive fixture is assigned to the exact Job and verified before its initial resume.
    $positiveMarker = Join-Path $runRoot 'assigned-worker.marker'
    $positiveScript = '[IO.File]::WriteAllText(' + (ConvertTo-V43PowerShellLiteral $positiveMarker) + ',' + (ConvertTo-V43PowerShellLiteral $CandidateIdentitySha256) + '); exit 0'
    $positiveJob = [V43NativeLaunchBoundary]::CreateKillOnCloseJob('Local\RangeV43-' + [Guid]::NewGuid().ToString('N'))
    $positive = $null
    $positiveProcessHandle = [IntPtr]::Zero
    $positiveThreadHandle = [IntPtr]::Zero
    try {
        $positive = [V43NativeLaunchBoundary]::StartSuspended($powerShell, (New-V43EncodedPowerShellCommand $powerShell $positiveScript), $runRoot)
        $positiveProcessHandle = $positive.process
        $positiveThreadHandle = $positive.thread
        if (-not [V43NativeLaunchBoundary]::AssignProcessToJobObject($positiveJob, $positive.process)) { throw 'Could not assign the suspended v43 fixture worker to its Job.' }
        $member = $false
        if (-not [V43NativeLaunchBoundary]::IsProcessInJob($positive.process, $positiveJob, [ref]$member) -or -not $member) {
            throw 'Suspended v43 fixture worker is not proven in the exact Job.'
        }
        $suspendCount = [V43NativeLaunchBoundary]::ResumeThread($positive.thread)
        if ($suspendCount -ne 1) { throw "Initial fixture worker suspend count was not one: $suspendCount" }
        if (-not [V43NativeLaunchBoundary]::CloseHandle($positiveThreadHandle)) { throw 'Could not close the assigned v43 fixture worker thread handle.' }
        $positiveThreadHandle = [IntPtr]::Zero
        if ([V43NativeLaunchBoundary]::WaitForSingleObject($positiveProcessHandle, 10000) -ne 0) { throw 'Assigned v43 fixture worker did not exit within the bounded wait.' }
        $exitCode = [uint32]::MaxValue
        if (-not [V43NativeLaunchBoundary]::GetExitCodeProcess($positiveProcessHandle, [ref]$exitCode) -or $exitCode -ne 0 -or $exitCode -eq 259) {
            throw "Assigned v43 fixture worker did not report exit code zero: $exitCode"
        }
        if (-not [IO.File]::Exists($positiveMarker) -or [IO.File]::ReadAllText($positiveMarker) -cne $CandidateIdentitySha256) {
            throw 'Assigned v43 fixture worker did not publish the candidate-bound inert marker.'
        }
        [void]$rows.Add([pscustomobject]@{ case = 'assigned-before-resume'; status = 'PASS'; createdSuspended = $true; assigned = $true; exactJobMembershipVerified = $true; initialSuspendCount = $suspendCount; executed = $true; exitCode = $exitCode; markerCandidateIdentitySha256 = [IO.File]::ReadAllText($positiveMarker) })
    }
    finally {
        try {
            if ($null -ne $positive -and $positiveProcessHandle -ne [IntPtr]::Zero) {
                try { [void][V43NativeLaunchBoundary]::TerminateProcess($positiveProcessHandle, 125) }
                finally {
                    if ([V43NativeLaunchBoundary]::CloseHandle($positiveProcessHandle)) { $positiveProcessHandle = [IntPtr]::Zero }
                }
            }
        }
        finally {
            try {
                if ($positiveThreadHandle -ne [IntPtr]::Zero) {
                    if ([V43NativeLaunchBoundary]::CloseHandle($positiveThreadHandle)) { $positiveThreadHandle = [IntPtr]::Zero }
                }
            }
            finally {
                [void][V43NativeLaunchBoundary]::CloseHandle($positiveJob)
            }
        }
    }

    # KILL_ON_JOB_CLOSE is checked with a second harmless child that stays alive after its marker.
    $drainMarker = Join-Path $runRoot 'kill-on-close.marker'
    $drainScript = '[IO.File]::WriteAllText(' + (ConvertTo-V43PowerShellLiteral $drainMarker) + ',' + (ConvertTo-V43PowerShellLiteral $CandidateIdentitySha256) + '); while ($true) { Start-Sleep -Seconds 1 }'
    $drainJob = [V43NativeLaunchBoundary]::CreateKillOnCloseJob('Local\RangeV43-' + [Guid]::NewGuid().ToString('N'))
    $drain = $null
    $drainProcessHandle = [IntPtr]::Zero
    $drainThreadHandle = [IntPtr]::Zero
    try {
        $drain = [V43NativeLaunchBoundary]::StartSuspended($powerShell, (New-V43EncodedPowerShellCommand $powerShell $drainScript), $runRoot)
        $drainProcessHandle = $drain.process
        $drainThreadHandle = $drain.thread
        if (-not [V43NativeLaunchBoundary]::AssignProcessToJobObject($drainJob, $drain.process)) { throw 'Could not assign kill-on-close fixture worker.' }
        $member = $false
        if (-not [V43NativeLaunchBoundary]::IsProcessInJob($drain.process, $drainJob, [ref]$member) -or -not $member) { throw 'Kill-on-close worker lacks exact Job membership.' }
        $suspendCount = [V43NativeLaunchBoundary]::ResumeThread($drain.thread)
        if ($suspendCount -ne 1) { throw 'Kill-on-close worker initial suspend count was not one.' }
        if (-not [V43NativeLaunchBoundary]::CloseHandle($drainThreadHandle)) { throw 'Could not close the kill-on-close fixture worker thread handle.' }
        $drainThreadHandle = [IntPtr]::Zero
        $timer = [Diagnostics.Stopwatch]::StartNew()
        while (-not [IO.File]::Exists($drainMarker)) {
            if ($timer.Elapsed.TotalSeconds -gt 10) { throw 'Kill-on-close worker did not publish its inert marker.' }
            Start-Sleep -Milliseconds 25
        }
        if ([IO.File]::ReadAllText($drainMarker) -cne $CandidateIdentitySha256) { throw 'Kill-on-close marker differs from the v43 fixture identity.' }
        $activeExitCode = [uint32]::MaxValue
        if ([V43NativeLaunchBoundary]::WaitForSingleObject($drainProcessHandle, 0) -ne 258 -or
            -not [V43NativeLaunchBoundary]::GetExitCodeProcess($drainProcessHandle, [ref]$activeExitCode) -or $activeExitCode -ne 259) {
            throw 'Kill-on-close fixture worker was not observed alive immediately before Job close.'
        }
        if (-not [V43NativeLaunchBoundary]::CloseHandle($drainJob)) { throw 'Could not close the kill-on-close fixture Job handle.' }
        $drainJob = [IntPtr]::Zero
        if ([V43NativeLaunchBoundary]::WaitForSingleObject($drainProcessHandle, 10000) -ne 0) { throw 'KILL_ON_JOB_CLOSE did not drain the fixture worker.' }
        $exitCode = [uint32]::MaxValue
        if (-not [V43NativeLaunchBoundary]::GetExitCodeProcess($drainProcessHandle, [ref]$exitCode) -or $exitCode -eq 259) {
            throw "KILL_ON_JOB_CLOSE returned before the fixture worker had a terminal exit code: $exitCode"
        }
        [void]$rows.Add([pscustomobject]@{ case = 'kill-on-job-close'; status = 'PASS'; exactJobMembershipVerified = $true; killOnJobClose = $true; observedAliveBeforeClose = $true; drained = $true; exitCode = $exitCode })
    }
    finally {
        try {
            if ($drainProcessHandle -ne [IntPtr]::Zero) {
                try {
                    if ($drainJob -ne [IntPtr]::Zero) { [void][V43NativeLaunchBoundary]::TerminateJobObject($drainJob, 125) }
                    [void][V43NativeLaunchBoundary]::TerminateProcess($drainProcessHandle, 125)
                }
                finally {
                    if ([V43NativeLaunchBoundary]::CloseHandle($drainProcessHandle)) { $drainProcessHandle = [IntPtr]::Zero }
                }
            }
        }
        finally {
            try {
                if ($drainThreadHandle -ne [IntPtr]::Zero) {
                    if ([V43NativeLaunchBoundary]::CloseHandle($drainThreadHandle)) { $drainThreadHandle = [IntPtr]::Zero }
                }
            }
            finally {
                if ($drainJob -ne [IntPtr]::Zero) {
                    if ([V43NativeLaunchBoundary]::CloseHandle($drainJob)) { $drainJob = [IntPtr]::Zero }
                }
            }
        }
    }
    return [pscustomobject]@{
        status = 'PASS'
        candidateManifestIdentitySha256 = $CandidateIdentitySha256.ToLowerInvariant()
        fixtureOnly = $true
        cases = @($rows.ToArray())
        evidenceRoot = $runRoot
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
}
