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

// Current requested orientation: screen rotated 90 degrees CCW from the prior build.
#define SCREEN_W 160
#define SCREEN_H 128

SPIClass screenSPI(FSPI);
Adafruit_ST7735 tft(&screenSPI, TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 canvas(SCREEN_W, SCREEN_H);

enum AppMode { BOOT, MENU, WATER };
AppMode mode = BOOT;

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
float deadzone(float v, float d) {
  if (fabsf(v) <= d) return 0.0f;
  float s = v >= 0 ? 1.0f : -1.0f;
  return s * clampf((fabsf(v) - d) / (1.0f - d), 0.0f, 1.0f);
}

const uint16_t BG = rgb565(3, 7, 16);
const uint16_t PANEL = rgb565(10, 18, 34);
const uint16_t PANEL2 = rgb565(15, 27, 48);
const uint16_t LINE = rgb565(45, 73, 108);
const uint16_t WHITE = ST77XX_WHITE;
const uint16_t MUTED = rgb565(150, 170, 190);

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

// --------------------- inputs ---------------------
int potRaw = 0;
float potFiltered = 0;
int joyXRaw = 2048, joyYRaw = 2048;
int joyCenterX = 2048, joyCenterY = 2048;
bool joyPressed = false, joyPressedPrev = false;

bool keyRawPrev = HIGH, keyStable = HIGH, keyPressedEdge = false;
uint32_t keyChangedAt = 0;
const uint32_t KEY_DEBOUNCE = 35;

// --------------------- motor ----------------------
uint32_t motorUntil = 0;
void motorWrite(bool on) {
  digitalWrite(MOTOR_PIN, MOTOR_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH));
}
void motorPulse(uint32_t ms) {
  uint32_t t = millis() + ms;
  if (t > motorUntil) motorUntil = t;
}
void updateMotor() { motorWrite(millis() < motorUntil); }

// --------------------- MPU6050 --------------------
uint8_t mpuAddr = 0x68;
bool mpuOk = false;
float ax = 0, ay = 0, az = 1, gx = 0, gy = 0, gz = 0;
float biasAX = 0, biasAY = 0;
float lastAX = 0, lastAY = 0, lastAZ = 1;
float forceX = 0, forceY = 0;
float joyForceX = 0, joyForceY = 0;

// These signs match the previous orientation calibration.
const float TILT_X_SIGN = 1.0f;
const float TILT_Y_SIGN = -1.0f;
const float JOY_X_SIGN = 1.0f;
const float JOY_Y_SIGN = 1.0f;

bool checkI2C(uint8_t a) { Wire.beginTransmission(a); return Wire.endTransmission() == 0; }
void initMPU() {
  Wire.setPins(I2C_SDA, I2C_SCL);
  Wire.begin();
  delay(80);
  if (checkI2C(0x68)) { mpuAddr = 0x68; mpuOk = true; }
  else if (checkI2C(0x69)) { mpuAddr = 0x69; mpuOk = true; }
  if (!mpuOk) return;
  const uint8_t regs[][2] = {{0x6B,0x00},{0x1A,0x03},{0x1C,0x00},{0x1B,0x00}};
  for (uint8_t i=0;i<4;i++) {
    Wire.beginTransmission(mpuAddr); Wire.write(regs[i][0]); Wire.write(regs[i][1]); Wire.endTransmission(true);
  }
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
  for (int i=0;i<120;i++) { if (readMPU()) { sx += ax; sy += ay; n++; } delay(6); }
  if (n > 0) { biasAX = sx/n; biasAY = sy/n; }
  lastAX = ax; lastAY = ay; lastAZ = az;
}
void calibrateJoystick() {
  long sx=0, sy=0;
  for (int i=0;i<48;i++) { sx += analogRead(JOY_X_PIN); sy += analogRead(JOY_Y_PIN); delay(4); }
  joyCenterX = sx/48; joyCenterY = sy/48;
}

