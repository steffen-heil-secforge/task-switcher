# ============================================================================
#  Task Switcher — Hyper-V two-VM provisioning (RUN ELEVATED)
#  Idempotent. Stages:
#    start  : switch + host IP + answer ISOs + create/configure + start VMs
#             (2GB startup dyn; injects keypresses past the Gen2 "press any key" prompt)
#             then sleeps 90s and logs memory demand so we can confirm Setup booted.
#    finish : wait for install (PowerShell Direct) -> static IP -> deploy -> RDP check
#    all    : both
# ============================================================================
param([string]$Stage = 'all')
$ErrorActionPreference = 'Stop'
$Base      = 'L:\Development\wsl\taskswitcher'
$VmRoot    = 'L:\Virtual Machines\Hyper-V'
$IsoDir    = Join-Path $VmRoot 'iso'
$Win11Iso  = Join-Path $IsoDir 'win11.iso'
$BuildDir  = Join-Path $Base 'repo\build'
$Log       = Join-Path $Base 'provision.log'
$Switch    = 'TaskSwitcher-Test-Switch'
$HostIp    = '192.168.234.1'
$Prefix    = 24
$Cred      = New-Object System.Management.Automation.PSCredential('tsw', (ConvertTo-SecureString 'Tsw.Test-2026' -AsPlainText -Force))
$Vms = @(
  @{ Name='TaskSwitcher-Test-Server'; Computer='TSW-TEST-SERVER'; Ip='192.168.234.10'; Role='server' },
  @{ Name='TaskSwitcher-Test-Client'; Computer='TSW-TEST-CLIENT'; Ip='192.168.234.11'; Role='client' }
)
$ClientVmName = 'TaskSwitcher-Test-Client'
$ServerVmName = 'TaskSwitcher-Test-Server'

function Log($m){ $t = Get-Date -Format 'HH:mm:ss'; "$t  $m" | Out-File -FilePath $Log -Append -Encoding utf8; Write-Host "$t  $m" }

# --- inline data-ISO builder (IMAPI2, no ADK) ---
$isoHelper = @'
public class ISOFile {
  public unsafe static void Create(string Path, object Stream, int BlockSize, int TotalBlocks) {
    int bytes = 0; byte[] buf = new byte[BlockSize];
    var ptr = (System.IntPtr)(&bytes);
    var o = System.IO.File.OpenWrite(Path);
    var i = Stream as System.Runtime.InteropServices.ComTypes.IStream;
    if (o != null) { while (TotalBlocks-- > 0) { i.Read(buf, BlockSize, ptr); o.Write(buf, 0, bytes); } o.Flush(); o.Close(); }
  }
}
'@
if (-not ('ISOFile' -as [type])) {
  $ccp = New-Object System.CodeDom.Compiler.CompilerParameters
  $ccp.CompilerOptions = '/unsafe'
  Add-Type -TypeDefinition $isoHelper -CompilerParameters $ccp   # PS 5.1 has no -CompilerOptions
}

