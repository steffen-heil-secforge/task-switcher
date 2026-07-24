#!/usr/bin/env python3
"""Generate src/client_agent/logo_png.h from two rasterized logo PNGs.

Usage: gen_logo_header.py <light.png> <dark.png> <out.h>

Emits two byte arrays (kLogoLight / kLogoDark) + their lengths. The PNGs are
transparent RGBA; the picker decodes them once via WIC. Invoked by tools/update-logo.sh.
"""
import sys


def emit(name, path):
    data = open(path, "rb").read()
    lines = [f"static const unsigned char {name}[] = {{"]
    row = []
    for b in data:
        row.append(str(b))
        if len(row) == 24:
            lines.append("  " + ",".join(row) + ",")
            row = []
    if row:
        lines.append("  " + ",".join(row) + ",")
    lines.append("};")
    lines.append(f"static const unsigned {name}Len = {len(data)};")
    return lines, len(data)


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: gen_logo_header.py <light.png> <dark.png> <out.h>")
    light, ll = emit("kLogoLight", sys.argv[1])
    dark, dl = emit("kLogoDark", sys.argv[2])
    out = [
        "// Auto-generated from assets/logo.svg (light) and assets/logo-white.svg (dark).",
        "// To refresh after editing those SVGs: run tools/update-logo.sh (needs rsvg-convert),",
        "// then commit this file. Do not edit by hand.",
        "//",
        "// BRANDING NOTICE - NOT COVERED BY THE MIT LICENSE:",
        "// This embeds the secforge logo. The name \"secforge\" and the secforge logo are",
        "// trademarks of secforge GmbH and are NOT licensed under the project's MIT license.",
        "// Redistributing secforge's original, unmodified release binaries is fine. But if you",
        "// rebuild, modify, or fork, you MUST replace the logo: swap assets/logo.svg and",
        "// assets/logo-white.svg for your own artwork and regenerate this file via",
        "// tools/update-logo.sh. See the LICENSE file.",
        "#pragma once",
        "",
        *light,
        "",
        *dark,
        "",
    ]
    open(sys.argv[3], "w").write("\n".join(out) + "\n")
    print(f"gen_logo_header: light={ll}b dark={dl}b -> {sys.argv[3]}")


if __name__ == "__main__":
    main()
