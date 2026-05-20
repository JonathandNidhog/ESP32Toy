#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <math.h>
#include <string.h>

// ============================================================
// ESP32Toy Liquid OS
// Hardware: ESP32-S3 + ST7735 1.8" + MPU6050 + joystick
// Inputs: potentiometer, joystick SW, mechanical key
// Output: vibration motor
// ============================================================

#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC    9
#define TFT_BL   21
#define TFT_RST  -1

#define I2C_SDA   4
#define I2C_SCL   5
#define POT_PIN   1
#define JOY_X_PIN 16
#define JOY_Y_PIN 8
#define JOY_SW_PIN 6
#define MOTOR_PIN 7
#define KEY_PIN 15
#define MOTOR_ACTIVE_HIGH true

// ============================================================
// Display canvases
// The screen can rotate at runtime, so both landscape and
// portrait offscreen buffers are kept ready.
// ============================================================
#define LAND_W 160
#define LAND_H 128
#define PORT_W 128
#define PORT_H 160

SPIClass screenSPI(FSPI);
Adafruit_ST7735 tft(&screenSPI, TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 canvasLandscape(LAND_W, LAND_H);
GFXcanvas16 canvasPortrait(PORT_W, PORT_H);
GFXcanvas16 *canvas = &canvasLandscape;

int screenW = LAND_W;
int screenH = LAND_H;
uint8_t displayRotation = 3;  // Current preferred orientation from the previous build.

#define CV (*canvas)

// ============================================================
// System modes
// ============================================================
enum AppMode { BOOT, MENU, WATER, SETTINGS };
AppMode mode = BOOT;

// ============================================================
// Colors / math
// ============================================================
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

float clampf(float v, float a, float b) {
  return v < a ? a : (v > b ? b : v);
}

float deadzone(float v, float d) {
  if (fabsf(v) <= d) return 0.0f;
  float s = v >= 0.0f ? 1.0f : -1.0f;
  return s * clampf((fabsf(v) - d) / (1.0f - d), 0.0f, 1.0f);
}

struct Vec2 {
  float x;
  float y;
};

const uint16_t BG = rgb565(3, 7, 16);
const uint16_t PANEL = rgb565(10, 18, 34);
const uint16_t PANEL2 = rgb565(15, 27, 48);
const uint16_t LINE = rgb565(45, 73, 108);
const uint16_t WHITE = ST77XX_WHITE;
const uint16_t MUTED = rgb565(150, 170, 190);
const uint16_t ACCENT = rgb565(90, 210, 255);

const uint8_t COLOR_COUNT = 7;
uint8_t waterColorIndex = 5;
uint16_t waterColors[COLOR_COUNT] = {
  rgb565(255, 60, 60), rgb565(255, 145, 40), rgb565(255, 220, 40),
  rgb565(60, 220, 90), rgb565(40, 220, 235), rgb565(55, 125, 255),
  rgb565(180, 85, 255)
};
uint16_t edgeColors[COLOR_COUNT] = {
  rgb565(255, 180, 180), rgb565(255, 215, 155), rgb565(255, 245, 170),
  rgb565(180, 255, 195), rgb565(170, 255, 255), rgb565(185, 220, 255),
  rgb565(230, 190, 255)
};
uint16_t waterColor() { return waterColors[waterColorIndex]; }
uint16_t edgeColor() { return edgeColors[waterColorIndex]; }

// ============================================================
// Inputs
// ============================================================
int potRaw = 0;
float potFiltered = 0.0f;
int joyXRaw = 2048, joyYRaw = 2048;
int joyCenterX = 2048, joyCenterY = 2048;
bool joyPressed = false, joyPressedPrev = false;

bool keyRawPrev = HIGH, keyStable = HIGH, keyPressedEdge = false;
uint32_t keyChangedAt = 0;
const uint32_t KEY_DEBOUNCE = 35;

// ============================================================
// Motor
// ============================================================
uint32_t motorUntil = 0;
void motorWrite(bool on) {
  digitalWrite(MOTOR_PIN, MOTOR_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH));
}
void motorPulse(uint32_t ms) {
  uint32_t t = millis() + ms;
  if (t > motorUntil) motorUntil = t;
}
void updateMotor() {
  motorWrite(millis() < motorUntil);
}

