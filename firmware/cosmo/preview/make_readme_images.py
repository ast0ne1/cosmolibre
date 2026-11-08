#!/usr/bin/env python3
"""
Regenerate the README images (docs/images/*.png) from the desktop preview.

    python preview/preview_server.py          # in one terminal (fresh start)
    python preview/make_readme_images.py      # in another

Every tile is the firmware's own rendering (Face.cpp / Ui.cpp compiled for the
desktop), so the images match the real panel pixel for pixel. Start the preview
fresh: the menu sequence below assumes the clock is showing its face.
"""

from __future__ import annotations

import io
import json
import urllib.request
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT.parents[1] / "docs" / "images"
LABEL_FONT = ROOT / "fonts-src" / "Inconsolata[wdth,wght].ttf"
B = "http://localhost:8377"

BG, BORDER, INK = (230, 234, 230), (40, 40, 40), (25, 25, 25)
PAD, GAP, LABEL_H = 22, 22, 34


def post(path: str, data: str) -> dict:
    req = urllib.request.Request(B + path, data=data.encode(), headers={
        "X-Cosmo": "1", "Content-Type": "application/x-www-form-urlencoded"})
    return json.loads(urllib.request.urlopen(req, timeout=300).read() or b"{}")


def bmp(path: str) -> Image.Image:
    raw = urllib.request.urlopen(B + path, timeout=300).read()
    return Image.open(io.BytesIO(raw)).convert("RGB")


def pad(p: str, gesture: str = "tap", times: int = 1) -> None:
    for _ in range(times):
        post("/api/_pad", f"pad={p}&gesture={gesture}")


def sheet(tiles: list[tuple[Image.Image, str]], cols: int, out: Path) -> None:
    w, h = tiles[0][0].size
    rows = (len(tiles) + cols - 1) // cols
    sheet_w = PAD * 2 + cols * (w + 4) + (cols - 1) * GAP
    sheet_h = PAD * 2 + rows * (h + 4 + LABEL_H) + (rows - 1) * GAP
    img = Image.new("RGB", (sheet_w, sheet_h), BG)
    d = ImageDraw.Draw(img)
    font = ImageFont.truetype(str(LABEL_FONT), 17)
    font.set_variation_by_axes([700, 100])
    for i, (tile, label) in enumerate(tiles):
        r, c = divmod(i, cols)
        x = PAD + c * (w + 4 + GAP)
        y = PAD + r * (h + 4 + LABEL_H + GAP)
        d.rectangle([x, y, x + w + 3, y + h + 3], outline=BORDER, width=2)
        img.paste(tile, (x + 2, y + 2))
        d.text((x + (w + 4) / 2, y + h + 4 + LABEL_H / 2), label, font=font,
               fill=INK, anchor="mm")
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out, optimize=True)
    print("wrote", out.relative_to(ROOT.parents[1]))


# Four example pages of three widgets each: the clock flips or rotates between them.
PAGES = [
    ("Everyday", ["1:0", "4:0", "3:0"]),       # big time · date · weather now
    ("Good morning", ["2:1", "1:3", "3:2"]),   # greeting · 12h time · 3-day forecast
    ("In words", ["1:4", "4:2", "3:3"]),       # time in words · long date · description
    ("Weather", ["1:2", "3:5", "3:4"]),        # time · big icon · cloud/rain/humidity
]


def set_pages(current: int) -> None:
    form = "&".join(f"p{i}={','.join(slots)}" for i, (_, slots) in enumerate(PAGES))
    post("/api/pages", f"{form}&current={current}&rotate=0&every=5")


def pages() -> None:
    post("/api/settings", "city=Sydney&lat=-33.87&lon=151.21")
    post("/api/_mock", "time=07:45&date=2026-11-04&synced=1&wx=1&temp=18&code=2")
    tiles = []
    for i, (name, _) in enumerate(PAGES):
        set_pages(i)
        tiles.append((bmp("/api/_screen.bmp"), f"Page {i + 1} · {name}"))
    set_pages(0)
    sheet(tiles, 4, OUT / "pages.png")


def menu() -> None:
    post("/api/_mock", "time=07:45&date=2026-11-04&synced=1&wx=1&temp=18&code=2")
    pad("M", "hold")                                    # open the menu (APPS)
    tiles = [(bmp("/api/_screen.bmp"), "Menu")]
    pad("R", times=3); pad("M")                         # SETTINGS
    tiles.append((bmp("/api/_screen.bmp"), "Settings"))
    pad("R", times=2); pad("M")                         # WI-FI
    tiles.append((bmp("/api/_screen.bmp"), "Wi-Fi"))
    pad("R"); pad("M")                                  # CONNECT NOW -> WEB UI
    tiles.append((bmp("/api/_screen.bmp"), "Wi-Fi › Connect"))
    pad("M")                                            # back to Wi-Fi (on CONNECT)
    pad("L", times=2); pad("M")                         # MODE, ↰ -> Settings (on WI-FI)
    pad("L", times=3); pad("M")                         # NIGHT, SOUND, ↰ -> top (on SETTINGS)
    pad("L", times=2); pad("M")                         # TIMER, ALARM
    tiles.append((bmp("/api/_screen.bmp"), "Alarms"))
    pad("L", times=2); pad("M")                         # ↰ -> top (on ALARM)
    pad("L", times=2); pad("M")                         # APPS, ↰: close the menu
    post("/api/_mock", "time=&synced=1")
    sheet(tiles, 5, OUT / "device-menu.png")


if __name__ == "__main__":
    pages()
    menu()
