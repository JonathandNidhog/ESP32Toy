#pragma once

#include <Arduino.h>

#ifdef __cplusplus
extern "C" {
#endif

void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick(void);
void DG_Init(void);
void DG_DrawFrame(void);
void DG_SleepMs(uint32_t ms);
uint32_t DG_GetTicksMs(void);
int DG_GetKey(int *pressed, unsigned char *key);
void DG_SetWindowTitle(const char *title);

#ifdef __cplusplus
}
#endif

void ESP32Toy_DoomPlatformInitHardware(void);
void ESP32Toy_DoomPlatformAfterTick(void);
