#include <Arduino.h>
#include "doomgeneric_esp32toy.h"

static bool doomStarted = false;

void setup() {
  ESP32Toy_DoomPlatformInitHardware();

  if (!ESP32Toy_DoomPlatformReadyToStart()) {
    doomStarted = false;
    return;
  }

  static char arg0[] = "esp32toy";
  static char arg1[] = "-iwad";
  static char arg2[] = "/littlefs/doom1.wad";
  static char *argv[] = { arg0, arg1, arg2 };

  doomgeneric_Create(3, argv);
  doomStarted = true;
}

void loop() {
  if (!doomStarted) {
    ESP32Toy_DoomPlatformIdle();
    delay(30);
    return;
  }

  ESP32Toy_DoomPlatformBeforeTick();
  doomgeneric_Tick();
  ESP32Toy_DoomPlatformAfterTick();
}
