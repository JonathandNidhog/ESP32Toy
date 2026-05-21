#include <Arduino.h>
#include <SPI.h>
#include <stdio.h>
#include <string.h>
#include <esp32-hal-psram.h>
#include "FS.h"
#include <LittleFS.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_NeoPixel.h>
#include "doomgeneric_esp32toy.h"

// DoomGeneric symbols provided by upstream engine sources.
// Keep the classic internal Doom framebuffer.  The ST7735 presentation layer
// downsamples this 320x200 buffer to a 160x120 4:3 image with 4-pixel bars on
// the 160x128 panel.
#ifndef DOOMGENERIC_RESX
#define DOOMGENERIC_RESX 320
#endif
#ifndef DOOMGENERIC_RESY
#define DOOMGENERIC_RESY 200
#endif

#ifndef KEY_RIGHTARROW
#define KEY_RIGHTARROW 0xae
#endif
#ifndef KEY_LEFTARROW
#define KEY_LEFTARROW 0xac
#endif
#ifndef KEY_UPARROW
#define KEY_UPARROW 0xad
#endif
#ifndef KEY_DOWNARROW
#define KEY_DOWNARROW 0xaf
#endif
#ifndef KEY_STRAFE_L
#define KEY_STRAFE_L 0xa0
#endif
#ifndef KEY_STRAFE_R
#define KEY_STRAFE_R 0xa1
#endif
#ifndef KEY_USE
#define KEY_USE 0xa2
#endif
#ifndef KEY_FIRE
#define KEY_FIRE 0xa3
#endif
#ifndef KEY_ESCAPE
#define KEY_ESCAPE 27
#endif
#ifndef KEY_ENTER
#define KEY_ENTER 13
#endif
#ifndef KEY_TAB
#define KEY_TAB 9
#endif
#ifndef KEY_RSHIFT
#define KEY_RSHIFT (0x80 + 0x36)
#endif

extern "C" {
  typedef uint32_t pixel_t;
  extern pixel_t *DG_ScreenBuffer;
}

// ---------------- Display pins ----------------
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC    9
#define TFT_BL   21
#define TFT_RST  -1

// ---------------- Existing / old right joystick hardware ----------------
#define POT_PIN          1
#define RIGHT_JOY_X_PIN 16
#define RIGHT_JOY_Y_PIN  8
#define RIGHT_JOY_SW_PIN 6
#define MOTOR_PIN        7
#define KEY_A_PIN       15
#define KEY_B_PIN       14

// ---------------- New / left joystick hardware ----------------
#define LEFT_JOY_X_PIN  17
#define LEFT_JOY_Y_PIN  18
#define LEFT_JOY_SW_PIN 13

// ---------------- RGB LED board ----------------
#define RGB_PIN   47
#define RGB_COUNT  4

#define MOTOR_ACTIVE_HIGH true
#define TFT_ROTATION 3

// Physical ST7735 panel size in current landscape orientation.
static const int kPanelWidth = 160;
static const int kPanelHeight = 128;

// Doom presentation target on the panel.
// 160x120 preserves the intended 4:3 Doom display better than stretching raw
// 320x200 directly into 160x128, while leaving only thin 4px bars above/below.
static const int kDoomPresentWidth = 160;
static const int kDoomPresentHeight = 120;
static const int kDoomPresentX = 0;
static const int kDoomPresentY = 4;

// LittleFS is mounted at /littlefs by Arduino-ESP32's official wrapper.
static const char *kLittleFSBasePath = "/littlefs";
static const char *kIWADRelativePath = "/doom1.wad";
static const char *kIWADPosixPath = "/littlefs/doom1.wad";
static const bool kFormatLittleFSIfMountFails = false;

SPIClass screenSPI(FSPI);
Adafruit_ST7735 tft(&screenSPI, TFT_CS, TFT_DC, TFT_RST);
Adafruit_NeoPixel leds(RGB_COUNT, RGB_PIN, NEO_GRB + NEO_KHZ800);

// One physical ST7735 scanline after downsampling.
static uint16_t line565[kDoomPresentWidth];

// ---------------- Input calibration ----------------
static int rightCenterX = 2048;
static int rightCenterY = 2048;
static int leftCenterX = 2048;
static int leftCenterY = 2048;

