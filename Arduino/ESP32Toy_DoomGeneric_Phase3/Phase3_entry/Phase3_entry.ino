#include <Arduino.h>
#include "doomgeneric_esp32toy.h"

void setup() {
  ESP32Toy_DoomPlatformInitHardware();
  static char arg0[] = "esp32toy";
  static char *argv[] = { arg0 };
  doomgeneric_Create(1, argv);
}

void loop() {
  doomgeneric_Tick();
  ESP32Toy_DoomPlatformAfterTick();
}
