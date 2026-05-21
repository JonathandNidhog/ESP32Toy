#include "esp32toy_base_system.h"

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

extern Adafruit_ST7735 tft;
#define osTft tft

#define OS_TFT_BL 21
#define OS_POT_PIN 1
#define OS_I2C_SDA 4
#define OS_I2C_SCL 5
#define OS_RIGHT_JOY_X_PIN 16
#define OS_RIGHT_JOY_Y_PIN 8
#define OS_RIGHT_JOY_SW_PIN 6
#define OS_KEY_A_PIN 15
#define OS_KEY_B_PIN 14
#define OS_LEFT_JOY_X_PIN 17
#define OS_LEFT_JOY_Y_PIN 18
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

static bool aStable = false, bStable = false, leftSWStable = false;
static bool aRawPrev = false, bRawPrev = false, leftSWRawPrev = false;
static uint32_t aChangedAt = 0, bChangedAt = 0, leftSWChangedAt = 0;
static bool aPrev = false, bPrev = false, leftSWPrev = false;

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
  return s * osClamp((fabsf(v) - dz) / (1.0f - dz), 0.0f, 1.0f);
}

static bool osDebouncedPressed(int pin, bool *rawPrev, bool *stable, uint32_t *changedAt) {
  const bool rawPressed = digitalRead(pin) == LOW;
  const uint32_t now = millis();
  if (rawPressed != *rawPrev) {
    *rawPrev = rawPressed;
    *changedAt = now;
  }
  if (now - *changedAt >= 30 && rawPressed != *stable) *stable = rawPressed;
  return *stable;
}

static void osAllocFrameBuffer() {
  if (frameBuf) return;
  frameBuf = (uint16_t*)heap_caps_malloc(kFramePixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!frameBuf) frameBuf = (uint16_t*)heap_caps_malloc(kFramePixels * sizeof(uint16_t), MALLOC_CAP_8BIT);
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
  if (!frameBuf || w <= 0 || h <= 0) return;
  const int x0 = max(0, x);
  const int y0 = max(0, y);
  const int x1 = min(kOSW, x + w);
  const int y1 = min(kOSH, y + h);
  for (int yy = y0; yy < y1; ++yy) {
    uint16_t *row = frameBuf + yy * kOSW;
    for (int xx = x0; xx < x1; ++xx) row[xx] = c;
  }
}

static void fbCircle(int cx, int cy, int r, uint16_t c) {
  if (r <= 0) { fbPixel(cx, cy, c); return; }
  const int r2 = r * r;
  for (int y = -r; y <= r; ++y) {
    for (int x = -r; x <= r; ++x) if (x * x + y * y <= r2) fbPixel(cx + x, cy + y, c);
  }
}

static void fbPush() {
  if (frameBuf) osTft.drawRGBBitmap(0, 0, frameBuf, kOSW, kOSH);
}

static void osDrawCenteredDirect(int y, const char *text, uint16_t color, uint8_t size = 1) {
  osTft.setTextSize(size);
  osTft.setTextColor(color);
  const int textWidth = (int)strlen(text) * 6 * size;
  int x = (kOSW - textWidth) / 2;
  if (x < 0) x = 0;
  osTft.setCursor(x, y);
  osTft.print(text);
}