static float axisRightX = 0.0f;
static float axisRightY = 0.0f;
static float axisLeftX = 0.0f;
static float axisLeftY = 0.0f;

static const float DEADZONE = 0.18f;

// Final control directions confirmed in the raycaster prototype:
// - Old/right stick Y: move forward/backward, inverted in the latest test.
// - Old/right stick X: strafe left/right.
// - New/left stick X: turn camera left/right, inverted in the latest test.
// - New/left stick Y is read/calibrated for future Doom-specific extensions,
//   but vanilla Doom itself has no free vertical-look keyboard axis.
static const float MOVE_SIGN = 1.0f;
static const float STRAFE_SIGN = 1.0f;
static const float TURN_SIGN = 1.0f;

// ---------------- Digital event queue ----------------
struct KeyEvent {
  uint8_t pressed;
  uint8_t key;
};

static KeyEvent keyQueue[32];
static uint8_t keyRead = 0;
static uint8_t keyWrite = 0;

static bool lastKeyState[256] = { false };

static bool keyAStable = false;
static bool keyBStable = false;
static bool rightSWStable = false;
static bool leftSWStable = false;
static bool keyARawPrev = false;
static bool keyBRawPrev = false;
static bool rightSWRawPrev = false;
static bool leftSWRawPrev = false;
static uint32_t keyAChangedAt = 0;
static uint32_t keyBChangedAt = 0;
static uint32_t rightSWChangedAt = 0;
static uint32_t leftSWChangedAt = 0;

// ---------------- Haptics / LED ----------------
static uint32_t motorUntilMs = 0;
static uint32_t flashUntilMs = 0;
static uint32_t lastLedMs = 0;
static uint8_t ledPhase = 0;

// ---------------- Board flash / memory boot state ----------------
static bool littleFSMounted = false;
static bool iwadPresent = false;
static bool psramAvailable = false;
static size_t iwadBytes = 0;
static uint32_t idleBlinkAtMs = 0;
static bool idleLedOn = false;

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static float applyDeadzone(float v) {
  if (fabsf(v) <= DEADZONE) return 0.0f;
  const float signValue = v >= 0.0f ? 1.0f : -1.0f;
  const float t = (fabsf(v) - DEADZONE) / (1.0f - DEADZONE);
  return signValue * clampf(t, 0.0f, 1.0f);
}

static void enqueueKey(bool pressed, uint8_t key) {
  const uint8_t next = (uint8_t)((keyWrite + 1) % 32);
  if (next == keyRead) return;
  keyQueue[keyWrite].pressed = pressed ? 1 : 0;
  keyQueue[keyWrite].key = key;
  keyWrite = next;
}

static void setDoomKey(uint8_t key, bool pressed) {
  if (lastKeyState[key] == pressed) return;
  lastKeyState[key] = pressed;
  enqueueKey(pressed, key);
}

static bool debouncedPressed(int pin, bool *rawPrev, bool *stable, uint32_t *changedAt) {
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

static void motorWrite(bool on) {
  digitalWrite(MOTOR_PIN, MOTOR_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH));
}

static void motorPulse(uint32_t ms) {
  const uint32_t until = millis() + ms;
  if (until > motorUntilMs) motorUntilMs = until;
}

static void ledAll(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < RGB_COUNT; ++i) {
    leds.setPixelColor(i, leds.Color(r, g, b));
  }
  leds.show();
}

static void updateEffects() {
  const uint32_t now = millis();
  motorWrite(now < motorUntilMs);

  if (now < flashUntilMs) {
    ledAll(90, 28, 0);
    return;
  }

  if (now - lastLedMs < 70) return;
  lastLedMs = now;
  ledPhase += 6;
  for (int i = 0; i < RGB_COUNT; ++i) {
    uint8_t wave = (uint8_t)((ledPhase + i * 35) & 127);
    if (wave > 63) wave = 127 - wave;
    leds.setPixelColor(i, leds.Color(22 + wave / 2, 0, 0));
  }
  leds.show();
}

