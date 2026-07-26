# Downloads the Windows 11 ISO. NOTE: curl.exe emits its progress meter on stderr;
# under $ErrorActionPreference='Stop' PowerShell turns that into a fatal
# NativeCommandError. So keep 'Continue' here and gate success on $LASTEXITCODE.
$ErrorActionPreference = 'Continue'
$log = 'L:\Development\wsl\taskswitcher\iso-download.log'
$dir = 'L:\Virtual Machines\Hyper-V\iso'
$out = Join-Path $dir 'win11.iso'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$url = (Get-Content 'L:\Development\wsl\taskswitcher\iso-url.txt' -Raw).Trim()
"START $(Get-Date -Format o)" | Out-File $log -Encoding utf8
# -s silent (no progress meter -> no stderr spam), --fail on HTTP >=400, --retry transient
& curl.exe -L --fail --retry 3 --retry-delay 5 -s -o $out $url
$code = $LASTEXITCODE
if ($code -eq 0 -and (Test-Path $out)) {
  $gb = [math]::Round((Get-Item $out).Length/1GB, 2)
  "DONE $(Get-Date -Format o) size_GB=$gb" | Out-File $log -Append -Encoding utf8
} else {
  "FAILED $(Get-Date -Format o) curl_exit=$code" | Out-File $log -Append -Encoding utf8
}