// ---------------- MPU6050 ----------------
static uint8_t mpuAddr = 0x68;
static bool mpuOk = false;
static bool mpuTriedInit = false;
static float ax = 0.0f, ay = 0.0f, az = 1.0f;
static float gx = 0.0f, gy = 0.0f, gz = 0.0f;
static float biasAX = 0.0f, biasAY = 0.0f;
static float lastAX = 0.0f, lastAY = 0.0f, lastAZ = 1.0f;

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

  const int16_t rawAX = (int16_t)((Wire.read() << 8) | Wire.read());
  const int16_t rawAY = (int16_t)((Wire.read() << 8) | Wire.read());
  const int16_t rawAZ = (int16_t)((Wire.read() << 8) | Wire.read());
  Wire.read(); Wire.read();
  const int16_t rawGX = (int16_t)((Wire.read() << 8) | Wire.read());
  const int16_t rawGY = (int16_t)((Wire.read() << 8) | Wire.read());
  const int16_t rawGZ = (int16_t)((Wire.read() << 8) | Wire.read());

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

  if (osCheckI2C(0x68)) { mpuAddr = 0x68; mpuOk = true; }
  else if (osCheckI2C(0x69)) { mpuAddr = 0x69; mpuOk = true; }
  else { mpuOk = false; return; }

  const uint8_t initRegs[][2] = { {0x6B, 0x00}, {0x1A, 0x03}, {0x1C, 0x00}, {0x1B, 0x00} };
  for (uint8_t i = 0; i < 4; ++i) {
    Wire.beginTransmission(mpuAddr);
    Wire.write(initRegs[i][0]);
    Wire.write(initRegs[i][1]);
    Wire.endTransmission(true);
  }

  float sumAX = 0.0f, sumAY = 0.0f;
  int valid = 0;
  for (int i = 0; i < 80; ++i) {
    if (osReadMPU()) { sumAX += ax; sumAY += ay; valid++; }
    delay(4);
  }
  if (valid > 0) { biasAX = sumAX / valid; biasAY = sumAY / valid; }
  lastAX = ax; lastAY = ay; lastAZ = az;
}

static void mapBaseVectorToScreen(float baseX, float baseY, float *outX, float *outY) {
  *outX = -baseY;
  *outY = baseX;
}

static void osEnsureHardware() {
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
  const uint16_t bg = selected ? osRGB565(90, 18, 12) : osRGB565(16, 14, 14);
  const uint16_t border = selected ? osRGB565(220, 55, 30) : osRGB565(50, 45, 45);
  const uint16_t fg = enabled ? ST77XX_WHITE : osRGB565(105, 105, 105);
  osTft.fillRoundRect(14, y, 132, 24, 5, bg);
  osTft.drawRoundRect(14, y, 132, 24, 5, border);
  osTft.setTextSize(1);
  osTft.setTextColor(fg);
  osTft.setCursor(28, y + 8);
  osTft.print(label);
  if (!enabled) { osTft.setCursor(104, y + 8); osTft.print("MISS"); }
}

