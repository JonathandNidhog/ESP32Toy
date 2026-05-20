#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

// ============================================================
// ESP32Toy - Doom Raycaster Phase 2: Dual Stick Prototype
// Board: ESP32-S3 N16R8
// Screen: 1.8 inch ST7735, landscape rotation 3
//
// Existing / right stick:
//   X  -> IO16
//   Y  -> IO8
//   SW -> IO6
//
// New / left stick:
//   X  -> IO17
//   Y  -> IO18
//   SW -> IO13
//
// Controls:
//   New stick UP/DOWN    -> move forward / backward
//   New stick LEFT/RIGHT -> strafe left / right
//   Old stick LEFT/RIGHT -> turn camera left / right
//   Old stick UP/DOWN    -> smooth vertical look / horizon shift
//   A key IO15           -> fire test + motor pulse + muzzle flash
//   B key IO14           -> reset player position
//   Old joystick SW IO6  -> toggle minimap
//   New joystick SW IO13 -> sprint while held
//   Pot IO1              -> base move speed scale
//   RGB IO47             -> red Doom pulse / muzzle flash
// ============================================================

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

// ---------------- Display setup ----------------
#define SCREEN_W 160
#define SCREEN_H 128
#define DISPLAY_ROTATION 3

SPIClass screenSPI(FSPI);
Adafruit_ST7735 tft(&screenSPI, TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 canvas(SCREEN_W, SCREEN_H);
Adafruit_NeoPixel leds(RGB_COUNT, RGB_PIN, NEO_GRB + NEO_KHZ800);

// ============================================================
// Utility
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

// ============================================================
// Colors
// ============================================================
const uint16_t C_SKY_TOP = rgb565(15, 18, 30);
const uint16_t C_SKY_BOTTOM = rgb565(30, 20, 22);
const uint16_t C_FLOOR_TOP = rgb565(28, 18, 14);
const uint16_t C_FLOOR_BOTTOM = rgb565(8, 7, 8);
const uint16_t C_WHITE = ST77XX_WHITE;
const uint16_t C_MUTED = rgb565(160, 160, 160);
const uint16_t C_RED = rgb565(230, 40, 28);
const uint16_t C_YELLOW = rgb565(255, 210, 70);
const uint16_t C_HUD = rgb565(18, 10, 10);
const uint16_t C_CROSS = rgb565(255, 220, 120);
const uint16_t C_MAP_BG = rgb565(7, 7, 8);
const uint16_t C_MAP_WALL = rgb565(90, 28, 22);
const uint16_t C_GREEN = rgb565(80, 230, 110);

// ============================================================
// World map
// ============================================================
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

// Open cell: WORLD[1][2] == '0'
const float START_X = 2.5f;
const float START_Y = 1.5f;
const float START_A = 0.15f;

float playerX = START_X;
float playerY = START_Y;
float playerA = START_A;

// ============================================================
// Input state
// ============================================================
int rightCenterX = 2048;
int rightCenterY = 2048;
int leftCenterX = 2048;
int leftCenterY = 2048;

int rightRawX = 2048;
int rightRawY = 2048;
int leftRawX = 2048;
int leftRawY = 2048;
int potRaw = 2048;

float rightAxisX = 0.0f;
float rightAxisY = 0.0f;
float leftAxisX = 0.0f;
float leftAxisY = 0.0f;

float lastMoveAxis = 0.0f;
float lastStrafeAxis = 0.0f;
float lastTurnAxis = 0.0f;
float lastLookAxis = 0.0f;

// Direction knobs tied to RAW physical axes.
const float MOVE_SIGN = -1.0f;
const float STRAFE_SIGN = 1.0f;
const float TURN_SIGN = 1.0f;
const float LOOK_SIGN = -1.0f;

// Fake vertical look state. This is a classic raycaster-style horizon shift,
// not true 3D pitch, but it makes the right-stick Y direction meaningful.
float lookPitchPixels = 0.0f;
const float LOOK_PITCH_MAX = 18.0f;

bool keyAPrevRaw = HIGH;
bool keyBPrevRaw = HIGH;
bool rightSWPrevRaw = HIGH;
bool keyAStable = HIGH;
bool keyBStable = HIGH;
bool rightSWStable = HIGH;
bool keyAEdge = false;
bool keyBEdge = false;
bool rightSWEdge = false;
uint32_t keyAChangedMs = 0;
uint32_t keyBChangedMs = 0;
uint32_t rightSWChangedMs = 0;

bool leftSWPressed = false;
bool keyARawPressed = false;
uint32_t lastFireMs = 0;

// ============================================================
// Effects state
// ============================================================
bool minimapEnabled = true;
uint32_t muzzleUntilMs = 0;
uint32_t motorUntilMs = 0;
uint32_t lastLedMs = 0;
uint8_t ledPhase = 0;
uint32_t lastFrameMs = 0;

// ============================================================
// Motor / LEDs
// ============================================================
void motorWrite(bool on) {
  digitalWrite(MOTOR_PIN, MOTOR_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH));
}

