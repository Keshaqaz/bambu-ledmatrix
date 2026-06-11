/*
 ╔══════════════════════════════════════════════════════════════════╗
 ║  ESP32 · 16×16 WS2812B LED Controller · v2.0                   ║
 ║                                                                  ║
 ║  Режимы:                                                         ║
 ║    2 – Огонь (32×32)  4 – Плазма                               ║
 ║    5 – Лава-лампа    6 – Часы + Темп                           ║
 ║                                                                  ║
 ║  Подключение:                                                    ║
 ║    WS2812B DATA → GPIO 4  (прямое подключение)                  ║
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
#include <ctype.h>
#include <stddef.h>
#include <esp_arduino_version.h>
#include <esp_wifi.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <atomic>
#include "app.hpp"

// PlatformIO compiles sources as plain C++ and does not apply Arduino's .ino
// preprocessing. Keep explicit prototypes here so the same code builds in
// both Arduino CLI/IDE and PlatformIO.
float safeGetTemp();
static void fatalHalt(const char* msg);
int XY(int x, int y);
void px(int x, int y, CRGB c);
void clearMatrix();
void matrixShow();
static int microGlyphIndex(char c);
void drawChar3(int x, int y, char c, CRGB col);
int strW3(const char* s);
void drawStr3(int x, int y, const char* s, CRGB col);
CRGB tempToColor(float t);
void showClockTemp();
void fetchWeather();
void fetchSunriseSunset();
void bambuTask(void* param);
bool saveConfig();
void loadConfig();
bool checkAuth();
void handleRoot();
void handleGetSettings();
void handlePostSettings();
void handleStatus();
void handleScanWifi();
void handleConnectWifi();
void handleDisconnectWifi();
void handleReboot();
void handleWeatherRefresh();
void handleUpdateUpload();
void handleUpdateFinal();
void showBoot();
void showConnecting();
static void startAP();
void wifiConnTask(void* param);
void weatherTask(void* param);
void setup();
void loop();

// ════════════════════════════════════════════════════════════
//  ЖЕЛЕЗО
// ════════════════════════════════════════════════════════════
#define LED_PIN      4
#define NUM_LEDS     256
#define MW           16
#define MH           16

static constexpr uint16_t CONFIG_VERSION = 332;
// Версия прошивки: поднимать при каждой OTA-итерации — по смене значения в
// /api/status видно, что обновление реально применилось.
static constexpr const char* FW_VERSION = "4.0.0-2026.06.11";
static constexpr uint32_t STORED_STATE_MAGIC = 0x4D415458UL; // MATX
static constexpr uint16_t STORED_STATE_VERSION = 1;
static constexpr size_t BAMBU_TOKEN_CAPACITY = 384;
static constexpr const char* LEGACY_DEFAULT_AP_PASS = "12345678";
static constexpr uint8_t MIN_UI_BRIGHTNESS = 1;

CRGB leds[NUM_LEDS];

// ════════════════════════════════════════════════════════════
//  ШРИФТ 3×5: цифры/знаки (индекс 0-12)
//  0-9='0'-'9'  10='-'  11='+'  12=space
// ════════════════════════════════════════════════════════════
static const uint8_t F_MICRO[13][5] PROGMEM = {
  {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},
  {5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},
  {7,5,7,5,7},{7,5,7,1,7},
  {0,0,7,0,0},{0,2,7,2,0},{0,0,0,0,0}
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

  float    tzOffset        = 3.0f; // UTC-смещение в часах; дробные значения (5.5=UTC+5:30) поддерживаются
  uint8_t  brightness      = 80;
  bool     autoBrightness  = false; // режим день/ночь
  uint8_t  brightnessDay   = 80;
  uint8_t  brightnessNight = 20;
  uint8_t  dayStartHour    = 7;     // час начала дня (0-23)
  uint8_t  nightStartHour  = 22;    // час начала ночи (0-23)
  bool     autoBrSunMode   = false; // закат/рассвет по координатам
  bool     showDegree      = false; // legacy: отдельный режим температуры удалён
  uint8_t  fontIndex       = 0;     // legacy: выбор шрифтов удалён, всегда Micro 3×5
  uint8_t  mode            = 6;
  uint8_t  clockFace       = 0;     // legacy: отдельный режим часов удалён
  uint16_t updateMin       = 10;
  bool     use24h          = true;
  bool     showColon       = true;
  bool     blinkColon      = false; // legacy: мигание разделителя удалено, разделитель постоянный
  uint8_t  colonBright     = 90;  // яркость разделителя : (0–255, относительно цвета часов)
  bool     autoColor       = true;
  uint8_t  customR         = 255;
  uint8_t  customG         = 128;
  uint8_t  customB         = 0;

  float    owmLat          = 0.0f;
  float    owmLon          = 0.0f;
  bool     owmUseCoords    = false; // true=запрос OWM по координатам, false=по названию города
  bool     lavaAutoColor   = true;
  uint16_t lavaHue         = 15;
  uint8_t  rotation        = 0;   // 0=нормальный 1=90°CW 2=180° 3=270°CW
  bool     flipH           = false; // зеркало по горизонтали
  bool     flipV           = false; // зеркало по вертикали
  char     webPass[32]     = "";        // пароль веб-интерфейса не используется
  char     apPass[64]      = "12345678"; // фиксированный пароль точки доступа ESP32-Matrix

  // Режим 6 «Часы + Темп»: позиция строк и независимые цвета.
  // Автоцвет влияет только на температуру; часы всегда берут ct6ClockRGB.
  int8_t  ct6ClockX  = 0;    // X-координата строки часов (0–1)
  int8_t  ct6ClockY  = 2;    // Y-координата строки часов (0–11)
  int8_t  ct6TempX   = -1;   // X-координата температуры (-1 = авто-центр, 0–15 = ручная)
  int8_t  ct6TempY   = 9;    // Y-координата строки температуры (0–11)
  bool    ct6ShowWeatherIcon = false; // legacy: настройка удалена, weather-иконки на матрице не рисуются
  uint8_t ct6ClockR  = 255; uint8_t ct6ClockG = 128; uint8_t ct6ClockB = 0;
  uint8_t ct6TempR   = 0;   uint8_t ct6TempG  = 200; uint8_t ct6TempB  = 255;
  uint8_t weatherProvider = 0;   // 0=авто (OWM → Open-Meteo), 1=только OWM, 2=только Open-Meteo
  bool    tzFromWeather   = true; // UTC-смещение из ответа погоды (OWM timezone / OM utc_offset_seconds)
  // Новые поля добавлять только в конец struct Config.
  // loadConfig() умеет частичную миграцию по csz, поэтому вставка в середину
  // ломает раскладку байтов старых NVS-конфигов.
};

struct LegacyConfigV326 {
  char     ssid[40];
  char     wifiPass[64];
  char     city[64];
  char     apiKey[48];
  char     ntpServer[64];
  float    tzOffset;
  uint8_t  brightness;
  bool     autoBrightness;
  uint8_t  brightnessDay;
  uint8_t  brightnessNight;
  uint8_t  dayStartHour;
  uint8_t  nightStartHour;
  bool     autoBrSunMode;
  bool     showDegree;
  uint8_t  fontIndex;
  uint8_t  mode;
  uint8_t  clockFace;
  uint16_t updateMin;
  bool     use24h;
  bool     showColon;
  bool     blinkColon;
  uint8_t  colonBright;
  bool     autoColor;
  uint8_t  customR;
  uint8_t  customG;
  uint8_t  customB;
  float    owmLat;
  float    owmLon;
  bool     owmUseCoords;
  bool     lavaAutoColor;
  uint16_t lavaHue;
  uint8_t  rotation;
  bool     flipH;
  bool     flipV;
  char     webPass[32];
  char     apPass[64];
  int8_t   ct6ClockX;
  int8_t   ct6ClockY;
  int8_t   ct6TempX;
  int8_t   ct6TempY;
  uint8_t  ct6ClockR;
  uint8_t  ct6ClockG;
  uint8_t  ct6ClockB;
  uint8_t  ct6TempR;
  uint8_t  ct6TempG;
  uint8_t  ct6TempB;
};

struct LegacyConfigV331Compat {
  char     ssid[40];
  char     wifiPass[64];
  char     city[64];
  char     apiKey[48];
  char     ntpServer[64];
  float    tzOffset;
  uint8_t  brightness;
  bool     autoBrightness;
  uint8_t  brightnessDay;
  uint8_t  brightnessNight;
  uint8_t  dayStartHour;
  uint8_t  nightStartHour;
  bool     autoBrSunMode;
  bool     showDegree;
  uint8_t  fontIndex;
  uint8_t  mode;
  uint8_t  clockFace;
  uint16_t updateMin;
  bool     use24h;
  bool     showColon;
  bool     blinkColon;
  uint8_t  colonBright;
  bool     autoColor;
  uint8_t  customR;
  uint8_t  customG;
  uint8_t  customB;
  float    owmLat;
  float    owmLon;
  bool     owmUseCoords;
  bool     lavaAutoColor;
  uint16_t lavaHue;
  uint8_t  rotation;
  bool     flipH;
  bool     flipV;
  char     webPass[32];
  char     apPass[64];
  char     reserved0[160];
  int8_t   ct6ClockX;
  int8_t   ct6ClockY;
  int8_t   ct6TempX;
  int8_t   ct6TempY;
  bool     ct6ShowWeatherIcon;
  uint8_t  ct6ClockR;
  uint8_t  ct6ClockG;
  uint8_t  ct6ClockB;
  uint8_t  ct6TempR;
  uint8_t  ct6TempG;
  uint8_t  ct6TempB;
};

static_assert(sizeof(LegacyConfigV326) == 432, "LegacyConfigV326 size changed");
static_assert(sizeof(LegacyConfigV331Compat) > sizeof(Config),
              "LegacyConfigV331Compat must remain larger than current Config");

struct BambuConfig {
  bool enabled = false;
  // Legacy ESP-side Bambu Cloud fields kept only so old NVS blobs keep their layout.
  // Runtime is relay-only; these fields are cleared on load/save and ignored.
  char email[96] = "";
  char password[96] = "";
  bool relayEnabled = true;
  char relayUrl[160] = "";
  char relayKey[64] = "";
};

struct StoredStateBlob {
  uint32_t magic = STORED_STATE_MAGIC;
  uint16_t version = STORED_STATE_VERSION;
  uint16_t size = sizeof(StoredStateBlob);
  Config cfg;
  BambuConfig bambuCfg;
  char bambuToken[BAMBU_TOKEN_CAPACITY] = "";
  uint32_t checksum = 0;
};

Config     cfg;
BambuConfig bambuCfg;
Preferences prefs;
// Legacy direct-cloud token/serial placeholders kept only to migrate/clear old NVS keys.
static char bambuManualToken[BAMBU_TOKEN_CAPACITY] = "";
static char bambuPrinterSerial[32] = "";

// ════════════════════════════════════════════════════════════
//  РАНТАЙМ
// ════════════════════════════════════════════════════════════
WebServer server(80);
DNSServer dnsServer;

static const char* AP_NAME = "ESP32-Matrix"; // SSID точки доступа (не меняется)

float    currentTemp    = -999.0f;
std::atomic<bool> wifiConnected{false};
std::atomic<bool>     apMode{true};   // стартуем всегда в AP
std::atomic<bool> weatherRequested{false}; // флаг запроса погоды для weatherTask
// configTime() модифицирует глобальное состояние lwIP — вызываем только из loop task.
// wifiConnTask выставляет этот флаг, loop task вызывает configTime.
std::atomic<bool> ntpPending{false};
// Атомарный флаг "SSID сохранён" — избегает чтения cfg.ssid (char[]) из matrixShow без мьютекса
std::atomic<bool> hasSsid{false};
// Флаг отложенной перезагрузки: обработчик отвечает браузеру, loop() перезагружает
// после 500 мс — TCP-стек успевает сбросить ответ, анимация не блокируется.
static std::atomic<bool>     rebootPending{false};
static std::atomic<uint32_t> rebootMs{0};

// OTA: пока идёт приём образа, weatherTask/bambuTask приостанавливаются, чтобы
// TLS-handshake (~12 KB heap) не стартовал посреди записи флеша.
static std::atomic<bool> otaInProgress{false};

// Boot-guard / анти-кирпич: счётчик незавершённых загрузок в отдельном NVS
// namespace "bootguard" (StoredStateBlob в "matrix" не трогаем). 3 сбоя подряд →
// откат на другой OTA-слот. 3 ручных ресета в первые 60 с дают тот же эффект —
// это осознанный компромисс.
static constexpr uint8_t  BOOT_GUARD_MAX_FAILS = 3;
static constexpr uint32_t BOOT_GUARD_STABLE_MS = 60000;
static bool bootGuardCleared = false;

static void bootGuardReset() {
  Preferences bg;
  if (bg.begin("bootguard", false)) { bg.putUChar("cnt", 0); bg.end(); }
}

uint32_t lastWeatherMs  = 0;
uint32_t lastWeatherAttemptMs = 0;
int      lastWeatherHttpCode = 0;
uint8_t  lastWeatherKeyLen = 0;
char     lastWeatherError[96] = "";
char     lastWeatherSource[16] = "";
uint32_t lastDrawMs     = 0;
static std::atomic<uint32_t> displayInvalidationSeq{0};

static constexpr time_t MIN_VALID_EPOCH = 1609459200; // 2021-01-01: время уже синхронизировано
static portMUX_TYPE clockCacheMux = portMUX_INITIALIZER_UNLOCKED;
static bool   clockCacheValid = false;
static time_t clockCacheEpoch = 0;
static uint32_t clockCacheMs = 0;

// Спинлок для защиты currentTemp, lastWeatherMs и lastWeatherAttemptMs при доступе из разных FreeRTOS tasks
// (weatherTask и loop task на ESP32-C3 time-slice на одном ядре) — операция очень короткая, spinlock допустим
static portMUX_TYPE weatherMux = portMUX_INITIALIZER_UNLOCKED;
// wifiConnMux защищает только два коротких strlcpy — spinlock допустим
// (объявлен ниже вместе с остальными WiFi-переменными)

// Мьютексы FreeRTOS для операций, которые могут занимать много времени:
// taskENTER_CRITICAL нельзя использовать вокруг NVS и длинных блоков —
// он надолго отключает прерывания и может вызвать Interrupt WDT reset.
static SemaphoreHandle_t cfgMtx;   // защищает cfg: пишет handlePostSettings/wifiConnTask, читает fetchWeather
static SemaphoreHandle_t prefsMtx; // защищает NVS/Preferences: saveConfig/loadConfig
static SemaphoreHandle_t bambuCfgMtx; // защищает bambuCfg: пишет веб-интерфейс, читает bambuTask
static TaskHandle_t bambuTaskHandle = nullptr;

// Безопасное чтение температуры — читает loop task, пишет weatherTask
inline float safeGetTemp() {
  taskENTER_CRITICAL(&weatherMux);
  float t = currentTemp;
  taskEXIT_CRITICAL(&weatherMux);
  return t;
}

static void setWeatherDiag(int code, const char* error, uint8_t keyLen, const char* source = nullptr) {
  taskENTER_CRITICAL(&weatherMux);
  lastWeatherHttpCode = code;
  lastWeatherKeyLen = keyLen;
  strlcpy(lastWeatherError, error ? error : "", sizeof(lastWeatherError));
  if (source) strlcpy(lastWeatherSource, source, sizeof(lastWeatherSource));
  taskEXIT_CRITICAL(&weatherMux);
}

enum BambuLifecycle : uint8_t {
  BAMBU_UNKNOWN   = 0,
  BAMBU_IDLE      = 1,
  BAMBU_PREPARING = 2,
  BAMBU_PRINTING  = 3,
  BAMBU_PAUSED    = 4,
  BAMBU_FINISHED  = 5,
  BAMBU_ERROR     = 6,
  BAMBU_OFFLINE   = 7,
};

struct BambuState {
  bool enabled = false;
  bool configured = false;
  bool connected = false;
  bool printerOnline = false;
  uint8_t lifecycle = BAMBU_UNKNOWN;
  uint8_t displayLifecycle = BAMBU_UNKNOWN;
  uint8_t progress = 0;
  char detail[96] = "Bambu выключен";
  char rawStatus[40] = "";
  char rawStage[48] = "";
};

static BambuState bambuState;
static portMUX_TYPE bambuStateMux = portMUX_INITIALIZER_UNLOCKED;

static BambuState getBambuStateSnapshot() {
  taskENTER_CRITICAL(&bambuStateMux);
  BambuState snap = bambuState;
  taskEXIT_CRITICAL(&bambuStateMux);
  return snap;
}

static void setBambuStateSnapshot(const BambuState& next) {
  taskENTER_CRITICAL(&bambuStateMux);
  bambuState = next;
  taskEXIT_CRITICAL(&bambuStateMux);
}

static uint8_t bambuOverlayLifecycle(uint8_t nextLifecycle) {
  switch (nextLifecycle) {
    case BAMBU_PREPARING:
      return BAMBU_PREPARING;
    case BAMBU_PRINTING:
      return BAMBU_PRINTING;
    case BAMBU_PAUSED:
    case BAMBU_ERROR:
    case BAMBU_FINISHED:
      return nextLifecycle;
    case BAMBU_IDLE:
    case BAMBU_OFFLINE:
    case BAMBU_UNKNOWN:
    default:
      return BAMBU_UNKNOWN;
  }
}

static void setBambuDetail(BambuState& st, const char* detail) {
  strlcpy(st.detail, detail ? detail : "", sizeof(st.detail));
}

static bool bambuHasRelayConfig(bool* enabledOut = nullptr,
                                char* relayUrl = nullptr, size_t relayUrlSz = 0,
                                char* relayKey = nullptr, size_t relayKeySz = 0) {
  bool enabled = false;
  char localRelayUrl[sizeof(bambuCfg.relayUrl)] = "";
  xSemaphoreTake(bambuCfgMtx, portMAX_DELAY);
  enabled = bambuCfg.enabled;
  // Relay-only runtime: keep the old flag forced on for migrated configs.
  bambuCfg.relayEnabled = true;
  // Drop obsolete ESP-side Cloud credentials from RAM so they are not re-saved.
  bambuCfg.email[0] = '\0';
  bambuCfg.password[0] = '\0';
  bambuManualToken[0] = '\0';
  bambuPrinterSerial[0] = '\0';
  strlcpy(localRelayUrl, bambuCfg.relayUrl, sizeof(localRelayUrl));
  if (relayUrl && relayUrlSz) strlcpy(relayUrl, bambuCfg.relayUrl, relayUrlSz);
  if (relayKey && relayKeySz) strlcpy(relayKey, bambuCfg.relayKey, relayKeySz);
  xSemaphoreGive(bambuCfgMtx);
  if (enabledOut) *enabledOut = enabled;
  return enabled && localRelayUrl[0] != '\0';
}

static void wakeBambuTask() {
  if (bambuTaskHandle) xTaskNotifyGive(bambuTaskHandle);
}

static void bambuSleepOrWake(uint32_t delayMs) {
  if (delayMs == 0) return;
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delayMs));
}

// ── Асинхронное подключение к WiFi ──────────────────────────────────────────
// handleConnectWifi() только сохраняет параметры и возвращает ответ браузеру.
// wifiConnTask выполняет фактическое подключение в отдельной FreeRTOS task.
// Результат читается браузером через /api/status polling.
static char              wifiPendSsid[40]  = "";
static char              wifiPendPass[64]  = "";
static std::atomic<bool> wifiConnPending{false};
static std::atomic<int>  wifiConnResult{0}; // 0=idle 1=connecting 2=ok 3=fail
static std::atomic<int>  wifiConnFailStatus{0};
static portMUX_TYPE      wifiConnMux = portMUX_INITIALIZER_UNLOCKED;
// Флаг: wifiConnTask просит loop task перезапустить AP (AP-функции не потокобезопасны)
static std::atomic<bool> wifiNeedApRestart{false};
// Loop-task: WiFi API не потокобезопасен — все вызовы mode/begin делаем из loop task
// wifiConnTask выставляет wifiDoConnect, loop task вызывает WiFi.begin
// и выставляет wifiConnInProgress, после чего wifiConnTask начинает опрос статуса.
static std::atomic<bool>    wifiDoConnect{false};
static std::atomic<bool>    wifiConnInProgress{false};
// Результат мониторинга подключения записывает loop task, читает wifiConnTask.
// Исключает прямые вызовы WiFi.status() из wifiConnTask (WiFi API не потокобезопасен).
static std::atomic<bool>    wifiConnDone{false};    // опрос завершён
static std::atomic<bool>    wifiConnOk{false};      // true = WL_CONNECTED
static std::atomic<uint8_t> wifiConnFinalSt{0};     // итоговый статус (wl_status_t)
// NVS-запись только из loop task: wifiConnTask устанавливает флаг, loop() вызывает saveConfig().
static std::atomic<bool>    savePending{false};
static std::atomic<uint32_t> saveDueMs{0};
static std::atomic<bool>    saveError{false};

// Таймер выключения AP — вынесен в глобал, чтобы wifiConnTask мог сбросить его
static std::atomic<uint32_t> apOffMs{0};

// Кэш восхода/заката — обновляется weatherTask раз в сутки, хранится в local time.
// Упакованы в один atomic<uint32_t>: биты 31-24=riseH, 23-16=riseM, 15-8=setH, 7-0=setM.
// Атомарная запись/чтение исключает torn read между background task и loop task.
static std::atomic<uint32_t> sunTimes{(6u<<24)|(0u<<16)|(20u<<8)|0u}; // дефолт 06:00 / 20:00
static std::atomic<bool>     sunTimeValid{false};
static std::atomic<uint32_t> lastSunFetchMs{0};
static std::atomic<bool>     sunFetchRequested{false};
static constexpr const char* PREF_LAST_AUTO_BRIGHTNESS = "lastAutoBr";
static std::atomic<uint8_t>  lastAutoBrightness{0};
static std::atomic<bool>     lastAutoBrightnessValid{false};
static std::atomic<bool>     brightnessRefreshRequested{true};
static std::atomic<uint8_t>  appliedUiBrightness{0};
static std::atomic<bool>     appliedUiBrightnessValid{false};

// Снапшот полей cfg, нужных для рендера — обновляется в начале loop() под cfgMtx.
// px() и show*() читают отсюда, а не из cfg напрямую,
// чтобы избежать гонок с wifiConnTask, который может писать cfg.ssid/wifiPass.
struct RenderSnap {
  uint8_t  rotation; bool flipH, flipV;
  bool     autoColor;
  bool     use24h, showColon; uint8_t colonBright;
  bool     lavaAutoColor; uint16_t lavaHue;
  int8_t   ct6ClockX, ct6ClockY, ct6TempX, ct6TempY;
  uint8_t  ct6ClockR, ct6ClockG, ct6ClockB;
  uint8_t  ct6TempR,  ct6TempG,  ct6TempB;
};
static RenderSnap rSnap;

// Снапшот полей рендера из cfg. Вызывать с захваченным cfgMtx. Используется
// из loop() (раз в 100мс) и из setup() до boot-анимации — иначе px() рисует
// её с дефолтной ориентацией, игнорируя rotation/flip пользователя.
static void refreshRenderSnapLocked() {
  rSnap.rotation       = cfg.rotation;
  rSnap.flipH          = cfg.flipH;
  rSnap.flipV          = cfg.flipV;
  rSnap.autoColor      = cfg.autoColor;
  rSnap.use24h         = cfg.use24h;
  rSnap.showColon      = cfg.showColon;
  rSnap.colonBright    = cfg.colonBright;
  rSnap.lavaAutoColor  = cfg.lavaAutoColor;
  rSnap.lavaHue        = cfg.lavaHue;
  rSnap.ct6ClockX      = cfg.ct6ClockX;
  rSnap.ct6ClockY      = cfg.ct6ClockY;
  rSnap.ct6TempX       = cfg.ct6TempX;
  rSnap.ct6TempY       = cfg.ct6TempY;
  rSnap.ct6ClockR      = cfg.ct6ClockR; rSnap.ct6ClockG = cfg.ct6ClockG; rSnap.ct6ClockB = cfg.ct6ClockB;
  rSnap.ct6TempR       = cfg.ct6TempR;  rSnap.ct6TempG  = cfg.ct6TempG;  rSnap.ct6TempB  = cfg.ct6TempB;
}

static void scheduleDeferredSave(uint32_t delayMs = 0) {
  saveDueMs.store(millis() + delayMs);
  saveError.store(false);
  savePending.store(true);
}

static bool flushDeferredSave(bool force = false) {
  if (!savePending.load()) return true;
  uint32_t due = saveDueMs.load();
  if (!force && (int32_t)(millis() - due) < 0) return true;
  if (!savePending.exchange(false)) return true;
  bool ok = saveConfig();
  saveError.store(!ok);
  return ok;
}

// Аварийная остановка на раннем boot/runtime-отказе: лучше явный halt, чем продолжение
// с NULL-мьютексом или отсутствующими фоновыми задачами.
static void fatalHalt(const char* msg) {
  Serial.printf("[FATAL] %s\n", msg);
  fill_solid(leds, NUM_LEDS, CRGB::Red);
  FastLED.show();
  for (;;) delay(1000);
}

static uint8_t normalizeMode(int mode) {
  // Декоративные режимы (огонь/плазма/лава) удалены вместе с выбором режима
  // в веб-интерфейсе; "Часы + темп" — единственный режим отображения.
  (void)mode;
  return 6;
}

static void sanitizeLoadedConfig(Config& loaded) {
  loaded.mode = normalizeMode(loaded.mode);
  loaded.ct6ClockX = (int8_t)constrain((int)loaded.ct6ClockX, 0, 1);
  loaded.ct6ClockY = (int8_t)constrain((int)loaded.ct6ClockY, 0, 11);
  loaded.ct6TempX = (int8_t)constrain((int)loaded.ct6TempX, -1, 15);
  loaded.ct6TempY = (int8_t)constrain((int)loaded.ct6TempY, 0, 11);
  loaded.rotation = (uint8_t)constrain((int)loaded.rotation, 0, 3);
  loaded.fontIndex = 0;
  loaded.showDegree = false;
  loaded.clockFace = 0;
  // Разделитель часов удалён совсем; colonBright переиспользуется как
  // яркость подложки принтер-оверлея, поэтому фиксируем его значение
  loaded.showColon = false;
  loaded.colonBright = 90;
  loaded.weatherProvider = (uint8_t)constrain((int)loaded.weatherProvider, 0, 2);
  loaded.brightness = (uint8_t)constrain((int)loaded.brightness, (int)MIN_UI_BRIGHTNESS, 255);
  loaded.brightnessDay = (uint8_t)constrain((int)loaded.brightnessDay, (int)MIN_UI_BRIGHTNESS, 255);
  loaded.brightnessNight = (uint8_t)constrain((int)loaded.brightnessNight, (int)MIN_UI_BRIGHTNESS, 255);
  loaded.dayStartHour = (uint8_t)constrain((int)loaded.dayStartHour, 0, 23);
  loaded.nightStartHour = (uint8_t)constrain((int)loaded.nightStartHour, 0, 23);
  loaded.updateMin = (uint16_t)constrain((int)loaded.updateMin, 1, 60);
  loaded.lavaHue = (uint16_t)constrain((int)loaded.lavaHue, 0, 359);
  loaded.tzOffset = constrain(loaded.tzOffset, -12.0f, 14.0f);
  loaded.blinkColon = false;
  loaded.ct6ShowWeatherIcon = false;
  loaded.webPass[0] = '\0';
  strlcpy(loaded.apPass, LEGACY_DEFAULT_AP_PASS, sizeof(loaded.apPass));
}

static bool configEquals(const Config& a, const Config& b) {
  return
    strncmp(a.ssid, b.ssid, sizeof(a.ssid)) == 0 &&
    strncmp(a.wifiPass, b.wifiPass, sizeof(a.wifiPass)) == 0 &&
    strncmp(a.city, b.city, sizeof(a.city)) == 0 &&
    strncmp(a.apiKey, b.apiKey, sizeof(a.apiKey)) == 0 &&
    strncmp(a.ntpServer, b.ntpServer, sizeof(a.ntpServer)) == 0 &&
    a.tzOffset == b.tzOffset &&
    a.brightness == b.brightness &&
    a.autoBrightness == b.autoBrightness &&
    a.brightnessDay == b.brightnessDay &&
    a.brightnessNight == b.brightnessNight &&
    a.dayStartHour == b.dayStartHour &&
    a.nightStartHour == b.nightStartHour &&
    a.autoBrSunMode == b.autoBrSunMode &&
    a.showDegree == b.showDegree &&
    a.fontIndex == b.fontIndex &&
    a.mode == b.mode &&
    a.clockFace == b.clockFace &&
    a.updateMin == b.updateMin &&
    a.use24h == b.use24h &&
    a.showColon == b.showColon &&
    a.blinkColon == b.blinkColon &&
    a.colonBright == b.colonBright &&
    a.autoColor == b.autoColor &&
    a.customR == b.customR &&
    a.customG == b.customG &&
    a.customB == b.customB &&
    a.owmLat == b.owmLat &&
    a.owmLon == b.owmLon &&
    a.owmUseCoords == b.owmUseCoords &&
    a.lavaAutoColor == b.lavaAutoColor &&
    a.lavaHue == b.lavaHue &&
    a.rotation == b.rotation &&
    a.flipH == b.flipH &&
    a.flipV == b.flipV &&
    strncmp(a.webPass, b.webPass, sizeof(a.webPass)) == 0 &&
    strncmp(a.apPass, b.apPass, sizeof(a.apPass)) == 0 &&
    a.ct6ClockX == b.ct6ClockX &&
    a.ct6ClockY == b.ct6ClockY &&
    a.ct6TempX == b.ct6TempX &&
    a.ct6TempY == b.ct6TempY &&
    a.ct6ShowWeatherIcon == b.ct6ShowWeatherIcon &&
    a.ct6ClockR == b.ct6ClockR &&
    a.ct6ClockG == b.ct6ClockG &&
    a.ct6ClockB == b.ct6ClockB &&
    a.ct6TempR == b.ct6TempR &&
    a.ct6TempG == b.ct6TempG &&
    a.ct6TempB == b.ct6TempB &&
    a.weatherProvider == b.weatherProvider &&
    a.tzFromWeather == b.tzFromWeather;
}

static bool bambuConfigEquals(const BambuConfig& a, const BambuConfig& b) {
  return
    a.enabled == b.enabled &&
    strncmp(a.email, b.email, sizeof(a.email)) == 0 &&
    strncmp(a.password, b.password, sizeof(a.password)) == 0 &&
    a.relayEnabled == b.relayEnabled &&
    strncmp(a.relayUrl, b.relayUrl, sizeof(a.relayUrl)) == 0 &&
    strncmp(a.relayKey, b.relayKey, sizeof(a.relayKey)) == 0;
}

static uint32_t fnv1a32(const uint8_t* data, size_t len) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= 16777619UL;
  }
  return hash;
}

static uint32_t storedStateChecksum(const StoredStateBlob& blob) {
  return fnv1a32(reinterpret_cast<const uint8_t*>(&blob), offsetof(StoredStateBlob, checksum));
}

static void ensureBootstrapSecrets() {
  bool changed = false;

  xSemaphoreTake(cfgMtx, portMAX_DELAY);
  if (strncmp(cfg.apPass, LEGACY_DEFAULT_AP_PASS, sizeof(cfg.apPass)) != 0) {
    strlcpy(cfg.apPass, LEGACY_DEFAULT_AP_PASS, sizeof(cfg.apPass));
    changed = true;
  }
  if (cfg.webPass[0] != '\0') {
    cfg.webPass[0] = '\0';
    changed = true;
  }
  xSemaphoreGive(cfgMtx);

  if (changed) {
    if (saveConfig()) Serial.println("[Security] Restored default AP password and disabled web auth");
    else Serial.println("[Security] Failed to persist default AP password / web auth policy");
  }

  Serial.printf("[Security] AP password: %s\n", LEGACY_DEFAULT_AP_PASS);
  Serial.println("[Security] Web auth disabled");
}

static bool loadStoredStateBlob(StoredStateBlob& blob) {
  if (!prefs.isKey("state")) return false;
  size_t sz = prefs.getBytesLength("state");
  if (sz != sizeof(blob)) {
    Serial.printf("[NVS] state size mismatch: %u (expected %u)\n",
                  (unsigned)sz, (unsigned)sizeof(blob));
    return false;
  }
  size_t got = prefs.getBytes("state", &blob, sizeof(blob));
  if (got != sizeof(blob)) {
    Serial.printf("[NVS] getBytes(state) size mismatch: %u (expected %u)\n",
                  (unsigned)got, (unsigned)sizeof(blob));
    return false;
  }
  if (blob.magic != STORED_STATE_MAGIC || blob.version != STORED_STATE_VERSION || blob.size != sizeof(blob)) {
    Serial.printf("[NVS] state header mismatch: magic=%08lx version=%u size=%u\n",
                  (unsigned long)blob.magic, (unsigned)blob.version, (unsigned)blob.size);
    return false;
  }
  uint32_t expected = storedStateChecksum(blob);
  if (blob.checksum != expected) {
    Serial.printf("[NVS] state checksum mismatch: %08lx != %08lx\n",
                  (unsigned long)blob.checksum, (unsigned long)expected);
    return false;
  }
  return true;
}

static void migrateLegacyConfigV326(const LegacyConfigV326& src, Config& dst) {
  Config migrated;
  memcpy(&migrated.ssid, &src.ssid, offsetof(Config, ct6ClockX));
  migrated.blinkColon = false;
  migrated.ct6ClockX = src.ct6ClockX;
  migrated.ct6ClockY = src.ct6ClockY;
  migrated.ct6TempX = src.ct6TempX;
  migrated.ct6TempY = src.ct6TempY;
  migrated.ct6ShowWeatherIcon = false;
  migrated.ct6ClockR = src.ct6ClockR;
  migrated.ct6ClockG = src.ct6ClockG;
  migrated.ct6ClockB = src.ct6ClockB;
  migrated.ct6TempR = src.ct6TempR;
  migrated.ct6TempG = src.ct6TempG;
  migrated.ct6TempB = src.ct6TempB;
  dst = migrated;
}

static void migrateLegacyConfigV331(const LegacyConfigV331Compat& src, Config& dst) {
  Config migrated;
  memcpy(&migrated.ssid, &src.ssid, offsetof(Config, ct6ClockX));
  migrated.blinkColon = false;
  migrated.ct6ClockX = src.ct6ClockX;
  migrated.ct6ClockY = src.ct6ClockY;
  migrated.ct6TempX = src.ct6TempX;
  migrated.ct6TempY = src.ct6TempY;
  migrated.ct6ShowWeatherIcon = false;
  migrated.ct6ClockR = src.ct6ClockR;
  migrated.ct6ClockG = src.ct6ClockG;
  migrated.ct6ClockB = src.ct6ClockB;
  migrated.ct6TempR = src.ct6TempR;
  migrated.ct6TempG = src.ct6TempG;
  migrated.ct6TempB = src.ct6TempB;
  dst = migrated;
}

static bool loadConfigFromNvs(uint16_t ver, uint16_t csz, Config& out) {
  if (csz == 0 || !prefs.isKey("cfg")) return false;

  if (ver >= CONFIG_VERSION && csz <= (uint16_t)sizeof(Config)) {
    Config loaded = Config();
    size_t got = prefs.getBytes("cfg", &loaded, csz);
    if (got != csz) {
      Serial.printf("[NVS] getBytes(cfg) size mismatch: %u (expected %u)\n",
                    (unsigned)got, (unsigned)csz);
      return false;
    }
    out = loaded;
    return true;
  }

  if (ver >= 331 && csz <= (uint16_t)sizeof(LegacyConfigV331Compat)) {
    LegacyConfigV331Compat legacy = {};
    size_t got = prefs.getBytes("cfg", &legacy, csz);
    if (got != csz) {
      Serial.printf("[NVS] getBytes(cfg legacy-v331) size mismatch: %u (expected %u)\n",
                    (unsigned)got, (unsigned)csz);
      return false;
    }
    migrateLegacyConfigV331(legacy, out);
    Serial.println("[NVS] Migrated legacy cfg: v331 layout");
    return true;
  }

  if (ver >= 326 && csz <= (uint16_t)sizeof(LegacyConfigV326)) {
    LegacyConfigV326 legacy = {};
    size_t got = prefs.getBytes("cfg", &legacy, csz);
    if (got != csz) {
      Serial.printf("[NVS] getBytes(cfg legacy) size mismatch: %u (expected %u)\n",
                    (unsigned)got, (unsigned)csz);
      return false;
    }
    migrateLegacyConfigV326(legacy, out);
    Serial.println("[NVS] Migrated legacy cfg: v326 layout");
    return true;
  }

  Serial.printf("[NVS] Unsupported cfg layout: ver=%u csz=%u\n", ver, csz);
  return false;
}

static uint8_t bambuResolveLifecycle(const String& rawStatus, const String& rawStage, float progress) {
  String normalized;
  normalized.reserve(rawStatus.length() + rawStage.length());
  auto appendNorm = [&](const String& src) {
    for (size_t i = 0; i < src.length(); i++) {
      char ch = src[i];
      if (isalnum((unsigned char)ch)) normalized += (char)toupper((unsigned char)ch);
    }
  };
  appendNorm(rawStatus);
  appendNorm(rawStage);

  if (normalized.indexOf("FAIL") >= 0 || normalized.indexOf("ERROR") >= 0 || normalized.indexOf("CANCEL") >= 0) return BAMBU_ERROR;
  if (normalized.indexOf("PAUSE") >= 0) return BAMBU_PAUSED;
  if (normalized.indexOf("DONE") >= 0 || normalized.indexOf("SUCCESS") >= 0 || normalized.indexOf("COMPLETE") >= 0 || normalized.indexOf("COMPLETED") >= 0 || normalized.indexOf("FINISH") >= 0) return BAMBU_FINISHED;
  if (normalized.indexOf("PREPARE") >= 0 || normalized.indexOf("PREPARING") >= 0 || normalized.indexOf("STARTING") >= 0 || normalized.indexOf("HEATING") >= 0 || normalized.indexOf("DOWNLOAD") >= 0) return BAMBU_PREPARING;
  if (normalized.indexOf("RUNNING") >= 0 || normalized.indexOf("PRINTING") >= 0 || normalized.indexOf("PROCESSING") >= 0) return BAMBU_PRINTING;
  if (normalized.indexOf("OFFLINE") >= 0) return BAMBU_OFFLINE;
  if (normalized.indexOf("IDLE") >= 0 || normalized.indexOf("WAIT") >= 0) return BAMBU_IDLE;
  if (progress > 0.0f && progress < 100.0f) return BAMBU_PRINTING;
  return BAMBU_UNKNOWN;
}

static const char* bambuLifecycleLabel(uint8_t lifecycle) {
  switch (lifecycle) {
    case BAMBU_PRINTING: return "печать";
    case BAMBU_PREPARING: return "подготовка";
    case BAMBU_PAUSED: return "пауза";
    case BAMBU_FINISHED: return "готово";
    case BAMBU_ERROR: return "ошибка";
    case BAMBU_IDLE: return "ожидание";
    case BAMBU_OFFLINE: return "офлайн";
    default: return "нет данных";
  }
}

static uint8_t bambuLifecycleFromString(const String& raw) {
  String normalized;
  normalized.reserve(raw.length());
  for (size_t i = 0; i < raw.length(); i++) {
    char ch = raw[i];
    if (isalnum((unsigned char)ch)) normalized += (char)toupper((unsigned char)ch);
  }
  if (normalized == "PRINTING" || normalized == "RUNNING") return BAMBU_PRINTING;
  if (normalized == "PREPARING" || normalized == "PREPARE") return BAMBU_PREPARING;
  if (normalized == "PAUSED" || normalized == "PAUSE") return BAMBU_PAUSED;
  if (normalized == "FINISHED" || normalized == "SUCCESS" || normalized == "DONE") return BAMBU_FINISHED;
  if (normalized == "ERROR" || normalized == "FAILED" || normalized == "FAIL") return BAMBU_ERROR;
  if (normalized == "OFFLINE") return BAMBU_OFFLINE;
  if (normalized == "IDLE") return BAMBU_IDLE;
  return BAMBU_UNKNOWN;
}

static bool bambuFetchRelayStatus(const char* relayUrl, const char* relayKey) {
  if (!relayUrl || !relayUrl[0]) return false;

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(relayUrl)) {
    Serial.println("[BambuRelay] begin failed");
    return false;
  }
  if (relayKey && relayKey[0]) http.addHeader("X-Relay-Key", relayKey);
  int code = http.GET();
  String body = code > 0 ? http.getString() : String();
  http.end();

  if (code != 200) {
    Serial.printf("[BambuRelay] HTTP %d\n", code);
    if (body.length()) Serial.printf("[BambuRelay] body=%s\n", body.c_str());
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    Serial.printf("[BambuRelay] bad json: %s\n", body.c_str());
    return false;
  }

  bool connected = doc["connected"] | false;
  bool printerOnline = doc["printerOnline"].isNull() ? connected : doc["printerOnline"].as<bool>();
  float progress = doc["progress"] | 0.0f;
  if (progress >= 0.0f && progress < 1.0f) progress *= 100.0f;
  String rawStatus = String(doc["status"] | "");
  String rawStage = String(doc["stage"] | "");
  String detail = String(doc["detail"] | "");
  String lifecycleText = String(doc["lifecycle"] | "");
  uint8_t lifecycle = bambuLifecycleFromString(lifecycleText);
  if (lifecycle == BAMBU_UNKNOWN) lifecycle = bambuResolveLifecycle(rawStatus, rawStage, progress);
  if (!printerOnline) lifecycle = BAMBU_OFFLINE;
  if (lifecycle == BAMBU_FINISHED && progress < 100.0f) progress = 100.0f;

  BambuState st = getBambuStateSnapshot();
  st.enabled = true;
  st.configured = true;
  st.connected = connected;
  st.printerOnline = printerOnline;
  st.lifecycle = lifecycle;
  st.displayLifecycle = (lifecycle == BAMBU_IDLE || lifecycle == BAMBU_OFFLINE || lifecycle == BAMBU_UNKNOWN)
    ? BAMBU_UNKNOWN
    : bambuOverlayLifecycle(lifecycle);
  st.progress = (uint8_t)constrain((int)lroundf(progress), 0, 100);
  strlcpy(st.rawStatus, rawStatus.c_str(), sizeof(st.rawStatus));
  strlcpy(st.rawStage, rawStage.c_str(), sizeof(st.rawStage));
  if (detail.length()) {
    setBambuDetail(st, detail.c_str());
  } else if (!connected) {
    setBambuDetail(st, "Relay недоступен");
  } else {
    char msg[96];
    snprintf(msg, sizeof(msg), "Relay: %s, %u%%", bambuLifecycleLabel(st.lifecycle), st.progress);
    setBambuDetail(st, msg);
  }
  setBambuStateSnapshot(st);
  Serial.printf("[BambuRelay] connected=%d lifecycle=%u progress=%u status='%s' stage='%s'\n",
                connected ? 1 : 0, st.lifecycle, st.progress, st.rawStatus, st.rawStage);
  return true;
}

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
  int fx = rSnap.flipH ? (MW-1-x) : x;
  int fy = rSnap.flipV ? (MH-1-y) : y;
  // 2. Поворот
  int rx = fx, ry = fy;
  switch (rSnap.rotation) {
    case 1: rx = MH-1-fy; ry = fx;       break; // 90° по часовой
    case 2: rx = MW-1-fx; ry = MH-1-fy;  break; // 180°
    case 3: rx = fy;      ry = MW-1-fx;  break; // 270° по часовой
  }
  int i = XY(rx, ry);
  if (i >= 0) leds[i] = c;
}
inline void clearMatrix() { fill_solid(leds, NUM_LEDS, CRGB::Black); }

// Вызывается вместо FastLED.show() во всех режимах отображения.
// Накладывает красный диод в правом верхнем углу при потере WiFi.
inline void matrixShow() {
  if (!wifiConnected.load() && hasSsid.load()) {
    // Физический правый верхний угол матрицы — обходим rotation/flip
    int i = XY(15, 0);
    if (i >= 0) leds[i] = CRGB::Red;
  }
  FastLED.show();
}

static uint8_t calibratedBrightness(uint8_t uiBrightness) {
  uint8_t v = (uint8_t)constrain((int)uiBrightness, (int)MIN_UI_BRIGHTNESS, 255);
  // WS2812 на малых значениях всё ещё субъективно яркие. Храним UI-шкалу 1..255,
  // а на железо отдаём мягкую квадратичную кривую: больше контроля в тёмной зоне,
  // 1 остаётся едва заметным свечением, 255 остаётся полной яркостью.
  uint16_t mapped = ((uint16_t)v * (uint16_t)v + 254) / 255;
  return (uint8_t)max((int)MIN_UI_BRIGHTNESS, (int)mapped);
}

static void applyBrightness(uint8_t uiBrightness) {
  uiBrightness = (uint8_t)constrain((int)uiBrightness, (int)MIN_UI_BRIGHTNESS, 255);
  if (appliedUiBrightnessValid.load() && appliedUiBrightness.load() == uiBrightness) return;
  FastLED.setBrightness(calibratedBrightness(uiBrightness));
  appliedUiBrightness.store(uiBrightness);
  appliedUiBrightnessValid.store(true);
}

static bool getStableLocalTime(struct tm* out) {
  if (!out) return false;

  time_t now = time(nullptr);
  if (now >= MIN_VALID_EPOCH) {
    localtime_r(&now, out);
    taskENTER_CRITICAL(&clockCacheMux);
    clockCacheEpoch = now;
    clockCacheMs = millis();
    clockCacheValid = true;
    taskEXIT_CRITICAL(&clockCacheMux);
    return true;
  }

  bool valid;
  time_t epoch;
  uint32_t ms;
  taskENTER_CRITICAL(&clockCacheMux);
  valid = clockCacheValid;
  epoch = clockCacheEpoch;
  ms = clockCacheMs;
  taskEXIT_CRITICAL(&clockCacheMux);

  if (!valid || epoch < MIN_VALID_EPOCH) return false;
  time_t estimated = epoch + (time_t)((millis() - ms) / 1000UL);
  localtime_r(&estimated, out);
  return true;
}

static bool isWithinDayWindow(uint16_t nowMins, uint16_t dayStartMins, uint16_t nightStartMins) {
  return (dayStartMins < nightStartMins)
    ? (nowMins >= dayStartMins && nowMins < nightStartMins)
    : (nowMins >= dayStartMins || nowMins < nightStartMins);
}

static void rememberAutoBrightness(uint8_t uiBrightness) {
  uiBrightness = (uint8_t)constrain((int)uiBrightness, (int)MIN_UI_BRIGHTNESS, 255);
  uint8_t prev = lastAutoBrightness.load();
  bool hadPrev = lastAutoBrightnessValid.load();
  lastAutoBrightness.store(uiBrightness);
  lastAutoBrightnessValid.store(true);
  if (hadPrev && prev == uiBrightness) return;
  if (!prefsMtx) return;

  xSemaphoreTake(prefsMtx, portMAX_DELAY);
  bool ok = prefs.begin("matrix", false);
  if (ok) {
    ok = prefs.putUChar(PREF_LAST_AUTO_BRIGHTNESS, uiBrightness) == 1;
    prefs.end();
  }
  xSemaphoreGive(prefsMtx);
  if (!ok) Serial.println("[Brightness] Failed to persist last auto brightness");
}

static bool applyConfiguredBrightnessNow(bool fallbackToCached) {
  uint8_t br, brDay, brNight, dayH, nightH;
  bool autoB, sunMode;

  xSemaphoreTake(cfgMtx, portMAX_DELAY);
  br       = cfg.brightness;
  autoB    = cfg.autoBrightness;
  sunMode  = cfg.autoBrSunMode;
  brDay    = cfg.brightnessDay;
  brNight  = cfg.brightnessNight;
  dayH     = cfg.dayStartHour;
  nightH   = cfg.nightStartHour;
  xSemaphoreGive(cfgMtx);

  if (!autoB) {
    applyBrightness(br);
    return true;
  }

  struct tm ti;
  if (getStableLocalTime(&ti)) {
    uint16_t nowMins = (uint16_t)ti.tm_hour * 60 + ti.tm_min;
    bool isDay;
    if (sunMode && sunTimeValid.load()) {
      uint32_t snt = sunTimes.load();
      uint16_t rise = (uint16_t)((snt >> 24) & 0xFF) * 60 + ((snt >> 16) & 0xFF);
      uint16_t set_ = (uint16_t)((snt >>  8) & 0xFF) * 60 + ( snt        & 0xFF);
      isDay = isWithinDayWindow(nowMins, rise, set_);
    } else {
      isDay = isWithinDayWindow(nowMins, (uint16_t)dayH * 60, (uint16_t)nightH * 60);
    }
    uint8_t target = isDay ? brDay : brNight;
    applyBrightness(target);
    rememberAutoBrightness(target);
    return true;
  }

  if (fallbackToCached) {
    uint8_t fallback = lastAutoBrightnessValid.load() ? lastAutoBrightness.load() : br;
    applyBrightness(fallback);
  }
  return false;
}

// ════════════════════════════════════════════════════════════
//  РЕНДЕРИНГ ТЕКСТА
// ════════════════════════════════════════════════════════════

// Индекс символа для единственного Micro 3×5 шрифта
static int microGlyphIndex(char c) {
  uint8_t u = (uint8_t)c;
  if (u >= '0' && u <= '9') return (int)(u - '0');
  if (c == '-') return 10;
  if (c == '+') return 11;
  return 12; // space
}

void drawChar3(int x, int y, char c, CRGB col) {
  int idx = microGlyphIndex(c);
  for (int row = 0; row < 5; row++) {
    uint8_t bits = pgm_read_byte(&F_MICRO[idx][row]);
    for (int col2 = 0; col2 < 3; col2++)
      if (bits & (1 << (2 - col2))) px(x + col2, y + row, col);
  }
}

int strW3(const char* s) {
  int n = strlen(s);
  return n ? n * 3 + (n - 1) : 0;
}


void drawStr3(int x, int y, const char* s, CRGB col) {
  int cx = x;
  for (int i = 0; s[i]; i++) {
    if (i > 0) cx++;
    drawChar3(cx, y, s[i], col);
    cx += 3;
  }
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

// ════════════════════════════════════════════════════════════
//  РАЗДЕЛИТЕЛЬ ЧАСОВ: удалён — часы рисуются без двоеточия
// ════════════════════════════════════════════════════════════

// ════════════════════════════════════════════════════════════
//  РЕЖИМ 6: ЧАСЫ + ТЕМПЕРАТУРА
//  Сверху ЧЧ:ММ, снизу температура — оба слоя выровнены по центру
// ════════════════════════════════════════════════════════════
void showClockTemp() {
  int cx = constrain((int)rSnap.ct6ClockX, 0, 1);
  int cy = rSnap.ct6ClockY;
  int ty = rSnap.ct6TempY;
  bool autoCompactTempLayout = (rSnap.ct6TempX < 0);

  CRGB clockCol = CRGB(rSnap.ct6ClockR, rSnap.ct6ClockG, rSnap.ct6ClockB);

  bool timeValid = false;
  char hh[3] = "--";
  char mm[3] = "--";
  struct tm ti;
  if (getStableLocalTime(&ti)) {
    int h = rSnap.use24h ? ti.tm_hour : (ti.tm_hour % 12 == 0 ? 12 : ti.tm_hour % 12);
    snprintf(hh, sizeof(hh), "%02d", h);
    snprintf(mm, sizeof(mm), "%02d", ti.tm_min);
    timeValid = true;
  }

  float t = safeGetTemp();
  bool tempKnown = t > -999.0f;
  char tempText[5] = "---";
  CRGB tempCol = rSnap.autoColor ? CRGB(60, 60, 60) : CRGB(rSnap.ct6TempR, rSnap.ct6TempG, rSnap.ct6TempB);
  int16_t tempColorKey = -9999;
  if (tempKnown) {
    t = constrain(t, -50.0f, 60.0f);
    tempCol = rSnap.autoColor ? tempToColor(t) : CRGB(rSnap.ct6TempR, rSnap.ct6TempG, rSnap.ct6TempB);
    int abst = (int)roundf(fabsf(t));
    if (autoCompactTempLayout) {
      if (abst < 10) snprintf(tempText, sizeof(tempText), "%c%d", (t < 0) ? '-' : '+', abst);
      else snprintf(tempText, sizeof(tempText), "%c%02d", (t < 0) ? '-' : '+', abst);
    } else {
      int bi = 0;
      tempText[bi++] = (t < 0) ? '-' : '+';
      if (abst >= 10) tempText[bi++] = '0' + abst / 10;
      tempText[bi++] = '0' + abst % 10;
      tempText[bi] = '\0';
    }
    tempColorKey = (int16_t)lroundf(t * 10.0f);
  }

  BambuState printer = getBambuStateSnapshot();
  uint8_t overlayPhase = 0;
  if (printer.displayLifecycle == BAMBU_FINISHED ||
      printer.displayLifecycle == BAMBU_PAUSED ||
      printer.displayLifecycle == BAMBU_ERROR) {
    overlayPhase = (uint8_t)((millis() / 500UL) & 1UL);
  } else if (printer.displayLifecycle == BAMBU_PREPARING) {
    overlayPhase = (uint8_t)((millis() / 250UL) & 3UL);
  }

  struct ClockFrameKey {
    uint32_t invalidationSeq;
    bool timeValid;
    char hh[3];
    char mm[3];
    char tempText[5];
    int16_t tempColorKey;
    int8_t cx, cy, tx, ty;
    bool autoCompactTempLayout;
    bool use24h, showColon, autoColor;
    uint8_t colonBright;
    uint8_t clockR, clockG, clockB;
    uint8_t tempR, tempG, tempB;
    uint8_t tempColR, tempColG, tempColB;
    uint8_t rotation;
    bool flipH, flipV;
    uint8_t bambuLifecycle;
    uint8_t bambuProgress;
    uint8_t overlayPhase;
    bool wifiWarning;
    bool brightnessValid;
    uint8_t uiBrightness;
  };

  ClockFrameKey key = {};
  key.invalidationSeq = displayInvalidationSeq.load();
  key.timeValid = timeValid;
  strlcpy(key.hh, hh, sizeof(key.hh));
  strlcpy(key.mm, mm, sizeof(key.mm));
  strlcpy(key.tempText, tempText, sizeof(key.tempText));
  key.tempColorKey = tempColorKey;
  key.cx = (int8_t)cx;
  key.cy = (int8_t)cy;
  key.tx = rSnap.ct6TempX;
  key.ty = (int8_t)ty;
  key.autoCompactTempLayout = autoCompactTempLayout;
  key.use24h = rSnap.use24h;
  key.showColon = rSnap.showColon;
  key.autoColor = rSnap.autoColor;
  key.colonBright = rSnap.colonBright;
  key.clockR = rSnap.ct6ClockR; key.clockG = rSnap.ct6ClockG; key.clockB = rSnap.ct6ClockB;
  key.tempR = rSnap.ct6TempR; key.tempG = rSnap.ct6TempG; key.tempB = rSnap.ct6TempB;
  key.tempColR = tempCol.r; key.tempColG = tempCol.g; key.tempColB = tempCol.b;
  key.rotation = rSnap.rotation;
  key.flipH = rSnap.flipH;
  key.flipV = rSnap.flipV;
  key.bambuLifecycle = printer.displayLifecycle;
  key.bambuProgress = printer.progress;
  key.overlayPhase = overlayPhase;
  key.wifiWarning = !wifiConnected.load() && hasSsid.load();
  key.brightnessValid = appliedUiBrightnessValid.load();
  key.uiBrightness = appliedUiBrightness.load();

  static ClockFrameKey lastKey = {};
  static bool lastKeyValid = false;
  if (lastKeyValid && memcmp(&key, &lastKey, sizeof(key)) == 0) {
    return;
  }
  lastKey = key;
  lastKeyValid = true;

  clearMatrix();

  if (timeValid) {
    drawStr3(cx,     cy, hh, clockCol);
    drawStr3(cx + 9, cy, mm, clockCol);
  } else {
    drawStr3(cx,     cy, "--", CRGB(60, 60, 60));
    drawStr3(cx + 9, cy, "--", CRGB(60, 60, 60));
  }

  auto centeredTextX = [](const char* text, int areaX, int areaW) -> int {
    int w = strW3(text);
    if (w <= 0) return areaX;
    return areaX + max(0, (areaW - w) / 2);
  };

  auto drawAutoTempText = [&](const char* text, CRGB color) {
    int tx = centeredTextX(text, 0, MW);
    drawStr3(tx, ty, text, color);
  };

  if (autoCompactTempLayout) {
    drawAutoTempText(tempText, tempCol);
  } else {
    int tx = constrain((int)rSnap.ct6TempX, 0, 15);
    drawStr3(tx, ty, tempText, tempCol);
  }

  switch (printer.displayLifecycle) {
    case BAMBU_PREPARING: {
      CRGB track = clockCol;
      track.nscale8(rSnap.colonBright);
      CRGB c = CRGB(0, 220, 70);
      c.nscale8(beatsin8(10, 72, 220));
      for (uint8_t x = 0; x < MW; x++) {
        CRGB pxc = (x == 0) ? c : track;
        px(x, 7, pxc);
        px(x, 8, pxc);
      }
      break;
    }
    case BAMBU_PRINTING: {
      CRGB track = clockCol;
      track.nscale8(rSnap.colonBright);
      CRGB fill = CRGB(0, 220, 70);
      CRGB fillMin = fill;
      fillMin.nscale8(56);
      for (uint8_t x = 0; x < MW; x++) {
        float lower = (100.0f * x) / MW;
        float upper = (100.0f * (x + 1)) / MW;
        CRGB c = track;
        if (printer.progress >= upper) {
          c = fill;
        } else if (printer.progress > lower && x < (MW - 1)) {
          float phase = (printer.progress - lower) / (upper - lower);
          uint8_t mix = (uint8_t)constrain((int)lroundf(phase * 255.0f), 0, 255);
          c = blend(fillMin, fill, mix);
        }
        px(x, 7, c);
        px(x, 8, c);
      }
      break;
    }
    case BAMBU_FINISHED: {
      if (overlayPhase) {
        CRGB c = CRGB(0, 220, 70);
        for (uint8_t x = 0; x < MW; x++) {
          px(x, 7, c);
          px(x, 8, c);
        }
      }
      break;
    }
    case BAMBU_PAUSED:
    case BAMBU_ERROR: {
      if (overlayPhase) {
        CRGB c = printer.displayLifecycle == BAMBU_PAUSED ? CRGB(255, 200, 0) : CRGB(255, 0, 0);
        for (uint8_t y = 7; y <= 8; y++)
          for (uint8_t x = 0; x < MW; x++)
            px(x, y, c);
      }
      break;
    }
    case BAMBU_IDLE:
    case BAMBU_OFFLINE:
    case BAMBU_UNKNOWN:
    default: break;
  }

  matrixShow();
}

// ════════════════════════════════════════════════════════════
//  ПОГОДА
// ════════════════════════════════════════════════════════════
// Применяет UTC-смещение из ответа погодного API при включённом tzFromWeather.
// Вызывается из weatherTask; configTime дёрнет loop task через ntpPending.
static void applyWeatherTz(long offsetSec) {
  if (offsetSec < -12L * 3600 || offsetSec > 14L * 3600) return;
  float tzH = offsetSec / 3600.0f;
  bool changed = false;
  xSemaphoreTake(cfgMtx, portMAX_DELAY);
  if (cfg.tzFromWeather && fabsf(cfg.tzOffset - tzH) > 0.01f) {
    cfg.tzOffset = tzH;
    changed = true;
  }
  xSemaphoreGive(cfgMtx);
  if (changed) {
    Serial.printf("[Weather] TZ из локации погоды: UTC%+.2f\n", tzH);
    ntpPending.store(true);     // loop task вызовет configTime с новым смещением
    scheduleDeferredSave(1500);
  }
}

void fetchWeather() {
  if (!wifiConnected) return;
  taskENTER_CRITICAL(&weatherMux);
  lastWeatherAttemptMs = millis();
  taskEXIT_CRITICAL(&weatherMux);

  // Снимок cfg под cfgMtx — защита от гонки с handlePostSettings в loop task
  char apiKey[sizeof(cfg.apiKey)];
  char city[sizeof(cfg.city)];
  float owmLat, owmLon;
  bool owmUseCoords;
  uint8_t provider;
  xSemaphoreTake(cfgMtx, portMAX_DELAY);
  strlcpy(apiKey, cfg.apiKey, sizeof(apiKey));
  strlcpy(city,   cfg.city,   sizeof(city));
  owmLat = cfg.owmLat;
  owmLon = cfg.owmLon;
  owmUseCoords = cfg.owmUseCoords;
  provider = cfg.weatherProvider;   // 0=авто (OWM→OM), 1=только OWM, 2=только Open-Meteo
  xSemaphoreGive(cfgMtx);

  uint8_t keyLen = (uint8_t)min(strlen(apiKey), (size_t)255);
  bool tryOwm = (provider != 2);
  bool owmOk = false;

  if (tryOwm && keyLen == 0) {
    setWeatherDiag(0, "missing api key", keyLen, "owm");
    Serial.println("[Weather] OpenWeatherMap API key is missing");
    if (provider == 1) return;      // только OWM — без ключа делать нечего
    tryOwm = false;                 // авто: сразу пробуем Open-Meteo
  }

  auto urlEncode = [](const char* s) -> String {
    String out;
    while (*s) {
      uint8_t c = (uint8_t)*s++;
      bool unreserved =
        (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~';
      if (unreserved) {
        out += (char)c;
      } else {
        char hex[4];
        snprintf(hex, sizeof(hex), "%%%02X", c);
        out += hex;
      }
    }
    return out;
  };

  // ── OpenWeatherMap: основной источник (провайдеры «авто» и «только OWM») ──
  if (tryOwm) {
    WiFiClientSecure sc;
    sc.setInsecure(); // HTTPS защищает API key от пассивного перехвата в LAN; CA не проверяем ради RAM.
    // Ядро ESP32 Arduino 3.x: setTimeout() — миллисекунды, setHandshakeTimeout() — секунды.
    sc.setTimeout(8000);
    sc.setHandshakeTimeout(8);
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    http.setReuse(false);
    http.useHTTP10(true);

    String path;
    if (owmUseCoords) {
      path = String("/data/2.5/weather?lat=") + String(owmLat, 4) +
             "&lon=" + String(owmLon, 4) +
             "&appid=" + String(apiKey) + "&units=metric";
      Serial.printf("[Weather] OWM coords lat=%.4f lon=%.4f keyLen=%u\n",
                    owmLat, owmLon, keyLen);
    } else {
      path = String("/data/2.5/weather?q=") + urlEncode(city) +
             "&appid=" + String(apiKey) + "&units=metric";
      Serial.printf("[Weather] OWM city='%s' keyLen=%u\n", city, keyLen);
    }

    auto applyWeatherPayload = [&](Stream& body) -> bool {
      JsonDocument doc;
      // getStream() парсит прямо из потока — без промежуточного String в heap
      DeserializationError err = deserializeJson(doc, body);
      if (err) {
        char msg[64];
        snprintf(msg, sizeof(msg), "json: %s", err.c_str());
        setWeatherDiag(200, msg, keyLen, "owm");
        Serial.printf("[Weather] JSON error: %s\n", err.c_str());
        return false;
      }

      float t = doc["main"]["temp"].as<float>();
      taskENTER_CRITICAL(&weatherMux);
      currentTemp   = t;
      lastWeatherMs = millis();
      lastWeatherHttpCode = 200;
      lastWeatherKeyLen = keyLen;
      lastWeatherError[0] = '\0';
      strlcpy(lastWeatherSource, "owm", sizeof(lastWeatherSource));
      taskEXIT_CRITICAL(&weatherMux);
      Serial.printf("[Weather] OK %.1f C\n", t);

      // OWM отдаёт смещение UTC в поле timezone (секунды)
      if (doc["timezone"].is<long>()) applyWeatherTz(doc["timezone"].as<long>());

      // В ответе OWM всегда присутствует coord.lat/coord.lon — кэшируем в cfg.
      // В city-режиме это единственный источник координат для fetchSunriseSunset().
      if (!owmUseCoords) {
        float lat = doc["coord"]["lat"] | 0.0f;
        float lon = doc["coord"]["lon"] | 0.0f;
        if (lat != 0.0f || lon != 0.0f) {
          bool sunMode;
          xSemaphoreTake(cfgMtx, portMAX_DELAY);
          cfg.owmLat = lat;
          cfg.owmLon = lon;
          sunMode = cfg.autoBrSunMode;
          xSemaphoreGive(cfgMtx);
          owmLat = lat; owmLon = lon; // для возможного фоллбэка в этом же вызове
          // Координаты только что появились — запросить восход/закат если нужно
          if (sunMode && !sunTimeValid.load()) sunFetchRequested.store(true);
        }
      }
      return true;
    };

    // Сначала HTTPS, чтобы не передавать API key открытым текстом.
    String url = "https://api.openweathermap.org" + path;
    if (http.begin(sc, url)) {
      int code = http.GET();
      if (code == 200) {
        owmOk = applyWeatherPayload(http.getStream());
      } else {
        String err = http.errorToString(code);
        char tlsErr[64] = "";
        int tlsCode = sc.lastError(tlsErr, sizeof(tlsErr));
        char msg[96];
        if (tlsCode != 0) {
          snprintf(msg, sizeof(msg), "HTTP %d: %s; TLS %d: %s", code, err.c_str(), tlsCode, tlsErr);
        } else {
          snprintf(msg, sizeof(msg), "HTTP %d: %s", code, err.c_str());
        }
        setWeatherDiag(code, msg, keyLen, "owm");
        Serial.printf("[Weather] %s\n", msg);
      }
      http.end();
    } else {
      setWeatherDiag(0, "http begin failed", keyLen, "owm");
      Serial.println("[Weather] HTTP begin failed");
    }
  }

  if (owmOk || provider == 1) return;

  // ── Open-Meteo: основной источник (провайдер 2) или фоллбэк в авто-режиме ──
  // Работает только по координатам; в city-режиме они кэшируются из ответов OWM.
  if (owmLat == 0.0f && owmLon == 0.0f) {
    setWeatherDiag(0, provider == 2 ? "open-meteo: no coords"
                                    : "open-meteo fallback skipped: no coords",
                   keyLen, "open-meteo");
    return;
  }

  WiFiClientSecure meteoClient;
  meteoClient.setInsecure();
  // Как фоллбэк — таймауты короче основного, чтобы при недоступности обоих API
  // weatherTask не блокировался дольше необходимого; как основной — полные 8 с.
  uint16_t meteoTimeout = (provider == 2) ? 8000 : 5000;
  meteoClient.setTimeout(meteoTimeout);
  meteoClient.setHandshakeTimeout(meteoTimeout / 1000);
  HTTPClient meteoHttp;
  meteoHttp.setConnectTimeout(meteoTimeout);
  meteoHttp.setTimeout(meteoTimeout);
  meteoHttp.setReuse(false);
  meteoHttp.useHTTP10(true);
  // timezone=auto: в ответ добавляется utc_offset_seconds локации
  String meteoUrl = String("https://api.open-meteo.com/v1/forecast?latitude=") +
                    String(owmLat, 4) + "&longitude=" + String(owmLon, 4) +
                    "&current=temperature_2m&timezone=auto";
  if (!meteoHttp.begin(meteoClient, meteoUrl)) {
    setWeatherDiag(0, "open-meteo begin failed", keyLen, "open-meteo");
    Serial.println("[Weather] Open-Meteo begin failed");
    return;
  }

  int meteoCode = meteoHttp.GET();
  if (meteoCode == 200) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, meteoHttp.getStream());
    if (err) {
      char msg[64];
      snprintf(msg, sizeof(msg), "open-meteo json: %s", err.c_str());
      setWeatherDiag(meteoCode, msg, keyLen, "open-meteo");
      Serial.printf("[Weather] %s\n", msg);
    } else {
      float t = doc["current"]["temperature_2m"] | NAN;
      if (isnan(t)) t = doc["current_weather"]["temperature"] | NAN;
      if (isnan(t)) {
        setWeatherDiag(meteoCode, "open-meteo missing temperature", keyLen, "open-meteo");
        Serial.println("[Weather] Open-Meteo missing temperature");
      } else {
        taskENTER_CRITICAL(&weatherMux);
        currentTemp = t;
        lastWeatherMs = millis();
        lastWeatherHttpCode = meteoCode;
        lastWeatherKeyLen = keyLen;
        lastWeatherError[0] = '\0';
        strlcpy(lastWeatherSource, "open-meteo", sizeof(lastWeatherSource));
        taskEXIT_CRITICAL(&weatherMux);
        Serial.printf("[Weather] Open-Meteo OK %.1f C\n", t);
        if (doc["utc_offset_seconds"].is<long>())
          applyWeatherTz(doc["utc_offset_seconds"].as<long>());
      }
    }
  } else {
    String meteoErr = meteoHttp.errorToString(meteoCode);
    char msg[96];
    snprintf(msg, sizeof(msg), "open-meteo HTTP %d: %s", meteoCode, meteoErr.c_str());
    setWeatherDiag(meteoCode, msg, keyLen, "open-meteo");
    Serial.printf("[Weather] %s\n", msg);
  }
  meteoHttp.end();
}

// Запрашивает актуальные времена восхода и заката с api.sunrise-sunset.org.
// Использует координаты из cfg (owmLat/owmLon) и tzOffset для конвертации UTC→local.
// Вызывается только из weatherTask.
void fetchSunriseSunset() {
  if (!wifiConnected) return;
  float lat, lon, tzF;
  xSemaphoreTake(cfgMtx, portMAX_DELAY);
  lat = cfg.owmLat;
  lon = cfg.owmLon;
  tzF = cfg.tzOffset;
  xSemaphoreGive(cfgMtx);
  int tzMins = (int)roundf(tzF * 60.0f); // полное UTC-смещение в минутах (±840 = ±14h)
  if (lat == 0.0f && lon == 0.0f) {
    // City-режим: координаты ещё не получены от OWM.
    // Запросим погоду — fetchWeather() кэширует coord.lat/lon и выставит sunFetchRequested снова.
    Serial.println("[Sun] Координаты не заданы — ждём fetchWeather()");
    weatherRequested.store(true);
    return;
  }
  Serial.printf("[Sun] Запрос: lat=%.4f lon=%.4f tz=%+.2g\n", lat, lon, tzF);
  WiFiClientSecure sc;
  sc.setInsecure(); // сертификат не проверяем — экономим RAM, API публичный
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  // Для ArduinoJson надёжнее получить небольшой JSON-ответ целиком:
  // потоковый разбор через HTTPClient::getStream() здесь иногда ловил
  // InvalidInput при HTTP 200.
  http.useHTTP10(true);
  // formatted=0 → времена в ISO8601 UTC: "2024-03-29T03:42:00+00:00"
  String url = "https://api.sunrise-sunset.org/json?lat=" +
               String(lat, 4) + "&lng=" + String(lon, 4) + "&formatted=0";
  Serial.printf("[Sun] URL: %s\n", url.c_str());
  http.begin(sc, url);
  int code = http.GET();
  Serial.printf("[Sun] HTTP код: %d\n", code);
  if (code == 200) {
    JsonDocument doc;
    String body = http.getString();
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
      Serial.printf("[Sun] JSON ошибка: %s\n", err.c_str());
      Serial.printf("[Sun] Тело: %s\n", body.c_str());
    } else {
      const char* rStr = doc["results"]["sunrise"] | "";
      const char* sStr = doc["results"]["sunset"]  | "";
      Serial.printf("[Sun] sunrise='%s' sunset='%s'\n", rStr, sStr);
      auto isoTimeDigitsOk = [](const char* s) {
        return isdigit((unsigned char)s[11]) && isdigit((unsigned char)s[12]) &&
               isdigit((unsigned char)s[14]) && isdigit((unsigned char)s[15]);
      };
      if (strlen(rStr) >= 16 && strlen(sStr) >= 16 &&
          isoTimeDigitsOk(rStr) && isoTimeDigitsOk(sStr)) {
        // Извлекаем UTC HH:MM из позиций 11-15 ISO-строки
        int rH = (rStr[11]-'0')*10 + (rStr[12]-'0');
        int rM = (rStr[14]-'0')*10 + (rStr[15]-'0');
        int sH = (sStr[11]-'0')*10 + (sStr[12]-'0');
        int sM = (sStr[14]-'0')*10 + (sStr[15]-'0');
        // UTC → local с учётом дробных часовых поясов (UTC+5:30, UTC+5:45 и т.д.)
        int rTot = rH * 60 + rM + tzMins;
        int sTot = sH * 60 + sM + tzMins;
        // Нормализуем в [0, 1439] до деления — C++ truncates toward zero,
        // поэтому -10/60 = 0 вместо -1, что даёт неверный час.
        rTot = ((rTot % 1440) + 1440) % 1440;
        sTot = ((sTot % 1440) + 1440) % 1440;
        rH = rTot / 60;  rM = rTot % 60;
        sH = sTot / 60;  sM = sTot % 60;
        sunTimes.store(((uint32_t)(uint8_t)rH << 24) | ((uint32_t)(uint8_t)rM << 16) |
                       ((uint32_t)(uint8_t)sH <<  8) |  (uint32_t)(uint8_t)sM);
        sunTimeValid.store(true);
        lastSunFetchMs.store(millis());
        brightnessRefreshRequested.store(true);
        Serial.printf("[Sun] Восход %02d:%02d  Закат %02d:%02d (local UTC%+.2g)\n",
                      rH, rM, sH, sM, tzF);
      } else {
        Serial.printf("[Sun] Неожиданный формат времени: rise='%s' set='%s'\n",
                      rStr, sStr);
      }
    }
  } else {
    Serial.printf("[Sun] Ошибка HTTP %d\n", code);
    if (code > 0) Serial.printf("[Sun] Тело: %s\n", http.getString().c_str());
  }
  http.end();
}

// ════════════════════════════════════════════════════════════
//  КОНФИГ: сохранение / загрузка
// ════════════════════════════════════════════════════════════
bool saveConfig() {
  // Снимок под cfgMtx — wifiConnTask может менять cfg во время NVS-записи
  StoredStateBlob blob;
  xSemaphoreTake(cfgMtx, portMAX_DELAY);
  blob.cfg = cfg;
  xSemaphoreGive(cfgMtx);
  xSemaphoreTake(bambuCfgMtx, portMAX_DELAY);
  blob.bambuCfg = bambuCfg;
  // Relay-only runtime: do not persist obsolete ESP-side Cloud credentials.
  blob.bambuCfg.email[0] = '\0';
  blob.bambuCfg.password[0] = '\0';
  blob.bambuCfg.relayEnabled = true;
  blob.bambuToken[0] = '\0';
  bambuCfg.email[0] = '\0';
  bambuCfg.password[0] = '\0';
  bambuCfg.relayEnabled = true;
  bambuManualToken[0] = '\0';
  bambuPrinterSerial[0] = '\0';
  xSemaphoreGive(bambuCfgMtx);
  blob.checksum = storedStateChecksum(blob);
  // NVS-запись под prefsMtx (занимает сотни мкс — мьютекс, не spinlock)
  xSemaphoreTake(prefsMtx, portMAX_DELAY);
  bool ok = prefs.begin("matrix", false);
  if (ok) {
    ok = prefs.putBytes("state", &blob, sizeof(blob)) == sizeof(blob);
    // Remove legacy split keys from older builds; failures here are non-fatal.
    prefs.remove("bserial");
    prefs.remove("btok");
    prefs.end();
  } else {
    Serial.println("[NVS] prefs.begin(write) failed");
  }
  xSemaphoreGive(prefsMtx);
  saveError.store(!ok);
  if (!ok) Serial.println("[NVS] saveConfig failed");
  return ok;
}
void loadConfig() {
  // cfgMtx: симметрично с saveConfig() — защищает запись cfg на случай
  // если loadConfig() когда-либо вызовут после запуска задач
  xSemaphoreTake(cfgMtx,   portMAX_DELAY);
  xSemaphoreTake(bambuCfgMtx, portMAX_DELAY);
  xSemaphoreTake(prefsMtx, portMAX_DELAY);
  if (prefs.begin("matrix", true)) {
    StoredStateBlob blob;
    if (loadStoredStateBlob(blob)) {
      cfg = blob.cfg;
      sanitizeLoadedConfig(cfg);
      bambuCfg = blob.bambuCfg;
      strlcpy(bambuManualToken, blob.bambuToken, sizeof(bambuManualToken));
      if (prefs.isKey("bserial")) {
        size_t got = prefs.getBytes("bserial", &bambuPrinterSerial, sizeof(bambuPrinterSerial));
        if (got != sizeof(bambuPrinterSerial)) bambuPrinterSerial[0] = '\0';
      } else {
        bambuPrinterSerial[0] = '\0';
      }
    } else {
      uint16_t ver = prefs.getUShort("ver", 0);
      uint16_t csz = prefs.getUShort("csz", 0);
      if (loadConfigFromNvs(ver, csz, cfg)) {
        sanitizeLoadedConfig(cfg);
      }
      uint16_t bver = prefs.getUShort("bver", 0);
      uint16_t bsz = prefs.getUShort("bcsz", 0);
      if (bver == 2 && bsz == (uint16_t)sizeof(bambuCfg) && prefs.isKey("bcfg")) {
        size_t got = prefs.getBytes("bcfg", &bambuCfg, sizeof(bambuCfg));
        if (got != sizeof(bambuCfg)) {
          Serial.printf("[NVS] getBytes(bcfg) size mismatch: %u\n", (unsigned)got);
        }
      }
      uint16_t btver = prefs.getUShort("btver", 0);
      uint16_t btsz = prefs.getUShort("btsz", 0);
      if (btver == 1 && btsz == (uint16_t)sizeof(bambuManualToken) && prefs.isKey("btok")) {
        size_t got = prefs.getBytes("btok", &bambuManualToken, sizeof(bambuManualToken));
        if (got != sizeof(bambuManualToken)) {
          Serial.printf("[NVS] getBytes(btok) size mismatch: %u\n", (unsigned)got);
          bambuManualToken[0] = '\0';
        }
      } else {
        bambuManualToken[0] = '\0';
      }
      if (prefs.isKey("bserial")) {
        size_t got = prefs.getBytes("bserial", &bambuPrinterSerial, sizeof(bambuPrinterSerial));
        if (got != sizeof(bambuPrinterSerial)) bambuPrinterSerial[0] = '\0';
      } else {
        bambuPrinterSerial[0] = '\0';
      }
    }
    if (!prefs.isKey("state") && !prefs.isKey("cfg")) {
      bambuManualToken[0] = '\0';
      bambuPrinterSerial[0] = '\0';
    }
    if (prefs.isKey(PREF_LAST_AUTO_BRIGHTNESS)) {
      uint8_t remembered = prefs.getUChar(PREF_LAST_AUTO_BRIGHTNESS, cfg.brightness);
      remembered = (uint8_t)constrain((int)remembered, (int)MIN_UI_BRIGHTNESS, 255);
      lastAutoBrightness.store(remembered);
      lastAutoBrightnessValid.store(true);
      Serial.printf("[Brightness] Last auto brightness=%u\n", remembered);
    } else {
      lastAutoBrightnessValid.store(false);
    }
    // Relay-only runtime: migrate away from obsolete ESP-side Cloud fields.
    bambuCfg.relayEnabled = true;
    bambuCfg.email[0] = '\0';
    bambuCfg.password[0] = '\0';
    bambuManualToken[0] = '\0';
    bambuPrinterSerial[0] = '\0';
    prefs.end();
  } else {
    Serial.println("[NVS] prefs.begin(read) failed, using defaults");
    lastAutoBrightnessValid.store(false);
    bambuCfg.relayEnabled = true;
    bambuCfg.email[0] = '\0';
    bambuCfg.password[0] = '\0';
    bambuManualToken[0] = '\0';
    bambuPrinterSerial[0] = '\0';
  }
  xSemaphoreGive(prefsMtx);
  xSemaphoreGive(bambuCfgMtx);
  xSemaphoreGive(cfgMtx);

  BambuState st;
  st.enabled = bambuCfg.enabled;
  st.configured = bambuCfg.relayUrl[0] != '\0';
  st.connected = false;
  st.printerOnline = false;
  st.lifecycle = BAMBU_UNKNOWN;
  st.displayLifecycle = BAMBU_UNKNOWN;
  setBambuDetail(st, bambuCfg.enabled ? (st.configured ? "Bambu relay ждёт WiFi" : "Заполните Relay URL")
                                      : "Bambu выключен");
  setBambuStateSnapshot(st);
}

// ════════════════════════════════════════════════════════════
//  ВСТРОЕННЫЙ ВЕБ-ИНТЕРФЕЙС
// ════════════════════════════════════════════════════════════
#include "index_html.h"

// ════════════════════════════════════════════════════════════
//  ВЕБ-СЕРВЕР: обработчики
// ════════════════════════════════════════════════════════════
// Веб-аутентификация отключена по требованию пользователя.
bool checkAuth() {
  return true;
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleGetSettings() {
  if (!checkAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  JsonDocument doc;
  xSemaphoreTake(cfgMtx, portMAX_DELAY);
  doc["brightness"]   = cfg.brightness;
  doc["autoColor"]    = cfg.autoColor;
  doc["use24h"]       = cfg.use24h;
  doc["ntpServer"]    = cfg.ntpServer;
  doc["tzOffset"]     = cfg.tzOffset;
  doc["tzFromWeather"] = cfg.tzFromWeather;
  doc["updateMin"]    = cfg.updateMin;
  doc["weatherProvider"] = cfg.weatherProvider;
  doc["apiKeySet"]    = cfg.apiKey[0] != '\0';
  doc["apiKey"]       = cfg.apiKey[0] ? "***" : "";  // не отдаём ключ открытым текстом
  doc["city"]         = cfg.city;
  doc["owmLat"]       = cfg.owmLat;
  doc["owmLon"]       = cfg.owmLon;
  doc["owmUseCoords"] = cfg.owmUseCoords;
  doc["rotation"]         = cfg.rotation;
  doc["flipH"]            = cfg.flipH;
  doc["flipV"]            = cfg.flipV;
  doc["autoBrightness"]   = cfg.autoBrightness;
  doc["brightnessDay"]    = cfg.brightnessDay;
  doc["brightnessNight"]  = cfg.brightnessNight;
  doc["dayStartHour"]     = cfg.dayStartHour;
  doc["nightStartHour"]   = cfg.nightStartHour;
  doc["autoBrSunMode"]    = cfg.autoBrSunMode;
  doc["ct6ClockX"]  = cfg.ct6ClockX;
  doc["ct6ClockY"]  = cfg.ct6ClockY;
  doc["ct6TempX"]   = cfg.ct6TempX;
  doc["ct6TempY"]   = cfg.ct6TempY;
  doc["ct6ClockR"]  = cfg.ct6ClockR; doc["ct6ClockG"] = cfg.ct6ClockG; doc["ct6ClockB"] = cfg.ct6ClockB;
  doc["ct6TempR"]   = cfg.ct6TempR;  doc["ct6TempG"]  = cfg.ct6TempG;  doc["ct6TempB"]  = cfg.ct6TempB;
  xSemaphoreGive(cfgMtx);
  xSemaphoreTake(bambuCfgMtx, portMAX_DELAY);
  doc["bambuEnabled"]      = bambuCfg.enabled;
  doc["bambuRelayEnabled"] = true; // legacy web compatibility; runtime is relay-only
  doc["bambuRelayUrl"]     = bambuCfg.relayUrl;
  doc["bambuRelayKeySet"]  = bambuCfg.relayKey[0] != '\0';
  xSemaphoreGive(bambuCfgMtx);
  { BambuState st = getBambuStateSnapshot();
    doc["bambuConfigured"] = st.configured;
    doc["bambuConnected"]  = st.connected;
    doc["bambuPrinterOnline"] = st.printerOnline;
    doc["bambuLifecycle"]  = st.lifecycle;
    doc["bambuProgress"]   = st.progress;
    doc["bambuDetail"]     = st.detail; }
  // Данные восхода/заката хранятся в RAM (не в cfg) — читаем вне мьютекса
  doc["sunTimeValid"]  = sunTimeValid.load();
  { uint32_t st = sunTimes.load();
    doc["sunriseH"] = (int)((st >> 24) & 0xFF);
    doc["sunriseM"] = (int)((st >> 16) & 0xFF);
    doc["sunsetH"]  = (int)((st >>  8) & 0xFF);
    doc["sunsetM"]  = (int)( st        & 0xFF); }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handlePostSettings() {
  if (!checkAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  if (!server.hasArg("plain")) { server.send(400); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }

  auto setStr = [&](const char* key, char* dst, size_t sz) {
    if (doc[key].is<const char*>()) strlcpy(dst, doc[key].as<const char*>(), sz);
  };
  // API-ключи: обновляем всегда, кроме "***" (маска из GET-ответа).
  // Пустая строка разрешена — это удаление ключа.
  auto setKey = [&](const char* key, char* dst, size_t sz) {
    if (doc[key].is<const char*>()) {
      const char* v = doc[key].as<const char*>();
      if (strcmp(v, "***") != 0) strlcpy(dst, v, sz);
    }
  };
  // cfgMtx: cfg читается из background tasks (fetchWeather, wifiConnTask) — защищаем все записи.
  // Используем мьютекс, а не spinlock: блок длинный — spinlock вызвал бы Interrupt WDT reset.
  Config prevCfg;
  xSemaphoreTake(cfgMtx, portMAX_DELAY);
  prevCfg = cfg;
  setStr("ntpServer", cfg.ntpServer, sizeof(cfg.ntpServer));
  setStr("city",      cfg.city,      sizeof(cfg.city));
  setKey("apiKey",    cfg.apiKey,    sizeof(cfg.apiKey));
  if (!doc["brightness"].isNull())    cfg.brightness    = (uint8_t)constrain((int)doc["brightness"], (int)MIN_UI_BRIGHTNESS, 255);
  if (!doc["autoColor"].isNull())     cfg.autoColor     = doc["autoColor"];
  if (!doc["use24h"].isNull())        cfg.use24h        = doc["use24h"];
  if (!doc["tzOffset"].isNull())      cfg.tzOffset      = constrain((float)doc["tzOffset"], -12.0f, 14.0f);
  if (!doc["tzFromWeather"].isNull()) cfg.tzFromWeather = doc["tzFromWeather"];
  if (!doc["updateMin"].isNull())     cfg.updateMin     = (uint16_t)constrain((int)doc["updateMin"], 1, 60);
  if (!doc["weatherProvider"].isNull()) cfg.weatherProvider = (uint8_t)constrain((int)doc["weatherProvider"], 0, 2);
  if (!doc["owmLat"].isNull())        cfg.owmLat        = doc["owmLat"];
  if (!doc["owmLon"].isNull())        cfg.owmLon        = doc["owmLon"];
  if (!doc["owmUseCoords"].isNull())  cfg.owmUseCoords  = doc["owmUseCoords"];
  if (!doc["rotation"].isNull())      cfg.rotation      = (uint8_t)constrain((int)doc["rotation"], 0, 3);
  if (!doc["flipH"].isNull())              cfg.flipH           = doc["flipH"];
  if (!doc["flipV"].isNull())              cfg.flipV           = doc["flipV"];
  if (!doc["autoBrightness"].isNull())     cfg.autoBrightness  = doc["autoBrightness"];
  if (!doc["brightnessDay"].isNull())      cfg.brightnessDay   = (uint8_t)constrain((int)doc["brightnessDay"],   (int)MIN_UI_BRIGHTNESS, 255);
  if (!doc["brightnessNight"].isNull())    cfg.brightnessNight = (uint8_t)constrain((int)doc["brightnessNight"], (int)MIN_UI_BRIGHTNESS, 255);
  if (!doc["dayStartHour"].isNull())       cfg.dayStartHour    = (uint8_t)constrain((int)doc["dayStartHour"],    0, 23);
  if (!doc["nightStartHour"].isNull())     cfg.nightStartHour  = (uint8_t)constrain((int)doc["nightStartHour"],  0, 23);
  if (!doc["autoBrSunMode"].isNull())      cfg.autoBrSunMode   = doc["autoBrSunMode"];
  if (!doc["ct6ClockX"].isNull()) cfg.ct6ClockX = (int8_t)constrain((int)doc["ct6ClockX"], 0, 1);
  if (!doc["ct6ClockY"].isNull()) cfg.ct6ClockY = (int8_t)constrain((int)doc["ct6ClockY"], 0, 11);
  if (!doc["ct6TempX"].isNull())  cfg.ct6TempX  = (int8_t)constrain((int)doc["ct6TempX"],  -1, 15);
  if (!doc["ct6TempY"].isNull())  cfg.ct6TempY  = (int8_t)constrain((int)doc["ct6TempY"],  0, 11);
  if (!doc["ct6ClockR"].isNull()) cfg.ct6ClockR = doc["ct6ClockR"];
  if (!doc["ct6ClockG"].isNull()) cfg.ct6ClockG = doc["ct6ClockG"];
  if (!doc["ct6ClockB"].isNull()) cfg.ct6ClockB = doc["ct6ClockB"];
  if (!doc["ct6TempR"].isNull())  cfg.ct6TempR  = doc["ct6TempR"];
  if (!doc["ct6TempG"].isNull())  cfg.ct6TempG  = doc["ct6TempG"];
  if (!doc["ct6TempB"].isNull())  cfg.ct6TempB  = doc["ct6TempB"];
  cfg.fontIndex = 0;
  cfg.blinkColon = false;
  cfg.ct6ShowWeatherIcon = false;
  cfg.showDegree = false;
  cfg.clockFace = 0;
  cfg.mode = 6;
  cfg.showColon = false;  // разделитель часов удалён
  cfg.colonBright = 90;   // переиспользуется как яркость подложки оверлея
  cfg.webPass[0] = '\0';
  strlcpy(cfg.apPass, LEGACY_DEFAULT_AP_PASS, sizeof(cfg.apPass));
  // Снимаем значения для использования вне мьютекса — cfg нельзя читать после Give
  uint8_t snapBr      = cfg.brightness;
  bool    snapAutoB   = cfg.autoBrightness;
  bool    snapSunMode = cfg.autoBrSunMode;
  bool cfgChanged = !configEquals(prevCfg, cfg);
  bool ntpChanged = (prevCfg.tzOffset != cfg.tzOffset) ||
                    (strncmp(prevCfg.ntpServer, cfg.ntpServer, sizeof(cfg.ntpServer)) != 0);
  bool weatherCfgChanged =
      (prevCfg.weatherProvider != cfg.weatherProvider) ||
      (prevCfg.tzFromWeather != cfg.tzFromWeather && cfg.tzFromWeather);
  xSemaphoreGive(cfgMtx);
  BambuConfig prevBambuCfg;
  xSemaphoreTake(bambuCfgMtx, portMAX_DELAY);
  prevBambuCfg = bambuCfg;
  if (!doc["bambuEnabled"].isNull()) bambuCfg.enabled = doc["bambuEnabled"];
  bambuCfg.relayEnabled = true;
  bambuCfg.email[0] = '\0';
  bambuCfg.password[0] = '\0';
  setStr("bambuRelayUrl", bambuCfg.relayUrl, sizeof(bambuCfg.relayUrl));
  setKey("bambuRelayKey", bambuCfg.relayKey, sizeof(bambuCfg.relayKey));
  bambuManualToken[0] = '\0';
  bambuPrinterSerial[0] = '\0';
  bool bambuChanged = !bambuConfigEquals(prevBambuCfg, bambuCfg);
  xSemaphoreGive(bambuCfgMtx);

  // Применяем яркость немедленно; если включили sun-режим — запрашиваем данные
  if (snapAutoB) {
    if (!applyConfiguredBrightnessNow(false)) brightnessRefreshRequested.store(true);
  } else if (!doc["brightness"].isNull()) {
    applyBrightness(snapBr);
  }
  brightnessRefreshRequested.store(true);
  // Запросить восход/закат если данные устарели или отсутствуют
  if (snapSunMode && !sunTimeValid.load()) sunFetchRequested.store(true);
  if (ntpChanged) ntpPending.store(true); // configTime вызовет loop task — не дублируем вызов
  if (weatherCfgChanged) weatherRequested = true; // сменили провайдера/tz-режим — обновим погоду сразу
  if (bambuChanged) {
    Serial.printf("[BambuRelay] Settings updated: changed=%d\n", (int)bambuChanged);
    wakeBambuTask();
  }
  bool persistPending = cfgChanged || bambuChanged;
  if (persistPending) scheduleDeferredSave(1500);
  JsonDocument resp;
  resp["ok"] = true;
  resp["persistPending"] = persistPending;
  String out;
  serializeJson(resp, out);
  server.send(200, "application/json", out);
}

void handleStatus() {
  if (!checkAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  JsonDocument doc;
  doc["wifi"]   = wifiConnected.load();
  doc["ap"]     = apMode.load();
  // cfg.ssid пишется wifiConnTask под cfgMtx — читаем тоже под ним
  { char ssid[sizeof(cfg.ssid)];
    xSemaphoreTake(cfgMtx, portMAX_DELAY);
    strlcpy(ssid, cfg.ssid, sizeof(ssid));
    xSemaphoreGive(cfgMtx);
    doc["ssid"] = ssid; }
  doc["ip"]     = wifiConnected.load() ? WiFi.localIP().toString() : "192.168.4.1";
  doc["apIP"]   = "192.168.4.1";
  doc["cpuTemp"]    = temperatureRead(); // встроенный датчик температуры ESP32 (°C)
  doc["temp"]       = safeGetTemp();
  {
    uint32_t nowMs = millis();
    uint32_t weatherMs;
    uint32_t weatherAttemptMs;
    int weatherCode;
    uint8_t weatherKeyLen;
    char weatherError[sizeof(lastWeatherError)];
    char weatherSource[sizeof(lastWeatherSource)];
    taskENTER_CRITICAL(&weatherMux);
    weatherMs = lastWeatherMs;
    weatherAttemptMs = lastWeatherAttemptMs;
    weatherCode = lastWeatherHttpCode;
    weatherKeyLen = lastWeatherKeyLen;
    strlcpy(weatherError, lastWeatherError, sizeof(weatherError));
    strlcpy(weatherSource, lastWeatherSource, sizeof(weatherSource));
    taskEXIT_CRITICAL(&weatherMux);
    doc["weatherCode"] = weatherCode;
    doc["weatherError"] = weatherError;
    doc["weatherSource"] = weatherSource;
    doc["weatherKeyLen"] = weatherKeyLen;
    doc["weatherAgeSec"] = weatherMs ? (int32_t)((nowMs - weatherMs) / 1000UL) : -1;
    doc["weatherAttemptAgeSec"] = weatherAttemptMs ? (int32_t)((nowMs - weatherAttemptMs) / 1000UL) : -1;
  }
  doc["uptime"]     = String(millis() / 1000 / 3600, DEC) + "ч " +
                      String((millis() / 1000 % 3600) / 60, DEC) + "м";
  doc["fwVersion"]  = FW_VERSION;
  doc["fwPartition"] = esp_ota_get_running_partition()->label; // app0/app1 — виден фактический слот
  doc["heap"]       = String(ESP.getFreeHeap() / 1024) + " KB";
  doc["rssi"]       = wifiConnected.load() ? String(WiFi.RSSI()) + " dBm" : "—";
  doc["connResult"] = wifiConnResult.load();
  doc["connIp"]     = (wifiConnResult.load() == 2) ? WiFi.localIP().toString() : String("");
  doc["connStatus"] = wifiConnFailStatus.load();
  switch (wifiConnFailStatus.load()) {
    case WL_NO_SSID_AVAIL:  doc["connStatusText"] = "сеть не найдена"; break;
    case WL_CONNECT_FAILED: doc["connStatusText"] = "ошибка подключения"; break;
    case WL_CONNECTION_LOST:doc["connStatusText"] = "соединение потеряно"; break;
    case WL_DISCONNECTED:   doc["connStatusText"] = "соединение разорвано"; break;
    default:                doc["connStatusText"] = ""; break;
  }
  doc["savePending"] = savePending.load();
  doc["saveError"]   = saveError.load();
  { BambuState st = getBambuStateSnapshot();
    doc["bambuEnabled"]    = st.enabled;
    doc["bambuConfigured"] = st.configured;
    doc["bambuConnected"]  = st.connected;
    doc["bambuPrinterOnline"] = st.printerOnline;
    doc["bambuLifecycle"]  = st.lifecycle;
    doc["bambuProgress"]   = st.progress;
    doc["bambuDetail"]     = st.detail; }
  // Данные восхода/заката — нужны для авто-обновления sunStatusTxt каждые 3 сек
  doc["sunTimeValid"] = sunTimeValid.load();
  { uint32_t snt = sunTimes.load();
    doc["sunriseH"] = (int)((snt >> 24) & 0xFF);
    doc["sunriseM"] = (int)((snt >> 16) & 0xFF);
    doc["sunsetH"]  = (int)((snt >>  8) & 0xFF);
    doc["sunsetM"]  = (int)( snt        & 0xFF); }

  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// Сканирование WiFi: двухфазный async без блокировки loop().
// Первый вызов: запускает сканирование, возвращает {"scanning":true}.
// Второй вызов: возвращает результаты (браузер повторяет запрос каждые 1 сек).
void handleScanWifi() {
  if (!checkAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  // Нельзя запускать сканирование во время попытки подключения — конфликт WiFi-драйвера
  if (wifiConnInProgress.load()) {
    server.send(503, "application/json", "{\"error\":\"connection in progress\"}");
    return;
  }
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) {
    // Предыдущий результат устарел или сканирование ещё не запускалось — стартуем заново
    WiFi.scanDelete();
    WiFi.scanNetworks(true); // async, не блокирует
    server.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  if (n == WIFI_SCAN_RUNNING) {
    // Сканирование в процессе — браузер подождёт ещё секунду
    server.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  // n >= 0: результаты готовы
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

// Подключение к сети — немедленно возвращает ответ браузеру.
// Фактическое подключение выполняет wifiConnTask.
// Браузер получает результат через /api/status polling.
void handleConnectWifi() {
  if (!checkAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  if (!server.hasArg("plain")) { server.send(400); return; }
  if (wifiConnPending.load() || wifiConnInProgress.load()) {
    server.send(409, "application/json", "{\"error\":\"connection in progress\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  const char* ssid = doc["ssid"] | "";
  const char* pass = doc["password"] | "";
  if (strlen(ssid) == 0) {
    server.send(400, "application/json", "{\"error\":\"empty ssid\"}");
    return;
  }
  taskENTER_CRITICAL(&wifiConnMux);
  strlcpy(wifiPendSsid, ssid, sizeof(wifiPendSsid));
  strlcpy(wifiPendPass, pass, sizeof(wifiPendPass));
  taskEXIT_CRITICAL(&wifiConnMux);
  wifiConnFailStatus.store(0);
  wifiConnResult.store(1);      // сразу показываем "connecting" в браузере
  wifiConnPending.store(true);  // будит wifiConnTask
  server.send(200, "application/json", "{\"ok\":true,\"pending\":true}");
}


void handleDisconnectWifi() {
  if (!checkAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  char prevSsid[sizeof(cfg.ssid)];
  char prevPass[sizeof(cfg.wifiPass)];
  bool prevConnected = wifiConnected.load();
  xSemaphoreTake(cfgMtx, portMAX_DELAY);
  strlcpy(prevSsid, cfg.ssid, sizeof(prevSsid));
  strlcpy(prevPass, cfg.wifiPass, sizeof(prevPass));
  cfg.ssid[0] = '\0';
  cfg.wifiPass[0] = '\0';
  xSemaphoreGive(cfgMtx);
  hasSsid.store(false);
  wifiConnected.store(false);
  wifiConnResult.store(0);
  wifiConnFailStatus.store(0);
  if (!saveConfig()) {
    xSemaphoreTake(cfgMtx, portMAX_DELAY);
    strlcpy(cfg.ssid, prevSsid, sizeof(cfg.ssid));
    strlcpy(cfg.wifiPass, prevPass, sizeof(cfg.wifiPass));
    xSemaphoreGive(cfgMtx);
    hasSsid.store(prevSsid[0] != '\0');
    wifiConnected.store(prevConnected);
    Serial.println("[NVS] Failed to persist WiFi disconnect");
    server.send(500, "application/json", "{\"error\":\"save_failed\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
  rebootMs.store(millis()); rebootPending.store(true);
}


void handleReboot() {
  if (!checkAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  if (!flushDeferredSave(true)) {
    server.send(500, "application/json", "{\"error\":\"save_failed\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
  rebootMs.store(millis()); rebootPending.store(true);
}

void handleWeatherRefresh() {
  if (!checkAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  weatherRequested = true;
  wakeBambuTask();
  server.send(200, "application/json", "{\"ok\":true}");
}

// ════════════════════════════════════════════════════════════
//  OTA: веб-загрузка /api/update + ArduinoOTA (espota)
//  Образ пишется в неактивный слот; otadata переключается атомарно
//  в Update.end() — до этого момента обрыв безвреден.
// ════════════════════════════════════════════════════════════
// Прогресс OTA на матрице: янтарный столб растёт снизу вверх (в тон boot-
// анимации), дробная часть строки показывается яркостью. pct > 100 = размер
// неизвестен — дышит нижняя строка. Перерисовка только при смене процента и
// не чаще ~120 мс: FastLED.show() на 256 LED ≈ 8 мс, не тормозим приём.
static void showOtaProgress(uint8_t pct) {
  static uint32_t lastMs = 0;
  static uint8_t lastPct = 254;
  uint32_t now = millis();
  if (pct <= 100) {                     // полоса: перерисовка на смене процента
    if (pct == lastPct) return;
  } else if (now - lastMs < 120) {      // «дыхание»: по таймеру
    return;
  }
  lastMs = now; lastPct = pct;
  clearMatrix();
  if (pct > 100) {
    for (int x = 0; x < MW; x++) px(x, MH - 1, CHSV(24, 255, beatsin8(30, 30, 160)));
  } else {
    uint16_t sub = (uint16_t)pct * MH * 255 / 100; // высота столба в 1/255 строки
    int full = sub / 255;
    uint8_t frac = sub % 255;
    for (int fromBottom = 0; fromBottom <= full && fromBottom < MH; fromBottom++) {
      uint8_t v = (fromBottom < full) ? 200 : scale8(200, frac);
      if (!v) continue;
      uint8_t hue = 16 + fromBottom * 2;  // снизу оранжевый, к верху желтее
      for (int x = 0; x < MW; x++) px(x, MH - 1 - fromBottom, CHSV(hue, 255, v));
    }
  }
  FastLED.show();
}

// Образ принят, ждём перезагрузку: ровная зелёная заливка до ресета.
static void showOtaDone() {
  fill_solid(leds, NUM_LEDS, CRGB(0, 150, 50));
  FastLED.show();
}

// Ошибка/обрыв OTA: красный крест на полсекунды.
static void showOtaError() {
  clearMatrix();
  for (int i = 0; i < MW; i++) {
    px(i, i, CRGB(180, 20, 20));
    px(MW - 1 - i, i, CRGB(180, 20, 20));
  }
  FastLED.show(); delay(500);
  clearMatrix(); FastLED.show();
}

static void otaEnterUpdateMode() {
  otaInProgress.store(true);
  clearMatrix(); FastLED.show();        // прежний кадр гасим; дальше рисуем прогресс
  esp_wifi_set_ps(WIFI_PS_NONE);        // power save рвёт приём при flash-операциях
}

static void otaAbortUpdateMode() {
  otaInProgress.store(false);
  showOtaError();
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   // steady state и в STA, и в AP
}

// Upload-callback: вызывается из server.handleClient() порциями ~1.4 КБ.
// Пока идёт приём, loop() заблокирован внутри handleClient — рендер
// останавливается сам, дополнительной синхронизации не нужно.
void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START: {
      otaEnterUpdateMode();
      flushDeferredSave(true);          // дописать отложенный конфиг до перешивки
      // Точный размер приходит query-параметром (?size=N от веб-UI): Update.begin
      // проверит влезание в слот до первой записи. Без параметра — UNKNOWN.
      size_t expected = UPDATE_SIZE_UNKNOWN;
      if (server.hasArg("size")) {
        long sz = server.arg("size").toInt();
        if (sz > 0) expected = (size_t)sz;
      }
      Serial.printf("[OTA] Web upload: %s (%u bytes)\n",
                    up.filename.c_str(), (unsigned)(expected == UPDATE_SIZE_UNKNOWN ? 0 : expected));
      if (!Update.begin(expected)) Update.printError(Serial);
      break;
    }
    case UPLOAD_FILE_WRITE: {
      if (!Update.isRunning()) break;
      if (up.totalSize == 0 && up.currentSize >= 0x24) {
        // Первый чанк: отличаем app-образ от bootloader/merged.bin — оба
        // начинаются с 0xE9, но esp_app_desc_t (magic 0xABCD5432 по смещению
        // 0x20) есть только у приложения. merged.bin влезает в слот и без этой
        // проверки прошёл бы Update до битого слота.
        uint32_t descMagic = 0;
        memcpy(&descMagic, up.buf + 0x20, sizeof(descMagic));
        if (up.buf[0] != 0xE9 || descMagic != ESP_APP_DESC_MAGIC_WORD) {
          Serial.println("[OTA] Отклонено: не app-образ (загружайте .ino.bin, не merged.bin)");
          Update.abort();
          break;
        }
      }
      if (Update.write(up.buf, up.currentSize) != up.currentSize)
        Update.printError(Serial);      // ошибка запомнена в Update, отдадим в final
      else {
        size_t total = Update.size();
        showOtaProgress((total && total != UPDATE_SIZE_UNKNOWN)
          ? (uint8_t)((uint64_t)Update.progress() * 100 / total) : (uint8_t)255);
      }
      break;
    }
    case UPLOAD_FILE_END:
      if (Update.isRunning() && Update.end(true)) { // true: завершить и при UNKNOWN size
        Serial.printf("[OTA] Образ принят: %u байт\n", up.totalSize);
        showOtaDone();                  // зелёная заливка висит до перезагрузки
      } else
        Update.printError(Serial);
      break;
    case UPLOAD_FILE_ABORTED:
      Update.abort();
      otaAbortUpdateMode();
      Serial.println("[OTA] Upload прерван клиентом");
      break;
    default: break;
  }
}

void handleUpdateFinal() {
  if (!checkAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
  if (Update.hasError() || !Update.isFinished()) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = Update.errorString();
    String out; serializeJson(doc, out);
    Update.abort();
    otaAbortUpdateMode();
    server.send(500, "application/json", out);
    return;
  }
  // Новая прошивка не должна унаследовать счётчик, если OTA прошло в первые 60 с
  xSemaphoreTake(prefsMtx, portMAX_DELAY);
  bootGuardReset();
  xSemaphoreGive(prefsMtx);
  server.send(200, "application/json", "{\"ok\":true}");
  rebootMs.store(millis()); rebootPending.store(true); // ответ успевает уйти клиенту
}

// ════════════════════════════════════════════════════════════
//  BOOT-АНИМАЦИЯ И СТАТУС
// ════════════════════════════════════════════════════════════
void showBoot() {
  // «Зажигание»: янтарное кольцо расходится из центра, оставляя затухающий
  // след, к краям уходит в жёлтый; в конце матрица мягко гаснет.
  for (int f = 0; f < 26; f++) {
    fadeToBlackBy(leds, NUM_LEDS, 60);
    float r = f * 0.55f;
    for (int y = 0; y < MH; y++)
      for (int x = 0; x < MW; x++) {
        float d = hypotf(x - 7.5f, y - 7.5f);
        float t = fabsf(d - r);
        if (t < 1.3f)
          px(x, y, CHSV(24 + (uint8_t)(d * 5), 255, (uint8_t)(255 * (1.0f - t / 1.3f))));
      }
    FastLED.show(); delay(30);
  }
  for (int f = 0; f < 10; f++) { fadeToBlackBy(leds, NUM_LEDS, 70); FastLED.show(); delay(25); }
  clearMatrix(); FastLED.show();
}

void showConnecting() {
  // Значок Wi-Fi: точка-источник внизу и три дуги, загорающиеся снизу вверх
  // (шаг каждые ~320 мс при вызове раз в 80 мс), затем пауза с тусклой точкой.
  static uint8_t frame = 0;
  const uint8_t step = (frame / 4) % 5; // 0=точка, 1..3=дуги, 4=пауза
  frame++;
  clearMatrix();
  const float cx = 7.5f, cy = 13.5f;
  const uint8_t dotV = (step == 4) ? 60 : 220;
  px(7, 12, CHSV(150, 220, dotV)); px(8, 12, CHSV(150, 220, dotV));
  px(7, 13, CHSV(150, 220, dotV)); px(8, 13, CHSV(150, 220, dotV));
  static const float R[3] = {3.6f, 6.1f, 8.6f};
  if (step >= 1 && step <= 3) {
    for (int y = 0; y < MH; y++)
      for (int x = 0; x < MW; x++) {
        float dx = x - cx, dy = cy - y;
        if (dy < 0 || dy < fabsf(dx)) continue; // конус 90° вверх от точки
        float d = hypotf(dx, dy);
        for (int a = 0; a < step; a++) {
          float t = fabsf(d - R[a]);
          if (t < 0.9f) px(x, y, CHSV(150, 220, (uint8_t)(220 * (1.0f - t / 0.9f))));
        }
      }
  }
  FastLED.show();
}

static void configureStaNetworkPolicy() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3 && CONFIG_LWIP_IPV6
  bool ok = WiFi.enableIPv6(false);
  Serial.printf("[WiFi] STA IPv6 disable request: %s\n", ok ? "ok" : "failed");
#elif defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR < 3
  Serial.println("[WiFi] STA IPv6 disable unavailable in Arduino-ESP32 2.x");
#else
  Serial.println("[WiFi] STA IPv6 not available in this build");
#endif
}

// Старт AP — вызывать ТОЛЬКО когда WiFi уже в WIFI_OFF или WIFI_AP
static void startAP() {
  WiFi.mode(WIFI_AP);
  delay(200);
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_NAME, LEGACY_DEFAULT_AP_PASS, 6, 0, 4);
  delay(300);
  // В AP fallback не нужен постоянно бодрствующий радиомодуль: это только греет кристалл.
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  apMode.store(true);
  Serial.printf("[AP] Запущена: %s → %s  (ch6, ps=min)\n",
    AP_NAME, WiFi.softAPIP().toString().c_str());
}

// ════════════════════════════════════════════════════════════
//  АСИНХРОННОЕ ПОДКЛЮЧЕНИЕ К WiFi (FreeRTOS task)
//  handleConnectWifi() выставляет wifiConnPending и сразу отвечает браузеру.
//  Этот task выполняет фактический connect и пишет результат в wifiConnResult.
//  Восстановление AP при ошибке — через флаг wifiNeedApRestart → loop task.
// ════════════════════════════════════════════════════════════
void wifiConnTask(void* param) {
  for (;;) {
    if (wifiConnPending.load()) {
      wifiConnPending.store(false);

      char ssid[40], pass[64];
      taskENTER_CRITICAL(&wifiConnMux);
      strlcpy(ssid, wifiPendSsid, sizeof(ssid));
      strlcpy(pass, wifiPendPass, sizeof(pass));
      taskEXIT_CRITICAL(&wifiConnMux);

      Serial.printf("[WiFi] Подключаемся к '%s' (async)...\n", ssid);

      // WiFi API не потокобезопасен — просим loop task вызвать WiFi.mode/begin
      wifiDoConnect.store(true);
      // Ждём, пока loop() вызовет WiFi.begin и выставит wifiConnInProgress (таймаут 5 сек)
      {
        uint32_t t0 = millis();
        bool timedOut = false;
        while (!wifiConnInProgress.load()) {
          if (millis() - t0 > 5000) { timedOut = true; break; }
          vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        if (timedOut) {
          wifiDoConnect.store(false);
          wifiConnResult.store(3); // fail
          wifiNeedApRestart.store(true);
          Serial.println("[WiFi] Таймаут ожидания loop() → сброс");
          vTaskDelay(100 / portTICK_PERIOD_MS);
          continue;
        }
      }

      // Ждём результата от loop task — прямой вызов WiFi.status() из wifiConnTask небезопасен.
      { uint32_t t0 = millis();
        while (!wifiConnDone.load() && millis() - t0 < 20000)
          vTaskDelay(50 / portTICK_PERIOD_MS);
      }

      if (wifiConnOk.load()) {
        bool snapSunMode;
        xSemaphoreTake(cfgMtx, portMAX_DELAY);
        strlcpy(cfg.ssid,     ssid, sizeof(cfg.ssid));
        strlcpy(cfg.wifiPass, pass, sizeof(cfg.wifiPass));
        snapSunMode = cfg.autoBrSunMode; // снимаем под мьютексом — см. ниже
        xSemaphoreGive(cfgMtx);
        hasSsid.store(true);
        scheduleDeferredSave(); // NVS-запись делегируем loop task
        wifiConnected.store(true);
        apMode.store(true);
        apOffMs.store(0); // loop() начнёт 60-сек отсчёт заново
        ntpPending.store(true); // loop task вызовет configTime
        weatherRequested.store(true);
        wakeBambuTask();
        if (snapSunMode) sunFetchRequested.store(true);
        wifiConnInProgress.store(false); // сбрасываем для корректной повторной попытки
        wifiConnFailStatus.store(0);
        wifiConnResult.store(2); // ok
        Serial.println("[WiFi] Подключено!"); // WiFi.localIP() читаем в loop/status path
      } else {
        wifiConnFailStatus.store(wifiConnFinalSt.load());
        wifiConnResult.store(3); // fail
        wifiNeedApRestart.store(true); // loop task переключит WiFi в AP
        Serial.printf("[WiFi] Ошибка подключения, статус=%d\n", wifiConnFailStatus.load());
      }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// ════════════════════════════════════════════════════════════
//  ФОНОВАЯ ЗАГРУЗКА ПОГОДЫ (FreeRTOS task)
//  WebServer НЕ потокобезопасен — он остаётся в loop task
//  fetchWeather() блокирует до 8 сек — выносим его в отдельный task
// ════════════════════════════════════════════════════════════
void weatherTask(void* param) {
  for (;;) {
    if (otaInProgress.load()) { vTaskDelay(200 / portTICK_PERIOD_MS); continue; }
    if (weatherRequested.exchange(false)) {
      if (wifiConnected) {
        Serial.println("[Weather] Запрос погоды...");
        fetchWeather();
        Serial.printf("[Weather] %.1f C  стек: %u bytes free\n",
          safeGetTemp(), uxTaskGetStackHighWaterMark(NULL));
      } else {
        weatherRequested.store(true); // WiFi недоступен — сохраняем флаг для повтора
      }
    }
    if (sunFetchRequested.exchange(false)) {
      Serial.printf("[Sun] sunFetchRequested сработал, wifiConnected=%d\n", (int)wifiConnected.load());
      if (wifiConnected) fetchSunriseSunset();
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

void bambuTask(void* param) {
  for (;;) {
    if (otaInProgress.load()) { vTaskDelay(200 / portTICK_PERIOD_MS); continue; }
    char relayUrl[sizeof(bambuCfg.relayUrl)];
    char relayKey[sizeof(bambuCfg.relayKey)];
    bool enabled = false;
    bool configured = bambuHasRelayConfig(&enabled, relayUrl, sizeof(relayUrl), relayKey, sizeof(relayKey));

    BambuState st = getBambuStateSnapshot();
    st.enabled = enabled;
    st.configured = configured;

    if (!enabled) {
      st.connected = false;
      st.printerOnline = false;
          st.lifecycle = BAMBU_UNKNOWN;
      st.displayLifecycle = BAMBU_UNKNOWN;
      st.progress = 0;
      setBambuDetail(st, "Bambu выключен");
      setBambuStateSnapshot(st);
      bambuSleepOrWake(2000);
      continue;
    }

    if (!configured) {
      st.connected = false;
      st.printerOnline = false;
      st.lifecycle = BAMBU_UNKNOWN;
      st.displayLifecycle = BAMBU_UNKNOWN;
      st.progress = 0;
      setBambuDetail(st, "Заполните Relay URL");
      setBambuStateSnapshot(st);
      bambuSleepOrWake(2000);
      continue;
    }

    if (!wifiConnected.load()) {
      st.connected = false;
      st.printerOnline = false;
      st.lifecycle = BAMBU_UNKNOWN;
      st.displayLifecycle = BAMBU_UNKNOWN;
      st.progress = 0;
      setBambuDetail(st, "Bambu Relay ждёт WiFi");
      setBambuStateSnapshot(st);
      bambuSleepOrWake(2000);
      continue;
    }

    if (bambuFetchRelayStatus(relayUrl, relayKey)) {
      BambuState next = getBambuStateSnapshot();
      uint32_t delayMs = 30000;
      if (next.displayLifecycle == BAMBU_PRINTING ||
          next.displayLifecycle == BAMBU_PAUSED ||
          next.displayLifecycle == BAMBU_ERROR) {
        delayMs = 5000;
      }
      bambuSleepOrWake(delayMs);
      continue;
    }

    st = getBambuStateSnapshot();
    st.connected = false;
    st.printerOnline = false;
      st.lifecycle = BAMBU_UNKNOWN;
    st.displayLifecycle = BAMBU_UNKNOWN;
    st.progress = 0;
    strlcpy(st.rawStatus, "", sizeof(st.rawStatus));
    strlcpy(st.rawStage, "", sizeof(st.rawStage));
    setBambuDetail(st, "Relay недоступен");
    setBambuStateSnapshot(st);
    bambuSleepOrWake(5000);
  }
}

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("\n╔════════════════════════╗");
  Serial.println("║  ESP32 LED Matrix v3.0 ║");
  Serial.println("╚════════════════════════╝");
  Serial.printf("FW %s, partition %s\n", FW_VERSION, esp_ota_get_running_partition()->label);

  // ── Boot-guard / анти-кирпич ──
  // Максимально рано, до FastLED/loadConfig: краш в ранней инициализации тоже
  // должен учитываться. Задачи ещё не созданы — NVS доступен без мьютекса.
  {
    Preferences bg;
    if (bg.begin("bootguard", false)) {
      uint8_t cnt = bg.getUChar("cnt", 0) + 1;
      if (cnt > BOOT_GUARD_MAX_FAILS) {
        const esp_partition_t* run  = esp_ota_get_running_partition();
        const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);
        Serial.printf("[BootGuard] %u сбоев подряд, пробуем откат %s → %s\n",
                      (unsigned)(cnt - 1), run ? run->label : "?", next ? next->label : "?");
        // Счётчик обнуляем до отката, иначе пинг-понг между слотами без шанса
        // на стабильные 60 с. esp_ota_set_boot_partition сам верифицирует образ:
        // на пустом/битом слоте вернёт ошибку и останемся на текущей прошивке.
        bg.putUChar("cnt", 0);
        if (next && next != run && esp_ota_set_boot_partition(next) == ESP_OK) {
          bg.end();
          Serial.println("[BootGuard] Откат принят, перезагрузка");
          delay(100);
          ESP.restart();
        }
        Serial.println("[BootGuard] Откат невозможен (второй слот пуст/битый) — продолжаем");
        cnt = 1;
      }
      bg.putUChar("cnt", cnt);
      bg.end();
      Serial.printf("[BootGuard] boot count = %u\n", (unsigned)cnt);
    }
  }

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 2500);
  clearMatrix(); FastLED.show();

  randomSeed(esp_random());

  cfgMtx  = xSemaphoreCreateMutex();
  prefsMtx = xSemaphoreCreateMutex();
  bambuCfgMtx = xSemaphoreCreateMutex();
  if (!cfgMtx || !prefsMtx || !bambuCfgMtx) fatalHalt("Failed to create mutexes");

  loadConfig();
  ensureBootstrapSecrets();
  hasSsid.store(cfg.ssid[0] != '\0'); // синхронизируем с загруженным конфигом
  // Яркость при старте: если авто-режим уже успел сохранить последнее вычисленное
  // значение, применяем его сразу. Как только NTP даст актуальное время, loop()
  // пересчитает день/ночь без прежней минутной задержки.
  applyConfiguredBrightnessNow(true);
  xSemaphoreTake(cfgMtx, portMAX_DELAY);
  refreshRenderSnapLocked();
  xSemaphoreGive(cfgMtx);
  showBoot();

  // ── WiFi init ──
  // persistent(false) — не писать credentials в NVS (используем Preferences)
  WiFi.persistent(false);

  if (strlen(cfg.ssid) > 0) {
    Serial.printf("══════════════════════════════════\n");
    Serial.printf("Сохранённая сеть: '%s'\n", cfg.ssid);
    Serial.printf("══════════════════════════════════\n");

    const int MAX_TRIES = 2;
    const uint32_t TIMEOUT_MS = 12000;
    bool connected = false;
    char primarySsid[sizeof(cfg.ssid)];
    char primaryPass[sizeof(cfg.wifiPass)];
    xSemaphoreTake(cfgMtx, portMAX_DELAY);
    strlcpy(primarySsid, cfg.ssid, sizeof(primarySsid));
    strlcpy(primaryPass, cfg.wifiPass, sizeof(primaryPass));
    xSemaphoreGive(cfgMtx);

    for (int attempt = 1; attempt <= MAX_TRIES && !connected; attempt++) {
      Serial.printf("[WiFi] Попытка %d/%d → SSID='%s'\n",
        attempt, MAX_TRIES, primarySsid);
      WiFi.mode(WIFI_STA);
      delay(100);
      configureStaNetworkPolicy();
      esp_wifi_set_ps(WIFI_PS_NONE);  // power save off для стабильного STA
      WiFi.begin(primarySsid, primaryPass);

      uint32_t t0 = millis();
      uint32_t printT = 0;
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < TIMEOUT_MS) {
        showConnecting();
        delay(80);
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
        apMode.store(false);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        Serial.printf("[WiFi] ✓ IP=%s  RSSI=%d dBm\n",
          WiFi.localIP().toString().c_str(), WiFi.RSSI());
        // Фоллбэк-серверы: SNTP сам опрашивает их при недоступности основного
        configTime((long)roundf(cfg.tzOffset * 3600.0f), 0, cfg.ntpServer,
                   "time.google.com", "time.cloudflare.com");
        brightnessRefreshRequested.store(true);
        weatherRequested = true;  // погода через task, не блокируя
        { bool sunMode; xSemaphoreTake(cfgMtx, portMAX_DELAY); sunMode = cfg.autoBrSunMode; xSemaphoreGive(cfgMtx);
          if (sunMode) sunFetchRequested.store(true); }
      } else {
        Serial.printf("[WiFi] ✗ Попытка %d: статус=%d\n", attempt, st);
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
  if (apMode.load()) {
    dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
    Serial.println("[DNS] Captive portal запущен");
  }

  server.on("/generate_204",              HTTP_GET, [](){ server.sendHeader("Location","/"); server.send(302); });
  server.on("/hotspot-detect.html",       HTTP_GET, [](){ server.sendHeader("Location","/"); server.send(302); });
  server.on("/library/test/success.html", HTTP_GET, [](){ server.sendHeader("Location","/"); server.send(302); });
  server.on("/connecttest.txt",           HTTP_GET, [](){ server.sendHeader("Location","/"); server.send(302); });
  server.on("/ncsi.txt",                  HTTP_GET, [](){ server.sendHeader("Location","/"); server.send(302); });
  server.on("/fwlink",                    HTTP_GET, [](){ server.sendHeader("Location","/"); server.send(302); });
  server.on("/redirect",                  HTTP_GET, [](){ server.sendHeader("Location","/"); server.send(302); });

  server.on("/",              HTTP_GET,  handleRoot);
  server.on("/api/settings",  HTTP_GET,  handleGetSettings);
  server.on("/api/settings",  HTTP_POST, handlePostSettings);
  server.on("/api/status",    HTTP_GET,  handleStatus);
  server.on("/api/scan",      HTTP_GET,  handleScanWifi);
  server.on("/api/connect",   HTTP_POST, handleConnectWifi);
  server.on("/api/disconnect",HTTP_POST, handleDisconnectWifi);

  server.on("/api/reboot",    HTTP_POST, handleReboot);
  server.on("/api/weather/refresh", HTTP_POST, handleWeatherRefresh);
  server.on("/api/update",    HTTP_POST, handleUpdateFinal, handleUpdateUpload);
  server.onNotFound([](){ server.sendHeader("Location","/"); server.send(302); });
  server.begin();
  Serial.println("Server started");

  // ── ArduinoOTA (espota, порт 3232): arduino-cli upload --protocol network ──
  ArduinoOTA.setHostname("esp32-matrix");
  ArduinoOTA.setMdnsEnabled(false);     // IP статичный, espota работает по IP — mDNS не нужен (и ~5-10 KB heap целее)
  ArduinoOTA.onStart([]() {
    otaEnterUpdateMode();
    flushDeferredSave(true);
    Serial.println("[OTA] ArduinoOTA start");
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    showOtaProgress(total ? (uint8_t)((uint64_t)done * 100 / total) : (uint8_t)255);
  });
  ArduinoOTA.onEnd([]() {
    // espota перезагружает устройство сам, final-обработчика нет — счётчик
    // boot-guard сбрасываем здесь, чтобы он не уехал в новую прошивку.
    xSemaphoreTake(prefsMtx, portMAX_DELAY);
    bootGuardReset();
    xSemaphoreGive(prefsMtx);
    showOtaDone();
    Serial.println("[OTA] ArduinoOTA done");
  });
  ArduinoOTA.onError([](ota_error_t err) {
    otaAbortUpdateMode();
    Serial.printf("[OTA] ArduinoOTA error %u\n", (unsigned)err);
  });
  ArduinoOTA.begin();
  Serial.println("ArduinoOTA listening on :3232");

  // ESP32-C3 одноядерный: фоновые задачи time-slice на том же ядре, WebServer остаётся в loop task.
  BaseType_t weatherTaskOk = xTaskCreatePinnedToCore(
    weatherTask,   // погода в background task — HTTPClient блокирует до 8 сек
    "weather",
    20480,         // стек: HTTPClient + ArduinoJson + WiFiClientSecure/mbedTLS (~12 KB TLS handshake)
    NULL,
    1,             // низкий приоритет
    NULL,
    0              // единственное ядро ESP32-C3
  );
  BaseType_t wifiConnTaskOk = xTaskCreatePinnedToCore(
    wifiConnTask,  // асинхронное подключение к WiFi — не блокирует loop/сервер
    "wifiConn",
    8192,
    NULL,
    1,
    NULL,
    0              // единственное ядро ESP32-C3
  );
  BaseType_t bambuTaskOk = xTaskCreatePinnedToCore(
    bambuTask,
    "bambuRelay",
    12288,
    NULL,
    1,
    &bambuTaskHandle,
    0              // единственное ядро ESP32-C3
  );
  if (weatherTaskOk != pdPASS) fatalHalt("Failed to create weather task");
  if (wifiConnTaskOk != pdPASS) fatalHalt("Failed to create WiFi connect task");
  if (bambuTaskOk != pdPASS) fatalHalt("Failed to create Bambu task");
  Serial.println("Background tasks started; WebServer stays in loop task");
}

// ════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════
void loop() {
  // Отложенная перезагрузка: ждём 500 мс после ответа браузеру, чтобы TCP успел сброситься
  if (rebootPending.load() && millis() - rebootMs.load() > 1000) ESP.restart();

  // Веб-сервер в loop task (WebServer не потокобезопасен)
  dnsServer.processNextRequest();
  server.handleClient();
  ArduinoOTA.handle();

  // Boot-guard: 60 с аптайма без краша = загрузка успешна. Критерий — только
  // millis(): прошивка в AP-режиме без Wi-Fi тоже валидна, привязка к Wi-Fi
  // давала бы ложные откаты.
  if (!bootGuardCleared && millis() >= BOOT_GUARD_STABLE_MS) {
    bootGuardCleared = true;
    xSemaphoreTake(prefsMtx, portMAX_DELAY);
    bootGuardReset();
    xSemaphoreGive(prefsMtx);
    Serial.println("[BootGuard] Загрузка стабильна, счётчик сброшен");
  }

  // Loop-task: WiFi.mode/begin вызываем из loop task (единая точка доступа к WiFi API).
  // wifiConnTask выставляет wifiDoConnect с уже сохранёнными ssid/pass.
  if (wifiDoConnect.load()) {
    wifiDoConnect.store(false);
    char ssid[40], pass[64];
    taskENTER_CRITICAL(&wifiConnMux);
    strlcpy(ssid, wifiPendSsid, sizeof(ssid));
    strlcpy(pass, wifiPendPass, sizeof(pass));
    taskEXIT_CRITICAL(&wifiConnMux);
    WiFi.mode(WIFI_OFF);
    delay(300);
    WiFi.mode(WIFI_AP_STA);
    delay(300);
    { IPAddress apIP(192, 168, 4, 1);
      WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
      WiFi.softAP(AP_NAME, LEGACY_DEFAULT_AP_PASS, 6, 0, 4); }
    delay(500);
    configureStaNetworkPolicy();
    wifiConnDone.store(false); // сброс перед началом — loop() начнёт мониторинг
    wifiConnOk.store(false);
    WiFi.begin(ssid, pass);
    esp_wifi_set_ps(WIFI_PS_NONE); // power save off для стабильного подключения
    wifiConnInProgress.store(true); // сигнал wifiConnTask: мониторинг начался
  }

  // Мониторинг подключения WiFi — WiFi.status() вызывается только из loop task.
  // wifiConnTask ждёт wifiConnDone вместо прямого вызова WiFi.status().
  { static uint32_t monT0 = 0;
    if (wifiConnInProgress.load() && !wifiConnDone.load()) {
      if (monT0 == 0) monT0 = millis() | 1u; // |1 чтобы не спутать с "не запущено"
      int ws = WiFi.status();
      if (ws == WL_CONNECTED || millis() - monT0 >= 15000) {
        if (ws == WL_CONNECTED) {
          esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        }
        wifiConnFinalSt.store((uint8_t)ws);
        wifiConnOk.store(ws == WL_CONNECTED);
        wifiConnDone.store(true);
        monT0 = 0;
      }
    } else if (!wifiConnInProgress.load()) {
      monT0 = 0;
    }
  }

  // Автопереподключение к сохранённой сети — раз в 10 мин если при старте не удалось
  // (или после потери связи, пока не подключились). Реиспользует wifiConnPending-путь.
  { static uint32_t autoReconMs = 0;
    if (!wifiConnected.load() && hasSsid.load() &&
        !wifiConnPending.load() && !wifiConnInProgress.load() &&
        millis() - autoReconMs >= 600000UL) {
      autoReconMs = millis();
      char ssid[sizeof(cfg.ssid)], pass[sizeof(cfg.wifiPass)];
      xSemaphoreTake(cfgMtx, portMAX_DELAY);
      strlcpy(ssid, cfg.ssid,     sizeof(ssid));
      strlcpy(pass, cfg.wifiPass, sizeof(pass));
      xSemaphoreGive(cfgMtx);
      if (ssid[0]) {
        Serial.printf("[WiFi] Авто-переподключение к '%s'...\n", ssid);
        taskENTER_CRITICAL(&wifiConnMux);
        strlcpy(wifiPendSsid, ssid, sizeof(wifiPendSsid));
        strlcpy(wifiPendPass, pass, sizeof(wifiPendPass));
        taskEXIT_CRITICAL(&wifiConnMux);
        wifiConnFailStatus.store(0);
        wifiConnResult.store(1); // "connecting" в браузере
        wifiConnPending.store(true); // передаём управление wifiConnTask
      }
    }
  }

  // Восстановление AP после неудачного WiFi-подключения (запрошено wifiConnTask)
  if (wifiNeedApRestart.load()) {
    wifiNeedApRestart.store(false);
    wifiConnInProgress.store(false); // сброс флага мониторинга
    wifiConnDone.store(true);        // разблокируем wifiConnTask: больше не нужно ждать 20с
    startAP();
    dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
  }

  // Выключить AP через 60 сек после успешного подключения к WiFi
  if (wifiConnected && apMode.load()) {
    uint32_t apOff = apOffMs.load();
    if (apOff == 0) { apOff = millis(); apOffMs.store(apOff); }
    if (millis() - apOff > 60000) {
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      apMode.store(false);
      dnsServer.stop();
      Serial.println("[AP] Выключен (WiFi подключён)");
    }
  }
  // Проверяем что STA соединение не упало — неблокирующий переход в AP:
  // WiFi.mode(WIFI_OFF) требует ~200мс на завершение, используем таймер вместо delay()
  { static uint32_t wifiDropOffMs = 0;
    if (wifiConnected.load() && !apMode.load() && WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Соединение потеряно — перейдём в AP, авто-реконнект через 10 мин");
      wifiConnected.store(false);
      wifiConnFailStatus.store(WL_CONNECTION_LOST);
      wifiConnResult.store(3);
      WiFi.mode(WIFI_OFF);
      wifiDropOffMs = millis() | 1u; // |1 чтобы 0 означало «не в процессе»
    }
    if (wifiDropOffMs && millis() - wifiDropOffMs >= 200) {
      wifiDropOffMs = 0;
      startAP();
      dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
      apMode.store(true);
      apOffMs.store(0);
    }
  }

  // Авто-яркость день/ночь: стартуем с кэша, затем коротко проверяем NTP часто.
  // Если время не появилось быстро, уходим в мягкий backoff, чтобы не греть CPU
  // постоянным polling'ом. После первого успешного применения — раз в минуту.
  { static uint32_t lastBrChkMs = 0;
    static bool brightnessResolved = false;
    uint32_t now = millis();
    uint32_t interval = brightnessResolved ? 60000UL : (now < 30000UL ? 500UL : 5000UL);
    bool forced = brightnessRefreshRequested.exchange(false);
    if (forced || lastBrChkMs == 0 || now - lastBrChkMs >= interval) {
      lastBrChkMs = now | 1u;
      bool applied = applyConfiguredBrightnessNow(false);
      if (applied) brightnessResolved = true;
      else brightnessResolved = false;
    }
  }

  // Ежесуточное обновление данных восхода/заката (если режим включён)
  if (wifiConnected) {
    static uint32_t sunReqThrottleMs = 0;
    xSemaphoreTake(cfgMtx, portMAX_DELAY);
    bool sunMode = cfg.autoBrSunMode;
    xSemaphoreGive(cfgMtx);
    if (sunMode) {
      uint32_t last = lastSunFetchMs.load();
      bool stale = !sunTimeValid.load() || (last != 0 && millis() - last >= 86400000UL);
      if (stale && millis() - sunReqThrottleMs >= 30000) {
        sunReqThrottleMs = millis();
        sunFetchRequested.store(true);
      }
    }
  }

  // NVS-запись — только из loop task (wifiConnTask выставляет флаг вместо прямого вызова saveConfig)
  if (savePending.load() && !flushDeferredSave(false) && !savePending.load()) {
    Serial.println("[NVS] Deferred save failed");
  }

  // configTime — только из loop task (единая точка изменения глобального lwIP/time state)
  if (ntpPending.exchange(false)) {
    xSemaphoreTake(cfgMtx, portMAX_DELAY);
    long tz = (long)roundf(cfg.tzOffset * 3600.0f);
    char ntp[sizeof(cfg.ntpServer)];
    strlcpy(ntp, cfg.ntpServer, sizeof(ntp));
    xSemaphoreGive(cfgMtx);
    configTime(tz, 0, ntp, "time.google.com", "time.cloudflare.com");
    brightnessRefreshRequested.store(true);
  }

  // Снапшот cfg под cfgMtx — обновляем не чаще раз в 100мс, чтобы не насыщать CPU
  // в tight-loop между кадрами. 100мс лаг смены настроек незаметен пользователю.
  static uint32_t rSnapMs = 0;
  static uint16_t snapUpdateMin = 10;
  if (millis() - rSnapMs >= 100) {
    rSnapMs = millis();
    xSemaphoreTake(cfgMtx, portMAX_DELAY);
    snapUpdateMin        = cfg.updateMin;
    refreshRenderSnapLocked();
    xSemaphoreGive(cfgMtx);
  }

  // Обновление погоды — через флаг weatherTask (не блокирует loop).
  // При сбое используем короткий retry-backoff, чтобы не spam'ить API и Serial.
  if (wifiConnected) {
    uint32_t interval = (uint32_t)snapUpdateMin * 60000UL;
    taskENTER_CRITICAL(&weatherMux);
    uint32_t lwm = lastWeatherMs;
    uint32_t lwa = lastWeatherAttemptMs;
    uint32_t failRetry = (interval < 30000UL) ? interval : 30000UL;
    bool needUpdate =
        (lwm != 0 && millis() - lwm >= interval) ||
        (lwm == 0 && (lwa == 0 || millis() - lwa >= failRetry));
    taskEXIT_CRITICAL(&weatherMux);
    if (needUpdate) weatherRequested = true;
  }

  // Единственный режим отображения — «Часы + темп», кадр раз в 250 мс
  if (millis() - lastDrawMs < 250) {
    // Без yield loop() крутится почти на 100% между кадрами и заметно греет ESP32-C3.
    // 4мс сохраняют отзывчивость WebServer и заметно уменьшают idle-polling.
    delay(4);
    return;
  }
  lastDrawMs = millis();
  showClockTemp();
}
