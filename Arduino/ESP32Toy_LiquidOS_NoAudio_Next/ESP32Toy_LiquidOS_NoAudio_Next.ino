#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

// ============================================================
// ESP32Toy LiquidOS No-Audio Next
// ESP32-S3 N16R8 + ST7735 1.8 + MPU6050 + joystick + pot
// A key IO15, B key IO14, RGB LED IO47
// Audio module is intentionally skipped.
// ============================================================

#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC    9
#define TFT_BL   21
#define TFT_RST  -1

#define I2C_SDA    4
#define I2C_SCL    5
#define POT_PIN    1
#define JOY_X_PIN 16
#define JOY_Y_PIN  8
#define JOY_SW_PIN 6
#define MOTOR_PIN  7
#define KEY_A_PIN 15
#define KEY_B_PIN 14
#define RGB_PIN   47
#define RGB_COUNT  4

#define MOTOR_ACTIVE_HIGH true

#define LAND_W 160
#define LAND_H 128
#define PORT_W 128
#define PORT_H 160

SPIClass screenSPI(FSPI);
Adafruit_ST7735 tft(&screenSPI, TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 canvasLand(LAND_W, LAND_H);
GFXcanvas16 canvasPort(PORT_W, PORT_H);
GFXcanvas16 *cv = &canvasLand;
Adafruit_NeoPixel leds(RGB_COUNT, RGB_PIN, NEO_GRB + NEO_KHZ800);

int screenW = LAND_W;
int screenH = LAND_H;
uint8_t displayRotation = 3;

#define C (*cv)

enum Mode { MODE_BOOT, MODE_MENU, MODE_WATER, MODE_DOOM, MODE_SETTINGS };
Mode mode = MODE_BOOT;

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
float dz(float v, float d) {
  if (fabsf(v) <= d) return 0.0f;
  float s = v >= 0.0f ? 1.0f : -1.0f;
  return s * clampf((fabsf(v) - d) / (1.0f - d), 0.0f, 1.0f);
}

const uint16_t BG = rgb565(3, 7, 16);
const uint16_t PANEL = rgb565(10, 18, 34);
const uint16_t PANEL2 = rgb565(15, 27, 48);
const uint16_t LINE = rgb565(45, 73, 108);
const uint16_t WHITE = ST77XX_WHITE;
const uint16_t MUTED = rgb565(150, 170, 190);
const uint16_t DOOM_RED = rgb565(220, 35, 25);

const uint8_t WATER_COLOR_COUNT = 7;
uint8_t waterColorIndex = 5;
uint16_t waterColors[WATER_COLOR_COUNT] = {
  rgb565(255,60,60), rgb565(255,145,40), rgb565(255,220,40),
  rgb565(60,220,90), rgb565(40,220,235), rgb565(55,125,255), rgb565(180,85,255)
};
uint16_t edgeColors[WATER_COLOR_COUNT] = {
  rgb565(255,180,180), rgb565(255,215,155), rgb565(255,245,170),
  rgb565(180,255,195), rgb565(170,255,255), rgb565(185,220,255), rgb565(230,190,255)
};
uint16_t waterColor() { return waterColors[waterColorIndex]; }
uint16_t edgeColor() { return edgeColors[waterColorIndex]; }

void mapBaseVectorToScreen(float bx, float by, float *ox, float *oy) {
  switch (displayRotation & 3) {
    case 0: *ox = bx;  *oy = by;  break;
    case 1: *ox = by;  *oy = -bx; break;
    case 2: *ox = -bx; *oy = -by; break;
    default:*ox = -by; *oy = bx;  break;
  }
}

void pushCanvas() { tft.drawRGBBitmap(0, 0, cv->getBuffer(), screenW, screenH); }

void centered(const char *txt, int y, uint8_t size, uint16_t color) {
  C.setTextSize(size); C.setTextColor(color); C.setTextWrap(false);
  int16_t x1, y1; uint16_t w, h;
  C.getTextBounds(txt, 0, y, &x1, &y1, &w, &h);
  int x = (screenW - (int)w) / 2; if (x < 0) x = 0;
  C.setCursor(x, y); C.print(txt);
}

void panel(int x, int y, int w, int h, uint16_t fill, uint16_t border) {
  C.fillRoundRect(x, y, w, h, 5, fill);
  C.drawRoundRect(x, y, w, h, 5, border);
}

void ledsAll(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < RGB_COUNT; i++) leds.setPixelColor(i, leds.Color(r, g, b));
  leds.show();
}