void motorPulse(uint32_t durationMs) {
  uint32_t untilMs = millis() + durationMs;
  if (untilMs > motorUntilMs) motorUntilMs = untilMs;
}

void updateMotor() {
  motorWrite(millis() < motorUntilMs);
}

void ledAll(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < RGB_COUNT; i++) {
    leds.setPixelColor(i, leds.Color(r, g, b));
  }
  leds.show();
}

void updateLeds() {
  uint32_t now = millis();
  if (now < muzzleUntilMs) {
    ledAll(85, 30, 0);
    return;
  }

  if (now - lastLedMs < 70) return;
  lastLedMs = now;
  ledPhase += 6;

  for (int i = 0; i < RGB_COUNT; i++) {
    uint8_t wave = (ledPhase + i * 35) & 127;
    if (wave > 63) wave = 127 - wave;
    leds.setPixelColor(i, leds.Color(24 + wave / 2, 0, 0));
  }
  leds.show();
}

void fireNow() {
  muzzleUntilMs = millis() + 110;
  motorPulse(100);
  lastFireMs = millis();
}

// ============================================================
// Buttons / joystick reading
// ============================================================
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

void calibrateJoysticks() {
  long rX = 0;
  long rY = 0;
  long lX = 0;
  long lY = 0;

  for (int i = 0; i < 72; i++) {
    rX += analogRead(RIGHT_JOY_X_PIN);
    rY += analogRead(RIGHT_JOY_Y_PIN);
    lX += analogRead(LEFT_JOY_X_PIN);
    lY += analogRead(LEFT_JOY_Y_PIN);
    delay(4);
  }

  rightCenterX = rX / 72;
  rightCenterY = rY / 72;
  leftCenterX = lX / 72;
  leftCenterY = lY / 72;
}

void updateInputs() {
  rightRawX = analogRead(RIGHT_JOY_X_PIN);
  rightRawY = analogRead(RIGHT_JOY_Y_PIN);
  leftRawX = analogRead(LEFT_JOY_X_PIN);
  leftRawY = analogRead(LEFT_JOY_Y_PIN);
  potRaw = analogRead(POT_PIN);
  leftSWPressed = digitalRead(LEFT_JOY_SW_PIN) == LOW;
  keyARawPressed = digitalRead(KEY_A_PIN) == LOW;

  debounceButton(KEY_A_PIN, &keyAPrevRaw, &keyAStable, &keyAEdge, &keyAChangedMs);
  debounceButton(KEY_B_PIN, &keyBPrevRaw, &keyBStable, &keyBEdge, &keyBChangedMs);
  debounceButton(RIGHT_JOY_SW_PIN, &rightSWPrevRaw, &rightSWStable, &rightSWEdge, &rightSWChangedMs);

  if (keyAEdge) {
    fireNow();
  }

  if (keyARawPressed && millis() - lastFireMs > 220) {
    fireNow();
  }

  if (keyBEdge) {
    playerX = START_X;
    playerY = START_Y;
    playerA = START_A;
    lookPitchPixels = 0.0f;
    motorPulse(110);
  }

  if (rightSWEdge) {
    minimapEnabled = !minimapEnabled;
    motorPulse(65);
  }
}