static void calibrateJoysticks() {
  long rx = 0;
  long ry = 0;
  long lx = 0;
  long ly = 0;
  for (int i = 0; i < 72; ++i) {
    rx += analogRead(RIGHT_JOY_X_PIN);
    ry += analogRead(RIGHT_JOY_Y_PIN);
    lx += analogRead(LEFT_JOY_X_PIN);
    ly += analogRead(LEFT_JOY_Y_PIN);
    delay(4);
  }
  rightCenterX = rx / 72;
  rightCenterY = ry / 72;
  leftCenterX = lx / 72;
  leftCenterY = ly / 72;
}

static void drawCenteredLine(int y, const char *text, uint16_t color) {
  tft.setTextColor(color);
  tft.setTextSize(1);
  const int textWidth = (int)strlen(text) * 6;
  int x = (kPanelWidth - textWidth) / 2;
  if (x < 0) x = 0;
  tft.setCursor(x, y);
  tft.print(text);
}

static void drawBootHeader(void) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);
  tft.setTextSize(2);
  tft.setTextColor(rgb565(220, 40, 28));
  tft.setCursor(22, 22);
  tft.print("DOOM BOOT");
  tft.drawFastHLine(12, 46, 136, rgb565(120, 24, 18));
}

static void drawMissingPSRAMScreen(void) {
  drawBootHeader();
  drawCenteredLine(58, "PSRAM NOT FOUND", ST77XX_RED);
  drawCenteredLine(72, "ENABLE PSRAM IN IDE", ST77XX_YELLOW);
  drawCenteredLine(86, "BOARD: ESP32-S3 N16R8", ST77XX_WHITE);
  drawCenteredLine(100, "DOOM BOOT IS BLOCKED", rgb565(170, 170, 170));
}

static void drawMissingIWADScreen(void) {
  drawBootHeader();
  drawCenteredLine(58, littleFSMounted ? "BOARD FLASH READY" : "LITTLEFS MOUNT FAILED", littleFSMounted ? ST77XX_GREEN : ST77XX_RED);
  drawCenteredLine(72, "MISSING: doom1.wad", ST77XX_YELLOW);
  drawCenteredLine(86, "UPLOAD TO LITTLEFS /", ST77XX_WHITE);
  drawCenteredLine(100, "EXPECTED: /doom1.wad", rgb565(170, 170, 170));
}

static void drawFoundIWADScreen(void) {
  char sizeLine[32];
  snprintf(sizeLine, sizeof(sizeLine), "WAD OK: %lu KB", (unsigned long)(iwadBytes / 1024UL));
  drawBootHeader();
  drawCenteredLine(58, "PSRAM + FLASH OK", ST77XX_GREEN);
  drawCenteredLine(72, sizeLine, ST77XX_WHITE);
  drawCenteredLine(86, "STARTING DOOM...", ST77XX_YELLOW);
}

static void mountLittleFSAndFindIWAD(void) {
  littleFSMounted = LittleFS.begin(kFormatLittleFSIfMountFails, kLittleFSBasePath, 10, "spiffs");
  if (!littleFSMounted) {
    Serial.println("[ESP32Toy Doom] LittleFS mount failed.");
    iwadPresent = false;
    iwadBytes = 0;
    drawMissingIWADScreen();
    return;
  }

  Serial.printf("[ESP32Toy Doom] LittleFS mounted. total=%lu used=%lu\n", (unsigned long)LittleFS.totalBytes(), (unsigned long)LittleFS.usedBytes());

  File wad = LittleFS.open(kIWADRelativePath, FILE_READ);
  if (!wad || wad.isDirectory()) {
    Serial.printf("[ESP32Toy Doom] IWAD missing: %s\n", kIWADRelativePath);
    iwadPresent = false;
    iwadBytes = 0;
    drawMissingIWADScreen();
    if (wad) wad.close();
    return;
  }

  iwadBytes = wad.size();
  iwadPresent = iwadBytes > 0;
  wad.close();

  Serial.printf("[ESP32Toy Doom] IWAD found: %s (%lu bytes)\n", kIWADRelativePath, (unsigned long)iwadBytes);
  drawFoundIWADScreen();
  delay(550);
  tft.fillScreen(ST77XX_BLACK);
}

