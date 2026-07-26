# Task Switcher — Test Environment Setup Command Log

Purpose: record every command used to stand up the two-VM Hyper-V test rig,
so it can be turned into a fully automated provisioning script later.

## Facts established
- Host user: AzureAD\SteffenHeilsecforge (non-elevated in WSL interop)
- Elevation path: `Start-Process <ps> -Verb RunAs -Wait` (one UAC prompt per batch) — VERIFIED
- Hyper-V feature: Enabled (Microsoft-Hyper-V-All)
- PowerShell: 5.1 at C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe ; pwsh 7 also present
- Windows-side work dir: L:\Development\wsl\taskswitcher  (WSL: /source/windows-development/wsl/taskswitcher)
- VM target dir (per user): L:\Virtual Machines\Hyper-V
- Code repo (WSL): /source/task-switcher  -> origin git@github.com:steffen-heil-secforge/task-switcher.git

## Elevation loop test (VERIFIED working)
Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','L:\Development\wsl\taskswitcher\elev-test.ps1'
# -> elev-test.log: ELEVATED=True, HYPERV_FEATURE=Enabled

## ISO acquisition via Fido
```powershell
Invoke-WebRequest 'https://raw.githubusercontent.com/pbatard/Fido/master/Fido.ps1' -OutFile Fido.ps1
& Fido.ps1 -Win 11 -Ed Enterprise -Lang Eng -Arch x64 -GetUrl   # URL-only probe
```

### Resolved ISO URL (genuine MS, time-limited)
`https://software.download.prss.microsoft.com/dbazure/Win11_25H2_English_x64_v2.iso?t=3363f20a-4ef1-41cc-8d90-f7d7d84bf7b7&P1=1784725906&P2=602&P3=2&P4=HwJL%2fTOBmLwmzQZqUyC%2b1yNIdViLHnKo8PXAa%2bcZgGSvgJZ8Dvcn%2ftu0LF4mwAH3GeCONqf4Pd2YAmF%2fKWSZ2pGgacfmvQzkPKOwewNFOii0X3DAMV5MvsDdR%2fcDka6BQxfsKqD4kV8jNv0kGXSVR3nD4yV%2fiqhwBZjemUFIsmluJyovYsGZVOp97Z94PSfp9eTKUiJwvg9BVBsiMjz0XMJpJgnMk3I4v%2fAmMtxOC5qjGYJMCgwg6ZjSemiaGXozBb5CRs831Ir3nuYaLHqf6nRHN3o0xVbhB6H7qcPTu2s8YWrLs5m9UbPcFZ93oKsHmmlYWrNbZ95kuo3EB4HD1A%3d%3d`

## Toolchain (host)
- Visual Studio Community 2026 (MSVC 19.51.36248), Windows SDK 10.0.26100
- CMake: C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
- DVC header present: tsvirtualchannels.h ; WtsApi32.h present

## Build (host, from a Windows-side copy of the repo)
cmake -S <src> -B <src>\build -G "Visual Studio 18 2026" -A x64
cmake --build <src>\build --config Debug
# Artifacts: tsw_plugin.dll, tsw_agent.exe, tsw_tests.exe (unified agent + build.bat/wsl-build.bat since 1.0.0)
# NOTE: vendor the SINGLE-INCLUDE nlohmann/json.hpp (amalgamated ~26k lines),
#       not the split system header, or MSVC can't find nlohmann/*.hpp siblings.

## ISO
- Fido -> genuine MS CDN URL (Win11 25H2 x64, "Home/Pro/Edu"), 7.89 GB
- Saved: L:\Virtual Machines\Hyper-V\iso\win11.iso

## VM provisioning (elevated, one UAC)
- Networking: Internal switch "TaskSwitcher-Test-Switch", host vNIC 192.168.234.1/24
  (chosen over External because host is Wi-Fi-only; bridged Wi-Fi is unreliable).
- VMs (Gen2, 4GB dyn / 2 vCPU / 64GB dyn disk, SecureBoot off, no vTPM, Win11 checks bypassed):
    TaskSwitcher-Test-Server  -> TSW-TEST-SERVER  192.168.234.10  (session agent)
    TaskSwitcher-Test-Client  -> TSW-TEST-CLIENT  192.168.234.11  (plugin + client agent)
- Answer file: autounattend-template.xml -> per-VM data ISO (IMAPI2, no ADK).
- Mgmt/deploy via PowerShell Direct (no guest network needed). Local admin: tsw / Tsw.Test-2026
- Launch: Start-Process powershell -Verb RunAs -Wait -File provision.ps1  (idempotent)

### Bugs found & fixed (fold into final automation)
1. curl.exe progress meter on stderr + $ErrorActionPreference='Stop' => fatal NativeCommandError.
   Fix: 'Continue' + curl -s + gate on $LASTEXITCODE.
2. PS 5.1 has no `Add-Type -CompilerOptions`. Fix: System.CodeDom CompilerParameters { CompilerOptions='/unsafe' }.
3. autounattend: `wcm:` prefix must be declared on root <unattend> (xmlns:wcm=...), not only per-element.
4. Vendor SINGLE-INCLUDE nlohmann/json.hpp (split system header breaks isolated MSVC build).

### Provisioning fixes that made it work (2026-07-21)
5. Gen2 "Press any key to boot from CD/DVD" prompt blocks unattended install even with
   DVD-first boot order. Fix: inject keypresses via WMI right after Start-VM:
     $sys=Get-CimInstance -Namespace root\virtualization\v2 -ClassName Msvm_ComputerSystem -Filter "ElementName='<vm>'"
     $kbd=Get-CimAssociatedInstance -InputObject $sys -ResultClassName Msvm_Keyboard
     1..20 | % { Invoke-CimMethod -InputObject $kbd -MethodName TypeKey -Arguments @{keyCode=[uint32]0x0D}; sleep -m 700 }
6. Build answer ISOs AFTER stopping any running VM (a running VM locks its mounted ISO).
7. Host RAM tight (27.7GB total, ~6GB free) -> VMs sized 2GB startup / 1-4GB dynamic; two run concurrently.
8. Result: unattended Win11 install ~8 min/VM; PowerShell Direct reachable; RDP client->server:3389 = True.

## PsExec-based in-session test launches (replaces scheduled-task workaround)

```powershell
# one-time per VM: deploy PsExec + accept EULA
Copy-Item -ToSession $s -Path L:\Development\wsl\taskswitcher\PsExec64.exe -Destination 'C:\TaskSwitcher\PsExec64.exe' -Force
Invoke-Command -Session $s { reg add "HKCU\Software\Sysinternals\PsExec" /v EulaAccepted /t REG_DWORD /d 1 /f }

# launch any program in the interactive session (session 1) as the desktop user:
#   -i 1  = interactive session 1;  -d = don't wait;  -u/-p = run as that user (not SYSTEM)
& C:\TaskSwitcher\PsExec64.exe -accepteula -nobanner -i 1 -d -u tsw -p '<password>' notepad.exe

# restart the client agent in-session without a reboot:
& C:\TaskSwitcher\PsExec64.exe -accepteula -nobanner -i 1 -d -u tsw -p '<password>' cmd /c C:\TaskSwitcher\run-client-agent.cmd
```

Notes: PsExec writes progress to stderr (PowerShell shows it as NativeCommandError noise — harmless;
append `2>$null`). Without `-u`, `-i` launches as SYSTEM in that session — windows then have a higher
integrity level, which can break SetForegroundWindow/AttachThreadInput from user-level agents, so
always pass `-u` for task-switcher tests.
