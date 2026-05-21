#!/usr/bin/env python3
"""
Patch imported DoomGeneric renderer buffers so they do not live in internal DRAM .bss.

Why this exists:
Arduino-ESP32 may not place EXT_RAM_BSS_ATTR globals into PSRAM unless the exact
board/core configuration enables external BSS placement.  If that option is not
active, the linker still fails with:

    section `.dram0.bss' will not fit in region `dram0_0_seg'

This script uses a harder and more portable approach for the biggest Doom
renderer buffers:

- r_plane.c: visplanes/openings/clipping/span/cache arrays become pointers
             allocated at runtime from PSRAM first, then normal calloc fallback.
- r_bsp.c:   drawsegs/solidsegs become pointers allocated at runtime.

Run after fetch_doomgeneric_sources.py and before Arduino compile:

    python patch_doom_runtime_memory.py
"""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def read(name: str) -> str:
    return (ROOT / name).read_text(encoding="utf-8")


def write(name: str, text: str) -> None:
    (ROOT / name).write_text(text, encoding="utf-8")


def replace_regex(text: str, pattern: str, repl: str, label: str) -> str:
    new, count = re.subn(pattern, repl, text, count=1, flags=re.MULTILINE)
    if count == 0 and repl not in text:
        raise RuntimeError(f"Could not patch {label}")
    return new


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if new in text:
        return text
    if old not in text:
        raise RuntimeError(f"Could not patch {label}")
    return text.replace(old, new, 1)


def ensure_heap_caps_include(text: str, anchor: str, label: str) -> str:
    if "#include <esp_heap_caps.h>" in text:
        return text
    if anchor not in text:
        raise RuntimeError(f"Could not find include anchor for {label}")
    return text.replace(
        anchor,
        anchor
        + "\n#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)\n"
        + "#include <esp_heap_caps.h>\n"
        + "#endif\n",
        1,
    )