void ledsWater() {
  switch (waterColorIndex) {
    case 0: ledsAll(45, 0, 0); break;
    case 1: ledsAll(45, 14, 0); break;
    case 2: ledsAll(38, 30, 0); break;
    case 3: ledsAll(0, 36, 8); break;
    case 4: ledsAll(0, 28, 28); break;
    case 5: ledsAll(0, 8, 45); break;
    default: ledsAll(28, 0, 36); break;
  }
}

uint32_t motorUntil = 0;
void motorWrite(bool on) { digitalWrite(MOTOR_PIN, MOTOR_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH)); }
void motorPulse(uint32_t ms) { uint32_t t = millis() + ms; if (t > motorUntil) motorUntil = t; }
void updateMotor() { motorWrite(millis() < motorUntil); }

int joyXRaw = 2048, joyYRaw = 2048, joyCenterX = 2048, joyCenterY = 2048;
int potRaw = 0;
float potFiltered = 0;
bool joyPressed = false, joyPressedPrev = false;

bool aPrevRaw = HIGH, aStable = HIGH, aEdge = false;
bool bPrevRaw = HIGH, bStable = HIGH, bEdge = false;
uint32_t aChangeMs = 0, bChangeMs = 0;

void updateInputs() {
  joyXRaw = analogRead(JOY_X_PIN);
  joyYRaw = analogRead(JOY_Y_PIN);
  potRaw = analogRead(POT_PIN);
  joyPressed = digitalRead(JOY_SW_PIN) == LOW;
  uint32_t now = millis();
  aEdge = false; bEdge = false;

  bool ar = digitalRead(KEY_A_PIN);
  if (ar != aPrevRaw) { aPrevRaw = ar; aChangeMs = now; }
  if (now - aChangeMs > 35 && ar != aStable) { aStable = ar; if (aStable == LOW) aEdge = true; }

  bool br = digitalRead(KEY_B_PIN);
  if (br != bPrevRaw) { bPrevRaw = br; bChangeMs = now; }
  if (now - bChangeMs > 35 && br != bStable) { bStable = br; if (bStable == LOW) bEdge = true; }
}

void joystickScreen(float *x, float *y) {
  float nx = dz(clampf((joyXRaw - joyCenterX) / 1800.0f, -1, 1), 0.14f);
  float ny = dz(clampf((joyYRaw - joyCenterY) / 1800.0f, -1, 1), 0.14f);
  mapBaseVectorToScreen(nx, ny, x, y);
}

// ---------------- MPU6050 ----------------
uint8_t mpuAddr = 0x68;
bool mpuOk = false;
float ax = 0, ay = 0, az = 1, gx = 0, gy = 0, gz = 0;
float biasAX = 0, biasAY = 0, lastAX = 0, lastAY = 0, lastAZ = 1;
float forceX = 0, forceY = 0, joyForceX = 0, joyForceY = 0;

