#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <math.h>
#include <string.h>
#include "driver/i2s.h"

// ============================================================
// ESP32Toy Hardware Test - I2S Speaker Output + Pot Volume
// Board: ESP32-S3 N16R8
// Audio power: VCC -> 3V3, GND -> GND
// Audio pins:
//   LRCLK -> IO40
//   BCLK  -> IO39
//   SDA   -> IO41
// Controls:
//   Pot IO1      -> volume
//   A IO15       -> melody
//   B IO14       -> low beep
//   JoySW IO6    -> sweep
// ============================================================

#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC    9
#define TFT_BL   21
#define TFT_RST  -1

#define POT_PIN 1
#define JOY_SW_PIN 6
#define KEY_A_PIN 15
#define KEY_B_PIN 14

#define AUDIO_LRCLK_PIN 40
#define AUDIO_BCLK_PIN  39
#define AUDIO_SDATA_PIN 41

#define SCREEN_W 160
#define SCREEN_H 128

#ifndef I2S_COMM_FORMAT_STAND_I2S
#define I2S_COMM_FORMAT_STAND_I2S I2S_COMM_FORMAT_I2S
#endif

SPIClass screenSPI(FSPI);
Adafruit_ST7735 tft(&screenSPI, TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 canvas(SCREEN_W, SCREEN_H);

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

const uint16_t C_BG = rgb565(4, 8, 18);
const uint16_t C_PANEL = rgb565(14, 24, 42);
const uint16_t C_WHITE = ST77XX_WHITE;
const uint16_t C_MUTED = rgb565(150, 170, 190);
const uint16_t C_GREEN = rgb565(80, 240, 120);
const uint16_t C_BLUE = rgb565(90, 190, 255);
const uint16_t C_RED = rgb565(255, 80, 80);
const uint16_t C_YELLOW = rgb565(255, 220, 80);

bool audioReady = false;
char lastAction[32] = "Booting";

bool aPrev = HIGH;
bool bPrev = HIGH;
bool swPrev = HIGH;

int potRaw = 0;
int volumePercent = 100;
int outputAmplitude = 30000;

void pushCanvas() {
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), SCREEN_W, SCREEN_H);
}

void setLastAction(const char *text) {
  strncpy(lastAction, text, sizeof(lastAction) - 1);
  lastAction[sizeof(lastAction) - 1] = '\0';
}

void updateVolumeFromPot() {
  potRaw = analogRead(POT_PIN);
  volumePercent = map(potRaw, 0, 4095, 0, 100);
  volumePercent = constrain(volumePercent, 0, 100);

  if (volumePercent <= 0) {
    outputAmplitude = 0;
  } else {
    outputAmplitude = map(volumePercent, 1, 100, 1200, 30000);
  }
}

void drawScreen() {
  updateVolumeFromPot();
  canvas.fillScreen(C_BG);

  canvas.setTextSize(2);
  canvas.setTextColor(C_WHITE);
  canvas.setCursor(10, 8);
  canvas.print("I2S VOLUME TEST");

  canvas.fillRoundRect(8, 34, 144, 66, 5, C_PANEL);
  canvas.setTextSize(1);

  canvas.setTextColor(C_MUTED);
  canvas.setCursor(16, 44);
  canvas.print("Audio init: ");
  canvas.setTextColor(audioReady ? C_GREEN : C_RED);
  canvas.print(audioReady ? "OK" : "FAIL");

  canvas.setTextColor(C_MUTED);
  canvas.setCursor(16, 58);
  canvas.print("Last: ");
  canvas.setTextColor(C_BLUE);
  canvas.print(lastAction);

  canvas.setTextColor(C_MUTED);
  canvas.setCursor(16, 72);
  canvas.print("Volume: ");
  canvas.setTextColor(volumePercent >= 70 ? C_YELLOW : C_GREEN);
  canvas.print(volumePercent);
  canvas.print("%");

  canvas.setTextColor(C_MUTED);
  canvas.setCursor(16, 86);
  canvas.print("Amplitude: ");
  canvas.setTextColor(C_WHITE);
  canvas.print(outputAmplitude);

  canvas.setTextColor(C_MUTED);
  canvas.setCursor(10, 108);
  canvas.print("A=melody B=beep JoySW=sweep");
  canvas.setCursor(10, 118);
  canvas.print("Pot=volume, test at 100%");

  pushCanvas();
}