void readDualStickAxes(float *moveAxis, float *strafeAxis, float *turnAxis, float *lookAxis) {
  rightAxisX = clampf((rightRawX - rightCenterX) / 1800.0f, -1.0f, 1.0f);
  rightAxisY = clampf((rightRawY - rightCenterY) / 1800.0f, -1.0f, 1.0f);
  leftAxisX = clampf((leftRawX - leftCenterX) / 1800.0f, -1.0f, 1.0f);
  leftAxisY = clampf((leftRawY - leftCenterY) / 1800.0f, -1.0f, 1.0f);

  rightAxisX = applyDeadzone(rightAxisX, 0.14f);
  rightAxisY = applyDeadzone(rightAxisY, 0.14f);
  leftAxisX = applyDeadzone(leftAxisX, 0.14f);
  leftAxisY = applyDeadzone(leftAxisY, 0.14f);

  *moveAxis = leftAxisY * MOVE_SIGN;
  *strafeAxis = leftAxisX * STRAFE_SIGN;
  *turnAxis = rightAxisX * TURN_SIGN;
  *lookAxis = rightAxisY * LOOK_SIGN;
}

// ============================================================
// Movement and collision
// ============================================================
bool mapSolid(float x, float y) {
  int mx = (int)x;
  int my = (int)y;
  if (mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_H) return true;
  return WORLD[my][mx] != '0';
}

void moveWithCollision(float deltaX, float deltaY) {
  float targetX = playerX + deltaX;
  float targetY = playerY + deltaY;

  if (!mapSolid(targetX, playerY)) playerX = targetX;
  if (!mapSolid(playerX, targetY)) playerY = targetY;
}

void updatePlayer(float dt) {
  float moveAxis = 0.0f;
  float strafeAxis = 0.0f;
  float turnAxis = 0.0f;
  float lookAxis = 0.0f;
  readDualStickAxes(&moveAxis, &strafeAxis, &turnAxis, &lookAxis);

  lastMoveAxis = moveAxis;
  lastStrafeAxis = strafeAxis;
  lastTurnAxis = turnAxis;
  lastLookAxis = lookAxis;

  float speedScale = 0.55f + (potRaw / 4095.0f) * 1.75f;
  if (leftSWPressed) speedScale *= 1.65f;

  float moveSpeed = 2.15f * speedScale;
  float turnSpeed = 2.20f;

  playerA += turnAxis * turnSpeed * dt;
  if (playerA < -PI) playerA += 2.0f * PI;
  if (playerA > PI) playerA -= 2.0f * PI;

  float targetPitch = lookAxis * LOOK_PITCH_MAX;
  float pitchBlend = clampf(dt * 10.0f, 0.0f, 1.0f);
  lookPitchPixels += (targetPitch - lookPitchPixels) * pitchBlend;

  float forwardX = cosf(playerA);
  float forwardY = sinf(playerA);
  float rightX = -sinf(playerA);
  float rightY = cosf(playerA);

  float deltaX = (forwardX * moveAxis + rightX * strafeAxis) * moveSpeed * dt;
  float deltaY = (forwardY * moveAxis + rightY * strafeAxis) * moveSpeed * dt;

  moveWithCollision(deltaX, deltaY);
}

// ============================================================
// Raycaster renderer
// ============================================================
uint16_t wallColor(float dist, bool side) {
  float shade = clampf(1.20f - dist * 0.10f, 0.18f, 1.0f);
  if (side) shade *= 0.78f;
  return rgb565((uint8_t)(170 * shade), (uint8_t)(35 * shade), (uint8_t)(26 * shade));
}