// --------------------- Water ----------------------
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
const int FIELD_W = SCREEN_W / GRID_STEP + 3;
const int FIELD_H = SCREEN_H / GRID_STEP + 3;
uint16_t field[FIELD_W * FIELD_H];
const int KERNEL_MIN = 8;
const int KERNEL_MAX = 12;
const int KERNEL_MAX_SIZE = KERNEL_MAX * 2 + 1;
int kernelR = 9, kernelBuiltR = -1;
uint16_t kernel[KERNEL_MAX_SIZE * KERNEL_MAX_SIZE];
const uint16_t FIELD_THRESHOLD = 455;

void rebuildKernel() {
  memset(kernel, 0, sizeof(kernel));
  float rp = kernelR * GRID_STEP, rs = rp * rp;
  int ks = kernelR * 2 + 1;
  for (int y=-kernelR;y<=kernelR;y++) for (int x=-kernelR;x<=kernelR;x++) {
    float px = x * GRID_STEP, py = y * GRID_STEP, d2 = px*px + py*py;
    if (d2 < rs) {
      float t = 1.0f - d2/rs;
      kernel[(y+kernelR)*ks + (x+kernelR)] = (uint16_t)(t*t*1180.0f);
    }
  }
  kernelBuiltR = kernelR;
}
void initWater() {
  randomSeed(micros());
  int id = 0;
  for (int y=0;y<10;y++) for (int x=0;x<13;x++) {
    if (id >= WATER_MAX) break;
    wx[id] = 12 + x * 10.5f + random(-1,2);
    wy[id] = SCREEN_H - 68 + y * 6.5f + random(-1,2);
    wvx[id] = wvy[id] = 0;
    id++;
  }
  for (int i=0;i<DROP_MAX;i++) dropOn[i] = false;
}
void updateWaterAmount() {
  potFiltered = potFiltered <= 0.1f ? potRaw : potFiltered * 0.90f + potRaw * 0.10f;
  float t = clampf(potFiltered / 4095.0f, 0, 1);
  int targetN = WATER_MIN + (int)(t * (WATER_MAX - WATER_MIN));
  int targetK = KERNEL_MIN + (int)(t * (KERNEL_MAX - KERNEL_MIN) + 0.5f);
  targetN = constrain(targetN, WATER_MIN, WATER_MAX);
  targetK = constrain(targetK, KERNEL_MIN, KERNEL_MAX);
  if (targetN > activeWater) {
    for (int i=activeWater;i<targetN;i++) {
      int ref = random(0, activeWater > 0 ? activeWater : 1);
      wx[i] = constrain(wx[ref] + random(-10,11), WR, SCREEN_W - 1 - WR);
      wy[i] = constrain(wy[ref] + random(-10,11), WR, SCREEN_H - 1 - WR);
      wvx[i] = wvx[ref] * 0.18f; wvy[i] = wvy[ref] * 0.18f;
    }
  }
  activeWater = targetN;
  kernelR = targetK;
  if (kernelR != kernelBuiltR) rebuildKernel();
}
void constrainWater(int i) {
  const float b = -0.18f;
  if (wx[i] < WR) { wx[i] = WR; wvx[i] *= b; }
  if (wx[i] > SCREEN_W - 1 - WR) { wx[i] = SCREEN_W - 1 - WR; wvx[i] *= b; }
  if (wy[i] < WR) { wy[i] = WR; wvy[i] *= b; }
  if (wy[i] > SCREEN_H - 1 - WR) { wy[i] = SCREEN_H - 1 - WR; wvy[i] *= b; }
}
void solveWaterPairs() {
  float td = WR * 1.52f, td2 = td * td;
  for (int i=0;i<activeWater;i++) for (int j=i+1;j<activeWater;j++) {
    float px = wx[j]-wx[i], py = wy[j]-wy[i], d2 = px*px + py*py;
    if (d2 < 0.0001f || d2 >= td2) continue;
    float d = sqrtf(d2), overlap = td-d, nx = px/d, ny = py/d, push = overlap * 0.46f;
    wx[i]-=nx*push; wy[i]-=ny*push; wx[j]+=nx*push; wy[j]+=ny*push;
  }
}
void viscosity() {
  float r = WR * 2.65f, r2 = r*r;
  for (int i=0;i<activeWater;i++) for (int j=i+1;j<activeWater;j++) {
    float px = wx[j]-wx[i], py = wy[j]-wy[i], d2 = px*px + py*py;
    if (d2 >= r2) continue;
    float vx = wvx[j]-wvx[i], vy = wvy[j]-wvy[i], k = 0.022f;
    wvx[i]+=vx*k; wvy[i]+=vy*k; wvx[j]-=vx*k; wvy[j]-=vy*k;
  }
}
void spawnDrop(float x, float y, float vx, float vy, float r) {
  for (int i=0;i<DROP_MAX;i++) if (!dropOn[i]) {
    dropOn[i] = true; dx[i]=x; dy[i]=y; dvx[i]=vx; dvy[i]=vy; dr[i]=r; dlife[i]=78; return;
  }
}
void splash(float fx, float fy, float strength) {
  if (activeWater <= 0) return;
  motorPulse(80);
  float len = sqrtf(fx*fx + fy*fy);
  float ux = 0, uy = -1;
  if (len > 0.05f) { ux = -fx/len; uy = -fy/len; }
  int ids[WATER_MAX]; float scores[WATER_MAX];
  for (int i=0;i<activeWater;i++) { ids[i]=i; scores[i] = wx[i]*ux + wy[i]*uy; }
  int top = min(8, activeWater);
  for (int a=0;a<top;a++) {
    int best=a; for (int b=a+1;b<activeWater;b++) if (scores[b] > scores[best]) best=b;
    float sv=scores[a]; scores[a]=scores[best]; scores[best]=sv;
    int iv=ids[a]; ids[a]=ids[best]; ids[best]=iv;
  }
  int count = constrain((int)(strength * 1.45f), 3, 7);
  for (int n=0;n<count;n++) {
    int id = ids[random(0, top)];
    float side = random(-100,101)/100.0f, sx = -uy, sy = ux;
    float launch = 0.90f + strength * 0.44f, spread = 0.48f + strength * 0.11f;
    spawnDrop(wx[id], wy[id], wvx[id] + ux*launch + sx*side*spread, wvy[id] + uy*launch + sy*side*spread, random(1,3));
    wvx[id] += ux * 0.14f; wvy[id] += uy * 0.14f;
  }
}
void updateDrops() {
  for (int i=0;i<DROP_MAX;i++) if (dropOn[i]) {
    dvx[i] += forceX*0.012f; dvy[i] += 0.070f + forceY*0.012f;
    dvx[i] *= 0.994f; dvy[i] *= 0.994f;
    dx[i] += dvx[i]; dy[i] += dvy[i]; dlife[i]--;
    if (dx[i] < 2) { dx[i]=2; dvx[i]*=-0.24f; }
    if (dx[i] > SCREEN_W-3) { dx[i]=SCREEN_W-3; dvx[i]*=-0.24f; }
    if (dy[i] < 2) { dy[i]=2; dvy[i]*=-0.20f; }
    for (int j=0;j<activeWater;j++) {
      float px = dx[i]-wx[j], py = dy[i]-wy[j];
      if (px*px + py*py < WR*WR*1.85f) { wvx[j]+=dvx[i]*0.045f; wvy[j]+=dvy[i]*0.045f; dropOn[i]=false; break; }
    }
    if (dy[i] > SCREEN_H || dlife[i] <= 0) dropOn[i]=false;
  }
}
void updateJoystickWaterForce() {
  float nx = deadzone(clampf((joyXRaw - joyCenterX) / 1800.0f, -1, 1), 0.14f);
  float ny = deadzone(clampf((joyYRaw - joyCenterY) / 1800.0f, -1, 1), 0.14f);
  joyForceX = joyForceX * 0.80f + JOY_X_SIGN * nx * 0.20f;
  joyForceY = joyForceY * 0.80f + JOY_Y_SIGN * ny * 0.20f;
}
void updateWater() {
  updateWaterAmount();
  if (!readMPU()) return;
  updateJoystickWaterForce();
  float tx = ax - biasAX, ty = ay - biasAY;
  if (fabsf(tx)<0.03f) tx=0; if (fabsf(ty)<0.03f) ty=0;
  float tiltX = clampf(TILT_X_SIGN * tx * 2.10f, -1, 1);
  float tiltY = clampf(TILT_Y_SIGN * ty * 2.10f, -1, 1);
  float targetX = tiltX + joyForceX * 0.75f;
  float targetY = tiltY + joyForceY * 0.75f;
  forceX = forceX * 0.84f + clampf(targetX, -1.35f, 1.35f) * 0.16f;
  forceY = forceY * 0.84f + clampf(targetY, -1.35f, 1.35f) * 0.16f;
  float gpx = clampf(gx * 0.008f, -1.3f, 1.3f), gpy = clampf(gy * 0.008f, -1.3f, 1.3f);
  for (int i=0;i<activeWater;i++) {
    wvx[i] += forceX*0.27f + gpx*0.18f; wvy[i] += forceY*0.27f + gpy*0.18f;
    wvx[i] *= 0.986f; wvy[i] *= 0.986f;
    wvx[i] = clampf(wvx[i], -4.2f, 4.2f); wvy[i] = clampf(wvy[i], -4.2f, 4.2f);
    wx[i] += wvx[i]; wy[i] += wvy[i]; constrainWater(i);
  }
  solveWaterPairs(); solveWaterPairs(); solveWaterPairs();
  for (int i=0;i<activeWater;i++) constrainWater(i);
  viscosity();
  float jerk = fabsf(ax-lastAX) + fabsf(ay-lastAY) + fabsf(az-lastAZ);
  lastAX=ax; lastAY=ay; lastAZ=az;
  float kick = fabsf(gx)*0.0028f + fabsf(gy)*0.0028f + fabsf(gz)*0.0018f;
  float s = jerk*6.5f + kick;
  if (splashCooldown>0) splashCooldown--;
  if (s > 1.25f && splashCooldown <= 0) { splash(forceX + gpx*0.35f, forceY + gpy*0.35f, s); splashCooldown = 9; }
}