bool checkI2C(uint8_t addr) { Wire.beginTransmission(addr); return Wire.endTransmission() == 0; }
void initMPU() {
  Wire.setPins(I2C_SDA, I2C_SCL); Wire.begin(); delay(80);
  if (checkI2C(0x68)) { mpuAddr = 0x68; mpuOk = true; }
  else if (checkI2C(0x69)) { mpuAddr = 0x69; mpuOk = true; }
  if (!mpuOk) return;
  const uint8_t regs[][2] = {{0x6B,0},{0x1A,3},{0x1C,0},{0x1B,0}};
  for (int i = 0; i < 4; i++) { Wire.beginTransmission(mpuAddr); Wire.write(regs[i][0]); Wire.write(regs[i][1]); Wire.endTransmission(true); }
}
bool readMPU() {
  if (!mpuOk) return false;
  Wire.beginTransmission(mpuAddr); Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(mpuAddr, (uint8_t)14, (uint8_t)true);
  if (Wire.available() < 14) return false;
  int16_t rax = Wire.read()<<8 | Wire.read();
  int16_t ray = Wire.read()<<8 | Wire.read();
  int16_t raz = Wire.read()<<8 | Wire.read();
  Wire.read(); Wire.read();
  int16_t rgx = Wire.read()<<8 | Wire.read();
  int16_t rgy = Wire.read()<<8 | Wire.read();
  int16_t rgz = Wire.read()<<8 | Wire.read();
  ax = rax / 16384.0f; ay = ray / 16384.0f; az = raz / 16384.0f;
  gx = rgx / 131.0f; gy = rgy / 131.0f; gz = rgz / 131.0f;
  return true;
}
void calibrateMPU() {
  float sx = 0, sy = 0; int n = 0;
  for (int i = 0; i < 100; i++) { if (readMPU()) { sx += ax; sy += ay; n++; } delay(6); }
  if (n > 0) { biasAX = sx / n; biasAY = sy / n; }
  lastAX = ax; lastAY = ay; lastAZ = az;
}
void calibrateJoy() {
  long sx = 0, sy = 0;
  for (int i = 0; i < 48; i++) { sx += analogRead(JOY_X_PIN); sy += analogRead(JOY_Y_PIN); delay(4); }
  joyCenterX = sx / 48; joyCenterY = sy / 48;
}

// ---------------- Water ----------------
const int WMIN = 36, WMAX = 125;
int activeWater = 75;
const float WR = 4.1f;
float wx[WMAX], wy[WMAX], wvx[WMAX], wvy[WMAX];

const int DMAX = 22;
bool dropOn[DMAX];
float dxp[DMAX], dyp[DMAX], dvx[DMAX], dvy[DMAX], dr[DMAX];
int dlife[DMAX];

const int GRID = 2;
const int FIELD_MAX_W = LAND_W / GRID + 3;
const int FIELD_MAX_H = PORT_H / GRID + 3;
uint16_t field[FIELD_MAX_W * FIELD_MAX_H];
int fieldW = LAND_W / GRID + 3, fieldH = LAND_H / GRID + 3;
const int KRMIN = 8, KRMAX = 12, KMAX = KRMAX * 2 + 1;
int kernelR = 9, kernelBuilt = -1;
uint16_t kernel[KMAX * KMAX];
const uint16_t FIELD_TH = 455;
int splashCooldown = 0;

void rebuildKernel() {
  memset(kernel, 0, sizeof(kernel));
  float rp = kernelR * GRID, rs = rp * rp; int ks = kernelR * 2 + 1;
  for (int y = -kernelR; y <= kernelR; y++) for (int x = -kernelR; x <= kernelR; x++) {
    float px = x * GRID, py = y * GRID, d2 = px*px + py*py;
    if (d2 < rs) { float q = 1.0f - d2 / rs; kernel[(y + kernelR) * ks + (x + kernelR)] = q * q * 1180.0f; }
  }
  kernelBuilt = kernelR;
}

void initWater() {
  int id = 0;
  int cols = screenW >= screenH ? 13 : 11;
  int rows = (WMAX + cols - 1) / cols;
  float sx = screenW >= screenH ? 10.5f : 10.0f;
  float sy = screenW >= screenH ? 6.5f : 7.0f;
  float startX = (screenW - (cols - 1) * sx) * 0.5f; if (startX < 7) startX = 7;
  float startY = screenH - (rows - 1) * sy - 10; if (startY < 18) startY = 18;
  for (int y = 0; y < rows; y++) for (int x = 0; x < cols; x++) {
    if (id >= WMAX) break;
    wx[id] = startX + x * sx + random(-1, 2);
    wy[id] = startY + y * sy + random(-1, 2);
    wvx[id] = 0; wvy[id] = 0; id++;
  }
  for (int i = 0; i < DMAX; i++) dropOn[i] = false;
}

