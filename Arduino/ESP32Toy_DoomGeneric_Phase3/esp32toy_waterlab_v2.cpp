#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_NeoPixel.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

extern Adafruit_ST7735 tft;
extern Adafruit_NeoPixel leds;

#define WL_W 160
#define WL_H 128
#define WL_POT_PIN 1
#define WL_I2C_SDA 4
#define WL_I2C_SCL 5
#define WL_MOTOR_PIN 7
#define WL_KEY_A_PIN 15
#define WL_KEY_B_PIN 14
#define WL_LEFT_JOY_X_PIN 17
#define WL_LEFT_JOY_Y_PIN 18
#define WL_LEFT_JOY_SW_PIN 13
#define WL_MOTOR_ACTIVE_HIGH true
#define WL_RGB_COUNT 4

static uint16_t *fb = nullptr;
static bool initialized = false;
static bool hardwareReady = false;
static uint32_t lastFrameMs = 0;
static uint32_t frameId = 0;

static bool aStable = false, bStable = false, swStable = false;
static bool aRawPrev = false, bRawPrev = false, swRawPrev = false;
static bool aPrev = false, bPrev = false, swPrev = false;
static uint32_t aChangedAt = 0, bChangedAt = 0, swChangedAt = 0;

static uint32_t motorUntilMs = 0;
static uint32_t ledFlashUntilMs = 0;
static uint16_t currentWaterCountForColor = 96;

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
static uint16_t blend565(uint16_t a, uint16_t b, uint8_t t) {
  uint8_t ar = ((a >> 11) & 31) << 3, ag = ((a >> 5) & 63) << 2, ab = (a & 31) << 3;
  uint8_t br = ((b >> 11) & 31) << 3, bg = ((b >> 5) & 63) << 2, bb = (b & 31) << 3;
  return rgb565((ar * (255 - t) + br * t) / 255, (ag * (255 - t) + bg * t) / 255, (ab * (255 - t) + bb * t) / 255);
}
static float clampf2(float v, float lo, float hi) { if (v < lo) return lo; if (v > hi) return hi; return v; }
static float dz(float v, float deadzone) { if (fabsf(v) <= deadzone) return 0.0f; float s = v >= 0.0f ? 1.0f : -1.0f; return s * clampf2((fabsf(v) - deadzone) / (1.0f - deadzone), 0.0f, 1.0f); }
static bool debounced(int pin, bool *rawPrev, bool *stable, uint32_t *changedAt) { bool raw = digitalRead(pin) == LOW; uint32_t now = millis(); if (raw != *rawPrev) { *rawPrev = raw; *changedAt = now; } if (now - *changedAt >= 30 && raw != *stable) *stable = raw; return *stable; }

static void allocFB() { if (fb) return; fb = (uint16_t*)heap_caps_malloc(WL_W * WL_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); if (!fb) fb = (uint16_t*)heap_caps_malloc(WL_W * WL_H * sizeof(uint16_t), MALLOC_CAP_8BIT); }
static void clearFB(uint16_t c) { if (!fb) return; for (int i = 0; i < WL_W * WL_H; ++i) fb[i] = c; }
static void px(int x, int y, uint16_t c) { if (!fb) return; if ((unsigned)x >= WL_W || (unsigned)y >= WL_H) return; fb[y * WL_W + x] = c; }
static void rectFB(int x, int y, int w, int h, uint16_t c) { if (!fb) return; int x0 = max(0, x), y0 = max(0, y); int x1 = min(WL_W, x + w), y1 = min(WL_H, y + h); for (int yy = y0; yy < y1; ++yy) { uint16_t *row = fb + yy * WL_W; for (int xx = x0; xx < x1; ++xx) row[xx] = c; } }
static void circleFB(int cx, int cy, int r, uint16_t c) { int r2 = r * r; for (int y = -r; y <= r; ++y) for (int x = -r; x <= r; ++x) if (x * x + y * y <= r2) px(cx + x, cy + y, c); }
static void pushFB() { if (fb) tft.drawRGBBitmap(0, 0, fb, WL_W, WL_H); }