// --------------------- drawing --------------------
void pushCanvas() { tft.drawRGBBitmap(0,0,canvas.getBuffer(),SCREEN_W,SCREEN_H); }
void centeredText(const char *s, int y, uint8_t size, uint16_t color) {
  canvas.setTextSize(size); canvas.setTextColor(color); canvas.setTextWrap(false);
  int16_t x1,y1; uint16_t w,h; canvas.getTextBounds(s,0,y,&x1,&y1,&w,&h);
  canvas.setCursor(max(0,(SCREEN_W-(int)w)/2), y); canvas.print(s);
}
void panel(int x,int y,int w,int h,uint16_t fill,uint16_t border) {
  canvas.fillRoundRect(x,y,w,h,5,fill); canvas.drawRoundRect(x,y,w,h,5,border);
}
void buildField() {
  memset(field,0,sizeof(field));
  int ks = kernelR*2+1;
  for (int i=0;i<activeWater;i++) {
    int cx = wx[i]/GRID_STEP, cy = wy[i]/GRID_STEP;
    for (int ky=-kernelR;ky<=kernelR;ky++) {
      int fy = cy+ky; if (fy<0 || fy>=FIELD_H) continue;
      for (int kx=-kernelR;kx<=kernelR;kx++) {
        int fx = cx+kx; if (fx<0 || fx>=FIELD_W) continue;
        uint16_t add = kernel[(ky+kernelR)*ks + (kx+kernelR)]; if (!add) continue;
        int id = fy*FIELD_W + fx; uint32_t v = field[id] + add; field[id] = v > 65535 ? 65535 : v;
      }
    }
  }
}
void drawWaterBody() {
  buildField();
  for (int gy=0;gy<FIELD_H;gy++) {
    int run=-1;
    for (int gx=0;gx<FIELD_W;gx++) {
      bool inside = field[gy*FIELD_W + gx] >= FIELD_THRESHOLD;
      if (inside && run<0) run=gx;
      bool last = gx == FIELD_W-1;
      if ((!inside || last) && run>=0) {
        int end = (inside && last) ? gx : gx-1;
        canvas.fillRect(run*GRID_STEP, gy*GRID_STEP, (end-run+1)*GRID_STEP, GRID_STEP, waterColor());
        run=-1;
      }
    }
  }
  for (int gy=0;gy<FIELD_H;gy++) for (int gx=0;gx<FIELD_W;gx++) {
    int id=gy*FIELD_W+gx; if (field[id] < FIELD_THRESHOLD) continue;
    bool top = gy==0 || field[(gy-1)*FIELD_W+gx] < FIELD_THRESHOLD;
    if (top) canvas.fillRect(gx*GRID_STEP, gy*GRID_STEP, GRID_STEP, 1, edgeColor());
  }
}
void drawDrops() {
  for (int i=0;i<DROP_MAX;i++) if (dropOn[i]) {
    canvas.fillCircle((int)dx[i],(int)dy[i],(int)dr[i],edgeColor());
    if (dr[i] >= 2) canvas.drawPixel((int)dx[i]-1,(int)dy[i]-1,WHITE);
  }
}
void drawWaterUI() {
  canvas.setTextSize(1); canvas.setTextWrap(false); canvas.setTextColor(MUTED);
  canvas.setCursor(5,6); canvas.print("Water");
  canvas.setCursor(5,116); canvas.print("JoySW: Back");
}
void drawWaterFrame() { canvas.fillScreen(BG); drawWaterBody(); drawDrops(); drawWaterUI(); pushCanvas(); }

