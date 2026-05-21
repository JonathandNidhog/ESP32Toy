#!/usr/bin/env python3
"""
Fetch / repair the DoomGeneric C core for the ESP32Toy Doom Runtime.

Default behavior is cache-friendly:
- existing local files are reused
- only missing files are downloaded
- pass --force to refresh everything from upstream

The script imports:
- all upstream headers from ozkl/doomgeneric/doomgeneric
- selected core C files needed by the Doom engine
- missing reachable headers from Chocolate Doom as a fallback
- no desktop platform backend C files

Then it applies ESP32Toy-specific patches for:
- classic 320x200 internal Doom framebuffer
- LittleFS IWAD lookup under /littlefs
- sound disabled for the current no-audio hardware
- PSRAM-first large heap allocations
- PSRAM BSS placement for several large renderer buffers
"""
from __future__ import annotations

import argparse
import json
import re
import ssl
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Iterable

TREE_API = "https://api.github.com/repos/ozkl/doomgeneric/git/trees/master?recursive=1"
RAW_BASE = "https://raw.githubusercontent.com/ozkl/doomgeneric/master/doomgeneric"
CHOCOLATE_DOOM_BASES = (
    "https://raw.githubusercontent.com/chocolate-doom/chocolate-doom/master/src/doom",
    "https://raw.githubusercontent.com/chocolate-doom/chocolate-doom/master/src",
)
USER_AGENT = "ESP32Toy-DoomGeneric-Fetcher/2.1"
TARGET_DIR = Path(__file__).resolve().parent
MAX_DOWNLOAD_RETRIES = 5

CORE_C_FILES = {
    "dummy.c", "am_map.c", "doomdef.c", "doomstat.c", "dstrings.c",
    "d_event.c", "d_items.c", "d_iwad.c", "d_loop.c", "d_main.c",
    "d_mode.c", "d_net.c", "f_finale.c", "f_wipe.c", "g_game.c",
    "hu_lib.c", "hu_stuff.c", "info.c", "i_cdmus.c", "i_endoom.c",
    "i_joystick.c", "i_scale.c", "i_sound.c", "i_system.c", "i_timer.c",
    "memio.c", "m_argv.c", "m_bbox.c", "m_cheat.c", "m_config.c",
    "m_controls.c", "m_fixed.c", "m_menu.c", "m_misc.c", "m_random.c",
    "p_ceilng.c", "p_doors.c", "p_enemy.c", "p_floor.c", "p_inter.c",
    "p_lights.c", "p_map.c", "p_maputl.c", "p_mobj.c", "p_plats.c",
    "p_pspr.c", "p_saveg.c", "p_setup.c", "p_sight.c", "p_spec.c",
    "p_switch.c", "p_telept.c", "p_tick.c", "p_user.c", "r_bsp.c",
    "r_data.c", "r_draw.c", "r_main.c", "r_plane.c", "r_segs.c",
    "r_sky.c", "r_things.c", "sha1.c", "sounds.c", "statdump.c",
    "st_lib.c", "st_stuff.c", "s_sound.c", "tables.c", "v_video.c",
    "wi_stuff.c", "w_checksum.c", "w_file.c", "w_file_stdc.c",
    "w_main.c", "w_wad.c", "z_zone.c", "i_input.c", "i_video.c",
    "doomgeneric.c",
}

PATCH_TARGETS = {
    "doomgeneric.h", "config.h", "doomfeatures.h", "i_system.c",
    "doomgeneric.c", "r_plane.c", "r_bsp.c", "r_things.c",
}

DESKTOP_ONLY_BASENAMES = {
    "SDL.h", "SDL_audio.h", "SDL_cdrom.h", "SDL_endian.h", "SDL_error.h",
    "SDL_events.h", "SDL_joystick.h", "SDL_keyboard.h", "SDL_keycode.h",
    "SDL_main.h", "SDL_mixer.h", "SDL_mouse.h", "SDL_mutex.h",
    "SDL_opengl.h", "SDL_rwops.h", "SDL_scancode.h", "SDL_stdinc.h",
    "SDL_surface.h", "SDL_thread.h", "SDL_timer.h", "SDL_types.h",
    "SDL_version.h", "SDL_video.h",
}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+\.h)"', re.MULTILINE)


