#include <stdio.h>

#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

#include "m_argv.h"

#include "doomgeneric.h"

pixel_t* DG_ScreenBuffer = NULL;

void M_FindResponseFile(void);
void D_DoomMain (void);


void doomgeneric_Create(int argc, char **argv)
{
	// save arguments
    myargc = argc;
    myargv = argv;

	M_FindResponseFile();

#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
	DG_ScreenBuffer = heap_caps_malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4,
	                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (DG_ScreenBuffer == NULL)
	{
		DG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);
	}
#else
	DG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);
#endif

	DG_Init();

	D_DoomMain ();
}