const uint8_t MENU_COUNT = 3;
const char *menuItems[MENU_COUNT] = {"Water", "Gallery", "Settings"};
const char *menuSubs[MENU_COUNT] = {"Interactive liquid toy", "Coming soon", "Coming soon"};
int menuIndex = 0;
uint32_t nextMenuMove = 0;
void drawMenu() {
  canvas.fillScreen(BG);
  centeredText("ESP32Toy", 8, 2, WHITE);
  centeredText("Mini System", 28, 1, MUTED);
  for (int i=0;i<MENU_COUNT;i++) {
    int y = 46 + i*24; bool sel = i==menuIndex;
    panel(18,y,124,18,sel?PANEL2:PANEL,sel?waterColor():LINE);
    canvas.setTextSize(1); canvas.setTextColor(sel?WHITE:MUTED); canvas.setCursor(28,y+5); canvas.print(menuItems[i]);
    if (sel) canvas.fillCircle(128,y+9,3,waterColor());
  }
  centeredText(menuSubs[menuIndex], 118, 1, MUTED);
  pushCanvas();
}
void updateMenu() {
  uint32_t now = millis();
  if (now < nextMenuMove) return;
  int dyv = joyYRaw - joyCenterY;
  if (dyv > 900) { menuIndex = (menuIndex+1)%MENU_COUNT; nextMenuMove = now+220; motorPulse(45); }
  else if (dyv < -900) { menuIndex = (menuIndex+MENU_COUNT-1)%MENU_COUNT; nextMenuMove = now+220; motorPulse(45); }
}

