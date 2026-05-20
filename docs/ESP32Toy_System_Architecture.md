# ESP32Toy System Architecture

## Decision

ESP32Toy will use a **hybrid architecture**:

1. **Main System firmware** remains the normal boot target.
   - Home / launcher UI
   - Water simulation app
   - Settings and device states
   - Future voice recognition service
   - Future PC communication service

2. **Heavy Runtime firmware** is reserved for applications that are too large or too lifecycle-heavy to behave like normal in-process apps.
   - Current target: Doom Runtime
   - Entered from the Main System UI
   - Returns to Main System by switching boot target and rebooting

This is **not** a pure dual-system product design. The user-facing device is still one system. Doom is presented as an app in the launcher, but technically runs in a dedicated runtime firmware slot.

---

## Why this architecture

### Why not make Doom a normal in-process app

The current DoomGeneric integration behaves like a standalone program rather than a lightweight widget:

- It owns a large framebuffer and a large gameplay memory zone.
- It initializes substantial global engine state.
- Its startup path is closer to `program start` than `open app`.
- A clean unload / return-to-launcher path would require deeper changes to DoomGeneric lifecycle and memory ownership.

That approach is possible, but it is not the best first architecture for ESP32Toy.

### Why not make the whole device a pure dual-system console

ESP32Toy is not only a Doom device. The product direction already includes:

- Water simulation
- A persistent launcher / settings experience
- Future voice recognition
- Future PC communication

Those features belong to one **Main System**, not to a game-specific firmware. A pure two-OS device would push the product in the wrong direction.

### Chosen compromise

- Lightweight and system-level features stay inside Main System.
- Heavy self-contained runtimes may use their own firmware slot.
- The launcher still makes the whole device feel unified.

---

## User-facing flow

### Boot

The board normally boots into:

```text
ESP32Toy Main System
  - Water
  - Doom
  - Settings
  - Future Voice / Link features
```

### Enter Doom

From Main System:

1. User selects `Doom`.
2. Main System requests the Doom Runtime partition as the next boot target.
3. Board reboots.
4. Doom Runtime starts.

### Return from Doom

Inside Doom Runtime:

1. User uses a deliberate exit gesture, recommended initial mapping:
   - Long-press B for 2 seconds, or
   - Hold both joystick switches for 2 seconds.
2. Doom Runtime requests the Main System partition as next boot target.
3. Board reboots.
4. Main System menu returns.

The reboot is intentional. It gives Doom a clean memory envelope and guarantees Main System comes back in a known-good state.

---

## Firmware roles

### Main System firmware

Recommended repository role:

```text
Arduino/ESP32Toy_LiquidOS_*/
```

Responsibilities:

- Device boot UX
- Main menu / launcher
- Water simulation app
- Settings app
- LED and motor policy outside heavy runtimes
- Future microphone / speech pipeline
- Future PC communication protocol
- Runtime-launch command for Doom

Current repository history already points in this direction: a LiquidOS build exists with a menu, Water entry, Settings entry, and a Doom placeholder entry. The architecture formalizes that direction instead of replacing it.

### Doom Runtime firmware

Recommended repository role:

```text
Arduino/ESP32Toy_DoomGeneric_Phase3/
```

Responsibilities:

- DoomGeneric engine
- Doom-specific input mapping
- ST7735 framebuffer output
- Doom haptics / LED reactions
- Board-flash IWAD loading
- Return-to-Main-System command

The current Doom Phase 3 work should be treated as the beginning of this Runtime firmware, not as the replacement of LiquidOS.

---

## Boot switching model

The firmware-switching mechanism should use the ESP OTA app-slot machinery:

```text
app0 / ota_0  -> Main System
app1 / ota_1  -> Doom Runtime
```

Switching flow conceptually becomes:

```text
Find target app partition
esp_ota_set_boot_partition(target)
esp_restart()
```

