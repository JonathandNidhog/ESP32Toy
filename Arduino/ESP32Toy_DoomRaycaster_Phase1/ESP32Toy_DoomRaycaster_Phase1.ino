#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

// ============================================================
// ESP32Toy - Doom Phase 1 Raycaster Prototype
// Board: ESP32-S3 N16R8
// Screen: 1.8 inch ST7735, landscape rotation 3
// Controls:
//   Joystick Y  -> Move forward / backward
//   Joystick X  -> Turn left / right
//   A key IO15  -> Fire test + motor pulse
//   B key IO14  -> Reset player position
//   Joy SW IO6  -> Toggle minimap
//   Pot IO1     -> Move speed scale
//   RGB IO47    -> Doom red pulse / muzzle flash
// ============================================================

#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC    9
#define TFT_BL   21
#define TFT_RST  -1

#define POT_PIN     1
#define JOY_X_PIN  16
#define JOY_Y_PIN   8
#define JOY_SW_PIN  6
#define MOTOR_PIN   7
#define KEY_A_PIN  15
#define KEY_B_PIN  14
#define RGB_PIN    47
#define RGB_COUNT   4

#define MOTOR_ACTIVE_HIGH true

#define SCREEN_W 160
#define SCREEN_H 128
#define DISPLAY_ROTATION 3

SPIClass screenSPI(FSPI);
Adafruit_ST7735 tft(&screenSPI, TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 canvas(SCREEN_W, SCREEN_H);
Adafruit_NeoPixel leds(RGB_COUNT, RGB_PIN, NEO_GRB + NEO_KHZ800);

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

float deadzone(float v, float d) {
  if (fabsf(v) <= d) return 0.0f;
  float s = v >= 0.0f ? 1.0f : -1.0f;
  return s * clampf((fabsf(v) - d) / (1.0f - d), 0.0f, 1.0f);
}

const uint16_t C_SKY_TOP = rgb565(15, 18, 30);
const uint16_t C_SKY_BOTTOM = rgb565(28, 18, 22);
const uint16_t C_FLOOR_TOP = rgb565(28, 18, 14);
const uint16_t C_FLOOR_BOTTOM = rgb565(8, 7, 8);
const uint16_t C_WHITE = ST77XX_WHITE;
const uint16_t C_MUTED = rgb565(165, 165, 165);
const uint16_t C_RED = rgb565(230, 40, 28);
const uint16_t C_YELLOW = rgb565(255, 210, 70);
const uint16_t C_HUD = rgb565(18, 10, 10);
const uint16_t C_CROSS = rgb565(255, 220, 120);

const int MAP_W = 16;
const int MAP_H = 16;
const char WORLD[MAP_H][MAP_W + 1] = {
  "1111111111111111",
  "1000000000000001",
  "1011110111110101",
  "1000010100010101",
  "1111010101010101",
  "1001010001010001",
  "1011011111011101",
  "1010000000010001",
  "1010111111010111",
  "1000100000010001",
  "1110101111110101",
  "1000101000000101",
  "1011101011111101",
  "1000001000000001",
  "1000000000000001",
  "1111111111111111"
};

// IMPORTANT:
// The previous prototype spawned at 2.5, 2.5, which is inside a wall cell
// in the current map. That caused raycasts/collision to behave incorrectly.
// This spawn is now in an actual open tile: WORLD[1][2] == '0'.
float playerX = 2.5f;
float playerY = 1.5f;
float playerA = 0.15f;
const float START_X = 2.5f;
const float START_Y = 1.5f;
const float START_A = 0.15f;

int joyCenterX = 2048;
int joyCenterY = 2048;
int joyRawX = 2048;
int joyRawY = 2048;
int potRaw = 2048;

bool aPrevRaw = HIGH;
bool bPrevRaw = HIGH;
bool swPrevRaw = HIGH;
bool aStable = HIGH;
bool bStable = HIGH;
bool swStable = HIGH;
bool aEdge = false;
bool bEdge = false;
bool swEdge = false;
uint32_t aChangedMs = 0;
uint32_t bChangedMs = 0;
uint32_t swChangedMs = 0;

bool showMinimap = true;
uint32_t muzzleUntil = 0;
uint32_t motorUntil = 0;
uint32_t lastFrameMs = 0;
uint32_t lastLedMs = 0;
uint8_t ledPhase = 0;
float fpsSmooth = 0.0f;

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

void ledAll(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < RGB_COUNT; i++) leds.setPixelColor(i, leds.Color(r, g, b));
  leds.show();
}