static void osBootAnimation(bool doomReady) {
  if (osBootAnimPlayed) return;
  osBootAnimPlayed = true;
  for (int f = 0; f < 24; ++f) {
    const uint8_t glow = (uint8_t)(18 + f * 6);
    osTft.fillScreen(osRGB565(1, 4, 12));
    osTft.drawRoundRect(12, 20, 136, 82, 8, osRGB565(glow, glow + 24, 255));
    osDrawCenteredDirect(34, "ESP32Toy", ST77XX_WHITE, 2);
    osDrawCenteredDirect(58, doomReady ? "DOOM READY" : "WATER READY", doomReady ? ST77XX_GREEN : ST77XX_CYAN, 1);
    osTft.fillRect(28, 86, (f * 104) / 23, 4, osRGB565(55, 210, 255));
    delay(16);
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
  bool aDown = false, bDown = false, leftSWDown = false;
  osReadButtons(&aDown, &bDown, &leftSWDown);
  const bool aPressed = aDown && !aPrev;
  aPrev = aDown;

  const int rawY = analogRead(OS_LEFT_JOY_Y_PIN);
  const float y = osDeadzone(osClamp((rawY - 2048) / 1800.0f, -1.0f, 1.0f) * -1.0f, 0.14f);
  const uint32_t now = millis();
  if (now - lastNavMs > 180) {
    if (y > 0.45f && selectedItem > 0) { selectedItem--; lastNavMs = now; ESP32Toy_OSRedrawLauncher(doomReady); }
    else if (y < -0.45f && selectedItem < 1) { selectedItem++; lastNavMs = now; ESP32Toy_OSRedrawLauncher(doomReady); }
  }

  if (warningUntilMs && now > warningUntilMs) { warningUntilMs = 0; ESP32Toy_OSRedrawLauncher(doomReady); }

  if (aPressed || (leftSWDown && !leftSWPrev)) {
    leftSWPrev = leftSWDown;
    if (selectedItem == 0) return ESP32TOY_OS_ACTION_START_WATER;
    if (doomReady) return ESP32TOY_OS_ACTION_START_DOOM;
    warningUntilMs = now + 1200;
    ESP32Toy_OSRedrawLauncher(doomReady);
  }
  leftSWPrev = leftSWDown;
  (void)bDown;
  return ESP32TOY_OS_ACTION_NONE;
}

// ---------------- Fullscreen metaball liquid ----------------
static const int WATER_COUNT_MIN = 48;
static const int WATER_COUNT_MAX = 120;
static int activeWaterCount = 88;
static const float WATER_RADIUS = 4.0f;
static float wx[WATER_COUNT_MAX], wy[WATER_COUNT_MAX], wvx[WATER_COUNT_MAX], wvy[WATER_COUNT_MAX];

static const int DROP_COUNT_MAX = 18;
static bool dropActive[DROP_COUNT_MAX];
static float dropX[DROP_COUNT_MAX], dropY[DROP_COUNT_MAX], dropVX[DROP_COUNT_MAX], dropVY[DROP_COUNT_MAX], dropRadius[DROP_COUNT_MAX];
static int dropLife[DROP_COUNT_MAX];
static int splashCooldown = 0;

static const int GRID_STEP = 2;
static const int FIELD_W = kOSW / GRID_STEP + 3;
static const int FIELD_H = kOSH / GRID_STEP + 3;
static uint16_t densityField[FIELD_W * FIELD_H];
static const int KERNEL_R_MIN = 8;
static const int KERNEL_R_MAX = 12;
static const int KERNEL_MAX_SIZE = KERNEL_R_MAX * 2 + 1;
static int kernelR = 10;
static int kernelBuiltR = -1;
static uint16_t kernel[KERNEL_MAX_SIZE * KERNEL_MAX_SIZE];
static const uint16_t FIELD_THRESHOLD = 430;
static const uint16_t FOAM_THRESHOLD = 1750;

static float forceX = 0.0f, forceY = 0.0f;
static float joystickForceX = 0.0f, joystickForceY = 0.0f;
static float potFiltered = 0.0f;
static uint32_t waterFrame = 0;

static const uint16_t C_BG_WATER = 0x0005;
static const uint16_t C_WATER_DARK = 0x01B1;
static const uint16_t C_WATER_MID = 0x049F;
static const uint16_t C_WATER_BRIGHT = 0x2DFF;
static const uint16_t C_WATER_FOAM = 0xAFFF;
static const uint16_t C_EDGE = 0xB73F;
static const uint16_t C_WHITE = 0xFFFF;

static uint16_t waterDensityColor(uint16_t d, int gy) {
  const int shimmer = (int)((waterFrame + gy * 3) & 15);
  if (d > FOAM_THRESHOLD) return shimmer < 4 ? C_WATER_FOAM : C_WATER_BRIGHT;
  if (d > 1000) return shimmer < 3 ? C_WATER_BRIGHT : C_WATER_MID;
  return shimmer < 2 ? C_WATER_MID : C_WATER_DARK;
}

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
        kernel[(ky + kernelR) * kernelSize + (kx + kernelR)] = (uint16_t)(t * t * 1240.0f);
      }
    }
  }
  kernelBuiltR = kernelR;
}

static void waterReset() {
  randomSeed(micros());
  if (kernelBuiltR != kernelR) waterBuildKernel();
  const int cols = 13;
  const int rows = (WATER_COUNT_MAX + cols - 1) / cols;
  const float spacingX = 10.2f;
  const float spacingY = 6.2f;
  const float startX = (kOSW - (cols - 1) * spacingX) * 0.5f;
  const float startY = kOSH - (rows - 1) * spacingY - 7.0f;
  int id = 0;
  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < cols; ++x) {
      if (id >= WATER_COUNT_MAX) break;
      wx[id] = startX + x * spacingX + random(-1, 2);
      wy[id] = startY + y * spacingY + random(-1, 2);
      wvx[id] = 0.0f;
      wvy[id] = 0.0f;
      id++;
    }
  }
  for (int i = 0; i < DROP_COUNT_MAX; ++i) { dropActive[i] = false; dropLife[i] = 0; }
  forceX = forceY = joystickForceX = joystickForceY = 0.0f;
  splashCooldown = 0;
  waterFrame = 0;
  activeWaterCount = 88;
  if (mpuOk) { lastAX = ax; lastAY = ay; lastAZ = az; }
}

