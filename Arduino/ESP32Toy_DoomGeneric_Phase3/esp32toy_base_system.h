#pragma once

#include <Arduino.h>

// Base-system launcher actions returned by ESP32Toy_OSLauncherTick().
enum ESP32Toy_OSAction : uint8_t {
  ESP32TOY_OS_ACTION_NONE = 0,
  ESP32TOY_OS_ACTION_START_WATER = 1,
  ESP32TOY_OS_ACTION_START_DOOM = 2,
};

void ESP32Toy_OSInitLauncher(bool doomReady);
ESP32Toy_OSAction ESP32Toy_OSLauncherTick(bool doomReady);
void ESP32Toy_OSRedrawLauncher(bool doomReady);

// Water Lab V2 is a standalone LiquidOS-style module.  The old implementation
// remains in esp32toy_base_system.cpp, but the application entry is redirected
// here so Doom/base launcher code does not need to be touched.
bool ESP32Toy_WaterLabV2Tick(void);
#define ESP32Toy_OSWaterTick ESP32Toy_WaterLabV2Tick
