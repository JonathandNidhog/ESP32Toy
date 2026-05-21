#include "esp32toy_base_system.h"

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

// Keep this small base-system layer independent from DoomGeneric internals.
// It uses the same physical hardware pins as the Doom platform bridge.
#define OS_TFT_MOSI 11
#define OS_TFT_SCLK 12
#define OS_TFT_CS   10
#define OS_TFT_DC    9
#define OS_TFT_BL   21
#define OS_TFT_RST  -1

#define OS_RIGHT_JOY_X_PIN 16
#define OS_RIGHT_JOY_Y_PIN  8
#define OS_RIGHT_JOY_SW_PIN 6
#define OS_KEY_A_PIN       15
#define OS_KEY_B_PIN       14
#define OS_LEFT_JOY_X_PIN  17
#define OS_LEFT_JOY_Y_PIN  18
#define OS_LEFT_JOY_SW_PIN 13

#define OS_TFT_ROTATION 3

static const int kOSW = 160;
static const int kOSH = 128;
static const int kWaterCols = 40;
static const int kWaterRows = 32;
static const int kCellW = 4;
static const int kCellH = 4;

static SPIClass osSPI(FSPI);
static Adafruit_ST7735 osTft(&osSPI, OS_TFT_CS, OS_TFT_DC, OS_TFT_RST);

static bool osDisplayReady = false;
static uint8_t selectedItem = 0;
static uint32_t lastNavMs = 0;
static uint32_t lastFrameMs = 0;
static uint32_t warningUntilMs = 0;

static bool aStable = false;
static bool bStable = false;
static bool leftSWStable = false;
static bool aRawPrev = false;
static bool bRawPrev = false;
static bool leftSWRawPrev = false;
static uint32_t aChangedAt = 0;
static uint32_t bChangedAt = 0;
static uint32_t leftSWChangedAt = 0;
static bool aPrev = false;
static bool bPrev = false;
static bool leftSWPrev = false;

static float waterA[kWaterRows][kWaterCols];
static float waterB[kWaterRows][kWaterCols];
static float waterVX = 0.0f;
static float waterVY = 0.0f;
static int waterCX = kWaterCols / 2;
static int waterCY = kWaterRows / 2;

