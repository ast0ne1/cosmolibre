#!/usr/bin/env python3
"""
Build and run the desktop firmware tests (no clock needed):

    python preview/run_tests.py

Each preview/tests/*_test.cpp is compiled with g++ against the real firmware
sources (src/hal/*.cpp) plus the stand-ins in preview/native (Arduino, EEPROM).
"""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

PROJ = Path(__file__).resolve().parents[1]
BUILD = PROJ / "preview" / "build"
GFX = PROJ / ".pio" / "libdeps" / "cosmo" / "Adafruit GFX Library"
QR = PROJ / ".pio" / "libdeps" / "cosmo" / "QRCode" / "src"
FIRMWARE = [PROJ / "src" / "hal" / f for f in (
    "Settings.cpp", "Ui.cpp", "Face.cpp", "FaceBitmap.cpp", "Widgets.cpp",
    "WeatherCodes.cpp", "Typo.cpp")]
EXT = ".exe" if sys.platform == "win32" else ""


def main() -> int:
    gxx, gcc = shutil.which("g++"), shutil.which("gcc")
    if not gxx or not gcc:
        print("g++/gcc not found on PATH")
        return 2
    BUILD.mkdir(parents=True, exist_ok=True)
    qr_obj = BUILD / "qrcode_t.o"
    subprocess.run([gcc, "-std=c11", "-O1", "-DLOCK_VERSION=3", "-c", str(QR / "qrcode.c"), "-o", str(qr_obj)],
                   check=True)
    failed = 0
    for test in sorted((PROJ / "preview" / "tests").glob("*_test.cpp")):
        exe = BUILD / (test.stem + EXT)
        cmd = [gxx, "-std=gnu++17", "-O1", "-Wall", "-Wno-unused-function", "-DARDUINO=100", "-DLOCK_VERSION=3",
               f"-I{PROJ / 'preview' / 'native'}", f"-I{PROJ / 'include'}", f"-I{GFX}",
               f"-I{QR}", str(test), *map(str, FIRMWARE), str(GFX / "Adafruit_GFX.cpp"),
               str(qr_obj), "-o", str(exe)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"{test.name}: BUILD FAILED\n{r.stderr[-2000:]}")
            failed += 1
            continue
        failed += subprocess.run([str(exe)]).returncode != 0
    print("ALL TESTS PASSED" if not failed else f"{failed} test program(s) failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