def patch_r_plane() -> None:
    text = read("r_plane.c")
    text = ensure_heap_caps_include(text, "#include <stdlib.h>\n", "r_plane.c")

    # Convert large renderer .bss arrays to pointers.
    patterns = [
        (r"(?:EXT_RAM_BSS_ATTR\s+)?visplane_t\s+visplanes\[MAXVISPLANES\];", "visplane_t*\t\t\tvisplanes;", "r_plane visplanes"),
        (r"(?:EXT_RAM_BSS_ATTR\s+)?short\s+openings\[MAXOPENINGS\];", "short*\t\t\topenings;", "r_plane openings"),
        (r"(?:EXT_RAM_BSS_ATTR\s+)?short\s+floorclip\[SCREENWIDTH\];", "short*\t\t\tfloorclip;", "r_plane floorclip"),
        (r"(?:EXT_RAM_BSS_ATTR\s+)?short\s+ceilingclip\[SCREENWIDTH\];", "short*\t\t\tceilingclip;", "r_plane ceilingclip"),
        (r"(?:EXT_RAM_BSS_ATTR\s+)?int\s+spanstart\[SCREENHEIGHT\];", "int*\t\t\tspanstart;", "r_plane spanstart"),
        (r"(?:EXT_RAM_BSS_ATTR\s+)?int\s+spanstop\[SCREENHEIGHT\];", "int*\t\t\tspanstop;", "r_plane spanstop"),
        (r"(?:EXT_RAM_BSS_ATTR\s+)?fixed_t\s+yslope\[SCREENHEIGHT\];", "fixed_t*\t\t\tyslope;", "r_plane yslope"),
        (r"(?:EXT_RAM_BSS_ATTR\s+)?fixed_t\s+distscale\[SCREENWIDTH\];", "fixed_t*\t\t\tdistscale;", "r_plane distscale"),
        (r"(?:EXT_RAM_BSS_ATTR\s+)?fixed_t\s+cachedheight\[SCREENHEIGHT\];", "fixed_t*\t\t\tcachedheight;", "r_plane cachedheight"),
        (r"(?:EXT_RAM_BSS_ATTR\s+)?fixed_t\s+cacheddistance\[SCREENHEIGHT\];", "fixed_t*\t\t\tcacheddistance;", "r_plane cacheddistance"),
        (r"(?:EXT_RAM_BSS_ATTR\s+)?fixed_t\s+cachedxstep\[SCREENHEIGHT\];", "fixed_t*\t\t\tcachedxstep;", "r_plane cachedxstep"),
        (r"(?:EXT_RAM_BSS_ATTR\s+)?fixed_t\s+cachedystep\[SCREENHEIGHT\];", "fixed_t*\t\t\tcachedystep;", "r_plane cachedystep"),
    ]
    for pattern, repl, label in patterns:
        text = replace_regex(text, pattern, repl, label)

    # sizeof(pointer) would be wrong after conversion.
    text = text.replace(
        "memset (cachedheight, 0, sizeof(cachedheight));",
        "memset (cachedheight, 0, sizeof(*cachedheight) * SCREENHEIGHT);",
    )

    if "ESP32TOY_DOOM_ALLOC_ARRAY" not in text:
        new_init = """void R_InitPlanes (void)
{
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#define ESP32TOY_DOOM_ALLOC_ARRAY(ptr, type, count)                                      \\
    do                                                                                   \\
    {                                                                                    \\
        if ((ptr) == NULL)                                                               \\
        {                                                                                \\
            (ptr) = (type*) heap_caps_calloc((count), sizeof(type),                      \\
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);       \\
            if ((ptr) == NULL)                                                           \\
            {                                                                            \\
                (ptr) = (type*) calloc((count), sizeof(type));                           \\
            }                                                                            \\
            if ((ptr) == NULL)                                                           \\
            {                                                                            \\
                I_Error("R_InitPlanes: failed to allocate %s", #ptr);                    \\
            }                                                                            \\
        }                                                                                \\
    } while (0)
#else
#define ESP32TOY_DOOM_ALLOC_ARRAY(ptr, type, count)                                      \\
    do                                                                                   \\
    {                                                                                    \\
        if ((ptr) == NULL)                                                               \\
        {                                                                                \\
            (ptr) = (type*) calloc((count), sizeof(type));                               \\
            if ((ptr) == NULL)                                                           \\
            {                                                                            \\
                I_Error("R_InitPlanes: failed to allocate %s", #ptr);                    \\
            }                                                                            \\
        }                                                                                \\
    } while (0)
#endif

    ESP32TOY_DOOM_ALLOC_ARRAY(visplanes, visplane_t, MAXVISPLANES);
    ESP32TOY_DOOM_ALLOC_ARRAY(openings, short, MAXOPENINGS);
    ESP32TOY_DOOM_ALLOC_ARRAY(floorclip, short, SCREENWIDTH);
    ESP32TOY_DOOM_ALLOC_ARRAY(ceilingclip, short, SCREENWIDTH);
    ESP32TOY_DOOM_ALLOC_ARRAY(spanstart, int, SCREENHEIGHT);
    ESP32TOY_DOOM_ALLOC_ARRAY(spanstop, int, SCREENHEIGHT);
    ESP32TOY_DOOM_ALLOC_ARRAY(yslope, fixed_t, SCREENHEIGHT);
    ESP32TOY_DOOM_ALLOC_ARRAY(distscale, fixed_t, SCREENWIDTH);
    ESP32TOY_DOOM_ALLOC_ARRAY(cachedheight, fixed_t, SCREENHEIGHT);
    ESP32TOY_DOOM_ALLOC_ARRAY(cacheddistance, fixed_t, SCREENHEIGHT);
    ESP32TOY_DOOM_ALLOC_ARRAY(cachedxstep, fixed_t, SCREENHEIGHT);
    ESP32TOY_DOOM_ALLOC_ARRAY(cachedystep, fixed_t, SCREENHEIGHT);

#undef ESP32TOY_DOOM_ALLOC_ARRAY
}
"""
        text = replace_once(
            text,
            "void R_InitPlanes (void)\n{\n  // Doh!\n}",
            new_init,
            "r_plane R_InitPlanes runtime allocation",
        )

    write("r_plane.c", text)
    print("[PATCH] r_plane.c -> large renderer arrays now runtime-allocated from PSRAM")


