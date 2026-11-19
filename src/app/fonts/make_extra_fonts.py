#!/usr/bin/env python3
"""Generates the fallback fonts lrt_extra_<size>.c (specs/04 §3, D71).

LVGL's built-in Montserrat fonts hold ASCII plus a few symbols. These fonts add the
characters text from files and from the G-code library can contain: Latin-1 (U+00A0..U+00FF,
which also covers names read as Latin-1), dashes, curly quotes and the ellipsis. They are
made from the same Montserrat-Medium.ttf, with the same options as LVGL's built-in fonts, so
they match. app/fonts.cpp sets each one as the fallback of the Montserrat font of its size.

Needs lv_font_conv (npm install lv_font_conv@1.5.2, then run with npx from that folder, or
put it on PATH). Run from the repo root after a firmware build has fetched LVGL:

    python src/app/fonts/make_extra_fonts.py [path to lv_font_conv]
"""
import os
import subprocess
import sys

SIZES = [14, 16, 18, 20, 22, 24, 26, 32]   # the sizes src/app uses
RANGES = "0xA0-0xFF,0x2013,0x2014,0x2018,0x2019,0x201C,0x201D,0x2026"
TTF = ".pio/libdeps/elecrow-crowpanel-7-advance-v13/lvgl/scripts/built_in_font/Montserrat-Medium.ttf"
OUT = os.path.join("src", "app", "fonts")


def main():
    conv = sys.argv[1] if len(sys.argv) > 1 else "lv_font_conv"
    if not os.path.exists(TTF):
        sys.exit(f"{TTF} not found: build the firmware once so PlatformIO fetches LVGL")
    for size in SIZES:
        name = f"lrt_extra_{size}"
        out = os.path.join(OUT, name + ".c")
        cmd = [conv, "--no-compress", "--no-prefilter", "--bpp", "4", "--size", str(size),
               "--font", TTF, "-r", RANGES, "--format", "lvgl", "--force-fast-kern-format", "--lv-include", "lvgl.h",
               "-o", out]   # the font is named after the file: lrt_extra_<size>
        subprocess.run(cmd, check=True, shell=(os.name == "nt"))
        print(out)


if __name__ == "__main__":
    main()
