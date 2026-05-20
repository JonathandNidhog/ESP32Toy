#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <math.h>
#include <string.h>
#include "driver/i2s.h"

// ============================================================
// ESP32Toy Hardware Test - I2S Audio Format Scan
// Board: ESP32-S3 N16R8
// Audio module power: VCC -> 3V3, GND -> GND
// Speaker-side audio pins:
//   LRCLK -> IO40
//   BCLK  -> IO39
//   SDA   -> IO41
// Controls:
//   A IO15       -> play current format test tone
//   B IO14       -> next I2S format preset
//   JoySW IO6    -> previous I2S format preset
//   Pot IO1      -> output amplitude / volume
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

#ifndef I2S_COMM_FORMAT_STAND_MSB
#ifdef I2S_COMM_FORMAT_I2S_MSB
#define I2S_COMM_FORMAT_STAND_MSB I2S_COMM_FORMAT_I2S_MSB
#else
#define I2S_COMM_FORMAT_STAND_MSB I2S_COMM_FORMAT_STAND_I2S
#endif
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
const uint16_t C_RED = rgb565(255, 80, 80);
const uint16_t C_YELLOW = rgb565(255, 220, 80);
const uint16_t C_BLUE = rgb565(90, 190, 255);

typedef struct {
  const char *name;
  i2s_bits_per_sample_t bits;
  i2s_channel_fmt_t channels;
  i2s_comm_format_t comm;
  int sampleRate;
} AudioPreset;

AudioPreset presets[] = {
  {"I2S 16 Stereo", I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_FMT_RIGHT_LEFT, I2S_COMM_FORMAT_STAND_I2S, 16000},
  {"I2S 16 Left",   I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_FMT_ONLY_LEFT,  I2S_COMM_FORMAT_STAND_I2S, 16000},
  {"I2S 16 Right",  I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_FMT_ONLY_RIGHT, I2S_COMM_FORMAT_STAND_I2S, 16000},
  {"MSB 16 Stereo", I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_FMT_RIGHT_LEFT, I2S_COMM_FORMAT_STAND_MSB, 16000},
  {"MSB 16 Left",   I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_FMT_ONLY_LEFT,  I2S_COMM_FORMAT_STAND_MSB, 16000},
  {"MSB 16 Right",  I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_FMT_ONLY_RIGHT, I2S_COMM_FORMAT_STAND_MSB, 16000},
  {"I2S 32 Stereo", I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_FMT_RIGHT_LEFT, I2S_COMM_FORMAT_STAND_I2S, 16000},
  {"MSB 32 Stereo", I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_FMT_RIGHT_LEFT, I2S_COMM_FORMAT_STAND_MSB, 16000}
};

const int PRESET_COUNT = sizeof(presets) / sizeof(presets[0]);
int presetIndex = 0;
bool audioReady = false;
char lastAction[32] = "Select + press A";

bool aPrev = HIGH;
bool bPrev = HIGH;
bool swPrev = HIGH;

int potRaw = 0;
int volumePercent = 100;
int amplitude16 = 30000;
int32_t amplitude32 = 1800000000L;

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
    amplitude16 = 0;
    amplitude32 = 0;
  } else {
    amplitude16 = map(volumePercent, 1, 100, 1600, 30000);
    amplitude32 = (int32_t)((int64_t)amplitude16 * 60000LL);
    if (amplitude32 > 2000000000L) amplitude32 = 2000000000L;
  }
}

