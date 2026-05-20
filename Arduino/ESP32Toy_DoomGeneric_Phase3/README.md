# ESP32Toy DoomGeneric Phase 3

This folder is the start of the **real DOOM** integration phase for ESP32Toy.

Target hardware:
- ESP32-S3 N16R8
- ST7735 1.8 inch display, 160x128 landscape
- Dual joysticks and A/B buttons already validated in `ESP32Toy_DoomRaycaster_Phase2_DualStick`
- RGB LEDs + vibration motor

Integration direction:
- Use `doomgeneric` as the real Doom engine base.
- Implement the small platform layer expected by DoomGeneric:
  - `DG_Init`
  - `DG_DrawFrame`
  - `DG_SleepMs`
  - `DG_GetTicksMs`
  - `DG_GetKey`
- Convert DoomGeneric's framebuffer to the ST7735 display.
- Map the validated dual-stick controls to Doom keyboard events.
- Load `doom1.wad` from on-device storage in a later step.

Current files in this folder:
- `Phase3_entry.ino`
- `doomgeneric_esp32toy.h`
- `doomgeneric_esp32toy.cpp`
- `fetch_doomgeneric_sources.py`

## Next local step

Run this script once from this folder:

```bash
python fetch_doomgeneric_sources.py
```

It downloads the official upstream DoomGeneric core C/H source files into this sketch folder, skips desktop platform backends, and patches `doomgeneric.h` to use `160x128`.

After that, open `Phase3_entry.ino` in Arduino IDE and try a first compile. The next integration pass will fix any Arduino/ESP32 compile differences and wire WAD loading.

## Why there is a fetch script

The DoomGeneric engine contains many source files. The ESP32Toy-specific platform layer is tracked directly in this repo, while the upstream engine core is imported on demand from the official DoomGeneric repository.