static void updateAnalogKeys() {
  const int rightRawX = analogRead(RIGHT_JOY_X_PIN);
  const int rightRawY = analogRead(RIGHT_JOY_Y_PIN);
  const int leftRawX = analogRead(LEFT_JOY_X_PIN);
  const int leftRawY = analogRead(LEFT_JOY_Y_PIN);

  axisRightX = applyDeadzone(clampf((rightRawX - rightCenterX) / 1800.0f, -1.0f, 1.0f));
  axisRightY = applyDeadzone(clampf((rightRawY - rightCenterY) / 1800.0f, -1.0f, 1.0f));
  axisLeftX = applyDeadzone(clampf((leftRawX - leftCenterX) / 1800.0f, -1.0f, 1.0f));
  axisLeftY = applyDeadzone(clampf((leftRawY - leftCenterY) / 1800.0f, -1.0f, 1.0f));

  const float moveAxis = axisRightY * MOVE_SIGN;
  const float strafeAxis = axisRightX * STRAFE_SIGN;
  const float turnAxis = axisLeftX * TURN_SIGN;

  setDoomKey(KEY_UPARROW, moveAxis > 0.35f);
  setDoomKey(KEY_DOWNARROW, moveAxis < -0.35f);
  setDoomKey(KEY_STRAFE_R, strafeAxis > 0.35f);
  setDoomKey(KEY_STRAFE_L, strafeAxis < -0.35f);
  setDoomKey(KEY_RIGHTARROW, turnAxis > 0.35f);
  setDoomKey(KEY_LEFTARROW, turnAxis < -0.35f);

  // Keep the new/left vertical axis sampled and calibrated even though
  // the vanilla Doom keyboard API does not expose mouse-look pitch.
  (void)axisLeftY;
}

static void updateDigitalKeys() {
  const bool a = debouncedPressed(KEY_A_PIN, &keyARawPrev, &keyAStable, &keyAChangedAt);
  const bool b = debouncedPressed(KEY_B_PIN, &keyBRawPrev, &keyBStable, &keyBChangedAt);
  const bool rightSW = debouncedPressed(RIGHT_JOY_SW_PIN, &rightSWRawPrev, &rightSWStable, &rightSWChangedAt);
  const bool leftSW = debouncedPressed(LEFT_JOY_SW_PIN, &leftSWRawPrev, &leftSWStable, &leftSWChangedAt);

  // DoomGeneric keyboard mapping for the ESP32Toy hardware:
  //   A button          -> fire in-game + enter/confirm in menus
  //   B button          -> escape/menu/back only
  //   Old/right SW      -> run modifier
  //   New/left SW       -> use/open door
  setDoomKey(KEY_FIRE, a);
  setDoomKey(KEY_ENTER, a);
  setDoomKey(KEY_ESCAPE, b);
  setDoomKey(KEY_RSHIFT, rightSW);
  setDoomKey(KEY_USE, leftSW);

  static bool aPrev = false;
  if (a && !aPrev) {
    motorPulse(90);
    flashUntilMs = millis() + 90;
  }
  aPrev = a;
}

static uint16_t doomPixelTo565(uint32_t p) {
  // DoomGeneric non-CMAP256 builds expose 0xAARRGGBB-like 32-bit pixels.
  // We only need RGB for ST7735.
  const uint8_t r = (uint8_t)((p >> 16) & 0xFF);
  const uint8_t g = (uint8_t)((p >> 8) & 0xFF);
  const uint8_t b = (uint8_t)(p & 0xFF);
  return rgb565(r, g, b);
}