void drawScreen() {
  updateVolumeFromPot();
  canvas.fillScreen(C_BG);

  canvas.setTextSize(2);
  canvas.setTextColor(C_WHITE);
  canvas.setCursor(8, 7);
  canvas.print("I2S FORMAT SCAN");

  canvas.fillRoundRect(7, 31, 146, 76, 5, C_PANEL);
  canvas.setTextSize(1);

  canvas.setTextColor(C_MUTED);
  canvas.setCursor(15, 40);
  canvas.print("Preset ");
  canvas.setTextColor(C_YELLOW);
  canvas.print(presetIndex + 1);
  canvas.print("/");
  canvas.print(PRESET_COUNT);

  canvas.setTextColor(C_WHITE);
  canvas.setCursor(15, 52);
  canvas.print(presets[presetIndex].name);

  canvas.setTextColor(C_MUTED);
  canvas.setCursor(15, 64);
  canvas.print("Init: ");
  canvas.setTextColor(audioReady ? C_GREEN : C_RED);
  canvas.print(audioReady ? "OK" : "FAIL");

  canvas.setTextColor(C_MUTED);
  canvas.setCursor(15, 76);
  canvas.print("Vol: ");
  canvas.setTextColor(volumePercent >= 70 ? C_YELLOW : C_GREEN);
  canvas.print(volumePercent);
  canvas.print("% ");
  canvas.setTextColor(C_MUTED);
  canvas.print("Amp16:");
  canvas.setTextColor(C_WHITE);
  canvas.print(amplitude16);

  canvas.setTextColor(C_MUTED);
  canvas.setCursor(15, 88);
  canvas.print("Last: ");
  canvas.setTextColor(C_BLUE);
  canvas.print(lastAction);

  canvas.setTextColor(C_MUTED);
  canvas.setCursor(6, 114);
  canvas.print("A=play  B=next  JoySW=prev");
  canvas.setCursor(6, 123);
  canvas.print("Pot=volume. Try at 100%");

  pushCanvas();
}

void stopAudio() {
  i2s_zero_dma_buffer(I2S_NUM_0);
  i2s_driver_uninstall(I2S_NUM_0);
  audioReady = false;
}

void initPreset() {
  stopAudio();
  delay(30);

  AudioPreset p = presets[presetIndex];
  i2s_config_t config;
  memset(&config, 0, sizeof(config));
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = p.sampleRate;
  config.bits_per_sample = p.bits;
  config.channel_format = p.channels;
  config.communication_format = p.comm;
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
  setLastAction(audioReady ? "Preset ready" : "Preset fail");
}

void writeStereo16(int16_t left, int16_t right) {
  int16_t stereo[2] = {left, right};
  size_t bytesWritten = 0;
  i2s_write(I2S_NUM_0, stereo, sizeof(stereo), &bytesWritten, portMAX_DELAY);
}

void writeStereo32(int32_t left, int32_t right) {
  int32_t stereo[2] = {left, right};
  size_t bytesWritten = 0;
  i2s_write(I2S_NUM_0, stereo, sizeof(stereo), &bytesWritten, portMAX_DELAY);
}

void playCurrentPresetTone() {
  updateVolumeFromPot();
  if (!audioReady) {
    setLastAction("Init not ready");
    drawScreen();
    return;
  }

  if (amplitude16 <= 0) {
    setLastAction("Volume is zero");
    drawScreen();
    return;
  }

  setLastAction("Playing 440 Hz");
  drawScreen();

  AudioPreset p = presets[presetIndex];
  const int toneHz = 440;
  int totalFrames = p.sampleRate * 800 / 1000;
  float phase = 0.0f;
  float step = 2.0f * PI * toneHz / p.sampleRate;

  for (int i = 0; i < totalFrames; i++) {
    float s = sinf(phase);
    phase += step;
    if (phase >= 2.0f * PI) phase -= 2.0f * PI;

    if (p.bits == I2S_BITS_PER_SAMPLE_32BIT) {
      int32_t sample = (int32_t)(s * amplitude32);
      writeStereo32(sample, sample);
    } else {
      int16_t sample = (int16_t)(s * amplitude16);
      writeStereo16(sample, sample);
    }
  }

  setLastAction("Tone done");
}

void nextPreset(int delta) {
  presetIndex += delta;
  while (presetIndex < 0) presetIndex += PRESET_COUNT;
  while (presetIndex >= PRESET_COUNT) presetIndex -= PRESET_COUNT;
  initPreset();
  drawScreen();
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

  if (aEdge) playCurrentPresetTone();
  if (bEdge) nextPreset(1);
  if (swEdge) nextPreset(-1);
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

  initPreset();
  drawScreen();
}

void loop() {
  updateButtons();

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 180) {
    drawScreen();
    lastDraw = millis();
  }
}
