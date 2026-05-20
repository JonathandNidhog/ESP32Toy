# ESP32Toy DoomGeneric Phase 3

This folder is the active **real DOOM** integration path for ESP32Toy.

Target hardware:
- ESP32-S3 N16R8
- ST7735 1.8 inch display, 160x128 landscape
- Dual joysticks and A/B buttons already validated in `ESP32Toy_DoomRaycaster_Phase2_DualStick`
- RGB LEDs + vibration motor

## Current implementation status

Tracked in this folder now:
- `Phase3_entry.ino`
- `doomgeneric_esp32toy.h`
- `doomgeneric_esp32toy.cpp`
- `fetch_doomgeneric_sources.py`

The ESP32Toy platform bridge already implements the DoomGeneric callbacks:
- `DG_Init`
- `DG_DrawFrame`
- `DG_SleepMs`
- `DG_GetTicksMs`
- `DG_GetKey`
- `DG_SetWindowTitle`

It also already provides:
- ST7735 framebuffer conversion from DoomGeneric's screen buffer to RGB565 scanlines
- Digital Doom key event queue
- Joystick calibration at boot
- RGB muzzle flash and vibration pulse on fire
- Input processing before each Doom engine tick for lower control latency

## Final control mapping currently wired for real Doom

These match the latest validated hardware direction fixes from the raycaster prototype:

- **Old / right joystick**
  - Up / down: move forward / backward
  - Left / right: strafe left / right
  - Press: run modifier (`RSHIFT`)

- **New / left joystick**
  - Left / right: turn camera left / right
  - Up / down: sampled and calibrated, currently reserved because vanilla Doom has no free vertical-look keyboard axis
  - Press: automap toggle (`TAB`)

- **Buttons**
  - A: fire
  - B: use / open door

## Upstream DoomGeneric core import

The platform bridge is committed directly in this repo. The upstream DoomGeneric C/H engine files are still imported using:

```bash
python fetch_doomgeneric_sources.py
```

That script downloads the official upstream DoomGeneric core C/H files into this sketch folder, skips desktop platform backends, and patches `doomgeneric.h` to use `160x128`.

The bridge is already written against the official DoomGeneric interface in `doomgeneric.h`, where DoomGeneric exposes the framebuffer pointer, create/tick entry points, and the platform callback contract. See the upstream `doomgeneric.h` interface for those symbols.

## Next compile target

After the upstream core files are present in this folder:
1. Open `Phase3_entry.ino` in Arduino IDE.
2. Compile for the ESP32-S3 board target.
3. Fix any Arduino/ESP32-specific compile issues from the imported upstream C core.
4. Wire `doom1.wad` loading from on-device storage.

The next engineering pass should focus on getting the upstream core to compile under Arduino-ESP32 and then mounting / locating the IWAD file.
