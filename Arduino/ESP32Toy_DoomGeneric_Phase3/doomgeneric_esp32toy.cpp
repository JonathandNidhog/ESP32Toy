#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_NeoPixel.h>
#include "doomgeneric_esp32toy.h"

// DoomGeneric symbols provided by upstream engine sources.
#ifndef DOOMGENERIC_RESX
#define DOOMGENERIC_RESX 160
#endif
#ifndef DOOMGENERIC_RESY
#define DOOMGENERIC_RESY 128
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

// ---------------- Existing hardware ----------------
#define POT_PIN          1
#define RIGHT_JOY_X_PIN 16
#define RIGHT_JOY_Y_PIN  8
#define RIGHT_JOY_SW_PIN 6
#define MOTOR_PIN        7
#define KEY_A_PIN       15
#define KEY_B_PIN       14

// ---------------- New left joystick ----------------
#define LEFT_JOY_X_PIN  17
#define LEFT_JOY_Y_PIN  18
#define LEFT_JOY_SW_PIN 13

// ---------------- RGB LED board ----------------
#define RGB_PIN   47
#define RGB_COUNT  4

#define MOTOR_ACTIVE_HIGH true
#define TFT_ROTATION 3

SPIClass screenSPI(FSPI);
Adafruit_ST7735 tft(&screenSPI, TFT_CS, TFT_DC, TFT_RST);
Adafruit_NeoPixel leds(RGB_COUNT, RGB_PIN, NEO_GRB + NEO_KHZ800);

static uint16_t line565[DOOMGENERIC_RESX];

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
static const float MOVE_SIGN = -1.0f;
static const float STRAFE_SIGN = 1.0f;
static const float TURN_SIGN = -1.0f;

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

static void updateAnalogKeys() {
  const int rightRawX = analogRead(RIGHT_JOY_X_PIN);
  const int rightRawY = analogRead(RIGHT_JOY_Y_PIN);
  const int leftRawX = analogRead(LEFT_JOY_X_PIN);
  const int leftRawY = analogRead(LEFT_JOY_Y_PIN);

  axisRightX = applyDeadzone(clampf((rightRawX - rightCenterX) / 1800.0f, -1.0f, 1.0f));
  axisRightY = applyDeadzone(clampf((rightRawY - rightCenterY) / 1800.0f, -1.0f, 1.0f));
  axisLeftX = applyDeadzone(clampf((leftRawX - leftCenterX) / 1800.0f, -1.0f, 1.0f));
  axisLeftY = applyDeadzone(clampf((leftRawY - leftCenterY) / 1800.0f, -1.0f, 1.0f));

  const float moveAxis = axisLeftY * MOVE_SIGN;
  const float strafeAxis = axisLeftX * STRAFE_SIGN;
  const float turnAxis = axisRightX * TURN_SIGN;

  setDoomKey(KEY_UPARROW, moveAxis > 0.35f);
  setDoomKey(KEY_DOWNARROW, moveAxis < -0.35f);
  setDoomKey(KEY_STRAFE_R, strafeAxis > 0.35f);
  setDoomKey(KEY_STRAFE_L, strafeAxis < -0.35f);
  setDoomKey(KEY_RIGHTARROW, turnAxis > 0.35f);
  setDoomKey(KEY_LEFTARROW, turnAxis < -0.35f);
}

static void updateDigitalKeys() {
  const bool a = debouncedPressed(KEY_A_PIN, &keyARawPrev, &keyAStable, &keyAChangedAt);
  const bool b = debouncedPressed(KEY_B_PIN, &keyBRawPrev, &keyBStable, &keyBChangedAt);
  const bool rightSW = debouncedPressed(RIGHT_JOY_SW_PIN, &rightSWRawPrev, &rightSWStable, &rightSWChangedAt);
  const bool leftSW = debouncedPressed(LEFT_JOY_SW_PIN, &leftSWRawPrev, &leftSWStable, &leftSWChangedAt);

  setDoomKey(KEY_FIRE, a);
  setDoomKey(KEY_USE, b);
  setDoomKey(KEY_ESCAPE, rightSW);
  setDoomKey(KEY_RSHIFT, leftSW);

  static bool aPrev = false;
  if (a && !aPrev) {
    motorPulse(90);
    flashUntilMs = millis() + 90;
  }
  aPrev = a;
}

static uint16_t doomPixelTo565(uint32_t p) {
  // DoomGeneric non-CMAP256 builds generally expose 0xAARRGGBB-like 32-bit pixels.
  // We only need RGB for ST7735.
  const uint8_t r = (uint8_t)((p >> 16) & 0xFF);
  const uint8_t g = (uint8_t)((p >> 8) & 0xFF);
  const uint8_t b = (uint8_t)(p & 0xFF);
  return rgb565(r, g, b);
}

void ESP32Toy_DoomPlatformInitHardware(void) {
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

  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(15, 44);
  tft.print("DOOMGENERIC BOOT");
  tft.setCursor(18, 61);
  tft.print("CALIBRATING STICKS");
  calibrateJoysticks();
  tft.fillScreen(ST77XX_BLACK);
}

void ESP32Toy_DoomPlatformAfterTick(void) {
  updateAnalogKeys();
  updateDigitalKeys();
  updateEffects();
}

extern "C" void DG_Init(void) {
  // Hardware is initialized by Arduino setup before doomgeneric_Create().
}

extern "C" void DG_DrawFrame(void) {
  if (!DG_ScreenBuffer) return;

  for (int y = 0; y < DOOMGENERIC_RESY; ++y) {
    const int row = y * DOOMGENERIC_RESX;
    for (int x = 0; x < DOOMGENERIC_RESX; ++x) {
      line565[x] = doomPixelTo565(DG_ScreenBuffer[row + x]);
    }
    tft.drawRGBBitmap(0, y, line565, DOOMGENERIC_RESX, 1);
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