static const uint8_t COLOR_COUNT = 7;
static uint8_t colorIndex = 5;
static const uint8_t waterRGB[COLOR_COUNT][3] = {{255,60,60},{255,145,40},{255,220,40},{60,220,90},{40,220,235},{55,125,255},{180,85,255}};
static const uint8_t edgeRGB[COLOR_COUNT][3] = {{255,180,180},{255,215,155},{255,245,170},{180,255,195},{170,255,255},{185,220,255},{230,190,255}};
static uint16_t waterColor() { return rgb565(waterRGB[colorIndex][0], waterRGB[colorIndex][1], waterRGB[colorIndex][2]); }
static uint16_t edgeColor() { return rgb565(edgeRGB[colorIndex][0], edgeRGB[colorIndex][1], edgeRGB[colorIndex][2]); }

static void motorWrite(bool on) { digitalWrite(WL_MOTOR_PIN, WL_MOTOR_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH)); }
static void motorPulse(uint32_t ms) { uint32_t until = millis() + ms; if (until > motorUntilMs) motorUntilMs = until; }
static void updateMotor() { motorWrite(millis() < motorUntilMs); }
static void ledAll(uint8_t r, uint8_t g, uint8_t b) { for (int i = 0; i < WL_RGB_COUNT; ++i) leds.setPixelColor(i, leds.Color(r, g, b)); leds.show(); }
static void updateLED() { uint8_t scale = millis() < ledFlashUntilMs ? 12 : 3; ledAll((waterRGB[colorIndex][0] * scale) / 64, (waterRGB[colorIndex][1] * scale) / 64, (waterRGB[colorIndex][2] * scale) / 64); }
static void nextColor() { colorIndex = (colorIndex + 1) % COLOR_COUNT; ledFlashUntilMs = millis() + 120; motorPulse(420); }

static bool mpuTried = false, mpuOk = false; static uint8_t mpuAddr = 0x68; static float ax = 0, ay = 0, az = 1, gx = 0, gy = 0, gz = 0; static float biasAX = 0, biasAY = 0, lastAX = 0, lastAY = 0, lastAZ = 1;
static bool i2cCheck(uint8_t addr) { Wire.beginTransmission(addr); return Wire.endTransmission() == 0; }
static bool readMPU() { if (!mpuOk) return false; Wire.beginTransmission(mpuAddr); Wire.write(0x3B); if (Wire.endTransmission(false) != 0) return false; Wire.requestFrom(mpuAddr, (uint8_t)14, (uint8_t)true); if (Wire.available() < 14) return false; int16_t rax = (Wire.read() << 8) | Wire.read(); int16_t ray = (Wire.read() << 8) | Wire.read(); int16_t raz = (Wire.read() << 8) | Wire.read(); Wire.read(); Wire.read(); int16_t rgx = (Wire.read() << 8) | Wire.read(); int16_t rgy = (Wire.read() << 8) | Wire.read(); int16_t rgz = (Wire.read() << 8) | Wire.read(); ax = rax / 16384.0f; ay = ray / 16384.0f; az = raz / 16384.0f; gx = rgx / 131.0f; gy = rgy / 131.0f; gz = rgz / 131.0f; return true; }
static void initMPU() { if (mpuTried) return; mpuTried = true; Wire.setPins(WL_I2C_SDA, WL_I2C_SCL); Wire.begin(); delay(60); if (i2cCheck(0x68)) { mpuAddr = 0x68; mpuOk = true; } else if (i2cCheck(0x69)) { mpuAddr = 0x69; mpuOk = true; } else return; const uint8_t regs[][2] = {{0x6B,0x00},{0x1A,0x03},{0x1C,0x00},{0x1B,0x00}}; for (int i = 0; i < 4; ++i) { Wire.beginTransmission(mpuAddr); Wire.write(regs[i][0]); Wire.write(regs[i][1]); Wire.endTransmission(true); } float sx = 0, sy = 0; int n = 0; for (int i = 0; i < 80; ++i) { if (readMPU()) { sx += ax; sy += ay; n++; } delay(4); } if (n) { biasAX = sx / n; biasAY = sy / n; } lastAX = ax; lastAY = ay; lastAZ = az; }
static void mapToScreen(float bx, float by, float *ox, float *oy) { *ox = -by; *oy = bx; }