def is_desktop_only_header(name: str) -> bool:
    normalized = name.replace("\\", "/")
    lower = normalized.lower()
    base = Path(normalized).name
    return (
        base in DESKTOP_ONLY_BASENAMES
        or lower.startswith("sdl/")
        or lower.startswith("sdl2/")
        or lower.startswith("sdl3/")
        or base.lower().startswith("sdl_")
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Fetch/repair DoomGeneric core sources for ESP32Toy.")
    parser.add_argument("--force", action="store_true", help="Redownload files even if they already exist locally.")
    return parser.parse_args()


def fetch_bytes(url: str) -> bytes:
    last_exc: BaseException | None = None
    for attempt in range(1, MAX_DOWNLOAD_RETRIES + 1):
        req = urllib.request.Request(
            url,
            headers={
                "User-Agent": USER_AGENT,
                "Accept": "application/vnd.github.raw, text/plain, */*",
                "Connection": "close",
            },
        )
        try:
            with urllib.request.urlopen(req, timeout=75) as resp:
                return resp.read()
        except urllib.error.HTTPError as exc:
            if exc.code == 404:
                raise
            last_exc = exc
        except (urllib.error.URLError, TimeoutError, ssl.SSLError, ConnectionResetError) as exc:
            last_exc = exc
        if attempt < MAX_DOWNLOAD_RETRIES:
            sleep_s = min(2 ** (attempt - 1), 8)
            print(f"[RETRY] {attempt}/{MAX_DOWNLOAD_RETRIES} failed for {url}: {last_exc}. Retrying in {sleep_s}s...")
            time.sleep(sleep_s)
    raise urllib.error.URLError(f"Failed after {MAX_DOWNLOAD_RETRIES} attempts: {url}; last error: {last_exc}")


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


def scan_quoted_includes(path: Path) -> set[str]:
    if not path.exists():
        return set()
    return set(INCLUDE_RE.findall(path.read_text(encoding="utf-8", errors="ignore")))


def collect_reachable_local_headers() -> set[str]:
    reachable: set[str] = set()
    queue: list[str] = []
    for c_name in CORE_C_FILES:
        queue.extend(scan_quoted_includes(TARGET_DIR / c_name))
    while queue:
        name = queue.pop(0)
        if is_desktop_only_header(name) or name in reachable:
            continue
        reachable.add(name)
        header_path = TARGET_DIR / name
        if header_path.exists():
            for child in scan_quoted_includes(header_path):
                if not is_desktop_only_header(child) and child not in reachable:
                    queue.append(child)
    return reachable


def fetch_from_chocolate(name: str) -> bytes:
    last_404: urllib.error.HTTPError | None = None
    for base in CHOCOLATE_DOOM_BASES:
        url = f"{base}/{name}"
        try:
            return fetch_bytes(url)
        except urllib.error.HTTPError as exc:
            if exc.code == 404:
                last_404 = exc
                continue
            raise
    if last_404 is not None:
        raise last_404
    raise urllib.error.URLError(f"Could not fetch {name} from Chocolate Doom fallback")


def fetch_missing_reachable_headers_from_chocolate(imported: list[str], force: bool) -> None:
    for _ in range(20):
        missing = sorted(
            name for name in collect_reachable_local_headers()
            if not is_desktop_only_header(name) and not (TARGET_DIR / name).exists()
        )
        if not missing:
            return
        for name in missing:
            print(f"[FALLBACK] {name} <- Chocolate Doom")
            try:
                data = fetch_from_chocolate(name)
            except urllib.error.HTTPError as exc:
                if exc.code == 404:
                    raise RuntimeError(
                        f"Missing reachable required header {name}; not found in ozkl/doomgeneric or Chocolate Doom fallback"
                    ) from exc
                raise
            path = TARGET_DIR / name
            if path.exists() and not force:
                print(f"[CACHE] {name}")
            else:
                path.write_bytes(data)
                imported.append(name)
    unresolved = sorted(
        name for name in collect_reachable_local_headers()
        if not is_desktop_only_header(name) and not (TARGET_DIR / name).exists()
    )
    if unresolved:
        raise RuntimeError(f"Unresolved reachable headers after fallback import: {', '.join(unresolved)}")


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
    text = re.sub(r"#ifndef DOOMGENERIC_RESX\s*\n#define DOOMGENERIC_RESX\s+\d+", "#ifndef DOOMGENERIC_RESX\n#define DOOMGENERIC_RESX 320", text, count=1)
    text = re.sub(r"#ifndef DOOMGENERIC_RESY\s*\n#define DOOMGENERIC_RESY\s+\d+", "#ifndef DOOMGENERIC_RESY\n#define DOOMGENERIC_RESY 200", text, count=1)
    path.write_text(text, encoding="utf-8")


def patch_config_h(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    text, count = re.subn(r'#define\s+FILES_DIR\s+"[^"]*"', '#define FILES_DIR "/littlefs"', text, count=1)
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
        text = text.replace(include_anchor, include_anchor + "\n#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)\n#include <esp_heap_caps.h>\n#endif\n", 1)
    old = "        zonemem = malloc(*size);"
    new = "#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)\n        zonemem = heap_caps_malloc(*size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);\n        if (zonemem == NULL)\n        {\n            zonemem = malloc(*size);\n        }\n#else\n        zonemem = malloc(*size);\n#endif"
    if "heap_caps_malloc(*size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)" not in text:
        if old not in text:
            raise RuntimeError("Could not find zone malloc site in i_system.c")
        text = text.replace(old, new, 1)
    path.write_text(text, encoding="utf-8")


def patch_doomgeneric_c(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    include_anchor = "#include <stdio.h>\n"
    if "#include <esp_heap_caps.h>" not in text:
        text = text.replace(include_anchor, include_anchor + "\n#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)\n#include <esp_heap_caps.h>\n#endif\n", 1)
    old = "\tDG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);"
    new = "#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)\n\tDG_ScreenBuffer = heap_caps_malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4,\n\t                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);\n\tif (DG_ScreenBuffer == NULL)\n\t{\n\t\tDG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);\n\t}\n#else\n\tDG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);\n#endif"
    if "heap_caps_malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4" not in text:
        if old not in text:
            raise RuntimeError("Could not find framebuffer malloc site in doomgeneric.c")
        text = text.replace(old, new, 1)
    path.write_text(text, encoding="utf-8")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if new in text:
        return text
    if old not in text:
        raise RuntimeError(f"Could not patch {label}: {old}")
    return text.replace(old, new, 1)


def patch_r_plane_c(path: Path) -> None:
    text = ensure_esp_attr_include(path.read_text(encoding="utf-8"), "#include <stdlib.h>\n")
    repl = {
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
    for old, new in repl.items():
        text = replace_once(text, old, new, "r_plane.c")
    path.write_text(text, encoding="utf-8")


def patch_r_bsp_c(path: Path) -> None:
    text = ensure_esp_attr_include(path.read_text(encoding="utf-8"), "#include \"doomdef.h\"\n")
    text = replace_once(text, "drawseg_t\tdrawsegs[MAXDRAWSEGS];", "EXT_RAM_BSS_ATTR drawseg_t\tdrawsegs[MAXDRAWSEGS];", "r_bsp.c drawsegs")
    text = replace_once(text, "cliprange_t\tsolidsegs[MAXSEGS];", "EXT_RAM_BSS_ATTR cliprange_t\tsolidsegs[MAXSEGS];", "r_bsp.c solidsegs")
    path.write_text(text, encoding="utf-8")


def patch_r_things_c(path: Path) -> None:
    text = ensure_esp_attr_include(path.read_text(encoding="utf-8"), "#include <stdlib.h>\n")
    text = replace_once(text, "short\t\tnegonearray[SCREENWIDTH];", "EXT_RAM_BSS_ATTR short\t\tnegonearray[SCREENWIDTH];", "r_things.c negonearray")
    text = replace_once(text, "short\t\tscreenheightarray[SCREENWIDTH];", "EXT_RAM_BSS_ATTR short\t\tscreenheightarray[SCREENWIDTH];", "r_things.c screenheightarray")
    text = replace_once(text, "spriteframe_t\tsprtemp[29];", "EXT_RAM_BSS_ATTR spriteframe_t\tsprtemp[29];", "r_things.c sprtemp")
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
        "Fallback headers may be imported from https://github.com/chocolate-doom/chocolate-doom",
        "Source selection: all ozkl headers + selected core C files + missing reachable Chocolate Doom headers.",
        "Desktop-only SDL/SDL2 dependencies are intentionally ignored.",
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
    lines.extend(f"- {name}" for name in sorted(set(imported)))
    (TARGET_DIR / "UPSTREAM_IMPORT_MANIFEST.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    print("[ESP32Toy Doom] Listing upstream DoomGeneric files...")
    try:
        upstream_files = list_upstream_files()
    except (urllib.error.URLError, RuntimeError) as exc:
        print(f"[ERROR] Could not list upstream files: {exc}", file=sys.stderr)
        return 2

    missing_core = sorted(name for name in CORE_C_FILES if name not in upstream_files and not (TARGET_DIR / name).exists())
    if missing_core:
        print(f"[ERROR] Upstream file list is missing required C files: {', '.join(missing_core)}", file=sys.stderr)
        return 5

    imported: list[str] = []
    for name in sorted(upstream_files.keys()):
        if (TARGET_DIR / name).exists() and not args.force:
            print(f"[CACHE] {name}")
            continue
        print(f"[FETCH] {name}")
        try:
            data = fetch_bytes(upstream_files[name])
        except urllib.error.URLError as exc:
            print(f"[ERROR] Failed to download {name}: {exc}", file=sys.stderr)
            return 3
        (TARGET_DIR / name).write_bytes(data)
        imported.append(name)

    try:
        fetch_missing_reachable_headers_from_chocolate(imported, args.force)
        apply_esp32toy_patches()
    except RuntimeError as exc:
        print(f"[ERROR] Patch/import stage failed: {exc}", file=sys.stderr)
        return 4

    all_local_core = sorted(name for name in (CORE_C_FILES | {p.name for p in TARGET_DIR.glob('*.h')}) if (TARGET_DIR / name).exists())
    write_manifest(all_local_core)
    print("")
    print("[DONE] Doom core is ready in:")
    print(f"       {TARGET_DIR}")
    print("[NEXT] Open ESP32Toy_DoomGeneric_Phase3.ino in Arduino IDE and compile.")
    if not args.force:
        print("[INFO] Existing files were reused. Use --force to redownload everything.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