static void waterUpdateAmount() {
  const int potRaw = analogRead(OS_POT_PIN);
  potFiltered = potFiltered <= 0.1f ? potRaw : potFiltered * 0.90f + potRaw * 0.10f;
  const float t = osClamp(potFiltered / 4095.0f, 0.0f, 1.0f);
  int targetWaterCount = constrain(WATER_COUNT_MIN + (int)(t * (WATER_COUNT_MAX - WATER_COUNT_MIN)), WATER_COUNT_MIN, WATER_COUNT_MAX);
  int targetKernelR = constrain(KERNEL_R_MIN + (int)(t * (KERNEL_R_MAX - KERNEL_R_MIN) + 0.5f), KERNEL_R_MIN, KERNEL_R_MAX);
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
  const float bounce = -0.20f;
  if (wx[i] < WATER_RADIUS) { wx[i] = WATER_RADIUS; wvx[i] *= bounce; }
  if (wx[i] > kOSW - 1 - WATER_RADIUS) { wx[i] = kOSW - 1 - WATER_RADIUS; wvx[i] *= bounce; }
  if (wy[i] < WATER_RADIUS) { wy[i] = WATER_RADIUS; wvy[i] *= bounce; }
  if (wy[i] > kOSH - 1 - WATER_RADIUS) { wy[i] = kOSH - 1 - WATER_RADIUS; wvy[i] *= bounce; }
}

static void waterSolvePairs() {
  const float targetDist = WATER_RADIUS * 1.50f;
  const float targetDistSq = targetDist * targetDist;
  for (int i = 0; i < activeWaterCount; ++i) {
    for (int j = i + 1; j < activeWaterCount; ++j) {
      const float dx = wx[j] - wx[i];
      const float dy = wy[j] - wy[i];
      const float d2 = dx * dx + dy * dy;
      if (d2 < 0.0001f || d2 >= targetDistSq) continue;
      const float dist = sqrtf(d2);
      const float push = (targetDist - dist) * 0.47f;
      const float nx = dx / dist;
      const float ny = dy / dist;
      wx[i] -= nx * push; wy[i] -= ny * push;
      wx[j] += nx * push; wy[j] += ny * push;
    }
  }
}

static void waterApplyViscosity() {
  const float rangeSq = WATER_RADIUS * WATER_RADIUS * 7.0f;
  for (int i = 0; i < activeWaterCount; ++i) {
    for (int j = i + 1; j < activeWaterCount; ++j) {
      const float dx = wx[j] - wx[i];
      const float dy = wy[j] - wy[i];
      if (dx * dx + dy * dy >= rangeSq) continue;
      const float dvx = wvx[j] - wvx[i];
      const float dvy = wvy[j] - wvy[i];
      const float blend = 0.024f;
      wvx[i] += dvx * blend; wvy[i] += dvy * blend;
      wvx[j] -= dvx * blend; wvy[j] -= dvy * blend;
    }
  }
}

static void waterSpawnDrop(float x, float y, float vx, float vy, float radius) {
  for (int i = 0; i < DROP_COUNT_MAX; ++i) {
    if (!dropActive[i]) {
      dropActive[i] = true;
      dropX[i] = x; dropY[i] = y; dropVX[i] = vx; dropVY[i] = vy; dropRadius[i] = radius; dropLife[i] = 74;
      return;
    }
  }
}