static const int N_MIN = 72, N_MAX = 164; static int nWater = 122; static const float R = 4.0f; static float wx[N_MAX], wy[N_MAX], wvx[N_MAX], wvy[N_MAX];
static const int DROP_MAX = 24; static bool dropOn[DROP_MAX]; static float dxp[DROP_MAX], dyp[DROP_MAX], dvx[DROP_MAX], dvy[DROP_MAX], dr[DROP_MAX]; static int dlife[DROP_MAX]; static int splashCooldown = 0;
static const int STEP = 2; static const int FW = WL_W / STEP + 3; static const int FH = WL_H / STEP + 3; static uint16_t field[FW * FH]; static const int KR_MAX = 13; static int kr = 11, krBuilt = -1; static uint16_t kernel[(KR_MAX * 2 + 1) * (KR_MAX * 2 + 1)];
static float forceX = 0, forceY = 0, joyFX = 0, joyFY = 0, potFilt = 0; static const uint16_t THRESH = 405, FOAM = 1900;

static void buildKernel() { memset(kernel, 0, sizeof(kernel)); int ks = kr * 2 + 1; float rp = kr * STEP, r2 = rp * rp; for (int y = -kr; y <= kr; ++y) for (int x = -kr; x <= kr; ++x) { float px = x * STEP, py = y * STEP, d2 = px * px + py * py; if (d2 < r2) { float t = 1.0f - d2 / r2; kernel[(y + kr) * ks + (x + kr)] = (uint16_t)(t * t * 1320.0f); } } krBuilt = kr; }
static void resetWater() { if (krBuilt != kr) buildKernel(); int id = 0, cols = 15, rows = (N_MAX + cols - 1) / cols; float sx = (WL_W - (cols - 1) * 8.7f) * 0.5f; float sy = WL_H - (rows - 1) * 5.4f - 4.0f; for (int y = 0; y < rows; ++y) for (int x = 0; x < cols; ++x) { if (id >= N_MAX) break; wx[id] = sx + x * 8.7f + random(-1, 2); wy[id] = sy + y * 5.4f + random(-1, 2); wvx[id] = wvy[id] = 0; id++; } for (int i = 0; i < DROP_MAX; ++i) { dropOn[i] = false; dlife[i] = 0; } forceX = forceY = joyFX = joyFY = 0; splashCooldown = 0; nWater = 122; currentWaterCountForColor = nWater; lastAX = ax; lastAY = ay; lastAZ = az; }
static void updateAmount() { int raw = analogRead(WL_POT_PIN); potFilt = potFilt <= 0.1f ? raw : potFilt * 0.90f + raw * 0.10f; float t = clampf2(potFilt / 4095.0f, 0, 1); int target = constrain(N_MIN + (int)(t * (N_MAX - N_MIN)), N_MIN, N_MAX); int targetKr = constrain(9 + (int)(t * 4.0f + 0.5f), 9, 13); if (target > nWater) for (int i = nWater; i < target; ++i) { int ref = random(0, max(1, nWater)); wx[i] = clampf2(wx[ref] + random(-8, 9), R, WL_W - 1 - R); wy[i] = clampf2(wy[ref] + random(-8, 9), R, WL_H - 1 - R); wvx[i] = wvx[ref] * 0.15f; wvy[i] = wvy[ref] * 0.15f; } nWater = target; currentWaterCountForColor = nWater; kr = targetKr; if (kr != krBuilt) buildKernel(); }
static void constrainP(int i) { float b = -0.20f; if (wx[i] < R) { wx[i] = R; wvx[i] *= b; } if (wx[i] > WL_W - 1 - R) { wx[i] = WL_W - 1 - R; wvx[i] *= b; } if (wy[i] < R) { wy[i] = R; wvy[i] *= b; } if (wy[i] > WL_H - 1 - R) { wy[i] = WL_H - 1 - R; wvy[i] *= b; } }
static void solvePairs() { float td = R * 1.34f, td2 = td * td; for (int i = 0; i < nWater; ++i) for (int j = i + 1; j < nWater; ++j) { float x = wx[j] - wx[i], y = wy[j] - wy[i], d2 = x*x + y*y; if (d2 < 0.0001f || d2 >= td2) continue; float d = sqrtf(d2), p = (td - d) * 0.39f, nx = x / d, ny = y / d; wx[i] -= nx*p; wy[i] -= ny*p; wx[j] += nx*p; wy[j] += ny*p; } }
static void viscosity() { float rsq = R * R * 6.2f; for (int i = 0; i < nWater; ++i) for (int j = i + 1; j < nWater; ++j) { float x = wx[j]-wx[i], y = wy[j]-wy[i]; if (x*x+y*y >= rsq) continue; float vx = wvx[j]-wvx[i], vy = wvy[j]-wvy[i]; wvx[i] += vx*0.010f; wvy[i] += vy*0.010f; wvx[j] -= vx*0.010f; wvy[j] -= vy*0.010f; } }
static void spawnDrop(float x, float y, float vx, float vy, float r) { for (int i=0;i<DROP_MAX;++i) if(!dropOn[i]) { dropOn[i]=true; dxp[i]=x; dyp[i]=y; dvx[i]=vx; dvy[i]=vy; dr[i]=r; dlife[i]=78; return; } }
static void splash(float fx, float fy, float s) { motorPulse(320); ledFlashUntilMs = millis() + 80; float l=sqrtf(fx*fx+fy*fy), ux=0, uy=-1; if(l>0.05f){ux=-fx/l; uy=-fy/l;} for(int n=0;n<constrain((int)(s*1.55f),4,9);++n){ int id=random(0,nWater); float side=random(-100,101)/100.0f; float sx=-uy, sy=ux; spawnDrop(wx[id], wy[id], wvx[id]+ux*(0.92f+s*0.44f)+sx*side*(0.52f+s*0.12f), wvy[id]+uy*(0.92f+s*0.44f)+sy*side*(0.52f+s*0.12f), random(1,3)); } }
static void updateDrops() { for(int i=0;i<DROP_MAX;++i) if(dropOn[i]){ dvx[i]+=forceX*0.012f; dvy[i]+=0.072f+forceY*0.012f; dvx[i]*=0.994f; dvy[i]*=0.994f; dxp[i]+=dvx[i]; dyp[i]+=dvy[i]; dlife[i]--; if(dxp[i]<2){dxp[i]=2;dvx[i]*=-0.24f;} if(dxp[i]>WL_W-3){dxp[i]=WL_W-3;dvx[i]*=-0.24f;} if(dyp[i]<2){dyp[i]=2;dvy[i]*=-0.20f;} if(dyp[i]>WL_H || dlife[i]<=0) dropOn[i]=false; } }
static void buildField() { memset(field,0,sizeof(field)); int ks=kr*2+1; for(int i=0;i<nWater;++i){int cx=(int)(wx[i]/STEP), cy=(int)(wy[i]/STEP); for(int yy=-kr;yy<=kr;++yy){int fy=cy+yy; if(fy<0||fy>=FH)continue; for(int xx=-kr;xx<=kr;++xx){int fx=cx+xx; if(fx<0||fx>=FW)continue; uint16_t add=kernel[(yy+kr)*ks+(xx+kr)]; if(!add)continue; int idx=fy*FW+fx; uint32_t v=field[idx]+add; field[idx]=v>65535?65535:v;}}} }
static uint16_t densityColor(uint16_t d,int y){ uint16_t base=waterColor(), edge=edgeColor(); float amountT=clampf2((currentWaterCountForColor-N_MIN)/(float)(N_MAX-N_MIN),0,1); if(amountT<0.20f)return blend565(base,edge,220); uint16_t center=blend565(base,rgb565(0,0,10),(uint8_t)(135+amountT*90)); uint16_t mid=blend565(base,edge,85); uint16_t light=blend565(base,edge,185); if(d>FOAM)return edge; if(d>1500)return center; if(d>950)return mid; return light; }
static void drawWater() { clearFB(rgb565(0,0,7)); buildField(); uint16_t edge=edgeColor(), foam=blend565(edge,ST77XX_WHITE,120); for(int y=0;y<FH;++y)for(int x=0;x<FW;++x){uint16_t d=field[y*FW+x]; if(d>=THRESH)rectFB(x*STEP,y*STEP,STEP,STEP,densityColor(d,y));} for(int y=1;y<FH-1;++y)for(int x=1;x<FW-1;++x){int idx=y*FW+x; if(field[idx]<THRESH)continue; bool top=field[idx-FW]<THRESH, side=field[idx-1]<THRESH||field[idx+1]<THRESH; if(top){rectFB(x*STEP,y*STEP,STEP,1,edge); if(((x*3+y+frameId)&15)<2)px(x*STEP,y*STEP,ST77XX_WHITE);} else if(side&&((y+frameId)&7)==0)px(x*STEP,y*STEP,edge); if(field[idx]>FOAM&&((x+y+frameId)&11)==0)px(x*STEP+1,y*STEP,foam);} for(int i=0;i<DROP_MAX;++i)if(dropOn[i]){circleFB((int)dxp[i],(int)dyp[i],(int)dr[i],edge); if(dr[i]>=2)px((int)dxp[i]-1,(int)dyp[i]-1,ST77XX_WHITE);} pushFB(); }
static void stepWater(bool manual) { updateAmount(); float jx=dz(clampf2((analogRead(WL_LEFT_JOY_X_PIN)-2048)/1800.0f,-1,1),0.14f), jy=dz(clampf2((analogRead(WL_LEFT_JOY_Y_PIN)-2048)/1800.0f,-1,1),0.14f); joyFX=joyFX*0.55f+jx*0.45f; joyFY=joyFY*0.55f+jy*0.45f; bool ok=readMPU(); float tx=0,ty=0,gsx=0,gsy=0,ss=0; if(ok){float bx=ax-biasAX, by=-(ay-biasAY); if(fabsf(bx)<0.018f)bx=0; if(fabsf(by)<0.018f)by=0; mapToScreen(clampf2(bx*3.25f,-1.4f,1.4f),clampf2(by*3.25f,-1.4f,1.4f),&tx,&ty); mapToScreen(clampf2(gx*0.014f,-1.9f,1.9f),clampf2(gy*0.014f,-1.9f,1.9f),&gsx,&gsy); float jerk=fabsf(ax-lastAX)+fabsf(ay-lastAY)+fabsf(az-lastAZ); lastAX=ax;lastAY=ay;lastAZ=az; ss=jerk*8.5f+fabsf(gx)*0.0042f+fabsf(gy)*0.0042f+fabsf(gz)*0.0025f;} forceX=forceX*0.55f+clampf2(tx+joyFX*0.90f,-1.65f,1.65f)*0.45f; forceY=forceY*0.55f+clampf2(ty+joyFY*0.90f,-1.65f,1.65f)*0.45f; for(int i=0;i<nWater;++i){wvx[i]+=forceX*0.42f+gsx*0.32f; wvy[i]+=forceY*0.42f+gsy*0.32f+0.010f; wvx[i]*=0.994f; wvy[i]*=0.994f; wvx[i]=clampf2(wvx[i],-5.7f,5.7f); wvy[i]=clampf2(wvy[i],-5.7f,5.7f); wx[i]+=wvx[i]; wy[i]+=wvy[i]; constrainP(i);} solvePairs(); solvePairs(); for(int i=0;i<nWater;++i)constrainP(i); viscosity(); updateDrops(); if(splashCooldown>0)splashCooldown--; if(ss>0.85f&&splashCooldown<=0){splash(forceX+gsx*0.45f,forceY+gsy*0.45f,ss);splashCooldown=7;} if(manual&&splashCooldown<=0){splash(forceX,forceY,2.0f);splashCooldown=8;} }