void updateWaterAmount() {
  potFiltered = potFiltered <= 0.1f ? potRaw : potFiltered * 0.90f + potRaw * 0.10f;
  float t = clampf(potFiltered / 4095.0f, 0, 1);
  int target = constrain(WMIN + (int)(t * (WMAX - WMIN)), WMIN, WMAX);
  int kr = constrain(KRMIN + (int)(t * (KRMAX - KRMIN) + 0.5f), KRMIN, KRMAX);
  if (target > activeWater) for (int i = activeWater; i < target; i++) {
    int r = random(0, activeWater > 0 ? activeWater : 1);
    wx[i] = constrain(wx[r] + random(-10, 11), WR, screenW - 1 - WR);
    wy[i] = constrain(wy[r] + random(-10, 11), WR, screenH - 1 - WR);
    wvx[i] = wvx[r] * 0.18f; wvy[i] = wvy[r] * 0.18f;
  }
  activeWater = target; kernelR = kr; if (kernelR != kernelBuilt) rebuildKernel();
}

void boundP(int i) {
  const float b = -0.18f;
  if (wx[i] < WR) { wx[i] = WR; wvx[i] *= b; }
  if (wx[i] > screenW - 1 - WR) { wx[i] = screenW - 1 - WR; wvx[i] *= b; }
  if (wy[i] < WR) { wy[i] = WR; wvy[i] *= b; }
  if (wy[i] > screenH - 1 - WR) { wy[i] = screenH - 1 - WR; wvy[i] *= b; }
}

void solvePairs() {
  float td = WR * 1.52f, td2 = td * td;
  for (int i = 0; i < activeWater; i++) for (int j = i + 1; j < activeWater; j++) {
    float x = wx[j] - wx[i], y = wy[j] - wy[i], d2 = x*x + y*y;
    if (d2 < 0.0001f || d2 >= td2) continue;
    float d = sqrtf(d2), push = (td - d) * 0.46f, nx = x / d, ny = y / d;
    wx[i] -= nx * push; wy[i] -= ny * push; wx[j] += nx * push; wy[j] += ny * push;
  }
}

void viscosity() {
  float r = WR * 2.65f, r2 = r*r;
  for (int i = 0; i < activeWater; i++) for (int j = i + 1; j < activeWater; j++) {
    float x = wx[j] - wx[i], y = wy[j] - wy[i];
    if (x*x + y*y >= r2) continue;
    float vx = wvx[j] - wvx[i], vy = wvy[j] - wvy[i], k = 0.022f;
    wvx[i] += vx*k; wvy[i] += vy*k; wvx[j] -= vx*k; wvy[j] -= vy*k;
  }
}

void spawnDrop(float x, float y, float vx, float vy, float rad) {
  for (int i = 0; i < DMAX; i++) if (!dropOn[i]) {
    dropOn[i] = true; dxp[i] = x; dyp[i] = y; dvx[i] = vx; dvy[i] = vy; dr[i] = rad; dlife[i] = 78; return;
  }
}

void splash(float fx, float fy, float strength) {
  if (activeWater <= 0) return;
  motorPulse(80);
  float len = sqrtf(fx*fx + fy*fy), ux = 0, uy = -1;
  if (len > 0.05f) { ux = -fx / len; uy = -fy / len; }
  int count = constrain((int)(strength * 1.4f), 3, 7);
  for (int n = 0; n < count; n++) {
    int id = random(0, activeWater);
    float side = random(-100, 101) / 100.0f;
    spawnDrop(wx[id], wy[id], wvx[id] + ux * (1.0f + strength * 0.4f) + (-uy) * side * 0.5f,
              wvy[id] + uy * (1.0f + strength * 0.4f) + ux * side * 0.5f, random(1, 3));
  }
}

