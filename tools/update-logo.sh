#!/usr/bin/env bash
# Regenerate the committed logo header (src/client_agent/logo_png.h) from the SVGs.
# Run from WSL after editing assets/logo.svg or assets/logo-white.svg, then commit
# the updated header. Needs rsvg-convert (apt install librsvg2-bin) and python3.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
command -v rsvg-convert >/dev/null || { echo "ERROR: rsvg-convert not found (apt install librsvg2-bin)"; exit 1; }
command -v python3      >/dev/null || { echo "ERROR: python3 not found"; exit 1; }
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
rsvg-convert -h 128 "$REPO/assets/logo.svg"       -o "$tmp/light.png"
rsvg-convert -h 128 "$REPO/assets/logo-white.svg" -o "$tmp/dark.png"
python3 "$REPO/tools/gen_logo_header.py" "$tmp/light.png" "$tmp/dark.png" "$REPO/src/client_agent/logo_png.h"
echo "Updated src/client_agent/logo_png.h - review and commit it."