bool ESP32Toy_WaterLabV2Tick(void) {
  if (!hardwareReady) { pinMode(WL_MOTOR_PIN, OUTPUT); motorWrite(false); pinMode(WL_KEY_A_PIN, INPUT_PULLUP); pinMode(WL_KEY_B_PIN, INPUT_PULLUP); pinMode(WL_LEFT_JOY_SW_PIN, INPUT_PULLUP); pinMode(WL_LEFT_JOY_X_PIN, INPUT); pinMode(WL_LEFT_JOY_Y_PIN, INPUT); pinMode(WL_POT_PIN, INPUT); allocFB(); initMPU(); hardwareReady = true; }
  updateMotor(); updateLED();
  if (!initialized) { resetWater(); clearFB(rgb565(0,0,7)); pushFB(); lastFrameMs = 0; motorPulse(650); ledFlashUntilMs = millis() + 120; initialized = true; }
  bool a=debounced(WL_KEY_A_PIN,&aRawPrev,&aStable,&aChangedAt), b=debounced(WL_KEY_B_PIN,&bRawPrev,&bStable,&bChangedAt), sw=debounced(WL_LEFT_JOY_SW_PIN,&swRawPrev,&swStable,&swChangedAt);
  bool ap=a&&!aPrev, bp=b&&!bPrev, sp=sw&&!swPrev; aPrev=a; bPrev=b; swPrev=sw;
  if (ap) nextColor();
  if (bp) { initialized=false; motorPulse(500); ledAll(0,0,0); return true; }
  uint32_t now=millis(); if(now-lastFrameMs<32)return false; lastFrameMs=now; frameId++; stepWater(sp); drawWater(); return false;
}