int currentHorizonY() {
  int horizon = SCREEN_H / 2 + (int)lookPitchPixels;
  if (horizon < 18) horizon = 18;
  if (horizon > SCREEN_H - 18) horizon = SCREEN_H - 18;
  return horizon;
}

void drawBackground() {
  int horizon = currentHorizonY();

  for (int y = 0; y < horizon; y++) {
    float denom = horizon > 0 ? (float)horizon : 1.0f;
    float t = y / denom;
    uint8_t r = (uint8_t)(15 + 15 * t);
    uint8_t g = (uint8_t)(18 + 4 * t);
    uint8_t b = (uint8_t)(30 - 8 * t);
    canvas.drawFastHLine(0, y, SCREEN_W, rgb565(r, g, b));
  }

  for (int y = horizon; y < SCREEN_H; y++) {
    float denom = SCREEN_H - horizon > 0 ? (float)(SCREEN_H - horizon) : 1.0f;
    float t = (y - horizon) / denom;
    uint8_t r = (uint8_t)(28 - 20 * t);
    uint8_t g = (uint8_t)(18 - 11 * t);
    uint8_t b = (uint8_t)(14 - 6 * t);
    canvas.drawFastHLine(0, y, SCREEN_W, rgb565(r, g, b));
  }
}

void renderRaycaster() {
  const float FOV = 1.05f;
  const int COL_STEP = 2;
  int horizon = currentHorizonY();

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
      } else if (WORLD[mapY][mapX] != '0') {
        hit = true;
      }
      guard++;
    }

    float perpDist;
    if (!side) perpDist = (mapX - playerX + (1 - stepX) * 0.5f) / rayDirX;
    else perpDist = (mapY - playerY + (1 - stepY) * 0.5f) / rayDirY;
    if (perpDist < 0.05f) perpDist = 0.05f;

    int lineH = (int)(SCREEN_H / perpDist);
    int drawStart = -lineH / 2 + horizon;
    int drawEnd = lineH / 2 + horizon;
    if (drawStart < 0) drawStart = 0;
    if (drawEnd >= SCREEN_H) drawEnd = SCREEN_H - 1;

    uint16_t color = wallColor(perpDist, side);
    canvas.fillRect(sx, drawStart, COL_STEP, drawEnd - drawStart + 1, color);
    if (drawStart > 0) canvas.drawFastHLine(sx, drawStart, COL_STEP, rgb565(245, 90, 70));
  }
}

void drawCrosshair() {
  int cx = SCREEN_W / 2;
  int cy = currentHorizonY();
  canvas.drawFastHLine(cx - 5, cy, 11, C_CROSS);
  canvas.drawFastVLine(cx, cy - 5, 11, C_CROSS);
  canvas.drawPixel(cx, cy, C_RED);
}

void drawGun() {
  int baseY = SCREEN_H - 1;
  bool flash = millis() < muzzleUntilMs;
  canvas.fillTriangle(54, baseY, 106, baseY, 90, 94, rgb565(35, 35, 40));
  canvas.fillTriangle(70, baseY, 94, baseY, 86, 86, rgb565(70, 72, 80));
  canvas.fillRect(80, 82, 9, 24, rgb565(78, 78, 84));
  canvas.fillRect(82, 78, 5, 7, rgb565(112, 112, 118));

  if (flash) {
    canvas.fillTriangle(76, 78, 92, 78, 84, 62, C_YELLOW);
    canvas.drawLine(84, 62, 84, 48, C_YELLOW);
  }
}

