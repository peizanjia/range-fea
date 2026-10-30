Microsoft.PowerShell.Core\Set-StrictMode -Version Latest

function Assert-V43SystemModuleBoundary {
    [CmdletBinding()]
    param()

    if ($PSVersionTable.PSEdition -ne 'Desktop' -or
        $PSVersionTable.PSVersion.Major -ne 5 -or
        $PSVersionTable.PSVersion.Minor -ne 1) {
        throw 'The v43 system-module boundary requires Windows PowerShell 5.1.'
    }

    $moduleRoot = [IO.Path]::GetFullPath([IO.Path]::Combine($PSHOME, 'Modules'))
    $moduleSpecs = @(
        [pscustomobject]@{
            name = 'Microsoft.PowerShell.Utility'
            commandNames = @('Get-FileHash', 'ConvertFrom-Json', 'ConvertTo-Json', 'New-Object', 'Select-Object', 'Write-Output')
        },
        [pscustomobject]@{
            name = 'Microsoft.PowerShell.Management'
            commandNames = @('Get-Content', 'Get-ChildItem', 'Get-Item', 'Join-Path', 'Start-Process')
        }
    )

    $global:PSModuleAutoLoadingPreference = 'None'
    $moduleResults = [Collections.Generic.List[object]]::new()

    foreach ($commandName in @('ForEach-Object', 'Get-Command', 'Set-StrictMode', 'Where-Object')) {
        $commands = @(& Microsoft.PowerShell.Core\Get-Command -Name $commandName -All -ErrorAction SilentlyContinue)
        $trustedCommandFound = $false
        foreach ($command in $commands) {
            if ($command.CommandType -in @('Alias', 'Function', 'Filter') -and
                [string]$command.ModuleName -ine 'Microsoft.PowerShell.Core') {
                throw "A same-name PowerShell alias/function was preloaded before the system module boundary: $commandName"
            }
            if ([string]$command.ModuleName -ieq 'Microsoft.PowerShell.Core') { $trustedCommandFound = $true }
        }
        if (-not $trustedCommandFound) { throw "Required command is not exported by the expected system module: Microsoft.PowerShell.Core\$commandName" }
    }

    foreach ($spec in $moduleSpecs) {
            $manifestPath = [IO.Path]::GetFullPath([IO.Path]::Combine($moduleRoot, $spec.name, ($spec.name + '.psd1')))
            if (-not [IO.File]::Exists($manifestPath)) {
                throw "Pinned Windows PowerShell module manifest is missing: $manifestPath"
            }

            $loadedBefore = @(& Microsoft.PowerShell.Core\Get-Module -Name $spec.name)
            foreach ($loaded in $loadedBefore) {
                if ([IO.Path]::GetFullPath([string]$loaded.Path) -ine $manifestPath) {
                    throw "A same-name module was preloaded from a non-system path: $($loaded.Path)"
                }
            }

            foreach ($commandName in $spec.commandNames) {
                $preloadedCommands = @(& Microsoft.PowerShell.Core\Get-Command -Name $commandName -All -ErrorAction SilentlyContinue)
                foreach ($preloadedCommand in $preloadedCommands) {
                    if ($preloadedCommand.CommandType -in @('Alias', 'Function', 'Filter') -and
                        [string]$preloadedCommand.ModuleName -ine [string]$spec.name) {
                        throw "A same-name PowerShell alias/function was preloaded before the system module boundary: $commandName"
                    }
                }
            }

            if ($loadedBefore.Count -eq 0) {
                & Microsoft.PowerShell.Core\Import-Module -Name $manifestPath -Global -ErrorAction Stop
            }

            $loadedAfter = @(& Microsoft.PowerShell.Core\Get-Module -Name $spec.name)
            if ($loadedAfter.Count -ne 1 -or [IO.Path]::GetFullPath([string]$loadedAfter[0].Path) -ine $manifestPath) {
                $observedPathList = [Collections.Generic.List[string]]::new()
                foreach ($loadedModule in $loadedAfter) { [void]$observedPathList.Add([string]$loadedModule.Path) }
                $observedPaths = [string]::Join(';', $observedPathList.ToArray())
                throw "The system module did not load from its exact Windows PowerShell path: $($spec.name); expected=$manifestPath; observed=$observedPaths"
            }

            $commandResults = [Collections.Generic.List[object]]::new()
            foreach ($commandName in $spec.commandNames) {
                $commands = @(& Microsoft.PowerShell.Core\Get-Command -Name $commandName -All -ErrorAction SilentlyContinue)
                $trusted = [Collections.Generic.List[object]]::new()
                $shadows = [Collections.Generic.List[object]]::new()
                foreach ($command in $commands) {
                    if ([string]$command.ModuleName -ieq [string]$spec.name) { [void]$trusted.Add($command) }
                    if ($command.CommandType -in @('Alias', 'Function', 'Filter') -and
                        [string]$command.ModuleName -ine [string]$spec.name) { [void]$shadows.Add($command) }
                }
                if ($trusted.Count -eq 0) {
                    throw "Required command is not exported by the expected system module: $($spec.name)\$commandName"
                }

                if ($shadows.Count -gt 0) {
                    throw "A same-name PowerShell alias/function shadows the system command: $commandName"
                }
                [void]$commandResults.Add([pscustomobject]@{
                    name = $commandName
                    module = [string]$trusted[0].ModuleName
                    commandType = [string]$trusted[0].CommandType
                })
            }

            [void]$moduleResults.Add([pscustomobject]@{
                name = [string]$spec.name
                manifestPath = $manifestPath
                loadedPath = [string]$loadedAfter[0].Path
                commands = $commandResults.ToArray()
            })
        }

    return [pscustomobject][ordered]@{
        status = 'PASS'
        powershellEdition = [string]$PSVersionTable.PSEdition
        powershellVersion = [string]$PSVersionTable.PSVersion.ToString()
        moduleRoot = $moduleRoot
        automaticModuleLoadingDisabledDuringCheck = $true
        modules = $moduleResults.ToArray()
    }
}