// ============================================================
// MPU6050
// Base-vector convention:
// rotation 0 is the calibration reference.
// All display rotations map this same base vector into the
// current screen coordinate system, so water stays correct
// after changing screen orientation in Settings.
// ============================================================
uint8_t mpuAddr = 0x68;
bool mpuOk = false;
float ax = 0, ay = 0, az = 1, gx = 0, gy = 0, gz = 0;
float biasAX = 0, biasAY = 0;
float lastAX = 0, lastAY = 0, lastAZ = 1;
float forceX = 0, forceY = 0;
float joyForceX = 0, joyForceY = 0;

const float BASE_TILT_X_SIGN = 1.0f;
const float BASE_TILT_Y_SIGN = -1.0f;
const float BASE_JOY_X_SIGN = 1.0f;
const float BASE_JOY_Y_SIGN = 1.0f;

Vec2 rotateBaseVectorToScreen(float bx, float by) {
  Vec2 out;
  switch (displayRotation & 3) {
    case 0: out.x = bx;  out.y = by;  break;
    case 1: out.x = by;  out.y = -bx; break;
    case 2: out.x = -bx; out.y = -by; break;
    default: out.x = -by; out.y = bx; break;  // rotation 3
  }
  return out;
}

bool checkI2C(uint8_t a) {
  Wire.beginTransmission(a);
  return Wire.endTransmission() == 0;
}

