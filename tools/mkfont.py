#!/usr/bin/env python3
"""Generates src/fontdata.c: antialiased 8-bit glyph atlases (ASCII 32..126).
Fonts: Inter (SIL OFL 1.1) and DejaVu Sans Mono (Bitstream Vera licence).
Run only when you want to change fonts; the generated file is checked in."""
import sys
from PIL import Image, ImageDraw, ImageFont

INTER = "/usr/share/fonts/opentype/inter/"
FONTS = [  # name, path, px size, mono
    ("F_SM",   INTER + "Inter-Regular.otf",  15, 0),
    ("F_MD",   INTER + "Inter-Regular.otf",  20, 0),
    ("F_LG",   INTER + "Inter-SemiBold.otf", 30, 0),
    ("F_MDB",  INTER + "Inter-SemiBold.otf", 20, 0),
    ("F_XL",   INTER + "Inter-Light.otf",    56, 0),
    ("F_MONO", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 17, 1),
]

data = bytearray()
out = ['#include "ktuyome.h"', '']
tables = []
for name, path, size, mono in FONTS:
    f = ImageFont.truetype(path, size)
    asc, desc = f.getmetrics()
    glyphs = []
    mono_adv = round(f.getlength("M"))
    for code in range(32, 127):
        ch = chr(code)
        adv = mono_adv if mono else round(f.getlength(ch))
        x0, y0, x1, y1 = f.getbbox(ch, anchor="ls")
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        off = len(data)
        if w and h:
            im = Image.new("L", (w, h), 0)
            ImageDraw.Draw(im).text((-x0, -y0), ch, font=f, fill=255, anchor="ls")
            data.extend(im.tobytes())
        else:
            w = h = 0
        glyphs.append((w, h, x0, y0, adv, off))
    tables.append((name, asc + desc, asc, mono))
    out.append("static const Glyph g_%s[95] = {" % name)
    for g in glyphs:
        out.append("  {%d,%d,%d,%d,%d,%d}," % g)
    out.append("};")

out.append("")
out.append("const u8 font_data[%d] = {" % len(data))
line = []
for i, b in enumerate(data):
    line.append(str(b))
    if len(line) == 40:
        out.append(",".join(line) + ",")
        line = []
if line:
    out.append(",".join(line))
out.append("};")
out.append("")
out.append("const Font fonts[%d] = {" % len(tables))
for name, h, asc, mono in tables:
    out.append("  {g_%s, font_data, %d, %d, %d}," % (name, h, asc, mono))
out.append("};")
open(sys.argv[1] if len(sys.argv) > 1 else "src/fontdata.c", "w").write("\n".join(out) + "\n")
print("font data bytes:", len(data))
