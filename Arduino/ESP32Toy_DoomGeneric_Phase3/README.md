# ESP32Toy DoomGeneric Phase 3

This folder is the active **Doom Runtime** integration path for ESP32Toy.
It is **not** intended to replace LiquidOS. LiquidOS remains the main system;
this Doom build is a heavy runtime that will later be launched from the main
system and can reboot back to it.

Target hardware:
- ESP32-S3 N16R8
- ST7735 1.8 inch display, 160x128 landscape
- Dual joysticks and A/B buttons already validated in `ESP32Toy_DoomRaycaster_Phase2_DualStick`
- RGB LEDs + vibration motor
- **No SD card slot required**: the IWAD is loaded from board flash via LittleFS

## Current implementation status

Tracked in this folder now:
- `Phase3_entry.ino`
- `doomgeneric_esp32toy.h`
- `doomgeneric_esp32toy.cpp`
- `fetch_doomgeneric_sources.py`
- `partitions.csv`
- `data/PUT_DOOM1_WAD_HERE.txt`

The ESP32Toy platform bridge already implements the DoomGeneric callbacks:
- `DG_Init`
- `DG_DrawFrame`
- `DG_SleepMs`
- `DG_GetTicksMs`
- `DG_GetKey`
- `DG_SetWindowTitle`

It also already provides:
- Classic internal Doom framebuffer path kept at **320x200**
- ESP32Toy presentation downsample from **320x200 -> 160x120**
- 160x120 Doom image letterboxed inside the 160x128 ST7735 panel with 4 px top/bottom bars
- Digital Doom key event queue
- Joystick calibration at boot
- RGB muzzle flash and vibration pulse on fire
- Input processing before each Doom engine tick for lower control latency
- PSRAM detection before Doom startup
- LittleFS mounting from onboard flash
- `/doom1.wad` existence check before Doom starts
- On-screen boot errors for missing PSRAM or missing WAD file
- Runtime starts Doom with `-nosound` because this hardware path currently has no audio output

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

## Board-flash IWAD path

The current boot sketch starts DoomGeneric with:

```cpp
-iwad /littlefs/doom1.wad -nosound
```

Arduino-ESP32 mounts LittleFS at `/littlefs`, while files uploaded into the LittleFS image appear inside the filesystem root. Therefore:

- Local file to place in the sketch data folder: `data/doom1.wad`
- Filesystem-visible path inside LittleFS: `/doom1.wad`
- Runtime DoomGeneric POSIX path: `/littlefs/doom1.wad`

The repo now includes:

```text
data/PUT_DOOM1_WAD_HERE.txt
```

as a placeholder so the upload location is unambiguous.

## Startup gate screens

Doom is only started when both conditions are true:

1. **PSRAM is detected** through Arduino-ESP32's PSRAM API.
2. **`doom1.wad` exists** in LittleFS.

If PSRAM is not available, the screen shows:

- `PSRAM NOT FOUND`
- `ENABLE PSRAM IN IDE`
- `DOOM BOOT IS BLOCKED`

If the WAD file is missing, the screen shows:

- `BOARD FLASH READY`
- `MISSING: doom1.wad`
- `UPLOAD TO LITTLEFS /`
- `EXPECTED: /doom1.wad`

If both are present, the screen shows:

- `PSRAM + FLASH OK`
- `WAD OK: ... KB`
- `STARTING DOOM...`

## Flash partition layout

`partitions.csv` is placed directly beside the Arduino sketch so the Arduino build system can pick it up as a custom partition table.

Current layout for the 16 MB flash board:

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x580000,
app1,     app,  ota_1,           ,0x580000,
spiffs,   data, spiffs,          ,0x4E0000,
```

The final `spiffs`-labeled data partition is used by Arduino-ESP32's LittleFS wrapper. It gives roughly **4.875 MiB** of board-flash filesystem storage for `doom1.wad` and related files.

This partitioning is still considered **provisional** until the LiquidOS main-system binary size, Doom Runtime binary size, and future voice-model storage plan are measured.

## Upstream DoomGeneric core import

The platform bridge is committed directly in this repo. The upstream DoomGeneric C/H engine files are still imported using:

```bash
python fetch_doomgeneric_sources.py
```

That script downloads the official upstream DoomGeneric core C/H files into this sketch folder, skips desktop platform backends, and applies ESP32Toy-specific patches:

- `doomgeneric.h` -> internal Doom framebuffer kept at classic `320x200`
- `config.h` -> `FILES_DIR` changed to `/littlefs`
- `i_system.c` -> Doom's large zone-memory allocation prefers PSRAM via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` and falls back to `malloc()` only if needed
- `doomgeneric.c` -> Doom's RGBA framebuffer allocation prefers PSRAM via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` and falls back to `malloc()` only if needed

The bridge is already written against the official DoomGeneric interface in `doomgeneric.h`, where DoomGeneric exposes the framebuffer pointer, create/tick entry points, and the platform callback contract.

## What the user will do later

Once the upstream core compiles, the local user-side steps are:

1. Put a legally obtained `doom1.wad` inside the sketch data folder as:
   - `data/doom1.wad`
2. Upload the LittleFS filesystem image to the board using an Arduino-ESP32 LittleFS upload workflow.
3. Flash the sketch.
4. Boot the board. If PSRAM and WAD are present, it proceeds to Doom startup; otherwise it stays on the matching diagnostic screen.

## Next compile target

After the upstream core files are present in this folder:
1. Run `python fetch_doomgeneric_sources.py`.
2. Open `Phase3_entry.ino` in Arduino IDE.
3. Compile for the ESP32-S3 board target.
4. Fix any Arduino/ESP32-specific compile issues from the imported upstream C core.
5. Confirm LittleFS + `/littlefs/doom1.wad` boot path on hardware.

The next engineering pass should focus on getting the upstream core to compile under Arduino-ESP32 cleanly, then verifying the DoomGeneric file I/O path reaches the board-flash IWAD.
