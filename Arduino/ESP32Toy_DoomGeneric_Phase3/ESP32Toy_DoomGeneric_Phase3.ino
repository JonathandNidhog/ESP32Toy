#include <Arduino.h>

#if !__has_include("doomgeneric.h")
#error "DoomGeneric core is missing. Run: python fetch_doomgeneric_sources.py inside Arduino/ESP32Toy_DoomGeneric_Phase3, then reopen/compile ESP32Toy_DoomGeneric_Phase3.ino."
#endif

#include "doomgeneric.h"
#include "doomgeneric_esp32toy.h"
#include "esp32toy_base_system.h"

enum ESP32Toy_RuntimeMode : uint8_t {
  MODE_LAUNCHER = 0,
  MODE_WATER = 1,
  MODE_DOOM = 2,
};

static ESP32Toy_RuntimeMode runtimeMode = MODE_LAUNCHER;
static bool doomStarted = false;
static bool baseSystemStarted = false;

static void startDoomRuntime() {
  if (doomStarted) {
    runtimeMode = MODE_DOOM;
    return;
  }

  static char arg0[] = "esp32toy";
  static char arg1[] = "-iwad";
  static char arg2[] = "/littlefs/doom1.wad";
  static char arg3[] = "-nosound";
  static char *argv[] = { arg0, arg1, arg2, arg3 };

  doomgeneric_Create(4, argv);
  doomStarted = true;
  runtimeMode = MODE_DOOM;
}

void setup() {
  ESP32Toy_DoomPlatformInitHardware();
  ESP32Toy_OSInitLauncher(ESP32Toy_DoomPlatformReadyToStart());
  baseSystemStarted = true;
  runtimeMode = MODE_LAUNCHER;
}

void loop() {
  if (!baseSystemStarted) {
    ESP32Toy_DoomPlatformIdle();
    delay(30);
    return;
  }

  switch (runtimeMode) {
    case MODE_LAUNCHER: {
      const ESP32Toy_OSAction action = ESP32Toy_OSLauncherTick(ESP32Toy_DoomPlatformReadyToStart());
      if (action == ESP32TOY_OS_ACTION_START_WATER) {
        runtimeMode = MODE_WATER;
      } else if (action == ESP32TOY_OS_ACTION_START_DOOM && ESP32Toy_DoomPlatformReadyToStart()) {
        startDoomRuntime();
      }
      delay(8);
      break;
    }

    case MODE_WATER: {
      const bool backToLauncher = ESP32Toy_WaterLabV2Tick();
      if (backToLauncher) {
        runtimeMode = MODE_LAUNCHER;
        ESP32Toy_OSRedrawLauncher(ESP32Toy_DoomPlatformReadyToStart());
      }
      break;
    }

    case MODE_DOOM: {
      ESP32Toy_DoomPlatformBeforeTick();
      doomgeneric_Tick();
      ESP32Toy_DoomPlatformAfterTick();
      break;
    }
  }
}
