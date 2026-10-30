# Cancel only a named native picker owned by the verified audit executable.
param([Parameter(Mandatory=$true)][string]$Manifest,[Parameter(Mandatory=$true)][int]$GuiProcessId,[Parameter(Mandatory=$true)][string]$Title,[switch]$Qt)
$ErrorActionPreference='Stop'
$spec=Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
$gui=Get-Process -Id $GuiProcessId
if ($gui.MainModule.FileName -ine $spec.executable -or (Get-FileHash -LiteralPath $spec.executable).Hash -ine $spec.executableSha256) { throw 'Owned executable identity mismatch.' }
$wpf=Join-Path ([Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory()) 'WPF'
Add-Type -Path (Join-Path $PSScriptRoot 'pc11_gui_uia.cs') -ReferencedAssemblies @((Join-Path $wpf 'UIAutomationClient.dll'),(Join-Path $wpf 'UIAutomationTypes.dll'),(Join-Path $wpf 'WindowsBase.dll'))
if (![Pc11GuiUia]::CancelNativePicker($GuiProcessId,$Title,[bool]$Qt)) { throw 'Named owned picker unavailable.' }
Start-Sleep -Milliseconds 500
[pscustomobject]@{status='OWNED_PICKER_CANCELLED';pid=$GuiProcessId;title=$Title} | ConvertTo-Json