void updateLeds() {
  uint32_t now = millis();
  if (now < muzzleUntil) {
    ledAll(85, 30, 0);
    return;
  }

  if (now - lastLedMs < 70) return;
  lastLedMs = now;
  ledPhase += 6;
  for (int i = 0; i < RGB_COUNT; i++) {
    uint8_t v = (ledPhase + i * 35) & 127;
    if (v > 63) v = 127 - v;
    leds.setPixelColor(i, leds.Color(24 + v / 2, 0, 0));
  }
  leds.show();
}

void mapBaseVectorToScreen(float bx, float by, float *ox, float *oy) {
  // Rotation 3, same screen orientation as current LiquidOS build.
  *ox = -by;
  *oy = bx;
}

void readJoystickScreen(float *outX, float *outY) {
  joyRawX = analogRead(JOY_X_PIN);
  joyRawY = analogRead(JOY_Y_PIN);
  float nx = clampf((joyRawX - joyCenterX) / 1800.0f, -1.0f, 1.0f);
  float ny = clampf((joyRawY - joyCenterY) / 1800.0f, -1.0f, 1.0f);
  nx = deadzone(nx, 0.14f);
  ny = deadzone(ny, 0.14f);
  mapBaseVectorToScreen(nx, ny, outX, outY);
}

void debounceButton(int pin, bool *prevRaw, bool *stable, bool *edge, uint32_t *changedMs) {
  bool raw = digitalRead(pin);
  uint32_t now = millis();
  *edge = false;
  if (raw != *prevRaw) {
    *prevRaw = raw;
    *changedMs = now;
  }
  if (now - *changedMs >= 35 && raw != *stable) {
    *stable = raw;
    if (*stable == LOW) *edge = true;
  }
}

void updateInputs() {
  potRaw = analogRead(POT_PIN);
  debounceButton(KEY_A_PIN, &aPrevRaw, &aStable, &aEdge, &aChangedMs);
  debounceButton(KEY_B_PIN, &bPrevRaw, &bStable, &bEdge, &bChangedMs);
  debounceButton(JOY_SW_PIN, &swPrevRaw, &swStable, &swEdge, &swChangedMs);

  if (aEdge) {
    muzzleUntil = millis() + 95;
    motorPulse(85);
  }
  if (bEdge) {
    playerX = START_X;
    playerY = START_Y;
    playerA = START_A;
    motorPulse(110);
  }
  if (swEdge) {
    showMinimap = !showMinimap;
    motorPulse(65);
  }
}

bool mapSolid(float x, float y) {
  int mx = (int)x;
  int my = (int)y;
  if (mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_H) return true;
  return WORLD[my][mx] != '0';
}

void movePlayer(float dt) {
  float joyX = 0.0f;
  float joyY = 0.0f;
  readJoystickScreen(&joyX, &joyY);

  // In screen space after rotation mapping:
  // joystick vertical controls forward/back, horizontal controls turning.
  float moveAxis = -joyY;
  float turnAxis = joyX;

  float speedScale = 0.55f + (potRaw / 4095.0f) * 1.75f;
  float moveSpeed = 2.1f * speedScale;
  float turnSpeed = 2.25f;

  playerA += turnAxis * turnSpeed * dt;
  if (playerA < -PI) playerA += 2.0f * PI;
  if (playerA > PI) playerA -= 2.0f * PI;

  float step = moveAxis * moveSpeed * dt;
  float nx = playerX + cosf(playerA) * step;
  float ny = playerY + sinf(playerA) * step;

  // Separate axis collision gives smoother sliding against walls.
  if (!mapSolid(nx, playerY)) playerX = nx;
  if (!mapSolid(playerX, ny)) playerY = ny;
}

uint16_t wallColorForCell(char cell, float dist, bool side) {
  uint8_t baseR = 170;
  uint8_t baseG = 35;
  uint8_t baseB = 26;
  if (cell == '2') { baseR = 120; baseG = 70; baseB = 35; }
  float shade = clampf(1.20f - dist * 0.10f, 0.18f, 1.0f);
  if (side) shade *= 0.78f;
  return rgb565((uint8_t)(baseR * shade), (uint8_t)(baseG * shade), (uint8_t)(baseB * shade));
}