void updateDrops() {
  for (int i = 0; i < DMAX; i++) if (dropOn[i]) {
    dvx[i] += forceX * 0.012f; dvy[i] += 0.070f + forceY * 0.012f;
    dvx[i] *= 0.994f; dvy[i] *= 0.994f; dxp[i] += dvx[i]; dyp[i] += dvy[i]; dlife[i]--;
    if (dxp[i] < 2) { dxp[i] = 2; dvx[i] *= -0.24f; }
    if (dxp[i] > screenW - 3) { dxp[i] = screenW - 3; dvx[i] *= -0.24f; }
    if (dyp[i] < 2) { dyp[i] = 2; dvy[i] *= -0.20f; }
    for (int j = 0; j < activeWater; j++) {
      float x = dxp[i] - wx[j], y = dyp[i] - wy[j];
      if (x*x + y*y < WR*WR*1.85f) { wvx[j] += dvx[i]*0.045f; wvy[j] += dvy[i]*0.045f; dropOn[i] = false; break; }
    }
    if (dyp[i] > screenH || dlife[i] <= 0) dropOn[i] = false;
  }
}

void updateWaterSim() {
  updateWaterAmount();
  if (!readMPU()) return;

  float jx, jy; joystickScreen(&jx, &jy);
  joyForceX = joyForceX * 0.80f + jx * 0.20f;
  joyForceY = joyForceY * 0.80f + jy * 0.20f;

  float bx = (ax - biasAX), by = -(ay - biasAY);
  if (fabsf(bx) < 0.03f) bx = 0; if (fabsf(by) < 0.03f) by = 0;
  float tx, ty, rgx, rgy;
  mapBaseVectorToScreen(clampf(bx * 2.1f, -1, 1), clampf(by * 2.1f, -1, 1), &tx, &ty);
  mapBaseVectorToScreen(clampf(gx * 0.008f, -1.3f, 1.3f), clampf(gy * 0.008f, -1.3f, 1.3f), &rgx, &rgy);

  forceX = forceX * 0.84f + clampf(tx + joyForceX * 0.75f, -1.35f, 1.35f) * 0.16f;
  forceY = forceY * 0.84f + clampf(ty + joyForceY * 0.75f, -1.35f, 1.35f) * 0.16f;

  for (int i = 0; i < activeWater; i++) {
    wvx[i] += forceX * 0.27f + rgx * 0.18f; wvy[i] += forceY * 0.27f + rgy * 0.18f;
    wvx[i] *= 0.986f; wvy[i] *= 0.986f;
    wvx[i] = clampf(wvx[i], -4.2f, 4.2f); wvy[i] = clampf(wvy[i], -4.2f, 4.2f);
    wx[i] += wvx[i]; wy[i] += wvy[i]; boundP(i);
  }
  solvePairs(); solvePairs(); solvePairs();
  for (int i = 0; i < activeWater; i++) boundP(i);
  viscosity();

  float jerk = fabsf(ax - lastAX) + fabsf(ay - lastAY) + fabsf(az - lastAZ);
  lastAX = ax; lastAY = ay; lastAZ = az;
  float kick = fabsf(gx)*0.0028f + fabsf(gy)*0.0028f + fabsf(gz)*0.0018f;
  if (splashCooldown > 0) splashCooldown--;
  float s = jerk * 6.5f + kick;
  if (s > 1.25f && splashCooldown <= 0) { splash(forceX + rgx*0.35f, forceY + rgy*0.35f, s); splashCooldown = 9; }
}

void buildField() {
  memset(field, 0, sizeof(field));
  int ks = kernelR * 2 + 1;
  for (int i = 0; i < activeWater; i++) {
    int cx = wx[i] / GRID, cy = wy[i] / GRID;
    for (int ky = -kernelR; ky <= kernelR; ky++) {
      int fy = cy + ky; if (fy < 0 || fy >= fieldH) continue;
      for (int kx = -kernelR; kx <= kernelR; kx++) {
        int fx = cx + kx; if (fx < 0 || fx >= fieldW) continue;
        uint16_t add = kernel[(ky + kernelR) * ks + (kx + kernelR)]; if (!add) continue;
        int id = fy * fieldW + fx; uint32_t v = field[id] + add; field[id] = v > 65535 ? 65535 : v;
      }
    }
  }
}