static void waterEmitSplash(float fx, float fy, float strength) {
  if (activeWaterCount <= 0) return;
  float len = sqrtf(fx * fx + fy * fy);
  float upX = 0.0f, upY = -1.0f;
  if (len > 0.05f) { upX = -fx / len; upY = -fy / len; }

  int ids[WATER_COUNT_MAX];
  float scores[WATER_COUNT_MAX];
  for (int i = 0; i < activeWaterCount; ++i) { ids[i] = i; scores[i] = wx[i] * upX + wy[i] * upY; }
  const int pick = min(10, activeWaterCount);
  for (int a = 0; a < pick; ++a) {
    int best = a;
    for (int b = a + 1; b < activeWaterCount; ++b) if (scores[b] > scores[best]) best = b;
    float ts = scores[a]; scores[a] = scores[best]; scores[best] = ts;
    int ti = ids[a]; ids[a] = ids[best]; ids[best] = ti;
  }

  const int count = constrain((int)(strength * 1.55f), 4, 8);
  for (int n = 0; n < count; ++n) {
    const int id = ids[random(0, pick)];
    const float side = random(-100, 101) / 100.0f;
    const float sideX = -upY, sideY = upX;
    const float launch = 0.92f + strength * 0.44f;
    const float spread = 0.52f + strength * 0.12f;
    waterSpawnDrop(wx[id], wy[id], wvx[id] + upX * launch + sideX * side * spread,
                   wvy[id] + upY * launch + sideY * side * spread, random(1, 3));
    wvx[id] += upX * 0.16f;
    wvy[id] += upY * 0.16f;
  }
}

static void waterUpdateDrops() {
  for (int i = 0; i < DROP_COUNT_MAX; ++i) {
    if (!dropActive[i]) continue;
    dropVX[i] += forceX * 0.012f;
    dropVY[i] += 0.072f + forceY * 0.012f;
    dropVX[i] *= 0.994f; dropVY[i] *= 0.994f;
    dropX[i] += dropVX[i]; dropY[i] += dropVY[i]; dropLife[i]--;
    if (dropX[i] < 2) { dropX[i] = 2; dropVX[i] *= -0.24f; }
    if (dropX[i] > kOSW - 3) { dropX[i] = kOSW - 3; dropVX[i] *= -0.24f; }
    if (dropY[i] < 2) { dropY[i] = 2; dropVY[i] *= -0.20f; }
    for (int j = 0; j < activeWaterCount; ++j) {
      const float dx = dropX[i] - wx[j], dy = dropY[i] - wy[j];
      if (dx * dx + dy * dy < WATER_RADIUS * WATER_RADIUS * 1.85f) {
        wvx[j] += dropVX[i] * 0.045f; wvy[j] += dropVY[i] * 0.045f; dropActive[i] = false; break;
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
        const int idx = fy * FIELD_W + fx;
        const uint32_t v = densityField[idx] + add;
        densityField[idx] = v > 65535 ? 65535 : v;
      }
    }
  }
}

static void waterDrawBody() {
  fbClear(C_BG_WATER);
  waterBuildDensityField();

  for (int gy = 0; gy < FIELD_H; ++gy) {
    int runStart = -1;
    uint16_t runColor = C_WATER_DARK;
    for (int gx = 0; gx < FIELD_W; ++gx) {
      const uint16_t d = densityField[gy * FIELD_W + gx];
      const bool inside = d >= FIELD_THRESHOLD;
      if (inside && runStart < 0) { runStart = gx; runColor = waterDensityColor(d, gy); }
      const bool last = gx == FIELD_W - 1;
      if ((!inside || last) && runStart >= 0) {
        const int runEnd = (inside && last) ? gx : gx - 1;
        fbRect(runStart * GRID_STEP, gy * GRID_STEP, (runEnd - runStart + 1) * GRID_STEP, GRID_STEP, runColor);
        runStart = -1;
      }
    }
  }

  for (int gy = 1; gy < FIELD_H - 1; ++gy) {
    for (int gx = 1; gx < FIELD_W - 1; ++gx) {
      const int idx = gy * FIELD_W + gx;
      if (densityField[idx] < FIELD_THRESHOLD) continue;
      const bool topEdge = densityField[idx - FIELD_W] < FIELD_THRESHOLD;
      const bool sideEdge = densityField[idx - 1] < FIELD_THRESHOLD || densityField[idx + 1] < FIELD_THRESHOLD;
      if (topEdge) {
        fbRect(gx * GRID_STEP, gy * GRID_STEP, GRID_STEP, 1, C_EDGE);
        if (((gx * 3 + gy + waterFrame) & 15) < 2) fbPixel(gx * GRID_STEP, gy * GRID_STEP, C_WHITE);
      } else if (sideEdge && ((gy + waterFrame) & 7) == 0) {
        fbPixel(gx * GRID_STEP, gy * GRID_STEP, C_WATER_BRIGHT);
      }
      if (densityField[idx] > FOAM_THRESHOLD && ((gx + gy + waterFrame) & 11) == 0) {
        fbPixel(gx * GRID_STEP + 1, gy * GRID_STEP, C_WATER_FOAM);
      }
    }
  }

  for (int i = 0; i < DROP_COUNT_MAX; ++i) {
    if (!dropActive[i]) continue;
    fbCircle((int)dropX[i], (int)dropY[i], (int)dropRadius[i], C_EDGE);
    if (dropRadius[i] >= 2) fbPixel((int)dropX[i] - 1, (int)dropY[i] - 1, C_WHITE);
  }
  fbPush();
}

