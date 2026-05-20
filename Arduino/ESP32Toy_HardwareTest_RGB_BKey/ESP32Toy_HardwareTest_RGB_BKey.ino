#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_NeoPixel.h>

// ============================================================
// ESP32Toy Hardware Test - RGB LED + B Key
// Target: ESP32-S3 N16R8
// ============================================================

// -------- ST7735 1.8 inch screen --------
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC    9
#define TFT_BL   21
#define TFT_RST  -1

// -------- Existing controls --------
#define POT_PIN     1
#define JOY_X_PIN  16
#define JOY_Y_PIN   8
#define JOY_SW_PIN  6
#define KEY_A_PIN  15

// -------- New B key --------
#define KEY_B_PIN  14

// -------- New RGB LED board --------
#define RGB_LED_PIN   47
#define RGB_LED_COUNT  4

#define SCREEN_W 160
#define SCREEN_H 128

SPIClass screenSPI(FSPI);
Adafruit_ST7735 tft(&screenSPI, TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 canvas(SCREEN_W, SCREEN_H);
Adafruit_NeoPixel pixels(RGB_LED_COUNT, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

const uint16_t C_BG = rgb565(4, 8, 18);
const uint16_t C_PANEL = rgb565(14, 24, 42);
const uint16_t C_WHITE = ST77XX_WHITE;
const uint16_t C_MUTED = rgb565(150, 170, 190);
const uint16_t C_GREEN = rgb565(80, 240, 120);
const uint16_t C_RED = rgb565(255, 80, 80);
const uint16_t C_BLUE = rgb565(90, 190, 255);

bool aPrev = HIGH;
bool bPrev = HIGH;
bool swPrev = HIGH;
bool aPressedEdge = false;
bool bPressedEdge = false;
bool swPressedEdge = false;

uint8_t colorIndex = 0;
bool rainbowMode = false;
uint8_t rainbowOffset = 0;
uint32_t lastRainbowMs = 0;

uint32_t colorTable[8];

void fillLeds(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < RGB_LED_COUNT; i++) {
    pixels.setPixelColor(i, pixels.Color(r, g, b));
  }
  pixels.show();
}

uint32_t wheel(byte pos) {
  pos = 255 - pos;
  if (pos < 85) return pixels.Color(255 - pos * 3, 0, pos * 3);
  if (pos < 170) {
    pos -= 85;
    return pixels.Color(0, pos * 3, 255 - pos * 3);
  }
  pos -= 170;
  return pixels.Color(pos * 3, 255 - pos * 3, 0);
}

void updateRainbow() {
  if (!rainbowMode) return;
  if (millis() - lastRainbowMs < 35) return;

  int pot = analogRead(POT_PIN);
  uint8_t brightness = map(pot, 0, 4095, 4, 80);
  pixels.setBrightness(brightness);

  for (int i = 0; i < RGB_LED_COUNT; i++) {
    pixels.setPixelColor(i, wheel(rainbowOffset + i * 64));
  }
  pixels.show();
  rainbowOffset += 3;
  lastRainbowMs = millis();
}

void applyStaticColor() {
  rainbowMode = false;
  int pot = analogRead(POT_PIN);
  uint8_t brightness = map(pot, 0, 4095, 4, 80);
  pixels.setBrightness(brightness);

  uint32_t c = colorTable[colorIndex];
  for (int i = 0; i < RGB_LED_COUNT; i++) pixels.setPixelColor(i, c);
  pixels.show();
}

void updateButtons() {
  bool aNow = digitalRead(KEY_A_PIN);
  bool bNow = digitalRead(KEY_B_PIN);
  bool swNow = digitalRead(JOY_SW_PIN);

  aPressedEdge = (aPrev == HIGH && aNow == LOW);
  bPressedEdge = (bPrev == HIGH && bNow == LOW);
  swPressedEdge = (swPrev == HIGH && swNow == LOW);

  aPrev = aNow;
  bPrev = bNow;
  swPrev = swNow;

  if (aPressedEdge) {
    colorIndex = (colorIndex + 1) % 8;
    applyStaticColor();
  }

  if (bPressedEdge) {
    rainbowMode = false;
    fillLeds(0, 0, 0);
  }

  if (swPressedEdge) {
    rainbowMode = !rainbowMode;
    if (!rainbowMode) applyStaticColor();
  }
}

void drawLabelValue(const char *label, const char *value, int x, int y, uint16_t valueColor) {
  canvas.setTextSize(1);
  canvas.setTextColor(C_MUTED);
  canvas.setCursor(x, y);
  canvas.print(label);
  canvas.setTextColor(valueColor);
  canvas.print(value);
}

void drawScreen() {
  canvas.fillScreen(C_BG);

  canvas.setTextSize(2);
  canvas.setTextColor(C_WHITE);
  canvas.setCursor(10, 8);
  canvas.print("RGB + B TEST");

  canvas.fillRoundRect(8, 32, 144, 66, 5, C_PANEL);

  char buf[24];
  snprintf(buf, sizeof(buf), "%s", digitalRead(KEY_A_PIN) == LOW ? "DOWN" : "UP");
  drawLabelValue("A IO15: ", buf, 16, 42, digitalRead(KEY_A_PIN) == LOW ? C_GREEN : C_WHITE);

  snprintf(buf, sizeof(buf), "%s", digitalRead(KEY_B_PIN) == LOW ? "DOWN" : "UP");
  drawLabelValue("B IO14: ", buf, 16, 54, digitalRead(KEY_B_PIN) == LOW ? C_GREEN : C_WHITE);

  snprintf(buf, sizeof(buf), "%s", digitalRead(JOY_SW_PIN) == LOW ? "DOWN" : "UP");
  drawLabelValue("JOY SW: ", buf, 16, 66, digitalRead(JOY_SW_PIN) == LOW ? C_GREEN : C_WHITE);

  snprintf(buf, sizeof(buf), "%4d %4d", analogRead(JOY_X_PIN), analogRead(JOY_Y_PIN));
  drawLabelValue("JOY X/Y: ", buf, 16, 78, C_BLUE);

  snprintf(buf, sizeof(buf), "%s", rainbowMode ? "RAINBOW" : "STATIC/OFF");
  drawLabelValue("LED: ", buf, 16, 90, rainbowMode ? C_GREEN : C_WHITE);

  canvas.setTextSize(1);
  canvas.setTextColor(C_MUTED);
  canvas.setCursor(10, 108);
  canvas.print("A=color  B=off  JoySW=rainbow");
  canvas.setCursor(10, 118);
  canvas.print("Pot=LED brightness");

  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), SCREEN_W, SCREEN_H);
}

void setup() {
  Serial.begin(115200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  pinMode(POT_PIN, INPUT);
  pinMode(JOY_X_PIN, INPUT);
  pinMode(JOY_Y_PIN, INPUT);
  pinMode(JOY_SW_PIN, INPUT_PULLUP);
  pinMode(KEY_A_PIN, INPUT_PULLUP);
  pinMode(KEY_B_PIN, INPUT_PULLUP);

  colorTable[0] = pixels.Color(40, 0, 0);
  colorTable[1] = pixels.Color(40, 12, 0);
  colorTable[2] = pixels.Color(35, 35, 0);
  colorTable[3] = pixels.Color(0, 40, 0);
  colorTable[4] = pixels.Color(0, 30, 30);
  colorTable[5] = pixels.Color(0, 0, 45);
  colorTable[6] = pixels.Color(28, 0, 36);
  colorTable[7] = pixels.Color(40, 40, 40);

  pixels.begin();
  pixels.setBrightness(24);
  applyStaticColor();

  screenSPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);

  drawScreen();
}

void loop() {
  updateButtons();
  updateRainbow();

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 50) {
    drawScreen();
    lastDraw = millis();
  }
}
