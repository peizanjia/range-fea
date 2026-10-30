[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or
    $PSVersionTable.PSVersion.Major -ne 5 -or
    $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'The v43 root-bootstrap fixture test requires Windows PowerShell 5.1.'
}

$script:PrepRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$script:BootstrapPath = [IO.Path]::GetFullPath((Join-Path $script:PrepRoot 'runtime\root-bootstrap-v43.ps1'))
if (-not [IO.File]::Exists($script:BootstrapPath)) { throw "Root bootstrap script is missing: $script:BootstrapPath" }

$script:RequiredStaticTestIds = @(
    'source_preflight', 'source_freeze', 'package_tree', 'module_boundary_build', 'module_boundary_source',
    'watchdog_preflight', 'save_predicate_preflight', 'path_shadow', 'root_seal', 'save_predicate',
    'watchdog_contract', 'sampler_arguments', 'supervisor', 'worker_handshake', 'native_launch_boundary',
    'parser', 'finalize_integrity', 'source_static_tests', 'root_bootstrap'
)
$script:ProductActionNames = @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')
$script:TestRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('range-pc11-v43-root-bootstrap-' + [Guid]::NewGuid().ToString('N'))))
[void][IO.Directory]::CreateDirectory($script:TestRoot)

function Get-Sha256File([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}

function Get-Sha256Bytes([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Write-JsonFile([string]$Path, $Value) {
    $json = $Value | ConvertTo-Json -Depth 30
    [IO.File]::WriteAllText($Path, $json + "`n", (New-Object Text.UTF8Encoding($false)))
    return Get-Sha256File $Path
}

function ConvertTo-JsonString([string]$Value) { return [string]($Value | ConvertTo-Json -Compress -ErrorAction Stop) }

function Get-CandidateIdentity($Manifest) {
    $parts = @(
        '{"candidateVersion":"v43"',
        ',"installRoot":' + (ConvertTo-JsonString ([string]$Manifest.installBinding.installRoot)),
        ',"inventoryReportSha256":' + (ConvertTo-JsonString ([string]$Manifest.installBinding.inventoryReportSha256)),
        ',"releaseBuildReceiptSha256":' + (ConvertTo-JsonString ([string]$Manifest.sourceAndAuthorityPins.releaseBuildReceiptSha256)),
        ',"rootCommit":' + (ConvertTo-JsonString ([string]$Manifest.sourceAndAuthorityPins.rootCommit)),
        ',"schema":"range.pc11-fresh-run-v43-admission-candidate/v1"',
        ',"sourceFreezeReceiptSha256":' + (ConvertTo-JsonString ([string]$Manifest.sourceAndAuthorityPins.sourceFreezeReceiptSha256)),
        ',"sourceTreeSha256":' + (ConvertTo-JsonString ([string]$Manifest.sourceAndAuthorityPins.sourceTreeSha256)) + '}'
    )
    return Get-Sha256Bytes ((New-Object Text.UTF8Encoding($false, $true)).GetBytes([string]::Join('', $parts)))
}

function Quote-WindowsArgument([string]$Value) {
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $builder = New-Object Text.StringBuilder
    [void]$builder.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') { $slashes++; continue }
        if ($character -eq '"') {
            [void]$builder.Append(('\' * (2 * $slashes + 1)))
            [void]$builder.Append('"')
            $slashes = 0
            continue
        }
        if ($slashes -gt 0) { [void]$builder.Append(('\' * $slashes)); $slashes = 0 }
        [void]$builder.Append($character)
    }
    if ($slashes -gt 0) { [void]$builder.Append(('\' * (2 * $slashes))) }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Quote-PowerShellLiteral([string]$Value) {
    return "'" + $Value.Replace("'", "''") + "'"
}

function Invoke-Bootstrap([string[]]$Arguments, [string]$ModulePathOverride = $null) {
    $powerShellPath = [IO.Path]::GetFullPath((Join-Path $PSHOME 'powershell.exe'))
    $quoted = New-Object 'System.Collections.Generic.List[string]'
    foreach ($argument in $Arguments) { [void]$quoted.Add((Quote-WindowsArgument ([string]$argument))) }
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $powerShellPath
    $info.Arguments = [string]::Join(' ', $quoted.ToArray())
    $info.WorkingDirectory = $script:TestRoot
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    if (-not [string]::IsNullOrWhiteSpace($ModulePathOverride)) {
        $info.EnvironmentVariables['PSModulePath'] = $ModulePathOverride
    }
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    try {
        if (-not $process.Start()) { throw 'Could not start a PowerShell fixture child.' }
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $stdout = $process.StandardOutput.ReadToEnd()
        $process.WaitForExit()
        return [pscustomobject]@{ exitCode = [int]$process.ExitCode; stdout = $stdout; stderr = $stderrTask.Result }
    }
    finally { $process.Dispose() }
}

function New-FakeUtilityModule([string]$Name) {
    $moduleRoot = Join-Path $script:TestRoot ($Name + '-fake-modules')
    $moduleDirectory = Join-Path $moduleRoot 'Microsoft.PowerShell.Utility'
    [void][IO.Directory]::CreateDirectory($moduleDirectory)
    $loadedMarker = Join-Path $script:TestRoot ($Name + '-fake-module-loaded.txt')
    $commandMarker = Join-Path $script:TestRoot ($Name + '-fake-command-executed.txt')
    $moduleSource = @(
        '[IO.File]::WriteAllText(' + (Quote-PowerShellLiteral $loadedMarker) + ", 'FAKE_UTILITY_MODULE_LOADED')"
        'function ConvertFrom-Json { [IO.File]::WriteAllText(' + (Quote-PowerShellLiteral $commandMarker) + ", 'FAKE_CONVERTFROMJSON_EXECUTED'); throw 'fake ConvertFrom-Json executed' }"
        'function ConvertTo-Json { [IO.File]::WriteAllText(' + (Quote-PowerShellLiteral $commandMarker) + ", 'FAKE_CONVERTTOJSON_EXECUTED'); throw 'fake ConvertTo-Json executed' }"
        "Export-ModuleMember -Function 'ConvertFrom-Json', 'ConvertTo-Json'"
    )
    [IO.File]::WriteAllText((Join-Path $moduleDirectory 'Microsoft.PowerShell.Utility.psm1'), [string]::Join("`r`n", $moduleSource), (New-Object Text.UTF8Encoding($false)))
    $manifest = @"
@{
    RootModule = 'Microsoft.PowerShell.Utility.psm1'
    ModuleVersion = '1.0.0'
    GUID = '6e41e4f6-9522-4424-96d0-738ed767c7a5'
    FunctionsToExport = @('ConvertFrom-Json', 'ConvertTo-Json')
    CmdletsToExport = @()
    VariablesToExport = @()
    AliasesToExport = @()
}
"@
    $manifestPath = Join-Path $moduleDirectory 'Microsoft.PowerShell.Utility.psd1'
    [IO.File]::WriteAllText($manifestPath, $manifest, (New-Object Text.UTF8Encoding($false)))
    return [pscustomobject]@{
        moduleRoot = $moduleRoot
        manifestPath = $manifestPath
        loadedMarker = $loadedMarker
        commandMarker = $commandMarker
    }
}

function Initialize-NativeOutputDirectoryProbe([string]$BootstrapSource) {
    $guardSourceMatch = [regex]::Match($BootstrapSource, '(?s)\$directoryGuardSource\s*=\s*@''\r?\n(?<code>.*?)\r?\n''@')
    if (-not $guardSourceMatch.Success) { throw 'Could not extract the root bootstrap native output-directory guard source for its reparse probe.' }
    $utilityManifest = Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1'
    Microsoft.PowerShell.Core\Import-Module -Name $utilityManifest -Global -ErrorAction Stop
    Microsoft.PowerShell.Utility\Add-Type -TypeDefinition $guardSourceMatch.Groups['code'].Value -Language CSharp -ErrorAction Stop

    $probeSource = @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace RangeFea.V43.Testing {
    public static class NativeReparseAttempt {
        private const uint GenericWrite = 0x40000000;
        private const uint FileShareRead = 0x00000001;
        private const uint FileShareWrite = 0x00000002;
        private const uint FileShareDelete = 0x00000004;
        private const uint OpenExisting = 3;
        private const uint FileFlagBackupSemantics = 0x02000000;
        private const uint FileFlagOpenReparsePoint = 0x00200000;
        private const uint FsctlSetReparsePoint = 0x000900A4;
        private const uint FsctlDeleteReparsePoint = 0x000900AC;
        private const uint IoReparseTagMountPoint = 0xA0000003;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeFileHandle CreateFile(
            string path, uint desiredAccess, uint shareMode, IntPtr securityAttributes,
            uint creationDisposition, uint flagsAndAttributes, IntPtr templateFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool DeviceIoControl(
            SafeFileHandle device, uint controlCode, byte[] inputBuffer, uint inputBufferSize,
            byte[] outputBuffer, uint outputBufferSize, out uint bytesReturned, IntPtr overlapped);

        private static byte[] BuildMountPointData(string targetPath) {
            string fullTarget = Path.GetFullPath(targetPath);
            string substituteName = @"\??\" + fullTarget;
            byte[] substituteBytes = Encoding.Unicode.GetBytes(substituteName);
            byte[] printBytes = Encoding.Unicode.GetBytes(fullTarget);
            int dataLength = 8 + substituteBytes.Length + 2 + printBytes.Length + 2;
            byte[] buffer = new byte[8 + dataLength];
            Buffer.BlockCopy(BitConverter.GetBytes(IoReparseTagMountPoint), 0, buffer, 0, 4);
            Buffer.BlockCopy(BitConverter.GetBytes((ushort)dataLength), 0, buffer, 4, 2);
            Buffer.BlockCopy(BitConverter.GetBytes((ushort)0), 0, buffer, 6, 2);
            Buffer.BlockCopy(BitConverter.GetBytes((ushort)0), 0, buffer, 8, 2);
            Buffer.BlockCopy(BitConverter.GetBytes((ushort)substituteBytes.Length), 0, buffer, 10, 2);
            Buffer.BlockCopy(BitConverter.GetBytes((ushort)(substituteBytes.Length + 2)), 0, buffer, 12, 2);
            Buffer.BlockCopy(BitConverter.GetBytes((ushort)printBytes.Length), 0, buffer, 14, 2);
            Buffer.BlockCopy(substituteBytes, 0, buffer, 16, substituteBytes.Length);
            Buffer.BlockCopy(printBytes, 0, buffer, 16 + substituteBytes.Length + 2, printBytes.Length);
            return buffer;
        }

        private static bool TryDeleteMountPoint(SafeFileHandle handle) {
            byte[] deleteBuffer = new byte[8];
            Buffer.BlockCopy(BitConverter.GetBytes(IoReparseTagMountPoint), 0, deleteBuffer, 0, 4);
            uint bytesReturned;
            return DeviceIoControl(handle, FsctlDeleteReparsePoint, deleteBuffer, (uint)deleteBuffer.Length,
                null, 0, out bytesReturned, IntPtr.Zero);
        }

        public static bool TryAttachMountPointWhileGuarded(
            SafeFileHandle guardHandle, string directoryPath, string targetPath,
            out bool guardHandleWasMutated, out int competingOpenError, out bool competingIoctlAttempted,
            out int competingIoctlError) {
            if (guardHandle == null || guardHandle.IsInvalid)
                throw new InvalidOperationException("The output-directory guard handle is invalid.");
            byte[] reparseData = BuildMountPointData(targetPath);
            bool changed = false;
            guardHandleWasMutated = false;
            competingOpenError = 0;
            competingIoctlAttempted = false;
            competingIoctlError = 0;
            uint bytesReturned;

            using (SafeFileHandle writer = CreateFile(directoryPath, GenericWrite,
                FileShareRead | FileShareWrite | FileShareDelete, IntPtr.Zero, OpenExisting,
                FileFlagBackupSemantics | FileFlagOpenReparsePoint, IntPtr.Zero)) {
                if (writer == null || writer.IsInvalid) {
                    competingOpenError = Marshal.GetLastWin32Error();
                    if (writer != null) writer.Dispose();
                    return false;
                }
                competingIoctlAttempted = true;
                changed = DeviceIoControl(writer, FsctlSetReparsePoint, reparseData,
                    (uint)reparseData.Length, null, 0, out bytesReturned, IntPtr.Zero);
                competingIoctlError = changed ? 0 : Marshal.GetLastWin32Error();
                return changed;
            }
        }

        public static bool TryDeleteMountPointAtPath(string directoryPath, out int error) {
            error = 0;
            using (SafeFileHandle writer = CreateFile(directoryPath, GenericWrite,
                FileShareRead | FileShareWrite | FileShareDelete, IntPtr.Zero, OpenExisting,
                FileFlagBackupSemantics | FileFlagOpenReparsePoint, IntPtr.Zero)) {
                if (writer == null || writer.IsInvalid) {
                    error = Marshal.GetLastWin32Error();
                    if (writer != null) writer.Dispose();
                    return false;
                }
                if (TryDeleteMountPoint(writer)) return true;
                error = Marshal.GetLastWin32Error();
                return false;
            }
        }

        public static bool TryAttachMountPointThroughGuard(SafeFileHandle guardHandle, string targetPath, out int error) {
            byte[] reparseData = BuildMountPointData(targetPath);
            uint bytesReturned;
            bool changed = DeviceIoControl(guardHandle, FsctlSetReparsePoint, reparseData,
                (uint)reparseData.Length, null, 0, out bytesReturned, IntPtr.Zero);
            error = changed ? 0 : Marshal.GetLastWin32Error();
            return changed;
        }

        public static bool TryDeleteMountPointThroughGuard(SafeFileHandle guardHandle, out int error) {
            byte[] deleteBuffer = new byte[8];
            Buffer.BlockCopy(BitConverter.GetBytes(IoReparseTagMountPoint), 0, deleteBuffer, 0, 4);
            uint bytesReturned;
            bool deleted = DeviceIoControl(guardHandle, FsctlDeleteReparsePoint, deleteBuffer,
                (uint)deleteBuffer.Length, null, 0, out bytesReturned, IntPtr.Zero);
            error = deleted ? 0 : Marshal.GetLastWin32Error();
            return deleted;
        }
    }
}
'@
    Microsoft.PowerShell.Utility\Add-Type -TypeDefinition $probeSource -Language CSharp -ErrorAction Stop
}

function Invoke-NativeOutputDirectoryReparseProbe([string]$Name, [string]$BootstrapSource) {
    Initialize-NativeOutputDirectoryProbe $BootstrapSource
    $probeRoot = Join-Path $script:TestRoot $Name
    $guardedDirectory = Join-Path $probeRoot 'guarded-output-parent'
    $targetDirectory = Join-Path $probeRoot 'protected-source-target'
    [void][IO.Directory]::CreateDirectory($guardedDirectory)
    [void][IO.Directory]::CreateDirectory($targetDirectory)
    $sourceSentinel = Join-Path $targetDirectory 'source-sentinel.txt'
    [IO.File]::WriteAllText($sourceSentinel, 'SYNTHETIC_SOURCE_UNCHANGED')
    $sentinelShaBefore = Get-Sha256File $sourceSentinel
    $guardHandle = [RangeFea.V43.OutputDirectoryGuard]::OpenDirectory($guardedDirectory)
    try {
        $guardWasMutated = $false
        $competingOpenError = 0
        $competingIoctlAttempted = $false
        $competingIoctlError = 0
        $changed = [RangeFea.V43.Testing.NativeReparseAttempt]::TryAttachMountPointWhileGuarded(
            $guardHandle, $guardedDirectory, $targetDirectory,
            [ref]$guardWasMutated, [ref]$competingOpenError, [ref]$competingIoctlAttempted,
            [ref]$competingIoctlError)
        if ($guardWasMutated) { throw 'The native reparse probe unexpectedly invoked mutation through the bootstrap guard handle.' }
        if (-not $changed -and $competingOpenError -ne 32) {
            throw "Native FSCTL_SET_REPARSE_POINT attempt failed for an unexpected reason: openError=$competingOpenError ioctlAttempted=$competingIoctlAttempted ioctlError=$competingIoctlError"
        }
        $sameHandleSetError = 0
        $sameHandleSetSucceeded = [RangeFea.V43.Testing.NativeReparseAttempt]::TryAttachMountPointThroughGuard(
            $guardHandle, $targetDirectory, [ref]$sameHandleSetError)
        $relativeFilePath = Join-Path $guardedDirectory 'relative-create-probe.txt'
        $relativeCreateOutcome = 'NOT_ATTEMPTED'
        $relativeCreateFailure = $null
        $relativeCreateEscaped = $false
        $relativeCreateHandleReturned = $false
        $relativeCreateRefusedByReparse = $false
        $relativeCreateNativeErrorCode = 0
        $relativeFileHandle = $null
        if ($sameHandleSetSucceeded) {
            try {
                $relativeFileHandle = [RangeFea.V43.OutputDirectoryGuard]::CreateNewFileRelative(
                    $guardHandle, [IO.Path]::GetFileName($relativeFilePath))
                $relativeCreateHandleReturned = $true
            }
            catch [System.ComponentModel.Win32Exception] {
                $relativeCreateNativeErrorCode = $_.Exception.NativeErrorCode
                $relativeCreateRefusedByReparse = (
                    $relativeCreateNativeErrorCode -eq 4398 -and
                    $_.Exception.Message -match 'Could not create a single-use receipt relative to the held output directory handle \(NTSTATUS=0xc000050b\)\.'
                )
                $relativeCreateFailure = $_.Exception.Message
                if ($relativeCreateRefusedByReparse) { $relativeCreateOutcome = 'RELATIVE_CREATE_REPARSE_REFUSED' }
            }
            catch {
                $relativeCreateFailure = $_.Exception.Message
            }

            if ($null -ne $relativeFileHandle) {
                try {
                    $actualRelativePath = Normalize-DirectoryIdentityPath ([RangeFea.V43.OutputDirectoryGuard]::GetFinalPath($relativeFileHandle))
                    $expectedRelativePath = Normalize-DirectoryIdentityPath $relativeFilePath
                    $targetRelativeFilePath = Join-Path $targetDirectory 'relative-create-probe.txt'
                    $targetRelativeFileExistsBeforeCleanup = [IO.File]::Exists($targetRelativeFilePath)
                    if (-not $actualRelativePath.Equals($expectedRelativePath, [StringComparison]::OrdinalIgnoreCase) -or
                        $targetRelativeFileExistsBeforeCleanup) {
                        $relativeCreateEscaped = $true
                        $relativeCreateFailure = "Relative create escaped the held output directory: expected=$expectedRelativePath actual=$actualRelativePath targetFileExists=$targetRelativeFileExistsBeforeCleanup"
                    } else {
                        $relativeCreateOutcome = 'CREATED_ONLY_IN_HELD_DIRECTORY'
                    }
                }
                catch {
                    if ($null -eq $relativeCreateFailure) { $relativeCreateFailure = $_.Exception.Message }
                    else { $relativeCreateFailure += ' | path verification failed: ' + $_.Exception.Message }
                }
            finally {
                if ($null -ne $relativeFileHandle) {
                    try {
                        if (-not [RangeFea.V43.OutputDirectoryGuard]::DeleteFileByHandle($relativeFileHandle)) {
                            $deleteError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
                            $deleteFailure = "The native relative-create probe could not remove its empty file by handle (Win32=$deleteError)."
                            if ($null -eq $relativeCreateFailure) { $relativeCreateFailure = $deleteFailure }
                            else { $relativeCreateFailure += ' | ' + $deleteFailure }
                        }
                    }
                    catch {
                        if ($null -eq $relativeCreateFailure) { $relativeCreateFailure = $_.Exception.Message }
                        else { $relativeCreateFailure += ' | handle cleanup failed: ' + $_.Exception.Message }
                    }
                    finally { $relativeFileHandle.Dispose(); $relativeFileHandle = $null }
                }
            }
        }
        }
        $junctionCleanupError = 0
        $junctionCleanupSucceeded = if ($sameHandleSetSucceeded) {
            [RangeFea.V43.Testing.NativeReparseAttempt]::TryDeleteMountPointThroughGuard($guardHandle, [ref]$junctionCleanupError)
        } else { $true }
        if (-not $junctionCleanupSucceeded) { throw "Native reparse fixture could not remove its temporary junction: $junctionCleanupError" }
        if ($relativeCreateEscaped) { throw $relativeCreateFailure }
        if ($null -ne $relativeCreateFailure -and -not $relativeCreateRefusedByReparse) {
            throw "Native relative create did not fail through the whitelisted reparse refusal: $relativeCreateFailure"
        }
        if ($sameHandleSetSucceeded -and -not $relativeCreateHandleReturned -and -not $relativeCreateRefusedByReparse) {
            throw 'Native relative create neither returned a handle nor produced the whitelisted reparse refusal.'
        }
        if ([IO.File]::Exists((Join-Path $targetDirectory 'redirected-write-probe.txt'))) {
            throw 'The synthetic protected source target received a redirected write.'
        }
        if ([IO.File]::Exists((Join-Path $targetDirectory 'relative-create-probe.txt'))) {
            throw 'The handle-relative output create was redirected into the synthetic source target.'
        }
        if ([IO.File]::Exists($relativeFilePath)) { throw 'The native relative-create fixture left its empty probe receipt behind.' }
        if ((Get-Sha256File $sourceSentinel) -cne $sentinelShaBefore) {
            throw 'The synthetic protected source sentinel changed during the reparse attempt.'
        }
        $attributes = [IO.File]::GetAttributes($guardedDirectory)
        if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'The output directory became a reparse point during the native tamper fixture.'
        }
        return [pscustomobject]@{
            name = $Name
            status = if ($sameHandleSetSucceeded) { $relativeCreateOutcome } else { 'REPARSE_SET_DENIED_ON_GUARD' }
            fsctlSetReparsePointAttempted = $competingIoctlAttempted
            sameHandleFsctlSetSucceeded = $sameHandleSetSucceeded
            sameHandleFsctlSetError = $sameHandleSetError
            competingOpenError = $competingOpenError
            competingFsctlAttempted = $competingIoctlAttempted
            relativeCreateFailure = $relativeCreateFailure
            relativeCreateHandleReturned = $relativeCreateHandleReturned
            relativeCreateRefusedByReparse = $relativeCreateRefusedByReparse
            relativeCreateNativeErrorCode = $relativeCreateNativeErrorCode
            relativeCreateEscaped = $relativeCreateEscaped
            junctionCleanupSucceeded = $junctionCleanupSucceeded
            targetSentinelSha256 = $sentinelShaBefore
            redirectedWriteCreated = $false
        }
    }
    finally {
        if ($null -ne $guardHandle) { $guardHandle.Dispose() }
    }
}

function ConvertTo-BootstrapParameterMap([string[]]$Arguments) {
    $map = [ordered]@{}
    for ($index = 0; $index -lt $Arguments.Count; $index++) {
        $argument = [string]$Arguments[$index]
        if ($argument -ceq '-VerifyOnly') {
            $map['VerifyOnly'] = $true
            continue
        }
        if (-not $argument.StartsWith('-', [StringComparison]::Ordinal) -or $index + 1 -ge $Arguments.Count) {
            throw "Cannot construct preloaded-command fixture parameter map from argument: $argument"
        }
        $name = $argument.Substring(1)
        $index++
        $map[$name] = [string]$Arguments[$index]
    }
    return $map
}

function New-BootstrapPreloadWrapper([string[]]$Arguments, [string]$MarkerPath, [string]$PreloadKind, [string]$FakeManifestPath = $null) {
    $map = ConvertTo-BootstrapParameterMap $Arguments
    $lines = New-Object 'System.Collections.Generic.List[string]'
    if ($PreloadKind -eq 'global-function') {
        [void]$lines.Add('function global:ConvertFrom-Json { [IO.File]::WriteAllText(' + (Quote-PowerShellLiteral $MarkerPath) + ", 'REVIEW_PROBE_SHADOW_CONVERTFROMJSON_EXECUTED'); throw 'REVIEW_PROBE_SHADOW_CONVERTFROMJSON_EXECUTED' }")
    }
    elseif ($PreloadKind -eq 'fake-module') {
        [void]$lines.Add('Microsoft.PowerShell.Core\Import-Module -Name ' + (Quote-PowerShellLiteral $FakeManifestPath) + ' -Global -ErrorAction Stop')
    }
    elseif ($PreloadKind -eq 'guard-type') {
        [void]$lines.Add("`$guardSource = @'")
        [void]$lines.Add('using System; using Microsoft.Win32.SafeHandles; namespace RangeFea.V43 { public static class OutputDirectoryGuard { public static SafeFileHandle OpenDirectory(string path) { return new SafeFileHandle(IntPtr.Zero, true); } public static uint GetDirectoryAttributes(SafeFileHandle handle) { return 0; } public static string GetFinalPath(SafeFileHandle handle) { return String.Empty; } } }')
        [void]$lines.Add("'@")
        [void]$lines.Add("`$utilityManifest = [IO.Path]::Combine(`$PSHOME, 'Modules', 'Microsoft.PowerShell.Utility', 'Microsoft.PowerShell.Utility.psd1')")
        [void]$lines.Add('Microsoft.PowerShell.Core\Import-Module -Name $utilityManifest -Global -ErrorAction Stop')
        [void]$lines.Add('Microsoft.PowerShell.Utility\Add-Type -TypeDefinition $guardSource -ErrorAction Stop')
    }
    else { throw "Unknown preload fixture kind: $PreloadKind" }

    [void]$lines.Add('$bootstrapParameters = @{')
    foreach ($key in $map.Keys) {
        if ($key -ceq 'VerifyOnly') { [void]$lines.Add('    VerifyOnly = $true') }
        else { [void]$lines.Add(('    {0} = {1}' -f $key, (Quote-PowerShellLiteral ([string]$map[$key])))) }
    }
    [void]$lines.Add('}')
    [void]$lines.Add('& ' + (Quote-PowerShellLiteral $script:BootstrapPath) + ' @bootstrapParameters')
    $wrapperPath = Join-Path $script:TestRoot ($PreloadKind + '-' + [Guid]::NewGuid().ToString('N') + '.ps1')
    [IO.File]::WriteAllText($wrapperPath, [string]::Join("`r`n", $lines.ToArray()), (New-Object Text.UTF8Encoding($false)))
    return $wrapperPath
}

function Invoke-PreloadedBootstrap([string[]]$BootstrapParameters, [string]$MarkerPath, [string]$PreloadKind, [string]$FakeManifestPath = $null) {
    $wrapperPath = New-BootstrapPreloadWrapper $BootstrapParameters $MarkerPath $PreloadKind $FakeManifestPath
    return Invoke-Bootstrap @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $wrapperPath)
}

function New-Fixture([string]$Name, [string]$Variant) {
    $root = Join-Path $script:TestRoot $Name
    $sourceRoot = Join-Path $root 'source'
    $installRoot = Join-Path $root 'install'
    $auditRoot = Join-Path $root 'audit'
    $evidenceRoot = Join-Path $root 'evidence'
    foreach ($directory in @($sourceRoot, $installRoot, $auditRoot, $evidenceRoot)) { [void][IO.Directory]::CreateDirectory($directory) }

    $hashes = @{
        inventory = '1' * 64
        releaseBuild = '2' * 64
        rootCommit = 'a' * 40
        sourceFreeze = '3' * 64
        sourceTree = '4' * 64
        packageTree = '5' * 64
        aggregateStatic = '6' * 64
        reviewerSource = '7' * 64
        decisionSource = '8' * 64
    }
    $submoduleCount = switch ($Variant) {
        'submodule-one' { 1 }
        'submodule-seven' { 7 }
        default { 0 }
    }
    $submoduleCommits = @()
    for ($submoduleIndex = 0; $submoduleIndex -lt $submoduleCount; $submoduleIndex++) {
        $submoduleCommits += [ordered]@{
            path = ('src/submodule-{0}' -f $submoduleIndex)
            registeredCommit = 'a' * 40
            headCommit = 'b' * 40
            worktreeStatus = 'CLEAN'
            sourceTreeSha256 = 'c' * 64
        }
    }
    $manifestPath = Join-Path $evidenceRoot 'candidate-manifest-v43.json'
    $finalStaticPath = Join-Path $evidenceRoot 'final-static-v43.json'
    $reviewPath = Join-Path $evidenceRoot 'independent-review-v43.json'
    $decisionPath = Join-Path $evidenceRoot 'root-decision-v43.json'
    $freezePath = Join-Path $evidenceRoot 'source-freeze-v43.json'
    $packagePath = Join-Path $evidenceRoot 'package-tree-v43.json'
    $aggregatePath = Join-Path $evidenceRoot 'static-suite-v43.json'
    $outputPath = if ($Name -eq 'build-output') {
        Join-Path $sourceRoot 'build\audit\root-bootstrap-v43.json'
    } else { Join-Path $auditRoot 'root-bootstrap-v43.json' }
    if ($Name -eq 'build-output') {
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($outputPath))
    }

    $manifest = [ordered]@{
        schema = 'range.pc11-fresh-run-v43-admission-candidate/v1'
        candidateVersion = 'v43'
        candidateState = 'STATIC_CANDIDATE_PENDING_REVIEW_AND_ROOT_DECISION'
        goPermitted = $false
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        installBinding = [ordered]@{
            requiredVariant = 'OFF'
            installRoot = $installRoot
            inventoryReportSha256 = $hashes.inventory
        }
        sourceAndAuthorityPins = [ordered]@{
            sourceRootPath = $sourceRoot
            sourceFreezeReceiptPath = $freezePath
            releaseBuildReceiptPath = (Join-Path $evidenceRoot 'release-build-v43.json')
            releaseBuildReceiptSha256 = $hashes.releaseBuild
            rootCommit = $hashes.rootCommit
            sourceFreezeReceiptSha256 = $hashes.sourceFreeze
            sourceTreeSha256 = $hashes.sourceTree
            submoduleCommits = $submoduleCommits
        }
        candidateEvidencePins = [ordered]@{
            packageTreeReceiptSha256 = $hashes.packageTree
            packageTreeReceiptPath = $packagePath
            requiredStaticTestIds = $script:RequiredStaticTestIds
            staticTestReceipts = @([ordered]@{ name = 'v43-static-suite'; path = $aggregatePath; sha256 = $hashes.aggregateStatic })
        }
        workflowBoundary = [ordered]@{
            staticReadinessDoesNotGrantProductGo = $true
            externalIndependentReviewRequired = $true
            separateRootTechnicalDecisionRequired = $true
            goSealRequiredBeforeAnyProductAction = $true
            independentReviewReceiptPath = $reviewPath
            independentReviewReceiptSha256 = '0' * 64
            rootDecisionReceiptPath = $decisionPath
            rootDecisionReceiptSha256 = '0' * 64
        }
    }
    if ($Variant -eq 'array-candidate-version') { $manifest.candidateVersion = @('v43') }
    if ($Variant -eq 'array-required-variant') { $manifest.installBinding.requiredVariant = @('OFF') }
    if ($Variant -eq 'array-source-root') { $manifest.sourceAndAuthorityPins.sourceRootPath = @($sourceRoot) }
    if ($Variant -eq 'array-release-build-path') { $manifest.sourceAndAuthorityPins.releaseBuildReceiptPath = @((Join-Path $evidenceRoot 'release-build-v43.json')) }
    if ($Variant -eq 'array-release-build-sha') { $manifest.sourceAndAuthorityPins.releaseBuildReceiptSha256 = @($hashes.releaseBuild) }
    if ($Variant -eq 'array-go-permitted') { $manifest.goPermitted = @($false) }
    if ($Variant -eq 'empty-array-go-permitted') { $manifest.goPermitted = @() }
    if ($Variant -eq 'array-product-action') { $manifest.productActions.guiLaunched = @($false) }
    $identity = Get-CandidateIdentity $manifest
    $rows = @($script:RequiredStaticTestIds | ForEach-Object { [ordered]@{ testId = $_; status = 'PASS' } })
    $finalStatus = if ($Variant -eq 'incomplete') { 'STATIC_INCOMPLETE' } else { 'STATIC_COMPLETE_NO_GO' }
    $finalIdentity = if ($Variant -eq 'identity') { 'f' * 64 } else { $identity }
    [string[]]$pendingIds = @()
    if ($Variant -eq 'incomplete') { $pendingIds = @('root_bootstrap') }
    $results = if ($Variant -eq 'incomplete') {
        @($rows | ForEach-Object { if ($_.testId -eq 'root_bootstrap') { [ordered]@{ testId = $_.testId; status = 'PENDING' } } else { $_ } })
    } else { $rows }
    $finalStatic = [ordered]@{
        schema = 'range.pc11-v43-final-static-receipt/v1'
        status = $finalStatus
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        candidateManifestSha256 = '9' * 64
        candidateManifestIdentitySha256 = $finalIdentity
        sourceFreezeReceiptPath = $freezePath
        sourceFreezeReceiptSha256 = $hashes.sourceFreeze
        sourceTreeSha256 = $hashes.sourceTree
        inventoryReportSha256 = $hashes.inventory
        packageTreeReceiptPath = $packagePath
        packageTreeReceiptSha256 = $hashes.packageTree
        staticTestReceiptPath = $aggregatePath
        staticTestReceiptSha256 = $hashes.aggregateStatic
        requiredTestIds = $script:RequiredStaticTestIds
        results = $results
        allRequiredStaticTestsPass = ($Variant -ne 'incomplete')
        pendingTestIds = $pendingIds
        missingV39EquivalentGates = $pendingIds
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
    if ($Variant -eq 'array-final-variant') { $finalStatic.variant = @('OFF') }
    if ($Variant -eq 'array-static-only') { $finalStatic.staticOnly = @($true) }
    $finalSha = Write-JsonFile $finalStaticPath $finalStatic
    $reviewStatus = if ($Variant -eq 'review') { 'FAIL' } else { 'PASS' }
    $review = [ordered]@{
        schema = 'range.pc11-v43-independent-static-review/v1'
        status = $reviewStatus
        conclusion = 'INDEPENDENT_REVIEW_PASS_NO_GO'
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        candidateManifestIdentitySha256 = $identity
        finalStaticReceiptSha256 = $finalSha
        sourceTreeSha256 = $hashes.sourceTree
        sourceFreezeReceiptSha256 = $hashes.sourceFreeze
        reviewerIdentity = 'synthetic-reviewer-fixture'
        reviewerSourceSha256 = $hashes.reviewerSource
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
    $reviewSha = Write-JsonFile $reviewPath $review
    $decisionValue = if ($Variant -eq 'decision') { 'REJECT_ROOT_SEAL' } else { 'APPROVE_ROOT_SEAL' }
    $decision = [ordered]@{
        schema = 'range.pc11-v43-root-technical-decision/v1'
        status = 'PASS'
        decision = $decisionValue
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        candidateManifestIdentitySha256 = $identity
        finalStaticReceiptSha256 = $finalSha
        independentReviewReceiptSha256 = $reviewSha
        sourceTreeSha256 = $hashes.sourceTree
        decisionId = 'synthetic-root-decision-fixture'
        decisionAuthorityIdentity = 'synthetic-root-authority-fixture'
        decisionSourceSha256 = $hashes.decisionSource
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
    $decisionSha = Write-JsonFile $decisionPath $decision
    $manifest.workflowBoundary.independentReviewReceiptSha256 = $reviewSha
    $manifest.workflowBoundary.rootDecisionReceiptSha256 = $decisionSha
    $manifestSha = Write-JsonFile $manifestPath $manifest

    return [pscustomobject]@{
        manifestPath = $manifestPath
        manifestSha256 = $manifestSha
        finalStaticPath = $finalStaticPath
        finalStaticSha256 = $finalSha
        reviewPath = $reviewPath
        reviewSha256 = $reviewSha
        decisionPath = $decisionPath
        decisionSha256 = $decisionSha
        outputPath = $outputPath
        sourceRoot = $sourceRoot
        installRoot = $installRoot
        auditRoot = $auditRoot
        submoduleCount = $submoduleCount
        identitySha256 = $identity
    }
}

function Get-FixtureBootstrapParameters($Fixture, [string]$OutputPath) {
    return @(
        '-CandidateManifestPath', $Fixture.manifestPath, '-CandidateManifestSha256', $Fixture.manifestSha256,
        '-FinalStaticReceiptPath', $Fixture.finalStaticPath, '-FinalStaticReceiptSha256', $Fixture.finalStaticSha256,
        '-IndependentReviewReceiptPath', $Fixture.reviewPath, '-IndependentReviewReceiptSha256', $Fixture.reviewSha256,
        '-RootTechnicalDecisionReceiptPath', $Fixture.decisionPath, '-RootTechnicalDecisionReceiptSha256', $Fixture.decisionSha256,
        '-OutputPath', $OutputPath, '-VerifyOnly'
    )
}

function Invoke-Scenario(
    [string]$Name,
    [string]$Variant,
    [string]$ExpectedStatus,
    [string]$FailureKind,
    [string]$FakeModuleRoot = $null,
    [string]$FakeModuleLoadedMarker = $null,
    [string]$FakeCommandMarker = $null
) {
    $fixture = New-Fixture $Name $Variant
    $manifestPath = $fixture.manifestPath
    $finalPath = $fixture.finalStaticPath
    $finalSha = $fixture.finalStaticSha256
    $reviewPath = $fixture.reviewPath
    $reviewSha = $fixture.reviewSha256
    $decisionPath = $fixture.decisionPath
    $decisionSha = $fixture.decisionSha256
    $actualOutputPath = $fixture.outputPath
    if ($FailureKind -eq 'missing') { $finalPath = Join-Path $script:TestRoot "$Name\evidence\missing-final-static.json" }
    if ($FailureKind -eq 'hash') { $finalSha = 'b' * 64 }
    if ($FailureKind -eq 'malformed') { $finalSha = 'not-a-sha256-pin' }
    if ($FailureKind -eq 'decision-pin') { $decisionSha = 'c' * 64 }
    if ($FailureKind -eq 'output-in-source') { $actualOutputPath = Join-Path $fixture.sourceRoot 'unexpected-receipt.json' }
    if ($FailureKind -eq 'output-in-install') { $actualOutputPath = Join-Path $fixture.installRoot 'unexpected-receipt.json' }
    $missingOutputParent = $null
    if ($FailureKind -eq 'missing-output-parent') {
        $missingOutputParent = Join-Path $fixture.auditRoot 'must-remain-missing'
        $actualOutputPath = Join-Path $missingOutputParent 'root-bootstrap-v43.json'
        if ([IO.Directory]::Exists($missingOutputParent)) { throw 'Missing-parent fixture setup unexpectedly found its output parent.' }
    }

    $scenarioFixture = [pscustomobject]@{
        manifestPath = $manifestPath
        manifestSha256 = $fixture.manifestSha256
        finalStaticPath = $finalPath
        finalStaticSha256 = $finalSha
        reviewPath = $reviewPath
        reviewSha256 = $reviewSha
        decisionPath = $decisionPath
        decisionSha256 = $decisionSha
    }
    $bootstrapParameters = Get-FixtureBootstrapParameters $scenarioFixture $actualOutputPath
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script:BootstrapPath) + $bootstrapParameters
    $modulePathOverride = $null
    if (-not [string]::IsNullOrWhiteSpace($FakeModuleRoot)) {
        $modulePathOverride = $FakeModuleRoot + ';' + (Join-Path $PSHOME 'Modules')
    }
    $result = Invoke-Bootstrap $arguments $modulePathOverride
    if (-not [string]::IsNullOrWhiteSpace($FakeModuleLoadedMarker) -and
        ([IO.File]::Exists($FakeModuleLoadedMarker) -or [IO.File]::Exists($FakeCommandMarker))) {
        throw "Fake-first module path fixture '$Name' loaded or executed the fake system Utility module."
    }
    if ($ExpectedStatus -eq 'NO_RECEIPT_REJECTED') {
        if ($result.exitCode -eq 0) { throw "Unsafe-output fixture '$Name' unexpectedly succeeded." }
        if ([IO.File]::Exists($actualOutputPath)) { throw "Unsafe-output fixture '$Name' wrote a receipt inside source/install." }
        if ($null -ne $missingOutputParent -and [IO.Directory]::Exists($missingOutputParent)) {
            throw "Missing-parent fixture '$Name' created its output parent during VerifyOnly."
        }
        return [pscustomobject]@{ name = $Name; status = 'REJECTED_NO_RECEIPT'; sha256 = $null }
    }
    if ($ExpectedStatus -eq 'ROOT_BOOTSTRAP_VERIFIED_NO_GO') {
        if ($result.exitCode -ne 0) {
            $detail = if ([IO.File]::Exists($fixture.outputPath)) {
                $failedReceipt = (New-Object Text.UTF8Encoding($false, $true)).GetString([IO.File]::ReadAllBytes($actualOutputPath)) | ConvertFrom-Json -ErrorAction Stop
                [string]$failedReceipt.failure
            } else { 'no failure receipt' }
            throw "Positive synthetic root-bootstrap fixture failed: $detail $($result.stderr) $($result.stdout)"
        }
    }
    elseif ($result.exitCode -eq 0) {
        throw "Negative synthetic root-bootstrap fixture '$Name' unexpectedly succeeded."
    }
    if (-not [IO.File]::Exists($actualOutputPath)) { throw "Fixture '$Name' did not produce its single-use receipt." }
    $receipt = (New-Object Text.UTF8Encoding($false, $true)).GetString([IO.File]::ReadAllBytes($actualOutputPath)) | ConvertFrom-Json -ErrorAction Stop
    if ($receipt.schema -cne 'range.pc11-v43-root-bootstrap-receipt/v1' -or $receipt.status -cne $ExpectedStatus -or
        $receipt.goPermitted -ne $false -or $receipt.operationMode -cne 'VERIFY_ONLY') {
        throw "Fixture '$Name' emitted an unexpected schema, status, or product boundary."
    }
    foreach ($actionName in $script:ProductActionNames) {
        if ($receipt.productActions.$actionName -ne $false) { throw "Fixture '$Name' permits product action '$actionName'." }
    }
    if ($ExpectedStatus -eq 'ROOT_BOOTSTRAP_VERIFIED_NO_GO') {
        if ($receipt.outputDirectoryIdentityGuard -cne 'ALL_ANCESTOR_HANDLES_HELD_WITHOUT_DELETE_SHARING_UNTIL_RECEIPT_CREATE' -or
            $receipt.outputDirectoryHandleCount -lt 2) {
            throw "Fixture '$Name' did not prove it held the output directory ancestry through receipt creation."
        }
        if ($receipt.candidateManifestIdentitySha256 -cne $fixture.identitySha256 -or
            $receipt.finalStaticReceiptSha256 -cne $fixture.finalStaticSha256 -or
            $receipt.independentReviewReceiptSha256 -cne $fixture.reviewSha256 -or
            $receipt.rootTechnicalDecisionReceiptSha256 -cne $fixture.decisionSha256 -or
            $receipt.reviewerIdentity -cne 'synthetic-reviewer-fixture' -or
            $receipt.decisionId -cne 'synthetic-root-decision-fixture') {
            throw 'Positive fixture receipt did not preserve all candidate, review, and root-decision pins.'
        }
        $expectedBoundary = if ($Name -eq 'build-output') { 'EXCLUDED_REPOSITORY_BUILD' } else { 'EXTERNAL_TO_SOURCE_TREE' }
        if ($receipt.outputBoundary -cne $expectedBoundary) { throw "Fixture '$Name' reported the wrong output-boundary classification." }
    }
    elseif ([string]::IsNullOrWhiteSpace([string]$receipt.failure)) { throw "Rejected fixture '$Name' did not record its failure reason." }
    $receiptSha = Get-Sha256File $actualOutputPath
    if ($ExpectedStatus -eq 'ROOT_BOOTSTRAP_VERIFIED_NO_GO') {
        $second = Invoke-Bootstrap $arguments $modulePathOverride
        if ($second.exitCode -eq 0 -or (Get-Sha256File $actualOutputPath) -cne $receiptSha) {
            throw "Fixture '$Name' did not enforce single-use output without changing its first receipt."
        }
    }
    return [pscustomobject]@{ name = $Name; status = $receipt.status; sha256 = $receiptSha }
}

function Invoke-PreloadedBoundaryNegative([string]$Name, [string]$PreloadKind, [string]$ExpectedMessage) {
    $fixture = New-Fixture $Name 'positive'
    $parameters = Get-FixtureBootstrapParameters $fixture $fixture.outputPath
    $fakeModule = New-FakeUtilityModule $Name
    $markerPath = Join-Path $script:TestRoot ($Name + '-preloaded-command-marker.txt')
    $manifestPath = if ($PreloadKind -eq 'fake-module') { $fakeModule.manifestPath } else { $null }
    $result = Invoke-PreloadedBootstrap $parameters $markerPath $PreloadKind $manifestPath
    if ($result.exitCode -eq 0) { throw "Preloaded boundary fixture '$Name' unexpectedly succeeded." }
    if ([IO.File]::Exists($fixture.outputPath)) { throw "Preloaded boundary fixture '$Name' wrote a receipt before refusing the shadow." }
    $diagnostic = $result.stderr + $result.stdout
    if (-not $diagnostic.Contains($ExpectedMessage)) {
        throw "Preloaded boundary fixture '$Name' missed its expected refusal. Expected '$ExpectedMessage'; actual: $diagnostic"
    }
    if ($PreloadKind -eq 'global-function') {
        if ([IO.File]::Exists($markerPath)) {
            $markerContents = [IO.File]::ReadAllText($markerPath)
            throw "Exact preloaded ConvertFrom-Json adversarial probe executed: $markerContents"
        }
    }
    elseif ($PreloadKind -eq 'fake-module') {
        if (-not [IO.File]::Exists($fakeModule.loadedMarker)) { throw 'Preloaded fake Utility fixture did not load its probe module.' }
        if ([IO.File]::Exists($fakeModule.commandMarker)) { throw 'Preloaded fake Utility command executed before the bootstrap rejected its module path.' }
    }
    elseif ($PreloadKind -eq 'guard-type') {
        if (-not $diagnostic.Contains('A preloaded output directory guard type cannot bypass the trusted v43 native implementation.')) {
            throw "Preloaded type fixture '$Name' did not reject the shadow output guard type: $diagnostic"
        }
    }
    if ($diagnostic -match 'goPermitted=true|guiLaunched=true|savePerformed=true|runPerformed=true|confirmPerformed=true') {
        throw "Preloaded boundary fixture '$Name' reported a product action or GO permission."
    }
    return [pscustomobject]@{ name = $Name; status = 'REJECTED_BEFORE_JSON_OR_RECEIPT'; sha256 = $null }
}

try {
    $sourceText = [IO.File]::ReadAllText($script:BootstrapPath)
    if ($sourceText -match '(?im)\bStart-Process\b|Diagnostics\.Process|fea(?:-solver)?\.exe') {
        throw 'The root-bootstrap implementation contains a product process or executable launch reference.'
    }
    $tokens = $null
    $parseErrors = $null
    $bootstrapAst = [Management.Automation.Language.Parser]::ParseFile($script:BootstrapPath, [ref]$tokens, [ref]$parseErrors)
    if (@($parseErrors).Count -gt 0) { throw "PowerShell parser rejected root-bootstrap script: $($parseErrors[0].Message)" }
    $directoryCreateNodes = @($bootstrapAst.FindAll({
        param($node)
        if ($node -isnot [Management.Automation.Language.InvokeMemberExpressionAst]) { return $false }
        $member = $node.Member
        return ($member -is [Management.Automation.Language.StringConstantExpressionAst] -and
            $member.Value -ieq 'CreateDirectory')
    }, $true))
    $directoryCreateTextPattern = '(?i)\b(?:[\w.]+\.)?Directory\s*\]?\s*::\s*CreateDirectory\b'
    if ($directoryCreateNodes.Count -gt 0 -or $sourceText -match $directoryCreateTextPattern) {
        throw 'The full root-bootstrap runtime must contain no Directory.CreateDirectory call; both success and failure paths require an existing parent.'
    }
    $nativeReparseProbe = Invoke-NativeOutputDirectoryReparseProbe 'native-reparse-lock-probe' $sourceText

    $scenarios = New-Object 'System.Collections.Generic.List[object]'
    [void]$scenarios.Add($nativeReparseProbe)
    [void]$scenarios.Add((Invoke-Scenario 'positive' 'positive' 'ROOT_BOOTSTRAP_VERIFIED_NO_GO' ''))
    [void]$scenarios.Add((Invoke-Scenario 'build-output' 'positive' 'ROOT_BOOTSTRAP_VERIFIED_NO_GO' ''))
    [void]$scenarios.Add((Invoke-Scenario 'submodule-zero' 'submodule-zero' 'ROOT_BOOTSTRAP_VERIFIED_NO_GO' ''))
    [void]$scenarios.Add((Invoke-Scenario 'submodule-one' 'submodule-one' 'ROOT_BOOTSTRAP_VERIFIED_NO_GO' ''))
    [void]$scenarios.Add((Invoke-Scenario 'submodule-seven' 'submodule-seven' 'ROOT_BOOTSTRAP_VERIFIED_NO_GO' ''))
    $fakeFirstModule = New-FakeUtilityModule 'fake-first-path'
    $fakeFirstPath = Invoke-Scenario 'fake-first-module-path' 'positive' 'ROOT_BOOTSTRAP_VERIFIED_NO_GO' '' `
        $fakeFirstModule.moduleRoot $fakeFirstModule.loadedMarker $fakeFirstModule.commandMarker
    [void]$scenarios.Add($fakeFirstPath)
    [void]$scenarios.Add((Invoke-PreloadedBoundaryNegative 'preloaded-global-convertfrom-json' 'global-function' `
        'A same-name PowerShell alias/function was preloaded before the system module boundary: ConvertFrom-Json'))
    [void]$scenarios.Add((Invoke-PreloadedBoundaryNegative 'preloaded-fake-utility-module' 'fake-module' `
        'A same-name module was preloaded from a non-system path:'))
    [void]$scenarios.Add((Invoke-PreloadedBoundaryNegative 'preloaded-output-guard-type' 'guard-type' `
        'A preloaded output directory guard type cannot bypass the trusted v43 native implementation.'))
    [void]$scenarios.Add((Invoke-Scenario 'missing' 'positive' 'ROOT_BOOTSTRAP_REJECTED_NO_GO' 'missing'))
    [void]$scenarios.Add((Invoke-Scenario 'hash-mismatch' 'positive' 'ROOT_BOOTSTRAP_REJECTED_NO_GO' 'hash'))
    [void]$scenarios.Add((Invoke-Scenario 'malformed-pin' 'positive' 'ROOT_BOOTSTRAP_REJECTED_NO_GO' 'malformed'))
    [void]$scenarios.Add((Invoke-Scenario 'array-candidate-version' 'array-candidate-version' 'ROOT_BOOTSTRAP_REJECTED_NO_GO' ''))
    [void]$scenarios.Add((Invoke-Scenario 'array-required-variant' 'array-required-variant' 'ROOT_BOOTSTRAP_REJECTED_NO_GO' ''))
    [void]$scenarios.Add((Invoke-Scenario 'array-source-root' 'array-source-root' 'NO_RECEIPT_REJECTED' ''))
    [void]$scenarios.Add((Invoke-Scenario 'array-release-build-path' 'array-release-build-path' 'ROOT_BOOTSTRAP_REJECTED_NO_GO' ''))
    [void]$scenarios.Add((Invoke-Scenario 'array-release-build-sha' 'array-release-build-sha' 'ROOT_BOOTSTRAP_REJECTED_NO_GO' ''))
    [void]$scenarios.Add((Invoke-Scenario 'array-final-variant' 'array-final-variant' 'ROOT_BOOTSTRAP_REJECTED_NO_GO' ''))
    [void]$scenarios.Add((Invoke-Scenario 'array-static-only' 'array-static-only' 'ROOT_BOOTSTRAP_REJECTED_NO_GO' ''))
    [void]$scenarios.Add((Invoke-Scenario 'array-go-permitted' 'array-go-permitted' 'ROOT_BOOTSTRAP_REJECTED_NO_GO' ''))
    [void]$scenarios.Add((Invoke-Scenario 'empty-array-go-permitted' 'empty-array-go-permitted' 'ROOT_BOOTSTRAP_REJECTED_NO_GO' ''))
    [void]$scenarios.Add((Invoke-Scenario 'array-product-action' 'array-product-action' 'ROOT_BOOTSTRAP_REJECTED_NO_GO' ''))
    [void]$scenarios.Add((Invoke-Scenario 'identity-mismatch' 'identity' 'ROOT_BOOTSTRAP_REJECTED_NO_GO' ''))
    [void]$scenarios.Add((Invoke-Scenario 'static-incomplete' 'incomplete' 'ROOT_BOOTSTRAP_REJECTED_NO_GO' ''))
    [void]$scenarios.Add((Invoke-Scenario 'review-rejected' 'review' 'ROOT_BOOTSTRAP_REJECTED_NO_GO' ''))
    [void]$scenarios.Add((Invoke-Scenario 'decision-rejected' 'decision' 'ROOT_BOOTSTRAP_REJECTED_NO_GO' ''))
    [void]$scenarios.Add((Invoke-Scenario 'decision-pin-mismatch' 'positive' 'ROOT_BOOTSTRAP_REJECTED_NO_GO' 'decision-pin'))
    [void]$scenarios.Add((Invoke-Scenario 'source-output-rejected' 'positive' 'NO_RECEIPT_REJECTED' 'output-in-source'))
    [void]$scenarios.Add((Invoke-Scenario 'install-output-rejected' 'positive' 'NO_RECEIPT_REJECTED' 'output-in-install'))
    [void]$scenarios.Add((Invoke-Scenario 'missing-output-parent-no-create' 'positive' 'NO_RECEIPT_REJECTED' 'missing-output-parent'))

    Write-Output ("PASS_ROOT_BOOTSTRAP_V43_SYNTHETIC_FIXTURES scenarioCount={0} positive=6 rejected=24 shadowBoundaryCases=4 submoduleArrayCases=0,1,7 sourceBuildPinArrayCases=3 nativeReparseTamperCases=1 nativeReparseStatus={1} competingOpenError={2} competingFsctlAttempted={3} sameHandleFsctlSetSucceeded={4} relativeCreateHandleReturned={5} relativeCreateEscaped={6} redirectedWriteCreated={7} actualReviewOrDecisionPerformed=false" -f $scenarios.Count, $nativeReparseProbe.status, $nativeReparseProbe.competingOpenError, $nativeReparseProbe.competingFsctlAttempted, $nativeReparseProbe.sameHandleFsctlSetSucceeded, $nativeReparseProbe.relativeCreateHandleReturned, $nativeReparseProbe.relativeCreateEscaped, $nativeReparseProbe.redirectedWriteCreated)
}
finally {
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $script:TestRoot.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean fixture output outside the temporary root: $script:TestRoot"
    }
    if ([IO.Directory]::Exists($script:TestRoot)) { Remove-Item -LiteralPath $script:TestRoot -Recurse -Force }
}