void drawMinimap() {
  if (!minimapEnabled) return;

  const int scale = 3;
  const int ox = SCREEN_W - MAP_W * scale - 5;
  const int oy = 5;

  canvas.fillRect(ox - 2, oy - 2, MAP_W * scale + 4, MAP_H * scale + 4, C_MAP_BG);
  canvas.drawRect(ox - 2, oy - 2, MAP_W * scale + 4, MAP_H * scale + 4, rgb565(115, 30, 25));

  for (int y = 0; y < MAP_H; y++) {
    for (int x = 0; x < MAP_W; x++) {
      if (WORLD[y][x] != '0') {
        canvas.fillRect(ox + x * scale, oy + y * scale, scale, scale, C_MAP_WALL);
      }
    }
  }

  int px = ox + (int)(playerX * scale);
  int py = oy + (int)(playerY * scale);
  canvas.fillCircle(px, py, 2, C_YELLOW);
  canvas.drawLine(px, py, px + (int)(cosf(playerA) * 7), py + (int)(sinf(playerA) * 7), C_YELLOW);
}

void drawHud() {
  canvas.fillRect(0, 0, 132, 43, C_HUD);
  canvas.setTextSize(1);
  canvas.setTextWrap(false);

  canvas.setTextColor(C_WHITE);
  canvas.setCursor(5, 4);
  canvas.print("DOOM P2 LOOK Y");

  canvas.setTextColor(C_MUTED);
  canvas.setCursor(5, 13);
  canvas.print("M");
  canvas.print(lastMoveAxis, 1);
  canvas.print(" S");
  canvas.print(lastStrafeAxis, 1);
  canvas.print(" T");
  canvas.print(lastTurnAxis, 1);
  canvas.print(" L");
  canvas.print(lastLookAxis, 1);

  canvas.setCursor(5, 21);
  canvas.print("LX");
  canvas.print(leftAxisX, 1);
  canvas.print(" LY");
  canvas.print(leftAxisY, 1);
  canvas.print(" RX");
  canvas.print(rightAxisX, 1);

  canvas.setCursor(5, 29);
  canvas.print("RY");
  canvas.print(rightAxisY, 1);
  canvas.print(" P");
  canvas.print((int)lookPitchPixels);
  canvas.print(" ");
  canvas.setTextColor(keyARawPressed ? C_GREEN : C_MUTED);
  canvas.print(keyARawPressed ? "A:DOWN" : "A:UP");

  canvas.setCursor(5, 37);
  canvas.setTextColor(C_MUTED);
  canvas.print(leftSWPressed ? "SPRINT" : "MOVE");

  canvas.fillRect(0, SCREEN_H - 11, SCREEN_W, 11, rgb565(8, 5, 5));
  canvas.setTextColor(C_MUTED);
  canvas.setCursor(3, SCREEN_H - 9);
  canvas.print("L MOVE  R LOOK  A FIRE  B RESET");
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

// ============================================================
// Boot screen
// ============================================================
void drawBoot() {
  canvas.fillScreen(rgb565(6, 3, 3));
  canvas.setTextWrap(false);
  canvas.setTextSize(2);
  canvas.setTextColor(C_RED);
  canvas.setCursor(17, 34);
  canvas.print("DOOM PHASE 2");
  canvas.setTextSize(1);
  canvas.setTextColor(C_MUTED);
  canvas.setCursor(23, 61);
  canvas.print("DUAL STICK + LOOK Y");
  canvas.setCursor(25, 77);
  canvas.print("KEEP BOTH STICKS CENTERED");
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), SCREEN_W, SCREEN_H);
  ledAll(45, 0, 0);
  delay(900);
}

// ============================================================
// Setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);

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
  tft.setRotation(DISPLAY_ROTATION);
  tft.fillScreen(ST77XX_BLACK);

  drawBoot();
  calibrateJoysticks();
  lastFrameMs = millis();
}

void loop() {
  uint32_t now = millis();
  uint32_t deltaMs = now - lastFrameMs;

  updateInputs();
  updateMotor();
  updateLeds();

  if (deltaMs < 16) return;
  lastFrameMs = now;

  float dt = clampf(deltaMs / 1000.0f, 0.0f, 0.05f);
  updatePlayer(dt);
  drawFrame();
}
