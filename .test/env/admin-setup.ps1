# ============================================================================
#  ONE-TIME approval. Run this ELEVATED once (the single UAC prompt). It registers a
#  scheduled task "TSW-AdminRunner" that runs with highest privileges. Afterwards, an
#  UNELEVATED caller can trigger it with `schtasks /Run /TN TSW-AdminRunner` WITHOUT any
#  UAC prompt, and it executes whatever is staged in .admin-payload.ps1 as admin.
#
#  -Base is a scratch directory visible from BOTH Windows (this path) and WSL (the same dir
#  as run-admin.sh's TSW_ADMIN_BASE); payload/result files are handed off through it.
#
#  SECURITY NOTE: after this, anything that can write .admin-payload.ps1 in that folder runs
#  elevated. That's the whole point (convenience for the dev loop) but it IS a local privilege
#  path. Run admin-teardown.ps1 to remove the task when you're done.
# ============================================================================
#Requires -RunAsAdministrator
param([Parameter(Mandatory = $true)][string]$Base)
$ErrorActionPreference = 'Stop'

# Fixed dispatcher: clears markers, runs the staged payload capturing all output, signals done.
# It derives its own folder from $PSCommandPath, so no path is baked in.
$dispatch = @'
$ErrorActionPreference = "Continue"
$b = Split-Path -Parent $PSCommandPath
Remove-Item "$b\.admin.done","$b\.admin.out" -Force -ErrorAction SilentlyContinue
try   { & "$b\.admin-payload.ps1" *> "$b\.admin.out" 2>&1 }
catch { $_ | Out-String | Out-File "$b\.admin.out" -Append }
"done $(Get-Date -Format o)" | Out-File "$b\.admin.done"
'@
Set-Content -Path "$Base\admin-dispatch.ps1" -Value $dispatch -Encoding UTF8

$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$Base\admin-dispatch.ps1`""
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Highest
$settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 30) -MultipleInstances IgnoreNew

Register-ScheduledTask -TaskName 'TSW-AdminRunner' -Action $action -Principal $principal -Settings $settings -Force | Out-Null
Write-Host "Registered scheduled task 'TSW-AdminRunner' (highest privileges)."
Write-Host "Approve-once complete - no more UAC prompts for the dev loop."
