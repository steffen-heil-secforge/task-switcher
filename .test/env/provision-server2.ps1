# Provision a 2nd server VM (TaskSwitcher-Test-Server2) on the existing switch. RUN ELEVATED.
$ErrorActionPreference = 'Stop'
$Base='L:\Development\wsl\taskswitcher'; $VmRoot='L:\Virtual Machines\Hyper-V'; $IsoDir=Join-Path $VmRoot 'iso'
$Win11Iso=Join-Path $IsoDir 'win11.iso'; $Log=Join-Path $Base 'provision-srv2.log'
$BuildDir=Join-Path $Base 'repo\build'; $serverExe=Join-Path $BuildDir 'src\session_agent\Debug\tsw_session_agent.exe'
$Switch='TaskSwitcher-Test-Switch'
$Cred=New-Object System.Management.Automation.PSCredential('tsw',(ConvertTo-SecureString 'Tsw.Test-2026' -AsPlainText -Force))
$Name='TaskSwitcher-Test-Server2'; $Computer='TSW-TEST-SRV2'; $Ip='192.168.234.12'
function Log($m){ $t=Get-Date -Format 'HH:mm:ss'; "$t  $m"|Out-File $Log -Append -Encoding utf8; Write-Host "$t  $m" }
"==== provision-srv2 $(Get-Date -Format o) ====" | Out-File $Log -Append -Encoding utf8

$isoHelper=@'
public class ISOFile2 {
  public unsafe static void Create(string Path, object Stream, int BlockSize, int TotalBlocks) {
    int bytes=0; byte[] buf=new byte[BlockSize]; var ptr=(System.IntPtr)(&bytes);
    var o=System.IO.File.OpenWrite(Path); var i=Stream as System.Runtime.InteropServices.ComTypes.IStream;
    if(o!=null){ while(TotalBlocks-->0){ i.Read(buf,BlockSize,ptr); o.Write(buf,0,bytes);} o.Flush(); o.Close(); } } }
'@
if(-not ('ISOFile2' -as [type])){ $cp=New-Object System.CodeDom.Compiler.CompilerParameters; $cp.CompilerOptions='/unsafe'; Add-Type -TypeDefinition $isoHelper -CompilerParameters $cp }
function New-AnswerIso($xml,$iso){
  $tmp=Join-Path $env:TEMP ('u2_'+[guid]::NewGuid().ToString('N')); New-Item -ItemType Directory -Force -Path $tmp|Out-Null
  [IO.File]::WriteAllText((Join-Path $tmp 'autounattend.xml'),$xml,(New-Object Text.UTF8Encoding($false)))
  $fsi=New-Object -ComObject IMAPI2FS.MsftFileSystemImage; $fsi.FileSystemsToCreate=3; $fsi.VolumeName='UNATTEND'; $fsi.Root.AddTree($tmp,$false)
  $img=$fsi.CreateResultImage(); if(Test-Path $iso){Remove-Item $iso -Force}
  [ISOFile2]::Create($iso,$img.ImageStream,$img.BlockSize,$img.TotalBlocks); Remove-Item $tmp -Recurse -Force; Log "built $iso" }
function Send-VMBootKeys($vm){ $sys=Get-CimInstance -Namespace root\virtualization\v2 -ClassName Msvm_ComputerSystem -Filter "ElementName='$vm'"
  $kbd=Get-CimAssociatedInstance -InputObject $sys -ResultClassName Msvm_Keyboard
  for($i=0;$i -lt 20;$i++){ Invoke-CimMethod -InputObject $kbd -MethodName TypeKey -Arguments @{keyCode=[uint32]0x0D}|Out-Null; Start-Sleep -Milliseconds 700 } }

$xml=(Get-Content (Join-Path $Base 'autounattend-template.xml') -Raw).Replace('{COMPUTERNAME}',$Computer)
New-AnswerIso $xml (Join-Path $IsoDir "autounattend-$Name.iso")

$vhd=Join-Path $VmRoot "$Name.vhdx"
if(-not (Get-VM -Name $Name -EA SilentlyContinue)){
  if(Test-Path $vhd){Remove-Item $vhd -Force}
  New-VHD -Path $vhd -SizeBytes 64GB -Dynamic|Out-Null
  New-VM -Name $Name -Generation 2 -MemoryStartupBytes 2GB -VHDPath $vhd -SwitchName $Switch|Out-Null
  Set-VMFirmware -VMName $Name -EnableSecureBoot Off
  Add-VMDvdDrive -VMName $Name -Path $Win11Iso
  Add-VMDvdDrive -VMName $Name -Path (Join-Path $IsoDir "autounattend-$Name.iso")
  $dvd=Get-VMDvdDrive -VMName $Name|Where-Object {$_.Path -eq $Win11Iso}|Select-Object -First 1
  Set-VMFirmware -VMName $Name -FirstBootDevice $dvd
  Log "created $Name"
} else { Log "$Name exists" }
Set-VM -Name $Name -ProcessorCount 2
Set-VMMemory -VMName $Name -DynamicMemoryEnabled $true -StartupBytes 2GB -MinimumBytes 1GB -MaximumBytes 4GB
if((Get-VM -Name $Name).State -ne 'Off'){ Stop-VM -Name $Name -TurnOff -Force }
Start-VM -Name $Name; Log "started $Name"; Send-VMBootKeys $Name; Log "keys injected"

Log "waiting for install (up to 60 min)..."
$deadline=(Get-Date).AddMinutes(60); $ready=$false
while((Get-Date)-lt $deadline){ Start-Sleep 30
  try{ if(Invoke-Command -VMName $Name -Credential $Cred {Test-Path 'C:\Windows\Temp\oobe-done.txt'} -EA Stop){ $ready=$true; break } }catch{} }
if(-not $ready){ Log 'TIMEOUT'; return }
Log "install complete"
$s=New-PSSession -VMName $Name -Credential $Cred
Invoke-Command -Session $s -ScriptBlock { param($ip)
  $a=Get-NetAdapter|Where-Object Status -eq 'Up'|Select-Object -First 1
  Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -EA SilentlyContinue|Where-Object {$_.PrefixOrigin -ne 'WellKnown'}|Remove-NetIPAddress -Confirm:$false -EA SilentlyContinue
  New-NetIPAddress -InterfaceIndex $a.ifIndex -IPAddress $ip -PrefixLength 24 -EA SilentlyContinue|Out-Null
  New-Item -ItemType Directory -Force -Path C:\TaskSwitcher|Out-Null
  net accounts /lockoutthreshold:0 2>&1|Out-Null } -ArgumentList $Ip
Copy-Item -ToSession $s -Path $serverExe -Destination 'C:\TaskSwitcher\tsw_session_agent.exe' -Force
Invoke-Command -Session $s -ScriptBlock { reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v TswSession /t REG_SZ /d '"C:\TaskSwitcher\tsw_session_agent.exe"' /f|Out-Null }
Log "static IP $Ip + session agent deployed + Run key"
Invoke-Command -Session $s -ScriptBlock { shutdown /r /t 2 /f }
Remove-PSSession $s
Log "rebooting $Name; done"
