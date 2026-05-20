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

The next commit in this phase will add the ESP32Toy DoomGeneric platform bridge and boot sketch, then vendor or attach the DoomGeneric engine source needed for compilation.