void ESP32Toy_DoomPlatformInitHardware(void) {
  Serial.begin(115200);
  delay(120);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  pinMode(POT_PIN, INPUT);
  pinMode(RIGHT_JOY_X_PIN, INPUT);
  pinMode(RIGHT_JOY_Y_PIN, INPUT);
  pinMode(LEFT_JOY_X_PIN, INPUT);
  pinMode(LEFT_JOY_Y_PIN, INPUT);
  pinMode(RIGHT_JOY_SW_PIN, INPUT_PULLUP);
  pinMode(LEFT_JOY_SW_PIN, INPUT_PULLUP);
  pinMode(KEY_A_PIN, INPUT_PULLUP);
  pinMode(KEY_B_PIN, INPUT_PULLUP);
  pinMode(MOTOR_PIN, OUTPUT);
  motorWrite(false);

  leds.begin();
  leds.setBrightness(32);
  ledAll(0, 0, 0);

  screenSPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(TFT_ROTATION);
  tft.fillScreen(ST77XX_BLACK);

  drawBootHeader();
  drawCenteredLine(58, "CHECKING PSRAM", ST77XX_WHITE);
  psramAvailable = psramFound();
  Serial.printf("[ESP32Toy Doom] PSRAM found: %s\n", psramAvailable ? "yes" : "no");
  if (!psramAvailable) {
    drawMissingPSRAMScreen();
    return;
  }

  drawBootHeader();
  drawCenteredLine(58, "CALIBRATING STICKS", ST77XX_WHITE);
  calibrateJoysticks();
  mountLittleFSAndFindIWAD();
}

void ESP32Toy_DoomPlatformBeforeTick(void) {
  updateAnalogKeys();
  updateDigitalKeys();
}

void ESP32Toy_DoomPlatformAfterTick(void) {
  updateEffects();
}

void ESP32Toy_DoomPlatformIdle(void) {
  updateEffects();

  const uint32_t now = millis();
  if (now - idleBlinkAtMs < 450) return;
  idleBlinkAtMs = now;
  idleLedOn = !idleLedOn;
  if (idleLedOn) {
    ledAll(45, 6, 0);
  } else {
    ledAll(0, 0, 0);
  }
}

bool ESP32Toy_DoomPlatformHasIWAD(void) {
  return littleFSMounted && iwadPresent;
}

bool ESP32Toy_DoomPlatformHasPSRAM(void) {
  return psramAvailable;
}

bool ESP32Toy_DoomPlatformReadyToStart(void) {
  return ESP32Toy_DoomPlatformHasPSRAM() && ESP32Toy_DoomPlatformHasIWAD();
}

const char *ESP32Toy_DoomPlatformIWADPath(void) {
  return kIWADPosixPath;
}

extern "C" void DG_Init(void) {
  // Hardware is initialized by Arduino setup before doomgeneric_Create().
  // The thin top/bottom presentation bars remain black for the Doom runtime.
  tft.fillRect(0, 0, kPanelWidth, kDoomPresentY, ST77XX_BLACK);
  tft.fillRect(0, kDoomPresentY + kDoomPresentHeight, kPanelWidth,
               kPanelHeight - (kDoomPresentY + kDoomPresentHeight), ST77XX_BLACK);
}

extern "C" void DG_DrawFrame(void) {
  if (!DG_ScreenBuffer) return;

  // Downsample classic Doom 320x200 -> 160x120.
  // X is a cheap 2:1 sample. Y uses integer mapping to stretch the classic
  // 320x200 source to the intended 4:3 presentation ratio before sending it to
  // the 160x128 ST7735 panel.
  for (int dstY = 0; dstY < kDoomPresentHeight; ++dstY) {
    const int srcY = (dstY * DOOMGENERIC_RESY) / kDoomPresentHeight;
    const int srcRow = srcY * DOOMGENERIC_RESX;

    for (int dstX = 0; dstX < kDoomPresentWidth; ++dstX) {
      const int srcX = (dstX * DOOMGENERIC_RESX) / kDoomPresentWidth;
      line565[dstX] = doomPixelTo565(DG_ScreenBuffer[srcRow + srcX]);
    }

    tft.drawRGBBitmap(kDoomPresentX, kDoomPresentY + dstY, line565,
                      kDoomPresentWidth, 1);
  }
}

extern "C" void DG_SleepMs(uint32_t ms) {
  delay(ms);
}

extern "C" uint32_t DG_GetTicksMs(void) {
  return millis();
}

extern "C" int DG_GetKey(int *pressed, unsigned char *key) {
  if (keyRead == keyWrite) return 0;
  const KeyEvent evt = keyQueue[keyRead];
  keyRead = (uint8_t)((keyRead + 1) % 32);
  *pressed = evt.pressed;
  *key = evt.key;
  return 1;
}

extern "C" void DG_SetWindowTitle(const char *title) {
  (void)title;
}
