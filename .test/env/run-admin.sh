#!/bin/bash
# Run a PowerShell payload ELEVATED with no UAC prompt, via the pre-approved TSW-AdminRunner
# scheduled task (register it once with admin-setup.ps1).
#
# TSW_ADMIN_BASE must point at a scratch directory that is visible BOTH from WSL (this path)
# and from Windows (the -Base you gave admin-setup.ps1) - the two sides hand off the payload
# and result files through it.
#
# usage: TSW_ADMIN_BASE=/path/to/shared/dir run-admin.sh <payload.ps1> [timeout_sec]
set -u
BASE="${TSW_ADMIN_BASE:?set TSW_ADMIN_BASE to the WSL path of the shared runner dir}"
# Locate powershell.exe: PATH if Windows interop exposes it, else under any mounted Windows drive.
PS="$(command -v powershell.exe 2>/dev/null || true)"
if [ -z "$PS" ]; then
    for c in /mnt/*/Windows/System32/WindowsPowerShell/v1.0/powershell.exe; do
        [ -x "$c" ] && PS="$c" && break
    done
fi
: "${PS:?powershell.exe not found (is a Windows drive mounted under /mnt?)}"
payload="$1"; tmo="${2:-300}"
cp "$payload" "$BASE/.admin-payload.ps1"
rm -f "$BASE/.admin.done" "$BASE/.admin.out"
"$PS" -NoProfile -Command "schtasks /Run /TN TSW-AdminRunner" >/dev/null 2>&1
for ((i=0;i<tmo;i++)); do [ -f "$BASE/.admin.done" ] && break; sleep 1; done
[ -f "$BASE/.admin.out" ] && iconv -f UTF-16LE -t UTF-8 "$BASE/.admin.out" 2>/dev/null | tr -d "\r"
[ -f "$BASE/.admin.done" ] || echo "(payload did not finish within ${tmo}s)"