function New-AnswerIso($xmlText, $isoPath){
  $tmp = Join-Path $env:TEMP ('unattend_' + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Force -Path $tmp | Out-Null
  [IO.File]::WriteAllText((Join-Path $tmp 'autounattend.xml'), $xmlText, (New-Object Text.UTF8Encoding($false)))
  $fsi = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
  $fsi.FileSystemsToCreate = 3
  $fsi.VolumeName = 'UNATTEND'
  $fsi.Root.AddTree($tmp, $false)
  $img = $fsi.CreateResultImage()
  if (Test-Path $isoPath) { Remove-Item $isoPath -Force }
  [ISOFile]::Create($isoPath, $img.ImageStream, $img.BlockSize, $img.TotalBlocks)
  Remove-Item $tmp -Recurse -Force
  Log "built answer ISO: $isoPath"
}

# Press keys into a VM to clear the Gen2 "Press any key to boot from CD/DVD" prompt.
function Send-VMBootKeys($vmName){
  $sys = Get-CimInstance -Namespace root\virtualization\v2 -ClassName Msvm_ComputerSystem -Filter "ElementName='$vmName'"
  $kbd = Get-CimAssociatedInstance -InputObject $sys -ResultClassName Msvm_Keyboard
  for ($i=0; $i -lt 20; $i++) {
    Invoke-CimMethod -InputObject $kbd -MethodName TypeKey -Arguments @{ keyCode = [uint32]0x0D } | Out-Null  # ENTER
    Start-Sleep -Milliseconds 700
  }
}

function Do-Start {
  # 0) stop any running test VMs first so their mounted ISOs are free to rebuild
  foreach ($vm in $Vms) {
    $g = Get-VM -Name $vm.Name -ErrorAction SilentlyContinue
    if ($g -and $g.State -ne 'Off') { Stop-VM -Name $vm.Name -TurnOff -Force; Log "pre-stopped $($vm.Name)" }
  }
  # 1) switch + host vNIC
  if (-not (Get-VMSwitch -Name $Switch -ErrorAction SilentlyContinue)) {
    New-VMSwitch -Name $Switch -SwitchType Internal | Out-Null; Log "created Internal switch $Switch"
  } else { Log "switch $Switch exists" }
  $ifIndex = (Get-NetAdapter -Name "vEthernet ($Switch)").ifIndex
  if (-not (Get-NetIPAddress -InterfaceIndex $ifIndex -IPAddress $HostIp -ErrorAction SilentlyContinue)) {
    New-NetIPAddress -InterfaceIndex $ifIndex -IPAddress $HostIp -PrefixLength $Prefix | Out-Null; Log "host vNIC $HostIp/$Prefix"
  } else { Log "host vNIC IP present" }

  # 2) answer ISOs
  $template = Get-Content (Join-Path $Base 'autounattend-template.xml') -Raw
  foreach ($vm in $Vms) {
    $xml = $template.Replace('{COMPUTERNAME}', $vm.Computer)
    New-AnswerIso $xml (Join-Path $IsoDir ("autounattend-" + $vm.Name + ".iso"))
  }

  # 3) create/(re)configure + start, injecting boot keys
  foreach ($vm in $Vms) {
    $vhd = Join-Path $VmRoot ($vm.Name + '.vhdx')
    if (-not (Get-VM -Name $vm.Name -ErrorAction SilentlyContinue)) {
      if (Test-Path $vhd) { Remove-Item $vhd -Force }
      New-VHD -Path $vhd -SizeBytes 64GB -Dynamic | Out-Null
      New-VM -Name $vm.Name -Generation 2 -MemoryStartupBytes 2GB -VHDPath $vhd -SwitchName $Switch | Out-Null
      Set-VMFirmware -VMName $vm.Name -EnableSecureBoot Off
      Add-VMDvdDrive -VMName $vm.Name -Path $Win11Iso
      Add-VMDvdDrive -VMName $vm.Name -Path (Join-Path $IsoDir ("autounattend-" + $vm.Name + ".iso"))
      $dvd = Get-VMDvdDrive -VMName $vm.Name | Where-Object { $_.Path -eq $Win11Iso } | Select-Object -First 1
      Set-VMFirmware -VMName $vm.Name -FirstBootDevice $dvd
      Log "created VM $($vm.Name)"
    } else { Log "VM $($vm.Name) exists" }
    if ((Get-VM -Name $vm.Name).State -ne 'Off') { Stop-VM -Name $vm.Name -TurnOff -Force; Log "turned off $($vm.Name)" }
    Set-VM -Name $vm.Name -ProcessorCount 2
    Set-VMMemory -VMName $vm.Name -DynamicMemoryEnabled $true -StartupBytes 2GB -MinimumBytes 1GB -MaximumBytes 4GB
    Start-VM -Name $vm.Name; Log "started $($vm.Name) (2GB startup, dyn 1-4GB)"
    Send-VMBootKeys $vm.Name; Log "injected boot keypresses -> $($vm.Name)"
  }

  # verify Setup actually booted: after ~90s, Windows Setup (WinPE) drives memory demand up
  Log "sleeping 90s to observe whether Setup booted..."
  Start-Sleep -Seconds 90
  foreach ($vm in $Vms) {
    $g = Get-VM -Name $vm.Name
    Log ("STATE {0}: {1} demandMB={2} cpu%={3}" -f $vm.Name, $g.State, [math]::Round($g.MemoryDemand/1MB), $g.CPUUsage)
  }
  Log "==== start stage DONE (check demandMB: >600 and cpu>0 => Setup running) ===="
}

function Do-Finish {
  Log "waiting for guests to finish install (up to 60 min)..."
  $deadline = (Get-Date).AddMinutes(60)
  $pending = @($Vms.Name)
  while ($pending.Count -gt 0 -and (Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 30
    foreach ($name in @($pending)) {
      try {
        $ok = Invoke-Command -VMName $name -Credential $Cred -ScriptBlock { Test-Path 'C:\Windows\Temp\oobe-done.txt' } -ErrorAction Stop
        if ($ok) { Log "$name : install complete (guest reachable)"; $pending = $pending | Where-Object { $_ -ne $name } }
      } catch { }
    }
  }
  if ($pending.Count -gt 0) { Log "TIMEOUT waiting for: $($pending -join ', ')" }

  $pluginDll = Join-Path $BuildDir 'src\client_plugin\Debug\tsw_plugin.dll'
  $clientExe = Join-Path $BuildDir 'src\client_agent\Debug\tsw_client_agent.exe'
  $serverExe = Join-Path $BuildDir 'src\session_agent\Debug\tsw_session_agent.exe'
  foreach ($vm in $Vms) {
    if ($pending -contains $vm.Name) { Log "skip config for $($vm.Name) (not ready)"; continue }
    $sess = New-PSSession -VMName $vm.Name -Credential $Cred
    Invoke-Command -Session $sess -ScriptBlock {
      param($ip,$pfx)
      $a = Get-NetAdapter | Where-Object Status -eq 'Up' | Select-Object -First 1
      Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.PrefixOrigin -ne 'WellKnown' } | Remove-NetIPAddress -Confirm:$false -ErrorAction SilentlyContinue
      New-NetIPAddress -InterfaceIndex $a.ifIndex -IPAddress $ip -PrefixLength $pfx -ErrorAction SilentlyContinue | Out-Null
      New-Item -ItemType Directory -Force -Path 'C:\TaskSwitcher' | Out-Null
    } -ArgumentList $vm.Ip, $Prefix
    Log "$($vm.Name): static IP $($vm.Ip)"
    if ($vm.Role -eq 'server') {
      Copy-Item -ToSession $sess -Path $serverExe -Destination 'C:\TaskSwitcher\tsw_session_agent.exe' -Force
      Invoke-Command -Session $sess -ScriptBlock {
        reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v TswSession /t REG_SZ /d '"C:\TaskSwitcher\tsw_session_agent.exe"' /f | Out-Null
      }
      Log "$($vm.Name): deployed session agent + Run key"
    } else {
      Copy-Item -ToSession $sess -Path $pluginDll -Destination 'C:\TaskSwitcher\tsw_plugin.dll' -Force
      Copy-Item -ToSession $sess -Path $clientExe -Destination 'C:\TaskSwitcher\tsw_client_agent.exe' -Force
      Invoke-Command -Session $sess -ScriptBlock {
        reg add "HKLM\SOFTWARE\Microsoft\Terminal Server Client\Default\AddIns\TaskSwitcher" /v Name /t REG_SZ /d 'C:\TaskSwitcher\tsw_plugin.dll' /f | Out-Null
      }
      Log "$($vm.Name): deployed plugin+agent, registered DVC AddIn"
    }
    Remove-PSSession $sess
  }
  if (($pending -notcontains $ClientVmName) -and ($pending -notcontains $ServerVmName)) {
    $r = Invoke-Command -VMName $ClientVmName -Credential $Cred -ScriptBlock {
      (Test-NetConnection -ComputerName '192.168.234.10' -Port 3389).TcpTestSucceeded
    }
    Log "RDP reachability client->server:3389 = $r"
  }
}

"==== provision run (stage=$Stage) $(Get-Date -Format o) ====" | Out-File $Log -Append -Encoding utf8
try {
  if ($Stage -eq 'start' -or $Stage -eq 'all') { Do-Start }
  if ($Stage -eq 'finish' -or $Stage -eq 'all') { Do-Finish }
  Log "==== provision ($Stage) DONE ===="
}
catch { Log ("FATAL: " + $_.Exception.Message); Log ($_.ScriptStackTrace) }
