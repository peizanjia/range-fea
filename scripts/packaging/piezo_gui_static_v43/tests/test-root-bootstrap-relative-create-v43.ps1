[CmdletBinding()]
param(
    [string]$AuditRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSEdition -ne 'Desktop' -or
    $PSVersionTable.PSVersion.Major -ne 5 -or
    $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'The v43 root-bootstrap relative-create fixture requires Windows PowerShell 5.1.'
}

$prepRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoRoot = [IO.Path]::GetFullPath((Join-Path $prepRoot '..\..\..'))
$bootstrapPath = [IO.Path]::GetFullPath((Join-Path $prepRoot 'runtime\root-bootstrap-v43.ps1'))
$auditBase = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build\audit-20261014\piezo-delivery')).TrimEnd('\') + '\'
if ([string]::IsNullOrWhiteSpace($AuditRoot)) {
    $AuditRoot = Join-Path $auditBase ('root-bootstrap-relative-create-' + [Guid]::NewGuid().ToString('N'))
}
$AuditRoot = [IO.Path]::GetFullPath($AuditRoot)
if (-not $AuditRoot.StartsWith($auditBase, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The relative-create receipt must be preserved below build/audit-20261014/piezo-delivery.'
}
[void][IO.Directory]::CreateDirectory($AuditRoot)
$receiptPath = Join-Path $AuditRoot ('root-bootstrap-relative-create-' + [Guid]::NewGuid().ToString('N') + '.json')

$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$testRoot = [IO.Path]::GetFullPath((Join-Path $tempBase ('range-fea-v43-root-bootstrap-relative-create-' + [Guid]::NewGuid().ToString('N'))))
$testRootPrefix = $testRoot.TrimEnd('\') + '\'
if (-not $testRoot.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -or
    $testRoot.StartsWith($repoRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The native reparse fixture must use an isolated temporary root outside the repository.'
}

function Test-PathWithin([string]$Path, [string]$Root) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    return $fullPath.StartsWith($fullRoot, [StringComparison]::OrdinalIgnoreCase)
}

function Get-Sha256File([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}

function Initialize-ProductionGuard([string]$Source) {
    if ('RangeFea.V43.OutputDirectoryGuard' -as [type]) {
        throw 'A preloaded output-directory guard type cannot be used by this fixture.'
    }
    $match = [regex]::Match($Source, '(?s)\$directoryGuardSource\s*=\s*@''\r?\n(?<code>.*?)\r?\n''@')
    if (-not $match.Success) { throw 'Could not extract the unmodified production OutputDirectoryGuard C# class.' }
    $utilityManifest = Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1'
    Microsoft.PowerShell.Core\Import-Module -Name $utilityManifest -Global -ErrorAction Stop
    Microsoft.PowerShell.Utility\Add-Type -TypeDefinition $match.Groups['code'].Value -Language CSharp -ErrorAction Stop

    $testNativeSource = @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace RangeFea.V43.Testing {
    public static class RelativeCreateReparseFixture {
        private const uint GenericRead = 0x80000000;
        private const uint GenericWrite = 0x40000000;
        private const uint FileShareRead = 0x00000001;
        private const uint OpenExisting = 3;
        private const uint FileFlagBackupSemantics = 0x02000000;
        private const uint FileFlagOpenReparsePoint = 0x00200000;
        private const uint FsctlSetReparsePoint = 0x000900A4;
        private const uint FsctlDeleteReparsePoint = 0x000900AC;
        private const uint IoReparseTagMountPoint = 0xA0000003;

        [DllImport("kernel32.dll", EntryPoint = "CreateFileW", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeFileHandle CreateFileW(
            string path, uint desiredAccess, uint shareMode, IntPtr securityAttributes,
            uint creationDisposition, uint flagsAndAttributes, IntPtr templateFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool DeviceIoControl(
            SafeFileHandle device, uint controlCode, byte[] inputBuffer, uint inputBufferSize,
            byte[] outputBuffer, uint outputBufferSize, out uint bytesReturned, IntPtr overlapped);

        public static SafeFileHandle OpenWritableDirectory(string path, out int win32Error) {
            SafeFileHandle handle = CreateFileW(path, GenericRead | GenericWrite, FileShareRead,
                IntPtr.Zero, OpenExisting,
                FileFlagBackupSemantics | FileFlagOpenReparsePoint, IntPtr.Zero);
            if (handle == null || handle.IsInvalid) {
                win32Error = Marshal.GetLastWin32Error();
                if (handle != null) handle.Dispose();
                return null;
            }
            win32Error = 0;
            return handle;
        }

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

        public static bool TryAttachMountPointThroughSameHandle(
            SafeFileHandle directoryHandle, string targetPath, out int win32Error) {
            if (directoryHandle == null || directoryHandle.IsInvalid || directoryHandle.IsClosed)
                throw new InvalidOperationException("The test-only writable directory handle is not open.");
            byte[] reparseData = BuildMountPointData(targetPath);
            uint bytesReturned;
            bool attached = DeviceIoControl(directoryHandle, FsctlSetReparsePoint, reparseData,
                (uint)reparseData.Length, null, 0, out bytesReturned, IntPtr.Zero);
            win32Error = attached ? 0 : Marshal.GetLastWin32Error();
            return attached;
        }

        public static bool TryDeleteMountPointThroughSameHandle(
            SafeFileHandle directoryHandle, out int win32Error) {
            if (directoryHandle == null || directoryHandle.IsInvalid || directoryHandle.IsClosed)
                throw new InvalidOperationException("The test-only writable directory handle is not open for junction cleanup.");
            byte[] deleteBuffer = new byte[8];
            Buffer.BlockCopy(BitConverter.GetBytes(IoReparseTagMountPoint), 0, deleteBuffer, 0, 4);
            uint bytesReturned;
            bool deleted = DeviceIoControl(directoryHandle, FsctlDeleteReparsePoint, deleteBuffer,
                (uint)deleteBuffer.Length, null, 0, out bytesReturned, IntPtr.Zero);
            win32Error = deleted ? 0 : Marshal.GetLastWin32Error();
            return deleted;
        }
    }
}
'@
    Microsoft.PowerShell.Utility\Add-Type -TypeDefinition $testNativeSource -Language CSharp -ErrorAction Stop
}

function Test-SafeTemporaryTree([string]$Root) {
    try {
        $resolvedRoot = [IO.Path]::GetFullPath($Root)
        if (-not $resolvedRoot.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -or
            [IO.Path]::GetFileName($resolvedRoot) -notmatch '^range-fea-v43-root-bootstrap-relative-create-[0-9a-f]{32}$') {
            return $false
        }
        $pending = New-Object 'System.Collections.Generic.Stack[string]'
        $pending.Push($resolvedRoot)
        while ($pending.Count -gt 0) {
            $directory = $pending.Pop()
            if (-not $directory.Equals($resolvedRoot, [StringComparison]::OrdinalIgnoreCase) -and
                -not (Test-PathWithin $directory $resolvedRoot)) { return $false }
            if (([IO.File]::GetAttributes($directory) -band [IO.FileAttributes]::ReparsePoint) -ne 0) { return $false }
            foreach ($entry in [IO.Directory]::GetFileSystemEntries($directory)) {
                if (-not (Test-PathWithin $entry $resolvedRoot)) { return $false }
                $attributes = [IO.File]::GetAttributes($entry)
                if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { return $false }
                if (($attributes -band [IO.FileAttributes]::Directory) -ne 0) { $pending.Push($entry) }
            }
        }
        return $true
    }
    catch { return $false }
}

$result = [ordered]@{
    schema = 'range.pc11-v43-root-bootstrap-relative-create-receipt/v1'
    status = 'NOT_RUN'
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    tempRoot = $testRoot
    sourceDirectory = Join-Path $testRoot 'source'
    mutatedGuardDirectory = Join-Path $testRoot 'guard'
    reparseTargetDirectory = Join-Path $testRoot 'target'
    cleanGuardDirectory = Join-Path $testRoot 'clean-guard'
    writeHandleOpenSucceeded = $false
    writeHandleWin32Error = 0
    writeHandleDesiredAccess = 'GENERIC_READ|GENERIC_WRITE'
    writeHandleShareMode = 'FILE_SHARE_READ'
    writeHandleFlags = 'FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT'
    fsctlSetAttempted = $false
    fsctlSetSucceeded = $false
    fsctlSetWin32Error = 0
    relativeCreateAttempted = $false
    relativeCreateHandleReturned = $false
    relativeCreateNativeErrorCode = 0
    relativeCreateNtStatus = $null
    relativeCreateExceptionMessage = $null
    relativeCreateActualFinalPath = $null
    relativeCreateExpectedGuardPath = $null
    relativeCreateExpectedTargetPath = $null
    relativeCreateRefusedByWhitelistedReparseStatus = $false
    relativeCreateContainedIdentity = $false
    redirectedOutputAppearedInTarget = $false
    junctionCleanupAttempted = $false
    junctionCleanupSucceeded = $false
    junctionCleanupWin32Error = 0
    cleanPositivePassed = $false
    cleanPositiveExpectedPath = $null
    cleanPositiveActualPath = $null
    cleanPositiveDeleteSucceeded = $false
    sourceSentinelSha256Before = $null
    sourceSentinelSha256After = $null
    targetSentinelSha256Before = $null
    targetSentinelSha256After = $null
    tempTreeCleanup = 'NOT_ATTEMPTED'
    tempTreeRetained = $false
    failure = $null
    productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
    goPermitted = $false
}

$cleanDirectoryHandle = $null
$cleanOutputHandle = $null
$mutatedDirectoryHandle = $null
$mutatedOutputHandle = $null
$junctionMayExist = $false
$cleanupFailure = $null
$sourceSentinel = Join-Path $result.sourceDirectory 'source-sentinel.txt'
$targetSentinel = Join-Path $result.reparseTargetDirectory 'target-sentinel.txt'
$mutatedOutputLeaf = 'relative-create-' + [Guid]::NewGuid().ToString('N') + '.receipt'
$cleanOutputLeaf = 'clean-create-' + [Guid]::NewGuid().ToString('N') + '.receipt'

try {
    [void][IO.Directory]::CreateDirectory($testRoot)
    foreach ($directory in @($result.sourceDirectory, $result.mutatedGuardDirectory, $result.reparseTargetDirectory, $result.cleanGuardDirectory)) {
        if (-not (Test-PathWithin $directory $testRoot)) { throw "Fixture directory escaped the isolated temp root: $directory" }
        [void][IO.Directory]::CreateDirectory($directory)
    }
    [IO.File]::WriteAllText($sourceSentinel, 'SYNTHETIC_SOURCE_SENTINEL_UNCHANGED')
    [IO.File]::WriteAllText($targetSentinel, 'SYNTHETIC_REPARSE_TARGET_SENTINEL_UNCHANGED')
    $result.sourceSentinelSha256Before = Get-Sha256File $sourceSentinel
    $result.targetSentinelSha256Before = Get-Sha256File $targetSentinel

    $bootstrapSource = [IO.File]::ReadAllText($bootstrapPath)
    Initialize-ProductionGuard $bootstrapSource

    # A clean positive call proves the unmodified production helper still creates
    # the receipt at the exact identity represented by its held parent handle.
    $cleanDirectoryHandle = [RangeFea.V43.OutputDirectoryGuard]::OpenDirectory($result.cleanGuardDirectory)
    $result.cleanPositiveExpectedPath = [IO.Path]::GetFullPath((Join-Path $result.cleanGuardDirectory $cleanOutputLeaf))
    $cleanOutputHandle = [RangeFea.V43.OutputDirectoryGuard]::CreateNewFileRelative($cleanDirectoryHandle, $cleanOutputLeaf)
    $result.cleanPositiveActualPath = [IO.Path]::GetFullPath([RangeFea.V43.OutputDirectoryGuard]::GetFinalPath($cleanOutputHandle))
    if (-not $result.cleanPositiveActualPath.Equals($result.cleanPositiveExpectedPath, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Clean relative create returned a different file identity: expected=$($result.cleanPositiveExpectedPath) actual=$($result.cleanPositiveActualPath)"
    }
    $result.cleanPositivePassed = $true
    $result.cleanPositiveDeleteSucceeded = [RangeFea.V43.OutputDirectoryGuard]::DeleteFileByHandle($cleanOutputHandle)
    if (-not $result.cleanPositiveDeleteSucceeded) {
        throw "Clean relative-create fixture could not delete its empty file by handle (Win32=$([Runtime.InteropServices.Marshal]::GetLastWin32Error()))."
    }
    $cleanOutputHandle.Dispose()
    $cleanOutputHandle = $null
    $cleanDirectoryHandle.Dispose()
    $cleanDirectoryHandle = $null
    if ([IO.File]::Exists($result.cleanPositiveExpectedPath)) { throw 'Clean positive relative-create fixture left its output receipt behind.' }

    $mutatedWriteError = 0
    $mutatedDirectoryHandle = [RangeFea.V43.Testing.RelativeCreateReparseFixture]::OpenWritableDirectory(
        $result.mutatedGuardDirectory, [ref]$mutatedWriteError)
    $result.writeHandleWin32Error = $mutatedWriteError
    $result.writeHandleOpenSucceeded = ($null -ne $mutatedDirectoryHandle -and -not $mutatedDirectoryHandle.IsInvalid)
    if (-not $result.writeHandleOpenSucceeded) {
        $result.status = 'UNRESOLVED_WRITE_HANDLE_DENIED'
        $result.failure = 'Host refused the test-only write-capable directory handle; the mutated-directory branch was not attempted.'
    } else {
        $result.fsctlSetAttempted = $true
        $setError = 0
        $result.fsctlSetSucceeded = [RangeFea.V43.Testing.RelativeCreateReparseFixture]::TryAttachMountPointThroughSameHandle(
            $mutatedDirectoryHandle, $result.reparseTargetDirectory, [ref]$setError)
        $result.fsctlSetWin32Error = $setError
        $junctionMayExist = $result.fsctlSetSucceeded
        if (-not $result.fsctlSetSucceeded) {
            try {
                $guardAttributes = [IO.File]::GetAttributes($result.mutatedGuardDirectory)
                $junctionMayExist = (($guardAttributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
            } catch { $junctionMayExist = $false }
            $result.status = 'UNRESOLVED_FSCTL_SET_DENIED'
            $result.failure = 'Host refused FSCTL_SET_REPARSE_POINT on the held test-only handle; the mutated-directory branch was not attempted.'
        } else {
            $result.relativeCreateAttempted = $true
            $result.relativeCreateExpectedGuardPath = [IO.Path]::GetFullPath((Join-Path $result.mutatedGuardDirectory $mutatedOutputLeaf))
            $result.relativeCreateExpectedTargetPath = [IO.Path]::GetFullPath((Join-Path $result.reparseTargetDirectory $mutatedOutputLeaf))
            try {
                $mutatedOutputHandle = [RangeFea.V43.OutputDirectoryGuard]::CreateNewFileRelative(
                    $mutatedDirectoryHandle, $mutatedOutputLeaf)
                $result.relativeCreateHandleReturned = $true
                $result.relativeCreateActualFinalPath = [IO.Path]::GetFullPath(
                    [RangeFea.V43.OutputDirectoryGuard]::GetFinalPath($mutatedOutputHandle))
                $targetOutputExists = [IO.File]::Exists($result.relativeCreateExpectedTargetPath)
                $result.redirectedOutputAppearedInTarget = $targetOutputExists
                $result.relativeCreateContainedIdentity = (
                    $result.relativeCreateActualFinalPath.Equals($result.relativeCreateExpectedGuardPath, [StringComparison]::OrdinalIgnoreCase) -and
                    -not $targetOutputExists)
                if ($result.relativeCreateContainedIdentity) {
                    $result.status = 'PASS_CONTAINED_IDENTITY'
                } else {
                    $result.status = 'FAIL_ESCAPED_OR_UNPROVEN_IDENTITY'
                    $result.failure = "Relative create returned an output outside the unmutated guard identity: expected=$($result.relativeCreateExpectedGuardPath) actual=$($result.relativeCreateActualFinalPath) targetFileExists=$targetOutputExists"
                }
            }
            catch [System.ComponentModel.Win32Exception] {
                $result.relativeCreateNativeErrorCode = $_.Exception.NativeErrorCode
                $result.relativeCreateExceptionMessage = $_.Exception.Message
                $statusMatch = [regex]::Match($_.Exception.Message, 'NTSTATUS=(0x[0-9a-fA-F]{8})\)\.$')
                if ($statusMatch.Success) { $result.relativeCreateNtStatus = $statusMatch.Groups[1].Value.ToLowerInvariant() }
                $expectedWin32Error = switch -CaseSensitive ($result.relativeCreateNtStatus) {
                    '0xc000050b' { 4398; break }
                    '0xc0000280' { 1921; break }
                    default { 0 }
                }
                $expectedStatusMessage = '^Could not create a single-use receipt relative to the held output directory handle \(NTSTATUS=' +
                    [regex]::Escape([string]$result.relativeCreateNtStatus) + '\)\.$'
                $result.relativeCreateRefusedByWhitelistedReparseStatus = (
                    $expectedWin32Error -ne 0 -and
                    $result.relativeCreateNativeErrorCode -eq $expectedWin32Error -and
                    $_.Exception.Message -match $expectedStatusMessage
                )
                if ($result.relativeCreateRefusedByWhitelistedReparseStatus) {
                    $result.status = 'PASS_REPARSE_REFUSED'
                } else {
                    $result.status = 'FAIL_UNEXPECTED_RELATIVE_CREATE_ERROR'
                    $result.failure = $_.Exception.Message
                }
            }
            catch {
                $result.status = 'FAIL_UNEXPECTED_RELATIVE_CREATE_ERROR'
                $result.failure = $_.Exception.Message
            }
        }
    }
}
catch {
    if ($result.status -eq 'NOT_RUN') { $result.status = 'FAIL_SETUP_OR_ASSERTION' }
    $result.failure = $_.Exception.Message
}
finally {
    if ($null -ne $cleanOutputHandle) {
        try {
            $result.cleanPositiveDeleteSucceeded = [RangeFea.V43.OutputDirectoryGuard]::DeleteFileByHandle($cleanOutputHandle)
            if (-not $result.cleanPositiveDeleteSucceeded -and $null -eq $cleanupFailure) {
                $cleanupFailure = "Clean output handle deletion failed (Win32=$([Runtime.InteropServices.Marshal]::GetLastWin32Error()))."
            }
        } catch { if ($null -eq $cleanupFailure) { $cleanupFailure = $_.Exception.Message } }
        finally { $cleanOutputHandle.Dispose(); $cleanOutputHandle = $null }
    }
    if ($null -ne $cleanDirectoryHandle) { $cleanDirectoryHandle.Dispose(); $cleanDirectoryHandle = $null }

    if ($null -ne $mutatedOutputHandle) {
        try {
            if (-not [RangeFea.V43.OutputDirectoryGuard]::DeleteFileByHandle($mutatedOutputHandle) -and $null -eq $cleanupFailure) {
                $cleanupFailure = "Mutated relative output handle deletion failed (Win32=$([Runtime.InteropServices.Marshal]::GetLastWin32Error()))."
            }
        } catch { if ($null -eq $cleanupFailure) { $cleanupFailure = $_.Exception.Message } }
        finally { $mutatedOutputHandle.Dispose(); $mutatedOutputHandle = $null }
    }
    if ($null -ne $mutatedDirectoryHandle) {
        if ($junctionMayExist) {
            $result.junctionCleanupAttempted = $true
            try {
                $deleteError = 0
                $result.junctionCleanupSucceeded = [RangeFea.V43.Testing.RelativeCreateReparseFixture]::TryDeleteMountPointThroughSameHandle(
                    $mutatedDirectoryHandle, [ref]$deleteError)
                $result.junctionCleanupWin32Error = $deleteError
                if (-not $result.junctionCleanupSucceeded -and $null -eq $cleanupFailure) {
                    $cleanupFailure = "Mount-point cleanup through the same handle failed (Win32=$deleteError)."
                }
            } catch { if ($null -eq $cleanupFailure) { $cleanupFailure = $_.Exception.Message } }
        }
        $mutatedDirectoryHandle.Dispose()
        $mutatedDirectoryHandle = $null
    }
}

if ($result.status -eq 'NOT_RUN') { $result.status = 'FAIL_SETUP_OR_ASSERTION' }
if ([IO.File]::Exists($result.relativeCreateExpectedTargetPath)) { $result.redirectedOutputAppearedInTarget = $true }
if ([IO.File]::Exists($sourceSentinel)) { $result.sourceSentinelSha256After = Get-Sha256File $sourceSentinel }
if ([IO.File]::Exists($targetSentinel)) { $result.targetSentinelSha256After = Get-Sha256File $targetSentinel }
if ($result.sourceSentinelSha256After -cne $result.sourceSentinelSha256Before -or
    $result.targetSentinelSha256After -cne $result.targetSentinelSha256Before) {
    $result.status = 'FAIL_SENTINEL_CHANGED'
    $result.failure = 'A synthetic source or target sentinel changed during the reparse fixture.'
}
if (-not [string]::IsNullOrWhiteSpace([string]$result.relativeCreateExpectedGuardPath) -and
    [IO.File]::Exists($result.relativeCreateExpectedGuardPath)) {
    $result.status = 'FAIL_OUTPUT_LEFT_BEHIND'
    $result.failure = 'The mutated-directory relative-create probe left an output file behind.'
}
if ($result.redirectedOutputAppearedInTarget) {
    $result.status = 'FAIL_ESCAPED_OR_UNPROVEN_IDENTITY'
    $result.failure = 'The synthetic reparse target contains a redirected relative-create output.'
}
if ($null -ne $cleanupFailure) {
    $result.failure = $cleanupFailure
    $result.status = 'FAIL_FIXTURE_CLEANUP'
}

if ([IO.Directory]::Exists($testRoot)) {
    if ($result.junctionCleanupAttempted -and -not $result.junctionCleanupSucceeded) {
        $result.tempTreeCleanup = 'RETAINED_REPARSE_CLEANUP_FAILED'
        $result.tempTreeRetained = $true
    } elseif (-not (Test-SafeTemporaryTree $testRoot)) {
        $result.tempTreeCleanup = 'RETAINED_UNSAFE_OR_REPARSE_PATH'
        $result.tempTreeRetained = $true
        $result.status = 'FAIL_FIXTURE_CLEANUP'
        if ($null -eq $result.failure) { $result.failure = 'Temporary fixture tree contained a reparse or out-of-root path; recursive cleanup was refused.' }
    } else {
        try {
            Remove-Item -LiteralPath $testRoot -Recurse -Force
            $result.tempTreeCleanup = 'REMOVED_AFTER_DESCENDANT_AND_REPARSE_CHECK'
        } catch {
            $result.tempTreeCleanup = 'RETAINED_CLEANUP_ERROR'
            $result.tempTreeRetained = $true
            $result.status = 'FAIL_FIXTURE_CLEANUP'
            if ($null -eq $result.failure) { $result.failure = $_.Exception.Message }
        }
    }
}

if ($result.status -eq 'NOT_RUN') { $result.status = 'FAIL_SETUP_OR_ASSERTION' }
$receiptBytes = (New-Object Text.UTF8Encoding($false, $true)).GetBytes(($result | ConvertTo-Json -Depth 20) + "`n")
$receiptStream = [IO.File]::Open($receiptPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $receiptStream.Write($receiptBytes, 0, $receiptBytes.Length) }
finally { $receiptStream.Dispose() }

Write-Output ("{0} relativeCreate status={1} cleanPositive={2} writeHandleError={3} fsctlSetError={4} nativeError={5} ntStatus={6} receipt={7} tempRetained={8}" -f `
    $(if ($result.status -like 'PASS_*') { 'PASS' } elseif ($result.status -like 'UNRESOLVED_*') { 'UNRESOLVED' } else { 'FAIL' }),
    $result.status, $result.cleanPositivePassed, $result.writeHandleWin32Error, $result.fsctlSetWin32Error,
    $result.relativeCreateNativeErrorCode, $result.relativeCreateNtStatus, $receiptPath, $result.tempTreeRetained)

if ($result.status -like 'UNRESOLVED_*') { exit 2 }
if ($result.status -notlike 'PASS_*') { exit 1 }