static void waterReadJoystickForce(float *outX, float *outY) {
  const int rawX = analogRead(OS_LEFT_JOY_X_PIN);
  const int rawY = analogRead(OS_LEFT_JOY_Y_PIN);
  float joyX = osClamp((rawX - 2048) / 1800.0f, -1.0f, 1.0f);
  float joyY = osClamp((rawY - 2048) / 1800.0f, -1.0f, 1.0f) * -1.0f;
  *outX = osDeadzone(joyX, 0.14f);
  *outY = osDeadzone(joyY, 0.14f);
}

static void waterStep(bool aDown, bool leftSWDown) {
  waterUpdateAmount();

  float joyX = 0.0f, joyY = 0.0f;
  waterReadJoystickForce(&joyX, &joyY);
  joystickForceX = joystickForceX * 0.80f + joyX * 0.20f;
  joystickForceY = joystickForceY * 0.80f + joyY * 0.20f;

  bool gotMPU = osReadMPU();
  float tiltX = 0.0f, tiltY = 0.0f, gyroScreenX = 0.0f, gyroScreenY = 0.0f;
  float splashStrength = 0.0f;

  if (gotMPU) {
    float baseTiltX = 1.0f * (ax - biasAX);
    float baseTiltY = -1.0f * (ay - biasAY);
    if (fabsf(baseTiltX) < 0.03f) baseTiltX = 0.0f;
    if (fabsf(baseTiltY) < 0.03f) baseTiltY = 0.0f;
    mapBaseVectorToScreen(osClamp(baseTiltX * 2.10f, -1.0f, 1.0f), osClamp(baseTiltY * 2.10f, -1.0f, 1.0f), &tiltX, &tiltY);
    mapBaseVectorToScreen(osClamp(gx * 0.008f, -1.3f, 1.3f), osClamp(gy * 0.008f, -1.3f, 1.3f), &gyroScreenX, &gyroScreenY);
    const float jerk = fabsf(ax - lastAX) + fabsf(ay - lastAY) + fabsf(az - lastAZ);
    lastAX = ax; lastAY = ay; lastAZ = az;
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
    wvx[i] *= 0.986f; wvy[i] *= 0.986f;
    wvx[i] = osClamp(wvx[i], -4.2f, 4.2f);
    wvy[i] = osClamp(wvy[i], -4.2f, 4.2f);
    wx[i] += wvx[i]; wy[i] += wvy[i];
    waterConstrainParticle(i);
  }

  waterSolvePairs(); waterSolvePairs(); waterSolvePairs();
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

  bool aDown = false, bDown = false, leftSWDown = false;
  osReadButtons(&aDown, &bDown, &leftSWDown);
  const bool bPressed = bDown && !bPrev;
  bPrev = bDown;
  if (bPressed) { waterInitialized = false; return true; }

  const uint32_t now = millis();
  if (now - lastFrameMs < 32) return false;
  lastFrameMs = now;
  waterFrame++;
  waterStep(aDown, leftSWDown);
  waterDrawBody();
  return false;
}