static uint16_t osRGB565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static float osClamp(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static bool osDebouncedPressed(int pin, bool *rawPrev, bool *stable, uint32_t *changedAt) {
  const bool rawPressed = digitalRead(pin) == LOW;
  const uint32_t now = millis();
  if (rawPressed != *rawPrev) {
    *rawPrev = rawPressed;
    *changedAt = now;
  }
  if (now - *changedAt >= 30 && rawPressed != *stable) {
    *stable = rawPressed;
  }
  return *stable;
}

static void osDrawCentered(int y, const char *text, uint16_t color, uint8_t size = 1) {
  osTft.setTextSize(size);
  osTft.setTextColor(color);
  int textWidth = (int)strlen(text) * 6 * size;
  int x = (kOSW - textWidth) / 2;
  if (x < 0) x = 0;
  osTft.setCursor(x, y);
  osTft.print(text);
}

static void osEnsureHardware(void) {
  if (osDisplayReady) return;

  pinMode(OS_TFT_BL, OUTPUT);
  digitalWrite(OS_TFT_BL, HIGH);

  pinMode(OS_RIGHT_JOY_X_PIN, INPUT);
  pinMode(OS_RIGHT_JOY_Y_PIN, INPUT);
  pinMode(OS_LEFT_JOY_X_PIN, INPUT);
  pinMode(OS_LEFT_JOY_Y_PIN, INPUT);
  pinMode(OS_RIGHT_JOY_SW_PIN, INPUT_PULLUP);
  pinMode(OS_LEFT_JOY_SW_PIN, INPUT_PULLUP);
  pinMode(OS_KEY_A_PIN, INPUT_PULLUP);
  pinMode(OS_KEY_B_PIN, INPUT_PULLUP);

  osSPI.begin(OS_TFT_SCLK, -1, OS_TFT_MOSI, OS_TFT_CS);
  osTft.initR(INITR_BLACKTAB);
  osTft.setRotation(OS_TFT_ROTATION);
  osTft.fillScreen(ST77XX_BLACK);

  osDisplayReady = true;
}

static void osReadButtons(bool *aDown, bool *bDown, bool *leftSWDown) {
  *aDown = osDebouncedPressed(OS_KEY_A_PIN, &aRawPrev, &aStable, &aChangedAt);
  *bDown = osDebouncedPressed(OS_KEY_B_PIN, &bRawPrev, &bStable, &bChangedAt);
  *leftSWDown = osDebouncedPressed(OS_LEFT_JOY_SW_PIN, &leftSWRawPrev, &leftSWStable, &leftSWChangedAt);
}

static void osDrawAppRow(int y, const char *label, bool selected, bool enabled) {
  uint16_t bg = selected ? osRGB565(90, 18, 12) : osRGB565(16, 14, 14);
  uint16_t border = selected ? osRGB565(220, 55, 30) : osRGB565(50, 45, 45);
  uint16_t fg = enabled ? ST77XX_WHITE : osRGB565(105, 105, 105);
  osTft.fillRoundRect(14, y, 132, 24, 5, bg);
  osTft.drawRoundRect(14, y, 132, 24, 5, border);
  osTft.setTextSize(1);
  osTft.setTextColor(fg);
  osTft.setCursor(28, y + 8);
  osTft.print(label);
  if (!enabled) {
    osTft.setCursor(104, y + 8);
    osTft.print("MISS");
  }
}

void ESP32Toy_OSRedrawLauncher(bool doomReady) {
  osEnsureHardware();
  osTft.fillScreen(ST77XX_BLACK);
  osTft.fillRect(0, 0, kOSW, 20, osRGB565(95, 12, 8));
  osDrawCentered(5, "ESP32Toy OS", ST77XX_WHITE, 1);

  osTft.setTextSize(1);
  osTft.setTextColor(osRGB565(170, 170, 170));
  osTft.setCursor(12, 28);
  osTft.print("Select app");

  osDrawAppRow(44, "Water Lab", selectedItem == 0, true);
  osDrawAppRow(74, "DOOM", selectedItem == 1, doomReady);

  osTft.setTextColor(osRGB565(150, 150, 150));
  osTft.setCursor(10, 112);
  osTft.print("A:OK  B:Back  Stick:Move");

  if (millis() < warningUntilMs) {
    osTft.fillRect(12, 98, 136, 11, ST77XX_BLACK);
    osDrawCentered(99, "doom1.wad not ready", ST77XX_YELLOW, 1);
  }
}

void ESP32Toy_OSInitLauncher(bool doomReady) {
  osEnsureHardware();
  selectedItem = 0;
  lastNavMs = 0;
  warningUntilMs = 0;
  ESP32Toy_OSRedrawLauncher(doomReady);
}

ESP32Toy_OSAction ESP32Toy_OSLauncherTick(bool doomReady) {
  osEnsureHardware();

  bool aDown = false;
  bool bDown = false;
  bool leftSWDown = false;
  osReadButtons(&aDown, &bDown, &leftSWDown);

  const bool aPressed = aDown && !aPrev;
  aPrev = aDown;

  const int rawY = analogRead(OS_LEFT_JOY_Y_PIN);
  const float y = osClamp((rawY - 2048) / 1800.0f, -1.0f, 1.0f) * -1.0f;
  const uint32_t now = millis();
  if (now - lastNavMs > 180) {
    if (y > 0.45f && selectedItem > 0) {
      selectedItem--;
      lastNavMs = now;
      ESP32Toy_OSRedrawLauncher(doomReady);
    } else if (y < -0.45f && selectedItem < 1) {
      selectedItem++;
      lastNavMs = now;
      ESP32Toy_OSRedrawLauncher(doomReady);
    }
  }

  if (warningUntilMs && now > warningUntilMs) {
    warningUntilMs = 0;
    ESP32Toy_OSRedrawLauncher(doomReady);
  }

  if (aPressed || (leftSWDown && !leftSWPrev)) {
    leftSWPrev = leftSWDown;
    if (selectedItem == 0) {
      return ESP32TOY_OS_ACTION_START_WATER;
    }
    if (doomReady) {
      return ESP32TOY_OS_ACTION_START_DOOM;
    }
    warningUntilMs = now + 1200;
    ESP32Toy_OSRedrawLauncher(doomReady);
    return ESP32TOY_OS_ACTION_NONE;
  }
  leftSWPrev = leftSWDown;

  (void)bDown;
  return ESP32TOY_OS_ACTION_NONE;
}

static void waterReset(void) {
  memset(waterA, 0, sizeof(waterA));
  memset(waterB, 0, sizeof(waterB));
  waterVX = 0.0f;
  waterVY = 0.0f;
  waterCX = kWaterCols / 2;
  waterCY = kWaterRows / 2;
}

static void waterSplash(int x, int y, float amount) {
  if (x < 2 || x >= kWaterCols - 2 || y < 2 || y >= kWaterRows - 2) return;
  waterA[y][x] += amount;
  waterA[y - 1][x] += amount * 0.45f;
  waterA[y + 1][x] += amount * 0.45f;
  waterA[y][x - 1] += amount * 0.45f;
  waterA[y][x + 1] += amount * 0.45f;
}

static void waterDraw(void) {
  for (int y = 0; y < kWaterRows; ++y) {
    for (int x = 0; x < kWaterCols; ++x) {
      float h = waterA[y][x];
      h = osClamp(h, -1.0f, 1.0f);
      const uint8_t blue = (uint8_t)(75 + fabsf(h) * 150);
      const uint8_t green = (uint8_t)(20 + max(0.0f, h) * 85);
      const uint8_t red = (uint8_t)(max(0.0f, -h) * 35);
      osTft.fillRect(x * kCellW, y * kCellH, kCellW, kCellH, osRGB565(red, green, blue));
    }
  }

  osTft.drawCircle(waterCX * kCellW + 2, waterCY * kCellH + 2, 3, ST77XX_WHITE);
  osTft.fillRect(0, 0, 160, 10, ST77XX_BLACK);
  osTft.setTextSize(1);
  osTft.setTextColor(ST77XX_WHITE);
  osTft.setCursor(4, 1);
  osTft.print("Water Lab  B:Menu");
}

bool ESP32Toy_OSWaterTick(void) {
  osEnsureHardware();

  static bool waterInitialized = false;
  if (!waterInitialized) {
    waterReset();
    osTft.fillScreen(ST77XX_BLACK);
    waterInitialized = true;
    lastFrameMs = 0;
  }

  bool aDown = false;
  bool bDown = false;
  bool leftSWDown = false;
  osReadButtons(&aDown, &bDown, &leftSWDown);

  const bool bPressed = bDown && !bPrev;
  bPrev = bDown;
  if (bPressed) {
    waterInitialized = false;
    return true;
  }

  const int rawX = analogRead(OS_LEFT_JOY_X_PIN);
  const int rawY = analogRead(OS_LEFT_JOY_Y_PIN);
  const float ax = osClamp((rawX - 2048) / 1800.0f, -1.0f, 1.0f);
  const float ay = osClamp((rawY - 2048) / 1800.0f, -1.0f, 1.0f) * -1.0f;

  waterVX = waterVX * 0.82f + ax * 0.58f;
  waterVY = waterVY * 0.82f + ay * 0.58f;
  waterCX = constrain((int)(waterCX + waterVX), 2, kWaterCols - 3);
  waterCY = constrain((int)(waterCY + waterVY), 2, kWaterRows - 3);

  if (aDown || leftSWDown) {
    waterSplash(waterCX, waterCY, 1.8f);
  }

  const uint32_t now = millis();
  if (now - lastFrameMs < 32) {
    return false;
  }
  lastFrameMs = now;

  for (int y = 1; y < kWaterRows - 1; ++y) {
    for (int x = 1; x < kWaterCols - 1; ++x) {
      const float n = (waterA[y - 1][x] + waterA[y + 1][x] + waterA[y][x - 1] + waterA[y][x + 1]) * 0.5f - waterB[y][x];
      waterB[y][x] = n * 0.965f;
    }
  }

  for (int y = 1; y < kWaterRows - 1; ++y) {
    for (int x = 1; x < kWaterCols - 1; ++x) {
      waterA[y][x] = waterB[y][x];
    }
  }

  waterDraw();
  return false;
}