void drawBackground() {
  for (int y = 0; y < SCREEN_H / 2; y++) {
    float t = y / (float)(SCREEN_H / 2);
    uint8_t r = (uint8_t)(15 + 15 * t);
    uint8_t g = (uint8_t)(18 + 4 * t);
    uint8_t b = (uint8_t)(30 - 8 * t);
    canvas.drawFastHLine(0, y, SCREEN_W, rgb565(r, g, b));
  }
  for (int y = SCREEN_H / 2; y < SCREEN_H; y++) {
    float t = (y - SCREEN_H / 2) / (float)(SCREEN_H / 2);
    uint8_t r = (uint8_t)(28 - 20 * t);
    uint8_t g = (uint8_t)(18 - 11 * t);
    uint8_t b = (uint8_t)(14 - 6 * t);
    canvas.drawFastHLine(0, y, SCREEN_W, rgb565(r, g, b));
  }
}

void renderRaycaster() {
  const float FOV = 1.05f; // about 60 degrees
  const int COL_STEP = 2;

  for (int sx = 0; sx < SCREEN_W; sx += COL_STEP) {
    float cameraX = 2.0f * (sx / (float)SCREEN_W) - 1.0f;
    float rayA = playerA + cameraX * (FOV * 0.5f);
    float rayDirX = cosf(rayA);
    float rayDirY = sinf(rayA);

    int mapX = (int)playerX;
    int mapY = (int)playerY;

    float deltaDistX = fabsf(rayDirX) < 0.0001f ? 1e30f : fabsf(1.0f / rayDirX);
    float deltaDistY = fabsf(rayDirY) < 0.0001f ? 1e30f : fabsf(1.0f / rayDirY);
    float sideDistX;
    float sideDistY;
    int stepX;
    int stepY;

    if (rayDirX < 0) {
      stepX = -1;
      sideDistX = (playerX - mapX) * deltaDistX;
    } else {
      stepX = 1;
      sideDistX = (mapX + 1.0f - playerX) * deltaDistX;
    }

    if (rayDirY < 0) {
      stepY = -1;
      sideDistY = (playerY - mapY) * deltaDistY;
    } else {
      stepY = 1;
      sideDistY = (mapY + 1.0f - playerY) * deltaDistY;
    }

    bool hit = false;
    bool side = false;
    char cell = '1';
    int guard = 0;
    while (!hit && guard < 64) {
      if (sideDistX < sideDistY) {
        sideDistX += deltaDistX;
        mapX += stepX;
        side = false;
      } else {
        sideDistY += deltaDistY;
        mapY += stepY;
        side = true;
      }
      if (mapX < 0 || mapX >= MAP_W || mapY < 0 || mapY >= MAP_H) {
        hit = true;
        cell = '1';
      } else if (WORLD[mapY][mapX] != '0') {
        hit = true;
        cell = WORLD[mapY][mapX];
      }
      guard++;
    }

    float perpDist;
    if (!side) perpDist = (mapX - playerX + (1 - stepX) * 0.5f) / rayDirX;
    else perpDist = (mapY - playerY + (1 - stepY) * 0.5f) / rayDirY;
    if (perpDist < 0.05f) perpDist = 0.05f;

    int lineH = (int)(SCREEN_H / perpDist);
    int drawStart = -lineH / 2 + SCREEN_H / 2;
    int drawEnd = lineH / 2 + SCREEN_H / 2;
    if (drawStart < 0) drawStart = 0;
    if (drawEnd >= SCREEN_H) drawEnd = SCREEN_H - 1;

    uint16_t wc = wallColorForCell(cell, perpDist, side);
    canvas.fillRect(sx, drawStart, COL_STEP, drawEnd - drawStart + 1, wc);

    if (drawStart > 0) canvas.drawFastHLine(sx, drawStart, COL_STEP, rgb565(245, 90, 70));
  }
}

void drawGun() {
  int baseY = SCREEN_H - 1;
  bool flash = millis() < muzzleUntil;
  canvas.fillTriangle(54, baseY, 106, baseY, 90, 94, rgb565(35, 35, 40));
  canvas.fillTriangle(70, baseY, 94, baseY, 86, 86, rgb565(70, 72, 80));
  canvas.fillRect(80, 82, 9, 24, rgb565(78, 78, 84));
  canvas.fillRect(82, 78, 5, 7, rgb565(112, 112, 118));
  if (flash) {
    canvas.fillTriangle(76, 78, 92, 78, 84, 62, C_YELLOW);
    canvas.drawLine(84, 62, 84, 48, C_YELLOW);
  }
}