Implementation should be wrapped behind a tiny project-local Boot Manager API so app code never directly performs low-level partition logic.

Recommended interface:

```cpp
namespace Esp32ToyBoot {
  enum class Target {
    MainSystem,
    DoomRuntime,
  };

  bool RequestBoot(Target target);
  Target GetRunningTarget();
}
```

Future code can then do:

```cpp
Esp32ToyBoot::RequestBoot(Esp32ToyBoot::Target::DoomRuntime);
```

or:

```cpp
Esp32ToyBoot::RequestBoot(Esp32ToyBoot::Target::MainSystem);
```

---

## Partitioning strategy

### Principle

Do **not** permanently freeze the final partition table before the Main System, Doom Runtime, and future voice-resource needs are measured.

The current interim 16 MB layout is still useful as a first integration target:

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x580000,
app1,     app,  ota_1,           ,0x580000,
spiffs,   data, spiffs,          ,0x4E0000,
```

Interpretation:

- `app0`: Main System firmware slot
- `app1`: Doom Runtime firmware slot
- `spiffs`-labeled data partition: mounted as LittleFS for shared files

This gives roughly:

- 5.5 MiB Main System slot
- 5.5 MiB Doom Runtime slot
- 4.875 MiB shared flash filesystem

### Shared files

Shared flash storage should hold:

```text
/doom1.wad
/system/settings.json      (optional later)
/runtime/state.json         (optional later)
```

Voice models should **not** be committed to this shared FS layout yet. The exact speech stack and model footprint should be chosen first, then the final partition plan should be revisited.

---

## Resource ownership

### Main System owns

- General UI state
- Water simulation state
- Global preferences
- Voice settings
- PC link settings
- Boot into Runtime actions

### Doom Runtime owns

- Doom engine state
- Doom control mapping
- Doom framebuffer
- Doom-only runtime haptics
- Return-to-Main action

### Shared storage owns

- IWAD file
- Optional cross-runtime metadata
- Future neutral resources if needed

---

## Future voice recognition and PC communication

These should belong to the **Main System**, not Doom Runtime.

### Voice recognition

Reasoning:

- It is a system-level interaction layer.
- It may be used from menus, Water, and future apps.
- It may need dedicated assets or model storage, which should be planned as part of Main System evolution.

### PC communication

Reasoning:

- It is a platform service, not a Doom-specific feature.
- It may later expose state, telemetry, or commands to a desktop companion.
- It should remain available during normal Main System use.

During Doom Runtime, these services may be intentionally suspended in the first implementation. They can be reconsidered later only if a strong use case appears.

---

## Recommended development order

### Phase A — Freeze architecture

- Treat LiquidOS as Main System.
- Treat Doom Phase 3 as Doom Runtime.
- Stop viewing Doom as a Main-System replacement.

### Phase B — Finish Doom Runtime standalone

- Compile imported DoomGeneric core.
- Confirm `/doom1.wad` from LittleFS works.
- Confirm gameplay, controls, display, and memory stability.

### Phase C — Add Boot Manager

- Add target-partition switching helpers.
- Main System menu launches Doom Runtime.
- Doom Runtime exits back to Main System.

### Phase D — Revisit flash sizing

- Measure actual Main System binary size.
- Measure Doom Runtime binary size.
- Decide voice model/storage approach.
- Finalize partition CSV from measured needs, not guesses.

### Phase E — Voice + PC communication

- Add microphone / speech stack to Main System.
- Add desktop communication protocol to Main System.
- Keep Doom Runtime isolated unless a specific cross-runtime feature is needed.

---

## Short decision summary

ESP32Toy should become:

> **A Main System device with internal apps and optional heavy rebootable runtimes.**

For now:

- Water stays in Main System.
- Doom stays as a heavy Runtime launched from Main System.
- Future voice recognition and PC communication stay in Main System.
- Partitioning remains provisional until measured binary and model sizes are known.
