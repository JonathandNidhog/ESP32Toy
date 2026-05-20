#pragma once

#include <Arduino.h>

// DoomGeneric entry points are provided by the upstream DoomGeneric engine
// source files that will live beside this platform bridge in this folder.
#ifdef __cplusplus
extern "C" {
#endif

void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick(void);

// Platform callbacks required by DoomGeneric.
void DG_Init(void);
void DG_DrawFrame(void);
void DG_SleepMs(uint32_t ms);
uint32_t DG_GetTicksMs(void);
int DG_GetKey(int *pressed, unsigned char *key);
void DG_SetWindowTitle(const char *title);

#ifdef __cplusplus
}
#endif

// Arduino-facing hardware lifecycle.
void ESP32Toy_DoomPlatformInitHardware(void);
void ESP32Toy_DoomPlatformBeforeTick(void);
void ESP32Toy_DoomPlatformAfterTick(void);
void ESP32Toy_DoomPlatformIdle(void);

// Board-flash / memory boot status.
bool ESP32Toy_DoomPlatformHasIWAD(void);
bool ESP32Toy_DoomPlatformHasPSRAM(void);
bool ESP32Toy_DoomPlatformReadyToStart(void);
const char *ESP32Toy_DoomPlatformIWADPath(void);