void drawWaterBody() {
  buildField();
  for (int y = 0; y < fieldH; y++) {
    int run = -1;
    for (int x = 0; x < fieldW; x++) {
      bool inside = field[y * fieldW + x] >= FIELD_TH;
      if (inside && run < 0) run = x;
      bool last = x == fieldW - 1;
      if ((!inside || last) && run >= 0) {
        int end = (inside && last) ? x : x - 1;
        C.fillRect(run * GRID, y * GRID, (end - run + 1) * GRID, GRID, waterColor());
        run = -1;
      }
    }
  }
  for (int y = 0; y < fieldH; y++) for (int x = 0; x < fieldW; x++) {
    int id = y * fieldW + x; if (field[id] < FIELD_TH) continue;
    if (y == 0 || field[(y - 1) * fieldW + x] < FIELD_TH) C.fillRect(x * GRID, y * GRID, GRID, 1, edgeColor());
  }
}

void drawDrops() {
  for (int i = 0; i < DMAX; i++) if (dropOn[i]) {
    C.fillCircle((int)dxp[i], (int)dyp[i], (int)dr[i], edgeColor());
    if (dr[i] >= 2) C.drawPixel((int)dxp[i] - 1, (int)dyp[i] - 1, WHITE);
  }
}

void drawWater() {
  C.fillScreen(BG); drawWaterBody(); drawDrops();
  C.setTextSize(1); C.setTextColor(MUTED); C.setCursor(5, 6); C.print("Water");
  C.setCursor(5, screenH - 12); C.print("A:Color  B:Back");
  pushCanvas();
}

// ---------------- Menu / Doom / Settings ----------------
const int MENU_COUNT = 3;
const char *menuItems[MENU_COUNT] = {"Water", "Doom", "Settings"};
const char *menuSubs[MENU_COUNT] = {"Interactive liquid", "Placeholder app", "Rotation"};
int menuIndex = 0;
uint32_t nextMenuMs = 0, nextSettingMs = 0;

void drawMenu() {
  C.fillScreen(BG);
  centered("ESP32Toy", screenH >= 150 ? 10 : 8, 2, WHITE);
  centered("Mini System", screenH >= 150 ? 31 : 28, 1, MUTED);
  int top = screenH >= 150 ? 54 : 42;
  int h = screenH >= 150 ? 24 : 22;
  int gap = screenH >= 150 ? 30 : 26;
  for (int i = 0; i < MENU_COUNT; i++) {
    int y = top + i * gap;
    bool sel = i == menuIndex;
    uint16_t acc = (i == 1) ? DOOM_RED : waterColor();
    panel(12, y, screenW - 24, h, sel ? PANEL2 : PANEL, sel ? acc : LINE);
    C.setTextSize(1); C.setTextColor(sel ? WHITE : MUTED); C.setCursor(22, y + h/2 - 3); C.print(menuItems[i]);
    if (sel) C.fillCircle(screenW - 24, y + h/2, 3, acc);
  }
  centered(menuSubs[menuIndex], screenH - 12, 1, MUTED);
  pushCanvas();
}

void updateMenu() {
  if (millis() < nextMenuMs) return;
  float x, y; joystickScreen(&x, &y);
  if (y > 0.62f) { menuIndex = (menuIndex + 1) % MENU_COUNT; nextMenuMs = millis() + 220; motorPulse(45); }
  else if (y < -0.62f) { menuIndex = (menuIndex + MENU_COUNT - 1) % MENU_COUNT; nextMenuMs = millis() + 220; motorPulse(45); }
}