void initAudio() {
  i2s_config_t config;
  memset(&config, 0, sizeof(config));
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = 16000;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = 0;
  config.dma_buf_count = 4;
  config.dma_buf_len = 128;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;
#if ESP_IDF_VERSION_MAJOR < 5
  config.fixed_mclk = 0;
#endif

  i2s_pin_config_t pins;
  memset(&pins, 0, sizeof(pins));
  pins.bck_io_num = AUDIO_BCLK_PIN;
  pins.ws_io_num = AUDIO_LRCLK_PIN;
  pins.data_out_num = AUDIO_SDATA_PIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  esp_err_t a = i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
  esp_err_t b = i2s_set_pin(I2S_NUM_0, &pins);
  esp_err_t c = i2s_zero_dma_buffer(I2S_NUM_0);
  audioReady = (a == ESP_OK && b == ESP_OK && c == ESP_OK);
  setLastAction(audioReady ? "Audio ready" : "Init failed");
}

void writeStereoSample(int16_t sample) {
  int16_t stereo[2] = {sample, sample};
  size_t bytesWritten = 0;
  i2s_write(I2S_NUM_0, stereo, sizeof(stereo), &bytesWritten, portMAX_DELAY);
}

void playTone(int frequencyHz, int durationMs) {
  updateVolumeFromPot();
  if (!audioReady || frequencyHz <= 0 || durationMs <= 0 || outputAmplitude <= 0) return;

  const int sampleRate = 16000;
  int totalFrames = sampleRate * durationMs / 1000;
  float phase = 0.0f;
  float step = 2.0f * PI * frequencyHz / sampleRate;

  for (int i = 0; i < totalFrames; i++) {
    int16_t sample = (int16_t)(sinf(phase) * outputAmplitude);
    writeStereoSample(sample);
    phase += step;
    if (phase >= 2.0f * PI) phase -= 2.0f * PI;
  }
}

void playSilence(int durationMs) {
  if (!audioReady) return;
  const int sampleRate = 16000;
  int totalFrames = sampleRate * durationMs / 1000;
  for (int i = 0; i < totalFrames; i++) writeStereoSample(0);
}

void playMelody() {
  setLastAction("Melody");
  drawScreen();
  playTone(523, 160);
  playSilence(20);
  playTone(659, 160);
  playSilence(20);
  playTone(784, 240);
}

void playLowBeep() {
  setLastAction("Low beep");
  drawScreen();
  playTone(220, 280);
}

void playSweep() {
  setLastAction("Sweep");
  drawScreen();
  for (int f = 180; f <= 1600; f += 70) {
    playTone(f, 50);
  }
}

void updateButtons() {
  bool aNow = digitalRead(KEY_A_PIN);
  bool bNow = digitalRead(KEY_B_PIN);
  bool swNow = digitalRead(JOY_SW_PIN);

  bool aEdge = (aPrev == HIGH && aNow == LOW);
  bool bEdge = (bPrev == HIGH && bNow == LOW);
  bool swEdge = (swPrev == HIGH && swNow == LOW);

  aPrev = aNow;
  bPrev = bNow;
  swPrev = swNow;

  if (aEdge) playMelody();
  if (bEdge) playLowBeep();
  if (swEdge) playSweep();
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  pinMode(POT_PIN, INPUT);
  pinMode(KEY_A_PIN, INPUT_PULLUP);
  pinMode(KEY_B_PIN, INPUT_PULLUP);
  pinMode(JOY_SW_PIN, INPUT_PULLUP);

  screenSPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);

  updateVolumeFromPot();
  initAudio();
  drawScreen();
}

void loop() {
  updateButtons();

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 120) {
    drawScreen();
    lastDraw = millis();
  }
}