// --------------------- boot animation --------------
void drawBoot(float t) {
  canvas.fillScreen(BG);
  float e = t*t*(3.0f - 2.0f*t);
  canvas.drawCircle(SCREEN_W/2,72,18,rgb565(20,45,70));
  canvas.drawCircle(SCREEN_W/2,72,26,rgb565(10,30,52));
  canvas.fillCircle((int)(30+(SCREEN_W/2-30)*e),(int)(-8+(62+8)*e),6,rgb565(45,160,255));
  canvas.fillCircle((int)(SCREEN_W/2),(int)(-16+(56+16)*e),7,rgb565(65,190,255));
  canvas.fillCircle((int)(130+(SCREEN_W/2-130)*e),(int)(-6+(66+6)*e),6,rgb565(90,220,255));
  if (t > 0.52f) { float q = clampf((t-0.52f)/0.48f,0,1); int r = 4 + q*16; canvas.fillCircle(SCREEN_W/2,72,r,rgb565(90,210,255)); canvas.drawCircle(SCREEN_W/2,72,r+2,rgb565(170,235,255)); }
  if (t > 0.73f) centeredText("ESP32 TOY", 104, 1, WHITE);
  pushCanvas();
}
void runBoot() {
  uint32_t s = millis();
  while (millis()-s < 1800) { drawBoot((millis()-s)/1800.0f); delay(20); }
  drawBoot(1.0f); delay(140);
}
void bootText(const char *s) { canvas.fillScreen(BG); centeredText(s, 60, 1, WHITE); pushCanvas(); }

