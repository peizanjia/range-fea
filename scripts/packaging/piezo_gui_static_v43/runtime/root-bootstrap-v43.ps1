[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CandidateManifestPath,
    [Parameter(Mandatory = $true)][string]$CandidateManifestSha256,
    [Parameter(Mandatory = $true)][string]$FinalStaticReceiptPath,
    [Parameter(Mandatory = $true)][string]$FinalStaticReceiptSha256,
    [Parameter(Mandatory = $true)][string]$IndependentReviewReceiptPath,
    [Parameter(Mandatory = $true)][string]$IndependentReviewReceiptSha256,
    [Parameter(Mandatory = $true)][string]$RootTechnicalDecisionReceiptPath,
    [Parameter(Mandatory = $true)][string]$RootTechnicalDecisionReceiptSha256,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [Parameter(Mandatory = $true)][switch]$VerifyOnly
)

$global:PSModuleAutoLoadingPreference = 'None'
if ($PSVersionTable.PSEdition -ne 'Desktop' -or
    $PSVersionTable.PSVersion.Major -ne 5 -or
    $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'The v43 root bootstrap requires Windows PowerShell 5.1.'
}
if (-not $VerifyOnly) { throw 'The v43 root bootstrap requires explicit -VerifyOnly.' }

$producerRuntimeRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$producerPackageRoot = [IO.Path]::GetFullPath([IO.Path]::Combine($producerRuntimeRoot, '..'))
$producerToolsRoot = [IO.Path]::GetFullPath([IO.Path]::Combine($producerPackageRoot, 'tools'))
$systemModuleHelperPath = [IO.Path]::GetFullPath([IO.Path]::Combine($producerToolsRoot, 'system-module-boundary-v43.ps1'))
$producerPackagePrefix = $producerPackageRoot.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if (-not $systemModuleHelperPath.StartsWith($producerPackagePrefix, [StringComparison]::OrdinalIgnoreCase) -or
    -not [IO.File]::Exists($systemModuleHelperPath)) {
    throw 'The canonical package-local v43 system-module helper is missing or outside its package.'
}
foreach ($trustedPath in @($producerPackageRoot, $producerRuntimeRoot, $producerToolsRoot, $systemModuleHelperPath)) {
    if (([IO.File]::GetAttributes($trustedPath) -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "A v43 system-module boundary path is a reparse point: $trustedPath"
    }
}
$preloadedBoundaryCommands = @(& Microsoft.PowerShell.Core\Get-Command -Name 'Assert-V43SystemModuleBoundary' -All -ListImported -ErrorAction SilentlyContinue)
if ($preloadedBoundaryCommands.Count -gt 0) { throw 'A preloaded Assert-V43SystemModuleBoundary command cannot bypass the trusted v43 helper.' }
. $systemModuleHelperPath
$trustedBoundaryCommand = $null
foreach ($boundaryCommand in @(& Microsoft.PowerShell.Core\Get-Command -Name 'Assert-V43SystemModuleBoundary' -All -ListImported -ErrorAction SilentlyContinue)) {
    if ($boundaryCommand.CommandType -eq 'Function' -and
        [IO.Path]::GetFullPath([string]$boundaryCommand.ScriptBlock.Ast.Extent.File) -ceq $systemModuleHelperPath) {
        $trustedBoundaryCommand = $boundaryCommand
        break
    }
}
if ($null -eq $trustedBoundaryCommand) { throw 'Assert-V43SystemModuleBoundary was not loaded from the canonical package-local helper.' }
$moduleBoundaryResult = & $trustedBoundaryCommand.ScriptBlock
if ($moduleBoundaryResult.status -cne 'PASS') { throw 'The v43 system-module boundary helper did not return PASS.' }

$ErrorActionPreference = 'Stop'

$addTypeCommands = @(& Microsoft.PowerShell.Core\Get-Command -Name 'Add-Type' -All -ListImported -ErrorAction SilentlyContinue)
$trustedAddTypeFound = $false
foreach ($addTypeCommand in $addTypeCommands) {
    if ([string]$addTypeCommand.ModuleName -ieq 'Microsoft.PowerShell.Utility' -and
        $addTypeCommand.CommandType -eq 'Cmdlet') { $trustedAddTypeFound = $true }
    if ($addTypeCommand.CommandType -in @('Alias', 'Function', 'Filter') -and
        [string]$addTypeCommand.ModuleName -ine 'Microsoft.PowerShell.Utility') {
        throw 'A preloaded Add-Type alias/function shadows the v43 system command.'
    }
}
if (-not $trustedAddTypeFound) { throw 'Add-Type is not available from the trusted system Utility module.' }

if ('RangeFea.V43.OutputDirectoryGuard' -as [type]) {
    throw 'A preloaded output directory guard type cannot bypass the trusted v43 native implementation.'
}
if (-not ('RangeFea.V43.OutputDirectoryGuard' -as [type])) {
    $directoryGuardSource = @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace RangeFea.V43 {
    public static class OutputDirectoryGuard {
        private const uint FileReadAttributes = 0x00000080;
        private const uint FileListDirectory = 0x00000001;
        private const uint FileTraverse = 0x00000020;
        private const uint FileShareRead = 0x00000001;
        private const uint GenericWrite = 0x40000000;
        private const uint DeleteAccess = 0x00010000;
        private const uint Synchronize = 0x00100000;
        private const uint OpenExisting = 3;
        private const uint FileCreate = 2;
        private const uint FileAttributeNormal = 0x00000080;
        private const uint FileNonDirectory = 0x00000040;
        private const uint FileSynchronousIoNonAlert = 0x00000020;
        private const uint FileFlagBackupSemantics = 0x02000000;
        private const uint FileFlagOpenReparsePoint = 0x00200000;
        private const uint FileAttributeDirectory = 0x00000010;
        private const uint FileAttributeReparsePoint = 0x00000400;
        private const uint ObjectCaseInsensitive = 0x00000040;
        private const uint ObjectDontReparse = 0x00001000;
        private const int FileDispositionInfo = 4;

        [StructLayout(LayoutKind.Sequential)]
        private struct NativeFileTime {
            public uint Low;
            public uint High;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct ByHandleFileInformation {
            public uint FileAttributes;
            public NativeFileTime CreationTime;
            public NativeFileTime LastAccessTime;
            public NativeFileTime LastWriteTime;
            public uint VolumeSerialNumber;
            public uint FileSizeHigh;
            public uint FileSizeLow;
            public uint NumberOfLinks;
            public uint FileIndexHigh;
            public uint FileIndexLow;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativeUnicodeString {
            public ushort Length;
            public ushort MaximumLength;
            public IntPtr Buffer;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativeObjectAttributes {
            public uint Length;
            public IntPtr RootDirectory;
            public IntPtr ObjectName;
            public uint Attributes;
            public IntPtr SecurityDescriptor;
            public IntPtr SecurityQualityOfService;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct NativeIoStatusBlock {
            public IntPtr Status;
            public IntPtr Information;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeFileHandle CreateFile(
            string path, uint desiredAccess, uint shareMode, IntPtr securityAttributes,
            uint creationDisposition, uint flagsAndAttributes, IntPtr templateFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetFileInformationByHandle(
            SafeFileHandle file, out ByHandleFileInformation information);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetFinalPathNameByHandle(
            SafeFileHandle file, StringBuilder path, uint pathLength, uint flags);

        [DllImport("ntdll.dll")]
        private static extern int NtCreateFile(
            out IntPtr fileHandle, uint desiredAccess, ref NativeObjectAttributes objectAttributes,
            out NativeIoStatusBlock ioStatusBlock, IntPtr allocationSize, uint fileAttributes,
            uint shareAccess, uint createDisposition, uint createOptions, IntPtr eaBuffer, uint eaLength);

        [DllImport("ntdll.dll")]
        private static extern uint RtlNtStatusToDosError(int status);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetFileInformationByHandle(
            SafeFileHandle file, int fileInformationClass, IntPtr fileInformation, uint bufferSize);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool WriteFile(
            SafeFileHandle file, byte[] buffer, uint numberOfBytesToWrite, out uint numberOfBytesWritten,
            IntPtr overlapped);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool FlushFileBuffers(SafeFileHandle file);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr handle);

        public static SafeFileHandle OpenDirectory(string path) {
            SafeFileHandle handle = CreateFile(
                path, FileReadAttributes | FileListDirectory | FileTraverse, FileShareRead, IntPtr.Zero,
                OpenExisting, FileFlagBackupSemantics | FileFlagOpenReparsePoint, IntPtr.Zero);
            if (handle == null || handle.IsInvalid) {
                int error = Marshal.GetLastWin32Error();
                if (handle != null) handle.Dispose();
                throw new Win32Exception(error, "Could not hold output directory path stable: " + path);
            }
            return handle;
        }

        public static SafeFileHandle CreateNewFileRelative(SafeFileHandle parentDirectory, string leafName) {
            if (parentDirectory == null || parentDirectory.IsInvalid || parentDirectory.IsClosed)
                throw new InvalidOperationException("The held output parent handle is not open.");
            if (String.IsNullOrWhiteSpace(leafName) || leafName == "." || leafName == ".." ||
                leafName.IndexOfAny(new char[] { '\\', '/' }) >= 0 || leafName.IndexOf(':') >= 0)
                throw new ArgumentException("Output receipt must be a single relative file name.", "leafName");

            byte[] nameBytes = Encoding.Unicode.GetBytes(leafName);
            if (nameBytes.Length > UInt16.MaxValue - 2)
                throw new ArgumentException("Output receipt file name is too long.", "leafName");
            IntPtr nameBuffer = Marshal.StringToHGlobalUni(leafName);
            IntPtr nameStructure = IntPtr.Zero;
            bool parentReferenceAdded = false;
            try {
                NativeUnicodeString name = new NativeUnicodeString();
                name.Length = (ushort)nameBytes.Length;
                name.MaximumLength = (ushort)(nameBytes.Length + 2);
                name.Buffer = nameBuffer;
                nameStructure = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(NativeUnicodeString)));
                Marshal.StructureToPtr(name, nameStructure, false);

                parentDirectory.DangerousAddRef(ref parentReferenceAdded);
                NativeObjectAttributes attributes = new NativeObjectAttributes();
                attributes.Length = (uint)Marshal.SizeOf(typeof(NativeObjectAttributes));
                attributes.RootDirectory = parentDirectory.DangerousGetHandle();
                attributes.ObjectName = nameStructure;
                attributes.Attributes = ObjectCaseInsensitive | ObjectDontReparse;
                NativeIoStatusBlock ioStatus;
                IntPtr rawHandle;
                int status = NtCreateFile(out rawHandle,
                    GenericWrite | DeleteAccess | Synchronize,
                    ref attributes, out ioStatus, IntPtr.Zero, FileAttributeNormal, 0,
                    FileCreate, FileNonDirectory | FileSynchronousIoNonAlert | FileFlagOpenReparsePoint,
                    IntPtr.Zero, 0);
                if (status < 0) {
                    if (rawHandle != IntPtr.Zero && rawHandle != new IntPtr(-1)) CloseHandle(rawHandle);
                    uint win32Error = RtlNtStatusToDosError(status);
                    throw new Win32Exception((int)win32Error,
                        "Could not create a single-use receipt relative to the held output directory handle (NTSTATUS=0x" + status.ToString("x8") + ").");
                }
                if (rawHandle == IntPtr.Zero || rawHandle == new IntPtr(-1))
                    throw new InvalidOperationException("NtCreateFile returned an invalid output receipt handle.");
                return new SafeFileHandle(rawHandle, true);
            }
            finally {
                if (parentReferenceAdded) parentDirectory.DangerousRelease();
                if (nameStructure != IntPtr.Zero) Marshal.FreeHGlobal(nameStructure);
                Marshal.FreeHGlobal(nameBuffer);
            }
        }

        public static void WriteAndFlush(SafeFileHandle file, byte[] bytes) {
            uint written;
            if (!WriteFile(file, bytes, (uint)bytes.Length, out written, IntPtr.Zero))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not write the root-bootstrap receipt handle.");
            if (written != bytes.Length)
                throw new IOException("The root-bootstrap receipt handle accepted only a partial write.");
            if (!FlushFileBuffers(file))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not flush the root-bootstrap receipt handle.");
        }

        public static bool DeleteFileByHandle(SafeFileHandle file) {
            IntPtr disposition = Marshal.AllocHGlobal(1);
            try {
                Marshal.WriteByte(disposition, 1);
                return SetFileInformationByHandle(file, FileDispositionInfo, disposition, 1);
            }
            finally { Marshal.FreeHGlobal(disposition); }
        }

        public static uint GetDirectoryAttributes(SafeFileHandle handle) {
            ByHandleFileInformation information;
            if (!GetFileInformationByHandle(handle, out information))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not inspect held output directory handle.");
            uint attributes = information.FileAttributes;
            if ((attributes & FileAttributeDirectory) == 0 || (attributes & FileAttributeReparsePoint) != 0)
                throw new InvalidOperationException("Output path component is not a regular directory.");
            return attributes;
        }

        public static string GetFinalPath(SafeFileHandle handle) {
            StringBuilder buffer = new StringBuilder(32768);
            uint length = GetFinalPathNameByHandle(handle, buffer, (uint)buffer.Capacity, 0);
            if (length == 0 || length >= buffer.Capacity)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not resolve held output directory identity.");
            string path = buffer.ToString();
            if (path.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase))
                return @"\\" + path.Substring(8);
            if (path.StartsWith(@"\\?\", StringComparison.OrdinalIgnoreCase))
                return path.Substring(4);
            return path;
        }
    }
}
'@
    Microsoft.PowerShell.Utility\Add-Type -TypeDefinition $directoryGuardSource -Language CSharp -ErrorAction Stop
}
Set-StrictMode -Version Latest

$script:RequiredStaticTestIds = @(
    'source_preflight', 'source_freeze', 'package_tree', 'module_boundary_build', 'module_boundary_source',
    'watchdog_preflight', 'save_predicate_preflight', 'path_shadow', 'root_seal', 'save_predicate',
    'watchdog_contract', 'sampler_arguments', 'supervisor', 'worker_handshake', 'native_launch_boundary',
    'parser', 'finalize_integrity', 'source_static_tests', 'root_bootstrap'
)
$script:RequiredProductActions = @('guiLaunched', 'savePerformed', 'runPerformed', 'confirmPerformed')
$script:Receipt = $null
$script:Failure = $null
$script:OutputBoundaryValidated = $false
$script:OutputBoundaryClassification = $null
$script:OutputDirectoryHandles = $null

function Get-Sha256Bytes([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-Sha256File([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose(); $stream.Dispose() }
}

function Assert-Sha256Value([string]$Value, [string]$Label) {
    if ([string]::IsNullOrWhiteSpace($Value) -or $Value -notmatch '^[0-9a-fA-F]{64}$') {
        throw "$Label must be a 64-character SHA256 value."
    }
    return $Value.ToLowerInvariant()
}

function Test-PathWithin([string]$Path, [string]$Root) {
    $fullPath = [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    if ($fullPath.Equals($fullRoot, [StringComparison]::OrdinalIgnoreCase)) { return $true }
    $prefix = $fullRoot + [IO.Path]::DirectorySeparatorChar
    return $fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
}

function Assert-NoReparseComponents([string]$Path, [bool]$IncludeLeaf) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetPathRoot($fullPath)
    $relative = $fullPath.Substring($root.Length)
    $parts = @($relative.Split(@([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar), [StringSplitOptions]::RemoveEmptyEntries))
    $lastIndex = $parts.Count - 1
    if (-not $IncludeLeaf) { $lastIndex-- }
    $current = $root
    for ($index = 0; $index -le $lastIndex; $index++) {
        $current = [IO.Path]::Combine($current, [string]$parts[$index])
        if ([IO.Directory]::Exists($current) -or [IO.File]::Exists($current)) {
            $attributes = [IO.File]::GetAttributes($current)
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "A reparse point is not allowed in an evidence or output path: $current"
            }
        }
    }
}

function Assert-OutputBoundary([string]$Path, [string]$SourceRoot, [string]$InstallRoot) {
    $fullOutput = [IO.Path]::GetFullPath($Path)
    if (Test-PathWithin $fullOutput $InstallRoot) { throw 'OutputPath must be outside the installed product tree.' }

    # The frozen v43 source contract excludes only the repository-root build/ tree.
    # That is the approved in-checkout location for external audit receipts.
    if ((Test-PathWithin $fullOutput $SourceRoot) -and
        -not (Test-PathWithin $fullOutput (Join-Path $SourceRoot 'build'))) {
        throw 'OutputPath must be outside the frozen source tree or under its excluded repository-root build/ tree.'
    }
    Assert-NoReparseComponents $fullOutput $false
}

function Normalize-DirectoryIdentityPath([string]$Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetPathRoot($fullPath)
    if (-not $fullPath.Equals($root, [StringComparison]::OrdinalIgnoreCase)) {
        $fullPath = $fullPath.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    }
    return $fullPath
}

function Lock-OutputDirectoryChain([string]$DirectoryPath) {
    $fullDirectory = [IO.Path]::GetFullPath($DirectoryPath)
    if (-not [IO.Directory]::Exists($fullDirectory)) { throw "Output parent directory is missing: $fullDirectory" }
    $root = [IO.Path]::GetPathRoot($fullDirectory)
    $relative = $fullDirectory.Substring($root.Length)
    $parts = @($relative.Split(@([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar), [StringSplitOptions]::RemoveEmptyEntries))
    $components = [Collections.Generic.List[string]]::new()
    [void]$components.Add($root)
    $current = $root
    foreach ($part in $parts) {
        $current = [IO.Path]::Combine($current, [string]$part)
        [void]$components.Add($current)
    }

    $handles = [Collections.Generic.List[Microsoft.Win32.SafeHandles.SafeFileHandle]]::new()
    try {
        foreach ($component in $components) {
            $handle = [RangeFea.V43.OutputDirectoryGuard]::OpenDirectory($component)
            try {
                [void][RangeFea.V43.OutputDirectoryGuard]::GetDirectoryAttributes($handle)
                $actualIdentity = Normalize-DirectoryIdentityPath ([RangeFea.V43.OutputDirectoryGuard]::GetFinalPath($handle))
                $expectedIdentity = Normalize-DirectoryIdentityPath $component
                if (-not $actualIdentity.Equals($expectedIdentity, [StringComparison]::OrdinalIgnoreCase)) {
                    throw "Output directory handle resolves to an unexpected path: expected=$expectedIdentity actual=$actualIdentity"
                }
                [void]$handles.Add($handle)
                $handle = $null
            }
            finally {
                if ($null -ne $handle) { $handle.Dispose() }
            }
        }
        return ,$handles.ToArray()
    }
    catch {
        foreach ($handle in $handles) { $handle.Dispose() }
        throw
    }
}

function Dispose-OutputDirectoryHandles {
    if ($null -ne $script:OutputDirectoryHandles) {
        foreach ($handle in @($script:OutputDirectoryHandles)) {
            if ($null -ne $handle) { $handle.Dispose() }
        }
        $script:OutputDirectoryHandles = $null
    }
}

function Read-PinnedJsonObject([string]$Path, [string]$ExpectedSha256, [string]$Label) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "$Label path is required." }
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not [IO.File]::Exists($fullPath)) { throw "$Label file is missing: $fullPath" }
    Assert-NoReparseComponents $fullPath $true
    $attributes = [IO.File]::GetAttributes($fullPath)
    if (($attributes -band [IO.FileAttributes]::Directory) -ne 0) { throw "$Label path is not a regular file." }
    $expected = Assert-Sha256Value $ExpectedSha256 "$Label external pin"
    $bytes = [IO.File]::ReadAllBytes($fullPath)
    $actual = Get-Sha256Bytes $bytes
    if ($actual -cne $expected) { throw "$Label bytes do not match the externally supplied SHA256 pin." }
    $text = (New-Object Text.UTF8Encoding($false, $true)).GetString($bytes)
    try { $value = $text | ConvertFrom-Json -ErrorAction Stop }
    catch { throw "$Label is not valid UTF-8 JSON: $($_.Exception.Message)" }
    if ($null -eq $value -or $value -isnot [pscustomobject]) { throw "$Label must contain a JSON object." }
    return [pscustomobject]@{ path = $fullPath; sha256 = $actual; document = $value }
}

function Get-RequiredProperty($Object, [string]$Name, [string]$Label, [ValidateSet('String', 'Boolean', 'Object', 'Array')][string]$ExpectedKind) {
    if ([string]::IsNullOrWhiteSpace($ExpectedKind)) { throw 'Get-RequiredProperty requires an explicit expected JSON kind.' }
    if ($null -eq $Object) { throw "$Label is missing." }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { throw "$Label is missing required field '$Name'." }
    $value = $property.Value
    if ($ExpectedKind -ne 'Array' -and $value -is [array]) {
        throw "$Label field '$Name' must not be a JSON array."
    }
    switch ($ExpectedKind) {
        'String' { if ($value -isnot [string]) { throw "$Label field '$Name' must be a JSON string." } }
        'Boolean' { if ($value -isnot [bool]) { throw "$Label field '$Name' must be a JSON boolean." } }
        'Object' { if ($value -isnot [pscustomobject]) { throw "$Label field '$Name' must be a JSON object." } }
        'Array' { if ($value -isnot [array]) { throw "$Label field '$Name' must be a JSON array." } }
    }
    return ,$value
}

function Assert-NoGoBoundary($Object, [string]$Label) {
    if ($Object -isnot [pscustomobject]) { throw "$Label must be a JSON object." }
    $candidateVersion = Get-RequiredProperty $Object 'candidateVersion' $Label 'String'
    $variant = Get-RequiredProperty $Object 'variant' $Label 'String'
    $staticOnly = Get-RequiredProperty $Object 'staticOnly' $Label 'Boolean'
    $goPermitted = Get-RequiredProperty $Object 'goPermitted' $Label 'Boolean'
    if ($candidateVersion -cne 'v43' -or $variant -cne 'OFF' -or
        $staticOnly -isnot [bool] -or $staticOnly -ne $true -or
        $goPermitted -isnot [bool] -or $goPermitted -ne $false) {
        throw "$Label does not preserve the v43 OFF static-only no-GO boundary."
    }
    $actions = Get-RequiredProperty $Object 'productActions' $Label 'Object'
    $actionNames = @($actions.PSObject.Properties | ForEach-Object { [string]$_.Name })
    if ($actionNames.Count -ne $script:RequiredProductActions.Count) { throw "$Label productActions fields differ from the frozen v43 boundary." }
    foreach ($name in $script:RequiredProductActions) {
        if ($actionNames -cnotcontains $name) { throw "$Label productActions is missing '$name'." }
        $value = Get-RequiredProperty $actions $name "$Label productActions" 'Boolean'
        if ($value -ne $false) { throw "$Label permits product action '$name'." }
    }
}

function Assert-CandidateManifestNoGo($Manifest) {
    if ($Manifest -isnot [pscustomobject]) { throw 'Candidate manifest must be a JSON object.' }
    $goPermitted = Get-RequiredProperty $Manifest 'goPermitted' 'Candidate manifest' 'Boolean'
    if ($goPermitted -ne $false) {
        throw 'Candidate manifest does not preserve goPermitted=false.'
    }
    $binding = Get-RequiredProperty $Manifest 'installBinding' 'Candidate manifest' 'Object'
    if ((Get-RequiredProperty $binding 'requiredVariant' 'Candidate installBinding' 'String') -cne 'OFF') {
        throw 'Candidate manifest requiredVariant must be OFF.'
    }
    $actions = Get-RequiredProperty $Manifest 'productActions' 'Candidate manifest' 'Object'
    $actionNames = @($actions.PSObject.Properties | ForEach-Object { [string]$_.Name })
    if ($actionNames.Count -ne $script:RequiredProductActions.Count) { throw 'Candidate manifest productActions fields differ from the frozen v43 boundary.' }
    foreach ($name in $script:RequiredProductActions) {
        if ($actionNames -cnotcontains $name) { throw "Candidate manifest productActions is missing '$name'." }
        $value = Get-RequiredProperty $actions $name 'Candidate manifest productActions' 'Boolean'
        if ($value -ne $false) { throw "Candidate manifest permits product action '$name'." }
    }
}

function ConvertTo-JsonString([string]$Value) {
    return [string]($Value | ConvertTo-Json -Compress -ErrorAction Stop)
}

function Assert-SubmoduleCommitArray($Submodules, [string]$Label) {
    if ($Submodules -isnot [array]) { throw "$Label must be a JSON array." }
    $expectedNames = @('headCommit', 'path', 'registeredCommit', 'sourceTreeSha256', 'worktreeStatus')
    $seenPaths = @{}
    for ($index = 0; $index -lt $Submodules.Count; $index++) {
        $row = $Submodules[$index]
        if ($row -isnot [pscustomobject]) { throw "$Label[$index] must be a JSON object." }
        $actualNames = @($row.PSObject.Properties | ForEach-Object { [string]$_.Name } | Sort-Object -CaseSensitive)
        $sortedExpectedNames = @($expectedNames | Sort-Object -CaseSensitive)
        if ($actualNames.Count -ne $sortedExpectedNames.Count) { throw "$Label[$index] has an invalid field set." }
        for ($fieldIndex = 0; $fieldIndex -lt $sortedExpectedNames.Count; $fieldIndex++) {
            if ($actualNames[$fieldIndex] -cne $sortedExpectedNames[$fieldIndex]) { throw "$Label[$index] has an invalid field set." }
        }
        $path = Get-RequiredProperty $row 'path' "$Label[$index]" 'String'
        $registeredCommit = Get-RequiredProperty $row 'registeredCommit' "$Label[$index]" 'String'
        $headCommit = Get-RequiredProperty $row 'headCommit' "$Label[$index]" 'String'
        $sourceTreeSha = Get-RequiredProperty $row 'sourceTreeSha256' "$Label[$index]" 'String'
        $worktreeStatus = Get-RequiredProperty $row 'worktreeStatus' "$Label[$index]" 'String'
        if ([string]::IsNullOrWhiteSpace($path) -or $path.StartsWith('/', [StringComparison]::Ordinal) -or
            $path.Contains('\') -or $path -match '(^|/)\.{1,2}(/|$)') {
            throw "$Label[$index].path must be a normalized relative POSIX path."
        }
        $pathKey = $path.ToLowerInvariant()
        if ($seenPaths.ContainsKey($pathKey)) { throw "$Label contains duplicate or case-colliding submodule paths." }
        $seenPaths[$pathKey] = $true
        if ($registeredCommit -notmatch '^(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})$' -or
            $headCommit -notmatch '^(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})$') {
            throw "$Label[$index] has a malformed commit pin."
        }
        [void](Assert-Sha256Value $sourceTreeSha "$Label[$index] sourceTreeSha256")
        if ([string]::IsNullOrWhiteSpace($worktreeStatus)) { throw "$Label[$index].worktreeStatus must be a non-empty string." }
    }
}

function Get-CandidateManifestIdentitySha256($Manifest) {
    if ($Manifest -isnot [pscustomobject]) { throw 'Candidate manifest must be a JSON object.' }
    $binding = Get-RequiredProperty $Manifest 'installBinding' 'Candidate manifest' 'Object'
    $source = Get-RequiredProperty $Manifest 'sourceAndAuthorityPins' 'Candidate manifest' 'Object'
    $candidateVersion = Get-RequiredProperty $Manifest 'candidateVersion' 'Candidate manifest' 'String'
    if ($candidateVersion -cne 'v43') { throw 'Candidate manifest candidateVersion must be v43.' }

    $installRoot = Get-RequiredProperty $binding 'installRoot' 'Candidate installBinding' 'String'
    $inventorySha = Assert-Sha256Value (Get-RequiredProperty $binding 'inventoryReportSha256' 'Candidate installBinding' 'String') 'Inventory report SHA256'
    $releaseBuildSha = Assert-Sha256Value (Get-RequiredProperty $source 'releaseBuildReceiptSha256' 'Candidate sourceAndAuthorityPins' 'String') 'Release build receipt SHA256'
    $releaseBuildPath = Get-RequiredProperty $source 'releaseBuildReceiptPath' 'Candidate sourceAndAuthorityPins' 'String'
    $freezePath = Get-RequiredProperty $source 'sourceFreezeReceiptPath' 'Candidate sourceAndAuthorityPins' 'String'
    $rootCommit = (Get-RequiredProperty $source 'rootCommit' 'Candidate sourceAndAuthorityPins' 'String').ToLowerInvariant()
    $freezeSha = Assert-Sha256Value (Get-RequiredProperty $source 'sourceFreezeReceiptSha256' 'Candidate sourceAndAuthorityPins' 'String') 'Source-freeze SHA256'
    $sourceTreeSha = Assert-Sha256Value (Get-RequiredProperty $source 'sourceTreeSha256' 'Candidate sourceAndAuthorityPins' 'String') 'Source-tree SHA256'
    if ([string]::IsNullOrWhiteSpace($releaseBuildPath) -or $releaseBuildPath -match '^PENDING_' -or
        [string]::IsNullOrWhiteSpace($freezePath) -or $freezePath -match '^PENDING_') {
        throw 'Candidate source/build receipt paths are incomplete.'
    }
    if ([string]::IsNullOrWhiteSpace($installRoot) -or $installRoot -match '^PENDING_') { throw 'Candidate manifest installRoot is incomplete.' }
    if ($rootCommit -notmatch '^(?:[0-9a-f]{40}|[0-9a-f]{64})$') { throw 'Candidate manifest rootCommit is malformed.' }

    Assert-SubmoduleCommitArray (Get-RequiredProperty $source 'submoduleCommits' 'Candidate sourceAndAuthorityPins' 'Array') 'Candidate sourceAndAuthorityPins.submoduleCommits'

    $parts = @(
        '{"candidateVersion":"v43"',
        ',"installRoot":' + (ConvertTo-JsonString $installRoot),
        ',"inventoryReportSha256":' + (ConvertTo-JsonString $inventorySha),
        ',"releaseBuildReceiptSha256":' + (ConvertTo-JsonString $releaseBuildSha),
        ',"rootCommit":' + (ConvertTo-JsonString $rootCommit),
        ',"schema":"range.pc11-fresh-run-v43-admission-candidate/v1"',
        ',"sourceFreezeReceiptSha256":' + (ConvertTo-JsonString $freezeSha),
        ',"sourceTreeSha256":' + (ConvertTo-JsonString $sourceTreeSha) + '}'
    )
    return Get-Sha256Bytes ((New-Object Text.UTF8Encoding($false, $true)).GetBytes([string]::Join('', $parts)))
}

function Assert-ExactIdArray($Actual, [string[]]$Expected, [string]$Label) {
    if ($Actual -isnot [array]) { throw "$Label must be a JSON array." }
    $actualRows = $Actual
    if ($actualRows.Count -ne $Expected.Count) { throw "$Label count differs from the frozen v43 contract." }
    for ($index = 0; $index -lt $Expected.Count; $index++) {
        if ($actualRows[$index] -isnot [string] -or $actualRows[$index] -cne $Expected[$index]) { throw "$Label differs from the frozen v43 contract." }
    }
}

function Assert-OutputPathReady([string]$Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    if ([IO.File]::Exists($fullPath) -or [IO.Directory]::Exists($fullPath)) {
        throw "Refusing to overwrite or reuse existing root-bootstrap output: $fullPath"
    }
    $parent = [IO.Path]::GetDirectoryName($fullPath)
    if ([string]::IsNullOrWhiteSpace($parent)) { throw 'OutputPath must have a parent directory.' }
    if (-not [IO.Directory]::Exists($parent)) {
        throw "Output parent directory must already exist; refusing to create it during VerifyOnly: $parent"
    }
    Assert-NoReparseComponents $fullPath $false
    $script:OutputDirectoryHandles = Lock-OutputDirectoryChain $parent
    return $fullPath
}

function Write-SingleUseReceipt([string]$Path, $Value) {
    $json = $Value | ConvertTo-Json -Depth 20
    $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes($json + "`n")
    $directoryHandles = @($script:OutputDirectoryHandles)
    if ($directoryHandles.Count -eq 0) { throw 'The output parent directory identity is not held.' }
    $parentHandle = $directoryHandles[$directoryHandles.Count - 1]
    $parentPath = [IO.Path]::GetDirectoryName($Path)
    [void][RangeFea.V43.OutputDirectoryGuard]::GetDirectoryAttributes($parentHandle)
    $heldParentPath = Normalize-DirectoryIdentityPath ([RangeFea.V43.OutputDirectoryGuard]::GetFinalPath($parentHandle))
    $expectedParentPath = Normalize-DirectoryIdentityPath $parentPath
    if (-not $heldParentPath.Equals($expectedParentPath, [StringComparison]::OrdinalIgnoreCase)) {
        throw "The held output parent identity changed before receipt creation: expected=$expectedParentPath actual=$heldParentPath"
    }

    $fileHandle = [RangeFea.V43.OutputDirectoryGuard]::CreateNewFileRelative(
        $parentHandle, [IO.Path]::GetFileName($Path))
    try {
        $actualFilePath = Normalize-DirectoryIdentityPath ([RangeFea.V43.OutputDirectoryGuard]::GetFinalPath($fileHandle))
        $expectedFilePath = Normalize-DirectoryIdentityPath $Path
        if (-not $actualFilePath.Equals($expectedFilePath, [StringComparison]::OrdinalIgnoreCase)) {
            if (-not [RangeFea.V43.OutputDirectoryGuard]::DeleteFileByHandle($fileHandle)) {
                throw "Receipt handle identity differs from OutputPath and the unexpected empty file could not be deleted: expected=$expectedFilePath actual=$actualFilePath"
            }
            throw "Receipt handle identity differs from OutputPath: expected=$expectedFilePath actual=$actualFilePath"
        }
        [RangeFea.V43.OutputDirectoryGuard]::WriteAndFlush($fileHandle, $bytes)
    }
    finally { $fileHandle.Dispose() }
}

$outputFullPath = $null
try {
    $outputFullPath = [IO.Path]::GetFullPath($OutputPath)
    if ([IO.File]::Exists($outputFullPath) -or [IO.Directory]::Exists($outputFullPath)) {
        throw "Refusing to overwrite or reuse existing root-bootstrap output: $outputFullPath"
    }

    $manifestInput = Read-PinnedJsonObject $CandidateManifestPath $CandidateManifestSha256 'Candidate manifest'
    $manifest = $manifestInput.document
    # Establish the receipt boundary from the externally pinned candidate before
    # semantic checks, so malformed candidate fields can still produce a safe
    # rejection receipt. These path fields must themselves be scalar strings.
    $sourcePins = Get-RequiredProperty $manifest 'sourceAndAuthorityPins' 'Candidate manifest' 'Object'
    $installBinding = Get-RequiredProperty $manifest 'installBinding' 'Candidate manifest' 'Object'
    $sourceRoot = Get-RequiredProperty $sourcePins 'sourceRootPath' 'Candidate sourceAndAuthorityPins' 'String'
    $installRoot = Get-RequiredProperty $installBinding 'installRoot' 'Candidate installBinding' 'String'
    if ([string]::IsNullOrWhiteSpace($sourceRoot) -or $sourceRoot -match '^PENDING_' -or -not [IO.Directory]::Exists($sourceRoot)) {
        throw 'Candidate sourceRootPath must identify an existing source root.'
    }
    if ([string]::IsNullOrWhiteSpace($installRoot) -or $installRoot -match '^PENDING_' -or -not [IO.Directory]::Exists($installRoot)) {
        throw 'Candidate installRoot must identify an existing installed product tree.'
    }
    $sourceRoot = [IO.Path]::GetFullPath($sourceRoot)
    $installRoot = [IO.Path]::GetFullPath($installRoot)
    Assert-NoReparseComponents $sourceRoot $true
    Assert-NoReparseComponents $installRoot $true
    Assert-OutputBoundary $outputFullPath $sourceRoot $installRoot
    $script:OutputBoundaryClassification = if (Test-PathWithin $outputFullPath $sourceRoot) {
        'EXCLUDED_REPOSITORY_BUILD'
    } else { 'EXTERNAL_TO_SOURCE_TREE' }
    $outputFullPath = Assert-OutputPathReady $outputFullPath
    $script:OutputBoundaryValidated = $true

    if ((Get-RequiredProperty $manifest 'schema' 'Candidate manifest' 'String') -cne 'range.pc11-fresh-run-v43-admission-candidate/v1' -or
        (Get-RequiredProperty $manifest 'candidateVersion' 'Candidate manifest' 'String') -cne 'v43') {
        throw 'Candidate manifest schema or version is invalid.'
    }
    Assert-CandidateManifestNoGo $manifest

    $workflow = Get-RequiredProperty $manifest 'workflowBoundary' 'Candidate manifest' 'Object'
    foreach ($name in @('staticReadinessDoesNotGrantProductGo', 'externalIndependentReviewRequired', 'separateRootTechnicalDecisionRequired', 'goSealRequiredBeforeAnyProductAction')) {
        if ((Get-RequiredProperty $workflow $name 'Candidate workflowBoundary' 'Boolean') -ne $true) {
            throw "Candidate workflowBoundary must preserve '$name'."
        }
    }
    $identitySha = Get-CandidateManifestIdentitySha256 $manifest

    $evidencePins = Get-RequiredProperty $manifest 'candidateEvidencePins' 'Candidate manifest' 'Object'
    Assert-ExactIdArray (Get-RequiredProperty $evidencePins 'requiredStaticTestIds' 'Candidate candidateEvidencePins' 'Array') $script:RequiredStaticTestIds 'Candidate requiredStaticTestIds'
    $workflowReviewPath = Get-RequiredProperty $workflow 'independentReviewReceiptPath' 'Candidate workflowBoundary' 'String'
    $workflowReviewSha = Assert-Sha256Value (Get-RequiredProperty $workflow 'independentReviewReceiptSha256' 'Candidate workflowBoundary' 'String') 'Manifest independent review SHA256'
    $workflowDecisionPath = Get-RequiredProperty $workflow 'rootDecisionReceiptPath' 'Candidate workflowBoundary' 'String'
    $workflowDecisionSha = Assert-Sha256Value (Get-RequiredProperty $workflow 'rootDecisionReceiptSha256' 'Candidate workflowBoundary' 'String') 'Manifest root decision SHA256'

    $finalInput = Read-PinnedJsonObject $FinalStaticReceiptPath $FinalStaticReceiptSha256 'Final static receipt'
    $reviewInput = Read-PinnedJsonObject $IndependentReviewReceiptPath $IndependentReviewReceiptSha256 'Independent review receipt'
    $decisionInput = Read-PinnedJsonObject $RootTechnicalDecisionReceiptPath $RootTechnicalDecisionReceiptSha256 'Root technical decision receipt'
    if (-not ([IO.Path]::GetFullPath($workflowReviewPath)).Equals($reviewInput.path, [StringComparison]::OrdinalIgnoreCase) -or
        $workflowReviewSha -cne $reviewInput.sha256) {
        throw 'Independent review path or SHA256 differs from the candidate manifest external pin.'
    }
    if (-not ([IO.Path]::GetFullPath($workflowDecisionPath)).Equals($decisionInput.path, [StringComparison]::OrdinalIgnoreCase) -or
        $workflowDecisionSha -cne $decisionInput.sha256) {
        throw 'Root technical decision path or SHA256 differs from the candidate manifest external pin.'
    }

    $finalStatic = $finalInput.document
    if ((Get-RequiredProperty $finalStatic 'schema' 'Final static receipt' 'String') -cne 'range.pc11-v43-final-static-receipt/v1' -or
        (Get-RequiredProperty $finalStatic 'status' 'Final static receipt' 'String') -cne 'STATIC_COMPLETE_NO_GO' -or
        (Get-RequiredProperty $finalStatic 'allRequiredStaticTestsPass' 'Final static receipt' 'Boolean') -ne $true) {
        throw 'Final static receipt is not a complete PASS no-GO receipt.'
    }
    Assert-NoGoBoundary $finalStatic 'Final static receipt'
    if ((Assert-Sha256Value (Get-RequiredProperty $finalStatic 'candidateManifestIdentitySha256' 'Final static receipt' 'String') 'Final static candidate identity') -cne $identitySha) {
        throw 'Final static receipt candidate identity differs from the pinned candidate manifest.'
    }
    [void](Assert-Sha256Value (Get-RequiredProperty $finalStatic 'candidateManifestSha256' 'Final static receipt' 'String') 'Final static candidate manifest SHA256')
    $candidateFreezePath = [IO.Path]::GetFullPath((Get-RequiredProperty $sourcePins 'sourceFreezeReceiptPath' 'Candidate sourceAndAuthorityPins' 'String'))
    $finalFreezePath = [IO.Path]::GetFullPath((Get-RequiredProperty $finalStatic 'sourceFreezeReceiptPath' 'Final static receipt' 'String'))
    if ((Assert-Sha256Value (Get-RequiredProperty $finalStatic 'sourceFreezeReceiptSha256' 'Final static receipt' 'String') 'Final static source-freeze SHA256') -cne
        (Assert-Sha256Value (Get-RequiredProperty $sourcePins 'sourceFreezeReceiptSha256' 'Candidate sourceAndAuthorityPins' 'String') 'Candidate source-freeze SHA256') -or
        -not $candidateFreezePath.Equals($finalFreezePath, [StringComparison]::OrdinalIgnoreCase) -or
        (Assert-Sha256Value (Get-RequiredProperty $finalStatic 'sourceTreeSha256' 'Final static receipt' 'String') 'Final static source-tree SHA256') -cne
        (Assert-Sha256Value (Get-RequiredProperty $sourcePins 'sourceTreeSha256' 'Candidate sourceAndAuthorityPins' 'String') 'Candidate source-tree SHA256') -or
        (Assert-Sha256Value (Get-RequiredProperty $finalStatic 'inventoryReportSha256' 'Final static receipt' 'String') 'Final static inventory SHA256') -cne
        (Assert-Sha256Value (Get-RequiredProperty $installBinding 'inventoryReportSha256' 'Candidate installBinding' 'String') 'Candidate inventory SHA256')) {
        throw 'Final static receipt source or inventory pins differ from the candidate manifest.'
    }
    $manifestPackageSha = Assert-Sha256Value (Get-RequiredProperty $evidencePins 'packageTreeReceiptSha256' 'Candidate candidateEvidencePins' 'String') 'Candidate package-tree SHA256'
    $candidatePackagePath = [IO.Path]::GetFullPath((Get-RequiredProperty $evidencePins 'packageTreeReceiptPath' 'Candidate candidateEvidencePins' 'String'))
    $finalPackagePath = [IO.Path]::GetFullPath((Get-RequiredProperty $finalStatic 'packageTreeReceiptPath' 'Final static receipt' 'String'))
    $aggregatePins = Get-RequiredProperty $evidencePins 'staticTestReceipts' 'Candidate candidateEvidencePins' 'Array'
    if ($aggregatePins.Count -ne 1 -or $aggregatePins[0] -isnot [pscustomobject]) {
        throw 'Candidate manifest must pin exactly one v43-static-suite receipt object.'
    }
    if ((Get-RequiredProperty $aggregatePins[0] 'name' 'Candidate aggregate static-test pin' 'String') -cne 'v43-static-suite') {
        throw 'Candidate manifest must pin exactly one v43-static-suite receipt.'
    }
    $aggregatePath = [IO.Path]::GetFullPath((Get-RequiredProperty $aggregatePins[0] 'path' 'Candidate aggregate static-test pin' 'String'))
    $aggregateSha = Assert-Sha256Value (Get-RequiredProperty $aggregatePins[0] 'sha256' 'Candidate aggregate static-test pin' 'String') 'Candidate aggregate static-test SHA256'
    $finalAggregatePath = [IO.Path]::GetFullPath((Get-RequiredProperty $finalStatic 'staticTestReceiptPath' 'Final static receipt' 'String'))
    if ((Assert-Sha256Value (Get-RequiredProperty $finalStatic 'packageTreeReceiptSha256' 'Final static receipt' 'String') 'Final static package-tree SHA256') -cne $manifestPackageSha -or
        -not $candidatePackagePath.Equals($finalPackagePath, [StringComparison]::OrdinalIgnoreCase) -or
        (Assert-Sha256Value (Get-RequiredProperty $finalStatic 'staticTestReceiptSha256' 'Final static receipt' 'String') 'Final static aggregate-test SHA256') -cne $aggregateSha -or
        -not $aggregatePath.Equals($finalAggregatePath, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Final static receipt package or aggregate test pins differ from the candidate manifest.'
    }
    Assert-ExactIdArray (Get-RequiredProperty $finalStatic 'requiredTestIds' 'Final static receipt' 'Array') $script:RequiredStaticTestIds 'Final static requiredTestIds'
    $pendingProperty = $finalStatic.PSObject.Properties['pendingTestIds']
    if ($null -eq $pendingProperty) { throw 'Final static receipt is missing required field pendingTestIds.' }
    $pendingValue = $pendingProperty.Value
    if ($pendingValue -isnot [array]) { throw 'Final static receipt pendingTestIds must be an array.' }
    if ($pendingValue.Count -ne 0) { throw 'Final static receipt still has pending static tests.' }
    $missingProperty = $finalStatic.PSObject.Properties['missingV39EquivalentGates']
    if ($null -eq $missingProperty -or $missingProperty.Value -isnot [array] -or $missingProperty.Value.Count -ne 0) {
        throw 'Final static receipt missingV39EquivalentGates must be an empty array.'
    }
    $results = Get-RequiredProperty $finalStatic 'results' 'Final static receipt' 'Array'
    if ($results.Count -ne $script:RequiredStaticTestIds.Count) { throw 'Final static receipt result count differs from the frozen v43 contract.' }
    for ($index = 0; $index -lt $script:RequiredStaticTestIds.Count; $index++) {
        if ($results[$index] -isnot [pscustomobject] -or
            (Get-RequiredProperty $results[$index] 'testId' 'Final static result' 'String') -cne $script:RequiredStaticTestIds[$index] -or
            (Get-RequiredProperty $results[$index] 'status' 'Final static result' 'String') -cne 'PASS') {
            throw 'Final static receipt has a missing, reordered, or non-PASS required result.'
        }
    }

    $review = $reviewInput.document
    if ((Get-RequiredProperty $review 'schema' 'Independent review receipt' 'String') -cne 'range.pc11-v43-independent-static-review/v1' -or
        (Get-RequiredProperty $review 'status' 'Independent review receipt' 'String') -cne 'PASS' -or
        (Get-RequiredProperty $review 'conclusion' 'Independent review receipt' 'String') -cne 'INDEPENDENT_REVIEW_PASS_NO_GO') {
        throw 'Independent review receipt schema, status, or conclusion is not an accepted v43 review result.'
    }
    Assert-NoGoBoundary $review 'Independent review receipt'
    if ((Assert-Sha256Value (Get-RequiredProperty $review 'candidateManifestIdentitySha256' 'Independent review receipt' 'String') 'Review candidate identity') -cne $identitySha -or
        (Assert-Sha256Value (Get-RequiredProperty $review 'finalStaticReceiptSha256' 'Independent review receipt' 'String') 'Review final-static SHA256') -cne $finalInput.sha256 -or
        (Assert-Sha256Value (Get-RequiredProperty $review 'sourceTreeSha256' 'Independent review receipt' 'String') 'Review source-tree SHA256') -cne
        (Assert-Sha256Value (Get-RequiredProperty $sourcePins 'sourceTreeSha256' 'Candidate sourceAndAuthorityPins' 'String') 'Candidate source-tree SHA256') -or
        (Assert-Sha256Value (Get-RequiredProperty $review 'sourceFreezeReceiptSha256' 'Independent review receipt' 'String') 'Review source-freeze SHA256') -cne
        (Assert-Sha256Value (Get-RequiredProperty $sourcePins 'sourceFreezeReceiptSha256' 'Candidate sourceAndAuthorityPins' 'String') 'Candidate source-freeze SHA256')) {
        throw 'Independent review receipt is not bound to the pinned candidate, final static receipt, and source hashes.'
    }
    $reviewerIdentity = Get-RequiredProperty $review 'reviewerIdentity' 'Independent review receipt' 'String'
    if ([string]::IsNullOrWhiteSpace($reviewerIdentity) -or $reviewerIdentity.Length -gt 200 -or
        $reviewerIdentity -match '^PENDING_' -or $reviewerIdentity -match '[\x00-\x1f\x7f]') {
        throw 'Independent review reviewerIdentity is missing or malformed.'
    }
    $reviewerSourceSha = Assert-Sha256Value (Get-RequiredProperty $review 'reviewerSourceSha256' 'Independent review receipt' 'String') 'Reviewer source SHA256'

    $decision = $decisionInput.document
    if ((Get-RequiredProperty $decision 'schema' 'Root technical decision receipt' 'String') -cne 'range.pc11-v43-root-technical-decision/v1' -or
        (Get-RequiredProperty $decision 'status' 'Root technical decision receipt' 'String') -cne 'PASS' -or
        (Get-RequiredProperty $decision 'decision' 'Root technical decision receipt' 'String') -cne 'APPROVE_ROOT_SEAL') {
        throw 'Root technical decision receipt schema, status, or decision is not an accepted v43 result.'
    }
    Assert-NoGoBoundary $decision 'Root technical decision receipt'
    if ((Assert-Sha256Value (Get-RequiredProperty $decision 'candidateManifestIdentitySha256' 'Root technical decision receipt' 'String') 'Decision candidate identity') -cne $identitySha -or
        (Assert-Sha256Value (Get-RequiredProperty $decision 'finalStaticReceiptSha256' 'Root technical decision receipt' 'String') 'Decision final-static SHA256') -cne $finalInput.sha256 -or
        (Assert-Sha256Value (Get-RequiredProperty $decision 'independentReviewReceiptSha256' 'Root technical decision receipt' 'String') 'Decision review SHA256') -cne $reviewInput.sha256 -or
        (Assert-Sha256Value (Get-RequiredProperty $decision 'sourceTreeSha256' 'Root technical decision receipt' 'String') 'Decision source-tree SHA256') -cne
        (Assert-Sha256Value (Get-RequiredProperty $sourcePins 'sourceTreeSha256' 'Candidate sourceAndAuthorityPins' 'String') 'Candidate source-tree SHA256')) {
        throw 'Root technical decision is not bound to the pinned candidate, final static receipt, review, and source tree.'
    }
    $decisionId = Get-RequiredProperty $decision 'decisionId' 'Root technical decision receipt' 'String'
    $decisionAuthority = Get-RequiredProperty $decision 'decisionAuthorityIdentity' 'Root technical decision receipt' 'String'
    if ([string]::IsNullOrWhiteSpace($decisionId) -or $decisionId.Length -gt 200 -or $decisionId -match '^PENDING_' -or $decisionId -match '[\x00-\x1f\x7f]' -or
        [string]::IsNullOrWhiteSpace($decisionAuthority) -or $decisionAuthority.Length -gt 200 -or $decisionAuthority -match '^PENDING_' -or $decisionAuthority -match '[\x00-\x1f\x7f]') {
        throw 'Root technical decision decisionId or decisionAuthorityIdentity is missing or malformed.'
    }
    $decisionSourceSha = Assert-Sha256Value (Get-RequiredProperty $decision 'decisionSourceSha256' 'Root technical decision receipt' 'String') 'Root decision source SHA256'

    $script:Receipt = [ordered]@{
        schema = 'range.pc11-v43-root-bootstrap-receipt/v1'
        status = 'ROOT_BOOTSTRAP_VERIFIED_NO_GO'
        candidateVersion = 'v43'
        variant = 'OFF'
        staticOnly = $true
        operationMode = 'VERIFY_ONLY'
        verificationScope = 'SCHEMA_STATUS_IDENTITY_AND_SHA256_PIN_CHECKS_ONLY'
        generatedAtUtc = [DateTime]::UtcNow.ToString('o')
        candidateManifestPath = $manifestInput.path
        candidateManifestSha256 = $manifestInput.sha256
        candidateManifestIdentitySha256 = $identitySha
        finalStaticReceiptPath = $finalInput.path
        finalStaticReceiptSha256 = $finalInput.sha256
        finalStaticReceiptStatus = 'STATIC_COMPLETE_NO_GO'
        independentReviewReceiptPath = $reviewInput.path
        independentReviewReceiptSha256 = $reviewInput.sha256
        independentReviewReceiptStatus = 'PASS'
        reviewerIdentity = $reviewerIdentity
        reviewerSourceSha256 = $reviewerSourceSha
        rootTechnicalDecisionReceiptPath = $decisionInput.path
        rootTechnicalDecisionReceiptSha256 = $decisionInput.sha256
        rootTechnicalDecisionStatus = 'PASS'
        rootTechnicalDecision = 'APPROVE_ROOT_SEAL'
        decisionId = $decisionId
        decisionAuthorityIdentity = $decisionAuthority
        decisionSourceSha256 = $decisionSourceSha
        sourceTreeSha256 = (Get-RequiredProperty $sourcePins 'sourceTreeSha256' 'Candidate sourceAndAuthorityPins' 'String').ToLowerInvariant()
        sourceFreezeReceiptSha256 = (Get-RequiredProperty $sourcePins 'sourceFreezeReceiptSha256' 'Candidate sourceAndAuthorityPins' 'String').ToLowerInvariant()
        outputPath = $outputFullPath
        outputBoundary = $script:OutputBoundaryClassification
        outputDirectoryIdentityGuard = 'ALL_ANCESTOR_HANDLES_HELD_WITHOUT_DELETE_SHARING_UNTIL_RECEIPT_CREATE'
        outputDirectoryHandleCount = @($script:OutputDirectoryHandles).Count
        outputOutsideInstallTree = $true
        outputOutsideFrozenSourceScope = $true
        outputMayBeUnderExcludedRepositoryBuildTree = $true
        productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
        goPermitted = $false
    }
}
catch {
    $script:Failure = $_.Exception.Message
    $script:Receipt = $null
}

$bootstrapExitCode = 0
try {
    if ($null -eq $script:Receipt) {
        # Keep a failed verification record when the caller's output location is safe and unused.
        # Failures in output-path validation or creation leave no receipt rather than write inside source/install.
        try {
            if (-not $script:OutputBoundaryValidated) { throw 'Output boundary was not validated against the candidate source and install roots.' }
            if ($null -eq $outputFullPath) { $outputFullPath = [IO.Path]::GetFullPath($OutputPath) }
            if ([IO.File]::Exists($outputFullPath) -or [IO.Directory]::Exists($outputFullPath)) { throw 'Output path is already in use.' }
            $parent = [IO.Path]::GetDirectoryName($outputFullPath)
            if ([string]::IsNullOrWhiteSpace($parent)) { throw 'OutputPath must have a parent directory.' }
            if (-not [IO.Directory]::Exists($parent)) {
                throw "Failure receipt parent directory must already exist; refusing to create it during VerifyOnly: $parent"
            }
            Assert-NoReparseComponents $outputFullPath $false
            if ($null -eq $script:OutputDirectoryHandles) {
                $script:OutputDirectoryHandles = Lock-OutputDirectoryChain $parent
            }
            $script:Receipt = [ordered]@{
                schema = 'range.pc11-v43-root-bootstrap-receipt/v1'
                status = 'ROOT_BOOTSTRAP_REJECTED_NO_GO'
                candidateVersion = 'v43'
                variant = 'OFF'
                staticOnly = $true
                operationMode = 'VERIFY_ONLY'
                generatedAtUtc = [DateTime]::UtcNow.ToString('o')
                failure = $script:Failure
                outputPath = $outputFullPath
                outputDirectoryIdentityGuard = 'ALL_ANCESTOR_HANDLES_HELD_WITHOUT_DELETE_SHARING_UNTIL_RECEIPT_CREATE'
                outputDirectoryHandleCount = @($script:OutputDirectoryHandles).Count
                productActions = [ordered]@{ guiLaunched = $false; savePerformed = $false; runPerformed = $false; confirmPerformed = $false }
                goPermitted = $false
            }
        }
        catch {
            Write-Error ("Root bootstrap rejected and no receipt was written: {0}; output handling: {1}" -f $script:Failure, $_.Exception.Message)
            $bootstrapExitCode = 1
        }
    }

    if ($null -ne $script:Receipt) {
        try { Write-SingleUseReceipt $outputFullPath $script:Receipt }
        catch {
            Write-Error ("Root bootstrap could not write single-use receipt: {0}" -f $_.Exception.Message)
            $bootstrapExitCode = 1
        }
        if ($bootstrapExitCode -eq 0) {
            Write-Output ("{0} goPermitted=false productActions=false output={1}" -f $script:Receipt.status, $outputFullPath)
            if ($script:Receipt.status -cne 'ROOT_BOOTSTRAP_VERIFIED_NO_GO') { $bootstrapExitCode = 1 }
        }
    }
}
finally {
    # The identity locks cover validation and the single-use create, then close on every exit path.
    Dispose-OutputDirectoryHandles
}

if ($bootstrapExitCode -ne 0) { exit 1 }
