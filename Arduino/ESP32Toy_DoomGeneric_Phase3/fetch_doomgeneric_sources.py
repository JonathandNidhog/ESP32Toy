#!/usr/bin/env python3
"""
Fetch the upstream DoomGeneric C core into this Arduino sketch folder.

Why this exists:
- The ESP32Toy repo keeps the custom ESP32 platform layer tracked directly.
- The upstream DoomGeneric engine is large, so this script imports the exact
  core C/H files from the official upstream repository on demand.
- It intentionally skips desktop platform backends such as SDL/X11/Win32 and
  keeps ESP32Toy's own doomgeneric_esp32toy.cpp platform bridge.
- After importing, it applies ESP32Toy-specific patch steps so the generated
  sketch tree is closer to a board-flash, PSRAM-backed build immediately.

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
USER_AGENT = "ESP32Toy-DoomGeneric-Fetcher/1.5"
TARGET_DIR = Path(__file__).resolve().parent

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


def ensure_esp_attr_include(text: str, anchor: str) -> str:
    if "#include <esp_attr.h>" in text:
        return text
    if anchor not in text:
        raise RuntimeError(f"Could not find include anchor: {anchor!r}")
    return text.replace(anchor, anchor + "#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)\n#include <esp_attr.h>\n#endif\n", 1)


def patch_doomgeneric_h(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = re.sub(
        r"#ifndef DOOMGENERIC_RESX\s*\n#define DOOMGENERIC_RESX\s+\d+",
        "#ifndef DOOMGENERIC_RESX\n#define DOOMGENERIC_RESX 320",
        text,
        count=1,
    )
    text = re.sub(
        r"#ifndef DOOMGENERIC_RESY\s*\n#define DOOMGENERIC_RESY\s+\d+",
        "#ifndef DOOMGENERIC_RESY\n#define DOOMGENERIC_RESY 200",
        text,
        count=1,
    )
    path.write_text(text, encoding="utf-8")


def patch_config_h(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text, count = re.subn(
        r'#define\s+FILES_DIR\s+"[^"]*"',
        '#define FILES_DIR "/littlefs"',
        text,
        count=1,
    )
    if count != 1:
        raise RuntimeError("Could not patch FILES_DIR in config.h")
    path.write_text(text, encoding="utf-8")


def patch_doomfeatures_h(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = re.sub(r"//\s*#undef\s+FEATURE_SOUND", "#undef FEATURE_SOUND", text, count=1)
    if "#undef FEATURE_SOUND" not in text:
        if "FEATURE_SOUND" not in text:
            raise RuntimeError("Could not locate FEATURE_SOUND in doomfeatures.h")
        text += "\n#undef FEATURE_SOUND\n"
    path.write_text(text, encoding="utf-8")


def patch_i_system_c(path: Path) -> None:
    text = path.read_text(encoding="utf-8")

    include_anchor = "#include <string.h>\n"
    include_patch = (
        "#include <string.h>\n"
        "\n"
        "#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)\n"
        "#include <esp_heap_caps.h>\n"
        "#endif\n"
    )
    if "#include <esp_heap_caps.h>" not in text:
        if include_anchor not in text:
            raise RuntimeError("Could not find include anchor in i_system.c")
        text = text.replace(include_anchor, include_patch, 1)

    old_alloc = "        zonemem = malloc(*size);"
    new_alloc = (
        "#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)\n"
        "        // Doom's large zone block should live in PSRAM on the ESP32-S3.\n"
        "        zonemem = heap_caps_malloc(*size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);\n"
        "        if (zonemem == NULL)\n"
        "        {\n"
        "            zonemem = malloc(*size);\n"
        "        }\n"
        "#else\n"
        "        zonemem = malloc(*size);\n"
        "#endif"
    )
    if "heap_caps_malloc(*size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)" not in text:
        if old_alloc not in text:
            raise RuntimeError("Could not find zone malloc site in i_system.c")
        text = text.replace(old_alloc, new_alloc, 1)

    path.write_text(text, encoding="utf-8")


def patch_doomgeneric_c(path: Path) -> None:
    text = path.read_text(encoding="utf-8")

    include_anchor = "#include <stdio.h>\n"
    include_patch = (
        "#include <stdio.h>\n"
        "\n"
        "#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)\n"
        "#include <esp_heap_caps.h>\n"
        "#endif\n"
    )
    if "#include <esp_heap_caps.h>" not in text:
        if include_anchor not in text:
            raise RuntimeError("Could not find include anchor in doomgeneric.c")
        text = text.replace(include_anchor, include_patch, 1)

    old_alloc = "\tDG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);"
    new_alloc = (
        "#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)\n"
        "\t// Keep the 320x200 RGBA Doom framebuffer out of scarce internal RAM.\n"
        "\tDG_ScreenBuffer = heap_caps_malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4,\n"
        "\t                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);\n"
        "\tif (DG_ScreenBuffer == NULL)\n"
        "\t{\n"
        "\t\tDG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);\n"
        "\t}\n"
        "#else\n"
        "\tDG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);\n"
        "#endif"
    )
    if "heap_caps_malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4" not in text:
        if old_alloc not in text:
            raise RuntimeError("Could not find framebuffer malloc site in doomgeneric.c")
        text = text.replace(old_alloc, new_alloc, 1)

    path.write_text(text, encoding="utf-8")


def patch_r_plane_c(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = ensure_esp_attr_include(text, "#include <stdlib.h>\n")

    replacements = {
        "visplane_t\t\t\tvisplanes[MAXVISPLANES];": "EXT_RAM_BSS_ATTR visplane_t\t\t\tvisplanes[MAXVISPLANES];",
        "short\t\t\topenings[MAXOPENINGS];": "EXT_RAM_BSS_ATTR short\t\t\topenings[MAXOPENINGS];",
        "short\t\t\tfloorclip[SCREENWIDTH];": "EXT_RAM_BSS_ATTR short\t\t\tfloorclip[SCREENWIDTH];",
        "short\t\t\tceilingclip[SCREENWIDTH];": "EXT_RAM_BSS_ATTR short\t\t\tceilingclip[SCREENWIDTH];",
        "int\t\t\tspanstart[SCREENHEIGHT];": "EXT_RAM_BSS_ATTR int\t\t\tspanstart[SCREENHEIGHT];",
        "int\t\t\tspanstop[SCREENHEIGHT];": "EXT_RAM_BSS_ATTR int\t\t\tspanstop[SCREENHEIGHT];",
        "fixed_t\t\t\tyslope[SCREENHEIGHT];": "EXT_RAM_BSS_ATTR fixed_t\t\t\tyslope[SCREENHEIGHT];",
        "fixed_t\t\t\tdistscale[SCREENWIDTH];": "EXT_RAM_BSS_ATTR fixed_t\t\t\tdistscale[SCREENWIDTH];",
        "fixed_t\t\t\tcachedheight[SCREENHEIGHT];": "EXT_RAM_BSS_ATTR fixed_t\t\t\tcachedheight[SCREENHEIGHT];",
        "fixed_t\t\t\tcacheddistance[SCREENHEIGHT];": "EXT_RAM_BSS_ATTR fixed_t\t\t\tcacheddistance[SCREENHEIGHT];",
        "fixed_t\t\t\tcachedxstep[SCREENHEIGHT];": "EXT_RAM_BSS_ATTR fixed_t\t\t\tcachedxstep[SCREENHEIGHT];",
        "fixed_t\t\t\tcachedystep[SCREENHEIGHT];": "EXT_RAM_BSS_ATTR fixed_t\t\t\tcachedystep[SCREENHEIGHT];",
    }

    for old, new in replacements.items():
        if new in text:
            continue
        if old not in text:
            raise RuntimeError(f"Could not patch r_plane.c symbol: {old}")
        text = text.replace(old, new, 1)

    path.write_text(text, encoding="utf-8")


def patch_r_bsp_c(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = ensure_esp_attr_include(text, "#include \"doomdef.h\"\n")

    replacements = {
        "drawseg_t\tdrawsegs[MAXDRAWSEGS];": "EXT_RAM_BSS_ATTR drawseg_t\tdrawsegs[MAXDRAWSEGS];",
        "cliprange_t\tsolidsegs[MAXSEGS];": "EXT_RAM_BSS_ATTR cliprange_t\tsolidsegs[MAXSEGS];",
    }
    for old, new in replacements.items():
        if new in text:
            continue
        if old not in text:
            raise RuntimeError(f"Could not patch r_bsp.c symbol: {old}")
        text = text.replace(old, new, 1)

    path.write_text(text, encoding="utf-8")


def patch_r_things_c(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = ensure_esp_attr_include(text, "#include <stdlib.h>\n")

    replacements = {
        "short\t\tnegonearray[SCREENWIDTH];": "EXT_RAM_BSS_ATTR short\t\tnegonearray[SCREENWIDTH];",
        "short\t\tscreenheightarray[SCREENWIDTH];": "EXT_RAM_BSS_ATTR short\t\tscreenheightarray[SCREENWIDTH];",
        "spriteframe_t\tsprtemp[29];": "EXT_RAM_BSS_ATTR spriteframe_t\tsprtemp[29];",
    }
    for old, new in replacements.items():
        if new in text:
            continue
        if old not in text:
            raise RuntimeError(f"Could not patch r_things.c symbol: {old}")
        text = text.replace(old, new, 1)

    path.write_text(text, encoding="utf-8")


def apply_esp32toy_patches() -> None:
    doom_h = TARGET_DIR / "doomgeneric.h"
    config_h = TARGET_DIR / "config.h"
    features_h = TARGET_DIR / "doomfeatures.h"
    i_system_c = TARGET_DIR / "i_system.c"
    doomgeneric_c = TARGET_DIR / "doomgeneric.c"
    r_plane_c = TARGET_DIR / "r_plane.c"
    r_bsp_c = TARGET_DIR / "r_bsp.c"
    r_things_c = TARGET_DIR / "r_things.c"

    targets = (doom_h, config_h, features_h, i_system_c, doomgeneric_c, r_plane_c, r_bsp_c, r_things_c)
    missing = [str(path.name) for path in targets if not path.exists()]
    if missing:
        raise RuntimeError(f"Patch target(s) missing after import: {', '.join(missing)}")

    patch_doomgeneric_h(doom_h)
    print("[PATCH] doomgeneric.h -> keep classic 320x200 internal framebuffer")
    patch_config_h(config_h)
    print('[PATCH] config.h -> FILES_DIR "/littlefs"')
    patch_doomfeatures_h(features_h)
    print("[PATCH] doomfeatures.h -> force FEATURE_SOUND off")
    patch_i_system_c(i_system_c)
    print("[PATCH] i_system.c -> prefer PSRAM for Doom zone memory")
    patch_doomgeneric_c(doomgeneric_c)
    print("[PATCH] doomgeneric.c -> prefer PSRAM for Doom framebuffer")
    patch_r_plane_c(r_plane_c)
    print("[PATCH] r_plane.c -> move large renderer BSS buffers to PSRAM")
    patch_r_bsp_c(r_bsp_c)
    print("[PATCH] r_bsp.c -> move draw/clip BSS buffers to PSRAM")
    patch_r_things_c(r_things_c)
    print("[PATCH] r_things.c -> move sprite helper BSS buffers to PSRAM")


def write_manifest(imported: Iterable[str]) -> None:
    manifest = TARGET_DIR / "UPSTREAM_IMPORT_MANIFEST.txt"
    lines = [
        "Imported from https://github.com/ozkl/doomgeneric/tree/master/doomgeneric",
        "Source selection: all headers + core C files from upstream Makefile,",
        "excluding desktop platform backends.",
        "",
        "ESP32Toy post-import patches:",
        "- doomgeneric.h: internal framebuffer kept at classic 320x200",
        "- platform bridge: downsamples 320x200 to the 160x128 ST7735 panel",
        "- config.h: FILES_DIR changed to /littlefs",
        "- doomfeatures.h: FEATURE_SOUND explicitly disabled",
        "- i_system.c: Doom zone memory prefers ESP32 PSRAM via heap_caps_malloc",
        "- doomgeneric.c: Doom framebuffer prefers ESP32 PSRAM via heap_caps_malloc",
        "- r_plane.c/r_bsp.c/r_things.c: large renderer static BSS buffers moved to PSRAM using EXT_RAM_BSS_ATTR",
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

    try:
        apply_esp32toy_patches()
    except RuntimeError as exc:
        print(f"[ERROR] Patch stage failed: {exc}", file=sys.stderr)
        return 4

    write_manifest(imported)

    print("")
    print(f"[DONE] Imported {len(imported)} files into:")
    print(f"       {TARGET_DIR}")
    print(f"[INFO] Skipped {len(skipped)} non-core or desktop-specific files.")
    print("[NEXT] Open ESP32Toy_DoomGeneric_Phase3.ino in Arduino IDE and compile.")
    print("       This imported tree is patched for classic 320x200 Doom rendering,")
    print("       /littlefs IWAD discovery, no-audio runtime mode, PSRAM-first")
    print("       heap allocations, and external-RAM renderer BSS buffers.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
