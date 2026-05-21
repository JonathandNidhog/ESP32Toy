#!/usr/bin/env python3
"""
Fetch the upstream DoomGeneric C core into this Arduino sketch folder.

This script imports:
- all upstream headers from ozkl/doomgeneric/doomgeneric
- selected core C files needed by the Doom engine
- missing included headers from Chocolate Doom as a fallback
- no desktop platform backend files

Then it applies ESP32Toy-specific patches for:
- classic 320x200 internal Doom framebuffer
- LittleFS IWAD lookup under /littlefs
- sound disabled for the current no-audio hardware
- PSRAM-first large heap allocations
- PSRAM BSS placement for several large renderer buffers
"""
from __future__ import annotations

import json
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Iterable

TREE_API = "https://api.github.com/repos/ozkl/doomgeneric/git/trees/master?recursive=1"
RAW_BASE = "https://raw.githubusercontent.com/ozkl/doomgeneric/master/doomgeneric"
CHOCOLATE_RAW_BASE = "https://raw.githubusercontent.com/chocolate-doom/chocolate-doom/master/src/doom"
USER_AGENT = "ESP32Toy-DoomGeneric-Fetcher/1.8"
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
    "w_file_stdc.c",
    "w_main.c",
    "w_wad.c",
    "z_zone.c",
    "i_input.c",
    "i_video.c",
    "doomgeneric.c",
}

PATCH_TARGETS = {
    "doomgeneric.h",
    "config.h",
    "doomfeatures.h",
    "i_system.c",
    "doomgeneric.c",
    "r_plane.c",
    "r_bsp.c",
    "r_things.c",
}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+\.h)"', re.MULTILINE)


def fetch_bytes(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=45) as resp:
        return resp.read()


def fetch_json(url: str):
    return json.loads(fetch_bytes(url).decode("utf-8"))


def list_upstream_files() -> dict[str, str]:
    data = fetch_json(TREE_API)
    if data.get("truncated"):
        raise RuntimeError("GitHub tree result was truncated; cannot safely import DoomGeneric core")

    out: dict[str, str] = {}
    for item in data.get("tree", []):
        path = item.get("path", "")
        if not path.startswith("doomgeneric/") or item.get("type") != "blob":
            continue
        name = Path(path).name
        if name.endswith(".h") or name in CORE_C_FILES:
            out[name] = f"{RAW_BASE}/{name}"
    return out


def require(path: Path) -> None:
    if not path.exists():
        raise RuntimeError(f"Patch target missing after import: {path.name}")


def collect_local_includes() -> set[str]:
    includes: set[str] = set()
    for path in TARGET_DIR.glob("*.c"):
        includes.update(INCLUDE_RE.findall(path.read_text(encoding="utf-8", errors="ignore")))
    for path in TARGET_DIR.glob("*.h"):
        includes.update(INCLUDE_RE.findall(path.read_text(encoding="utf-8", errors="ignore")))
    return includes


def fetch_missing_headers_from_chocolate(imported: list[str]) -> None:
    # Resolve local quoted header includes until fixed point.  ozkl/doomgeneric
    # is not fully self-contained; some .c files still include headers that only
    # exist in the fuller Chocolate Doom tree, such as st_stuff.h.
    for _ in range(12):
        missing = sorted(
            name for name in collect_local_includes()
            if not (TARGET_DIR / name).exists()
        )
        if not missing:
            return

        progress = False
        for name in missing:
            url = f"{CHOCOLATE_RAW_BASE}/{name}"
            print(f"[FALLBACK] {name} <- Chocolate Doom")
            try:
                data = fetch_bytes(url)
            except urllib.error.HTTPError as exc:
                if exc.code == 404:
                    raise RuntimeError(
                        f"Missing required header {name}; not found in ozkl/doomgeneric or Chocolate Doom fallback"
                    ) from exc
                raise
            (TARGET_DIR / name).write_bytes(data)
            imported.append(name)
            progress = True

        if not progress:
            break

    unresolved = sorted(
        name for name in collect_local_includes()
        if not (TARGET_DIR / name).exists()
    )
    if unresolved:
        raise RuntimeError(f"Unresolved local headers after fallback import: {', '.join(unresolved)}")