void initMPU() {
  Wire.setPins(I2C_SDA, I2C_SCL);
  Wire.begin();
  delay(80);

  if (checkI2C(0x68)) { mpuAddr = 0x68; mpuOk = true; }
  else if (checkI2C(0x69)) { mpuAddr = 0x69; mpuOk = true; }

  if (!mpuOk) return;

  const uint8_t regs[][2] = {{0x6B,0x00},{0x1A,0x03},{0x1C,0x00},{0x1B,0x00}};
  for (uint8_t i = 0; i < 4; i++) {
    Wire.beginTransmission(mpuAddr);
    Wire.write(regs[i][0]);
    Wire.write(regs[i][1]);
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

  int16_t rax = Wire.read() << 8 | Wire.read();
  int16_t ray = Wire.read() << 8 | Wire.read();
  int16_t raz = Wire.read() << 8 | Wire.read();
  Wire.read(); Wire.read();
  int16_t rgx = Wire.read() << 8 | Wire.read();
  int16_t rgy = Wire.read() << 8 | Wire.read();
  int16_t rgz = Wire.read() << 8 | Wire.read();

  ax = rax / 16384.0f; ay = ray / 16384.0f; az = raz / 16384.0f;
  gx = rgx / 131.0f; gy = rgy / 131.0f; gz = rgz / 131.0f;
  return true;
}

void calibrateMPU() {
  float sx = 0, sy = 0;
  int n = 0;
  for (int i = 0; i < 120; i++) {
    if (readMPU()) { sx += ax; sy += ay; n++; }
    delay(6);
  }
  if (n > 0) { biasAX = sx / n; biasAY = sy / n; }
  lastAX = ax; lastAY = ay; lastAZ = az;
}

void calibrateJoystick() {
  long sx = 0, sy = 0;
  for (int i = 0; i < 48; i++) {
    sx += analogRead(JOY_X_PIN);
    sy += analogRead(JOY_Y_PIN);
    delay(4);
  }
  joyCenterX = sx / 48;
  joyCenterY = sy / 48;
}

Vec2 readJoystickVectorToScreen() {
  float nx = deadzone(clampf((joyXRaw - joyCenterX) / 1800.0f, -1.0f, 1.0f), 0.14f);
  float ny = deadzone(clampf((joyYRaw - joyCenterY) / 1800.0f, -1.0f, 1.0f), 0.14f);
  return rotateBaseVectorToScreen(BASE_JOY_X_SIGN * nx, BASE_JOY_Y_SIGN * ny);
}

// ============================================================
// Water simulation
// ============================================================
const int WATER_MIN = 36;
const int WATER_MAX = 125;
int activeWater = 75;
const float WR = 4.1f;
float wx[WATER_MAX], wy[WATER_MAX], wvx[WATER_MAX], wvy[WATER_MAX];

const int DROP_MAX = 22;
bool dropOn[DROP_MAX];
float dx[DROP_MAX], dy[DROP_MAX], dvx[DROP_MAX], dvy[DROP_MAX], dr[DROP_MAX];
int dlife[DROP_MAX];
int splashCooldown = 0;

const int GRID_STEP = 2;
const int FIELD_MAX_W = LAND_W / GRID_STEP + 3;
const int FIELD_MAX_H = PORT_H / GRID_STEP + 3;
uint16_t field[FIELD_MAX_W * FIELD_MAX_H];
int fieldW = LAND_W / GRID_STEP + 3;
int fieldH = LAND_H / GRID_STEP + 3;

const int KERNEL_MIN = 8;
const int KERNEL_MAX = 12;
const int KERNEL_MAX_SIZE = KERNEL_MAX * 2 + 1;
int kernelR = 9, kernelBuiltR = -1;
uint16_t kernel[KERNEL_MAX_SIZE * KERNEL_MAX_SIZE];
const uint16_t FIELD_THRESHOLD = 455;

void rebuildKernel() {
  memset(kernel, 0, sizeof(kernel));
  float rp = kernelR * GRID_STEP;
  float rs = rp * rp;
  int ks = kernelR * 2 + 1;

  for (int y = -kernelR; y <= kernelR; y++) {
    for (int x = -kernelR; x <= kernelR; x++) {
      float px = x * GRID_STEP;
      float py = y * GRID_STEP;
      float d2 = px * px + py * py;
      if (d2 < rs) {
        float t = 1.0f - d2 / rs;
        kernel[(y + kernelR) * ks + (x + kernelR)] = (uint16_t)(t * t * 1180.0f);
      }
    }
  }
  kernelBuiltR = kernelR;
}

void initWater() {
  randomSeed(micros());
  int id = 0;

  int cols = screenW >= screenH ? 13 : 11;
  int rows = (WATER_MAX + cols - 1) / cols;
  float spacingX = screenW >= screenH ? 10.5f : 10.0f;
  float spacingY = screenW >= screenH ? 6.5f : 7.0f;
  float startX = (screenW - (cols - 1) * spacingX) * 0.5f;
  if (startX < 7.0f) startX = 7.0f;
  float blockH = (rows - 1) * spacingY;
  float startY = screenH - blockH - 10.0f;
  if (startY < 18.0f) startY = 18.0f;

  for (int y = 0; y < rows; y++) {
    for (int x = 0; x < cols; x++) {
      if (id >= WATER_MAX) break;
      wx[id] = startX + x * spacingX + random(-1, 2);
      wy[id] = startY + y * spacingY + random(-1, 2);
      wvx[id] = 0;
      wvy[id] = 0;
      id++;
    }
  }

  for (int i = 0; i < DROP_MAX; i++) dropOn[i] = false;
}

void updateWaterAmount() {
  potFiltered = potFiltered <= 0.1f ? potRaw : potFiltered * 0.90f + potRaw * 0.10f;
  float t = clampf(potFiltered / 4095.0f, 0, 1);
  int targetN = WATER_MIN + (int)(t * (WATER_MAX - WATER_MIN));
  int targetK = KERNEL_MIN + (int)(t * (KERNEL_MAX - KERNEL_MIN) + 0.5f);
  targetN = constrain(targetN, WATER_MIN, WATER_MAX);
  targetK = constrain(targetK, KERNEL_MIN, KERNEL_MAX);

  if (targetN > activeWater) {
    for (int i = activeWater; i < targetN; i++) {
      int ref = random(0, activeWater > 0 ? activeWater : 1);
      wx[i] = constrain(wx[ref] + random(-10, 11), WR, screenW - 1 - WR);
      wy[i] = constrain(wy[ref] + random(-10, 11), WR, screenH - 1 - WR);
      wvx[i] = wvx[ref] * 0.18f;
      wvy[i] = wvy[ref] * 0.18f;
    }
  }

  activeWater = targetN;
  kernelR = targetK;
  if (kernelR != kernelBuiltR) rebuildKernel();
}

void constrainWater(int i) {
  const float b = -0.18f;
  if (wx[i] < WR) { wx[i] = WR; wvx[i] *= b; }
  if (wx[i] > screenW - 1 - WR) { wx[i] = screenW - 1 - WR; wvx[i] *= b; }
  if (wy[i] < WR) { wy[i] = WR; wvy[i] *= b; }
  if (wy[i] > screenH - 1 - WR) { wy[i] = screenH - 1 - WR; wvy[i] *= b; }
}

void solveWaterPairs() {
  float td = WR * 1.52f;
  float td2 = td * td;
  for (int i = 0; i < activeWater; i++) {
    for (int j = i + 1; j < activeWater; j++) {
      float px = wx[j] - wx[i];
      float py = wy[j] - wy[i];
      float d2 = px * px + py * py;
      if (d2 < 0.0001f || d2 >= td2) continue;
      float d = sqrtf(d2);
      float overlap = td - d;
      float nx = px / d;
      float ny = py / d;
      float push = overlap * 0.46f;
      wx[i] -= nx * push; wy[i] -= ny * push;
      wx[j] += nx * push; wy[j] += ny * push;
    }
  }
}

void viscosity() {
  float r = WR * 2.65f;
  float r2 = r * r;
  for (int i = 0; i < activeWater; i++) {
    for (int j = i + 1; j < activeWater; j++) {
      float px = wx[j] - wx[i];
      float py = wy[j] - wy[i];
      float d2 = px * px + py * py;
      if (d2 >= r2) continue;
      float vx = wvx[j] - wvx[i];
      float vy = wvy[j] - wvy[i];
      float k = 0.022f;
      wvx[i] += vx * k; wvy[i] += vy * k;
      wvx[j] -= vx * k; wvy[j] -= vy * k;
    }
  }
}

void spawnDrop(float x, float y, float vx, float vy, float r) {
  for (int i = 0; i < DROP_MAX; i++) {
    if (!dropOn[i]) {
      dropOn[i] = true;
      dx[i] = x; dy[i] = y;
      dvx[i] = vx; dvy[i] = vy;
      dr[i] = r;
      dlife[i] = 78;
      return;
    }
  }
}

void splash(float fx, float fy, float strength) {
  if (activeWater <= 0) return;
  motorPulse(80);

  float len = sqrtf(fx * fx + fy * fy);
  float ux = 0, uy = -1;
  if (len > 0.05f) { ux = -fx / len; uy = -fy / len; }

  int ids[WATER_MAX];
  float scores[WATER_MAX];
  for (int i = 0; i < activeWater; i++) {
    ids[i] = i;
    scores[i] = wx[i] * ux + wy[i] * uy;
  }

  int top = min(8, activeWater);
  for (int a = 0; a < top; a++) {
    int best = a;
    for (int b = a + 1; b < activeWater; b++) if (scores[b] > scores[best]) best = b;
    float sv = scores[a]; scores[a] = scores[best]; scores[best] = sv;
    int iv = ids[a]; ids[a] = ids[best]; ids[best] = iv;
  }

  int count = constrain((int)(strength * 1.45f), 3, 7);
  for (int n = 0; n < count; n++) {
    int id = ids[random(0, top)];
    float side = random(-100, 101) / 100.0f;
    float sx = -uy;
    float sy = ux;
    float launch = 0.90f + strength * 0.44f;
    float spread = 0.48f + strength * 0.11f;
    spawnDrop(
      wx[id], wy[id],
      wvx[id] + ux * launch + sx * side * spread,
      wvy[id] + uy * launch + sy * side * spread,
      random(1, 3)
    );
    wvx[id] += ux * 0.14f;
    wvy[id] += uy * 0.14f;
  }
}

void updateDrops() {
  for (int i = 0; i < DROP_MAX; i++) {
    if (!dropOn[i]) continue;
    dvx[i] += forceX * 0.012f;
    dvy[i] += 0.070f + forceY * 0.012f;
    dvx[i] *= 0.994f;
    dvy[i] *= 0.994f;
    dx[i] += dvx[i];
    dy[i] += dvy[i];
    dlife[i]--;

    if (dx[i] < 2) { dx[i] = 2; dvx[i] *= -0.24f; }
    if (dx[i] > screenW - 3) { dx[i] = screenW - 3; dvx[i] *= -0.24f; }
    if (dy[i] < 2) { dy[i] = 2; dvy[i] *= -0.20f; }

    for (int j = 0; j < activeWater; j++) {
      float px = dx[i] - wx[j];
      float py = dy[i] - wy[j];
      if (px * px + py * py < WR * WR * 1.85f) {
        wvx[j] += dvx[i] * 0.045f;
        wvy[j] += dvy[i] * 0.045f;
        dropOn[i] = false;
        break;
      }
    }

    if (dy[i] > screenH || dlife[i] <= 0) dropOn[i] = false;
  }
}

void updateJoystickWaterForce() {
  Vec2 j = readJoystickVectorToScreen();
  joyForceX = joyForceX * 0.80f + j.x * 0.20f;
  joyForceY = joyForceY * 0.80f + j.y * 0.20f;
}

void updateWater() {
  updateWaterAmount();
  if (!readMPU()) return;
  updateJoystickWaterForce();

  float bx = BASE_TILT_X_SIGN * (ax - biasAX);
  float by = BASE_TILT_Y_SIGN * (ay - biasAY);
  if (fabsf(bx) < 0.03f) bx = 0;
  if (fabsf(by) < 0.03f) by = 0;

  Vec2 tilt = rotateBaseVectorToScreen(clampf(bx * 2.10f, -1.0f, 1.0f), clampf(by * 2.10f, -1.0f, 1.0f));
  Vec2 gyro = rotateBaseVectorToScreen(clampf(gx * 0.008f, -1.3f, 1.3f), clampf(gy * 0.008f, -1.3f, 1.3f));

  float targetX = tilt.x + joyForceX * 0.75f;
  float targetY = tilt.y + joyForceY * 0.75f;
  forceX = forceX * 0.84f + clampf(targetX, -1.35f, 1.35f) * 0.16f;
  forceY = forceY * 0.84f + clampf(targetY, -1.35f, 1.35f) * 0.16f;

  for (int i = 0; i < activeWater; i++) {
    wvx[i] += forceX * 0.27f + gyro.x * 0.18f;
    wvy[i] += forceY * 0.27f + gyro.y * 0.18f;
    wvx[i] *= 0.986f;
    wvy[i] *= 0.986f;
    wvx[i] = clampf(wvx[i], -4.2f, 4.2f);
    wvy[i] = clampf(wvy[i], -4.2f, 4.2f);
    wx[i] += wvx[i];
    wy[i] += wvy[i];
    constrainWater(i);
  }

  solveWaterPairs();
  solveWaterPairs();
  solveWaterPairs();
  for (int i = 0; i < activeWater; i++) constrainWater(i);
  viscosity();

  float jerk = fabsf(ax - lastAX) + fabsf(ay - lastAY) + fabsf(az - lastAZ);
  lastAX = ax; lastAY = ay; lastAZ = az;
  float kick = fabsf(gx) * 0.0028f + fabsf(gy) * 0.0028f + fabsf(gz) * 0.0018f;
  float s = jerk * 6.5f + kick;
  if (splashCooldown > 0) splashCooldown--;
  if (s > 1.25f && splashCooldown <= 0) {
    splash(forceX + gyro.x * 0.35f, forceY + gyro.y * 0.35f, s);
    splashCooldown = 9;
  }
}

// ============================================================
// Drawing helpers
// ============================================================
void pushCanvas() {
  tft.drawRGBBitmap(0, 0, canvas->getBuffer(), screenW, screenH);
}

void centeredText(const char *s, int y, uint8_t size, uint16_t color) {
  CV.setTextSize(size);
  CV.setTextColor(color);
  CV.setTextWrap(false);
  int16_t x1, y1;
  uint16_t w, h;
  CV.getTextBounds(s, 0, y, &x1, &y1, &w, &h);
  int x = (screenW - (int)w) / 2;
  if (x < 0) x = 0;
  CV.setCursor(x, y);
  CV.print(s);
}

void panel(int x, int y, int w, int h, uint16_t fill, uint16_t border) {
  CV.fillRoundRect(x, y, w, h, 5, fill);
  CV.drawRoundRect(x, y, w, h, 5, border);
}

void buildField() {
  memset(field, 0, sizeof(field));
  int ks = kernelR * 2 + 1;

  for (int i = 0; i < activeWater; i++) {
    int cx = wx[i] / GRID_STEP;
    int cy = wy[i] / GRID_STEP;
    for (int ky = -kernelR; ky <= kernelR; ky++) {
      int fy = cy + ky;
      if (fy < 0 || fy >= fieldH) continue;
      for (int kx = -kernelR; kx <= kernelR; kx++) {
        int fx = cx + kx;
        if (fx < 0 || fx >= fieldW) continue;
        uint16_t add = kernel[(ky + kernelR) * ks + (kx + kernelR)];
        if (!add) continue;
        int id = fy * fieldW + fx;
        uint32_t v = field[id] + add;
        field[id] = v > 65535 ? 65535 : v;
      }
    }
  }
}

void drawWaterBody() {
  buildField();

  for (int gy = 0; gy < fieldH; gy++) {
    int run = -1;
    for (int gxv = 0; gxv < fieldW; gxv++) {
      bool inside = field[gy * fieldW + gxv] >= FIELD_THRESHOLD;
      if (inside && run < 0) run = gxv;
      bool last = gxv == fieldW - 1;
      if ((!inside || last) && run >= 0) {
        int end = (inside && last) ? gxv : gxv - 1;
        CV.fillRect(run * GRID_STEP, gy * GRID_STEP, (end - run + 1) * GRID_STEP, GRID_STEP, waterColor());
        run = -1;
      }
    }
  }

  for (int gy = 0; gy < fieldH; gy++) {
    for (int gxv = 0; gxv < fieldW; gxv++) {
      int id = gy * fieldW + gxv;
      if (field[id] < FIELD_THRESHOLD) continue;
      bool top = gy == 0 || field[(gy - 1) * fieldW + gxv] < FIELD_THRESHOLD;
      if (top) CV.fillRect(gxv * GRID_STEP, gy * GRID_STEP, GRID_STEP, 1, edgeColor());
    }
  }
}

void drawDrops() {
  for (int i = 0; i < DROP_MAX; i++) {
    if (!dropOn[i]) continue;
    CV.fillCircle((int)dx[i], (int)dy[i], (int)dr[i], edgeColor());
    if (dr[i] >= 2) CV.drawPixel((int)dx[i] - 1, (int)dy[i] - 1, WHITE);
  }
}

void drawWaterUI() {
  CV.setTextSize(1);
  CV.setTextWrap(false);
  CV.setTextColor(MUTED);
  CV.setCursor(5, 6);
  CV.print("Water");
  CV.setCursor(5, screenH - 12);
  CV.print("JoySW: Back");
}

void drawWaterFrame() {
  CV.fillScreen(BG);
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
const char *menuSubs[MENU_COUNT] = {"Interactive liquid toy", "Display + controls"};
int menuIndex = 0;
uint32_t nextMenuMove = 0;
uint32_t nextSettingMove = 0;

const char *rotationName(uint8_t r) {
  switch (r & 3) {
    case 0: return "0 Portrait";
    case 1: return "1 Landscape";
    case 2: return "2 Portrait Flip";
    default: return "3 Landscape Flip";
  }
}

void drawMenu() {
  CV.fillScreen(BG);
  centeredText("ESP32Toy", screenH >= 150 ? 10 : 8, 2, WHITE);
  centeredText("Mini System", screenH >= 150 ? 31 : 28, 1, MUTED);

  int topY = screenH >= 150 ? 58 : 46;
  int boxH = screenH >= 150 ? 24 : 22;
  int gap = screenH >= 150 ? 30 : 28;
  int boxW = screenW - 24;
  int boxX = 12;

  for (int i = 0; i < MENU_COUNT; i++) {
    int y = topY + i * gap;
    bool sel = i == menuIndex;
    panel(boxX, y, boxW, boxH, sel ? PANEL2 : PANEL, sel ? waterColor() : LINE);
    CV.setTextSize(1);
    CV.setTextColor(sel ? WHITE : MUTED);
    CV.setCursor(boxX + 10, y + (boxH / 2 - 3));
    CV.print(menuItems[i]);
    if (sel) CV.fillCircle(boxX + boxW - 12, y + boxH / 2, 3, waterColor());
  }

  centeredText(menuSubs[menuIndex], screenH - 12, 1, MUTED);
  pushCanvas();
}

void updateMenu() {
  uint32_t now = millis();
  if (now < nextMenuMove) return;
  Vec2 j = readJoystickVectorToScreen();
  if (j.y > 0.62f) {
    menuIndex = (menuIndex + 1) % MENU_COUNT;
    nextMenuMove = now + 220;
    motorPulse(45);
  } else if (j.y < -0.62f) {
    menuIndex = (menuIndex + MENU_COUNT - 1) % MENU_COUNT;
    nextMenuMove = now + 220;
    motorPulse(45);
  }
}

void drawSettings() {
  CV.fillScreen(BG);
  centeredText("Settings", screenH >= 150 ? 10 : 8, 2, WHITE);
  centeredText("Screen rotation", screenH >= 150 ? 36 : 30, 1, MUTED);

  int boxW = screenW - 18;
  int boxX = 9;
  int boxY = screenH >= 150 ? 62 : 52;
  int boxH = 34;
  panel(boxX, boxY, boxW, boxH, PANEL2, waterColor());

  centeredText(rotationName(displayRotation), boxY + 11, 1, WHITE);
  centeredText("<  joystick  >", boxY + boxH + 16, 1, MUTED);
  centeredText("Key: rotate   JoySW: back", screenH - 12, 1, MUTED);
  pushCanvas();
}

// ============================================================
// Runtime rotation application
// ============================================================
void applyDisplayRotation(uint8_t rot, bool resetWater) {
  displayRotation = rot & 3;
  tft.setRotation(displayRotation);

  if (displayRotation == 0 || displayRotation == 2) {
    canvas = &canvasPortrait;
    screenW = PORT_W;
    screenH = PORT_H;
  } else {
    canvas = &canvasLandscape;
    screenW = LAND_W;
    screenH = LAND_H;
  }

  fieldW = screenW / GRID_STEP + 3;
  fieldH = screenH / GRID_STEP + 3;
  CV.fillScreen(BG);
  pushCanvas();

  if (resetWater) {
    forceX = 0;
    forceY = 0;
    joyForceX = 0;
    joyForceY = 0;
    initWater();
  }
}

void rotateDisplayBy(int delta) {
  int next = ((int)displayRotation + delta) % 4;
  if (next < 0) next += 4;
  applyDisplayRotation((uint8_t)next, true);
  motorPulse(90);
}

void updateSettings() {
  uint32_t now = millis();
  Vec2 j = readJoystickVectorToScreen();

  if (now >= nextSettingMove) {
    if (j.x > 0.62f) {
      rotateDisplayBy(1);
      nextSettingMove = now + 320;
    } else if (j.x < -0.62f) {
      rotateDisplayBy(-1);
      nextSettingMove = now + 320;
    }
  }

  if (keyPressedEdge) {
    rotateDisplayBy(1);
    nextSettingMove = now + 320;
  }

  if (joyPressed && !joyPressedPrev) {
    mode = MENU;
    motorPulse(120);
  }
  joyPressedPrev = joyPressed;
}

// ============================================================
// Boot animation
// ============================================================
void drawBoot(float t) {
  CV.fillScreen(BG);
  float e = t * t * (3.0f - 2.0f * t);

  int cx = screenW / 2;
  int cy = screenH / 2;
  CV.drawCircle(cx, cy, 18, rgb565(20, 45, 70));
  CV.drawCircle(cx, cy, 26, rgb565(10, 30, 52));

  CV.fillCircle((int)(screenW * 0.20f + (cx - screenW * 0.20f) * e), (int)(-8 + (cy - 10 + 8) * e), 6, rgb565(45, 160, 255));
  CV.fillCircle(cx, (int)(-16 + (cy - 16 + 16) * e), 7, rgb565(65, 190, 255));
  CV.fillCircle((int)(screenW * 0.82f + (cx - screenW * 0.82f) * e), (int)(-6 + (cy - 6 + 6) * e), 6, rgb565(90, 220, 255));

  if (t > 0.52f) {
    float q = clampf((t - 0.52f) / 0.48f, 0, 1);
    int r = 4 + q * 16;
    CV.fillCircle(cx, cy, r, ACCENT);
    CV.drawCircle(cx, cy, r + 2, rgb565(170, 235, 255));
  }

  if (t > 0.73f) centeredText("ESP32 TOY", screenH - 24, 1, WHITE);
  pushCanvas();
}

void runBoot() {
  uint32_t s = millis();
  while (millis() - s < 1800) {
    drawBoot((millis() - s) / 1800.0f);
    delay(20);
  }
  drawBoot(1.0f);
  delay(140);
}

void bootText(const char *s) {
  CV.fillScreen(BG);
  centeredText(s, screenH / 2 - 4, 1, WHITE);
  pushCanvas();
}

// ============================================================
// Input / app controls
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
  if (now - keyChangedAt >= KEY_DEBOUNCE && raw != keyStable) {
    keyStable = raw;
    if (keyStable == LOW) keyPressedEdge = true;
  }
}

void nextWaterColor() {
  waterColorIndex = (waterColorIndex + 1) % COLOR_COUNT;
  motorPulse(150);
}

void handleWaterControls() {
  if (keyPressedEdge) nextWaterColor();
  if (joyPressed && !joyPressedPrev) {
    mode = MENU;
    motorPulse(120);
  }
  joyPressedPrev = joyPressed;
}

// ============================================================
// Setup / loop
// ============================================================
uint32_t lastFrame = 0;

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

  runBoot();
  bootText("CALIBRATING");
  initMPU();
  calibrateMPU();
  calibrateJoystick();

  potRaw = analogRead(POT_PIN);
  potFiltered = potRaw;
  updateWaterAmount();
  rebuildKernel();
  initWater();

  mode = MENU;
  drawMenu();
}

void loop() {
  updateInputs();
  updateMotor();
  uint32_t now = millis();

  if (mode == MENU) {
    updateMenu();
    if (keyPressedEdge) {
      if (menuIndex == 0) {
        mode = WATER;
        motorPulse(90);
      } else {
        mode = SETTINGS;
        motorPulse(90);
      }
    }
    if (now - lastFrame >= 20) {
      drawMenu();
      lastFrame = now;
    }
    joyPressedPrev = joyPressed;
    return;
  }

  if (mode == SETTINGS) {
    updateSettings();
    if (now - lastFrame >= 20) {
      drawSettings();
      lastFrame = now;
    }
    return;
  }

  if (mode == WATER) {
    handleWaterControls();
    updateWater();
    updateDrops();
    if (now - lastFrame >= 20) {
      drawWaterFrame();
      lastFrame = now;
    }
    return;
  }
}
