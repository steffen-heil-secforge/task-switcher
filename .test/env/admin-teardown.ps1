# Remove the approve-once elevated runner. Run ELEVATED.
#Requires -RunAsAdministrator
Get-ScheduledTask -TaskName 'TSW-AdminRunner' -ErrorAction SilentlyContinue | Unregister-ScheduledTask -Confirm:$false
Write-Host "Removed scheduled task 'TSW-AdminRunner'."