// --------------------- input handling --------------
void updateInputs() {
  joyXRaw = analogRead(JOY_X_PIN); joyYRaw = analogRead(JOY_Y_PIN); joyPressed = digitalRead(JOY_SW_PIN)==LOW; potRaw = analogRead(POT_PIN);
  bool raw = digitalRead(KEY_PIN); uint32_t now = millis(); keyPressedEdge = false;
  if (raw != keyRawPrev) { keyRawPrev = raw; keyChangedAt = now; }
  if (now-keyChangedAt >= KEY_DEBOUNCE && raw != keyStable) { keyStable = raw; if (keyStable==LOW) keyPressedEdge = true; }
}
void nextWaterColor() { waterColorIndex = (waterColorIndex + 1) % COLOR_COUNT; motorPulse(150); }
void handleWaterControls() {
  if (keyPressedEdge) nextWaterColor();
  if (joyPressed && !joyPressedPrev) { mode = MENU; motorPulse(120); }
  joyPressedPrev = joyPressed;
}

// --------------------- setup / loop ----------------
uint32_t lastFrame = 0;
void setup() {
  Serial.begin(115200); delay(500);
  pinMode(TFT_BL,OUTPUT); digitalWrite(TFT_BL,HIGH);
  pinMode(POT_PIN,INPUT); pinMode(JOY_X_PIN,INPUT); pinMode(JOY_Y_PIN,INPUT);
  pinMode(JOY_SW_PIN,INPUT_PULLUP); pinMode(KEY_PIN,INPUT_PULLUP);
  pinMode(MOTOR_PIN,OUTPUT); motorWrite(false);
  screenSPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB); tft.setRotation(3); tft.fillScreen(ST77XX_BLACK);
  runBoot();
  bootText("CALIBRATING");
  initMPU(); calibrateMPU(); calibrateJoystick();
  potRaw = analogRead(POT_PIN); potFiltered = potRaw; updateWaterAmount(); rebuildKernel(); initWater();
  mode = MENU; drawMenu();
}
void loop() {
  updateInputs(); updateMotor(); uint32_t now = millis();
  if (mode == MENU) {
    updateMenu(); if (keyPressedEdge && menuIndex == 0) { mode = WATER; motorPulse(90); }
    if (now-lastFrame >= 20) { drawMenu(); lastFrame = now; }
    return;
  }
  if (mode == WATER) {
    handleWaterControls(); updateWater(); updateDrops();
    if (now-lastFrame >= 20) { drawWaterFrame(); lastFrame = now; }
  }
}