def patch_r_bsp() -> None:
    text = read("r_bsp.c")
    text = ensure_heap_caps_include(text, "#include \"doomdef.h\"\n", "r_bsp.c")

    text = replace_regex(
        text,
        r"(?:EXT_RAM_BSS_ATTR\s+)?drawseg_t\s+drawsegs\[MAXDRAWSEGS\];",
        "drawseg_t*\tdrawsegs;",
        "r_bsp drawsegs pointer",
    )
    text = replace_regex(
        text,
        r"(?:EXT_RAM_BSS_ATTR\s+)?cliprange_t\s+solidsegs\[MAXSEGS\];",
        "cliprange_t*\tsolidsegs;",
        "r_bsp solidsegs pointer",
    )

    if "R_AllocDrawSegs" not in text:
        alloc_draw = """
static void R_AllocDrawSegs(void)
{
    if (drawsegs != NULL)
    {
        return;
    }
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
    drawsegs = (drawseg_t*) heap_caps_calloc(MAXDRAWSEGS, sizeof(drawseg_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (drawsegs == NULL)
    {
        drawsegs = (drawseg_t*) calloc(MAXDRAWSEGS, sizeof(drawseg_t));
    }
#else
    drawsegs = (drawseg_t*) calloc(MAXDRAWSEGS, sizeof(drawseg_t));
#endif
    if (drawsegs == NULL)
    {
        I_Error("R_AllocDrawSegs: failed to allocate drawsegs");
    }
}
"""
        text = text.replace("void\nR_StoreWallRange", alloc_draw + "\nvoid\nR_StoreWallRange", 1)

    text = replace_once(
        text,
        "void R_ClearDrawSegs (void)\n{\n    ds_p = drawsegs;\n}",
        "void R_ClearDrawSegs (void)\n{\n    R_AllocDrawSegs();\n    ds_p = drawsegs;\n}",
        "r_bsp R_ClearDrawSegs allocation call",
    )

    if "R_AllocSolidSegs" not in text:
        alloc_solid = """
static void R_AllocSolidSegs(void)
{
    if (solidsegs != NULL)
    {
        return;
    }
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
    solidsegs = (cliprange_t*) heap_caps_calloc(MAXSEGS, sizeof(cliprange_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (solidsegs == NULL)
    {
        solidsegs = (cliprange_t*) calloc(MAXSEGS, sizeof(cliprange_t));
    }
#else
    solidsegs = (cliprange_t*) calloc(MAXSEGS, sizeof(cliprange_t));
#endif
    if (solidsegs == NULL)
    {
        I_Error("R_AllocSolidSegs: failed to allocate solidsegs");
    }
}
"""
        text = text.replace("//\n// R_ClipSolidWallSegment", alloc_solid + "\n//\n// R_ClipSolidWallSegment", 1)

    text = replace_once(
        text,
        "void R_ClearClipSegs (void)\n{\n    solidsegs[0].first = -0x7fffffff;",
        "void R_ClearClipSegs (void)\n{\n    R_AllocSolidSegs();\n    solidsegs[0].first = -0x7fffffff;",
        "r_bsp R_ClearClipSegs allocation call",
    )

    write("r_bsp.c", text)
    print("[PATCH] r_bsp.c -> drawsegs/solidsegs now runtime-allocated from PSRAM")


def main() -> int:
    required = ["r_plane.c", "r_bsp.c"]
    missing = [name for name in required if not (ROOT / name).exists()]
    if missing:
        print("[ERROR] Missing imported Doom source file(s): " + ", ".join(missing))
        print("        Run fetch_doomgeneric_sources.py first.")
        return 2

    patch_r_plane()
    patch_r_bsp()
    print("[DONE] Doom renderer static DRAM patch applied.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