void drawCrosshair() {
  int cx = SCREEN_W / 2;
  int cy = SCREEN_H / 2;
  canvas.drawFastHLine(cx - 5, cy, 11, C_CROSS);
  canvas.drawFastVLine(cx, cy - 5, 11, C_CROSS);
  canvas.drawPixel(cx, cy, C_RED);
}

void drawMinimap() {
  if (!showMinimap) return;
  const int scale = 3;
  const int ox = SCREEN_W - MAP_W * scale - 5;
  const int oy = 5;
  canvas.fillRect(ox - 2, oy - 2, MAP_W * scale + 4, MAP_H * scale + 4, rgb565(7, 7, 8));
  canvas.drawRect(ox - 2, oy - 2, MAP_W * scale + 4, MAP_H * scale + 4, rgb565(115, 30, 25));
  for (int y = 0; y < MAP_H; y++) {
    for (int x = 0; x < MAP_W; x++) {
      if (WORLD[y][x] != '0') canvas.fillRect(ox + x * scale, oy + y * scale, scale, scale, rgb565(90, 28, 22));
    }
  }
  int px = ox + (int)(playerX * scale);
  int py = oy + (int)(playerY * scale);
  canvas.fillCircle(px, py, 2, C_YELLOW);
  canvas.drawLine(px, py, px + (int)(cosf(playerA) * 7), py + (int)(sinf(playerA) * 7), C_YELLOW);
}

void drawHud() {
  canvas.fillRect(0, 0, 84, 18, C_HUD);
  canvas.setTextSize(1);
  canvas.setTextColor(C_WHITE);
  canvas.setCursor(5, 4);
  canvas.print("DOOM P1");
  canvas.setTextColor(C_MUTED);
  canvas.setCursor(5, 12);
  canvas.print("SPD ");
  int speedPct = (int)(55 + (potRaw / 4095.0f) * 175);
  canvas.print(speedPct);
  canvas.print("%");

  canvas.fillRect(0, SCREEN_H - 11, SCREEN_W, 11, rgb565(8, 5, 5));
  canvas.setTextColor(C_MUTED);
  canvas.setCursor(4, SCREEN_H - 9);
  canvas.print("A FIRE  B RESET  SW MAP");
}

void drawFrame() {
  drawBackground();
  renderRaycaster();
  drawCrosshair();
  drawGun();
  drawMinimap();
  drawHud();
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), SCREEN_W, SCREEN_H);
}

void calibrateJoystick() {
  long sx = 0;
  long sy = 0;
  for (int i = 0; i < 64; i++) {
    sx += analogRead(JOY_X_PIN);
    sy += analogRead(JOY_Y_PIN);
    delay(4);
  }
  joyCenterX = sx / 64;
  joyCenterY = sy / 64;
}

void drawBoot() {
  canvas.fillScreen(rgb565(6, 3, 3));
  canvas.setTextWrap(false);
  canvas.setTextSize(2);
  canvas.setTextColor(C_RED);
  canvas.setCursor(21, 38);
  canvas.print("DOOM PHASE 1");
  canvas.setTextSize(1);
  canvas.setTextColor(C_MUTED);
  canvas.setCursor(39, 68);
  canvas.print("RAYCASTER BOOT");
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), SCREEN_W, SCREEN_H);
  ledAll(45, 0, 0);
  delay(800);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  pinMode(POT_PIN, INPUT);
  pinMode(JOY_X_PIN, INPUT);
  pinMode(JOY_Y_PIN, INPUT);
  pinMode(JOY_SW_PIN, INPUT_PULLUP);
  pinMode(KEY_A_PIN, INPUT_PULLUP);
  pinMode(KEY_B_PIN, INPUT_PULLUP);
  pinMode(MOTOR_PIN, OUTPUT);
  motorWrite(false);

  leds.begin();
  leds.setBrightness(32);
  ledAll(0, 0, 0);

  screenSPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(DISPLAY_ROTATION);
  tft.fillScreen(ST77XX_BLACK);

  drawBoot();
  calibrateJoystick();
  lastFrameMs = millis();
}

void loop() {
  uint32_t now = millis();
  uint32_t deltaMs = now - lastFrameMs;
  if (deltaMs < 16) {
    updateInputs();
    updateMotor();
    updateLeds();
    return;
  }
  lastFrameMs = now;
  float dt = clampf(deltaMs / 1000.0f, 0.0f, 0.05f);

  updateInputs();
  updateMotor();
  updateLeds();
  movePlayer(dt);
  drawFrame();
}
