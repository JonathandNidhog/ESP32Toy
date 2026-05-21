#include "esp32toy_base_system.h"

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

// Base System reuses the screen object initialized by doomgeneric_esp32toy.cpp.
// Do not create or initialize a second FSPI/ST7735 instance here.
extern Adafruit_ST7735 tft;
#define osTft tft

// Same physical hardware pins as the Doom platform bridge.
#define OS_TFT_BL   21
#define OS_POT_PIN   1
#define OS_I2C_SDA   4
#define OS_I2C_SCL   5
#define OS_RIGHT_JOY_X_PIN 16
#define OS_RIGHT_JOY_Y_PIN  8
#define OS_RIGHT_JOY_SW_PIN 6
#define OS_KEY_A_PIN       15
#define OS_KEY_B_PIN       14
#define OS_LEFT_JOY_X_PIN  17
#define OS_LEFT_JOY_Y_PIN  18
#define OS_LEFT_JOY_SW_PIN 13

static const int kOSW = 160;
static const int kOSH = 128;
static const int kFramePixels = kOSW * kOSH;
static uint16_t *frameBuf = nullptr;

static bool osDisplayReady = false;
static bool osBootAnimPlayed = false;
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

static uint16_t osRGB565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static float osClamp(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static float osDeadzone(float v, float dz) {
  if (fabsf(v) <= dz) return 0.0f;
  const float s = v >= 0.0f ? 1.0f : -1.0f;
  const float t = (fabsf(v) - dz) / (1.0f - dz);
  return s * osClamp(t, 0.0f, 1.0f);
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

static void osAllocFrameBuffer() {
  if (frameBuf) return;
  frameBuf = (uint16_t*) heap_caps_malloc(kFramePixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!frameBuf) {
    frameBuf = (uint16_t*) heap_caps_malloc(kFramePixels * sizeof(uint16_t), MALLOC_CAP_8BIT);
  }
}

static void fbClear(uint16_t c) {
  if (!frameBuf) return;
  for (int i = 0; i < kFramePixels; ++i) frameBuf[i] = c;
}

static void fbPixel(int x, int y, uint16_t c) {
  if (!frameBuf) return;
  if ((unsigned)x >= kOSW || (unsigned)y >= kOSH) return;
  frameBuf[y * kOSW + x] = c;
}

static void fbRect(int x, int y, int w, int h, uint16_t c) {
  if (!frameBuf) return;
  if (w <= 0 || h <= 0) return;
  int x0 = max(0, x);
  int y0 = max(0, y);
  int x1 = min(kOSW, x + w);
  int y1 = min(kOSH, y + h);
  for (int yy = y0; yy < y1; ++yy) {
    uint16_t *row = frameBuf + yy * kOSW;
    for (int xx = x0; xx < x1; ++xx) row[xx] = c;
  }
}

static void fbCircle(int cx, int cy, int r, uint16_t c) {
  if (r <= 0) {
    fbPixel(cx, cy, c);
    return;
  }
  const int r2 = r * r;
  for (int y = -r; y <= r; ++y) {
    for (int x = -r; x <= r; ++x) {
      if (x * x + y * y <= r2) fbPixel(cx + x, cy + y, c);
    }
  }
}

static void fbPush() {
  if (!frameBuf) return;
  osTft.drawRGBBitmap(0, 0, frameBuf, kOSW, kOSH);
}

static void osDrawCenteredDirect(int y, const char *text, uint16_t color, uint8_t size = 1) {
  osTft.setTextSize(size);
  osTft.setTextColor(color);
  int textWidth = (int)strlen(text) * 6 * size;
  int x = (kOSW - textWidth) / 2;
  if (x < 0) x = 0;
  osTft.setCursor(x, y);
  osTft.print(text);
}

// ============================================================
// MPU6050, migrated from old LiquidOS water behavior
// ============================================================
static uint8_t mpuAddr = 0x68;
static bool mpuOk = false;
static bool mpuTriedInit = false;

static float ax = 0.0f;
static float ay = 0.0f;
static float az = 1.0f;
static float gx = 0.0f;
static float gy = 0.0f;
static float gz = 0.0f;
static float biasAX = 0.0f;
static float biasAY = 0.0f;
static float lastAX = 0.0f;
static float lastAY = 0.0f;
static float lastAZ = 1.0f;

static bool osCheckI2C(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

static bool osReadMPU() {
  if (!mpuOk) return false;

  Wire.beginTransmission(mpuAddr);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;

  Wire.requestFrom(mpuAddr, (uint8_t)14, (uint8_t)true);
  if (Wire.available() < 14) return false;

  int16_t rawAX = (int16_t)((Wire.read() << 8) | Wire.read());
  int16_t rawAY = (int16_t)((Wire.read() << 8) | Wire.read());
  int16_t rawAZ = (int16_t)((Wire.read() << 8) | Wire.read());

  Wire.read();
  Wire.read();

  int16_t rawGX = (int16_t)((Wire.read() << 8) | Wire.read());
  int16_t rawGY = (int16_t)((Wire.read() << 8) | Wire.read());
  int16_t rawGZ = (int16_t)((Wire.read() << 8) | Wire.read());

  ax = rawAX / 16384.0f;
  ay = rawAY / 16384.0f;
  az = rawAZ / 16384.0f;
  gx = rawGX / 131.0f;
  gy = rawGY / 131.0f;
  gz = rawGZ / 131.0f;
  return true;
}

static void osInitMPU() {
  if (mpuTriedInit) return;
  mpuTriedInit = true;

  Wire.setPins(OS_I2C_SDA, OS_I2C_SCL);
  Wire.begin();
  delay(60);

  if (osCheckI2C(0x68)) {
    mpuAddr = 0x68;
    mpuOk = true;
  } else if (osCheckI2C(0x69)) {
    mpuAddr = 0x69;
    mpuOk = true;
  } else {
    mpuOk = false;
    return;
  }

  const uint8_t initRegs[][2] = {
    {0x6B, 0x00},
    {0x1A, 0x03},
    {0x1C, 0x00},
    {0x1B, 0x00}
  };

  for (uint8_t i = 0; i < 4; ++i) {
    Wire.beginTransmission(mpuAddr);
    Wire.write(initRegs[i][0]);
    Wire.write(initRegs[i][1]);
    Wire.endTransmission(true);
  }

  float sumAX = 0.0f;
  float sumAY = 0.0f;
  int valid = 0;
  for (int i = 0; i < 80; ++i) {
    if (osReadMPU()) {
      sumAX += ax;
      sumAY += ay;
      valid++;
    }
    delay(4);
  }
  if (valid > 0) {
    biasAX = sumAX / valid;
    biasAY = sumAY / valid;
  }
  lastAX = ax;
  lastAY = ay;
  lastAZ = az;
}

static void mapBaseVectorToScreen(float baseX, float baseY, float *outX, float *outY) {
  // Same tested landscape rotation 3 mapping from old LiquidOS.
  *outX = -baseY;
  *outY = baseX;
}

static void osEnsureHardware(void) {
  if (osDisplayReady) return;

  pinMode(OS_TFT_BL, OUTPUT);
  digitalWrite(OS_TFT_BL, HIGH);

  pinMode(OS_POT_PIN, INPUT);
  pinMode(OS_RIGHT_JOY_X_PIN, INPUT);
  pinMode(OS_RIGHT_JOY_Y_PIN, INPUT);
  pinMode(OS_LEFT_JOY_X_PIN, INPUT);
  pinMode(OS_LEFT_JOY_Y_PIN, INPUT);
  pinMode(OS_RIGHT_JOY_SW_PIN, INPUT_PULLUP);
  pinMode(OS_LEFT_JOY_SW_PIN, INPUT_PULLUP);
  pinMode(OS_KEY_A_PIN, INPUT_PULLUP);
  pinMode(OS_KEY_B_PIN, INPUT_PULLUP);

  osAllocFrameBuffer();
  osInitMPU();
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

static void osBootAnimation(bool doomReady) {
  if (osBootAnimPlayed) return;
  osBootAnimPlayed = true;

  for (int f = 0; f < 18; ++f) {
    uint8_t glow = (uint8_t)(20 + f * 7);
    osTft.fillScreen(osRGB565(1, 4, 12));
    osTft.drawRoundRect(14, 22, 132, 76, 8, osRGB565(glow, glow + 20, 255));
    osDrawCenteredDirect(38, "ESP32Toy", ST77XX_WHITE, 2);
    osDrawCenteredDirect(62, doomReady ? "DOOM READY" : "WATER READY", doomReady ? ST77XX_GREEN : ST77XX_CYAN, 1);
    osTft.fillRect(30, 86, (f * 100) / 17, 4, osRGB565(55, 125, 255));
    delay(18);
  }
}

void ESP32Toy_OSRedrawLauncher(bool doomReady) {
  osEnsureHardware();
  osTft.fillScreen(ST77XX_BLACK);
  osTft.fillRect(0, 0, kOSW, 20, osRGB565(95, 12, 8));
  osDrawCenteredDirect(5, "ESP32Toy OS", ST77XX_WHITE, 1);

  osTft.setTextSize(1);
  osTft.setTextColor(osRGB565(170, 170, 170));
  osTft.setCursor(12, 28);
  osTft.print(mpuOk ? "Select app   GYRO" : "Select app   NO GYRO");

  osDrawAppRow(44, "Water Lab", selectedItem == 0, true);
  osDrawAppRow(74, "DOOM", selectedItem == 1, doomReady);

  osTft.setTextColor(osRGB565(150, 150, 150));
  osTft.setCursor(10, 112);
  osTft.print("A:OK  B:Back  Stick:Move");

  if (millis() < warningUntilMs) {
    osTft.fillRect(12, 98, 136, 11, ST77XX_BLACK);
    osDrawCenteredDirect(99, "doom1.wad not ready", ST77XX_YELLOW, 1);
  }
}

void ESP32Toy_OSInitLauncher(bool doomReady) {
  osEnsureHardware();
  osBootAnimation(doomReady);
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
  // Final hardware direction: new joystick Y is inverted.
  const float y = osDeadzone(osClamp((rawY - 2048) / 1800.0f, -1.0f, 1.0f) * -1.0f, 0.14f);
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
    if (selectedItem == 0) return ESP32TOY_OS_ACTION_START_WATER;
    if (doomReady) return ESP32TOY_OS_ACTION_START_DOOM;
    warningUntilMs = now + 1200;
    ESP32Toy_OSRedrawLauncher(doomReady);
    return ESP32TOY_OS_ACTION_NONE;
  }
  leftSWPrev = leftSWDown;

  (void)bDown;
  return ESP32TOY_OS_ACTION_NONE;
}

// ============================================================
// Fullscreen LiquidOS-style water simulation
// Particle solver + density field + metaball rendering.
// ============================================================
static const int WATER_COUNT_MIN = 42;
static const int WATER_COUNT_MAX = 108;
static int activeWaterCount = 82;

static const float WATER_RADIUS = 4.0f;
static float wx[WATER_COUNT_MAX];
static float wy[WATER_COUNT_MAX];
static float wvx[WATER_COUNT_MAX];
static float wvy[WATER_COUNT_MAX];

static const int DROP_COUNT_MAX = 16;
static bool dropActive[DROP_COUNT_MAX];
static float dropX[DROP_COUNT_MAX];
static float dropY[DROP_COUNT_MAX];
static float dropVX[DROP_COUNT_MAX];
static float dropVY[DROP_COUNT_MAX];
static float dropRadius[DROP_COUNT_MAX];
static int dropLife[DROP_COUNT_MAX];
static int splashCooldown = 0;

static const int GRID_STEP = 2;
static const int FIELD_W = kOSW / GRID_STEP + 3;
static const int FIELD_H = kOSH / GRID_STEP + 3;
static uint16_t densityField[FIELD_W * FIELD_H];

static const int KERNEL_R_MIN = 8;
static const int KERNEL_R_MAX = 11;
static const int KERNEL_MAX_SIZE = KERNEL_R_MAX * 2 + 1;
static int kernelR = 9;
static int kernelBuiltR = -1;
static uint16_t kernel[KERNEL_MAX_SIZE * KERNEL_MAX_SIZE];
static const uint16_t FIELD_THRESHOLD = 455;

static float forceX = 0.0f;
static float forceY = 0.0f;
static float joystickForceX = 0.0f;
static float joystickForceY = 0.0f;
static float potFiltered = 0.0f;
static uint32_t waterFrame = 0;

static const uint16_t C_WATER = 0x3D7F;
static const uint16_t C_WATER_MID = 0x1C9F;
static const uint16_t C_WATER_DEEP = 0x01B1;
static const uint16_t C_EDGE = 0xB73F;
static const uint16_t C_WHITE = 0xFFFF;
static const uint16_t C_BG_WATER = 0x0007;

static void waterBuildKernel() {
  memset(kernel, 0, sizeof(kernel));
  const float radiusPx = kernelR * GRID_STEP;
  const float radiusSq = radiusPx * radiusPx;
  const int kernelSize = kernelR * 2 + 1;

  for (int ky = -kernelR; ky <= kernelR; ++ky) {
    for (int kx = -kernelR; kx <= kernelR; ++kx) {
      const float px = kx * GRID_STEP;
      const float py = ky * GRID_STEP;
      const float d2 = px * px + py * py;
      if (d2 < radiusSq) {
        const float t = 1.0f - d2 / radiusSq;
        kernel[(ky + kernelR) * kernelSize + (kx + kernelR)] = (uint16_t)(t * t * 1180.0f);
      }
    }
  }

  kernelBuiltR = kernelR;
}

static void waterReset(void) {
  randomSeed(micros());
  if (kernelBuiltR != kernelR) waterBuildKernel();

  int id = 0;
  const int cols = 12;
  const int rows = (WATER_COUNT_MAX + cols - 1) / cols;
  const float spacingX = 10.5f;
  const float spacingY = 6.6f;
  const float startX = (kOSW - (cols - 1) * spacingX) * 0.5f;
  const float blockH = (rows - 1) * spacingY;
  const float startY = kOSH - blockH - 8.0f;

  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < cols; ++x) {
      if (id >= WATER_COUNT_MAX) break;
      wx[id] = startX + x * spacingX + random(-1, 2);
      wy[id] = startY + y * spacingY + random(-1, 2);
      wvx[id] = 0.0f;
      wvy[id] = 0.0f;
      ++id;
    }
  }

  for (int i = 0; i < DROP_COUNT_MAX; ++i) {
    dropActive[i] = false;
    dropLife[i] = 0;
  }

  forceX = 0.0f;
  forceY = 0.0f;
  joystickForceX = 0.0f;
  joystickForceY = 0.0f;
  splashCooldown = 0;
  waterFrame = 0;
  activeWaterCount = 82;

  if (mpuOk) {
    lastAX = ax;
    lastAY = ay;
    lastAZ = az;
  }
}

static void waterUpdateAmount() {
  const int potRaw = analogRead(OS_POT_PIN);
  potFiltered = potFiltered <= 0.1f ? potRaw : potFiltered * 0.90f + potRaw * 0.10f;
  const float t = osClamp(potFiltered / 4095.0f, 0.0f, 1.0f);

  int targetWaterCount = WATER_COUNT_MIN + (int)(t * (WATER_COUNT_MAX - WATER_COUNT_MIN));
  int targetKernelR = KERNEL_R_MIN + (int)(t * (KERNEL_R_MAX - KERNEL_R_MIN) + 0.5f);
  targetWaterCount = constrain(targetWaterCount, WATER_COUNT_MIN, WATER_COUNT_MAX);
  targetKernelR = constrain(targetKernelR, KERNEL_R_MIN, KERNEL_R_MAX);

  if (targetWaterCount > activeWaterCount) {
    for (int i = activeWaterCount; i < targetWaterCount; ++i) {
      const int ref = random(0, activeWaterCount > 0 ? activeWaterCount : 1);
      wx[i] = osClamp(wx[ref] + random(-8, 9), WATER_RADIUS, kOSW - 1 - WATER_RADIUS);
      wy[i] = osClamp(wy[ref] + random(-8, 9), WATER_RADIUS, kOSH - 1 - WATER_RADIUS);
      wvx[i] = wvx[ref] * 0.15f;
      wvy[i] = wvy[ref] * 0.15f;
    }
  }

  activeWaterCount = targetWaterCount;
  kernelR = targetKernelR;
  if (kernelR != kernelBuiltR) waterBuildKernel();
}

static void waterConstrainParticle(int i) {
  const float bounce = -0.18f;
  if (wx[i] < WATER_RADIUS) { wx[i] = WATER_RADIUS; wvx[i] *= bounce; }
  if (wx[i] > kOSW - 1 - WATER_RADIUS) { wx[i] = kOSW - 1 - WATER_RADIUS; wvx[i] *= bounce; }
  if (wy[i] < WATER_RADIUS) { wy[i] = WATER_RADIUS; wvy[i] *= bounce; }
  if (wy[i] > kOSH - 1 - WATER_RADIUS) { wy[i] = kOSH - 1 - WATER_RADIUS; wvy[i] *= bounce; }
}

static void waterSolvePairs() {
  const float targetDist = WATER_RADIUS * 1.52f;
  const float targetDistSq = targetDist * targetDist;

  for (int i = 0; i < activeWaterCount; ++i) {
    for (int j = i + 1; j < activeWaterCount; ++j) {
      const float dx = wx[j] - wx[i];
      const float dy = wy[j] - wy[i];
      const float d2 = dx * dx + dy * dy;
      if (d2 < 0.0001f || d2 >= targetDistSq) continue;

      const float dist = sqrtf(d2);
      const float overlap = targetDist - dist;
      const float nx = dx / dist;
      const float ny = dy / dist;
      const float push = overlap * 0.46f;

      wx[i] -= nx * push;
      wy[i] -= ny * push;
      wx[j] += nx * push;
      wy[j] += ny * push;
    }
  }
}

static void waterApplyViscosity() {
  const float range = WATER_RADIUS * 2.65f;
  const float rangeSq = range * range;

  for (int i = 0; i < activeWaterCount; ++i) {
    for (int j = i + 1; j < activeWaterCount; ++j) {
      const float dx = wx[j] - wx[i];
      const float dy = wy[j] - wy[i];
      const float d2 = dx * dx + dy * dy;
      if (d2 >= rangeSq) continue;

      const float deltaVX = wvx[j] - wvx[i];
      const float deltaVY = wvy[j] - wvy[i];
      const float blend = 0.022f;

      wvx[i] += deltaVX * blend;
      wvy[i] += deltaVY * blend;
      wvx[j] -= deltaVX * blend;
      wvy[j] -= deltaVY * blend;
    }
  }
}

static void waterSpawnDrop(float x, float y, float vx, float vy, float radius) {
  for (int i = 0; i < DROP_COUNT_MAX; ++i) {
    if (!dropActive[i]) {
      dropActive[i] = true;
      dropX[i] = x;
      dropY[i] = y;
      dropVX[i] = vx;
      dropVY[i] = vy;
      dropRadius[i] = radius;
      dropLife[i] = 72;
      return;
    }
  }
}

static void waterEmitSplash(float fx, float fy, float strength) {
  if (activeWaterCount <= 0) return;

  float len = sqrtf(fx * fx + fy * fy);
  float upX = 0.0f;
  float upY = -1.0f;
  if (len > 0.05f) {
    upX = -fx / len;
    upY = -fy / len;
  }

  int surfaceIds[WATER_COUNT_MAX];
  float surfaceScores[WATER_COUNT_MAX];
  for (int i = 0; i < activeWaterCount; ++i) {
    surfaceIds[i] = i;
    surfaceScores[i] = wx[i] * upX + wy[i] * upY;
  }

  const int surfacePickCount = min(8, activeWaterCount);
  for (int a = 0; a < surfacePickCount; ++a) {
    int best = a;
    for (int b = a + 1; b < activeWaterCount; ++b) {
      if (surfaceScores[b] > surfaceScores[best]) best = b;
    }
    float tempScore = surfaceScores[a];
    surfaceScores[a] = surfaceScores[best];
    surfaceScores[best] = tempScore;
    int tempId = surfaceIds[a];
    surfaceIds[a] = surfaceIds[best];
    surfaceIds[best] = tempId;
  }

  const int count = constrain((int)(strength * 1.45f), 3, 7);
  for (int n = 0; n < count; ++n) {
    const int id = surfaceIds[random(0, surfacePickCount)];
    const float side = random(-100, 101) / 100.0f;
    const float sideX = -upY;
    const float sideY = upX;
    const float launch = 0.90f + strength * 0.44f;
    const float spread = 0.48f + strength * 0.11f;

    waterSpawnDrop(wx[id], wy[id], wvx[id] + upX * launch + sideX * side * spread,
                   wvy[id] + upY * launch + sideY * side * spread, random(1, 3));
    wvx[id] += upX * 0.14f;
    wvy[id] += upY * 0.14f;
  }
}

static void waterUpdateDrops() {
  for (int i = 0; i < DROP_COUNT_MAX; ++i) {
    if (!dropActive[i]) continue;
    dropVX[i] += forceX * 0.012f;
    dropVY[i] += 0.070f + forceY * 0.012f;
    dropVX[i] *= 0.994f;
    dropVY[i] *= 0.994f;
    dropX[i] += dropVX[i];
    dropY[i] += dropVY[i];
    dropLife[i]--;

    if (dropX[i] < 2) { dropX[i] = 2; dropVX[i] *= -0.24f; }
    if (dropX[i] > kOSW - 3) { dropX[i] = kOSW - 3; dropVX[i] *= -0.24f; }
    if (dropY[i] < 2) { dropY[i] = 2; dropVY[i] *= -0.20f; }

    for (int j = 0; j < activeWaterCount; ++j) {
      const float dx = dropX[i] - wx[j];
      const float dy = dropY[i] - wy[j];
      if (dx * dx + dy * dy < WATER_RADIUS * WATER_RADIUS * 1.85f) {
        wvx[j] += dropVX[i] * 0.045f;
        wvy[j] += dropVY[i] * 0.045f;
        dropActive[i] = false;
        break;
      }
    }

    if (dropY[i] > kOSH || dropLife[i] <= 0) dropActive[i] = false;
  }
}

static void waterBuildDensityField() {
  memset(densityField, 0, sizeof(densityField));
  const int kernelSize = kernelR * 2 + 1;

  for (int i = 0; i < activeWaterCount; ++i) {
    const int centerX = (int)(wx[i] / GRID_STEP);
    const int centerY = (int)(wy[i] / GRID_STEP);

    for (int ky = -kernelR; ky <= kernelR; ++ky) {
      const int fy = centerY + ky;
      if (fy < 0 || fy >= FIELD_H) continue;
      for (int kx = -kernelR; kx <= kernelR; ++kx) {
        const int fx = centerX + kx;
        if (fx < 0 || fx >= FIELD_W) continue;
        const uint16_t add = kernel[(ky + kernelR) * kernelSize + (kx + kernelR)];
        if (!add) continue;
        const int fieldIndex = fy * FIELD_W + fx;
        const uint32_t value = densityField[fieldIndex] + add;
        densityField[fieldIndex] = value > 65535 ? 65535 : value;
      }
    }
  }
}

static void waterDrawBody() {
  fbClear(C_BG_WATER);
  waterBuildDensityField();

  // Water body.  Draw density runs into the framebuffer, not directly to TFT.
  for (int gyIndex = 0; gyIndex < FIELD_H; ++gyIndex) {
    int runStart = -1;
    for (int gxIndex = 0; gxIndex < FIELD_W; ++gxIndex) {
      const bool inside = densityField[gyIndex * FIELD_W + gxIndex] >= FIELD_THRESHOLD;
      if (inside && runStart < 0) runStart = gxIndex;

      const bool last = gxIndex == FIELD_W - 1;
      if ((!inside || last) && runStart >= 0) {
        const int runEnd = (inside && last) ? gxIndex : gxIndex - 1;
        uint16_t layerColor = C_WATER;
        if (((gyIndex + (int)(waterFrame & 3)) & 7) == 0) layerColor = C_WATER_MID;
        if (((gyIndex + (int)(waterFrame & 7)) & 15) == 0) layerColor = C_WATER_DEEP;
        fbRect(runStart * GRID_STEP, gyIndex * GRID_STEP,
               (runEnd - runStart + 1) * GRID_STEP, GRID_STEP, layerColor);
        runStart = -1;
      }
    }
  }

  // Old LiquidOS-like top edge highlight.
  for (int gyIndex = 0; gyIndex < FIELD_H; ++gyIndex) {
    for (int gxIndex = 0; gxIndex < FIELD_W; ++gxIndex) {
      const int idx = gyIndex * FIELD_W + gxIndex;
      if (densityField[idx] < FIELD_THRESHOLD) continue;
      const bool topEdge = gyIndex == 0 || densityField[(gyIndex - 1) * FIELD_W + gxIndex] < FIELD_THRESHOLD;
      if (topEdge) {
        fbRect(gxIndex * GRID_STEP, gyIndex * GRID_STEP, GRID_STEP, 1, C_EDGE);
        if (((gxIndex + waterFrame) & 9) == 0) fbPixel(gxIndex * GRID_STEP, gyIndex * GRID_STEP, C_WHITE);
      }
    }
  }

  // Droplets with highlight.
  for (int i = 0; i < DROP_COUNT_MAX; ++i) {
    if (!dropActive[i]) continue;
    fbCircle((int)dropX[i], (int)dropY[i], (int)dropRadius[i], C_EDGE);
    if (dropRadius[i] >= 2) fbPixel((int)dropX[i] - 1, (int)dropY[i] - 1, C_WHITE);
  }

  // Minimal HUD strip. Drawn into buffer too, then pushed once.
  fbRect(0, 0, 160, 9, ST77XX_BLACK);
  fbPush();
  osTft.setTextSize(1);
  osTft.setTextColor(ST77XX_WHITE);
  osTft.setCursor(4, 1);
  osTft.print(mpuOk ? "Water Lab  GYRO  B:Menu" : "Water Lab  NO GYRO  B:Menu");
}

static void waterReadJoystickForce(float *outX, float *outY) {
  const int rawX = analogRead(OS_LEFT_JOY_X_PIN);
  const int rawY = analogRead(OS_LEFT_JOY_Y_PIN);

  float joyX = osClamp((rawX - 2048) / 1800.0f, -1.0f, 1.0f);
  // Final hardware direction: new joystick Y is inverted.
  float joyY = osClamp((rawY - 2048) / 1800.0f, -1.0f, 1.0f) * -1.0f;
  joyX = osDeadzone(joyX, 0.14f);
  joyY = osDeadzone(joyY, 0.14f);

  *outX = joyX;
  *outY = joyY;
}

static void waterStep(bool aDown, bool leftSWDown) {
  waterUpdateAmount();

  float joyX = 0.0f;
  float joyY = 0.0f;
  waterReadJoystickForce(&joyX, &joyY);
  joystickForceX = joystickForceX * 0.80f + joyX * 0.20f;
  joystickForceY = joystickForceY * 0.80f + joyY * 0.20f;

  bool gotMPU = osReadMPU();
  float tiltX = 0.0f;
  float tiltY = 0.0f;
  float gyroScreenX = 0.0f;
  float gyroScreenY = 0.0f;
  float splashStrength = 0.0f;

  if (gotMPU) {
    float baseTiltX = 1.0f * (ax - biasAX);
    float baseTiltY = -1.0f * (ay - biasAY);
    if (fabsf(baseTiltX) < 0.03f) baseTiltX = 0.0f;
    if (fabsf(baseTiltY) < 0.03f) baseTiltY = 0.0f;

    mapBaseVectorToScreen(osClamp(baseTiltX * 2.10f, -1.0f, 1.0f),
                          osClamp(baseTiltY * 2.10f, -1.0f, 1.0f),
                          &tiltX, &tiltY);
    mapBaseVectorToScreen(osClamp(gx * 0.008f, -1.3f, 1.3f),
                          osClamp(gy * 0.008f, -1.3f, 1.3f),
                          &gyroScreenX, &gyroScreenY);

    const float jerk = fabsf(ax - lastAX) + fabsf(ay - lastAY) + fabsf(az - lastAZ);
    lastAX = ax;
    lastAY = ay;
    lastAZ = az;
    const float kick = fabsf(gx) * 0.0028f + fabsf(gy) * 0.0028f + fabsf(gz) * 0.0018f;
    splashStrength = jerk * 6.5f + kick;
  }

  const float targetX = tiltX + joystickForceX * 0.75f;
  const float targetY = tiltY + joystickForceY * 0.75f;
  forceX = forceX * 0.84f + osClamp(targetX, -1.35f, 1.35f) * 0.16f;
  forceY = forceY * 0.84f + osClamp(targetY, -1.35f, 1.35f) * 0.16f;

  for (int i = 0; i < activeWaterCount; ++i) {
    wvx[i] += forceX * 0.27f + gyroScreenX * 0.18f;
    wvy[i] += forceY * 0.27f + gyroScreenY * 0.18f + 0.012f;
    wvx[i] *= 0.986f;
    wvy[i] *= 0.986f;
    wvx[i] = osClamp(wvx[i], -4.2f, 4.2f);
    wvy[i] = osClamp(wvy[i], -4.2f, 4.2f);
    wx[i] += wvx[i];
    wy[i] += wvy[i];
    waterConstrainParticle(i);
  }

  waterSolvePairs();
  waterSolvePairs();
  waterSolvePairs();

  for (int i = 0; i < activeWaterCount; ++i) waterConstrainParticle(i);
  waterApplyViscosity();
  waterUpdateDrops();

  if (splashCooldown > 0) splashCooldown--;
  if (splashStrength > 1.25f && splashCooldown <= 0) {
    waterEmitSplash(forceX + gyroScreenX * 0.35f, forceY + gyroScreenY * 0.35f, splashStrength);
    splashCooldown = 9;
  }
  if ((aDown || leftSWDown) && splashCooldown <= 0) {
    waterEmitSplash(forceX, forceY, 2.0f);
    splashCooldown = 8;
  }
}

bool ESP32Toy_OSWaterTick(void) {
  osEnsureHardware();

  static bool waterInitialized = false;
  if (!waterInitialized) {
    waterReset();
    fbClear(C_BG_WATER);
    fbPush();
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

  const uint32_t now = millis();
  if (now - lastFrameMs < 32) return false;
  lastFrameMs = now;
  waterFrame++;

  waterStep(aDown, leftSWDown);
  waterDrawBody();
  return false;
}