def ensure_esp_attr_include(text: str, anchor: str) -> str:
    if "#include <esp_attr.h>" in text:
        return text
    if anchor not in text:
        raise RuntimeError(f"Could not find include anchor: {anchor!r}")
    return text.replace(
        anchor,
        anchor + "#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)\n#include <esp_attr.h>\n#endif\n",
        1,
    )


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
        text += "\n#undef FEATURE_SOUND\n"
    path.write_text(text, encoding="utf-8")


def patch_i_system_c(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    include_anchor = "#include <string.h>\n"
    if "#include <esp_heap_caps.h>" not in text:
        text = text.replace(
            include_anchor,
            include_anchor + "\n#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)\n#include <esp_heap_caps.h>\n#endif\n",
            1,
        )
    old = "        zonemem = malloc(*size);"
    new = (
        "#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)\n"
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
        if old not in text:
            raise RuntimeError("Could not find zone malloc site in i_system.c")
        text = text.replace(old, new, 1)
    path.write_text(text, encoding="utf-8")


def patch_doomgeneric_c(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    include_anchor = "#include <stdio.h>\n"
    if "#include <esp_heap_caps.h>" not in text:
        text = text.replace(
            include_anchor,
            include_anchor + "\n#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)\n#include <esp_heap_caps.h>\n#endif\n",
            1,
        )
    old = "\tDG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);"
    new = (
        "#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)\n"
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
        if old not in text:
            raise RuntimeError("Could not find framebuffer malloc site in doomgeneric.c")
        text = text.replace(old, new, 1)
    path.write_text(text, encoding="utf-8")


def patch_r_plane_c(path: Path) -> None:
    text = ensure_esp_attr_include(path.read_text(encoding="utf-8"), "#include <stdlib.h>\n")
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
        if new not in text:
            if old not in text:
                raise RuntimeError(f"Could not patch r_plane.c symbol: {old}")
            text = text.replace(old, new, 1)
    path.write_text(text, encoding="utf-8")


def patch_r_bsp_c(path: Path) -> None:
    text = ensure_esp_attr_include(path.read_text(encoding="utf-8"), "#include \"doomdef.h\"\n")
    replacements = {
        "drawseg_t\tdrawsegs[MAXDRAWSEGS];": "EXT_RAM_BSS_ATTR drawseg_t\tdrawsegs[MAXDRAWSEGS];",
        "cliprange_t\tsolidsegs[MAXSEGS];": "EXT_RAM_BSS_ATTR cliprange_t\tsolidsegs[MAXSEGS];",
    }
    for old, new in replacements.items():
        if new not in text:
            if old not in text:
                raise RuntimeError(f"Could not patch r_bsp.c symbol: {old}")
            text = text.replace(old, new, 1)
    path.write_text(text, encoding="utf-8")


def patch_r_things_c(path: Path) -> None:
    text = ensure_esp_attr_include(path.read_text(encoding="utf-8"), "#include <stdlib.h>\n")
    replacements = {
        "short\t\tnegonearray[SCREENWIDTH];": "EXT_RAM_BSS_ATTR short\t\tnegonearray[SCREENWIDTH];",
        "short\t\tscreenheightarray[SCREENWIDTH];": "EXT_RAM_BSS_ATTR short\t\tscreenheightarray[SCREENWIDTH];",
        "spriteframe_t\tsprtemp[29];": "EXT_RAM_BSS_ATTR spriteframe_t\tsprtemp[29];",
    }
    for old, new in replacements.items():
        if new not in text:
            if old not in text:
                raise RuntimeError(f"Could not patch r_things.c symbol: {old}")
            text = text.replace(old, new, 1)
    path.write_text(text, encoding="utf-8")


def apply_esp32toy_patches() -> None:
    for name in PATCH_TARGETS:
        require(TARGET_DIR / name)

    patch_doomgeneric_h(TARGET_DIR / "doomgeneric.h")
    print("[PATCH] doomgeneric.h -> classic 320x200 internal framebuffer")
    patch_config_h(TARGET_DIR / "config.h")
    print('[PATCH] config.h -> FILES_DIR "/littlefs"')
    patch_doomfeatures_h(TARGET_DIR / "doomfeatures.h")
    print("[PATCH] doomfeatures.h -> FEATURE_SOUND disabled")
    patch_i_system_c(TARGET_DIR / "i_system.c")
    print("[PATCH] i_system.c -> zone memory prefers PSRAM")
    patch_doomgeneric_c(TARGET_DIR / "doomgeneric.c")
    print("[PATCH] doomgeneric.c -> framebuffer prefers PSRAM")
    patch_r_plane_c(TARGET_DIR / "r_plane.c")
    print("[PATCH] r_plane.c -> large renderer BSS buffers to PSRAM")
    patch_r_bsp_c(TARGET_DIR / "r_bsp.c")
    print("[PATCH] r_bsp.c -> draw/clip BSS buffers to PSRAM")
    patch_r_things_c(TARGET_DIR / "r_things.c")
    print("[PATCH] r_things.c -> sprite helper BSS buffers to PSRAM")


def write_manifest(imported: Iterable[str]) -> None:
    lines = [
        "Imported from https://github.com/ozkl/doomgeneric/tree/master/doomgeneric",
        "Fallback headers may be imported from https://github.com/chocolate-doom/chocolate-doom/tree/master/src/doom",
        "Source selection: all ozkl headers + selected core C files + missing included Chocolate Doom headers.",
        "",
        "ESP32Toy post-import patches:",
        "- classic 320x200 internal framebuffer",
        "- /littlefs IWAD discovery",
        "- sound disabled",
        "- PSRAM-first zone/framebuffer heap allocations",
        "- renderer BSS buffers moved to PSRAM using EXT_RAM_BSS_ATTR",
        "",
        "Files:",
    ]
    lines.extend(f"- {name}" for name in sorted(imported))
    (TARGET_DIR / "UPSTREAM_IMPORT_MANIFEST.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    print("[ESP32Toy Doom] Listing upstream DoomGeneric files...")
    try:
        upstream_files = list_upstream_files()
    except (urllib.error.URLError, RuntimeError) as exc:
        print(f"[ERROR] Could not list upstream files: {exc}", file=sys.stderr)
        return 2

    missing_core = sorted(name for name in CORE_C_FILES if name not in upstream_files)
    if missing_core:
        print(f"[ERROR] Upstream file list is missing required C files: {', '.join(missing_core)}", file=sys.stderr)
        return 5

    import_names = sorted(upstream_files.keys())
    imported: list[str] = []
    for name in import_names:
        print(f"[FETCH] {name}")
        try:
            data = fetch_bytes(upstream_files[name])
        except urllib.error.URLError as exc:
            print(f"[ERROR] Failed to download {name}: {exc}", file=sys.stderr)
            return 3
        (TARGET_DIR / name).write_bytes(data)
        imported.append(name)

    try:
        fetch_missing_headers_from_chocolate(imported)
        apply_esp32toy_patches()
    except RuntimeError as exc:
        print(f"[ERROR] Patch/import stage failed: {exc}", file=sys.stderr)
        return 4

    write_manifest(imported)

    print("")
    print(f"[DONE] Imported {len(imported)} files into:")
    print(f"       {TARGET_DIR}")
    print("[NEXT] Open ESP32Toy_DoomGeneric_Phase3.ino in Arduino IDE and compile.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
