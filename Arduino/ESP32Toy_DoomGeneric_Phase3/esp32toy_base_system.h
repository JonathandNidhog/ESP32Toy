#pragma once

#include <Arduino.h>

enum ESP32Toy_OSAction : uint8_t {
  ESP32TOY_OS_ACTION_NONE = 0,
  ESP32TOY_OS_ACTION_START_WATER = 1,
  ESP32TOY_OS_ACTION_START_DOOM = 2,
};

void ESP32Toy_OSInitLauncher(bool doomReady);
ESP32Toy_OSAction ESP32Toy_OSLauncherTick(bool doomReady);
void ESP32Toy_OSRedrawLauncher(bool doomReady);
bool ESP32Toy_OSWaterTick(void);
bool ESP32Toy_WaterLabV2Tick(void);
