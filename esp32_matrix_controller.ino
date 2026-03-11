/*
 ╔══════════════════════════════════════════════════════════════════╗
 ║  ESP32 · 16×16 WS2812B LED Matrix Controller · v2.0            ║
 ║                                                                  ║
 ║  Режимы:                                                         ║
 ║    0 – Температура    7 – Метеоры                               ║
 ║    1 – Часы (5 видов) 8 – Змейка                               ║
 ║    2 – Радуга         9 – Лава-лампа                            ║
 ║    3 – Огонь (32×32)                                            ║
 ║    4 – Бегущая строка (Latin + Кириллица)                       ║
 ║    5 – Matrix rain                                               ║
 ║    6 – Плазма                                                    ║
 ║                                                                  ║
 ║  Подключение:                                                    ║
 ║    WS2812B DATA → GPIO 5  (резистор 270–330 Ω в разрыв)        ║
 ║    WS2812B VCC  → пин 5V ESP32 (от USB-C зарядника 5V 3A)      ║
 ║    WS2812B GND  → GND общий с ESP32                             ║
 ║                                                                  ║
 ║  Библиотеки (Library Manager):                                   ║
 ║    · FastLED · ArduinoJson                                       ║
 ╚══════════════════════════════════════════════════════════════════╝
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <esp_wifi.h>

// ════════════════════════════════════════════════════════════
//  ЖЕЛЕЗО
// ════════════════════════════════════════════════════════════
#define LED_PIN      5
#define NUM_LEDS     256
#define MW           16
#define MH           16

CRGB leds[NUM_LEDS];

// ════════════════════════════════════════════════════════════
//  ШРИФТ 3×5: цифры/знаки (индекс 0-14)
//  0-9='0'-'9'  10='-'  11='+'  12='°'  13='C'  14=space
// ════════════════════════════════════════════════════════════
static const uint8_t F_MICRO[15][5] PROGMEM = {
  {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},
  {5,5,7,1,1},{7,4,7,1,7},{6,4,7,5,7},{7,1,3,1,1},
  {7,5,7,5,7},{7,5,7,1,6},
  {0,0,7,0,0},{0,2,7,2,0},{6,6,0,0,0},{7,4,4,4,7},{0,0,0,0,0}
};
static const uint8_t F_ROUND[15][5] PROGMEM = {
  {2,5,5,5,2},{2,6,2,2,7},{6,1,2,4,7},{6,1,2,1,6},
  {5,5,7,1,1},{7,4,6,1,6},{3,4,7,5,2},{7,1,2,2,2},
  {2,5,2,5,2},{2,5,3,1,6},
  {0,0,7,0,0},{0,2,7,2,0},{6,6,0,0,0},{3,4,4,4,3},{0,0,0,0,0}
};
static const uint8_t F_BOLD[15][5] PROGMEM = {
  {7,7,5,7,7},{3,7,3,3,7},{7,1,7,6,7},{7,3,3,3,7},
  {5,7,7,1,1},{7,6,7,3,7},{6,6,7,7,7},{7,3,3,3,3},
  {7,7,7,7,7},{7,7,7,3,7},
  {0,7,7,7,0},{2,7,7,7,2},{6,6,0,0,0},{7,6,6,6,7},{0,0,0,0,0}
};

// ════════════════════════════════════════════════════════════
//  ШРИФТ 5×7: Big (только цифры 0-9, знаки '-'=10, '+'=11)
//  Бит 4 = левый столбец, бит 0 = правый
// ════════════════════════════════════════════════════════════
static const uint8_t F_BIG[12][7] PROGMEM = {
  {14,17,17,17,17,17,14},{4,12,4,4,4,4,31},
  {14,17,1,6,8,16,31},{30,1,1,14,1,1,30},
  {2,6,10,18,31,2,2},{31,16,30,1,1,17,14},
  {6,8,16,30,17,17,14},{31,1,2,4,8,8,8},
  {14,17,17,14,17,17,14},{14,17,17,15,1,2,12},
  {0,0,0,31,0,0,0},{0,4,4,31,4,4,0}
};

// ════════════════════════════════════════════════════════════
//  РАСШИРЕННЫЙ ШРИФТ 3×5: Latin A-Z (idx 0-25)
//  Кириллица А-Я (idx 26-57), пунктуация !?.,: (idx 58-62)
// ════════════════════════════════════════════════════════════
static const uint8_t F_EXT[63][5] PROGMEM = {
  // Latin A-Z
  {2,5,7,5,5},{6,5,6,5,6},{7,4,4,4,7},{6,5,5,5,6},{7,4,7,4,7},
  {7,4,7,4,4},{7,4,5,5,7},{5,5,7,5,5},{7,2,2,2,7},{7,1,1,5,2},
  {5,6,4,6,5},{4,4,4,4,7},{5,7,5,5,5},{5,6,5,5,5},{7,5,5,5,7},
  {7,5,7,4,4},{7,5,5,7,1},{7,5,7,6,5},{7,4,7,1,7},{7,2,2,2,2},
  {5,5,5,5,7},{5,5,5,7,2},{5,5,7,7,5},{5,5,2,5,5},{5,5,7,2,2},
  {7,1,2,4,7},
  // Cyrillic А-Я
  {2,5,7,5,5},{7,4,6,5,6},{6,5,6,5,6},{7,4,4,4,4},{6,5,5,5,7},
  {7,4,7,4,7},{5,5,7,5,5},{6,1,3,1,6},{5,6,5,5,5},{2,5,5,6,5},
  {5,6,4,6,5},{3,2,2,5,5},{5,7,5,5,5},{5,5,7,5,5},{7,5,5,5,7},
  {7,5,5,5,5},{7,5,7,4,4},{7,4,4,4,7},{7,2,2,2,2},{5,5,7,2,2},
  {2,7,5,7,2},{5,5,2,5,5},{5,5,5,7,1},{5,5,7,1,1},{5,5,5,5,7},
  {5,5,5,7,3},{6,4,6,5,6},{5,6,6,5,5},{4,4,6,5,6},{7,1,3,1,7},
  {5,7,5,7,5},{3,5,3,1,5},
  // Пунктуация !?.,: 
  {2,2,2,0,2},{3,1,2,0,2},{0,0,0,0,2},{0,0,0,2,4},{0,2,0,2,0}
};

// ════════════════════════════════════════════════════════════
//  КОНФИГУРАЦИЯ
// ════════════════════════════════════════════════════════════
struct Config {
  char     ssid[40]        = "";      // задаётся через веб-интерфейс
  char     wifiPass[64]    = "";
  char     city[64]        = "Moscow";
  char     apiKey[48]      = "";
  char     ntpServer[64]   = "pool.ntp.org";
  char     yaKey[64]       = "";
  float    yaLat           = 55.7558f;
  float    yaLon           = 37.6176f;
  int      tzOffset        = 3;
  uint8_t  brightness      = 80;
  uint8_t  fontIndex       = 0;
  uint8_t  mode            = 0;
  uint8_t  clockFace       = 0;
  uint16_t updateMin       = 10;
  bool     use24h          = true;
  bool     blinkColon      = true;
  char     scrollText[128] = "ESP32 Matrix";
  uint8_t  scrollSpeed     = 8;
  bool     autoColor       = true;
  uint8_t  customR         = 255;
  uint8_t  customG         = 128;
  uint8_t  customB         = 0;
  uint8_t  weatherProv     = 0;   // 0=OWM, 1=Яндекс
  bool     lavaAutoColor   = true;
  uint16_t lavaHue         = 15;
  uint8_t  rotation        = 0;   // 0=нормальный 1=90°CW 2=180° 3=270°CW
  bool     flipH           = false; // зеркало по горизонтали
  bool     flipV           = false; // зеркало по вертикали
};

Config     cfg;
Preferences prefs;

// ════════════════════════════════════════════════════════════
//  РАНТАЙМ
// ════════════════════════════════════════════════════════════
WebServer server(80);
DNSServer dnsServer;

static const char* AP_NAME = "ESP32-Matrix";
static const char* AP_PASS = "12345678";

float    currentTemp    = -999.0f;
volatile bool wifiConnected  = false;  // volatile: читается core 0 без мьютекса
bool     apMode         = true;   // стартуем всегда в AP
volatile bool weatherRequested = false; // флаг запроса погоды для weatherTask

// Мьютекс для доступа к WiFi-стеку между core 0 (weatherTask) и core 1 (loop/WebServer).
// Любая операция с WiFi/HTTPClient должна удерживать этот мьютекс.
SemaphoreHandle_t wifiMutex = NULL;

// Спинлок для атомарного доступа к currentTemp и lastWeatherMs с двух ядер.
// portENTER/EXIT_CRITICAL на ESP32 маскирует прерывания И блокирует второе ядро
// через HW-спинлок — безопасно для коротких (~1 мкс) критических секций.
static portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;

uint32_t lastWeatherMs  = 0;
uint32_t lastDrawMs     = 0;

int      scrollX        = MW;

// ════════════════════════════════════════════════════════════
//  МАТРИЦА: базовые утилиты
// ════════════════════════════════════════════════════════════

// Змейковая адресация
inline int XY(int x, int y) {
  if (x < 0 || x >= MW || y < 0 || y >= MH) return -1;
  return (y & 1) ? (y * MW + MW - 1 - x) : (y * MW + x);
}
inline void px(int x, int y, CRGB c) {
  // 1. Отражение
  int fx = cfg.flipH ? (MW-1-x) : x;
  int fy = cfg.flipV ? (MH-1-y) : y;
  // 2. Поворот
  int rx = fx, ry = fy;
  switch (cfg.rotation) {
    case 1: rx = MH-1-fy; ry = fx;       break; // 90° по часовой
    case 2: rx = MW-1-fx; ry = MH-1-fy;  break; // 180°
    case 3: rx = fy;      ry = MW-1-fx;  break; // 270° по часовой
  }
  int i = XY(rx, ry);
  if (i >= 0) leds[i] = c;
}
inline void clearMatrix() { fill_solid(leds, NUM_LEDS, CRGB::Black); }

// ════════════════════════════════════════════════════════════
//  РЕНДЕРИНГ ТЕКСТА
// ════════════════════════════════════════════════════════════

// Индекс символа для расширенного шрифта
int extIdx(char c) {
  uint8_t u = (uint8_t)c;
  if (u >= '0' && u <= '9') return -(int)(u - '0') - 1; // <0 → базовый шрифт
  if (c == '-') return -11; if (c == '+') return -12;
  if (c == '^' || u == 0xB0) return -13; // °
  if (c == 'C') return -14;
  if (c == ' ') return -15;
  if (c == '!') return 58; if (c == '?') return 59;
  if (c == '.') return 60; if (c == ',') return 61; if (c == ':') return 62;
  // Latin
  char uc = (c >= 'a' && c <= 'z') ? c - 32 : c;
  if (uc >= 'A' && uc <= 'Z') return (int)(uc - 'A');
  // Кириллица UTF-8: при компиляции с -fexec-charset=UTF-8
  // Обрабатываем как 1-байтовые значения Cp1251 если строки в Cp1251
  // Значения Cp1251: А=0xC0..Я=0xDF, а=0xE0..я=0xFF
  if (u >= 0xC0 && u <= 0xDF) return 26 + (int)(u - 0xC0);
  if (u >= 0xE0 && u <= 0xFF) return 26 + (int)(u - 0xE0); // строчные → заглавные
  return -15; // space
}

// Возвращает указатель на начало 5-байтовых данных символа в PROGMEM.
// Вызывающий код сам добавляет смещение строки: getFontData(ei, font) + row.
const uint8_t* getFontData(int ei, uint8_t font) {
  // ei < 0: базовый шрифт, -(ei+1) = индекс 0-14
  if (ei < 0) {
    int idx = -(ei + 1);
    if (idx > 14) idx = 14;
    if (font == 1) return &F_ROUND[idx][0];
    if (font == 2) return &F_BOLD[idx][0];
    return &F_MICRO[idx][0];
  }
  if (ei > 62) ei = 62;
  return &F_EXT[ei][0];
}

void drawChar3(int x, int y, char c, CRGB col, uint8_t font) {
  int ei = extIdx(c);
  for (int row = 0; row < 5; row++) {
    uint8_t bits = pgm_read_byte(getFontData(ei, font) + row);
    for (int col2 = 0; col2 < 3; col2++)
      if (bits & (1 << (2 - col2))) px(x + col2, y + row, col);
  }
}

int strW3(const char* s) {
  int n = strlen(s);
  return n ? n * 3 + (n - 1) : 0;
}

void drawStr3(int x, int y, const char* s, CRGB col, uint8_t font) {
  int cx = x;
  for (int i = 0; s[i]; i++) {
    if (i > 0) cx++;
    drawChar3(cx, y, s[i], col, font);
    cx += 3;
  }
}

void drawChar7(int x, int y, int di, CRGB col) {
  if (di < 0 || di > 11) return;
  for (int row = 0; row < 7; row++) {
    uint8_t bits = pgm_read_byte(&F_BIG[di][row]);
    for (int c = 0; c < 5; c++)
      if (bits & (1 << (4 - c))) px(x + c, y + row, col);
  }
}

// ════════════════════════════════════════════════════════════
//  ПОТОКОБЕЗОПАСНЫЕ АКСЕССОРЫ SHARED-ПЕРЕМЕННЫХ
//  currentTemp  — пишет core 0 (weatherTask), читает core 1 (render/web)
//  lastWeatherMs — пишут оба ядра, читает core 1
// ════════════════════════════════════════════════════════════

inline float getTemp() {
  float t;
  portENTER_CRITICAL(&statsMux);
  t = currentTemp;
  portEXIT_CRITICAL(&statsMux);
  return t;
}

// setTemp атомарно обновляет currentTemp И lastWeatherMs —
// чтобы на core 1 оба значения всегда были согласованы.
inline void setTemp(float t) {
  portENTER_CRITICAL(&statsMux);
  currentTemp   = t;
  lastWeatherMs = millis();
  portEXIT_CRITICAL(&statsMux);
}

inline uint32_t getLastWeatherMs() {
  uint32_t ms;
  portENTER_CRITICAL(&statsMux);
  ms = lastWeatherMs;
  portEXIT_CRITICAL(&statsMux);
  return ms;
}

inline void setLastWeatherMs(uint32_t ms) {
  portENTER_CRITICAL(&statsMux);
  lastWeatherMs = ms;
  portEXIT_CRITICAL(&statsMux);
}

// ════════════════════════════════════════════════════════════
//  ЦВЕТ ПО ТЕМПЕРАТУРЕ
// ════════════════════════════════════════════════════════════
CRGB tempToColor(float t) {
  if (t <= -20.0f) return CRGB(0, 0, 255);
  if (t <=   0.0f) { float f=(t+20)/20; return CRGB(0,(uint8_t)(200*f),255); }
  if (t <=  20.0f) { float f=t/20;       return CRGB(0,220,(uint8_t)(255*(1-f))); }
  if (t <=  30.0f) { float f=(t-20)/10;  return CRGB((uint8_t)(255*f),220,0); }
  if (t <=  40.0f) { float f=(t-30)/10;  return CRGB(255,(uint8_t)(220*(1-f)),0); }
  return CRGB(255, 0, 0);
}

CRGB activeColor() {
  float t = getTemp(); // один атомарный снимок
  if (cfg.autoColor && t > -999.0f) return tempToColor(t);
  return CRGB(cfg.customR, cfg.customG, cfg.customB);
}

// ════════════════════════════════════════════════════════════
//  РЕЖИМ 0: ТЕМПЕРАТУРА
// ════════════════════════════════════════════════════════════
void showTemperature() {
  clearMatrix();
  float t = getTemp(); // один атомарный снимок на весь кадр
  if (t <= -999.0f) {
    drawStr3((MW - strW3("---")) / 2, 5, "---", CRGB(60, 60, 60), 0);
    FastLED.show(); return;
  }
  CRGB color = cfg.autoColor ? tempToColor(t) : CRGB(cfg.customR, cfg.customG, cfg.customB);
  int abst = (int)abs(t);
  bool neg = (t < 0);
  bool showSign = (abst > 0);

  auto drawSign = [&](int cx, int sy) {
    px(cx-1,sy,color); px(cx,sy,color); px(cx+1,sy,color);
    if (!neg) { px(cx,sy-1,color); px(cx,sy+1,color); }
  };

  if (cfg.fontIndex == 3) {
    // 5×7: знак 3px слева, цифры, градус 2×2 справа
    int sy = 4;
    if (abst < 10) {
      drawChar7(7, sy, abst, color);
      if (showSign) drawSign(4, sy + 3);
    } else {
      drawChar7(4, sy, abst / 10, color);
      drawChar7(10, sy, abst % 10, color);
      if (showSign) drawSign(1, sy + 3);
    }
    px(14,0,color); px(15,0,color);
    px(14,1,color); px(15,1,color);
  } else {
    // 3×5
    int sy = 5;
    char ds[3] = {0};
    if (abst < 10) {
      ds[0] = '0' + abst;
      drawChar3(7, sy, ds[0], color, cfg.fontIndex);
      if (showSign) drawSign(5, sy + 2);
    } else {
      ds[0] = '0' + abst / 10; ds[1] = '0' + abst % 10;
      drawChar3(6, sy, ds[0], color, cfg.fontIndex);
      drawChar3(10, sy, ds[1], color, cfg.fontIndex);
      if (showSign) drawSign(3, sy + 2);
    }
    // Градус °
    px(14,1,color); px(15,1,color); px(14,2,color); px(15,2,color);
  }
  FastLED.show();
}

// ════════════════════════════════════════════════════════════
//  РЕЖИМ 1: ЧАСЫ — 5 видов
// ════════════════════════════════════════════════════════════

bool colonOn() {
  if (!cfg.blinkColon) return true;
  return (millis() / 500) & 1;
}

// ── Вид 0: ЧЧ сверху / ММ снизу ──────────────────────────
void clockFace0(int h, int m, CRGB col) {
  char hh[3], mm[3];
  snprintf(hh, 3, "%02d", h); snprintf(mm, 3, "%02d", m);
  uint8_t fi = min((int)cfg.fontIndex, 3);

  if (fi == 3) {
    drawChar7(2, 0, hh[0]-'0', col); drawChar7(8, 0, hh[1]-'0', col);
    drawChar7(2, 9, mm[0]-'0', col); drawChar7(8, 9, mm[1]-'0', col);
  } else {
    drawStr3(4, 1, hh, col, fi);
    drawStr3(4, 10, mm, col, fi);
  }
  if (colonOn()) {
    CRGB dim = col; dim.nscale8(90);
    px(7,7,dim); px(8,7,dim); px(7,8,dim); px(8,8,dim);
  }
}

// ── Вид 1: ЧЧ:ММ в одну строку ───────────────────────────
void clockFace1(int h, int m, CRGB col) {
  uint8_t fi = min((int)cfg.fontIndex, 2);
  char hh[3], mm[3];
  snprintf(hh, 3, "%02d", h); snprintf(mm, 3, "%02d", m);
  drawStr3(0, 5, hh, col, fi);
  drawStr3(8, 5, mm, col, fi);
  if (colonOn()) {
    CRGB dim = col; dim.nscale8(90);
    px(7, 6, dim); px(7, 8, dim);
  }
}

// ── Вид 2: Аналоговый ────────────────────────────────────
void clockFace2(int h, int m, CRGB col) {
  float cx = 7.5f, cy = 7.5f;
  CRGB dim = col; dim.nscale8(30);
  CRGB tick = col; tick.nscale8(120);
  // 12 делений
  for (int i = 0; i < 12; i++) {
    float a = i / 12.0f * 2 * M_PI - M_PI / 2;
    int tx = (int)round(cx + 7 * cos(a));
    int ty = (int)round(cy + 7 * sin(a));
    px(tx, ty, (i % 3 == 0) ? tick : dim);
  }
  // Минутная (len 6)
  float ma = m / 60.0f * 2 * M_PI - M_PI / 2;
  for (float t = 0; t <= 6; t += 0.5f)
    px((int)round(cx + t*cos(ma)), (int)round(cy + t*sin(ma)), col);
  // Часовая (len 4)
  float ha = ((h % 12) + m / 60.0f) / 12.0f * 2 * M_PI - M_PI / 2;
  for (float t = 0; t <= 4; t += 0.5f)
    px((int)round(cx + t*cos(ha)), (int)round(cy + t*sin(ha)), col);
  // Центр
  px(7,7,CRGB::White); px(8,7,CRGB::White);
  px(7,8,CRGB::White); px(8,8,CRGB::White);
}

// ── Вид 3: Двоичный ──────────────────────────────────────
void clockFace3(int h, int m, CRGB col) {
  int digits[4] = { h/10, h%10, m/10, m%10 };
  int maxBits[4] = { 2, 4, 3, 4 };
  int xs[4] = { 1, 5, 9, 13 };
  int ys[4] = { 13, 10, 7, 4 }; // LSB снизу
  CRGB dim = col; dim.nscale8(20);
  for (int col2 = 0; col2 < 4; col2++) {
    for (int bit = 0; bit < maxBits[col2]; bit++) {
      bool on = (digits[col2] >> bit) & 1;
      CRGB c = on ? col : dim;
      int bx = xs[col2], by = ys[bit];
      px(bx,by,c); px(bx+1,by,c); px(bx,by+1,c); px(bx+1,by+1,c);
    }
  }
  // Разделитель
  uint8_t sv = max(col.r, max(col.g, col.b));
  CRGB sep = CRGB(sv/8, sv/8, sv/8);
  for (int y = 4; y < 16; y += 3) px(7, y, sep);
}

// ── Вид 4: Кольцо минут ──────────────────────────────────
void clockFace4(int h, int m, CRGB col) {
  // 60-пиксельное кольцо по периметру
  int ring[60][2];
  int ri = 0;
  for (int x = 0; x < 16; x++) ring[ri][0]=x, ring[ri++][1]=0;
  for (int y = 1; y < 15; y++) ring[ri][0]=15, ring[ri++][1]=y;
  for (int x = 15; x >= 0; x--) ring[ri][0]=x, ring[ri++][1]=15;
  for (int y = 14; y >= 1; y--) ring[ri][0]=0, ring[ri++][1]=y;

  CRGB dim = col; dim.nscale8(18);
  for (int i = 0; i < 60; i++) px(ring[i][0], ring[i][1], dim);
  for (int i = 0; i <= m; i++) {
    float sc = 0.35f + 0.65f * (i / 60.0f);
    CRGB c = col; c.nscale8((uint8_t)(255 * sc));
    px(ring[i][0], ring[i][1], c);
  }
  // Часы внутри
  uint8_t fi = min((int)cfg.fontIndex, 3);
  char hh[3]; snprintf(hh, 3, "%02d", h);
  if (fi == 3) {
    drawChar7(2, 4, hh[0]-'0', col); drawChar7(8, 4, hh[1]-'0', col);
  } else {
    drawStr3(4, 5, hh, col, fi);
  }
}

void showClock() {
  clearMatrix();
  time_t now = time(nullptr);
  struct tm* ti = localtime(&now);
  int h = cfg.use24h ? ti->tm_hour : (ti->tm_hour % 12 == 0 ? 12 : ti->tm_hour % 12);
  int m = ti->tm_min;
  CRGB col = activeColor();
  switch (cfg.clockFace) {
    case 0: clockFace0(h, m, col); break;
    case 1: clockFace1(h, m, col); break;
    case 2: clockFace2(h, m, col); break;
    case 3: clockFace3(h, m, col); break;
    case 4: clockFace4(h, m, col); break;
  }
  FastLED.show();
}

// ════════════════════════════════════════════════════════════
//  РЕЖИМ 2: РАДУГА
// ════════════════════════════════════════════════════════════
uint8_t rainbowHue = 0;
void showRainbow() {
  for (int y = 0; y < MH; y++)
    for (int x = 0; x < MW; x++)
      px(x, y, CHSV((uint8_t)(rainbowHue + (x + y) * 10), 255, 255));
  FastLED.show();
  rainbowHue += 1;
}

// ════════════════════════════════════════════════════════════
//  РЕЖИМ 3: ОГОНЬ — хаотичный 32×32 → downscale 2:1
// ════════════════════════════════════════════════════════════
#define FW 32
#define FH 32

static float fA[FW * FH];
static float fB[FW * FH];
static float fCool[FW * FH]; // статический шум охлаждения
static float fWind[FW * FH]; // статический шум ветра
static float *fR = fA, *fW2 = fB;

struct Tongue { float x, y, heat, vx, vy; int life; };
static Tongue tongues[6];

static bool fireInit = false;
static uint32_t fireLastMs = 0;
static uint32_t fireTick = 0;

void respawnTongue(int i) {
  tongues[i].x    = random(FW);
  tongues[i].y    = FH - 1;
  tongues[i].heat = 0.7f + random(100) / 333.f;
  tongues[i].vx   = (random(100) - 50) / 17.f;
  tongues[i].vy   = -(2.f + random(100) / 25.f);
  tongues[i].life = 4 + random(10);
}

void fireSetup() {
  for (int i = 0; i < FW * FH; i++) {
    fCool[i] = random(100) / 100.f;
    fWind[i] = (random(100) - 50) / 50.f;
    fA[i] = fB[i] = 0;
  }
  for (int i = 0; i < 6; i++) respawnTongue(i);
  fireInit = true;
}

// 8-точечная палитра огня
struct FirePal { float t; uint8_t r, g, b; };
static const FirePal FPAL[] = {
  {0.00f,  0,  0,  0}, {0.13f, 55,  0,  0}, {0.28f,200,  5,  0},
  {0.44f,255, 55,  0}, {0.58f,255,138,  0}, {0.72f,255,215, 10},
  {0.86f,255,250, 90}, {1.00f,255,255,210}
};
CRGB fireColor(float v) {
  v = constrain(v, 0, 1);
  for (int i = 1; i < 8; i++) {
    if (v <= FPAL[i].t) {
      float p = (v - FPAL[i-1].t) / (FPAL[i].t - FPAL[i-1].t);
      return CRGB(
        (uint8_t)(FPAL[i-1].r + (FPAL[i].r - FPAL[i-1].r) * p),
        (uint8_t)(FPAL[i-1].g + (FPAL[i].g - FPAL[i-1].g) * p),
        (uint8_t)(FPAL[i-1].b + (FPAL[i].b - FPAL[i-1].b) * p)
      );
    }
  }
  return CRGB(255, 255, 210);
}

void showFire() {
  if (!fireInit) fireSetup();
  if (millis() - fireLastMs < 84) {
    // Только рисуем, без симуляции
    for (int y = 0; y < MH; y++)
      for (int x = 0; x < MW; x++)
        px(x, y, fireColor(fR[((y*2)*FW + x*2) + 0]));
    FastLED.show();
    return;
  }
  fireLastMs = millis(); fireTick++;

  // 1. База: нижние 3 строки
  for (int x = 0; x < FW; x++) {
    float ph = sinf(x * 0.55f + fireTick * 0.22f) * cosf(x * 1.4f - fireTick * 0.17f);
    float burst = (random(100) < 12) ? random(100) / 222.f : 0;
    fR[(FH-1)*FW+x] = constrain(0.55f + ph * 0.22f + burst, 0, 1);
    fR[(FH-2)*FW+x] = constrain(fR[(FH-1)*FW+x] * 0.82f + random(100)/1250.f, 0, 1);
    fR[(FH-3)*FW+x] = constrain(fR[(FH-1)*FW+x] * 0.60f + random(100)/1667.f, 0, 1);
  }

  // 2. Язычки пламени
  for (int i = 0; i < 6; i++) {
    Tongue& t = tongues[i];
    if (--t.life <= 0) { respawnTongue(i); continue; }
    t.x += t.vx * 0.4f + (random(100) - 50) / 83.f;
    t.y += t.vy * 0.4f;
    t.vx += (random(100) - 50) / 125.f;
    t.heat *= 0.92f;
    int xi = ((int)t.x % FW + FW) % FW;
    int yi = (int)t.y;
    if (yi >= 0 && yi < FH - 3) {
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          int nx = (xi + dx + FW) % FW, ny = constrain(yi + dy, 0, FH-1);
          fR[ny * FW + nx] = constrain(fR[ny * FW + nx] + t.heat * 0.6f, 0, 1);
        }
    }
  }

  // 3. Диффузия с асимметричным ядром
  for (int y = 0; y < FH - 1; y++) {
    for (int x = 0; x < FW; x++) {
      float rk = 0.85f + random(100) / 333.f;
      float lw = fWind[y*FW+x] * sinf(fireTick * 0.08f + x * 0.3f) * 0.07f;
      float sum =
        fR[(y+1)*FW + (x-1+FW)%FW] * (0.20f + lw) +
        fR[(y+1)*FW + x]            * 0.32f +
        fR[(y+1)*FW + (x+1)%FW]    * (0.20f - lw) +
        fR[y*FW     + (x-1+FW)%FW] * 0.10f +
        fR[y*FW     + (x+1)%FW]    * 0.10f;
      // Охлаждение
      float cool = 0.012f + (float)y / FH * 0.022f + fCool[y*FW+x] * 0.008f;
      if (random(1000) < 4) cool += 0.7f * random(100) / 100.f;
      fW2[y*FW+x] = constrain(sum * rk - cool, 0, 1);
    }
  }

  // Пинг-понг
  float* tmp = fR; fR = fW2; fW2 = tmp;

  // 4. Рисуем (32×32 → 16×16 downscale)
  for (int y = 0; y < MH; y++)
    for (int x = 0; x < MW; x++) {
      float v = (fR[(y*2)*FW + x*2] + fR[(y*2)*FW + x*2+1] +
                 fR[(y*2+1)*FW + x*2] + fR[(y*2+1)*FW + x*2+1]) * 0.25f;
      px(x, MH - 1 - y, fireColor(v));
    }
  FastLED.show();
}

// ════════════════════════════════════════════════════════════
//  РЕЖИМ 4: БЕГУЩАЯ СТРОКА
// ════════════════════════════════════════════════════════════
static uint32_t scrollLastMs = 0;
void showScroll() {
  clearMatrix();
  uint8_t fi = min((int)cfg.fontIndex, 2);
  CRGB col = activeColor();
  int sy = (MH - 5) / 2;
  drawStr3(scrollX, sy, cfg.scrollText, col, fi);
  FastLED.show();

  uint32_t interval = 1000UL / max((int)cfg.scrollSpeed, 1);
  if (millis() - scrollLastMs >= interval) {
    scrollLastMs = millis();
    if (--scrollX < -(int)strW3(cfg.scrollText)) scrollX = MW;
  }
}

// ════════════════════════════════════════════════════════════
//  РЕЖИМ 5: MATRIX RAIN
// ════════════════════════════════════════════════════════════
struct MatrixCol { int head; int speed; uint8_t bright[MH]; };
static MatrixCol mtxCols[MW];
static bool mtxInit2 = false;
static uint32_t mtxLastMs = 0;

void mtxReset() {
  for (int x = 0; x < MW; x++) {
    mtxCols[x].head  = random(MH);
    mtxCols[x].speed = 1 + random(3);
    for (int y = 0; y < MH; y++) mtxCols[x].bright[y] = 0;
  }
  mtxInit2 = true;
}

void showMatrix() {
  if (!mtxInit2) mtxReset();
  if (millis() - mtxLastMs < 160) {
    // Просто перерисовываем
    clearMatrix();
    for (int x = 0; x < MW; x++)
      for (int y = 0; y < MH; y++) {
        uint8_t b = mtxCols[x].bright[y];
        if (b > 0) px(x, y, (y == mtxCols[x].head) ? CRGB(180,255,180) : CRGB(0,b,0));
      }
    FastLED.show(); return;
  }
  mtxLastMs = millis();

  for (int x = 0; x < MW; x++) {
    MatrixCol& mc = mtxCols[x];
    // Fade all
    for (int y = 0; y < MH; y++) { if (mc.bright[y] > 0) mc.bright[y] = mc.bright[y] * 3 / 4; }
    // Advance head
    if (random(4) < mc.speed) {
      mc.head = (mc.head + 1) % MH;
      mc.bright[mc.head] = 220 + random(35);
    }
    // Random respawn
    if (random(40) == 0) mc.speed = 1 + random(3);
  }

  clearMatrix();
  for (int x = 0; x < MW; x++)
    for (int y = 0; y < MH; y++) {
      uint8_t b = mtxCols[x].bright[y];
      if (b > 0) px(x, y, (y == mtxCols[x].head) ? CRGB(180,255,180) : CRGB(0,b,0));
    }
  FastLED.show();
}

// ════════════════════════════════════════════════════════════
//  РЕЖИМ 6: ПЛАЗМА
// ════════════════════════════════════════════════════════════
static float plasmaT = 0;
void showPlasma() {
  for (int y = 0; y < MH; y++) {
    for (int x = 0; x < MW; x++) {
      float v = sinf(x * 0.7f + plasmaT)
              + sinf(y * 0.5f + plasmaT * 1.3f)
              + sinf((x + y) * 0.4f + plasmaT * 0.7f)
              + sinf(sqrtf((x-7.5f)*(x-7.5f)+(y-7.5f)*(y-7.5f)) * 0.8f - plasmaT);
      uint8_t hue = (uint8_t)((v + 4) / 8.0f * 255);
      px(x, y, CHSV(hue, 255, 220));
    }
  }
  FastLED.show();
  plasmaT += 0.06f;
}

// ════════════════════════════════════════════════════════════
//  РЕЖИМ 7: МЕТЕОРЫ
// ════════════════════════════════════════════════════════════
struct Meteor { float x, y, vx, vy; uint8_t hue; int len; float trail[10][2]; };
static Meteor meteors[5];
static bool metInit = false;

void spawnMeteor(int i) {
  meteors[i].x   = random(MW);
  meteors[i].y   = 0;
  meteors[i].vx  = (random(100) - 50) / 50.f * 0.6f;
  meteors[i].vy  = 0.6f + random(100) / 200.f;
  meteors[i].hue = random(256);
  meteors[i].len = 5 + random(5);
  for (int j = 0; j < 10; j++) { meteors[i].trail[j][0] = meteors[i].x; meteors[i].trail[j][1] = meteors[i].y; }
}

static uint32_t metLastMs = 0;
void showMeteors() {
  if (!metInit) { for (int i = 0; i < 5; i++) spawnMeteor(i); metInit = true; }
  if (millis() - metLastMs < 120) return;
  metLastMs = millis();
  clearMatrix();

  for (int i = 0; i < 5; i++) {
    Meteor& m = meteors[i];
    // Сдвиг шлейфа
    for (int j = m.len - 1; j > 0; j--) {
      m.trail[j][0] = m.trail[j-1][0];
      m.trail[j][1] = m.trail[j-1][1];
    }
    m.trail[0][0] = m.x; m.trail[0][1] = m.y;

    m.x += m.vx; m.y += m.vy;
    if (m.y >= MH || m.x < 0 || m.x >= MW) spawnMeteor(i);

    // Рисуем шлейф
    for (int j = 0; j < m.len; j++) {
      uint8_t br = 255 - j * (255 / m.len);
      px((int)m.trail[j][0], (int)m.trail[j][1], CHSV(m.hue, 200, br));
    }
    // Голова — белая вспышка
    px((int)m.x, (int)m.y, CRGB::White);
  }
  FastLED.show();
}

// ════════════════════════════════════════════════════════════
//  РЕЖИМ 8: ЗМЕЙКА (автономный AI)
// ════════════════════════════════════════════════════════════
#define SN_LEN 64
struct Snake {
  int bx[SN_LEN], by[SN_LEN];
  int len, dx, dy;
  int fx, fy;
  bool alive;
};
static Snake sn;
static uint32_t snLastMs = 0;

void snPlace() {
  // Ищем свободную клетку за максимум MW*MH попыток.
  // Если доска заполнена (змейка выиграла) — оставляем позицию как есть.
  for (int attempt = 0; attempt < MW * MH; attempt++) {
    int fx = random(MW), fy = random(MH);
    bool hit = false;
    for (int i = 0; i < sn.len; i++)
      if (sn.bx[i] == fx && sn.by[i] == fy) { hit = true; break; }
    if (!hit) { sn.fx = fx; sn.fy = fy; return; }
  }
}
void snReset() {
  sn.bx[0]=8; sn.by[0]=8; sn.len=3; sn.dx=1; sn.dy=0; sn.alive=true;
  for(int i=1;i<3;i++){sn.bx[i]=8-i;sn.by[i]=8;}
  snPlace();
}

void showSnake() {
  if (!sn.alive) snReset();
  if (millis() - snLastMs < 180) {
    clearMatrix();
    for (int i = 0; i < sn.len; i++) {
      uint8_t br = 255 - i * (180 / sn.len);
      px(sn.bx[i], sn.by[i], (i==0) ? CRGB(CRGB::White) : CRGB(CHSV(100, 200, br)));
    }
    px(sn.fx, sn.fy, CRGB(255, 60, 0));
    FastLED.show();
    return;
  }
  snLastMs = millis();

  // AI: движемся к еде, избегаем стен и себя
  int bestDx = sn.dx, bestDy = sn.dy;
  int bestDist = 999;
  int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
  for (auto& d : dirs) {
    if (d[0] == -sn.dx && d[1] == -sn.dy) continue; // не разворачиваемся
    int nx = sn.bx[0] + d[0], ny = sn.by[0] + d[1];
    if (nx < 0 || nx >= MW || ny < 0 || ny >= MH) continue;
    bool hit = false;
    for (int i = 0; i < sn.len; i++) if (sn.bx[i]==nx && sn.by[i]==ny) { hit=true; break; }
    if (hit) continue;
    int dist = abs(nx - sn.fx) + abs(ny - sn.fy);
    if (dist < bestDist) { bestDist = dist; bestDx = d[0]; bestDy = d[1]; }
  }
  sn.dx = bestDx; sn.dy = bestDy;

  int nx = sn.bx[0] + sn.dx, ny = sn.by[0] + sn.dy;
  if (nx < 0 || nx >= MW || ny < 0 || ny >= MH) { sn.alive=false; return; }
  bool hit = false;
  for (int i = 0; i < sn.len; i++) if (sn.bx[i]==nx && sn.by[i]==ny) { hit=true; break; }
  if (hit) { sn.alive=false; return; }

  for (int i = min(sn.len, SN_LEN-1); i > 0; i--) { sn.bx[i]=sn.bx[i-1]; sn.by[i]=sn.by[i-1]; }
  sn.bx[0]=nx; sn.by[0]=ny;

  if (nx==sn.fx && ny==sn.fy) { if (sn.len < SN_LEN-1) sn.len++; snPlace(); }

  clearMatrix();
  for (int i = 0; i < sn.len; i++) {
    uint8_t br = 255 - i * (180 / sn.len);
    px(sn.bx[i], sn.by[i], (i==0) ? CRGB(CRGB::White) : CRGB(CHSV(100, 200, br)));
  }
  px(sn.fx, sn.fy, CRGB(255, 60, 0));
  FastLED.show();
}

// ════════════════════════════════════════════════════════════
//  РЕЖИМ 9: ЛАВА-ЛАМПА (меташары)
// ════════════════════════════════════════════════════════════
#define LN 7
struct Blob { float x, y, vx, vy, r, phase; };
static Blob blobs[LN];
static bool lavaInit2 = false;
static uint32_t lavaLastMs = 0;

void lavaSetup() {
  for (int i = 0; i < LN; i++) {
    blobs[i].x     = 2 + random(MW - 4);
    blobs[i].y     = 2 + random(MH - 4);
    blobs[i].vx    = (random(100) - 50) / 500.f;
    blobs[i].vy    = (random(100) - 50) / 500.f;
    blobs[i].r     = 1.8f + random(100) / 100.f;
    blobs[i].phase = random(628) / 100.f;
  }
  lavaInit2 = true;
}

CRGB lavaColorPx(float field, uint16_t hue, bool autoC) {
  float tmp = getTemp(); // один атомарный снимок
  uint8_t h = autoC ? (uint8_t)((hue == 0 && tmp > -999) ?
      constrain((int)(240 - (tmp + 20) / 60.f * 240), 0, 240) : hue) : (uint8_t)hue;

  float t = constrain((field - 0.4f) / 2.0f, 0, 1);
  uint8_t sat = 220 + (uint8_t)(35 * t);
  uint8_t val = (uint8_t)(30 + 225 * t);
  return CHSV(h, sat, val);
}

void showLava() {
  if (!lavaInit2) lavaSetup();
  if (millis() - lavaLastMs < 80) {
    FastLED.show(); return;
  }
  lavaLastMs = millis();

  float lavaT = millis() / 1000.f;

  // Обновляем шары
  for (int i = 0; i < LN; i++) {
    Blob& b = blobs[i];
    b.phase += 0.025f;
    float ny = b.y / (MH - 1);
    b.vy += (ny - 0.5f) * 0.006f; // плавучесть
    b.vx += (b.y * 0.0004f - b.x * 0.0004f); // вращение
    b.vx += sinf(b.phase * 0.9f) * 0.0012f;
    b.vy += cosf(b.phase * 0.7f) * 0.0012f;
    b.vx *= 0.92f; b.vy *= 0.92f;
    b.vx = constrain(b.vx, -0.18f, 0.18f);
    b.vy = constrain(b.vy, -0.18f, 0.18f);
    b.x += b.vx; b.y += b.vy;
    if (b.x < 0.5f) { b.x = 0.5f; b.vx *= -0.4f; }
    if (b.x > MW-1.5f) { b.x = MW-1.5f; b.vx *= -0.4f; }
    if (b.y < 0.5f) { b.y = 0.5f; b.vy *= -0.4f; }
    if (b.y > MH-1.5f) { b.y = MH-1.5f; b.vy *= -0.4f; }
    b.r = 1.9f + sinf(b.phase * 1.7f) * 0.55f + cosf(b.phase * 0.8f) * 0.35f;
  }

  // Рендер поля
  float lavaTmp = getTemp(); // один атомарный снимок на весь кадр
  uint16_t hue = cfg.lavaAutoColor ? (uint16_t)constrain(
    (int)(240 - (lavaTmp > -999 ? (lavaTmp + 20) / 60.f : 0.5f) * 240), 0, 240) : cfg.lavaHue;

  for (int y = 0; y < MH; y++) {
    for (int x = 0; x < MW; x++) {
      float field = 0;
      for (int i = 0; i < LN; i++) {
        float dx = x - blobs[i].x, dy = y - blobs[i].y;
        float d2 = dx*dx + dy*dy;
        if (d2 < 0.01f) d2 = 0.01f;
        field += blobs[i].r * blobs[i].r / d2;
      }
      float t = constrain((field - 0.4f) / 2.0f, 0, 1);
      uint8_t h2 = (uint8_t)hue;
      px(x, y, CHSV(h2, 210 + (uint8_t)(45*t), (uint8_t)(25 + 230*t)));
    }
  }
  FastLED.show();
}

// ════════════════════════════════════════════════════════════
//  ПОГОДА
// ════════════════════════════════════════════════════════════
void fetchWeather() {
  if (!wifiConnected) return;

  // Защита от одновременного использования WiFi-стека с core 1 (handleConnectWifi).
  // Таймаут 500 мс: если core 1 сейчас переконфигурирует WiFi — пропускаем цикл,
  // следующий запрос придёт через updateMin минут.
  if (xSemaphoreTake(wifiMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    Serial.println("[Weather] wifiMutex занят, запрос отложен");
    return;
  }

  bool isYa = (cfg.weatherProv == 1);
  HTTPClient http;
  http.setTimeout(8000);

  if (isYa) {
    // Яндекс Погода — HTTPS, сертификат не проверяем (CA не встроен)
    if (strlen(cfg.yaKey) == 0) { xSemaphoreGive(wifiMutex); return; }
    WiFiClientSecure sc;
    sc.setInsecure(); // без проверки TLS-сертификата
    String url = "https://api.weather.yandex.ru/v2/forecast?lat=" +
                 String(cfg.yaLat, 4) + "&lon=" + String(cfg.yaLon, 4) + "&limit=1";
    http.begin(sc, url);
    http.addHeader("X-Yandex-API-Key", String(cfg.yaKey));
  } else {
    // OpenWeatherMap — обычный HTTP
    if (strlen(cfg.apiKey) == 0) { xSemaphoreGive(wifiMutex); return; }
    String url = "http://api.openweathermap.org/data/2.5/weather?q=" +
                 String(cfg.city) + "&appid=" + String(cfg.apiKey) + "&units=metric";
    http.begin(url);
  }

  int code = http.GET();
  if (code == 200) {
    // getStream() парсит JSON прямо из TCP-буфера без выделения String в heap.
    // http.getString() копировал весь ответ (~1 KB) — при фрагментированной
    // куче это вызывало OOM и перезагрузку.
    JsonDocument doc;
    if (!deserializeJson(doc, http.getStream())) {
      // setTemp атомарно обновляет currentTemp + lastWeatherMs под спинлоком
      setTemp(isYa ? doc["fact"]["temp"].as<float>()
                   : doc["main"]["temp"].as<float>());
    }
  }
  http.end();
  xSemaphoreGive(wifiMutex);
}

// ════════════════════════════════════════════════════════════
//  КОНФИГ: сохранение / загрузка
// ════════════════════════════════════════════════════════════
void saveConfig() {
  prefs.begin("matrix", false);
  prefs.putUShort("ver", 312);
  prefs.putBytes("cfg", &cfg, sizeof(cfg));
  prefs.end();
}
void loadConfig() {
  prefs.begin("matrix", true);
  uint16_t ver = prefs.getUShort("ver", 0);
  if (ver == 312 && prefs.isKey("cfg")) {
    prefs.getBytes("cfg", &cfg, sizeof(cfg));
  }
  // ver != 310 → первый запуск / старый формат → defaults из struct
  prefs.end();
}

// ════════════════════════════════════════════════════════════
//  ВСТРОЕННЫЙ ВЕБ-ИНТЕРФЕЙС
// ════════════════════════════════════════════════════════════
static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="ru"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>LED Matrix</title>
<style>
:root{--bg:#111010;--panel:#1a1816;--card:#201e1b;--border:#2e2a24;--border2:#3d3830;
  --glow:#e8720c;--text:#e8ddd0;--muted:#6b6054;--muted2:#998880;--green:#4caf7d;}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--text);font-family:system-ui,sans-serif;min-height:100vh}
.shell{max-width:520px;margin:0 auto;padding:0 14px 40px}
header{padding:16px 0 12px;border-bottom:1px solid var(--border);margin-bottom:18px;
  display:flex;align-items:center;justify-content:space-between}
h1{font-size:1.05rem;font-weight:700;letter-spacing:.1em;color:#fff}
.hstats{display:flex;gap:14px;font-size:.75rem;color:var(--muted2)}
.dot{display:inline-block;width:7px;height:7px;border-radius:50%;background:var(--border2);margin-right:4px}
.dot.on{background:var(--green);box-shadow:0 0 6px var(--green)}
.preview-wrap{display:flex;justify-content:center;margin-bottom:16px}
canvas{border-radius:8px;border:1px solid var(--border2);image-rendering:pixelated}
.tabs{display:grid;grid-template-columns:repeat(4,1fr);gap:5px;margin-bottom:14px}
.tab{padding:8px 4px;border:1px solid var(--border);background:var(--card);color:var(--muted2);
  font-size:.8rem;font-weight:600;border-radius:8px;cursor:pointer;transition:.2s;text-align:center}
.tab.active{background:rgba(232,114,12,.1);border-color:var(--glow);color:var(--glow)}
.section{display:none}.section.visible{display:block}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:16px;margin-bottom:11px}
.card::before{content:'';display:block;height:1px;background:linear-gradient(90deg,transparent,rgba(232,114,12,.2),transparent);margin:-16px -16px 13px}
.ct{font-size:.58rem;letter-spacing:.18em;color:var(--muted2);text-transform:uppercase;margin-bottom:12px}
.field{margin-bottom:10px}.field:last-child{margin-bottom:0}
label{display:block;font-size:.73rem;font-weight:600;letter-spacing:.06em;text-transform:uppercase;color:var(--muted2);margin-bottom:4px}
input[type=text],input[type=password],input[type=number],select{width:100%;padding:8px 11px;
  background:var(--panel);border:1px solid var(--border2);border-radius:7px;color:var(--text);
  font-size:.83rem;outline:none;transition:.2s}
input:focus,select:focus{border-color:var(--glow)}
select option{background:var(--panel)}
.sr{display:flex;align-items:center;gap:9px}
input[type=range]{flex:1;-webkit-appearance:none;height:4px;border-radius:2px;background:var(--border2)}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:17px;height:17px;border-radius:50%;
  background:var(--glow);cursor:pointer;box-shadow:0 0 7px var(--glow);border:2px solid var(--card)}
.vb{min-width:40px;text-align:center;padding:3px 6px;background:var(--panel);
  border:1px solid var(--border2);border-radius:6px;font-size:.76rem;color:var(--glow)}
.tr{display:flex;align-items:center;justify-content:space-between;padding:3px 0}
.tl{font-size:.84rem;font-weight:500}
.tog{position:relative;width:42px;height:22px;flex-shrink:0}
.tog input{opacity:0;width:0;height:0}
.ts{position:absolute;inset:0;background:var(--border2);border-radius:11px;cursor:pointer;transition:.3s}
.ts::before{content:'';position:absolute;width:16px;height:16px;left:3px;top:3px;
  background:var(--muted2);border-radius:50%;transition:.3s}
.tog input:checked+.ts{background:rgba(232,114,12,.3);border:1px solid var(--glow)}
.tog input:checked+.ts::before{transform:translateX(20px);background:var(--glow)}
.mg{display:grid;grid-template-columns:1fr 1fr;gap:7px}
.mb{padding:11px 8px;border:1px solid var(--border2);border-radius:9px;background:var(--panel);
  color:var(--muted2);font-size:.82rem;font-weight:600;cursor:pointer;text-align:center;transition:.2s}
.mb:hover{border-color:var(--glow);color:var(--text)}
.mb.active{border-color:var(--glow);background:rgba(232,114,12,.1);color:var(--glow)}
.mb .mi{font-size:1.2rem;display:block;margin-bottom:3px}
.fb{padding:9px 6px;border:1px solid var(--border2);border-radius:9px;background:var(--panel);
  color:var(--muted2);font-size:.8rem;font-weight:600;cursor:pointer;text-align:center;transition:.2s;
  display:flex;flex-direction:column;align-items:center;gap:3px}
.fb:hover{border-color:var(--glow);color:var(--text)}
.fb.active{border-color:var(--glow);background:rgba(232,114,12,.1);color:var(--glow)}
.pb{padding:11px 8px;border:1px solid var(--border2);border-radius:10px;background:var(--panel);
  color:var(--muted2);cursor:pointer;text-align:center;transition:.2s;display:flex;flex-direction:column;align-items:center;gap:3px}
.pb:hover{border-color:var(--glow);color:var(--text)}
.pb.active{border-color:var(--glow);background:rgba(232,114,12,.1);color:var(--glow)}
.pn{font-size:.85rem;font-weight:700}.ps{font-size:.62rem;color:var(--muted);opacity:.8}
.cpr{display:flex;gap:7px;flex-wrap:wrap;margin-bottom:10px}
.cp{width:26px;height:26px;border-radius:5px;cursor:pointer;border:2px solid transparent;transition:.15s}
.cp:hover,.cp.s{border-color:#fff;transform:scale(1.2)}
.rr{display:grid;grid-template-columns:1fr 1fr 1fr;gap:7px}
.gb{height:9px;border-radius:5px;background:linear-gradient(90deg,#00f,#0cf,#0d8,#fd0,#f00);margin:6px 0 3px}
.gl{display:flex;justify-content:space-between;font-size:.65rem;color:var(--muted)}
.ti{display:flex;align-items:center;gap:9px;padding:8px 0 2px}
.tb{flex:1;height:5px;background:linear-gradient(90deg,#00f,#0cf,#0d8,#fd0,#f00);border-radius:3px;position:relative}
.tn{position:absolute;top:-5px;width:14px;height:14px;background:#fff;border-radius:50%;
  border:2px solid var(--card);transform:translateX(-50%);box-shadow:0 0 7px rgba(255,255,255,.6);transition:left .8s}
.hint{background:rgba(232,114,12,.06);border:1px solid rgba(232,114,12,.18);border-radius:10px;
  padding:10px 14px;display:flex;align-items:center;gap:10px;font-size:.82rem;color:var(--muted2);margin-bottom:11px}
.hint strong{color:var(--text)}
.sg{display:grid;grid-template-columns:1fr 1fr;gap:9px}
.si{background:var(--panel);border:1px solid var(--border);border-radius:9px;padding:11px}
.sl{font-size:.63rem;letter-spacing:.1em;text-transform:uppercase;color:var(--muted);margin-bottom:4px}
.sv{font-size:1.05rem;color:var(--glow)}
.sv.a{color:#f59e0b}.sv.p{color:#c98a5a}
.si.wide{grid-column:1/-1}
.btn-save{width:100%;padding:12px;background:linear-gradient(135deg,rgba(232,114,12,.16),rgba(232,114,12,.05));
  border:1px solid var(--glow);border-radius:10px;color:var(--glow);font-size:.72rem;font-weight:700;
  letter-spacing:.15em;text-transform:uppercase;cursor:pointer;margin-top:6px;transition:.2s}
.btn-save:hover{box-shadow:0 0 18px rgba(232,114,12,.3)}
.btn-sec{width:100%;padding:9px;background:transparent;border:1px solid var(--border2);
  border-radius:8px;color:var(--muted2);font-size:.82rem;font-weight:600;cursor:pointer;transition:.2s;margin-top:7px}
.btn-sec:hover{color:var(--text);border-color:var(--glow)}
.ctx{margin-top:0}.g2{display:grid;grid-template-columns:3fr 1fr;gap:7px}
.g11{display:grid;grid-template-columns:1fr 1fr;gap:7px}
.toast{position:fixed;bottom:22px;left:50%;transform:translateX(-50%) translateY(18px);
  background:var(--card);border:1px solid var(--glow);color:var(--glow);padding:9px 20px;
  border-radius:8px;font-size:.78rem;opacity:0;transition:all .3s;pointer-events:none;z-index:99}
.toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
.sub{color:var(--muted);font-size:.53rem;font-weight:400;letter-spacing:.03em;text-transform:none;margin-left:4px}
.wbanner{background:rgba(232,114,12,.08);border:1px solid rgba(232,114,12,.3);border-radius:12px;
  padding:14px 16px;margin-bottom:16px;text-align:center;font-size:.85rem;color:var(--muted2)}
.wbanner b{color:var(--glow)}
.wbanner .arrow{cursor:pointer;text-decoration:underline;color:var(--glow)}
.net{padding:10px 12px;border:1px solid var(--border2);border-radius:8px;margin-bottom:5px;
  cursor:pointer;display:flex;justify-content:space-between;align-items:center;transition:.2s;font-size:.82rem}
.net:hover{border-color:var(--glow);background:rgba(232,114,12,.05)}
.net.sel{border-color:var(--glow);background:rgba(232,114,12,.1)}
.net .nm{font-weight:600;color:var(--text)}.net .lock{margin-right:4px;font-size:.7rem}
.net .sig{color:var(--muted);font-family:monospace;font-size:.72rem}
.spin{display:inline-block;animation:sp .8s linear infinite}@keyframes sp{to{transform:rotate(360deg)}}
.conn-ok{color:var(--green);font-weight:600;font-size:.85rem;padding:8px 0}
.conn-fail{color:#e85050;font-size:.82rem;padding:8px 0}
.pass-wrap{position:relative}
.pass-wrap input{padding-right:36px}
.eye{position:absolute;right:8px;top:50%;transform:translateY(-50%);cursor:pointer;font-size:.9rem;user-select:none;opacity:.5}
.eye:hover{opacity:1}
</style></head><body>
<div class="shell">
<header>
  <h1>LED MATRIX</h1>
  <div class="hstats">
    <span><span class="dot" id="wdot"></span><span id="wstat">—</span></span>
    <span>🌡 <span id="htemp">—</span></span>
    <span>🕐 <span id="htime">—</span></span>
  </div>
</header>

<div class="preview-wrap">
  <canvas id="cv" width="192" height="192"></canvas>
</div>

<div class="tabs">
  <button class="tab active" onclick="showTab('modes')">⊞ Режим</button>
  <button class="tab" onclick="showTab('display')">⚙ Дисплей</button>
  <button class="tab" onclick="showTab('weather')">🌡 Погода</button>
  <button class="tab" onclick="showTab('wifi')">📡 WiFi</button>
</div>

<div id="apBanner" class="wbanner" style="display:none">
  📡 Точка доступа <b>ESP32-Matrix</b> (пароль: <b>12345678</b>)<br>
  <span style="font-size:.78rem">Выберите сеть во вкладке <span class="arrow" onclick="showTab('wifi')">📡 WiFi</span></span>
</div>

<!-- TAB 1: РЕЖИМ -->
<div id="tab-modes" class="section visible">
  <div class="card">
    <div class="ct">Режим отображения</div>
    <div class="mg">
      <button class="mb active" data-m="0" onclick="setMode(0)"><span class="mi">🌡</span>Температура</button>
      <button class="mb" data-m="1" onclick="setMode(1)"><span class="mi">🕐</span>Часы</button>
      <button class="mb" data-m="2" onclick="setMode(2)"><span class="mi">🌈</span>Радуга</button>
      <button class="mb" data-m="3" onclick="setMode(3)"><span class="mi">🔥</span>Огонь</button>
      <button class="mb" data-m="9" onclick="setMode(9)"><span class="mi">🫧</span>Лава</button>
      <button class="mb" data-m="4" onclick="setMode(4)"><span class="mi">📜</span>Бегущая строка</button>
      <button class="mb" data-m="5" onclick="setMode(5)"><span class="mi">💻</span>Матрица</button>
      <button class="mb" data-m="6" onclick="setMode(6)"><span class="mi">🌊</span>Плазма</button>
      <button class="mb" data-m="7" onclick="setMode(7)"><span class="mi">☄️</span>Метеоры</button>
      <button class="mb" data-m="8" onclick="setMode(8)"><span class="mi">🐍</span>Змейка</button>
    </div>
  </div>

  <!-- ctx: Температура -->
  <div class="ctx" id="ctx-temp">
    <div class="hint">💡 <span>Цвет и шрифт — вкладка <strong>Дисплей</strong> · данные — <strong>Погода</strong></span></div>
  </div>

  <!-- ctx: Часы -->
  <div class="ctx" id="ctx-clock" style="display:none">
    <div class="card">
      <div class="ct">Вид часов</div>
      <div class="mg" id="faceGrid">
        <button class="fb active" data-f="0" onclick="setFace(0)"><span>📺</span>ЧЧ / ММ</button>
        <button class="fb" data-f="1" onclick="setFace(1)"><span>⏰</span>ЧЧ:ММ строка</button>
        <button class="fb" data-f="2" onclick="setFace(2)"><span>🕐</span>Аналоговый</button>
        <button class="fb" data-f="3" onclick="setFace(3)"><span>⚫</span>Двоичный</button>
        <button class="fb" data-f="4" onclick="setFace(4)" style="grid-column:1/-1"><span>💡</span>Кольцо минут</button>
      </div>
      <div style="margin-top:13px;display:flex;flex-direction:column;gap:9px">
        <div class="tr"><span class="tl">24-часовой формат</span>
          <label class="tog"><input type="checkbox" id="use24h" checked><span class="ts"></span></label></div>
        <div class="tr"><span class="tl">Мигающий разделитель :</span>
          <label class="tog"><input type="checkbox" id="blinkColon" checked><span class="ts"></span></label></div>
      </div>
      <div class="g2" style="margin-top:11px">
        <div class="field" style="margin-bottom:0"><label>NTP сервер</label><input type="text" id="ntpServer" value="pool.ntp.org"></div>
        <div class="field" style="margin-bottom:0"><label>UTC+</label><input type="number" id="tzOffset" value="3" min="-12" max="14"></div>
      </div>
    </div>
  </div>

  <!-- ctx: Бегущая строка -->
  <div class="ctx" id="ctx-scroll" style="display:none">
    <div class="card">
      <div class="ct">Бегущая строка</div>
      <div class="field"><label>Текст (Latin, Кириллица)</label>
        <input type="text" id="scrollText" value="ESP32 Matrix" maxlength="127"></div>
      <div class="field" style="margin-bottom:0"><label>Скорость (пикс/сек)</label>
        <div class="sr"><input type="range" id="scrollSpeed" min="1" max="20" value="8" oninput="sv(this,'svSpd')">
        <span class="vb" id="svSpd">8</span></div>
      </div>
    </div>
  </div>

  <!-- ctx: Лава -->
  <div class="ctx" id="ctx-lava" style="display:none">
    <div class="card">
      <div class="ct">Цвет лава-лампы</div>
      <div class="tr" style="margin-bottom:13px">
        <span class="tl">Авто-цвет по температуре</span>
        <label class="tog"><input type="checkbox" id="lavaAuto" checked onchange="onLavaAuto()"><span class="ts"></span></label>
      </div>
      <div id="lavaAutoLegend">
        <div class="gb"></div>
        <div class="gl"><span>−30°</span><span>0°</span><span>+40°</span></div>
        <div class="ti"><span style="font-size:.67rem;color:var(--muted)">NOW</span>
          <div class="tb"><div class="tn" id="lavaNeedle"></div></div></div>
      </div>
      <div id="lavaHueWrap" style="display:none">
        <div style="margin-bottom:7px;font-size:.6rem;letter-spacing:.14em;color:var(--muted2);text-transform:uppercase">Оттенок</div>
        <div id="lavaStrip" style="height:20px;border-radius:10px;cursor:pointer;user-select:none;touch-action:none;position:relative;margin-bottom:10px;
          background:linear-gradient(90deg,hsl(0,90%,40%),hsl(60,90%,45%),hsl(120,80%,35%),hsl(180,85%,35%),hsl(240,85%,40%),hsl(300,85%,42%),hsl(360,90%,40%))"
          onmousedown="lavaDown(event)">
          <div id="lavaTh" style="position:absolute;top:50%;width:22px;height:22px;border-radius:50%;background:#fff;
            border:2.5px solid #222;box-shadow:0 0 8px rgba(0,0,0,.7);transform:translate(-50%,-50%);left:4%;pointer-events:none"></div>
        </div>
      </div>
    </div>
  </div>
</div>

<!-- TAB 2: ДИСПЛЕЙ -->
<div id="tab-display" class="section">
  <div class="card">
    <div class="ct">Яркость матрицы</div>
    <div class="sr">
      <input type="range" id="brightness" min="5" max="255" value="80" oninput="sv(this,'svBr')">
      <span class="vb" id="svBr">80</span>
    </div>
  </div>
  <div class="card">
    <div class="ct">Цвет пикселей<span class="sub">температура · часы · плазма</span></div>
    <div class="tr" style="margin-bottom:11px">
      <span class="tl">Авто-цвет по температуре</span>
      <label class="tog"><input type="checkbox" id="autoColor" checked onchange="onAutoColor()"><span class="ts"></span></label>
    </div>
    <div id="autoLegend">
      <div class="gb"></div>
      <div class="gl"><span>−30°</span><span>−10°</span><span>0°</span><span>+20°</span><span>+40°</span></div>
      <div class="ti"><span style="font-size:.67rem;color:var(--muted)">NOW</span>
        <div class="tb"><div class="tn" id="tempNeedle"></div></div></div>
    </div>
    <div id="customWrap" style="display:none;margin-top:4px">
      <div class="cpr" id="cpresets"></div>
      <div class="rr">
        <div><label>R</label><input type="number" id="cR" min="0" max="255" value="255"></div>
        <div><label>G</label><input type="number" id="cG" min="0" max="255" value="128"></div>
        <div><label>B</label><input type="number" id="cB" min="0" max="255" value="0"></div>
      </div>
    </div>
  </div>
  <div class="card">
    <div class="ct">Шрифт<span class="sub">температура и часы</span></div>
    <select id="fontIndex">
      <option value="0">Micro 3×5 — компактный</option>
      <option value="1">Round 3×5 — скруглённый</option>
      <option value="2">Bold 3×5  — жирный</option>
      <option value="3">Big 5×7   — крупные цифры</option>
    </select>
  </div>
  <div class="card">
    <div class="ct">Поворот экрана</div>
    <div class="mg">
      <button class="mb active" data-r="0" onclick="setRotation(0)"><span class="mi">⬆</span>0° (норма)</button>
      <button class="mb" data-r="1" onclick="setRotation(1)"><span class="mi">➡</span>90° CW</button>
      <button class="mb" data-r="2" onclick="setRotation(2)"><span class="mi">⬇</span>180°</button>
      <button class="mb" data-r="3" onclick="setRotation(3)"><span class="mi">⬅</span>270° CW</button>
    </div>
  </div>
  <div class="card">
    <div class="ct">Отражение</div>
    <div class="g11">
      <div class="tr" style="border:1px solid var(--border2);border-radius:9px;padding:10px 12px;background:var(--panel)">
        <span class="tl" style="font-size:.85rem">↔ Горизонталь</span>
        <label class="tog"><input type="checkbox" id="flipH" onchange="onFlip()"><span class="ts"></span></label>
      </div>
      <div class="tr" style="border:1px solid var(--border2);border-radius:9px;padding:10px 12px;background:var(--panel)">
        <span class="tl" style="font-size:.85rem">↕ Вертикаль</span>
        <label class="tog"><input type="checkbox" id="flipV" onchange="onFlip()"><span class="ts"></span></label>
      </div>
    </div>
  </div>
</div>

<!-- TAB 3: ПОГОДА -->
<div id="tab-weather" class="section">
  <div class="card">
    <div class="ct">Источник данных</div>
    <div class="g11">
      <button class="pb active" id="provOwm" onclick="setProv('owm')">
        <span style="font-size:1.3rem">🌍</span>
        <span class="pn">OpenWeatherMap</span><span class="ps">Бесплатный ключ</span>
      </button>
      <button class="pb" id="provYa" onclick="setProv('ya')">
        <span style="font-size:1.3rem">🟡</span>
        <span class="pn">Яндекс</span><span class="ps">Коммерческий API</span>
      </button>
    </div>
  </div>
  <div class="card" id="owmCard">
    <div class="ct">OpenWeatherMap</div>
    <div class="field"><label>API ключ</label><input type="text" id="apiKey" placeholder="Вставьте ключ"></div>
    <div class="field" style="margin-bottom:0"><label>Город</label><input type="text" id="city" value="Moscow"></div>
  </div>
  <div class="card" id="yaCard" style="display:none">
    <div class="ct">Яндекс Погода</div>
    <div class="field"><label>API ключ</label><input type="text" id="yaKey" placeholder="X-Yandex-API-Key"></div>
    <div class="g11">
      <div class="field" style="margin-bottom:0"><label>Широта</label><input type="number" id="yaLat" value="55.7558" step="0.0001"></div>
      <div class="field" style="margin-bottom:0"><label>Долгота</label><input type="number" id="yaLon" value="37.6176" step="0.0001"></div>
    </div>
  </div>
  <div class="card">
    <div class="ct">Параметры обновления</div>
    <div class="field" style="margin-bottom:11px"><label>Интервал (мин)</label>
      <div class="sr"><input type="range" id="updateMin" min="1" max="60" value="10" oninput="sv(this,'svUpd')">
      <span class="vb" id="svUpd">10</span></div>
    </div>
    <button class="btn-sec" style="margin-top:0" onclick="forceUpdate()">🔄 Запросить сейчас</button>
  </div>
</div>

<!-- TAB 4: WiFi -->
<div id="tab-wifi" class="section">
  <div class="card">
    <div class="ct">Подключение к WiFi</div>
    <div id="wifiStatus" style="margin-bottom:12px"></div>
    <button class="btn-sec" style="margin-top:0;margin-bottom:12px" id="scanBtn" onclick="scanWifi()">📡 Сканировать сети</button>
    <div id="netList" style="max-height:220px;overflow-y:auto;margin-bottom:12px"></div>
    <div class="field"><label>Имя сети (SSID)</label><input type="text" id="ssid" placeholder="Выберите из списка или введите вручную"></div>
    <div class="field"><label>Пароль</label>
      <div class="pass-wrap"><input type="password" id="wifiPass" placeholder="Пароль сети">
        <span class="eye" onclick="togglePass()">👁</span></div></div>
    <div id="connResult"></div>
    <button class="btn-save" style="margin-top:8px" onclick="connectWifi()">⚡ ПОДКЛЮЧИТЬ</button>
    <button class="btn-sec" id="disconnBtn" style="display:none" onclick="disconnWifi()">✕ Отключиться и забыть сеть</button>
  </div>
  <div class="card">
    <div class="ct">Статус устройства</div>
    <div class="sg">
      <div class="si"><div class="sl">IP адрес</div><div class="sv" id="ipVal" style="font-size:.85rem">—</div></div>
      <div class="si"><div class="sl">Uptime</div><div class="sv a" id="uptVal">—</div></div>
      <div class="si"><div class="sl">Память</div><div class="sv" id="heapVal">—</div></div>
      <div class="si"><div class="sl">WiFi сигнал</div><div class="sv p" id="rssiVal">—</div></div>
    </div>
  </div>
  <div class="card">
    <div class="ct">Точка доступа</div>
    <div style="font-size:.78rem;color:var(--muted2);line-height:2">
      SSID: <strong style="color:var(--text)">ESP32-Matrix</strong> &nbsp;·&nbsp;
      Пароль: <strong style="color:var(--text)">12345678</strong><br>
      Адрес: <strong style="color:var(--glow)">192.168.4.1</strong>
    </div>
  </div>
  <div class="card">
    <div class="ct">Управление устройством</div>
    <button class="btn-sec" style="color:#e85050;border-color:#e85050" onclick="rebootDevice()">🔄 Перезагрузить устройство</button>
  </div>
</div>

<button class="btn-save" onclick="saveAll()">◉ &nbsp;СОХРАНИТЬ В ПАМЯТЬ</button>
</div><!-- /shell -->
<div class="toast" id="toastEl"></div>

<script>
const TABS=['modes','display','weather','wifi'];
const CTX={0:'ctx-temp',1:'ctx-clock',4:'ctx-scroll',9:'ctx-lava'};
let st={mode:0,clockFace:0,prov:'owm',lavaHue:15,rotation:0,flipH:false,flipV:false};

// ── Canvas preview ──
const cv=document.getElementById('cv'),ctx=cv.getContext('2d');
function drawMatrix(arr){
  // arr передаётся в XY-порядке (строка за строкой), индекс = y*16+x
  const SZ=12;
  ctx.fillStyle='#0a0c18';ctx.fillRect(0,0,192,192);
  for(let y=0;y<16;y++)for(let x=0;x<16;x++){
    const v=arr[y*16+x]||0,r=(v>>16)&0xff,g=(v>>8)&0xff,b=v&0xff;
    if(r||g||b){
      ctx.fillStyle=`rgb(${r},${g},${b})`;
      ctx.fillRect(x*SZ+1,y*SZ+1,SZ-2,SZ-2);
      // Блик
      ctx.fillStyle='rgba(255,255,255,0.13)';
      ctx.fillRect(x*SZ+3,y*SZ+2,SZ-6,3);
    } else {
      ctx.fillStyle='#141828';
      ctx.fillRect(x*SZ+1,y*SZ+1,SZ-2,SZ-2);
    }
  }
  // Сетка
  ctx.strokeStyle='rgba(255,255,255,0.04)';ctx.lineWidth=1;
  for(let i=0;i<=16;i++){
    ctx.beginPath();ctx.moveTo(i*SZ,0);ctx.lineTo(i*SZ,192);ctx.stroke();
    ctx.beginPath();ctx.moveTo(0,i*SZ);ctx.lineTo(192,i*SZ);ctx.stroke();
  }
}

// ── Tabs ──
function showTab(t){
  TABS.forEach((id,i)=>{
    document.getElementById('tab-'+id).classList.toggle('visible',id===t);
    document.querySelectorAll('.tab')[i].classList.toggle('active',id===t);
  });
}
function setMode(m){
  st.mode=m;
  document.querySelectorAll('.mb').forEach(b=>b.classList.toggle('active',+b.dataset.m===m));
  document.querySelectorAll('.ctx').forEach(p=>p.style.display='none');
  const c=CTX[m];if(c)document.getElementById(c).style.display='block';
}
function setFace(f){
  st.clockFace=f;
  document.querySelectorAll('.fb').forEach(b=>b.classList.toggle('active',+b.dataset.f===f));
}
function setRotation(r){
  st.rotation=r;
  document.querySelectorAll('[data-r]').forEach(b=>b.classList.toggle('active',+b.dataset.r===r));
}
function onFlip(){
  st.flipH=document.getElementById('flipH').checked;
  st.flipV=document.getElementById('flipV').checked;
}

// ── Lava hue ──
let _lavaD=false;
function lavaDown(e){_lavaD=true;lavaMv(e.touches?e.touches[0]:e);
  const mv=e2=>lavaMv(e2.touches?e2.touches[0]:e2);
  const up=()=>{_lavaD=false;window.removeEventListener('mousemove',mv);window.removeEventListener('touchmove',mv);
    window.removeEventListener('mouseup',up);window.removeEventListener('touchend',up);};
  window.addEventListener('mousemove',mv);window.addEventListener('touchmove',mv);
  window.addEventListener('mouseup',up);window.addEventListener('touchend',up);}
function lavaMv(e){if(!_lavaD)return;
  const r=document.getElementById('lavaStrip').getBoundingClientRect();
  const p=Math.max(0,Math.min(1,(e.clientX-r.left)/r.width));
  st.lavaHue=Math.round(p*359);
  document.getElementById('lavaTh').style.left=(p*100)+'%';}
function onLavaAuto(){
  const a=document.getElementById('lavaAuto').checked;
  document.getElementById('lavaAutoLegend').style.display=a?'block':'none';
  document.getElementById('lavaHueWrap').style.display=a?'none':'block';}
function updLavaNeedle(){
  const n=document.getElementById('lavaNeedle');
  if(n&&window._temp!==undefined)n.style.left=(Math.max(0,Math.min(1,(window._temp+30)/70))*100)+'%';}

// ── Color ──
function onAutoColor(){
  const a=document.getElementById('autoColor').checked;
  document.getElementById('autoLegend').style.display=a?'block':'none';
  document.getElementById('customWrap').style.display=a?'none':'block';}
function buildPresets(){
  const el=document.getElementById('cpresets');
  [[255,0,0],[255,80,0],[255,200,0],[0,200,60],[0,200,255],[0,60,255],[160,0,255],[255,255,255]].forEach(([r,g,b])=>{
    const d=document.createElement('div');d.className='cp';d.style.background=`rgb(${r},${g},${b})`;
    d.onclick=()=>{document.getElementById('cR').value=r;document.getElementById('cG').value=g;document.getElementById('cB').value=b;
      document.querySelectorAll('.cp').forEach(x=>x.classList.remove('s'));d.classList.add('s');};
    el.appendChild(d);});}

// ── Provider ──
function setProv(p){
  st.prov=p;
  document.getElementById('provOwm').classList.toggle('active',p==='owm');
  document.getElementById('provYa').classList.toggle('active',p==='ya');
  document.getElementById('owmCard').style.display=p==='owm'?'block':'none';
  document.getElementById('yaCard').style.display=p==='ya'?'block':'none';}

// ── Helpers ──
function sv(el,id){document.getElementById(id).textContent=el.value;}
function toast(msg){const el=document.getElementById('toastEl');el.textContent=msg;
  el.classList.add('show');setTimeout(()=>el.classList.remove('show'),2600);}
function fmtTime(){const n=new Date();
  return String(n.getHours()).padStart(2,'0')+':'+String(n.getMinutes()).padStart(2,'0')+':'+String(n.getSeconds()).padStart(2,'0');}

// ── API ──
async function loadSettings(){
  try{const d=await(await fetch('/api/settings')).json();
    st.mode=d.mode||0; st.clockFace=d.clockFace||0; st.lavaHue=d.lavaHue||15;
    setMode(d.mode||0); setFace(d.clockFace||0);
    document.getElementById('brightness').value=d.brightness||80;
    document.getElementById('svBr').textContent=d.brightness||80;
    document.getElementById('fontIndex').value=d.fontIndex||0;
    document.getElementById('autoColor').checked=d.autoColor!==false;onAutoColor();
    document.getElementById('cR').value=d.customR||255;
    document.getElementById('cG').value=d.customG||128;
    document.getElementById('cB').value=d.customB||0;
    document.getElementById('use24h').checked=d.use24h!==false;
    document.getElementById('blinkColon').checked=d.blinkColon!==false;
    document.getElementById('ntpServer').value=d.ntpServer||'pool.ntp.org';
    document.getElementById('tzOffset').value=d.tzOffset||3;
    document.getElementById('scrollText').value=d.scrollText||'ESP32 Matrix';
    document.getElementById('scrollSpeed').value=d.scrollSpeed||8;
    document.getElementById('svSpd').textContent=d.scrollSpeed||8;
    document.getElementById('updateMin').value=d.updateMin||10;
    document.getElementById('svUpd').textContent=d.updateMin||10;
    document.getElementById('apiKey').value=d.apiKey||'';
    document.getElementById('city').value=d.city||'Moscow';
    document.getElementById('yaKey').value=d.yaKey||'';
    document.getElementById('yaLat').value=d.yaLat||55.7558;
    document.getElementById('yaLon').value=d.yaLon||37.6176;
    setProv(d.weatherProv===1?'ya':'owm');
    document.getElementById('lavaAuto').checked=d.lavaAutoColor!==false;onLavaAuto();
    st.lavaHue=d.lavaHue||15;
    document.getElementById('lavaTh').style.left=(st.lavaHue/359*100)+'%';
    document.getElementById('ssid').value=d.ssid||'';
    st.rotation=d.rotation||0;setRotation(d.rotation||0);
    st.flipH=!!d.flipH;st.flipV=!!d.flipV;
    document.getElementById('flipH').checked=!!d.flipH;
    document.getElementById('flipV').checked=!!d.flipV;
  }catch(e){}}

async function fetchStatus(){
  try{const d=await(await fetch('/api/status')).json();
    window._temp=d.temp;
    document.getElementById('wdot').className='dot '+(d.wifi?'on':'');
    document.getElementById('wstat').textContent=d.wifi?(d.ip||'OK'):'—';
    const s=d.temp>-999?((d.temp>=0?'+':'')+d.temp.toFixed(1)+'°C'):'—';
    document.getElementById('htemp').textContent=s;
    document.getElementById('htime').textContent=fmtTime();
    document.getElementById('ipVal').textContent=d.ip||'—';
    document.getElementById('uptVal').textContent=d.uptime||'—';
    document.getElementById('heapVal').textContent=d.heap||'—';
    document.getElementById('rssiVal').textContent=d.rssi||'—';
    // Иглы
    if(d.temp>-999){
      const pct=(Math.max(0,Math.min(1,(d.temp+30)/70))*100)+'%';
      document.getElementById('tempNeedle').style.left=pct;
      updLavaNeedle();
    }
    if(d.matrix)drawMatrix(d.matrix);
    updateWifiUI(d);
  }catch(e){}}

async function saveAll(){
  const body={
    mode:st.mode,clockFace:st.clockFace,
    brightness:+document.getElementById('brightness').value,
    fontIndex:+document.getElementById('fontIndex').value,
    autoColor:document.getElementById('autoColor').checked,
    customR:+document.getElementById('cR').value,
    customG:+document.getElementById('cG').value,
    customB:+document.getElementById('cB').value,
    use24h:document.getElementById('use24h').checked,
    blinkColon:document.getElementById('blinkColon').checked,
    ntpServer:document.getElementById('ntpServer').value,
    tzOffset:+document.getElementById('tzOffset').value,
    scrollText:document.getElementById('scrollText').value,
    scrollSpeed:+document.getElementById('scrollSpeed').value,
    updateMin:+document.getElementById('updateMin').value,
    apiKey:document.getElementById('apiKey').value,
    city:document.getElementById('city').value,
    yaKey:document.getElementById('yaKey').value,
    yaLat:+document.getElementById('yaLat').value,
    yaLon:+document.getElementById('yaLon').value,
    weatherProv:st.prov==='ya'?1:0,
    lavaAutoColor:document.getElementById('lavaAuto').checked,
    lavaHue:st.lavaHue,
    rotation:st.rotation,
    flipH:st.flipH,
    flipV:st.flipV,
  };
  try{await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    toast('✅ Настройки сохранены в flash');}
  catch(e){toast('❌ Ошибка сохранения');}}

function togglePass(){const p=document.getElementById('wifiPass');p.type=p.type==='password'?'text':'password';}

// ── WiFi scan ──
function rssiBar(r){return r>-50?'▂▄▆█':r>-65?'▂▄▆░':r>-75?'▂▄░░':'▂░░░';}
async function scanWifi(){
  const btn=document.getElementById('scanBtn');
  const el=document.getElementById('netList');
  btn.disabled=true;btn.textContent='⏳ Сканирование…';
  el.innerHTML='';
  try{
    const d=await(await fetch('/api/scan')).json();
    if(!d.networks||!d.networks.length){
      el.innerHTML='<div style="text-align:center;color:var(--muted2);padding:14px">Сети не найдены</div>';return;}
    const seen=new Set();
    d.networks.sort((a,b)=>b.rssi-a.rssi).forEach(n=>{
      if(!n.ssid||seen.has(n.ssid))return;seen.add(n.ssid);
      const div=document.createElement('div');div.className='net';
      div.innerHTML='<span>'+(n.enc?'<span class="lock">🔒</span>':'')+
        '<span class="nm">'+n.ssid+'</span></span>'+
        '<span class="sig">'+rssiBar(n.rssi)+' '+n.rssi+'</span>';
      div.onclick=()=>{
        document.getElementById('ssid').value=n.ssid;
        document.getElementById('wifiPass').value='';
        if(n.enc)document.getElementById('wifiPass').focus();
        el.querySelectorAll('.net').forEach(x=>x.classList.remove('sel'));
        div.classList.add('sel');
        document.getElementById('connResult').innerHTML='';
      };
      el.appendChild(div);
    });
  }catch(e){el.innerHTML='<div style="text-align:center;color:var(--red);padding:14px">Ошибка сканирования</div>';}
  finally{btn.disabled=false;btn.textContent='📡 Сканировать сети';}}

async function connectWifi(){
  const ssid=document.getElementById('ssid').value;
  const pass=document.getElementById('wifiPass').value;
  if(!ssid){toast('❌ Введите имя сети');return;}
  const res=document.getElementById('connResult');
  res.innerHTML='<span style="display:inline-block;animation:sp .8s linear infinite">⏳</span> Подключение к <b>'+ssid+'</b>… (до 15 сек)';
  try{
    const d=await(await fetch('/api/connect',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({ssid,password:pass})})).json();
    if(d.ok){
      res.innerHTML='<div class="conn-ok">✅ Подключено! IP: <b>'+d.ip+'</b></div>';
      toast('✅ WiFi: '+d.ip);
      document.getElementById('disconnBtn').style.display='block';
      setTimeout(fetchStatus,1000);
    }else{
      const reasons={4:'таймаут',6:'неверный пароль',1:'сеть не найдена'};
      res.innerHTML='<div class="conn-fail">❌ '+(reasons[d.status]||'Ошибка, код: '+d.status)+'</div>';
    }
  }catch(e){res.innerHTML='<div class="conn-fail">❌ Нет связи с устройством</div>';}}

async function disconnWifi(){
  try{await fetch('/api/disconnect',{method:'POST'});
    toast('WiFi отключён');
    document.getElementById('connResult').innerHTML='';
    document.getElementById('disconnBtn').style.display='none';
    document.getElementById('ssid').value='';
    setTimeout(fetchStatus,1000);
  }catch(e){}}

function updateWifiUI(d){
  const ws=document.getElementById('wifiStatus');
  const banner=document.getElementById('apBanner');
  const disc=document.getElementById('disconnBtn');
  if(d.wifi){
    ws.innerHTML='<div class="conn-ok">✅ Подключено к <b>'+(d.ssid||'')+'</b> — IP: <b>'+(d.ip||'—')+'</b></div>';
    banner.style.display='none';
    disc.style.display='block';
  }else{
    ws.innerHTML='<div style="color:var(--muted2);font-size:.83rem">⚠ Не подключено к WiFi</div>';
    banner.style.display='block';
    disc.style.display='none';
  }}

async function forceUpdate(){
  await fetch('/api/update',{method:'POST'});
  toast('🔄 Запрос погоды отправлен');
  setTimeout(fetchStatus,4000);}

async function rebootDevice(){
  if(!confirm('Перезагрузить устройство?'))return;
  try{await fetch('/api/reboot',{method:'POST'});
    toast('🔄 Перезагрузка...');}
  catch(e){toast('Перезагрузка начата');}}

buildPresets();
loadSettings();
fetchStatus();
setInterval(fetchStatus,3000);
</script>
</body></html>)HTML";

// ════════════════════════════════════════════════════════════
//  ВЕБ-СЕРВЕР: обработчики
// ════════════════════════════════════════════════════════════
void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleGetSettings() {
  JsonDocument doc;
  doc["mode"]         = cfg.mode;
  doc["clockFace"]    = cfg.clockFace;
  doc["brightness"]   = cfg.brightness;
  doc["fontIndex"]    = cfg.fontIndex;
  doc["autoColor"]    = cfg.autoColor;
  doc["customR"]      = cfg.customR;
  doc["customG"]      = cfg.customG;
  doc["customB"]      = cfg.customB;
  doc["use24h"]       = cfg.use24h;
  doc["blinkColon"]   = cfg.blinkColon;
  doc["ntpServer"]    = cfg.ntpServer;
  doc["tzOffset"]     = cfg.tzOffset;
  doc["scrollText"]   = cfg.scrollText;
  doc["scrollSpeed"]  = cfg.scrollSpeed;
  doc["updateMin"]    = cfg.updateMin;
  doc["apiKey"]       = cfg.apiKey;
  doc["city"]         = cfg.city;
  doc["yaKey"]        = cfg.yaKey;
  doc["yaLat"]        = cfg.yaLat;
  doc["yaLon"]        = cfg.yaLon;
  doc["weatherProv"]  = cfg.weatherProv;
  doc["lavaAutoColor"]= cfg.lavaAutoColor;
  doc["lavaHue"]      = cfg.lavaHue;
  doc["ssid"]         = cfg.ssid;
  doc["rotation"]      = cfg.rotation;
  doc["flipH"]          = cfg.flipH;
  doc["flipV"]          = cfg.flipV;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handlePostSettings() {
  if (!server.hasArg("plain")) { server.send(400); return; }
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));

  auto setStr = [&](const char* key, char* dst, size_t sz) {
    if (doc[key].is<const char*>()) strlcpy(dst, doc[key].as<const char*>(), sz);
  };
  setStr("ntpServer",  cfg.ntpServer,  sizeof(cfg.ntpServer));
  setStr("scrollText", cfg.scrollText, sizeof(cfg.scrollText));
  setStr("apiKey",     cfg.apiKey,     sizeof(cfg.apiKey));
  setStr("city",       cfg.city,       sizeof(cfg.city));
  setStr("yaKey",      cfg.yaKey,      sizeof(cfg.yaKey));

  if (!doc["mode"].isNull())          cfg.mode          = doc["mode"];
  if (!doc["clockFace"].isNull())     cfg.clockFace     = doc["clockFace"];
  if (!doc["brightness"].isNull()) {  cfg.brightness    = doc["brightness"]; FastLED.setBrightness(cfg.brightness); }
  if (!doc["fontIndex"].isNull())     cfg.fontIndex     = doc["fontIndex"];
  if (!doc["autoColor"].isNull())     cfg.autoColor     = doc["autoColor"];
  if (!doc["customR"].isNull())       cfg.customR       = doc["customR"];
  if (!doc["customG"].isNull())       cfg.customG       = doc["customG"];
  if (!doc["customB"].isNull())       cfg.customB       = doc["customB"];
  if (!doc["use24h"].isNull())        cfg.use24h        = doc["use24h"];
  if (!doc["blinkColon"].isNull())    cfg.blinkColon    = doc["blinkColon"];
  if (!doc["tzOffset"].isNull())      cfg.tzOffset      = doc["tzOffset"];
  if (!doc["scrollSpeed"].isNull())   cfg.scrollSpeed   = doc["scrollSpeed"];
  if (!doc["updateMin"].isNull())     cfg.updateMin     = doc["updateMin"];
  if (!doc["yaLat"].isNull())         cfg.yaLat         = doc["yaLat"];
  if (!doc["yaLon"].isNull())         cfg.yaLon         = doc["yaLon"];
  if (!doc["weatherProv"].isNull())   cfg.weatherProv   = doc["weatherProv"];
  if (!doc["lavaAutoColor"].isNull()) cfg.lavaAutoColor = doc["lavaAutoColor"];
  if (!doc["lavaHue"].isNull())       cfg.lavaHue       = doc["lavaHue"];
  if (!doc["rotation"].isNull())      cfg.rotation      = (uint8_t)constrain((int)doc["rotation"], 0, 3);
  if (!doc["flipH"].isNull())         cfg.flipH         = doc["flipH"];
  if (!doc["flipV"].isNull())         cfg.flipV         = doc["flipV"];

  configTime(cfg.tzOffset * 3600L, 0, cfg.ntpServer);
  saveConfig();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleStatus() {
  JsonDocument doc;
  doc["wifi"]   = wifiConnected;
  doc["ap"]     = apMode;
  doc["ssid"]   = cfg.ssid;
  doc["ip"]     = wifiConnected ? WiFi.localIP().toString() : "192.168.4.1";
  doc["apIP"]   = "192.168.4.1";
  doc["temp"]   = getTemp();
  doc["uptime"] = String(millis() / 1000 / 3600, DEC) + "ч " +
                  String((millis() / 1000 % 3600) / 60, DEC) + "м";
  doc["heap"]   = String(ESP.getFreeHeap() / 1024) + " KB";
  doc["rssi"]   = wifiConnected ? String(WiFi.RSSI()) + " dBm" : "—";

  // Отправляем в логическом XY-порядке (с учётом змейки)
  JsonArray arr = doc["matrix"].to<JsonArray>();
  for (int y = 0; y < MH; y++)
    for (int x = 0; x < MW; x++) {
      int idx = XY(x, y);
      arr.add(idx >= 0 ? (((uint32_t)leds[idx].r << 16) | ((uint32_t)leds[idx].g << 8) | leds[idx].b) : 0u);
    }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// Сканирование WiFi: асинхронный запуск → быстрый ответ, результаты через polling
void handleScanWifi() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED || n == WIFI_SCAN_RUNNING) {
    // Запускаем асинхронное сканирование и ждём до 4 сек, отдавая сервер
    WiFi.scanDelete();
    WiFi.scanNetworks(true);  // async=true — не блокирует
    uint32_t t0 = millis();
    while (WiFi.scanComplete() < 0 && millis() - t0 < 4000) {
      dnsServer.processNextRequest();
      server.handleClient();
      delay(100);
    }
    n = WiFi.scanComplete();
  }
  JsonDocument doc;
  JsonArray arr = doc["networks"].to<JsonArray>();
  if (n > 0) {
    for (int i = 0; i < n && i < 20; i++) {
      if (WiFi.SSID(i).length() == 0) continue;
      JsonObject net = arr.add<JsonObject>();
      net["ssid"] = WiFi.SSID(i);
      net["rssi"] = WiFi.RSSI(i);
      net["enc"]  = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      net["ch"]   = WiFi.channel(i);
    }
  }
  WiFi.scanDelete();
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// Подключение к сети: не блокируем loop(), опрашиваем сервер во время ожидания
void handleConnectWifi() {
  if (!server.hasArg("plain")) { server.send(400); return; }
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  const char* ssid = doc["ssid"] | "";
  const char* pass = doc["password"] | "";
  if (strlen(ssid) == 0) {
    server.send(400, "application/json", "{\"error\":\"empty ssid\"}");
    return;
  }

  // Берём мьютекс до любого обращения к WiFi-стеку.
  // Ждём до 10 сек: weatherTask удерживает его максимум ~8 сек (http.setTimeout).
  // Если не дождались — сообщаем клиенту об ошибке вместо краша.
  if (xSemaphoreTake(wifiMutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
    server.send(503, "application/json", "{\"error\":\"WiFi busy, retry\"}");
    Serial.println("[WiFi] Не удалось взять wifiMutex, подключение отменено");
    return;
  }

  // Переходим в AP+STA — AP остаётся, браузер не теряет соединение
  Serial.printf("[WiFi] Подключаемся к '%s'...\n", ssid);
  WiFi.mode(WIFI_OFF);
  delay(300);
  WiFi.mode(WIFI_AP_STA);
  delay(300);
  IPAddress apIP(192,168,4,1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));
  WiFi.softAP(AP_NAME, AP_PASS, 6, 0, 4);
  delay(500);
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.begin(ssid, pass);

  // Ждём до 15 сек, периодически отдавая управление серверу.
  // wifiMutex удерживаем всё время — weatherTask не сможет начать HTTP-запрос,
  // пока мы не завершим реконфигурацию и не отпустим мьютекс.
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(50);
  }

  JsonDocument resp;
  if (WiFi.status() == WL_CONNECTED) {
    strlcpy(cfg.ssid,     ssid, sizeof(cfg.ssid));
    strlcpy(cfg.wifiPass, pass, sizeof(cfg.wifiPass));
    saveConfig();
    wifiConnected = true;
    apMode = true;
    resp["ok"]   = true;
    resp["ip"]   = WiFi.localIP().toString();
    resp["rssi"] = WiFi.RSSI();
    configTime(cfg.tzOffset * 3600L, 0, cfg.ntpServer);
    Serial.printf("[WiFi] Подключено! IP=%s\n", WiFi.localIP().toString().c_str());
  } else {
    int st = WiFi.status();
    resp["ok"]     = false;
    resp["status"] = st;
    Serial.printf("[WiFi] Ошибка подключения, статус=%d\n", st);
    WiFi.mode(WIFI_OFF);
    delay(100);
    startAP();
    dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
  }

  xSemaphoreGive(wifiMutex);

  // weatherRequested ставим ПОСЛЕ release мьютекса — иначе weatherTask
  // немедленно войдёт в fetchWeather() раньше, чем мы успели send().
  if (resp["ok"].as<bool>()) weatherRequested = true;

  String out; serializeJson(resp, out);
  server.send(200, "application/json", out);
}


void handleDisconnectWifi() {
  cfg.ssid[0] = '\0';
  cfg.wifiPass[0] = '\0';
  saveConfig();
  wifiConnected = false;
  server.send(200, "application/json", "{\"ok\":true}");
  delay(200);
  ESP.restart();
}

// Сохранить WiFi и перезагрузить (из ручного ввода в форме)
void handlePostWifi() {
  if (!server.hasArg("plain")) { server.send(400); return; }
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  const char* ssid = doc["ssid"] | "";
  const char* pass = doc["password"] | "";
  if (strlen(ssid) == 0) {
    server.send(400, "application/json", "{\"error\":\"empty ssid\"}");
    return;
  }
  strlcpy(cfg.ssid,     ssid, sizeof(cfg.ssid));
  strlcpy(cfg.wifiPass, pass, sizeof(cfg.wifiPass));
  saveConfig();
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"saved, rebooting\"}");
  delay(500);
  ESP.restart();
}

void handleReboot() {
  server.send(200, "application/json", "{\"ok\":true}");
  delay(200);
  ESP.restart();
}

void handleForceUpdate() {
  weatherRequested = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

// ════════════════════════════════════════════════════════════
//  BOOT-АНИМАЦИЯ И СТАТУС
// ════════════════════════════════════════════════════════════
void showBoot() {
  for (int y = MH - 1; y >= 0; y--) {
    for (int x = 0; x < MW; x++)
      px(x, y, CHSV((x + (MH-1-y)) * 10, 255, 180));
    FastLED.show(); delay(28);
  }
  delay(250); clearMatrix(); FastLED.show();
}

void showConnecting() {
  static uint8_t pos = 0;
  clearMatrix();
  for (int i = 0; i < 5; i++)
    leds[(pos + i * 5) % NUM_LEDS] = CHSV(160, 255, 180);
  pos = (pos + 1) % NUM_LEDS;
  FastLED.show();
}

// Старт AP — вызывать ТОЛЬКО когда WiFi уже в WIFI_OFF или WIFI_AP
static void startAP() {
  WiFi.mode(WIFI_AP);
  delay(300);
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  // Канал 6, скрытый=false, макс.клиентов=4
  WiFi.softAP(AP_NAME, AP_PASS, 6, 0, 4);
  delay(500);
  // КРИТИЧНО для ESP32-C3: отключить power save, иначе клиенты не могут подключиться
  esp_wifi_set_ps(WIFI_PS_NONE);
  apMode = true;
  Serial.printf("[AP] Запущена: %s / %s → %s  (ch6, ps=none)\n",
    AP_NAME, AP_PASS, WiFi.softAPIP().toString().c_str());
}

// ════════════════════════════════════════════════════════════
//  ФОНОВАЯ ЗАГРУЗКА ПОГОДЫ (FreeRTOS, core 0)
//  WebServer НЕ потокобезопасен — он остаётся в loop() на core 1
//  fetchWeather() блокирует до 8 сек — выносим его в отдельный task
// ════════════════════════════════════════════════════════════
void weatherTask(void* param) {
  for (;;) {
    if (weatherRequested) {
      weatherRequested = false;
      if (wifiConnected) {
        Serial.println("[Weather] Запрос погоды...");
        fetchWeather();
        Serial.printf("[Weather] %.1f C  стек: %u bytes free\n",
          getTemp(), uxTaskGetStackHighWaterMark(NULL));
      }
    }
    vTaskDelay(100);
  }
}

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
  // Мьютекс создаём самым первым — до любого WiFi-кода и до старта задач.
  // Это гарантирует что wifiMutex != NULL в момент первого xSemaphoreTake(),
  // независимо от будущей перестановки кода в setup().
  wifiMutex = xSemaphoreCreateMutex();
  configASSERT(wifiMutex);

  Serial.begin(115200);
  Serial.println("\n╔════════════════════════╗");
  Serial.println("║  ESP32 LED Matrix v3.0 ║");
  Serial.println("╚════════════════════════╝");

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 2500);
  clearMatrix(); FastLED.show();

  randomSeed(esp_random());

  loadConfig();
  FastLED.setBrightness(cfg.brightness);
  showBoot();

  // ── WiFi init ──
  // persistent(false) — не писать credentials в NVS (используем Preferences)
  WiFi.persistent(false);
  // Одиночный сброс в WIFI_OFF — больше нигде не повторяем до первого begin()
  WiFi.mode(WIFI_OFF);
  delay(500);  // ESP32-C3 нужно чуть больше времени на полный сброс радио

  if (strlen(cfg.ssid) > 0) {
    Serial.printf("══════════════════════════════════\n");
    Serial.printf("Сохранённая сеть: '%s'\n", cfg.ssid);
    Serial.printf("══════════════════════════════════\n");

    const int MAX_TRIES = 2;
    const uint32_t TIMEOUT_MS = 12000;
    bool connected = false;

    for (int attempt = 1; attempt <= MAX_TRIES && !connected; attempt++) {
      Serial.printf("[WiFi] Попытка %d/%d → SSID='%s'\n", attempt, MAX_TRIES, cfg.ssid);
      WiFi.mode(WIFI_STA);
      delay(100);
      esp_wifi_set_ps(WIFI_PS_NONE);  // power save off для стабильного STA
      WiFi.begin(cfg.ssid, cfg.wifiPass);

      uint32_t t0 = millis();
      uint32_t printT = 0;
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < TIMEOUT_MS) {
        showConnecting(); delay(80);
        if (millis() - printT > 2000) {
          Serial.printf("  ... статус: %d (%lu сек)\n",
            WiFi.status(), (millis()-t0)/1000);
          printT = millis();
        }
      }

      int st = WiFi.status();
      if (st == WL_CONNECTED) {
        connected = true;
        wifiConnected = true;
        apMode = false;
        Serial.printf("[WiFi] ✓ IP=%s  RSSI=%d dBm\n",
          WiFi.localIP().toString().c_str(), WiFi.RSSI());
        configTime(cfg.tzOffset * 3600L, 0, cfg.ntpServer);
        weatherRequested = true;  // погода через task, не блокируя
      } else {
        const char* reason =
          (st==6)?"НЕВЕРНЫЙ ПАРОЛЬ":
          (st==1)?"СЕТЬ НЕ НАЙДЕНА":
          (st==4)?"СОЕДИНЕНИЕ СБРОШЕНО":"НЕИЗВЕСТНО";
        Serial.printf("[WiFi] ✗ Попытка %d: статус=%d (%s)\n", attempt, st, reason);
        WiFi.disconnect(false);
        delay(100);
        WiFi.mode(WIFI_OFF);
        delay(500);
        if (attempt < MAX_TRIES) {
          Serial.println("[WiFi] Пауза 2 сек перед следующей попыткой...");
          delay(2000);
        }
      }
    }

    if (!connected) {
      Serial.printf("[WiFi] Все попытки исчерпаны → AP\n");
      startAP();
    }
  } else {
    Serial.println("[WiFi] Сеть не задана → AP");
    startAP();
  }

  // DNS captive portal — только если AP запущен
  if (apMode) {
    dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
    Serial.println("[DNS] Captive portal запущен");
  }

  // Captive portal редиректы для разных ОС
  server.on("/generate_204",        HTTP_GET, [](){ server.sendHeader("Location","/"); server.send(302); });
  server.on("/fwlink",              HTTP_GET, [](){ server.sendHeader("Location","/"); server.send(302); });
  server.on("/hotspot-detect.html", HTTP_GET, [](){ server.sendHeader("Location","/"); server.send(302); });
  server.on("/connecttest.txt",     HTTP_GET, [](){ server.sendHeader("Location","/"); server.send(302); });

  server.on("/",              HTTP_GET,  handleRoot);
  server.on("/api/settings",  HTTP_GET,  handleGetSettings);
  server.on("/api/settings",  HTTP_POST, handlePostSettings);
  server.on("/api/status",    HTTP_GET,  handleStatus);
  server.on("/api/scan",      HTTP_GET,  handleScanWifi);
  server.on("/api/connect",   HTTP_POST, handleConnectWifi);
  server.on("/api/disconnect",HTTP_POST, handleDisconnectWifi);
  server.on("/api/wifi",      HTTP_POST, handlePostWifi);
  server.on("/api/reboot",    HTTP_POST, handleReboot);
  server.on("/api/update",    HTTP_POST, handleForceUpdate);
  server.onNotFound([](){ server.sendHeader("Location","/"); server.send(302); });
  server.begin();
  Serial.println("Server started");

  // Запускаем задачу погоды на core 0, анимации остаются на core 1 (loop)
  xTaskCreatePinnedToCore(
    weatherTask,   // погода на core 0 — HTTPClient блокирует до 8 сек
    "weather",
    8192,          // стек для HTTPClient + ArduinoJson
    NULL,
    1,             // низкий приоритет
    NULL,
    0              // core 0
  );
  Serial.println("Weather task on core 0 (server stays on core 1)");

  scrollX = MW;
  memset(fA, 0, sizeof(fA)); memset(fB, 0, sizeof(fB));
  fireInit = false;
  mtxInit2 = false;
  metInit  = false;
  lavaInit2 = false;
  snReset();
}

// ════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════
void loop() {
  // Веб-сервер в loop() на core 1 (WebServer не потокобезопасен)
  dnsServer.processNextRequest();
  server.handleClient();

  // Выключить AP через 60 сек после успешного подключения к WiFi
  static uint32_t apOffMs = 0;
  if (wifiConnected && apMode) {
    if (apOffMs == 0) apOffMs = millis();
    if (millis() - apOffMs > 60000) {
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      apMode = false;
      dnsServer.stop();
      Serial.println("[AP] Выключен (WiFi подключён)");
    }
  }
  // Проверяем что STA соединение не упало
  {
    static uint32_t wifiLostMs = 0;
    if (wifiConnected && !apMode && WiFi.status() != WL_CONNECTED) {
      if (wifiLostMs == 0) {
        wifiLostMs = millis();
        Serial.println("[WiFi] Соединение потеряно, запускаем WiFi.reconnect()...");
        WiFi.reconnect();
      } else if (millis() - wifiLostMs > 30000) {
        // 30 сек переподключения не помогли — единственный надёжный выход
        Serial.println("[WiFi] Переподключение не удалось (30 сек), перезагрузка...");
        delay(500);
        ESP.restart();
      }
    } else {
      wifiLostMs = 0; // соединение есть — сбрасываем счётчик
    }
  }

  // Обновление погоды — через флаг weatherTask (не блокирует loop)
  if (wifiConnected) {
    uint32_t interval = (uint32_t)cfg.updateMin * 60000UL;
    uint32_t lw = getLastWeatherMs(); // атомарный снимок
    if (lw == 0 || millis() - lw >= interval) {
      setLastWeatherMs(millis()); // обновляем сразу, чтобы не спамить
      weatherRequested = true;
    }
  }

  // FPS по режиму
  static const uint32_t fps[] = {1000, 100, 80, 84, 40, 160, 70, 120, 360, 80};
  uint32_t fi2 = (cfg.mode < 10) ? fps[cfg.mode] : 100;

  if (millis() - lastDrawMs < fi2) return;
  lastDrawMs = millis();

  switch (cfg.mode) {
    case 0: showTemperature(); break;
    case 1: showClock();       break;
    case 2: showRainbow();     break;
    case 3: showFire();        break;
    case 4: showScroll();      break;
    case 5: showMatrix();      break;
    case 6: showPlasma();      break;
    case 7: showMeteors();     break;
    case 8: showSnake();       break;
    case 9: showLava();        break;
  }
}
