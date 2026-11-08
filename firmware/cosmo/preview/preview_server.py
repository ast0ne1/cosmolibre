#!/usr/bin/env python3
"""
CosmoLibre desktop preview — work on the web UI and the e-paper face without
reflashing.

    python preview/preview_server.py            # http://localhost:8377

* Serves web/index.html — the exact file the firmware embeds — re-read on
  every request, so edits show on reload.
* Mocks the device's /api with in-memory state (nothing touches the clock).
* /api/face.bmp compiles the firmware's real Face.cpp with g++ and renders it,
  so the preview is pixel-identical to the panel. Edit src/hal/Face.cpp, reload.
* Timezones, widgets and presets are parsed from the C++ sources, so the
  preview can't drift from the firmware.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import shutil
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.error import URLError
from urllib.parse import parse_qs, urlencode, urlparse
from urllib.request import urlopen

PROJ = Path(__file__).resolve().parents[1]
INDEX = PROJ / "web" / "index.html"
GFX = PROJ / ".pio" / "libdeps" / "cosmo" / "Adafruit GFX Library"
BUILD = PROJ / "preview" / "build"
EXT = ".exe" if sys.platform == "win32" else ""
# One long-running native program renders everything: the clock screen (menus,
# alarms via src/hal/Ui.cpp), draft faces and toolbox thumbnails. A single
# binary also means a single approval if Windows Smart App Control vets it.
SIM_EXE = BUILD / f"ui_sim{EXT}"
QR = PROJ / ".pio" / "libdeps" / "cosmo" / "QRCode" / "src"
SHARED = [
    PROJ / "src" / "hal" / "Face.cpp",
    PROJ / "src" / "hal" / "FaceBitmap.cpp",
    PROJ / "src" / "hal" / "Widgets.cpp",
    PROJ / "src" / "hal" / "WeatherCodes.cpp",
    PROJ / "src" / "hal" / "Typo.cpp",
]
SIM_SOURCES = [PROJ / "preview" / "ui_sim.cpp", PROJ / "src" / "hal" / "Ui.cpp", *SHARED]
# Optional: a folder of <name>.wav chimes for the preview's sound buttons
# (COSMO_SOUNDS=/path/to/wavs). Not shipped with the project.
AUDIO = Path(os.environ.get("COSMO_SOUNDS", PROJ / "preview" / "sounds"))
WATCH = SIM_SOURCES + list((PROJ / "include").rglob("*.h")) + list(
    (PROJ / "preview" / "native").glob("*.h"))


# ---- tables parsed from the firmware sources ---------------------------------

def parse_enums(text: str) -> dict[str, dict[str, int]]:
    enums = {}
    for name, body in re.findall(r"enum class (\w+)\s*:\s*\w+\s*\{(.*?)\};", text, re.S):
        enums[name] = {k: int(v) for k, v in re.findall(r"(\w+)\s*=\s*(\d+)", body)}
    return enums


def load_tables() -> dict:
    widgets = (PROJ / "include" / "hal" / "Widgets.h").read_text(encoding="utf-8")
    settings = (PROJ / "src" / "hal" / "Settings.cpp").read_text(encoding="utf-8")
    catalogue = (PROJ / "src" / "hal" / "Widgets.cpp").read_text(encoding="utf-8")
    enums = parse_enums(widgets)
    kinds = enums["WidgetKind"]

    def variant(expr: str) -> int:
        expr = expr.strip()
        if expr.isdigit():
            return int(expr)
        m = re.match(r"(?:\(uint8_t\))?(\w+)::(\w+)", expr)
        return enums[m.group(1)][m.group(2)]

    options = []
    block = catalogue.split("kSlotOptions[] PROGMEM = {", 1)[1].split("};", 1)[0]
    for kind, var, label in re.findall(
            r'\{\(uint8_t\)WidgetKind::(\w+),\s*([^,]+),\s*"([^"]+)"\}', block):
        options.append({"kind": kinds[kind], "variant": variant(var), "label": label})

    tz_block = settings.split("kTzTable[] PROGMEM = {", 1)[1].split("};", 1)[0]
    timezones = re.findall(r'\{"([^"]+)",\s*"[^"]+"\}', tz_block)

    presets = []
    p_block = settings.split("kPresets[] PROGMEM = {", 1)[1].split("\n};", 1)[0]
    for name, body in re.findall(r'\{"([^"]+)",\s*\{\{(.*?)\}\}\}', p_block, re.S):
        slots = []
        for item in re.findall(r"SLOT\((\w+),\s*([\w:]+)\)|NONE", body):
            slots.append({"kind": kinds[item[0]], "variant": variant(item[1])}
                         if item[0] else {"kind": 0, "variant": 0})
        presets.append({"name": name, "slots": slots})
    slot_count = int(re.search(r"kFaceSlots\s*=\s*(\d+)", widgets).group(1))
    ui = (PROJ / "include" / "hal" / "Ui.h").read_text(encoding="utf-8")
    max_pages = int(re.search(r"kMaxFaces\s*=\s*(\d+)", ui).group(1))
    rotate = [int(x) for x in re.search(r"kRotateChoices\[\]\s*=\s*\{([^}]*)\}", ui)
              .group(1).split(",")]
    return {"options": options, "timezones": timezones, "presets": presets,
            "slotCount": slot_count, "maxPages": max_pages, "rotateChoices": rotate}


TABLES = load_tables()

WX_TEXT = {0: "CLEAR SKY", 1: "MAINLY CLEAR", 2: "SLIGHTLY CLOUDY", 3: "OVERCAST",
           45: "FOG", 53: "LIGHT DRIZZLE", 63: "RAIN", 73: "SNOW", 95: "THUNDERSTORM"}
GEOCODE_URL = "https://geocoding-api.open-meteo.com/v1/search"


def geocode(city: str, count: int = 5) -> list[dict] | None:
    """Same Open-Meteo search the firmware does (Weather::searchCities).
    Returns candidates, or None when offline."""
    url = GEOCODE_URL + "?" + urlencode({"name": city, "count": count,
                                         "language": "en", "format": "json"})
    try:
        with urlopen(url, timeout=6) as r:
            data = json.load(r)
    except (URLError, OSError, ValueError) as e:
        print(f"[preview] geocode offline ({e})", flush=True)
        return None
    return [{"name": h.get("name", "")[:31], "region": (h.get("admin1") or "")[:31],
             "country": h.get("country_code", ""), "lat": round(h["latitude"], 4),
             "lon": round(h["longitude"], 4)} for h in data.get("results") or []]


# ---- mock device state --------------------------------------------------------

LOCK = threading.Lock()
now = dt.datetime.now()
STATE = {
    # Pages 1-3 start from presets, page 4 empty; cur/rotate/every mirror the
    # simulator (the device owns them; the web UI can change them).
    "pages": [[dict(s) for s in p["slots"]] for p in TABLES["presets"][:3]]
             + [[{"kind": 0, "variant": 0}] * TABLES["slotCount"]]
             * (TABLES["maxPages"] - 3),
    "cur": 0, "rotate": False, "every": 5, "simKeys": [],
    "showIp": False,  # opt-in status line; firmware hides it when all is well
    "tzId": 0, "beep": True, "location": ("Berlin", 52.52, 13.41),
    "synced": True, "time": None, "date": None,  # None = follow the PC clock
    "wx": True, "temp": 29, "code": 2, "cloud": 38, "hum": 19,
    "started": now,
}
DAY3 = ["SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"]


def mock_now() -> dt.datetime:
    real = dt.datetime.now()
    d = real.date()
    if STATE["date"]:
        try:
            d = dt.date.fromisoformat(STATE["date"])
        except ValueError:
            pass
    t = real.time()
    if STATE["time"]:
        try:
            t = dt.time.fromisoformat(STATE["time"])
        except ValueError:
            pass
    return dt.datetime.combine(d, t)


def forecast() -> list[tuple]:
    n = mock_now()
    base, code = STATE["temp"], STATE["code"]
    days = []
    for i, (dmax, dmin, c, rain) in enumerate([(2, -7, code, 11), (-2, -10, 61, 60), (-5, -12, 95, 80)]):
        wd = DAY3[(n.weekday() + 1 + i) % 7]
        days.append((wd, base + dmax, base + dmin, c, rain))
    return days


def cur_slots() -> list[dict]:
    return STATE["pages"][STATE["cur"]]



# ---- Wi-Fi setup portal preview -------------------------------------------
# Rebuilds WiFiManager's /wifi page from the library's own templates, with
# web/portal.html in the <head> exactly as the firmware injects it.
WM_STRINGS = PROJ / ".pio" / "libdeps" / "cosmo" / "WiFiManager" / "wm_strings_en.h"
PORTAL_HEAD = PROJ / "web" / "portal.html"
MOCK_NETS = [  # ssid, rssi %, secured
    ("Home WiFi", 92, True), ("Home WiFi 5G", 70, True), ("Network 3", 55, True),
    ("Guest WiFi", 40, False), ("Network 4", 22, False),
]


def wm_strings() -> dict[str, str]:
    """C string constants from wm_strings_en.h (adjacent literals joined)."""
    text = WM_STRINGS.read_text(encoding="utf-8")
    out = {}
    for m in re.finditer(r'^const char (\w+)\[\]\s+PROGMEM\s*=\s*((?:\s*"(?:[^"\\]|\\.)*"\s*|\s*//[^\n]*)+);',
                         text, re.M):
        lits = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2))
        out[m.group(1)] = "".join(lits).encode().decode("unicode_escape")
    return out


def portal_page(path: str, q: dict) -> bytes:
    import html
    s = wm_strings()
    gen = PROJ / "include" / "generated" / "portal_head.h"  # what the firmware ships
    if gen.exists() and gen.stat().st_mtime >= PORTAL_HEAD.stat().st_mtime:
        raw = "".join(re.findall(r'^  "(.*)";?$', gen.read_text(encoding="utf-8"), re.M))
        head = raw.encode("latin1").decode("unicode_escape").encode("latin1").decode("utf-8")
    else:  # not regenerated yet (pio run does it): show the source as is
        head = re.sub(r"<!--.*?-->", "", PORTAL_HEAD.read_text(encoding="utf-8"), flags=re.S)
    page = (s["HTTP_HEAD_START"].replace("{v}", "Config ESP") + s["HTTP_SCRIPT"]
            + s["HTTP_STYLE"] + head + s["HTTP_HEAD_END"].replace("{c}", ""))
    if path.endswith("/wifisave"):
        page += s["HTTP_SAVED"]
    else:
        nets = [] if q.get("empty") else MOCK_NETS
        if not nets:
            page += "No networks found. Refresh to scan again."
        for ssid, perc, lock in nets:
            qi = (s["HTTP_ITEM_QI"].replace("{r}", str(perc))
                  .replace("{q}", str(round(1 + perc * 3 / 100)))
                  .replace("{i}", "l" if lock else "").replace("{h}", ""))
            page += (s["HTTP_ITEM"].replace("{V}", html.escape(ssid, quote=True))
                     .replace("{v}", html.escape(ssid)).replace("{qi}", qi).replace("{qp}", ""))
        page += s["HTTP_FORM_START"].replace("{v}", "wifisave")
        page += (s["HTTP_FORM_WIFI"].replace("{v}", q.get("saved", "Home WiFi"))
                 .replace("{p}", "********"))
        page += s["HTTP_FORM_WIFI_END"] + s["HTTP_FORM_END"] + s["HTTP_SCAN_LINK"]
        reason = {"pw": s["HTTP_STATUS_OFFPW"], "noap": s["HTTP_STATUS_OFFNOAP"]}.get(q.get("fail", ""))
        if reason:
            page += (s["HTTP_STATUS_OFF"].replace("{c}", "D")
                     .replace("{v}", q.get("saved", "Home WiFi")).replace("{r}", reason))
        else:
            page += s["HTTP_STATUS_NONE"]
    page += s["HTTP_END"]
    return page.encode("utf-8")


def status_json() -> dict:
    n = mock_now()
    s = {
        "fw": "0.1.0-preview", "uptime": int((dt.datetime.now() - STATE["started"]).total_seconds()),
        "heap": 23456, "ip": "192.168.1.20", "ssid": "HomeNetwork", "rssi": -58,
        "state": "ntp ok" if STATE["synced"] else "ntp...", "stateId": 5,
        "synced": STATE["synced"], "tz": TABLES["timezones"][STATE["tzId"]],
        "location": STATE["location"][0] if STATE["location"] else None,
        "slots": cur_slots(),
    }
    if STATE["synced"]:
        s["time"] = n.strftime("%H:%M:%S")
        s["date"] = n.strftime("%Y-%m-%d")
    if STATE["wx"] and STATE["location"]:
        s["weather"] = {
            "valid": True, "tempNow": STATE["temp"], "codeNow": STATE["code"],
            "label": WX_TEXT.get(STATE["code"], "CLOUD"), "cloud": STATE["cloud"],
            "humidity": STATE["hum"], "ageSec": 300,
            "days": [{"wd": d[0], "max": d[1], "min": d[2], "code": d[3], "rain": d[4]}
                     for d in forecast()],
        }
    else:
        s["weather"] = {"valid": False}
    return s


PLAIN = False  # --plain: hide the preview-only controls (clean screenshots)


def config_json() -> dict:
    loc = STATE["location"]
    n = mock_now()
    cfg = {
        "tzId": STATE["tzId"], "timezones": TABLES["timezones"], "beepOnSync": STATE["beep"],
        "showIp": STATE["showIp"],
        "location": {"label": loc[0], "lat": loc[1], "lon": loc[2]} if loc else None,
        "slotCount": TABLES["slotCount"], "slots": cur_slots(),
        "maxPages": TABLES["maxPages"], "pages": STATE["pages"],
        "currentPage": STATE["cur"], "rotate": STATE["rotate"],
        "rotateMinutes": STATE["every"], "rotateChoices": TABLES["rotateChoices"],
        "presets": TABLES["presets"], "slotOptions": TABLES["options"],
        "mock": {"time": STATE["time"] or n.strftime("%H:%M"),
                 "date": STATE["date"] or n.strftime("%Y-%m-%d"),
                 "temp": STATE["temp"], "code": STATE["code"],
                 "synced": STATE["synced"], "wx": STATE["wx"]},
    }
    if PLAIN:
        del cfg["mock"]
    return cfg


# ---- native face renderer -------------------------------------------------------

BUILD_LOCK = threading.Lock()


def ensure_renderer() -> str | None:
    """Rebuild the native face renderer + UI simulator when any firmware source
    they use changed. Returns an error message or None."""
    with BUILD_LOCK:
        newest = max(p.stat().st_mtime for p in WATCH if p.exists())
        if SIM_EXE.exists() and SIM_EXE.stat().st_mtime >= newest:
            return None
        gxx, gcc = shutil.which("g++"), shutil.which("gcc")
        if not gxx or not gcc:
            return "g++/gcc not found on PATH (needed for the pixel-exact preview)"
        for lib in (GFX / "Adafruit_GFX.cpp", QR / "qrcode.c"):
            if not lib.exists():
                return f"{lib} missing - run `pio pkg install` once to fetch libraries"
        BUILD.mkdir(parents=True, exist_ok=True)
        print("[preview] compiling the native renderer (ui_sim) ...", flush=True)
        qr_obj = BUILD / "qrcode.o"
        common = [gxx, "-std=gnu++17", "-O2", "-DARDUINO=100", "-DLOCK_VERSION=3",
                  f"-I{PROJ / 'preview' / 'native'}", f"-I{PROJ / 'include'}",
                  f"-I{GFX}", f"-I{QR}"]
        steps = [
            [gcc, "-std=c11", "-O2", "-DLOCK_VERSION=3", "-c", str(QR / "qrcode.c"), "-o", str(qr_obj)],
            [*common, *map(str, SIM_SOURCES), str(GFX / "Adafruit_GFX.cpp"),
             str(qr_obj), "-o", str(SIM_EXE)],
        ]
        SIM.stop()  # Windows can't overwrite a running .exe
        for cmd in steps:
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                return "Preview build failed:\n" + r.stderr[-3000:]
        print("[preview] native renderer ready", flush=True)
        return None


def status_line() -> str:
    """Bottom line of the face: empty when all is well (same rule as main.cpp)."""
    if not STATE["synced"]:
        return "ntp..."
    return "192.168.1.20" if STATE["showIp"] else ""


def slots_arg(slots: list[dict]) -> str:
    return ",".join(f"{s['kind']}:{s['variant']}" for s in slots)


def mock_inputs(slots: list[dict] | None = None, sim: bool = False) -> list[str]:
    """key=value lines describing the mock device (shared by both programs)."""
    n = mock_now()
    if sim:
        lines = [f"f{i}={slots_arg(p)}" for i, p in enumerate(STATE["pages"])]
    else:
        lines = [f"s{i}={s['kind']}:{s['variant']}" for i, s in enumerate(slots or cur_slots())]
    lines += [f"synced={int(STATE['synced'])}", f"year={n.year}", f"mon={n.month}",
              f"mday={n.day}", f"wday={(n.weekday() + 1) % 7}", f"hour={n.hour}",
              f"min={n.minute}", f"sec={n.second}", f"status={status_line()}",
              "ip=192.168.1.20", "ssid=HomeNetwork", "rssi=-58", "wifi=1",
              f"uptime={int((dt.datetime.now() - STATE['started']).total_seconds())}"]
    if STATE["wx"] and STATE["location"]:
        lines += ["wx=1", f"temp={STATE['temp']}", f"code={STATE['code']}",
                  f"cloud={STATE['cloud']}", f"hum={STATE['hum']}"]
        lines += [f"d{i}={','.join(map(str, d))}" for i, d in enumerate(forecast())]
    else:
        lines += ["wx=0"]
    return lines


class Simulator:
    """Keeps ui_sim running so menu state survives between requests."""

    def __init__(self):
        self.proc = None
        self.lock = threading.Lock()

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        self.proc = None

    def _ensure(self):
        if self.proc is None or self.proc.poll() is not None:
            self.proc = subprocess.Popen([str(SIM_EXE)], stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE)
            STATE["simKeys"] = [f"cur={STATE['cur']}", f"rotate={int(STATE['rotate'])}",
                                f"every={STATE['every']}"]

    def _send(self, lines: list[str]):
        self.proc.stdin.write(("\n".join(lines) + "\n").encode())
        self.proc.stdin.flush()

    def _inputs(self) -> list[str]:
        keys, STATE["simKeys"] = STATE["simKeys"], []
        return mock_inputs(sim=True) + keys

    def call(self, command: str) -> dict:
        with self.lock:
            self._ensure()
            self._send(self._inputs() + [command])
            st = json.loads(self.proc.stdout.readline())
            # The device owns page selection/rotation; mirror it for the web UI.
            STATE["cur"], STATE["rotate"], STATE["every"] = st["face"], st["rotate"], st["every"]
            return st

    def render(self, command: str = "!render") -> bytes:
        with self.lock:
            self._ensure()
            self._send(self._inputs() + [command])
            header = self.proc.stdout.readline().split()
            if len(header) != 2 or header[0] != b"BMP":
                raise RuntimeError(f"renderer said {header!r}")
            return self.proc.stdout.read(int(header[1]))


SIM = Simulator()


def render_face(draft: list[dict] | None) -> bytes:
    return SIM.render("!face " + slots_arg(draft or cur_slots()))


# ---- HTTP -------------------------------------------------------------------------

def parse_slot(v: str):
    try:
        k, var = (int(x) for x in v.split(":"))
    except ValueError:
        return None
    ok = any(o["kind"] == k and o["variant"] == var for o in TABLES["options"])
    return {"kind": k, "variant": var} if ok else None


class Handler(BaseHTTPRequestHandler):
    server_version = "CosmoLibrePreview/1"

    def log_message(self, fmt, *args):
        first = str(args[0]) if args else ""
        if not any(p in first for p in ("/api/status", "/api/_state", ".bmp")):
            sys.stderr.write("[preview] " + fmt % args + "\n")

    def send(self, code: int, body: bytes, ctype: str):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def json(self, obj, code: int = 200):
        self.send(code, json.dumps(obj).encode(), "application/json")

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path in ("/", "/index.html"):
            return self.send(200, INDEX.read_bytes(), "text/html; charset=utf-8")
        if u.path.startswith("/portal/"):  # Wi-Fi setup portal mock
            return self.send(200, portal_page(u.path, q), "text/html; charset=utf-8")
        # Slow work (network lookup, compiling the renderer) runs outside LOCK
        # so status polls aren't blocked behind it.
        if u.path == "/api/geocode":
            q_ = q.get("q", "").strip()
            if not q_ or len(q_) > 64:
                return self.json({"ok": False, "error": "enter a city name"}, 400)
            hits = geocode(q_)
            if hits is None:  # offline: let UI work continue
                hits = [{"name": q_[:31], "region": "offline placeholder",
                         "country": "", "lat": 0.0, "lon": 0.0}]
            return self.json({"ok": True, "results": hits})
        if u.path.startswith("/api/_sound/"):
            name = u.path.rsplit("/", 1)[-1].removesuffix(".wav")
            wav = AUDIO / f"{name}.wav"
            if not name.isalnum() or not wav.exists():
                return self.json({"ok": False, "error": "no such sound"}, 404)
            return self.send(200, wav.read_bytes(), "audio/wav")
        if u.path == "/api/_splash.bmp":  # start-up screen
            err = ensure_renderer()
            if err:
                return self.send(500, err.encode(), "text/plain; charset=utf-8")
            with LOCK:
                return self.send(200, SIM.render("!splash"), "image/bmp")
        if u.path in ("/api/_screen.bmp", "/api/_state"):
            err = ensure_renderer()
            if err:
                return self.send(500, err.encode(), "text/plain; charset=utf-8")
            with LOCK:
                if u.path == "/api/_state":
                    return self.json(SIM.call("!state"))
                return self.send(200, SIM.render(), "image/bmp")
        if u.path == "/api/widget.bmp":
            err = ensure_renderer()
            slot = parse_slot(q.get("w", ""))
            if err or not slot:
                return self.send(400 if not err else 500, (err or "bad widget").encode(),
                                 "text/plain; charset=utf-8")
            with LOCK:
                return self.send(200, SIM.render(f"!widget {slot['kind']}:{slot['variant']}"),
                                 "image/bmp")
        if u.path == "/api/face.bmp":
            err = ensure_renderer()
            if err:
                print("[preview] " + err, flush=True)
                return self.send(500, err.encode(), "text/plain; charset=utf-8")
        with LOCK:
            if u.path == "/api/status":
                return self.json(status_json())
            if u.path == "/api/config":
                return self.json(config_json())
            if u.path == "/api/face.bmp":
                draft = None
                if any(f"s{i}" in q for i in range(TABLES["slotCount"])):
                    draft = [parse_slot(q.get(f"s{i}", "")) or cur_slots()[i]
                             for i in range(TABLES["slotCount"])]
                return self.send(200, render_face(draft), "image/bmp")
        if u.path.startswith("/api/"):
            return self.json({"ok": False, "error": "not found"}, 404)
        self.send_response(302)
        self.send_header("Location", "/")
        self.end_headers()

    def do_POST(self):
        u = urlparse(self.path)
        if u.path.startswith("/portal/"):  # portal forms post without X-Cosmo
            n = int(self.headers.get("Content-Length") or 0)
            f = {k: v[0] for k, v in parse_qs(self.rfile.read(n).decode(),
                                              keep_blank_values=True).items()}
            print(f"[preview] portal {u.path} ssid={f.get('s', '')!r} "
                  f"psk_len={len(f.get('p', ''))} has_p={'p' in f}", flush=True)
            return self.send(200, portal_page(u.path, {}), "text/html; charset=utf-8")
        if "X-Cosmo" not in self.headers:  # same rule as the firmware
            return self.json({"ok": False, "error": "missing X-Cosmo header"}, 403)
        n = int(self.headers.get("Content-Length") or 0)
        f = {k: v[0] for k, v in parse_qs(self.rfile.read(n).decode(),
                                          keep_blank_values=True).items()}
        # Name-only city (no picked coordinates): look it up before taking LOCK.
        city = f.get("city", "").strip()
        lookup = None
        if (u.path == "/api/settings" and city and not ("lat" in f and "lon" in f)
                and (not STATE["location"] or city != STATE["location"][0])):
            lookup = geocode(city, 1) or []
        with LOCK:
            if u.path == "/api/face":
                if f.get("preset"):
                    STATE["pages"][STATE["cur"]] = [dict(s) for s in TABLES["presets"][int(f["preset"])]["slots"]]
                for i in range(TABLES["slotCount"]):
                    s = parse_slot(f.get(f"s{i}", ""))
                    if s:
                        STATE["pages"][STATE["cur"]][i] = s
                print(f"[preview] face saved: {cur_slots()}", flush=True)
                return self.json({"ok": True})
            if u.path == "/api/pages":
                pages = []
                for i in range(TABLES["maxPages"]):
                    parts = (f.get(f"p{i}") or "").split(",")
                    page = [parse_slot(x) or {"kind": 0, "variant": 0}
                            for x in (parts + [""] * TABLES["slotCount"])[:TABLES["slotCount"]]]
                    pages.append(page)
                if not any(s["kind"] for p in pages for s in p):
                    return self.json({"ok": False, "error": "keep at least one page"}, 400)
                STATE["pages"] = pages
                cur = int(f.get("current", STATE["cur"]) or 0)
                if not any(s["kind"] for s in pages[cur]):
                    cur = next(i for i, p in enumerate(pages) if any(s["kind"] for s in p))
                STATE["cur"] = cur
                STATE["rotate"] = f.get("rotate", "0") == "1"
                every = int(f.get("every", STATE["every"]) or 5)
                STATE["every"] = every if every in TABLES["rotateChoices"] else 5
                STATE["simKeys"] = [f"cur={cur}", f"rotate={int(STATE['rotate'])}",
                                    f"every={STATE['every']}"]
                print(f"[preview] pages saved: current={cur} rotate={STATE['rotate']}", flush=True)
                return self.json({"ok": True})
            if u.path == "/api/settings":
                warning = None
                if "tz" in f and f["tz"].isdigit() and int(f["tz"]) < len(TABLES["timezones"]):
                    STATE["tzId"] = int(f["tz"])
                if "beep" in f:
                    STATE["beep"] = f["beep"] == "1"
                if "showIp" in f:
                    STATE["showIp"] = f["showIp"] == "1"
                if not city:
                    STATE["location"] = None
                elif "lat" in f and "lon" in f:
                    try:
                        STATE["location"] = (city[:31], float(f["lat"]), float(f["lon"]))
                    except ValueError:
                        warning = "Bad coordinates — location unchanged"
                elif lookup is not None:
                    if lookup:
                        h = lookup[0]
                        STATE["location"] = (h["name"], h["lat"], h["lon"])
                    else:
                        warning = "City not found — location unchanged"
                return self.json({"ok": True, **({"warning": warning} if warning else {})})
            if u.path == "/api/webui":
                if f.get("on") != "0":
                    return self.json({"ok": False,
                                      "error": "the web UI can only be turned on from the clock"}, 400)
                STATE["simKeys"].append("webui=0")
                print("[preview] device would stop its web UI (preview keeps serving)", flush=True)
                return self.json({"ok": True})
            if u.path in ("/api/beep", "/api/wifi", "/api/forget"):
                print(f"[preview] device action: {u.path} (ignored in preview)", flush=True)
                return self.json({"ok": True})
            if u.path == "/api/_pad":
                pad, gesture = f.get("pad", ""), f.get("gesture", "tap")
                if pad not in ("L", "M", "R") or gesture not in ("tap", "hold"):
                    return self.json({"ok": False, "error": "bad pad"}, 400)
                err = ensure_renderer()
                if err:
                    return self.json({"ok": False, "error": err}, 500)
                st = SIM.call(f"!pad {pad} {gesture}")
                for act in st["actions"]:
                    if act != "save":
                        print(f"[preview] device would: {act}", flush=True)
                return self.json({"ok": True, **st})
            if u.path == "/api/_mock":
                STATE["time"] = f.get("time") or None
                STATE["date"] = f.get("date") or None
                try:
                    STATE["temp"] = max(-40, min(50, int(f.get("temp", STATE["temp"]))))
                    STATE["code"] = int(f.get("code", STATE["code"]))
                except ValueError:
                    pass
                STATE["synced"] = f.get("synced") == "1"
                STATE["wx"] = f.get("wx") == "1"
                return self.json({"ok": True})
        self.json({"ok": False, "error": "not found"}, 404)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", type=int, default=8377)
    ap.add_argument("--plain", action="store_true",
                    help="hide the preview controls and virtual pads (for screenshots)")
    args = ap.parse_args()
    global PLAIN
    PLAIN = args.plain
    err = ensure_renderer()
    if err:
        print("[preview] WARNING: " + err, flush=True)
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"[preview] CosmoLibre UI preview on http://localhost:{args.port}/", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