void drawDoom() {
  C.fillScreen(rgb565(10, 4, 4));
  centered("DOOM", 12, 3, DOOM_RED);
  centered("APP STUB", 42, 1, WHITE);
  int cx = screenW / 2, cy = screenH / 2 + 5;
  C.drawRoundRect(cx - 52, cy - 22, 104, 44, 5, DOOM_RED);
  C.setTextSize(1); C.setTextColor(MUTED);
  C.setCursor(cx - 42, cy - 12); C.print("Engine later");
  C.setCursor(cx - 42, cy + 2); C.print("A: Fire test");
  C.setCursor(cx - 42, cy + 16); C.print("B: Back");
  pushCanvas();
}

void updateDoom() {
  static uint32_t lastLed = 0; static uint8_t ph = 0;
  if (millis() - lastLed > 80) {
    ph += 7;
    for (int i = 0; i < RGB_COUNT; i++) {
      uint8_t v = (ph + i * 40) & 127; if (v > 63) v = 127 - v;
      leds.setPixelColor(i, leds.Color(28 + v / 2, 0, 0));
    }
    leds.show(); lastLed = millis();
  }
  if (aEdge) motorPulse(120);
  if (bEdge || (joyPressed && !joyPressedPrev)) { mode = MODE_MENU; ledsWater(); motorPulse(120); }
  joyPressedPrev = joyPressed;
}

const char *rotName(uint8_t r) {
  switch (r & 3) { case 0: return "0 Portrait"; case 1: return "1 Landscape"; case 2: return "2 Portrait Flip"; default: return "3 Landscape Flip"; }
}

void applyRotation(uint8_t r, bool resetWater) {
  displayRotation = r & 3; tft.setRotation(displayRotation);
  if (displayRotation == 0 || displayRotation == 2) { cv = &canvasPort; screenW = PORT_W; screenH = PORT_H; }
  else { cv = &canvasLand; screenW = LAND_W; screenH = LAND_H; }
  fieldW = screenW / GRID + 3; fieldH = screenH / GRID + 3;
  C.fillScreen(BG); pushCanvas();
  if (resetWater) { forceX = forceY = joyForceX = joyForceY = 0; initWater(); }
}

void rotateBy(int d) { int n = ((int)displayRotation + d) % 4; if (n < 0) n += 4; applyRotation(n, true); motorPulse(90); }

void drawSettings() {
  C.fillScreen(BG);
  centered("Settings", screenH >= 150 ? 10 : 8, 2, WHITE);
  centered("Screen rotation", screenH >= 150 ? 36 : 30, 1, MUTED);
  int boxY = screenH >= 150 ? 62 : 52;
  panel(9, boxY, screenW - 18, 34, PANEL2, waterColor());
  centered(rotName(displayRotation), boxY + 11, 1, WHITE);
  centered("A / Joy Left Right", boxY + 50, 1, MUTED);
  centered("B: back", screenH - 12, 1, MUTED);
  pushCanvas();
}

void updateSettings() {
  uint32_t now = millis();
  float x, y; joystickScreen(&x, &y);
  if (now >= nextSettingMs) {
    if (x > 0.62f) { rotateBy(1); nextSettingMs = now + 320; }
    else if (x < -0.62f) { rotateBy(-1); nextSettingMs = now + 320; }
  }
  if (aEdge) { rotateBy(1); nextSettingMs = now + 320; }
  if (bEdge || (joyPressed && !joyPressedPrev)) { mode = MODE_MENU; ledsWater(); motorPulse(120); }
  joyPressedPrev = joyPressed;
}

