#!/usr/bin/env python3
"""
Fetch the upstream DoomGeneric C core into this Arduino sketch folder.

Why this exists:
- The ESP32Toy repo keeps the custom ESP32 platform layer tracked directly.
- The upstream DoomGeneric engine is large, so this script imports the exact
  core C/H files from the official upstream repository on demand.
- It intentionally skips desktop platform backends such as SDL/X11/Win32 and
  keeps ESP32Toy's own doomgeneric_esp32toy.cpp platform bridge.

Run from Windows PowerShell or a terminal:
    python fetch_doomgeneric_sources.py

The script writes the imported files next to itself.
"""
from __future__ import annotations

import json
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Iterable

API_DIR = "https://api.github.com/repos/ozkl/doomgeneric/contents/doomgeneric?ref=master"
USER_AGENT = "ESP32Toy-DoomGeneric-Fetcher/1.0"
TARGET_DIR = Path(__file__).resolve().parent

# C implementation files derived from upstream doomgeneric/Makefile's SRC_DOOM
# list, excluding the desktop platform backend doomgeneric_xlib.c.  Headers are
# downloaded wholesale because they are lightweight and transitively included.
CORE_C_FILES = {
    "dummy.c",
    "am_map.c",
    "doomdef.c",
    "doomstat.c",
    "dstrings.c",
    "d_event.c",
    "d_items.c",
    "d_iwad.c",
    "d_loop.c",
    "d_main.c",
    "d_mode.c",
    "d_net.c",
    "f_finale.c",
    "f_wipe.c",
    "g_game.c",
    "hu_lib.c",
    "hu_stuff.c",
    "info.c",
    "i_cdmus.c",
    "i_endoom.c",
    "i_joystick.c",
    "i_scale.c",
    "i_sound.c",
    "i_system.c",
    "i_timer.c",
    "memio.c",
    "m_argv.c",
    "m_bbox.c",
    "m_cheat.c",
    "m_config.c",
    "m_controls.c",
    "m_fixed.c",
    "m_menu.c",
    "m_misc.c",
    "m_random.c",
    "p_ceilng.c",
    "p_doors.c",
    "p_enemy.c",
    "p_floor.c",
    "p_inter.c",
    "p_lights.c",
    "p_map.c",
    "p_maputl.c",
    "p_mobj.c",
    "p_plats.c",
    "p_pspr.c",
    "p_saveg.c",
    "p_setup.c",
    "p_sight.c",
    "p_spec.c",
    "p_switch.c",
    "p_telept.c",
    "p_tick.c",
    "p_user.c",
    "r_bsp.c",
    "r_data.c",
    "r_draw.c",
    "r_main.c",
    "r_plane.c",
    "r_segs.c",
    "r_sky.c",
    "r_things.c",
    "sha1.c",
    "sounds.c",
    "statdump.c",
    "st_lib.c",
    "st_stuff.c",
    "s_sound.c",
    "tables.c",
    "v_video.c",
    "wi_stuff.c",
    "w_checksum.c",
    "w_file.c",
    "w_main.c",
    "w_wad.c",
    "z_zone.c",
    "w_file_stdc.c",
    "i_input.c",
    "i_video.c",
    "doomgeneric.c",
}


def fetch_bytes(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=45) as resp:
        return resp.read()


def fetch_json(url: str):
    return json.loads(fetch_bytes(url).decode("utf-8"))


def should_import(name: str) -> bool:
    if name.endswith(".h"):
        return True
    if name.endswith(".c"):
        return name in CORE_C_FILES
    return False


def patch_doomgeneric_h(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = re.sub(
        r"#ifndef DOOMGENERIC_RESX\s*\n#define DOOMGENERIC_RESX\s+\d+",
        "#ifndef DOOMGENERIC_RESX\n#define DOOMGENERIC_RESX 160",
        text,
        count=1,
    )
    text = re.sub(
        r"#ifndef DOOMGENERIC_RESY\s*\n#define DOOMGENERIC_RESY\s+\d+",
        "#ifndef DOOMGENERIC_RESY\n#define DOOMGENERIC_RESY 128",
        text,
        count=1,
    )
    path.write_text(text, encoding="utf-8")


def write_manifest(imported: Iterable[str]) -> None:
    manifest = TARGET_DIR / "UPSTREAM_IMPORT_MANIFEST.txt"
    lines = [
        "Imported from https://github.com/ozkl/doomgeneric/tree/master/doomgeneric",
        "Source selection: all headers + core C files from upstream Makefile,",
        "excluding desktop platform backends.",
        "",
        "Files:",
    ]
    lines.extend(f"- {name}" for name in sorted(imported))
    manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    print("[ESP32Toy Doom] Listing upstream DoomGeneric files...")
    try:
        entries = fetch_json(API_DIR)
    except urllib.error.URLError as exc:
        print(f"[ERROR] Could not reach GitHub: {exc}", file=sys.stderr)
        return 2

    imported: list[str] = []
    skipped: list[str] = []

    for entry in entries:
        name = entry.get("name", "")
        download_url = entry.get("download_url")
        if not name or not download_url:
            continue
        if not should_import(name):
            skipped.append(name)
            continue

        print(f"[FETCH] {name}")
        try:
            data = fetch_bytes(download_url)
        except urllib.error.URLError as exc:
            print(f"[ERROR] Failed to download {name}: {exc}", file=sys.stderr)
            return 3

        (TARGET_DIR / name).write_bytes(data)
        imported.append(name)

    doom_h = TARGET_DIR / "doomgeneric.h"
    if doom_h.exists():
        patch_doomgeneric_h(doom_h)
        print("[PATCH] doomgeneric.h -> 160x128")
    else:
        print("[ERROR] doomgeneric.h was not imported", file=sys.stderr)
        return 4

    write_manifest(imported)

    print("")
    print(f"[DONE] Imported {len(imported)} files into:")
    print(f"       {TARGET_DIR}")
    print(f"[INFO] Skipped {len(skipped)} non-core or desktop-specific files.")
    print("[NEXT] Open Phase3_entry.ino in Arduino IDE and compile.")
    print("       The next integration pass will address WAD file loading and any")
    print("       ESP32/Arduino compile differences reported by the IDE.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
