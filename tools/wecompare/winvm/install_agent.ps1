# typed into the VM once (winvm.sh bootstrap): copies agent.ps1 from the share and starts it at every logon
Set-ExecutionPolicy -Scope CurrentUser Bypass -Force
New-Item -ItemType Directory -Force C:\winvm | Out-Null
Copy-Item \\10.0.2.4\qemu\agent.ps1 C:\winvm\agent.ps1 -Force
$action = New-ScheduledTaskAction -Execute powershell.exe -Argument '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File C:\winvm\agent.ps1'
$trigger = New-ScheduledTaskTrigger -AtLogOn -User we
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit 0 -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName winvm-agent -Action $action -Trigger $trigger -Settings $settings -Force
Start-ScheduledTask -TaskName winvm-agent