void drawBoot(float t) {
  C.fillScreen(BG);
  float e = t * t * (3 - 2 * t); int cx = screenW / 2, cy = screenH / 2;
  C.drawCircle(cx, cy, 18, rgb565(20,45,70)); C.drawCircle(cx, cy, 26, rgb565(10,30,52));
  C.fillCircle((int)(screenW*0.2f + (cx-screenW*0.2f)*e), (int)(-8 + (cy-2+8)*e), 6, rgb565(45,160,255));
  C.fillCircle(cx, (int)(-16 + (cy-10+16)*e), 7, rgb565(65,190,255));
  C.fillCircle((int)(screenW*0.82f + (cx-screenW*0.82f)*e), (int)(-6 + (cy+2+6)*e), 6, rgb565(90,220,255));
  if (t > 0.52f) { float q = clampf((t - 0.52f) / 0.48f, 0, 1); int r = 4 + q*16; C.fillCircle(cx, cy, r, rgb565(90,210,255)); C.drawCircle(cx, cy, r + 2, rgb565(170,235,255)); }
  if (t > 0.73f) centered("ESP32 TOY", screenH - 24, 1, WHITE);
  pushCanvas();
}

uint32_t wheel(byte p) {
  p = 255 - p;
  if (p < 85) return leds.Color((255 - p * 3) / 5, 0, (p * 3) / 5);
  if (p < 170) { p -= 85; return leds.Color(0, (p * 3) / 5, (255 - p * 3) / 5); }
  p -= 170; return leds.Color((p * 3) / 5, (255 - p * 3) / 5, 0);
}

void runBoot() {
  uint32_t s = millis();
  while (millis() - s < 1800) {
    float t = (millis() - s) / 1800.0f;
    drawBoot(t);
    for (int i = 0; i < RGB_COUNT; i++) leds.setPixelColor(i, wheel((uint8_t)(t * 255) + i * 64));
    leds.show(); delay(20);
  }
}

uint32_t lastFrame = 0;

void setup() {
  Serial.begin(115200); delay(300);
  pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
  pinMode(POT_PIN, INPUT); pinMode(JOY_X_PIN, INPUT); pinMode(JOY_Y_PIN, INPUT);
  pinMode(JOY_SW_PIN, INPUT_PULLUP); pinMode(KEY_A_PIN, INPUT_PULLUP); pinMode(KEY_B_PIN, INPUT_PULLUP);
  pinMode(MOTOR_PIN, OUTPUT); motorWrite(false);
  leds.begin(); leds.setBrightness(32); ledsAll(0,0,0);
  screenSPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB); applyRotation(displayRotation, false);
  runBoot();
  C.fillScreen(BG); centered("CALIBRATING", screenH/2 - 4, 1, WHITE); pushCanvas();
  initMPU(); calibrateMPU(); calibrateJoy();
  potRaw = analogRead(POT_PIN); potFiltered = potRaw; updateWaterAmount(); rebuildKernel(); initWater();
  ledsWater(); mode = MODE_MENU; drawMenu();
}

void loop() {
  updateInputs(); updateMotor(); uint32_t now = millis();

  if (mode == MODE_MENU) {
    updateMenu();
    if (aEdge) {
      if (menuIndex == 0) { mode = MODE_WATER; ledsWater(); motorPulse(90); }
      else if (menuIndex == 1) { mode = MODE_DOOM; ledsAll(45,0,0); motorPulse(140); }
      else { mode = MODE_SETTINGS; ledsAll(12,12,35); motorPulse(90); }
    }
    if (now - lastFrame >= 20) { drawMenu(); lastFrame = now; }
    joyPressedPrev = joyPressed;
    return;
  }

  if (mode == MODE_WATER) {
    if (aEdge) { waterColorIndex = (waterColorIndex + 1) % WATER_COLOR_COUNT; ledsWater(); motorPulse(150); }
    if (bEdge || (joyPressed && !joyPressedPrev)) { mode = MODE_MENU; ledsWater(); motorPulse(120); }
    joyPressedPrev = joyPressed;
    updateWaterSim(); updateDrops();
    if (now - lastFrame >= 20) { drawWater(); lastFrame = now; }
    return;
  }

  if (mode == MODE_DOOM) {
    updateDoom();
    if (now - lastFrame >= 20) { drawDoom(); lastFrame = now; }
    return;
  }

  if (mode == MODE_SETTINGS) {
    updateSettings();
    if (now - lastFrame >= 20) { drawSettings(); lastFrame = now; }
    return;
  }
}
