#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <math.h>
#include <string.h>

// ============================================================
// ESP32Toy Liquid OS
// Board: ESP32-S3
// Display: 1.8 inch ST7735
// Sensors: MPU6050, joystick, potentiometer
// Inputs: mechanical key, joystick switch
// Output: vibration motor
// ============================================================

// ---------------- Display pins ----------------
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC    9
#define TFT_BL   21
#define TFT_RST  -1

// ---------------- Other pins ----------------
#define I2C_SDA    4
#define I2C_SCL    5
#define POT_PIN    1
#define JOY_X_PIN 16
#define JOY_Y_PIN  8
#define JOY_SW_PIN 6
#define MOTOR_PIN  7
#define KEY_PIN   15

#define MOTOR_ACTIVE_HIGH true

// ============================================================
// Screen buffers
// Runtime rotation uses one of two canvases.
// ============================================================
#define LAND_W 160
#define LAND_H 128
#define PORT_W 128
#define PORT_H 160

SPIClass screenSPI(FSPI);
Adafruit_ST7735 tft(&screenSPI, TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 canvasLandscape(LAND_W, LAND_H);
GFXcanvas16 canvasPortrait(PORT_W, PORT_H);
GFXcanvas16 *activeCanvas = &canvasLandscape;

int screenW = LAND_W;
int screenH = LAND_H;
uint8_t displayRotation = 3;  // Start with the currently tested landscape orientation.

#define CV (*activeCanvas)

// ============================================================
// Modes
// ============================================================
enum SystemMode {
  MODE_BOOT,
  MODE_MENU,
  MODE_WATER,
  MODE_SETTINGS
};

SystemMode mode = MODE_BOOT;

// ============================================================
// Generic helpers
// ============================================================
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

float applyDeadzone(float v, float deadzoneSize) {
  if (fabsf(v) <= deadzoneSize) return 0.0f;
  float signV = v >= 0.0f ? 1.0f : -1.0f;
  float t = (fabsf(v) - deadzoneSize) / (1.0f - deadzoneSize);
  return signV * clampf(t, 0.0f, 1.0f);
}

// Transform a vector from the fixed/base hardware coordinate system
// into the current screen coordinate system.
// No custom return type is used so Arduino sketch auto-prototypes stay safe.
void mapBaseVectorToScreen(float baseX, float baseY, float *outX, float *outY) {
  switch (displayRotation & 3) {
    case 0:
      *outX = baseX;
      *outY = baseY;
      break;
    case 1:
      *outX = baseY;
      *outY = -baseX;
      break;
    case 2:
      *outX = -baseX;
      *outY = -baseY;
      break;
    default: // rotation 3
      *outX = -baseY;
      *outY = baseX;
      break;
  }
}

// ============================================================
// Colors
// ============================================================
const uint16_t C_BG = rgb565(3, 7, 16);
const uint16_t C_PANEL = rgb565(10, 18, 34);
const uint16_t C_PANEL_SELECTED = rgb565(15, 27, 48);
const uint16_t C_LINE = rgb565(45, 73, 108);
const uint16_t C_WHITE = ST77XX_WHITE;
const uint16_t C_MUTED = rgb565(150, 170, 190);
const uint16_t C_BOOT = rgb565(90, 210, 255);

const uint8_t WATER_COLOR_COUNT = 7;
uint8_t waterColorIndex = 5;

uint16_t waterColors[WATER_COLOR_COUNT] = {
  rgb565(255, 60, 60),
  rgb565(255, 145, 40),
  rgb565(255, 220, 40),
  rgb565(60, 220, 90),
  rgb565(40, 220, 235),
  rgb565(55, 125, 255),
  rgb565(180, 85, 255)
};

uint16_t waterEdgeColors[WATER_COLOR_COUNT] = {
  rgb565(255, 180, 180),
  rgb565(255, 215, 155),
  rgb565(255, 245, 170),
  rgb565(180, 255, 195),
  rgb565(170, 255, 255),
  rgb565(185, 220, 255),
  rgb565(230, 190, 255)
};

uint16_t waterColor() {
  return waterColors[waterColorIndex];
}

uint16_t waterEdgeColor() {
  return waterEdgeColors[waterColorIndex];
}

// ============================================================
// Input state
// ============================================================
int potRaw = 0;
float potFiltered = 0.0f;

int joyXRaw = 2048;
int joyYRaw = 2048;
int joyCenterX = 2048;
int joyCenterY = 2048;

bool joyPressed = false;
bool joyPressedPrev = false;

bool keyRawPrev = HIGH;
bool keyStable = HIGH;
bool keyPressedEdge = false;
uint32_t keyChangedAt = 0;
const uint32_t KEY_DEBOUNCE_MS = 35;

// ============================================================
// Motor
// ============================================================
uint32_t motorUntilMs = 0;

void motorWrite(bool on) {
  if (MOTOR_ACTIVE_HIGH) {
    digitalWrite(MOTOR_PIN, on ? HIGH : LOW);
  } else {
    digitalWrite(MOTOR_PIN, on ? LOW : HIGH);
  }
}

void motorPulse(uint32_t durationMs) {
  uint32_t endTime = millis() + durationMs;
  if (endTime > motorUntilMs) motorUntilMs = endTime;
}

void updateMotor() {
  motorWrite(millis() < motorUntilMs);
}

// ============================================================
// MPU6050 state
// ============================================================
uint8_t mpuAddr = 0x68;
bool mpuOk = false;

float ax = 0.0f;
float ay = 0.0f;
float az = 1.0f;
float gx = 0.0f;
float gy = 0.0f;
float gz = 0.0f;

float biasAX = 0.0f;
float biasAY = 0.0f;
float lastAX = 0.0f;
float lastAY = 0.0f;
float lastAZ = 1.0f;

float forceX = 0.0f;
float forceY = 0.0f;
float joystickForceX = 0.0f;
float joystickForceY = 0.0f;

// Base signs tuned from the physically tested water behavior.
// Runtime screen rotation maps these into the correct screen direction.
const float BASE_TILT_X_SIGN = 1.0f;
const float BASE_TILT_Y_SIGN = -1.0f;
const float BASE_JOY_X_SIGN = 1.0f;
const float BASE_JOY_Y_SIGN = 1.0f;

bool checkI2C(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void initMPU() {
  Wire.setPins(I2C_SDA, I2C_SCL);
  Wire.begin();
  delay(80);

  if (checkI2C(0x68)) {
    mpuAddr = 0x68;
    mpuOk = true;
  } else if (checkI2C(0x69)) {
    mpuAddr = 0x69;
    mpuOk = true;
  }

  if (!mpuOk) return;

  const uint8_t initRegs[][2] = {
    {0x6B, 0x00},
    {0x1A, 0x03},
    {0x1C, 0x00},
    {0x1B, 0x00}
  };

  for (uint8_t i = 0; i < 4; i++) {
    Wire.beginTransmission(mpuAddr);
    Wire.write(initRegs[i][0]);
    Wire.write(initRegs[i][1]);
    Wire.endTransmission(true);
  }
}

bool readMPU() {
  if (!mpuOk) return false;

  Wire.beginTransmission(mpuAddr);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;

  Wire.requestFrom(mpuAddr, (uint8_t)14, (uint8_t)true);
  if (Wire.available() < 14) return false;

  int16_t rawAX = Wire.read() << 8 | Wire.read();
  int16_t rawAY = Wire.read() << 8 | Wire.read();
  int16_t rawAZ = Wire.read() << 8 | Wire.read();

  Wire.read();
  Wire.read();

  int16_t rawGX = Wire.read() << 8 | Wire.read();
  int16_t rawGY = Wire.read() << 8 | Wire.read();
  int16_t rawGZ = Wire.read() << 8 | Wire.read();

  ax = rawAX / 16384.0f;
  ay = rawAY / 16384.0f;
  az = rawAZ / 16384.0f;

  gx = rawGX / 131.0f;
  gy = rawGY / 131.0f;
  gz = rawGZ / 131.0f;

  return true;
}

void calibrateMPU() {
  float sumAX = 0.0f;
  float sumAY = 0.0f;
  int validSamples = 0;

  for (int i = 0; i < 120; i++) {
    if (readMPU()) {
      sumAX += ax;
      sumAY += ay;
      validSamples++;
    }
    delay(6);
  }

  if (validSamples > 0) {
    biasAX = sumAX / validSamples;
    biasAY = sumAY / validSamples;
  }

  lastAX = ax;
  lastAY = ay;
  lastAZ = az;
}

void calibrateJoystick() {
  long sumX = 0;
  long sumY = 0;

  for (int i = 0; i < 48; i++) {
    sumX += analogRead(JOY_X_PIN);
    sumY += analogRead(JOY_Y_PIN);
    delay(4);
  }

  joyCenterX = sumX / 48;
  joyCenterY = sumY / 48;
}

void getJoystickScreenVector(float *outX, float *outY) {
  float nx = clampf((joyXRaw - joyCenterX) / 1800.0f, -1.0f, 1.0f);
  float ny = clampf((joyYRaw - joyCenterY) / 1800.0f, -1.0f, 1.0f);

  nx = applyDeadzone(nx, 0.14f);
  ny = applyDeadzone(ny, 0.14f);

  mapBaseVectorToScreen(BASE_JOY_X_SIGN * nx, BASE_JOY_Y_SIGN * ny, outX, outY);
}

// ============================================================
// Water simulation
// ============================================================
const int WATER_COUNT_MIN = 36;
const int WATER_COUNT_MAX = 125;
int activeWaterCount = 75;

const float WATER_RADIUS = 4.1f;
float wx[WATER_COUNT_MAX];
float wy[WATER_COUNT_MAX];
float wvx[WATER_COUNT_MAX];
float wvy[WATER_COUNT_MAX];

const int DROP_COUNT_MAX = 22;
bool dropActive[DROP_COUNT_MAX];
float dropX[DROP_COUNT_MAX];
float dropY[DROP_COUNT_MAX];
float dropVX[DROP_COUNT_MAX];
float dropVY[DROP_COUNT_MAX];
float dropRadius[DROP_COUNT_MAX];
int dropLife[DROP_COUNT_MAX];
int splashCooldown = 0;

const int GRID_STEP = 2;
const int FIELD_MAX_W = LAND_W / GRID_STEP + 3;
const int FIELD_MAX_H = PORT_H / GRID_STEP + 3;
uint16_t densityField[FIELD_MAX_W * FIELD_MAX_H];
int fieldW = LAND_W / GRID_STEP + 3;
int fieldH = LAND_H / GRID_STEP + 3;

const int KERNEL_R_MIN = 8;
const int KERNEL_R_MAX = 12;
const int KERNEL_MAX_SIZE = KERNEL_R_MAX * 2 + 1;
int kernelR = 9;
int kernelBuiltR = -1;
uint16_t kernel[KERNEL_MAX_SIZE * KERNEL_MAX_SIZE];
const uint16_t FIELD_THRESHOLD = 455;

void rebuildKernel() {
  memset(kernel, 0, sizeof(kernel));

  float radiusPx = kernelR * GRID_STEP;
  float radiusSq = radiusPx * radiusPx;
  int kernelSize = kernelR * 2 + 1;

  for (int ky = -kernelR; ky <= kernelR; ky++) {
    for (int kx = -kernelR; kx <= kernelR; kx++) {
      float px = kx * GRID_STEP;
      float py = ky * GRID_STEP;
      float d2 = px * px + py * py;
      if (d2 < radiusSq) {
        float t = 1.0f - d2 / radiusSq;
        kernel[(ky + kernelR) * kernelSize + (kx + kernelR)] = (uint16_t)(t * t * 1180.0f);
      }
    }
  }

  kernelBuiltR = kernelR;
}

void initWater() {
  randomSeed(micros());

  int id = 0;
  int cols = screenW >= screenH ? 13 : 11;
  int rows = (WATER_COUNT_MAX + cols - 1) / cols;
  float spacingX = screenW >= screenH ? 10.5f : 10.0f;
  float spacingY = screenW >= screenH ? 6.5f : 7.0f;
  float startX = (screenW - (cols - 1) * spacingX) * 0.5f;
  float blockH = (rows - 1) * spacingY;
  float startY = screenH - blockH - 10.0f;

  if (startX < 7.0f) startX = 7.0f;
  if (startY < 18.0f) startY = 18.0f;

  for (int y = 0; y < rows; y++) {
    for (int x = 0; x < cols; x++) {
      if (id >= WATER_COUNT_MAX) break;
      wx[id] = startX + x * spacingX + random(-1, 2);
      wy[id] = startY + y * spacingY + random(-1, 2);
      wvx[id] = 0.0f;
      wvy[id] = 0.0f;
      id++;
    }
  }

  for (int i = 0; i < DROP_COUNT_MAX; i++) {
    dropActive[i] = false;
    dropLife[i] = 0;
  }
}

void updateWaterAmount() {
  potFiltered = potFiltered <= 0.1f ? potRaw : potFiltered * 0.90f + potRaw * 0.10f;
  float t = clampf(potFiltered / 4095.0f, 0.0f, 1.0f);

  int targetWaterCount = WATER_COUNT_MIN + (int)(t * (WATER_COUNT_MAX - WATER_COUNT_MIN));
  int targetKernelR = KERNEL_R_MIN + (int)(t * (KERNEL_R_MAX - KERNEL_R_MIN) + 0.5f);

  targetWaterCount = constrain(targetWaterCount, WATER_COUNT_MIN, WATER_COUNT_MAX);
  targetKernelR = constrain(targetKernelR, KERNEL_R_MIN, KERNEL_R_MAX);

  if (targetWaterCount > activeWaterCount) {
    for (int i = activeWaterCount; i < targetWaterCount; i++) {
      int ref = random(0, activeWaterCount > 0 ? activeWaterCount : 1);
      wx[i] = constrain(wx[ref] + random(-10, 11), WATER_RADIUS, screenW - 1 - WATER_RADIUS);
      wy[i] = constrain(wy[ref] + random(-10, 11), WATER_RADIUS, screenH - 1 - WATER_RADIUS);
      wvx[i] = wvx[ref] * 0.18f;
      wvy[i] = wvy[ref] * 0.18f;
    }
  }

  activeWaterCount = targetWaterCount;
  kernelR = targetKernelR;
  if (kernelR != kernelBuiltR) rebuildKernel();
}

void constrainWaterParticle(int i) {
  const float bounce = -0.18f;

  if (wx[i] < WATER_RADIUS) {
    wx[i] = WATER_RADIUS;
    wvx[i] *= bounce;
  }
  if (wx[i] > screenW - 1 - WATER_RADIUS) {
    wx[i] = screenW - 1 - WATER_RADIUS;
    wvx[i] *= bounce;
  }
  if (wy[i] < WATER_RADIUS) {
    wy[i] = WATER_RADIUS;
    wvy[i] *= bounce;
  }
  if (wy[i] > screenH - 1 - WATER_RADIUS) {
    wy[i] = screenH - 1 - WATER_RADIUS;
    wvy[i] *= bounce;
  }
}

void solveWaterPairs() {
  float targetDist = WATER_RADIUS * 1.52f;
  float targetDistSq = targetDist * targetDist;

  for (int i = 0; i < activeWaterCount; i++) {
    for (int j = i + 1; j < activeWaterCount; j++) {
      float dx = wx[j] - wx[i];
      float dy = wy[j] - wy[i];
      float d2 = dx * dx + dy * dy;
      if (d2 < 0.0001f || d2 >= targetDistSq) continue;

      float dist = sqrtf(d2);
      float overlap = targetDist - dist;
      float nx = dx / dist;
      float ny = dy / dist;
      float push = overlap * 0.46f;

      wx[i] -= nx * push;
      wy[i] -= ny * push;
      wx[j] += nx * push;
      wy[j] += ny * push;
    }
  }
}

void applyWaterViscosity() {
  float range = WATER_RADIUS * 2.65f;
  float rangeSq = range * range;

  for (int i = 0; i < activeWaterCount; i++) {
    for (int j = i + 1; j < activeWaterCount; j++) {
      float dx = wx[j] - wx[i];
      float dy = wy[j] - wy[i];
      float d2 = dx * dx + dy * dy;
      if (d2 >= rangeSq) continue;

      float deltaVX = wvx[j] - wvx[i];
      float deltaVY = wvy[j] - wvy[i];
      float blend = 0.022f;

      wvx[i] += deltaVX * blend;
      wvy[i] += deltaVY * blend;
      wvx[j] -= deltaVX * blend;
      wvy[j] -= deltaVY * blend;
    }
  }
}

void spawnDrop(float x, float y, float vx, float vy, float radius) {
  for (int i = 0; i < DROP_COUNT_MAX; i++) {
    if (!dropActive[i]) {
      dropActive[i] = true;
      dropX[i] = x;
      dropY[i] = y;
      dropVX[i] = vx;
      dropVY[i] = vy;
      dropRadius[i] = radius;
      dropLife[i] = 78;
      return;
    }
  }
}

void emitSplash(float splashFX, float splashFY, float strength) {
  if (activeWaterCount <= 0) return;

  motorPulse(80);

  float len = sqrtf(splashFX * splashFX + splashFY * splashFY);
  float upX = 0.0f;
  float upY = -1.0f;
  if (len > 0.05f) {
    upX = -splashFX / len;
    upY = -splashFY / len;
  }

  int surfaceIds[WATER_COUNT_MAX];
  float surfaceScores[WATER_COUNT_MAX];
  for (int i = 0; i < activeWaterCount; i++) {
    surfaceIds[i] = i;
    surfaceScores[i] = wx[i] * upX + wy[i] * upY;
  }

  int surfacePickCount = min(8, activeWaterCount);
  for (int a = 0; a < surfacePickCount; a++) {
    int best = a;
    for (int b = a + 1; b < activeWaterCount; b++) {
      if (surfaceScores[b] > surfaceScores[best]) best = b;
    }

    float tempScore = surfaceScores[a];
    surfaceScores[a] = surfaceScores[best];
    surfaceScores[best] = tempScore;

    int tempId = surfaceIds[a];
    surfaceIds[a] = surfaceIds[best];
    surfaceIds[best] = tempId;
  }

  int count = constrain((int)(strength * 1.45f), 3, 7);
  for (int n = 0; n < count; n++) {
    int id = surfaceIds[random(0, surfacePickCount)];
    float side = random(-100, 101) / 100.0f;
    float sideX = -upY;
    float sideY = upX;
    float launch = 0.90f + strength * 0.44f;
    float spread = 0.48f + strength * 0.11f;

    spawnDrop(
      wx[id],
      wy[id],
      wvx[id] + upX * launch + sideX * side * spread,
      wvy[id] + upY * launch + sideY * side * spread,
      random(1, 3)
    );

    wvx[id] += upX * 0.14f;
    wvy[id] += upY * 0.14f;
  }
}

void updateDrops() {
  for (int i = 0; i < DROP_COUNT_MAX; i++) {
    if (!dropActive[i]) continue;

    dropVX[i] += forceX * 0.012f;
    dropVY[i] += 0.070f + forceY * 0.012f;
    dropVX[i] *= 0.994f;
    dropVY[i] *= 0.994f;
    dropX[i] += dropVX[i];
    dropY[i] += dropVY[i];
    dropLife[i]--;

    if (dropX[i] < 2) {
      dropX[i] = 2;
      dropVX[i] *= -0.24f;
    }
    if (dropX[i] > screenW - 3) {
      dropX[i] = screenW - 3;
      dropVX[i] *= -0.24f;
    }
    if (dropY[i] < 2) {
      dropY[i] = 2;
      dropVY[i] *= -0.20f;
    }

    for (int j = 0; j < activeWaterCount; j++) {
      float dxLocal = dropX[i] - wx[j];
      float dyLocal = dropY[i] - wy[j];
      if (dxLocal * dxLocal + dyLocal * dyLocal < WATER_RADIUS * WATER_RADIUS * 1.85f) {
        wvx[j] += dropVX[i] * 0.045f;
        wvy[j] += dropVY[i] * 0.045f;
        dropActive[i] = false;
        break;
      }
    }

    if (dropY[i] > screenH || dropLife[i] <= 0) {
      dropActive[i] = false;
    }
  }
}

void updateJoystickWaterForce() {
  float joyMappedX = 0.0f;
  float joyMappedY = 0.0f;
  getJoystickScreenVector(&joyMappedX, &joyMappedY);

  joystickForceX = joystickForceX * 0.80f + joyMappedX * 0.20f;
  joystickForceY = joystickForceY * 0.80f + joyMappedY * 0.20f;
}

void updateWater() {
  updateWaterAmount();
  if (!readMPU()) return;

  updateJoystickWaterForce();

  float baseTiltX = BASE_TILT_X_SIGN * (ax - biasAX);
  float baseTiltY = BASE_TILT_Y_SIGN * (ay - biasAY);
  if (fabsf(baseTiltX) < 0.03f) baseTiltX = 0.0f;
  if (fabsf(baseTiltY) < 0.03f) baseTiltY = 0.0f;

  float tiltX = 0.0f;
  float tiltY = 0.0f;
  float gyroScreenX = 0.0f;
  float gyroScreenY = 0.0f;

  mapBaseVectorToScreen(
    clampf(baseTiltX * 2.10f, -1.0f, 1.0f),
    clampf(baseTiltY * 2.10f, -1.0f, 1.0f),
    &tiltX,
    &tiltY
  );

  mapBaseVectorToScreen(
    clampf(gx * 0.008f, -1.3f, 1.3f),
    clampf(gy * 0.008f, -1.3f, 1.3f),
    &gyroScreenX,
    &gyroScreenY
  );

  float targetX = tiltX + joystickForceX * 0.75f;
  float targetY = tiltY + joystickForceY * 0.75f;

  forceX = forceX * 0.84f + clampf(targetX, -1.35f, 1.35f) * 0.16f;
  forceY = forceY * 0.84f + clampf(targetY, -1.35f, 1.35f) * 0.16f;

  for (int i = 0; i < activeWaterCount; i++) {
    wvx[i] += forceX * 0.27f + gyroScreenX * 0.18f;
    wvy[i] += forceY * 0.27f + gyroScreenY * 0.18f;
    wvx[i] *= 0.986f;
    wvy[i] *= 0.986f;
    wvx[i] = clampf(wvx[i], -4.2f, 4.2f);
    wvy[i] = clampf(wvy[i], -4.2f, 4.2f);
    wx[i] += wvx[i];
    wy[i] += wvy[i];
    constrainWaterParticle(i);
  }

  solveWaterPairs();
  solveWaterPairs();
  solveWaterPairs();

  for (int i = 0; i < activeWaterCount; i++) {
    constrainWaterParticle(i);
  }

  applyWaterViscosity();

  float jerk = fabsf(ax - lastAX) + fabsf(ay - lastAY) + fabsf(az - lastAZ);
  lastAX = ax;
  lastAY = ay;
  lastAZ = az;

  float kick = fabsf(gx) * 0.0028f + fabsf(gy) * 0.0028f + fabsf(gz) * 0.0018f;
  float splashStrength = jerk * 6.5f + kick;

  if (splashCooldown > 0) splashCooldown--;
  if (splashStrength > 1.25f && splashCooldown <= 0) {
    emitSplash(forceX + gyroScreenX * 0.35f, forceY + gyroScreenY * 0.35f, splashStrength);
    splashCooldown = 9;
  }
}

// ============================================================
// Drawing
// ============================================================
void pushCanvas() {
  tft.drawRGBBitmap(0, 0, activeCanvas->getBuffer(), screenW, screenH);
}

void drawCenteredText(const char *text, int y, uint8_t size, uint16_t color) {
  CV.setTextSize(size);
  CV.setTextColor(color);
  CV.setTextWrap(false);

  int16_t x1, y1;
  uint16_t w, h;
  CV.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  int x = (screenW - (int)w) / 2;
  if (x < 0) x = 0;

  CV.setCursor(x, y);
  CV.print(text);
}

void drawPanel(int x, int y, int w, int h, uint16_t fill, uint16_t border) {
  CV.fillRoundRect(x, y, w, h, 5, fill);
  CV.drawRoundRect(x, y, w, h, 5, border);
}

void buildDensityField() {
  memset(densityField, 0, sizeof(densityField));
  int kernelSize = kernelR * 2 + 1;

  for (int i = 0; i < activeWaterCount; i++) {
    int centerX = wx[i] / GRID_STEP;
    int centerY = wy[i] / GRID_STEP;

    for (int ky = -kernelR; ky <= kernelR; ky++) {
      int fy = centerY + ky;
      if (fy < 0 || fy >= fieldH) continue;

      for (int kx = -kernelR; kx <= kernelR; kx++) {
        int fx = centerX + kx;
        if (fx < 0 || fx >= fieldW) continue;

        uint16_t add = kernel[(ky + kernelR) * kernelSize + (kx + kernelR)];
        if (!add) continue;

        int fieldIndex = fy * fieldW + fx;
        uint32_t value = densityField[fieldIndex] + add;
        densityField[fieldIndex] = value > 65535 ? 65535 : value;
      }
    }
  }
}

void drawWaterBody() {
  buildDensityField();

  for (int gyIndex = 0; gyIndex < fieldH; gyIndex++) {
    int runStart = -1;

    for (int gxIndex = 0; gxIndex < fieldW; gxIndex++) {
      bool inside = densityField[gyIndex * fieldW + gxIndex] >= FIELD_THRESHOLD;
      if (inside && runStart < 0) runStart = gxIndex;

      bool last = gxIndex == fieldW - 1;
      if ((!inside || last) && runStart >= 0) {
        int runEnd = (inside && last) ? gxIndex : gxIndex - 1;
        CV.fillRect(
          runStart * GRID_STEP,
          gyIndex * GRID_STEP,
          (runEnd - runStart + 1) * GRID_STEP,
          GRID_STEP,
          waterColor()
        );
        runStart = -1;
      }
    }
  }

  for (int gyIndex = 0; gyIndex < fieldH; gyIndex++) {
    for (int gxIndex = 0; gxIndex < fieldW; gxIndex++) {
      int fieldIndex = gyIndex * fieldW + gxIndex;
      if (densityField[fieldIndex] < FIELD_THRESHOLD) continue;

      bool topEdge = gyIndex == 0 || densityField[(gyIndex - 1) * fieldW + gxIndex] < FIELD_THRESHOLD;
      if (topEdge) {
        CV.fillRect(gxIndex * GRID_STEP, gyIndex * GRID_STEP, GRID_STEP, 1, waterEdgeColor());
      }
    }
  }
}

void drawDrops() {
  for (int i = 0; i < DROP_COUNT_MAX; i++) {
    if (!dropActive[i]) continue;

    CV.fillCircle((int)dropX[i], (int)dropY[i], (int)dropRadius[i], waterEdgeColor());
    if (dropRadius[i] >= 2) {
      CV.drawPixel((int)dropX[i] - 1, (int)dropY[i] - 1, C_WHITE);
    }
  }
}

void drawWaterUI() {
  CV.setTextSize(1);
  CV.setTextWrap(false);
  CV.setTextColor(C_MUTED);
  CV.setCursor(5, 6);
  CV.print("Water");
  CV.setCursor(5, screenH - 12);
  CV.print("JoySW: Back");
}

void drawWaterFrame() {
  CV.fillScreen(C_BG);
  drawWaterBody();
  drawDrops();
  drawWaterUI();
  pushCanvas();
}

// ============================================================
// Menu / Settings
// ============================================================
const uint8_t MENU_COUNT = 2;
const char *menuItems[MENU_COUNT] = {"Water", "Settings"};
const char *menuSubtitles[MENU_COUNT] = {"Interactive liquid toy", "Display + controls"};
int menuIndex = 0;
uint32_t nextMenuMoveMs = 0;
uint32_t nextSettingMoveMs = 0;

const char *rotationName(uint8_t rotation) {
  switch (rotation & 3) {
    case 0: return "0 Portrait";
    case 1: return "1 Landscape";
    case 2: return "2 Portrait Flip";
    default: return "3 Landscape Flip";
  }
}

void drawMenu() {
  CV.fillScreen(C_BG);
  drawCenteredText("ESP32Toy", screenH >= 150 ? 10 : 8, 2, C_WHITE);
  drawCenteredText("Mini System", screenH >= 150 ? 31 : 28, 1, C_MUTED);

  int topY = screenH >= 150 ? 58 : 46;
  int boxH = screenH >= 150 ? 24 : 22;
  int gap = screenH >= 150 ? 30 : 28;
  int boxW = screenW - 24;
  int boxX = 12;

  for (int i = 0; i < MENU_COUNT; i++) {
    int y = topY + i * gap;
    bool selected = i == menuIndex;
    drawPanel(boxX, y, boxW, boxH, selected ? C_PANEL_SELECTED : C_PANEL, selected ? waterColor() : C_LINE);
    CV.setTextSize(1);
    CV.setTextColor(selected ? C_WHITE : C_MUTED);
    CV.setCursor(boxX + 10, y + boxH / 2 - 3);
    CV.print(menuItems[i]);
    if (selected) CV.fillCircle(boxX + boxW - 12, y + boxH / 2, 3, waterColor());
  }

  drawCenteredText(menuSubtitles[menuIndex], screenH - 12, 1, C_MUTED);
  pushCanvas();
}

void updateMenu() {
  uint32_t now = millis();
  if (now < nextMenuMoveMs) return;

  float joyScreenX = 0.0f;
  float joyScreenY = 0.0f;
  getJoystickScreenVector(&joyScreenX, &joyScreenY);

  if (joyScreenY > 0.62f) {
    menuIndex = (menuIndex + 1) % MENU_COUNT;
    nextMenuMoveMs = now + 220;
    motorPulse(45);
  } else if (joyScreenY < -0.62f) {
    menuIndex = (menuIndex + MENU_COUNT - 1) % MENU_COUNT;
    nextMenuMoveMs = now + 220;
    motorPulse(45);
  }
}

void drawSettings() {
  CV.fillScreen(C_BG);
  drawCenteredText("Settings", screenH >= 150 ? 10 : 8, 2, C_WHITE);
  drawCenteredText("Screen rotation", screenH >= 150 ? 36 : 30, 1, C_MUTED);

  int boxW = screenW - 18;
  int boxX = 9;
  int boxY = screenH >= 150 ? 62 : 52;
  int boxH = 34;
  drawPanel(boxX, boxY, boxW, boxH, C_PANEL_SELECTED, waterColor());
  drawCenteredText(rotationName(displayRotation), boxY + 11, 1, C_WHITE);
  drawCenteredText("< joystick >", boxY + boxH + 16, 1, C_MUTED);
  drawCenteredText("Key: rotate   JoySW: back", screenH - 12, 1, C_MUTED);
  pushCanvas();
}

void applyDisplayRotation(uint8_t rotation, bool resetWater) {
  displayRotation = rotation & 3;
  tft.setRotation(displayRotation);

  if (displayRotation == 0 || displayRotation == 2) {
    activeCanvas = &canvasPortrait;
    screenW = PORT_W;
    screenH = PORT_H;
  } else {
    activeCanvas = &canvasLandscape;
    screenW = LAND_W;
    screenH = LAND_H;
  }

  fieldW = screenW / GRID_STEP + 3;
  fieldH = screenH / GRID_STEP + 3;

  CV.fillScreen(C_BG);
  pushCanvas();

  if (resetWater) {
    forceX = 0.0f;
    forceY = 0.0f;
    joystickForceX = 0.0f;
    joystickForceY = 0.0f;
    initWater();
  }
}

void rotateDisplayBy(int delta) {
  int nextRotation = ((int)displayRotation + delta) % 4;
  if (nextRotation < 0) nextRotation += 4;
  applyDisplayRotation((uint8_t)nextRotation, true);
  motorPulse(90);
}

void updateSettings() {
  uint32_t now = millis();

  float joyScreenX = 0.0f;
  float joyScreenY = 0.0f;
  getJoystickScreenVector(&joyScreenX, &joyScreenY);

  if (now >= nextSettingMoveMs) {
    if (joyScreenX > 0.62f) {
      rotateDisplayBy(1);
      nextSettingMoveMs = now + 320;
    } else if (joyScreenX < -0.62f) {
      rotateDisplayBy(-1);
      nextSettingMoveMs = now + 320;
    }
  }

  if (keyPressedEdge) {
    rotateDisplayBy(1);
    nextSettingMoveMs = now + 320;
  }

  if (joyPressed && !joyPressedPrev) {
    mode = MODE_MENU;
    motorPulse(120);
  }

  joyPressedPrev = joyPressed;
}

// ============================================================
// Boot animation
// ============================================================
void drawBootFrame(float t) {
  CV.fillScreen(C_BG);

  float ease = t * t * (3.0f - 2.0f * t);
  int centerX = screenW / 2;
  int centerY = screenH / 2;

  CV.drawCircle(centerX, centerY, 18, rgb565(20, 45, 70));
  CV.drawCircle(centerX, centerY, 26, rgb565(10, 30, 52));

  CV.fillCircle((int)(screenW * 0.20f + (centerX - screenW * 0.20f) * ease), (int)(-8 + (centerY - 10 + 8) * ease), 6, rgb565(45, 160, 255));
  CV.fillCircle(centerX, (int)(-16 + (centerY - 16 + 16) * ease), 7, rgb565(65, 190, 255));
  CV.fillCircle((int)(screenW * 0.82f + (centerX - screenW * 0.82f) * ease), (int)(-6 + (centerY - 6 + 6) * ease), 6, rgb565(90, 220, 255));

  if (t > 0.52f) {
    float q = clampf((t - 0.52f) / 0.48f, 0.0f, 1.0f);
    int radius = 4 + q * 16;
    CV.fillCircle(centerX, centerY, radius, C_BOOT);
    CV.drawCircle(centerX, centerY, radius + 2, rgb565(170, 235, 255));
  }

  if (t > 0.73f) {
    drawCenteredText("ESP32 TOY", screenH - 24, 1, C_WHITE);
  }

  pushCanvas();
}

void runBootAnimation() {
  uint32_t startMs = millis();
  while (millis() - startMs < 1800) {
    drawBootFrame((millis() - startMs) / 1800.0f);
    delay(20);
  }
  drawBootFrame(1.0f);
  delay(140);
}

void drawBootText(const char *text) {
  CV.fillScreen(C_BG);
  drawCenteredText(text, screenH / 2 - 4, 1, C_WHITE);
  pushCanvas();
}

// ============================================================
// Input scan and app controls
// ============================================================
void updateInputs() {
  joyXRaw = analogRead(JOY_X_PIN);
  joyYRaw = analogRead(JOY_Y_PIN);
  joyPressed = digitalRead(JOY_SW_PIN) == LOW;
  potRaw = analogRead(POT_PIN);

  bool raw = digitalRead(KEY_PIN);
  uint32_t now = millis();
  keyPressedEdge = false;

  if (raw != keyRawPrev) {
    keyRawPrev = raw;
    keyChangedAt = now;
  }

  if (now - keyChangedAt >= KEY_DEBOUNCE_MS && raw != keyStable) {
    keyStable = raw;
    if (keyStable == LOW) keyPressedEdge = true;
  }
}

void nextWaterColor() {
  waterColorIndex = (waterColorIndex + 1) % WATER_COLOR_COUNT;
  motorPulse(150);
}

void handleWaterControls() {
  if (keyPressedEdge) nextWaterColor();

  if (joyPressed && !joyPressedPrev) {
    mode = MODE_MENU;
    motorPulse(120);
  }

  joyPressedPrev = joyPressed;
}

// ============================================================
// Setup / loop
// ============================================================
uint32_t lastFrameMs = 0;

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  pinMode(POT_PIN, INPUT);
  pinMode(JOY_X_PIN, INPUT);
  pinMode(JOY_Y_PIN, INPUT);
  pinMode(JOY_SW_PIN, INPUT_PULLUP);
  pinMode(KEY_PIN, INPUT_PULLUP);
  pinMode(MOTOR_PIN, OUTPUT);
  motorWrite(false);

  screenSPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  applyDisplayRotation(displayRotation, false);

  runBootAnimation();
  drawBootText("CALIBRATING");
  initMPU();
  calibrateMPU();
  calibrateJoystick();

  potRaw = analogRead(POT_PIN);
  potFiltered = potRaw;
  updateWaterAmount();
  rebuildKernel();
  initWater();

  mode = MODE_MENU;
  drawMenu();
}

void loop() {
  updateInputs();
  updateMotor();
  uint32_t now = millis();

  if (mode == MODE_MENU) {
    updateMenu();

    if (keyPressedEdge) {
      if (menuIndex == 0) {
        mode = MODE_WATER;
        motorPulse(90);
      } else {
        mode = MODE_SETTINGS;
        motorPulse(90);
      }
    }

    if (now - lastFrameMs >= 20) {
      drawMenu();
      lastFrameMs = now;
    }

    joyPressedPrev = joyPressed;
    return;
  }

  if (mode == MODE_SETTINGS) {
    updateSettings();

    if (now - lastFrameMs >= 20) {
      drawSettings();
      lastFrameMs = now;
    }
    return;
  }

  if (mode == MODE_WATER) {
    handleWaterControls();
    updateWater();
    updateDrops();

    if (now - lastFrameMs >= 20) {
      drawWaterFrame();
      lastFrameMs = now;
    }
    return;
  }
}
