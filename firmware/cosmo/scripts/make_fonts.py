#!/usr/bin/env python3
"""
Generate Adafruit-GFX bitmap fonts for the CosmoLibre face from TrueType files.

Run manually when changing typefaces or sizes (not part of the build):

    python scripts/make_fonts.py

Outputs include/fonts/Cosmo*.h. Sizes are given as target *digit height* in
panel pixels so the face layout stays stable if the typeface is swapped.

Char 0x7F (DEL, unused) holds the degree sign; use FACE_DEG ("\\x7F") in text.

LICENSING: the defaults are the open fonts in fonts-src/ (SIL Open Font
License 1.1): Caladea Bold/Regular (metric-compatible with Cambria) and
Inconsolata Bold. The generated include/fonts/*.h are bitmap conversions of
those fonts and stay under the OFL (see fonts-src/OFL-*.txt), separate from the
firmware's GPL-3.0. A variable font is pinned to a weight with "path@wght".
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "include" / "fonts"
SRC = ROOT / "fonts-src"


def load(spec: str, size: int) -> ImageFont.FreeTypeFont:
    """`path` or `path@wght` (variable fonts: pick the weight axis value)."""
    path, _, weight = spec.partition("@")
    f = ImageFont.truetype(path, size)
    if weight:
        axes = f.get_variation_axes()
        values = [float(weight) if a["name"] in (b"Weight", "Weight") else a["default"]
                  for a in axes]
        f.set_variation_by_axes(values)
    return f

DIGITS = "0123456789:"
ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
HEADER = " -0123456789abcdefghijklmnopqrstuvwxyz"
DEGREE_SLOT = 0x7F

# name, family key, target digit height (px), charset
FONTS = [
    ("CosmoSerifXL", "serif", 100, DIGITS),  # Time Big (stacked HH / MM)
    ("CosmoSerifL", "serif", 62, DIGITS + " "),  # Time Small (HH:MM)
    ("CosmoSerifText", "serif", 26, HEADER),  # wordmark "cosmo" (bold)
    ("CosmoSerifTextLight", "serif_regular", 26, HEADER),  # wordmark "libre" (regular)
    ("CosmoMonoL", "mono", 40, ASCII),  # date digits, temperatures
    ("CosmoMonoM", "mono", 24, ASCII),  # greeting, words
    ("CosmoMonoS", "mono", 16, ASCII),  # labels (WEDNESDAY, SLIGHTLY CLOUDY)
    ("CosmoMonoXS", "mono", 10, ASCII),  # status line
]


def size_for_digit_height(path: str, target: int) -> ImageFont.FreeTypeFont:
    lo, hi = 4, 400
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        f = load(path, mid)
        l, t, r, b = f.getbbox("0", anchor="ls")
        h = b - t
        if h <= target:
            best = f
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def render_glyph(font: ImageFont.FreeTypeFont, ch: str):
    adv = int(round(font.getlength(ch)))
    l, t, r, b = font.getbbox(ch, anchor="ls")
    w, h = r - l, b - t
    if w <= 0 or h <= 0 or ch == " ":
        return dict(bits=[], w=0, h=0, adv=adv, xo=0, yo=0)
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((-l, -t), ch, font=font, fill=255, anchor="ls")
    px = img.load()
    # Trim fully empty rows/cols left by the bbox.
    rows = [y for y in range(h) if any(px[x, y] >= 128 for x in range(w))]
    cols = [x for x in range(w) if any(px[x, y] >= 128 for y in range(h))]
    if not rows or not cols:
        return dict(bits=[], w=0, h=0, adv=adv, xo=0, yo=0)
    y0, y1, x0, x1 = rows[0], rows[-1], cols[0], cols[-1]
    bits = [1 if px[x, y] >= 128 else 0 for y in range(y0, y1 + 1)
            for x in range(x0, x1 + 1)]
    return dict(bits=bits, w=x1 - x0 + 1, h=y1 - y0 + 1, adv=adv,
                xo=l + x0, yo=t + y0)


def pack(bits: list[int]) -> list[int]:
    out = []
    for i in range(0, len(bits), 8):
        chunk = bits[i:i + 8] + [0] * (8 - len(bits[i:i + 8]))
        v = 0
        for b in chunk:
            v = (v << 1) | b
        out.append(v)
    return out


def build(name: str, path: str, target: int, charset: str) -> str:
    font = size_for_digit_height(path, target)
    first = min(ord(c) for c in charset)
    has_degree = charset == ASCII
    last = DEGREE_SLOT if has_degree else max(ord(c) for c in charset)

    bitmap: list[int] = []
    glyphs = []
    for code in range(first, last + 1):
        ch = chr(code)
        if code == DEGREE_SLOT and has_degree:
            ch = "\u00b0"
        if ch not in charset and not (code == DEGREE_SLOT and has_degree):
            glyphs.append((len(bitmap), 0, 0, 0, 0, 0, repr(chr(code))))
            continue
        g = render_glyph(font, ch)
        data = pack(g["bits"])
        glyphs.append((len(bitmap), g["w"], g["h"], g["adv"], g["xo"], g["yo"],
                       repr(ch)))
        bitmap.extend(data)

    for off, w, h, adv, xo, yo, _ in glyphs:
        assert w < 256 and h < 256 and adv < 256, name
        assert -128 <= xo < 128 and -128 <= yo < 128, name
    assert len(bitmap) < 65536, name

    ascent, descent = font.getmetrics()
    lines = [
        f"// Generated by scripts/make_fonts.py from {Path(path).name} "
        f"@ {font.size}px (digit height {target}px). Do not edit.",
        "// Font data: SIL Open Font License 1.1 (see fonts-src/OFL-*.txt).",
        "#pragma once",
        "#include <Adafruit_GFX.h>",
        "",
        f"const uint8_t {name}Bitmaps[] PROGMEM = {{",
    ]
    for i in range(0, len(bitmap), 16):
        lines.append("  " + ", ".join(f"0x{b:02X}" for b in bitmap[i:i + 16]) + ",")
    lines.append("};")
    lines.append("")
    lines.append(f"const GFXglyph {name}Glyphs[] PROGMEM = {{")
    for off, w, h, adv, xo, yo, label in glyphs:
        lines.append(f"  {{{off}, {w}, {h}, {adv}, {xo}, {yo}}},  // {label}")
    lines.append("};")
    lines.append("")
    lines.append(
        f"const GFXfont {name} PROGMEM = {{(uint8_t *){name}Bitmaps, "
        f"(GFXglyph *){name}Glyphs, 0x{first:02X}, 0x{last:02X}, "
        f"{ascent + descent}}};")
    lines.append("")
    print(f"{name:14s} {Path(path).name} {font.size}px  bitmap={len(bitmap)}B")
    return "\n".join(lines)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--serif", default=str(SRC / "Caladea-Bold.ttf"))
    ap.add_argument("--mono", default=str(SRC / "Inconsolata[wdth,wght].ttf") + "@700")
    ap.add_argument("--serif-regular", default=str(SRC / "Caladea-Regular.ttf"))
    args = ap.parse_args()
    paths = {"serif": args.serif, "mono": args.mono,
             "serif_regular": args.serif_regular}
    OUT.mkdir(parents=True, exist_ok=True)
    for name, fam, target, charset in FONTS:
        (OUT / f"{name}.h").write_text(build(name, paths[fam], target, charset),
                                       encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
