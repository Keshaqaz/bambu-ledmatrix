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
#include <esp_heap_caps.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <atomic>
#include "app.hpp"
#include "bambu_cloud.hpp"
#include "bambu_legacy_migration.hpp"
#include "ota_display_policy.hpp"
#include "printer_status_source.hpp"
#include "security_policy.hpp"
#include "secure_network_coordinator.hpp"
#include "t05_resilience.hpp"
#include "local_access_policy.hpp"
#include "soak_log.hpp"
#include "wifi_reconnect_policy.hpp"

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
void handleRoot();
void handleGetSettings();
void handlePostSettings();
void handleStatus();
void handleSoakGet();
void handleSoakReset();
void handleScanWifi();
void handleConnectWifi();
void handleDisconnectWifi();
void handleReboot();
void handleWeatherRefresh();
void handleBambuAuthStatus();
void handleBambuAuthLogin();
void handleBambuAuthVerify();
void handleBambuPrinters();
void handleBambuPrinterSelect();
void handleBambuLogout();
void handleUpdateUpload();
void handleUpdateFinal();
void showBoot();
void showConnecting();
static void startAP();
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
#ifdef BAMBU_AUTH_TRANSPORT_CAPTURE
static constexpr const char* FW_VERSION = "4.3.11-cap04-2026.09.02";
#elif defined(BAMBU_AUTH_TRANSPORT_PROBE)
static constexpr const char* FW_VERSION = "4.3.12-at04-2026.09.02";
#else
static constexpr const char* FW_VERSION = "4.4.11-2026.11.14";
#endif
static constexpr uint8_t SOAK_FW_MINOR = 4;  // 4.x — пишется в soak boot ring
static constexpr uint32_t SOAK_SAMPLE_INTERVAL_MS = 60000UL;
static constexpr uint32_t SOAK_CHECKPOINT_INTERVAL_MS = 600000UL;
static constexpr uint32_t STORED_STATE_MAGIC = 0x4D415458UL; // MATX
static constexpr uint16_t STORED_STATE_VERSION = 2;
static constexpr size_t BAMBU_TOKEN_CAPACITY = 384;
static constexpr uint8_t MIN_UI_BRIGHTNESS = 1;
static constexpr size_t MAX_SETTINGS_BODY_BYTES = 4096;
static constexpr size_t MAX_CONNECT_BODY_BYTES = 256;
static constexpr size_t MAX_RELAY_JSON_BYTES = 4096;
static constexpr size_t MAX_WEATHER_JSON_BYTES = 12288;

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
  char     webPass[32]     = ""; // legacy NVS bytes; ignored by security level 0
  char     apPass[64]      = ""; // legacy NVS bytes; ignored by security level 0

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

using BambuConfig = BambuRelayConfig;

struct StoredStateBlob {
  uint32_t magic = STORED_STATE_MAGIC;
  uint16_t version = STORED_STATE_VERSION;
  uint16_t size = sizeof(StoredStateBlob);
  Config cfg;
  BambuConfig bambuCfg;
  uint32_t checksum = 0;
};

// Explicit v1 layout: it contained obsolete ESP-side Bambu account buffers.
// It is read only during migration, copied into the relay-only runtime layout,
// securely wiped from RAM and immediately rewritten as StoredStateBlob v2.
struct LegacyStoredStateBlobV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  Config cfg;
  LegacyBambuConfigV2 bambuCfg;
  char bambuToken[BAMBU_TOKEN_CAPACITY];
  uint32_t checksum;
};

Config     cfg;
BambuConfig bambuCfg;
Preferences prefs;

// ════════════════════════════════════════════════════════════
//  РАНТАЙМ
// ════════════════════════════════════════════════════════════
WebServer server(80);
DNSServer dnsServer;

static char AP_NAME[32] = "ESP32-Matrix";

float    currentTemp    = -999.0f;
std::atomic<bool> wifiConnected{false};
std::atomic<bool>     apMode{true};   // стартуем всегда в AP
std::atomic<bool> weatherRequested{false}; // флаг запроса погоды для weatherTask
// configTime() модифицирует глобальное состояние lwIP — вызываем только из loop task.
// Wi-Fi state machine выставляет этот флаг, loop task вызывает configTime.
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
static SecureNetworkCoordinator secureNetworkCoordinator;
static BambuCloud bambuCloud(secureNetworkCoordinator);
// T06: direct Cloud — production-default. NVS-ключ ниже хранит явный выбор
// пользователя; одноразовая миграция дефолта защищена marker-ключом, чтобы
// позже выбранный вручную relay-rollback не перетирался при следующих загрузках.
static std::atomic<PrinterSourceMode> printerSourceMode{PRINTER_SOURCE_CLOUD_DIRECT};
static constexpr const char* PREF_PRINTER_SOURCE = "bsource";
static constexpr const char* PREF_SOURCE_DEFAULT_MIGRATED = "bsrcmig";

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
char     lastWeatherError[160] = "";
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
static SemaphoreHandle_t cfgMtx;   // защищает cfg: пишет WebUI/Wi-Fi state machine, читает fetchWeather
static SemaphoreHandle_t prefsMtx; // защищает NVS/Preferences: saveConfig/loadConfig
static SemaphoreHandle_t bambuCfgMtx; // защищает bambuCfg: пишет веб-интерфейс, читает bambuTask
static TaskHandle_t bambuTaskHandle = nullptr;
static TaskHandle_t weatherTaskHandle = nullptr;
static TaskHandle_t loopTaskHandle = nullptr;
static std::atomic<uint32_t> minimumLargestInternalBlock{UINT32_MAX};

static void sampleMemoryMetrics() {
  uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint32_t previous = minimumLargestInternalBlock.load();
  while (largest < previous &&
         !minimumLargestInternalBlock.compare_exchange_weak(previous, largest)) {}
}

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

static portMUX_TYPE printerStatusMux = portMUX_INITIALIZER_UNLOCKED;

static bool bambuHasRelayConfig(bool* enabledOut = nullptr,
                                char* relayUrl = nullptr, size_t relayUrlSz = 0,
                                char* relayKey = nullptr, size_t relayKeySz = 0) {
  bool enabled = false;
  char localRelayUrl[sizeof(bambuCfg.relayUrl)] = "";
  xSemaphoreTake(bambuCfgMtx, portMAX_DELAY);
  enabled = bambuCfg.enabled;
  // Relay-only runtime: keep the old flag forced on for migrated configs.
  bambuCfg.relayEnabled = true;
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

class BambuCloudExternalTlsPause {
 public:
  BambuCloudExternalTlsPause() : acquired_(bambuCloud.pauseForExternalTls()) {}
  ~BambuCloudExternalTlsPause() {
    if (acquired_) {
      bambuCloud.resumeAfterExternalTls();
      wakeBambuTask();
    }
  }
  bool acquired() const { return acquired_; }
 private:
  bool acquired_;
};

// ── Асинхронное подключение к WiFi ──────────────────────────────────────────
// handleConnectWifi() только сохраняет параметры и возвращает ответ браузеру.
// loop() выполняет фактическое подключение неблокирующей state machine.
// Результат читается браузером через /api/status polling.
static char              wifiPendSsid[40]  = "";
static char              wifiPendPass[64]  = "";
static std::atomic<bool> wifiConnPending{false};
static std::atomic<int>  wifiConnResult{0}; // 0=idle 1=connecting 2=ok 3=fail
static std::atomic<int>  wifiConnFailStatus{0};
static portMUX_TYPE      wifiConnMux = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<bool>    wifiConnInProgress{false};
enum class WifiConnectPhase : uint8_t { Idle, RadioOffWait, StaStartWait, Connecting };
static WifiConnectPhase wifiConnectPhase = WifiConnectPhase::Idle;
static uint32_t wifiConnectDeadlineMs = 0;
static uint32_t wifiConnectStartedMs = 0;
static char wifiActiveSsid[40] = "";
static char wifiActivePass[64] = "";
// Планировщик читается/изменяется только из Arduino setup()/loop() task.
static WifiReconnectPolicy  wifiReconnectPolicy;
// NVS-запись выполняется только из loop task.
static std::atomic<bool>    savePending{false};
static std::atomic<uint32_t> saveDueMs{0};
static std::atomic<bool>    saveError{false};

// Таймер выключения AP после успешного STA подключения.
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
// чтобы избежать гонок с Wi-Fi state machine, которая пишет cfg.ssid/wifiPass.
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
  loaded.apPass[0] = '\0';
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

static void secureZero(void* ptr, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(ptr);
  while (length--) *bytes++ = 0;
}

static void initializeLocalIdentity() {
  uint64_t mac = ESP.getEfuseMac();
  snprintf(AP_NAME, sizeof(AP_NAME), "ESP32-Matrix-%06llX",
           static_cast<unsigned long long>(mac & 0xFFFFFFULL));
  Serial.println("[Security] Local access level 0: open AP and WebUI/API");
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

static bool loadLegacyStoredStateBlob(LegacyStoredStateBlobV1& blob) {
  if (!prefs.isKey("state") || prefs.getBytesLength("state") != sizeof(blob)) return false;
  size_t got = prefs.getBytes("state", &blob, sizeof(blob));
  if (got != sizeof(blob) || blob.magic != STORED_STATE_MAGIC || blob.version != 1 ||
      blob.size != sizeof(blob)) return false;
  uint32_t expected = fnv1a32(reinterpret_cast<const uint8_t*>(&blob),
                              offsetof(LegacyStoredStateBlobV1, checksum));
  if (blob.checksum != expected) {
    Serial.println("[NVS] legacy state checksum mismatch");
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

static PrinterLifecycle relayResolveLifecycle(const String& rawStatus,
                                               const String& rawStage,
                                               float progress) {
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

  if (normalized.indexOf("FAIL") >= 0 || normalized.indexOf("ERROR") >= 0 || normalized.indexOf("CANCEL") >= 0) return PRINTER_ERROR;
  if (normalized.indexOf("PAUSE") >= 0) return PRINTER_PAUSED;
  if (normalized.indexOf("DONE") >= 0 || normalized.indexOf("SUCCESS") >= 0 || normalized.indexOf("COMPLETE") >= 0 || normalized.indexOf("COMPLETED") >= 0 || normalized.indexOf("FINISH") >= 0) return PRINTER_FINISHED;
  if (normalized.indexOf("PREPARE") >= 0 || normalized.indexOf("PREPARING") >= 0 || normalized.indexOf("STARTING") >= 0 || normalized.indexOf("HEATING") >= 0 || normalized.indexOf("DOWNLOAD") >= 0) return PRINTER_PREPARING;
  if (normalized.indexOf("RUNNING") >= 0 || normalized.indexOf("PRINTING") >= 0 || normalized.indexOf("PROCESSING") >= 0) return PRINTER_PRINTING;
  if (normalized.indexOf("OFFLINE") >= 0) return PRINTER_OFFLINE;
  if (normalized.indexOf("IDLE") >= 0 || normalized.indexOf("WAIT") >= 0) return PRINTER_IDLE;
  if (progress > 0.0f && progress < 100.0f) return PRINTER_PRINTING;
  return PRINTER_UNKNOWN;
}

static const char* printerLifecycleLabel(PrinterLifecycle lifecycle) {
  switch (lifecycle) {
    case PRINTER_PRINTING: return "печать";
    case PRINTER_PREPARING: return "подготовка";
    case PRINTER_PAUSED: return "пауза";
    case PRINTER_FINISHED: return "готово";
    case PRINTER_ERROR: return "ошибка";
    case PRINTER_IDLE: return "ожидание";
    case PRINTER_OFFLINE: return "офлайн";
    default: return "нет данных";
  }
}

static PrinterLifecycle relayLifecycleFromString(const String& raw) {
  String normalized;
  normalized.reserve(raw.length());
  for (size_t i = 0; i < raw.length(); i++) {
    char ch = raw[i];
    if (isalnum((unsigned char)ch)) normalized += (char)toupper((unsigned char)ch);
  }
  if (normalized == "PRINTING" || normalized == "RUNNING") return PRINTER_PRINTING;
  if (normalized == "PREPARING" || normalized == "PREPARE") return PRINTER_PREPARING;
  if (normalized == "PAUSED" || normalized == "PAUSE") return PRINTER_PAUSED;
  if (normalized == "FINISHED" || normalized == "SUCCESS" || normalized == "DONE") return PRINTER_FINISHED;
  if (normalized == "ERROR" || normalized == "FAILED" || normalized == "FAIL") return PRINTER_ERROR;
  if (normalized == "OFFLINE") return PRINTER_OFFLINE;
  if (normalized == "IDLE") return PRINTER_IDLE;
  return PRINTER_UNKNOWN;
}

class RelayHttpSource : public PrinterStatusSource {
 public:
  PrinterStateSnapshot snapshot() const override {
    taskENTER_CRITICAL(&printerStatusMux);
    reducer_.advance(millis());
    PrinterStateSnapshot copy = reducer_.snapshot();
    taskEXIT_CRITICAL(&printerStatusMux);
    return copy;
  }

  void configure(bool enabled, bool configured, const char* detail) {
    taskENTER_CRITICAL(&printerStatusMux);
    reducer_.configure(enabled, configured, detail);
    taskEXIT_CRITICAL(&printerStatusMux);
  }

  void markUnavailable(bool transportConnected, PrinterLifecycle lifecycle,
                       const char* detail) {
    taskENTER_CRITICAL(&printerStatusMux);
    reducer_.markUnavailable(transportConnected, lifecycle, detail);
    taskEXIT_CRITICAL(&printerStatusMux);
  }

  uint32_t poll(const char* relayUrl, const char* relayKey);

 private:
  bool fetch(const char* relayUrl, const char* relayKey);

  void apply(const PrinterStatePatch& patch) {
    taskENTER_CRITICAL(&printerStatusMux);
    reducer_.apply(patch, millis());
    taskEXIT_CRITICAL(&printerStatusMux);
  }

  mutable PrinterStatusReducer reducer_;
};

static RelayHttpSource relayHttpSource;
static portMUX_TYPE shadowDiagnosticsMux = portMUX_INITIALIZER_UNLOCKED;
static PrinterShadowDiagnostics shadowDiagnostics;
static std::atomic<bool> shadowComparisonActive{false};
static std::atomic<bool> directSourceReady{false};

static PrinterStateSnapshot selectedPrinterSnapshot() {
  const PrinterStateSnapshot direct = bambuCloud.source().snapshot();
  xSemaphoreTake(bambuCfgMtx, portMAX_DELAY);
  const bool enabled = bambuCfg.enabled;
  xSemaphoreGive(bambuCfgMtx);
  directSourceReady.store(updateDirectSourceReadiness(
      directSourceReady.load(), enabled && direct.configured,
      bambuCloud.source().firstValidReport()));
  const PrinterSourceMode effectiveSource = effectivePrinterSourceMode(
      printerSourceMode.load(), directSourceReady.load());
  PrinterStateSnapshot snapshot =
      effectiveSource == PRINTER_SOURCE_CLOUD_DIRECT
          ? direct
          : relayHttpSource.snapshot();
  snapshot.enabled = enabled;
  return snapshot;
}

static void observePrinterShadow() {
  PrinterStateSnapshot relay = relayHttpSource.snapshot();
  PrinterStateSnapshot direct = bambuCloud.source().snapshot();
  if (!shadowComparisonActive.load()) {
    const bool bothValidated = relay.configured && direct.configured &&
                               relay.transportConnected &&
                               direct.transportConnected &&
                               relay.printerOnline && direct.printerOnline;
    if (!bothValidated) return;
    shadowComparisonActive.store(true);
  }
  // Once both sources produced a usable sample, compare every normalized
  // field, including later source-quality/offline transitions.
  taskENTER_CRITICAL(&shadowDiagnosticsMux);
  shadowDiagnostics.observe(relay, direct);
  taskEXIT_CRITICAL(&shadowDiagnosticsMux);
}

class BoundedInputStream : public Stream {
 public:
  BoundedInputStream(Stream& source, size_t limit) : source_(source), limit_(limit) {}
  int available() override {
    int sourceAvailable = source_.available();
    size_t remaining = consumed_ < limit_ ? limit_ - consumed_ : 0;
    return sourceAvailable < static_cast<int>(remaining) ? sourceAvailable : static_cast<int>(remaining);
  }
  int read() override {
    if (consumed_ >= limit_) {
      overflow_ = source_.available() > 0;
      return -1;
    }
    int value = source_.read();
    if (value >= 0) ++consumed_;
    return value;
  }
  int peek() override {
    if (consumed_ >= limit_) {
      overflow_ = source_.available() > 0;
      return -1;
    }
    return source_.peek();
  }
  size_t write(uint8_t) override { return 0; }
  bool overflowed() const { return overflow_; }
  bool drainUnknownLengthBody(HTTPClient& http, uint32_t idleTimeoutMs = 1000) {
    uint32_t lastProgressMs = millis();
    while (true) {
      if (source_.available() > 0) {
        if (read() < 0) return false;
        lastProgressMs = millis();
        continue;
      }
      if (!http.connected()) return !overflow_;
      if (millis() - lastProgressMs >= idleTimeoutMs) return false;
      delay(1);
    }
  }

 private:
  Stream& source_;
  size_t limit_;
  size_t consumed_ = 0;
  bool overflow_ = false;
};

static bool boundedHttpBodyAccepted(HTTPClient& http, BoundedInputStream& input,
                                    int contentLength, size_t limit) {
  if (input.overflowed() || contentLength > static_cast<int>(limit)) return false;
  // A known Content-Length was checked before parsing. For close-delimited
  // HTTP/1.0 responses, consume the remainder so a valid JSON prefix cannot
  // hide an oversized trailing body after ArduinoJson stops at the root value.
  return contentLength >= 0 || input.drainUnknownLengthBody(http);
}

bool RelayHttpSource::fetch(const char* relayUrl, const char* relayKey) {
  if (!relayUrl || !relayUrl[0]) return false;

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  http.setReuse(false);
  http.useHTTP10(true);
  if (!http.begin(relayUrl)) {
    Serial.println("[BambuRelay] begin failed");
    return false;
  }
  if (relayKey && relayKey[0]) http.addHeader("X-Relay-Key", relayKey);
  int code = http.GET();

  if (code != 200) {
    Serial.printf("[BambuRelay] HTTP %d\n", code);
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  if (contentLength > static_cast<int>(MAX_RELAY_JSON_BYTES)) {
    Serial.println("[BambuRelay] oversized response rejected");
    http.end();
    return false;
  }

  JsonDocument filter;
  filter["connected"] = true;
  filter["printerOnline"] = true;
  filter["progress"] = true;
  filter["status"] = true;
  filter["stage"] = true;
  filter["detail"] = true;
  filter["lifecycle"] = true;
  filter["jobName"] = true;
  filter["currentLayer"] = true;
  filter["totalLayers"] = true;
  filter["remainingSeconds"] = true;
  JsonDocument doc;
  BoundedInputStream input(http.getStream(), MAX_RELAY_JSON_BYTES);
  DeserializationError jsonError = deserializeJson(
    doc, input, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(4));
  bool bodyAccepted = boundedHttpBodyAccepted(
    http, input, contentLength, MAX_RELAY_JSON_BYTES);
  http.end();
  if (jsonError || !bodyAccepted) {
    Serial.printf("[BambuRelay] rejected JSON: %s\n",
                  !bodyAccepted ? "oversized or unterminated" : jsonError.c_str());
    return false;
  }

  const PrinterStateSnapshot previous = snapshot();
  const bool hasConnected = !doc["connected"].isNull();
  const bool hasPrinterOnline = !doc["printerOnline"].isNull();
  const bool connected = hasConnected
      ? doc["connected"].as<bool>()
      : previous.transportConnected;
  const bool printerOnline = hasPrinterOnline
      ? doc["printerOnline"].as<bool>()
      : previous.printerOnline;
  const bool hasProgress = !doc["progress"].isNull();
  float progress = hasProgress ? doc["progress"].as<float>() : 0.0f;
  // Relay progress is percent-valued. Preserve the long-standing fractional
  // compatibility rule without turning the valid value 1 into 100%.
  if (progress >= 0.0f && progress < 1.0f) progress *= 100.0f;
  String rawStatus = String(doc["status"] | "");
  String rawStage = String(doc["stage"] | "");
  String detail = String(doc["detail"] | "");
  String lifecycleText = String(doc["lifecycle"] | "");
  const bool hasLifecycleInput = !doc["lifecycle"].isNull() ||
                                 !doc["status"].isNull() ||
                                 !doc["stage"].isNull();
  PrinterLifecycle lifecycle = relayLifecycleFromString(lifecycleText);
  if (lifecycle == PRINTER_UNKNOWN && hasLifecycleInput)
    lifecycle = relayResolveLifecycle(rawStatus, rawStage, progress);
  if (hasPrinterOnline && !printerOnline) lifecycle = PRINTER_OFFLINE;

  PrinterStatePatch patch;
  if (hasConnected) {
    patch.present |= PRINTER_PATCH_TRANSPORT_CONNECTED;
    patch.transportConnected = connected;
  }
  if (hasPrinterOnline) {
    patch.present |= PRINTER_PATCH_PRINTER_ONLINE;
    patch.printerOnline = printerOnline;
  }
  if ((hasPrinterOnline && !printerOnline) || hasLifecycleInput) {
    patch.present |= PRINTER_PATCH_LIFECYCLE;
    patch.lifecycle = (hasPrinterOnline && !printerOnline)
        ? PRINTER_OFFLINE
        : lifecycle;
  }
  if (hasProgress) {
    patch.present |= PRINTER_PATCH_PROGRESS;
    patch.progress = (uint8_t)constrain((int)lroundf(progress), 0, 100);
  }
  // A stale/offline relay snapshot is a source-quality update, not a new
  // printer-state snapshot. Keep the last confirmed job metadata with the
  // visible overlay until an online update (especially explicit idle) arrives.
  if (!hasPrinterOnline || printerOnline) {
    if (!doc["jobName"].isNull()) {
      patch.present |= PRINTER_PATCH_JOB_NAME;
      strlcpy(patch.jobName, doc["jobName"] | "", sizeof(patch.jobName));
    }
    if (!doc["stage"].isNull()) {
      patch.present |= PRINTER_PATCH_STAGE;
      strlcpy(patch.stage, rawStage.c_str(), sizeof(patch.stage));
    }
    if (!doc["currentLayer"].isNull()) {
      patch.present |= PRINTER_PATCH_CURRENT_LAYER;
      patch.currentLayer = doc["currentLayer"].as<uint16_t>();
    }
    if (!doc["totalLayers"].isNull()) {
      patch.present |= PRINTER_PATCH_TOTAL_LAYERS;
      patch.totalLayers = doc["totalLayers"].as<uint16_t>();
    }
    if (!doc["remainingSeconds"].isNull()) {
      patch.present |= PRINTER_PATCH_REMAINING_SECONDS;
      patch.remainingSeconds = doc["remainingSeconds"].as<uint32_t>();
    }
  }
  if (detail.length()) {
    patch.present |= PRINTER_PATCH_DETAIL;
    strlcpy(patch.detail, detail.c_str(), sizeof(patch.detail));
  } else if (!connected) {
    patch.present |= PRINTER_PATCH_DETAIL;
    strlcpy(patch.detail, "Relay недоступен", sizeof(patch.detail));
  } else {
    char msg[96];
    const uint8_t shownProgress = hasProgress ? patch.progress : previous.progress;
    const PrinterLifecycle shownLifecycle =
        (patch.present & PRINTER_PATCH_LIFECYCLE) ? lifecycle : previous.lifecycle;
    snprintf(msg, sizeof(msg), "Relay: %s, %u%%",
             printerLifecycleLabel(shownLifecycle), shownProgress);
    patch.present |= PRINTER_PATCH_DETAIL;
    strlcpy(patch.detail, msg, sizeof(patch.detail));
  }
  apply(patch);
  const PrinterStateSnapshot current = snapshot();
  Serial.printf("[BambuRelay] connected=%d lifecycle=%u progress=%u status='%s' stage='%s'\n",
                connected ? 1 : 0, current.lifecycle, current.progress,
                rawStatus.c_str(), rawStage.c_str());
  return true;
}

uint32_t RelayHttpSource::poll(const char* relayUrl, const char* relayKey) {
  if (!fetch(relayUrl, relayKey)) {
    markUnavailable(false, PRINTER_UNKNOWN, "Relay недоступен");
    return 5000UL;
  }

  const PrinterStateSnapshot current = snapshot();
  if (current.visibleLifecycle == PRINTER_PRINTING ||
      current.visibleLifecycle == PRINTER_PAUSED ||
      current.visibleLifecycle == PRINTER_ERROR) {
    return 5000UL;
  }
  return 30000UL;
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

  PrinterStateSnapshot printer = selectedPrinterSnapshot();
  uint8_t overlayPhase = 0;
  if (printer.visibleLifecycle == PRINTER_FINISHED ||
      printer.visibleLifecycle == PRINTER_PAUSED ||
      printer.visibleLifecycle == PRINTER_ERROR) {
    overlayPhase = (uint8_t)((millis() / 500UL) & 1UL);
  } else if (printer.visibleLifecycle == PRINTER_PREPARING) {
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
  key.bambuLifecycle = printer.visibleLifecycle;
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

  switch (printer.visibleLifecycle) {
    case PRINTER_PREPARING: {
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
    case PRINTER_PRINTING: {
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
    case PRINTER_FINISHED: {
      if (overlayPhase) {
        CRGB c = CRGB(0, 220, 70);
        for (uint8_t x = 0; x < MW; x++) {
          px(x, 7, c);
          px(x, 8, c);
        }
      }
      break;
    }
    case PRINTER_PAUSED:
    case PRINTER_ERROR: {
      if (overlayPhase) {
        CRGB c = printer.visibleLifecycle == PRINTER_PAUSED ? CRGB(255, 200, 0) : CRGB(255, 0, 0);
        for (uint8_t y = 7; y <= 8; y++)
          for (uint8_t x = 0; x < MW; x++)
            px(x, y, c);
      }
      break;
    }
    case PRINTER_IDLE:
    case PRINTER_OFFLINE:
    case PRINTER_UNKNOWN:
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
  struct WeatherWifiPsGuard {
    WeatherWifiPsGuard() { esp_wifi_set_ps(WIFI_PS_NONE); }
    ~WeatherWifiPsGuard() { esp_wifi_set_ps(WIFI_PS_MIN_MODEM); }
  } wifiPsGuard;
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

    auto applyWeatherPayload = [&](HTTPClient& http) -> bool {
      int contentLength = http.getSize();
      if (contentLength > static_cast<int>(MAX_WEATHER_JSON_BYTES)) {
        setWeatherDiag(200, "json: oversized", keyLen, "owm");
        return false;
      }
      JsonDocument filter;
      filter["main"]["temp"] = true;
      filter["timezone"] = true;
      filter["coord"]["lat"] = true;
      filter["coord"]["lon"] = true;
      JsonDocument doc;
      BoundedInputStream input(http.getStream(), MAX_WEATHER_JSON_BYTES);
      DeserializationError err = deserializeJson(
        doc, input, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(4));
      bool bodyAccepted = boundedHttpBodyAccepted(
        http, input, contentLength, MAX_WEATHER_JSON_BYTES);
      if (err || !bodyAccepted) {
        char msg[64];
        snprintf(msg, sizeof(msg), "json: %s",
                 !bodyAccepted ? "oversized or unterminated" : err.c_str());
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

    auto runOwmRequest = [&](WiFiClient& client, bool useHttps, WiFiClientSecure* secureClient) -> bool {
      HTTPClient http;
      http.setConnectTimeout(8000);
      http.setTimeout(8000);
      http.setReuse(false);
      http.useHTTP10(true);

      const char* protocol = useHttps ? "https" : "http";
      const char* host = "api.openweathermap.org";
      IPAddress resolved;
      bool dnsOk = WiFi.hostByName(host, resolved);
      String resolvedIp = dnsOk ? resolved.toString() : String("dns-fail");
      String gatewayIp = WiFi.gatewayIP().toString();
      if (dnsOk) {
        Serial.printf("[Weather] OWM %s DNS %s -> %s gw=%s\n",
                      protocol, host, resolvedIp.c_str(), gatewayIp.c_str());
      } else {
        Serial.printf("[Weather] OWM %s DNS failed for %s gw=%s\n",
                      protocol, host, gatewayIp.c_str());
      }
      String url = String(protocol) + "://api.openweathermap.org" + path;
      Serial.printf("[Weather] OWM %s request\n", protocol);
      if (!http.begin(client, url)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "owm %s ip=%s gw=%s begin failed",
                 protocol, resolvedIp.c_str(), gatewayIp.c_str());
        setWeatherDiag(0, msg, keyLen, "owm");
        Serial.printf("[Weather] %s\n", msg);
        return false;
      }

      int code = http.GET();
      bool ok = false;
      if (code == 200) {
        ok = applyWeatherPayload(http);
      } else {
        String err = http.errorToString(code);
        char msg[96];
        if (useHttps) {
          char tlsErr[64] = "";
          int tlsCode = secureClient ? secureClient->lastError(tlsErr, sizeof(tlsErr)) : 0;
          if (tlsCode != 0) {
            snprintf(msg, sizeof(msg), "owm https ip=%s gw=%s HTTP %d: %s; TLS %d: %s",
                     resolvedIp.c_str(), gatewayIp.c_str(), code, err.c_str(), tlsCode, tlsErr);
          } else {
            snprintf(msg, sizeof(msg), "owm https ip=%s gw=%s HTTP %d: %s",
                     resolvedIp.c_str(), gatewayIp.c_str(), code, err.c_str());
          }
        } else {
          snprintf(msg, sizeof(msg), "owm http ip=%s gw=%s HTTP %d: %s",
                   resolvedIp.c_str(), gatewayIp.c_str(), code, err.c_str());
        }
        setWeatherDiag(code, msg, keyLen, "owm");
        Serial.printf("[Weather] %s\n", msg);
      }
      http.end();
      return ok;
    };

    // Сначала HTTP: быстрее после холодного старта и без TLS-handshake; HTTPS остаётся фоллбэком.
    {
      WiFiClient client;
      client.setTimeout(8000);
      owmOk = runOwmRequest(client, false, nullptr);
    }
    if (!owmOk) {
      BambuCloudExternalTlsPause cloudPause;
      if (!cloudPause.acquired()) {
        setWeatherDiag(0, "owm https deferred: active print", keyLen, "owm");
        if (provider == 1) return;
      } else {
        SecureNetworkLease tlsLease(secureNetworkCoordinator, SECURE_NETWORK_WEATHER);
        if (!tlsLease.acquired()) {
          setWeatherDiag(0, "owm https deferred: tls busy", keyLen, "owm");
        } else {
          WiFiClientSecure client;
          client.setInsecure(); // CA не проверяем ради RAM.
          // Ядро ESP32 Arduino 3.x: setTimeout() — миллисекунды, setHandshakeTimeout() — секунды.
          client.setTimeout(8000);
          client.setHandshakeTimeout(8);
          owmOk = runOwmRequest(client, true, &client);
        }
      }
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

  // Как фоллбэк — таймауты короче основного, чтобы при недоступности обоих API
  // weatherTask не блокировался дольше необходимого; как основной — полные 8 с.
  uint16_t meteoTimeout = (provider == 2) ? 8000 : 5000;
  // timezone=auto: в ответ добавляется utc_offset_seconds локации
  String meteoPath = String("/v1/forecast?latitude=") + String(owmLat, 4) +
                     "&longitude=" + String(owmLon, 4) +
                     "&current=temperature_2m&timezone=auto";

  auto applyMeteoPayload = [&](int meteoCode, HTTPClient& http) -> bool {
    int contentLength = http.getSize();
    if (contentLength > static_cast<int>(MAX_WEATHER_JSON_BYTES)) {
      setWeatherDiag(meteoCode, "open-meteo json: oversized", keyLen, "open-meteo");
      return false;
    }
    JsonDocument filter;
    filter["current"]["temperature_2m"] = true;
    filter["current_weather"]["temperature"] = true;
    filter["utc_offset_seconds"] = true;
    JsonDocument doc;
    BoundedInputStream input(http.getStream(), MAX_WEATHER_JSON_BYTES);
    DeserializationError err = deserializeJson(
      doc, input, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(4));
    bool bodyAccepted = boundedHttpBodyAccepted(
      http, input, contentLength, MAX_WEATHER_JSON_BYTES);
    if (err || !bodyAccepted) {
      char msg[64];
      snprintf(msg, sizeof(msg), "open-meteo json: %s",
               !bodyAccepted ? "oversized or unterminated" : err.c_str());
      setWeatherDiag(meteoCode, msg, keyLen, "open-meteo");
      Serial.printf("[Weather] %s\n", msg);
      return false;
    } else {
      float t = doc["current"]["temperature_2m"] | NAN;
      if (isnan(t)) t = doc["current_weather"]["temperature"] | NAN;
      if (isnan(t)) {
        setWeatherDiag(meteoCode, "open-meteo missing temperature", keyLen, "open-meteo");
        Serial.println("[Weather] Open-Meteo missing temperature");
        return false;
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
        return true;
      }
    }
  };

  auto runMeteoRequest = [&](WiFiClient& client, bool useHttps, WiFiClientSecure* secureClient) -> bool {
    HTTPClient meteoHttp;
    meteoHttp.setConnectTimeout(meteoTimeout);
    meteoHttp.setTimeout(meteoTimeout);
    meteoHttp.setReuse(false);
    meteoHttp.useHTTP10(true);

    const char* protocol = useHttps ? "https" : "http";
    const char* host = "api.open-meteo.com";
    IPAddress resolved;
    bool dnsOk = WiFi.hostByName(host, resolved);
    String resolvedIp = dnsOk ? resolved.toString() : String("dns-fail");
    String gatewayIp = WiFi.gatewayIP().toString();
    if (dnsOk) {
      Serial.printf("[Weather] Open-Meteo %s DNS %s -> %s gw=%s\n",
                    protocol, host, resolvedIp.c_str(), gatewayIp.c_str());
    } else {
      Serial.printf("[Weather] Open-Meteo %s DNS failed for %s gw=%s\n",
                    protocol, host, gatewayIp.c_str());
    }
    String meteoUrl = String(protocol) + "://api.open-meteo.com" + meteoPath;
    Serial.printf("[Weather] Open-Meteo %s request\n", protocol);
    if (!meteoHttp.begin(client, meteoUrl)) {
      char msg[104];
      snprintf(msg, sizeof(msg), "open-meteo %s ip=%s gw=%s begin failed",
               protocol, resolvedIp.c_str(), gatewayIp.c_str());
      setWeatherDiag(0, msg, keyLen, "open-meteo");
      Serial.printf("[Weather] %s\n", msg);
      return false;
    }

    int meteoCode = meteoHttp.GET();
    bool ok = false;
    if (meteoCode == 200) {
      ok = applyMeteoPayload(meteoCode, meteoHttp);
    } else {
      String meteoErr = meteoHttp.errorToString(meteoCode);
      char msg[96];
      if (useHttps) {
        char tlsErr[64] = "";
        int tlsCode = secureClient ? secureClient->lastError(tlsErr, sizeof(tlsErr)) : 0;
        if (tlsCode != 0) {
          snprintf(msg, sizeof(msg), "open-meteo https ip=%s gw=%s HTTP %d: %s; TLS %d: %s",
                   resolvedIp.c_str(), gatewayIp.c_str(), meteoCode, meteoErr.c_str(), tlsCode, tlsErr);
        } else {
          snprintf(msg, sizeof(msg), "open-meteo https ip=%s gw=%s HTTP %d: %s",
                   resolvedIp.c_str(), gatewayIp.c_str(), meteoCode, meteoErr.c_str());
        }
      } else {
        snprintf(msg, sizeof(msg), "open-meteo http ip=%s gw=%s HTTP %d: %s",
                 resolvedIp.c_str(), gatewayIp.c_str(), meteoCode, meteoErr.c_str());
      }
      setWeatherDiag(meteoCode, msg, keyLen, "open-meteo");
      Serial.printf("[Weather] %s\n", msg);
    }
    meteoHttp.end();
    return ok;
  };

  bool meteoOk = false;
  {
    WiFiClient client;
    client.setTimeout(meteoTimeout);
    meteoOk = runMeteoRequest(client, false, nullptr);
  }
  if (!meteoOk) {
    BambuCloudExternalTlsPause cloudPause;
    if (!cloudPause.acquired()) {
      setWeatherDiag(0, "open-meteo https deferred: active print", keyLen,
                     "open-meteo");
      return;
    }
    SecureNetworkLease tlsLease(secureNetworkCoordinator, SECURE_NETWORK_WEATHER);
    if (!tlsLease.acquired()) {
      setWeatherDiag(0, "open-meteo https deferred: tls busy", keyLen, "open-meteo");
      return;
    }
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(meteoTimeout);
    client.setHandshakeTimeout(meteoTimeout / 1000);
    runMeteoRequest(client, true, &client);
  }
}

// Запрашивает актуальные времена восхода и заката с api.sunrise-sunset.org.
// Использует координаты из cfg (owmLat/owmLon) и tzOffset для конвертации UTC→local.
// Вызывается только из weatherTask.
void fetchSunriseSunset() {
  if (!wifiConnected) return;
  BambuCloudExternalTlsPause cloudPause;
  if (!cloudPause.acquired()) {
    Serial.println("[Sun] Direct Cloud печатает — запрос отложен");
    return;
  }
  SecureNetworkLease tlsLease(secureNetworkCoordinator, SECURE_NETWORK_SUNRISE);
  if (!tlsLease.acquired()) {
    Serial.println("[Sun] TLS занят — запрос отложен");
    return;
  }
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
    int contentLength = http.getSize();
    if (contentLength > static_cast<int>(MAX_WEATHER_JSON_BYTES)) {
      Serial.println("[Sun] JSON ошибка: oversized");
      http.end();
      return;
    }
    JsonDocument filter;
    filter["results"]["sunrise"] = true;
    filter["results"]["sunset"] = true;
    JsonDocument doc;
    // This endpoint has historically been unreliable when parsed directly from
    // HTTPClient::getStream(); retain the bounded small body copy here.
    String body = http.getString();
    if (body.length() > MAX_WEATHER_JSON_BYTES) {
      Serial.println("[Sun] JSON ошибка: oversized");
      http.end();
      return;
    }
    DeserializationError err = deserializeJson(
      doc, body, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(4));
    if (err) {
      Serial.printf("[Sun] JSON ошибка: %s\n", err.c_str());
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
  }
  http.end();
}

// ════════════════════════════════════════════════════════════
//  КОНФИГ: сохранение / загрузка
// ════════════════════════════════════════════════════════════
bool saveConfig() {
  // Снимок под cfgMtx — Wi-Fi state machine может менять cfg во время NVS-записи
  StoredStateBlob blob;
  xSemaphoreTake(cfgMtx, portMAX_DELAY);
  blob.cfg = cfg;
  xSemaphoreGive(cfgMtx);
  xSemaphoreTake(bambuCfgMtx, portMAX_DELAY);
  blob.bambuCfg = bambuCfg;
  blob.bambuCfg.relayEnabled = true;
  bambuCfg.relayEnabled = true;
  xSemaphoreGive(bambuCfgMtx);
  blob.checksum = storedStateChecksum(blob);
  // NVS-запись под prefsMtx (занимает сотни мкс — мьютекс, не spinlock)
  xSemaphoreTake(prefsMtx, portMAX_DELAY);
  bool ok = prefs.begin("matrix", false);
  if (ok) {
    ok = prefs.putBytes("state", &blob, sizeof(blob)) == sizeof(blob);
    if (ok) {
      ok = prefs.putUChar(PREF_PRINTER_SOURCE,
                          static_cast<uint8_t>(printerSourceMode.load())) == 1;
    }
    if (ok) {
      // T06: фиксируем, что одноразовая миграция дефолта источника выполнена.
      ok = prefs.putUChar(PREF_SOURCE_DEFAULT_MIGRATED, 1) == 1;
    }
    // Remove legacy split keys, including obsolete account credential buffers.
    prefs.remove("bcfg");
    prefs.remove("bver");
    prefs.remove("bcsz");
    prefs.remove("bserial");
    prefs.remove("btok");
    prefs.remove("btver");
    prefs.remove("btsz");
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
  bool migratedLegacyState = false;
  bool migrateSourceDefault = false;  // T06: одноразовая миграция дефолта источника
  if (prefs.begin("matrix", true)) {
    StoredStateBlob blob;
    if (loadStoredStateBlob(blob)) {
      cfg = blob.cfg;
      sanitizeLoadedConfig(cfg);
      bambuCfg = blob.bambuCfg;
    } else {
      LegacyStoredStateBlobV1 legacyBlob = {};
      if (loadLegacyStoredStateBlob(legacyBlob)) {
        cfg = legacyBlob.cfg;
        sanitizeLoadedConfig(cfg);
        bambuCfg = migrateLegacyBambuConfig(legacyBlob.bambuCfg);
        migratedLegacyState = true;
        Serial.println("[NVS] Migrated state v1; obsolete Bambu credentials removed");
      } else {
        uint16_t ver = prefs.getUShort("ver", 0);
        uint16_t csz = prefs.getUShort("csz", 0);
        if (loadConfigFromNvs(ver, csz, cfg)) sanitizeLoadedConfig(cfg);
        uint16_t bver = prefs.getUShort("bver", 0);
        uint16_t bsz = prefs.getUShort("bcsz", 0);
        if (bver == 2 && bsz == (uint16_t)sizeof(LegacyBambuConfigV2) && prefs.isKey("bcfg")) {
          LegacyBambuConfigV2 legacyBambu;
          size_t got = prefs.getBytes("bcfg", &legacyBambu, sizeof(legacyBambu));
          if (got == sizeof(legacyBambu)) {
            bambuCfg = migrateLegacyBambuConfig(legacyBambu);
            migratedLegacyState = true;
          } else {
            Serial.printf("[NVS] getBytes(bcfg legacy) size mismatch: %u\n", (unsigned)got);
          }
          secureZero(&legacyBambu, sizeof(legacyBambu));
        }
      }
      secureZero(&legacyBlob, sizeof(legacyBlob));
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
    // T06: cloud_direct — дефолт для новых и мигрированных установок.
    // Одноразовая миграция: marker отсутствует → источник становится
    // cloud_direct (relay остаётся effective-fallback, пока direct-сессия не
    // готова, и явным rollback-выбором пользователя). После первой успешной
    // записи marker дальнейшие загрузки чтят сохранённый выбор как есть.
    const bool sourceDefaultMigrated = prefs.isKey(PREF_SOURCE_DEFAULT_MIGRATED);
    uint8_t storedSource = prefs.getUChar(
        PREF_PRINTER_SOURCE, PRINTER_SOURCE_CLOUD_DIRECT);
    if (!sourceDefaultMigrated) {
      storedSource = PRINTER_SOURCE_CLOUD_DIRECT;
      migrateSourceDefault = true;
    }
    printerSourceMode.store(storedSource == PRINTER_SOURCE_CLOUD_DIRECT
                                ? PRINTER_SOURCE_CLOUD_DIRECT
                                : PRINTER_SOURCE_RELAY);
    bambuCfg.relayEnabled = true;
    prefs.end();
  } else {
    Serial.println("[NVS] prefs.begin(read) failed, using defaults");
    lastAutoBrightnessValid.store(false);
    bambuCfg.relayEnabled = true;
    // Свежая установка (нет namespace "matrix") — дефолт T06 применяется
    // и фиксируется marker-ключом первой же записью saveConfig().
    migrateSourceDefault = true;
  }
  xSemaphoreGive(prefsMtx);
  xSemaphoreGive(bambuCfgMtx);
  xSemaphoreGive(cfgMtx);

  if (migratedLegacyState || migrateSourceDefault) {
    if (!saveConfig()) Serial.println("[NVS] Failed to persist state/source migration");
  }

  const bool printerConfigured = bambuCfg.relayUrl[0] != '\0';
  relayHttpSource.configure(
      bambuCfg.enabled, printerConfigured,
      bambuCfg.enabled
          ? (printerConfigured ? "Bambu relay ждёт WiFi" : "Заполните Relay URL")
          : "Bambu выключен");
}

// ════════════════════════════════════════════════════════════
//  ВСТРОЕННЫЙ ВЕБ-ИНТЕРФЕЙС
// ════════════════════════════════════════════════════════════
#include "index_html_gz.h"

// ════════════════════════════════════════════════════════════
//  ВЕБ-СЕРВЕР: обработчики
// ════════════════════════════════════════════════════════════
static void sendJsonDocument(int code, JsonDocument& doc) {
  if (doc.overflowed()) {
    server.send(500, "application/json", "{\"error\":\"json_no_memory\"}");
    return;
  }
  // ArduinoJson writes to a generic Print one byte at a time. Sending it
  // directly to WiFiClient turned a ~1.7 KB status response into hundreds of
  // tiny TCP writes (10–25 s on the live C3), so WebUI polling overlapped and
  // kept the radio/CPU hot. WebServer sends a String body in bounded chunks;
  // reuse its capacity to avoid steady-state heap fragmentation.
  static String payload;
  payload.remove(0);
  const size_t jsonBytes = measureJson(doc);
  if (!payload.reserve(jsonBytes + 1) ||
      serializeJson(doc, payload) != jsonBytes) {
    payload.remove(0);
    server.send(500, "application/json", "{\"error\":\"json_serialize_failed\"}");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", payload);
  payload.remove(0);
}

static bool parseBoundedBody(JsonDocument& doc, size_t maximumBytes) {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"missing_body\"}");
    return false;
  }
  const String& body = server.arg("plain");
  if (body.length() == 0 || body.length() > maximumBytes) {
    server.send(413, "application/json", "{\"error\":\"body_too_large\"}");
    return false;
  }
  DeserializationError error = deserializeJson(doc, body, DeserializationOption::NestingLimit(4));
  if (error) {
    const char* code = error == DeserializationError::NoMemory ? "json_no_memory" : "bad_json";
    server.send(400, "application/json", String("{\"error\":\"") + code + "\"}");
    return false;
  }
  return true;
}

static const char* bambuAuthPhaseName(BambuAuthPhase phase) {
  switch (phase) {
    case BAMBU_AUTH_SIGNED_OUT: return "signed_out";
    case BAMBU_AUTH_SUBMITTING_CREDENTIALS: return "submitting_credentials";
    case BAMBU_AUTH_VERIFICATION_EMAIL: return "verification_required_email";
    case BAMBU_AUTH_VERIFICATION_SMS: return "verification_required_sms";
    case BAMBU_AUTH_VERIFICATION_TOTP: return "verification_required_totp";
    case BAMBU_AUTH_FETCHING_PRINTERS: return "fetching_printers";
    case BAMBU_AUTH_PRINTER_SELECTION: return "printer_selection_required";
    case BAMBU_AUTH_CONNECTING: return "connecting";
    case BAMBU_AUTH_READY: return "ready";
    case BAMBU_AUTH_RECOVERABLE_ERROR: return "recoverable_error";
    default: return "recoverable_error";
  }
}

static void appendBambuCloudStatus(JsonDocument& doc,
                                   const BambuCloudPublicStatus& status,
                                   bool includePrinters) {
  doc["state"] = bambuAuthPhaseName(status.phase);
  doc["operationId"] = status.operationId;
  doc["operationExpiresIn"] = status.operationExpiresIn;
  doc["error"] = status.error;
  doc["configured"] = status.hasActiveCredential;
  doc["candidatePending"] = status.candidatePending;
#ifdef BAMBU_AUTH_TRANSPORT_PROBE
  doc["transportApplicationJson"] = status.authTransportApplicationJson;
  doc["transportProfile"] = status.authTransportProfile;
  doc["transport200Length"] = status.authTransport200Length;
  doc["transport200Read"] = status.authTransport200Read;
  doc["transport200First"] = status.authTransport200First;
  doc["transport200Error"] = status.authTransport200Error;
  doc["transportCode"] = status.authTransport200Code;
  char shapeText[7];
  snprintf(shapeText, sizeof(shapeText), "%04x", status.authTransportShape);
  doc["transportShape"] = shapeText;
  doc["transportCall"] = status.authTransportCall;
  JsonArray keys = doc["transportKeys"].to<JsonArray>();
  for (uint8_t i = 0; i < status.authTransportKeyCount && i < 12; ++i)
    keys.add(status.authTransportKeys[i]);
  String tail;
  for (uint8_t i = 0; i < status.authTransportTailLen && i < 16; ++i)
    tail += String(status.authTransportTail[i]);
  doc["transportTail"] = tail;
#endif
  doc["mqttConnected"] = status.mqttConnected;
  doc["firstValidReport"] = status.firstValidReport;
  doc["mqttReconnectCount"] = status.mqttReconnectCount;
  doc["rejectedReports"] = status.rejectedReports;
  doc["maximumReportBytes"] = status.maximumReportBytes;
  doc["automaticRefreshPending"] = status.automaticRefreshPending;
  doc["automaticRefreshFailures"] = status.automaticRefreshFailures;
  doc["nextRefreshAttemptIn"] = status.nextRefreshAttemptIn;
  if (includePrinters) {
    JsonArray printers = doc["printers"].to<JsonArray>();
    for (uint8_t i = 0; i < status.printerCount; ++i) {
      JsonObject printer = printers.add<JsonObject>();
      printer["serial"] = status.printers[i].serial;
      printer["name"] = status.printers[i].name;
    }
  }
}

void handleBambuAuthStatus() {
  JsonDocument doc;
  appendBambuCloudStatus(doc, bambuCloud.publicStatus(), true);
  sendJsonDocument(200, doc);
}

void handleBambuAuthLogin() {
  JsonDocument doc;
  if (!parseBoundedBody(doc, 512)) return;
  const char* emailValue = doc["email"] | "";
  const char* passwordValue = doc["password"] | "";
  char email[96] = "";
  char password[96] = "";
  strlcpy(email, emailValue, sizeof(email));
  strlcpy(password, passwordValue, sizeof(password));
  const bool china = strcmp(doc["region"] | "global", "china") == 0;
#ifdef BAMBU_AUTH_TRANSPORT_PROBE
  const int transportProfileValue = doc["transportProfile"] | 0;
  const uint8_t transportProfile =
      transportProfileValue >= 0 && transportProfileValue <= 12
          ? static_cast<uint8_t>(transportProfileValue)
          : UINT8_MAX;
#else
  const uint8_t transportProfile = 0;
#endif
  char operationId[17] = "";
  const bool accepted = bambuCloud.submitLogin(email, password, china,
                                                operationId, transportProfile);
  char* storedPassword = const_cast<char*>(passwordValue);
  if (storedPassword && storedPassword[0]) secureZero(storedPassword, strlen(storedPassword));
  secureZero(password, sizeof(password));
  secureZero(email, sizeof(email));
  doc.clear();
  if (!accepted) {
    server.send(409, "application/json", "{\"error\":\"login_busy_or_invalid\"}");
    return;
  }
  wakeBambuTask();
  JsonDocument response;
  response["accepted"] = true;
  response["operationId"] = operationId;
  sendJsonDocument(202, response);
  secureZero(operationId, sizeof(operationId));
}

void handleBambuAuthVerify() {
  JsonDocument doc;
  if (!parseBoundedBody(doc, 256)) return;
  char operationId[17] = "";
  char code[16] = "";
  strlcpy(operationId, doc["operationId"] | "", sizeof(operationId));
  strlcpy(code, doc["code"] | "", sizeof(code));
  const bool accepted = bambuCloud.submitVerification(operationId, code);
  char* storedCode = const_cast<char*>(doc["code"].as<const char*>());
  if (storedCode && storedCode[0]) secureZero(storedCode, strlen(storedCode));
  secureZero(code, sizeof(code));
  secureZero(operationId, sizeof(operationId));
  doc.clear();
  if (!accepted) {
    server.send(409, "application/json", "{\"error\":\"verification_expired_or_invalid\"}");
    return;
  }
  wakeBambuTask();
  server.send(202, "application/json", "{\"accepted\":true}");
}

void handleBambuPrinters() {
  BambuCloudPublicStatus status = bambuCloud.publicStatus();
  JsonDocument doc;
  appendBambuCloudStatus(doc, status, true);
  sendJsonDocument(200, doc);
}

void handleBambuPrinterSelect() {
  JsonDocument doc;
  if (!parseBoundedBody(doc, 256)) return;
  const bool accepted = bambuCloud.selectPrinter(
      doc["operationId"] | "", doc["serial"] | "");
  doc.clear();
  if (!accepted) {
    server.send(409, "application/json", "{\"error\":\"selection_expired_or_invalid\"}");
    return;
  }
  wakeBambuTask();
  server.send(202, "application/json", "{\"accepted\":true}");
}

void handleBambuLogout() {
  if (!bambuCloud.logout()) {
    server.send(409, "application/json", "{\"error\":\"cloud_busy\"}");
    return;
  }
  wakeBambuTask();
  server.send(202, "application/json", "{\"accepted\":true}");
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("Referrer-Policy", "no-referrer");
  server.sendHeader("Content-Security-Policy",
                    "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
                    "connect-src 'self'; img-src 'self' data:; object-src 'none'; frame-ancestors 'none'");
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html; charset=utf-8",
                reinterpret_cast<PGM_P>(INDEX_HTML_GZ), INDEX_HTML_GZ_LEN);
}

static void appendPrinterStatus(JsonDocument& doc,
                                const PrinterStateSnapshot& snapshot) {
  doc["bambuConfigured"] = snapshot.configured;
  doc["bambuConnected"] = snapshot.transportConnected;
  doc["bambuPrinterOnline"] = snapshot.printerOnline;
  doc["bambuLifecycle"] = snapshot.lifecycle;
  doc["bambuVisibleLifecycle"] = snapshot.visibleLifecycle;
  doc["bambuProgress"] = snapshot.progress;
  doc["bambuCurrentLayer"] = snapshot.currentLayer;
  doc["bambuTotalLayers"] = snapshot.totalLayers;
  doc["bambuRemainingSeconds"] = snapshot.remainingSeconds;
  doc["bambuJobName"] = snapshot.jobName;
  doc["bambuStage"] = snapshot.stage;
  doc["bambuDetail"] = snapshot.detail;
}

void handleGetSettings() {
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
  doc["apiKey"]       = redactSecretForApi(cfg.apiKey);
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
  doc["bambuSource"] = printerSourceModeName(printerSourceMode.load());
  appendPrinterStatus(doc, selectedPrinterSnapshot());
  // Данные восхода/заката хранятся в RAM (не в cfg) — читаем вне мьютекса
  doc["sunTimeValid"]  = sunTimeValid.load();
  { uint32_t st = sunTimes.load();
    doc["sunriseH"] = (int)((st >> 24) & 0xFF);
    doc["sunriseM"] = (int)((st >> 16) & 0xFF);
    doc["sunsetH"]  = (int)((st >>  8) & 0xFF);
    doc["sunsetM"]  = (int)( st        & 0xFF); }
  sendJsonDocument(200, doc);
}

void handlePostSettings() {
  JsonDocument doc;
  if (!parseBoundedBody(doc, MAX_SETTINGS_BODY_BYTES)) return;

  PrinterSourceMode requestedSource = printerSourceMode.load();
  const bool hasRequestedSource = doc["bambuSource"].is<const char*>();
  if (hasRequestedSource &&
      !parsePrinterSourceMode(doc["bambuSource"].as<const char*>(), requestedSource)) {
    server.send(400, "application/json", "{\"error\":\"invalid_bambu_source\"}");
    return;
  }
  if (hasRequestedSource && requestedSource == PRINTER_SOURCE_CLOUD_DIRECT &&
      printerSourceMode.load() != PRINTER_SOURCE_CLOUD_DIRECT &&
      (!bambuCloud.publicStatus().hasActiveCredential ||
       !directSourceReady.load())) {
    server.send(409, "application/json", "{\"error\":\"cloud_direct_not_configured\"}");
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
      if (!isRedactedSecretValue(v)) strlcpy(dst, v, sz);
    }
  };
  // cfgMtx: cfg читается из background tasks и Wi-Fi state machine — защищаем все записи.
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
  cfg.apPass[0] = '\0';
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
  setStr("bambuRelayUrl", bambuCfg.relayUrl, sizeof(bambuCfg.relayUrl));
  setKey("bambuRelayKey", bambuCfg.relayKey, sizeof(bambuCfg.relayKey));
  bool bambuChanged = !bambuConfigEquals(prevBambuCfg, bambuCfg);
  xSemaphoreGive(bambuCfgMtx);
  bool sourceChanged = false;
  if (hasRequestedSource) {
    PrinterSourceMode previous = printerSourceMode.exchange(requestedSource);
    sourceChanged = previous != requestedSource;
  }

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
  bool persistPending = cfgChanged || bambuChanged || sourceChanged;
  if (persistPending) scheduleDeferredSave(1500);
  JsonDocument resp;
  resp["ok"] = true;
  resp["persistPending"] = persistPending;
  sendJsonDocument(200, resp);
}

// ════════════════════════════════════════════════════════════
//  SOAK LOG: автономный журнал T05 в NVS (namespace "soaklog").
//  Прошивка сама копит минимумы/счётчики/аптайм и кольцо boot-записей;
//  вердикт читается GET /api/soak, сброс — POST /api/soak/reset.
// ════════════════════════════════════════════════════════════
static constexpr const char* SOAK_NVS_NAMESPACE = "soaklog";
static SoakState soakState;
static uint32_t soakLastSampleMs = 0;
static uint32_t soakLastCheckpointMs = 0;
static uint32_t soakUptimeRemainderMs = 0;
static uint32_t soakBootShadowMismatches = 0; // счётчики этой загрузки для дельт
static uint32_t soakBootMqttReconnects = 0;

static const char* soakResetReasonName(uint8_t reason) {
  switch (reason) {
    case 1:  return "poweron";
    case 2:  return "sw";
    case 3:  return "panic";
    case 4:  return "int_wdt";
    case 5:  return "task_wdt";
    case 6:  return "wdt";
    case 7:  return "deepsleep";
    case 8:  return "brownout";
    case 9:  return "sdio";
    case 10: return "usb";
    case 11: return "jtag";
    case 12: return "efuse";
    default: return "unknown";
  }
}

static void soakPersist() {
  soakSeal(soakState);
  Preferences nvs;
  if (nvs.begin(SOAK_NVS_NAMESPACE, false)) {
    nvs.putBytes("state", &soakState, sizeof(soakState));
    nvs.end();
  }
}

// Вызывается один раз из setup(): загрузить журнал, дописать boot-запись
// с причиной ресета (диагностика крашей в boot-окне) и сохранить.
static void soakRecordBoot() {
  Preferences nvs;
  if (nvs.begin(SOAK_NVS_NAMESPACE, true)) {
    nvs.getBytes("state", &soakState, sizeof(soakState));
    nvs.end();
  }
  if (!soakValid(soakState)) soakState = soakFresh();
  uint8_t partition = 255;
  const esp_partition_t* run = esp_ota_get_running_partition();
  if (run && run->label[0] == 'a' && run->label[1] == 'p' &&
      run->label[2] == 'p' && run->label[3] >= '0' && run->label[3] <= '9')
    partition = run->label[3] - '0';
  soakPushBoot(soakState, (uint8_t)esp_reset_reason(), partition, SOAK_FW_MINOR,
               soakState.soakUptimeSeconds);
  soakPersist();
  Serial.printf("[Soak] boot #%u, reset=%s, soakUptime=%u s\n",
                (unsigned)soakState.bootCount,
                soakResetReasonName((uint8_t)esp_reset_reason()),
                (unsigned)soakState.soakUptimeSeconds);
}

// Раз в минуту из loop(): сложить минимумы и дельты счётчиков в RAM-копию,
// раз в 10 минут — чекпоинт в NVS (потери при краше <= 10 минут аптайма).
static void soakServicePass() {
  uint32_t now = millis();
  if (soakLastSampleMs != 0 && now - soakLastSampleMs < SOAK_SAMPLE_INTERVAL_MS)
    return;
  if (soakLastSampleMs == 0) soakLastSampleMs = now;
  uint32_t elapsed = now - soakLastSampleMs;
  soakLastSampleMs = now;
  soakUptimeRemainderMs += elapsed;
  soakFoldHeap(soakState, heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               minimumLargestInternalBlock.load());
  soakFoldStack(soakState,
                loopTaskHandle ? (uint16_t)uxTaskGetStackHighWaterMark(loopTaskHandle) : 0,
                weatherTaskHandle ? (uint16_t)uxTaskGetStackHighWaterMark(weatherTaskHandle) : 0,
                bambuTaskHandle ? (uint16_t)uxTaskGetStackHighWaterMark(bambuTaskHandle) : 0,
                0); // mqtt-стек приходит через cloudStatus ниже
  {
    BambuCloudPublicStatus cloudStatus = bambuCloud.publicStatus();
    soakFoldStack(soakState, 0, 0, 0, (uint16_t)cloudStatus.mqttTaskStackReserve);
    taskENTER_CRITICAL(&shadowDiagnosticsMux);
    uint32_t mismatches = shadowDiagnostics.mismatches;
    taskEXIT_CRITICAL(&shadowDiagnosticsMux);
    soakFoldCounters(soakState, mismatches, soakBootShadowMismatches,
                     cloudStatus.mqttReconnectCount, soakBootMqttReconnects);
    soakBootShadowMismatches = mismatches;
    soakBootMqttReconnects = cloudStatus.mqttReconnectCount;
  }
  bool checkpoint = soakLastCheckpointMs == 0 ||
                    now - soakLastCheckpointMs >= SOAK_CHECKPOINT_INTERVAL_MS;
  if (checkpoint) {
    if (soakLastCheckpointMs != 0 || soakState.soakUptimeSeconds != 0) {
      soakState.soakUptimeSeconds += soakUptimeRemainderMs / 1000UL;
    }
    soakUptimeRemainderMs = 0;
    soakLastCheckpointMs = now;
    soakPersist();
  }
}

void handleSoakGet() {
  JsonDocument doc;
  doc["fwVersion"] = FW_VERSION;
  doc["fwPartition"] = esp_ota_get_running_partition()->label;
  doc["soakUptimeSeconds"] = soakState.soakUptimeSeconds;
  doc["soakUptimeHours"] = soakState.soakUptimeSeconds / 3600UL;
  doc["bootCount"] = soakState.bootCount;
  doc["minHeap"] = soakState.minHeap == SOAK_HEAP_UNSET ? 0 : soakState.minHeap;
  doc["minLargestBlock"] = soakState.minLargest == SOAK_HEAP_UNSET ? 0 : soakState.minLargest;
  doc["minStackLoop"] = soakState.minStackLoop == SOAK_STACK_UNSET ? 0 : soakState.minStackLoop;
  doc["minStackWeather"] = soakState.minStackWeather == SOAK_STACK_UNSET ? 0 : soakState.minStackWeather;
  doc["minStackPrinter"] = soakState.minStackPrinter == SOAK_STACK_UNSET ? 0 : soakState.minStackPrinter;
  doc["minStackMqtt"] = soakState.minStackMqtt == SOAK_STACK_UNSET ? 0 : soakState.minStackMqtt;
  doc["shadowMismatchesTotal"] = soakState.shadowMismatchesTotal;
  doc["mqttReconnectsTotal"] = soakState.mqttReconnectsTotal;
  doc["checkpointCount"] = soakState.checkpointCount;
  const SoakBootRecord* latest = soakLatestBoot(soakState);
  if (latest) {
    doc["currentBoot"]["seq"] = latest->seq;
    doc["currentBoot"]["resetReason"] = soakResetReasonName(latest->resetReason);
    doc["currentBoot"]["resetReasonCode"] = latest->resetReason;
    doc["currentBoot"]["partition"] = latest->partition == 0 ? "app0" :
                                      latest->partition == 1 ? "app1" : "unknown";
  }
  JsonArray boots = doc["boots"].to<JsonArray>();
  for (int i = SOAK_BOOT_RING_CAPACITY - 1; i >= 0; --i) {
    const SoakBootRecord& record = soakState.boots[i];
    if (record.seq == 0) continue;
    JsonObject entry = boots.add<JsonObject>();
    entry["seq"] = record.seq;
    entry["resetReason"] = soakResetReasonName(record.resetReason);
    entry["partition"] = record.partition == 0 ? "app0" :
                         record.partition == 1 ? "app1" : "unknown";
    entry["fwMinor"] = record.fwMinor;
    entry["soakUptimeAtBoot"] = record.soakUptimeAtBoot;
  }
  sendJsonDocument(200, doc);
}

void handleSoakReset() {
  soakState = soakFresh();
  soakLastSampleMs = 0;
  soakLastCheckpointMs = 0;
  soakUptimeRemainderMs = 0;
  soakBootShadowMismatches = 0;
  soakBootMqttReconnects = 0;
  soakPersist();
  Serial.println("[Soak] журнал сброшен владельцем, soak перезапущен");
  JsonDocument resp;
  resp["ok"] = true;
  sendJsonDocument(200, resp);
}

void handleStatus() {
  JsonDocument doc;
  doc["wifi"]   = wifiConnected.load();
  doc["ap"]     = apMode.load();
  // cfg.ssid пишется Wi-Fi state machine под cfgMtx — читаем тоже под ним
  { char ssid[sizeof(cfg.ssid)];
    xSemaphoreTake(cfgMtx, portMAX_DELAY);
    strlcpy(ssid, cfg.ssid, sizeof(ssid));
    xSemaphoreGive(cfgMtx);
    doc["ssid"] = ssid; }
  doc["ip"]     = wifiConnected.load() ? WiFi.localIP().toString() : "192.168.4.1";
  doc["apIP"]   = "192.168.4.1";
  doc["apSsid"] = AP_NAME;
  doc["gateway"] = wifiConnected.load() ? WiFi.gatewayIP().toString() : "";
  doc["subnet"]  = wifiConnected.load() ? WiFi.subnetMask().toString() : "";
  doc["dns0"]    = wifiConnected.load() ? WiFi.dnsIP(0).toString() : "";
  doc["dns1"]    = wifiConnected.load() ? WiFi.dnsIP(1).toString() : "";
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
  doc["uptimeSeconds"] = millis() / 1000UL;
  doc["fwVersion"]  = FW_VERSION;
  doc["fwPartition"] = esp_ota_get_running_partition()->label; // app0/app1 — виден фактический слот
  doc["localSecurityLevel"] = LOCAL_SECURITY_LEVEL;
  doc["otaPasswordRequired"] = OTA_PASSWORD_REQUIRED;
  doc["otaSignatureRequired"] = OTA_SIGNATURE_REQUIRED;
  uint32_t currentInternalHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint32_t minimumInternalHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint32_t largestInternalBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  doc["heap"]       = String(currentInternalHeap / 1024) + " KB";
  doc["heapInternalCurrent"] = currentInternalHeap;
  doc["heapInternalMinimum"] = minimumInternalHeap;
  doc["heapInternalLargest"] = largestInternalBlock;
  uint32_t minLargest = minimumLargestInternalBlock.load();
  doc["heapInternalMinimumLargest"] = minLargest == UINT32_MAX ? largestInternalBlock : minLargest;
  JsonObject stacks = doc["taskStackFree"].to<JsonObject>();
  stacks["loop"] = loopTaskHandle ? uxTaskGetStackHighWaterMark(loopTaskHandle) : 0;
  stacks["weather"] = weatherTaskHandle ? uxTaskGetStackHighWaterMark(weatherTaskHandle) : 0;
  stacks["printer"] = bambuTaskHandle ? uxTaskGetStackHighWaterMark(bambuTaskHandle) : 0;
  stacks["wifi"] = 0; // Wi-Fi connect is part of loop; no reserved task stack.
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
  { PrinterStateSnapshot printer = selectedPrinterSnapshot();
    doc["bambuEnabled"] = printer.enabled;
    appendPrinterStatus(doc, printer); }
  {
    BambuCloudPublicStatus cloudStatus = bambuCloud.publicStatus();
    JsonObject cloud = doc["bambuDirect"].to<JsonObject>();
    cloud["state"] = bambuAuthPhaseName(cloudStatus.phase);
    cloud["configured"] = cloudStatus.hasActiveCredential;
    cloud["candidatePending"] = cloudStatus.candidatePending;
    cloud["mqttConnected"] = cloudStatus.mqttConnected;
    cloud["firstValidReport"] = cloudStatus.firstValidReport;
    cloud["error"] = cloudStatus.error;
    cloud["mqttReconnectCount"] = cloudStatus.mqttReconnectCount;
    cloud["rejectedReports"] = cloudStatus.rejectedReports;
    cloud["maximumReportBytes"] = cloudStatus.maximumReportBytes;
    cloud["automaticRefreshPending"] = cloudStatus.automaticRefreshPending;
    cloud["automaticRefreshFailures"] = cloudStatus.automaticRefreshFailures;
    cloud["nextRefreshAttemptIn"] = cloudStatus.nextRefreshAttemptIn;
    cloud["recoveryReason"] = cloudStatus.recoveryReason;
    cloud["recoveryFailures"] = cloudStatus.recoveryFailures;
    cloud["nextRecoveryAttemptIn"] = cloudStatus.nextRecoveryAttemptIn;
    stacks["mqtt"] = cloudStatus.mqttTaskStackReserve;
    cloud["source"] = printerSourceModeName(printerSourceMode.load());
    cloud["effectiveSource"] = printerSourceModeName(
        effectivePrinterSourceMode(printerSourceMode.load(),
                                   directSourceReady.load()));
    PrinterShadowDiagnostics shadow;
    taskENTER_CRITICAL(&shadowDiagnosticsMux);
    shadow = shadowDiagnostics;
    taskEXIT_CRITICAL(&shadowDiagnosticsMux);
    JsonObject comparison = cloud["shadow"].to<JsonObject>();
    comparison["comparisons"] = shadow.comparisons;
    comparison["matches"] = shadow.matches;
    comparison["mismatches"] = shadow.mismatches;
    comparison["lastReasonMask"] = shadow.lastReasonMask;
    JsonArray reasons = comparison["reasonCounts"].to<JsonArray>();
    for (uint8_t i = 0; i < SHADOW_REASON_COUNT; ++i)
      reasons.add(shadow.reasonCounts[i]);
  }
  // Данные восхода/заката — нужны для авто-обновления sunStatusTxt каждые 3 сек
  doc["sunTimeValid"] = sunTimeValid.load();
  { uint32_t snt = sunTimes.load();
    doc["sunriseH"] = (int)((snt >> 24) & 0xFF);
    doc["sunriseM"] = (int)((snt >> 16) & 0xFF);
    doc["sunsetH"]  = (int)((snt >>  8) & 0xFF);
    doc["sunsetM"]  = (int)( snt        & 0xFF); }

  sendJsonDocument(200, doc);
}

// Сканирование WiFi: двухфазный async без блокировки loop().
// Первый вызов: запускает сканирование, возвращает {"scanning":true}.
// Второй вызов: возвращает результаты (браузер повторяет запрос каждые 1 сек).
void handleScanWifi() {
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
  sendJsonDocument(200, doc);
}

// Подключение к сети — немедленно возвращает ответ браузеру.
// Фактическое подключение выполняет неблокирующая loop state machine.
// Браузер получает результат через /api/status polling.
void handleConnectWifi() {
  if (wifiConnPending.load() || wifiConnInProgress.load()) {
    server.send(409, "application/json", "{\"error\":\"connection in progress\"}");
    return;
  }
  JsonDocument doc;
  if (!parseBoundedBody(doc, MAX_CONNECT_BODY_BYTES)) return;
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
  wifiConnPending.store(true);  // заберёт loop state machine
  server.send(200, "application/json", "{\"ok\":true,\"pending\":true}");
}


void handleDisconnectWifi() {
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
  if (!flushDeferredSave(true)) {
    server.send(500, "application/json", "{\"error\":\"save_failed\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
  rebootMs.store(millis()); rebootPending.store(true);
}

void handleWeatherRefresh() {
  weatherRequested = true;
  wakeBambuTask();
  server.send(200, "application/json", "{\"ok\":true}");
}

// ════════════════════════════════════════════════════════════
//  OTA: открытая локальная веб-загрузка /api/update без password/session gate
//  и без подписи (решение владельца 2026-09-03). Целостность образа
//  гарантируют проверка esp_app_desc в первом чанке, валидация Update.end()
//  и boot-guard rollback. Образ пишется в неактивный слот; otadata
//  переключается атомарно в Update.end() — до этого момента обрыв безвреден.
// ════════════════════════════════════════════════════════════
static bool otaUploadAuthorized = false;
static char otaUploadError[64] = "";
static bool otaTlsLeaseHeld = false;
static uint8_t otaPreviousGlobalBrightness = 0;
static bool otaBrightnessOverridden = false;

static void otaResetState() {
  otaUploadAuthorized = false;
}

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
      uint8_t v = (fromBottom < full)
          ? ota_display::kProgressValue
          : scale8(ota_display::kProgressValue, frac);
      if (!v) continue;
      uint8_t hue = 16 + fromBottom * 2;  // снизу оранжевый, к верху желтее
      for (int x = 0; x < MW; x++) px(x, MH - 1 - fromBottom, CHSV(hue, 255, v));
    }
  }
  FastLED.show();
}

// Образ принят, ждём перезагрузку: ровная зелёная заливка до ресета.
static void showOtaDone() {
  fill_solid(leds, NUM_LEDS,
             CRGB(0, ota_display::kDoneGreen, ota_display::kDoneBlue));
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

static bool otaEnterUpdateMode() {
  otaInProgress.store(true);
  secureNetworkCoordinator.requestOta();
  bambuCloud.stopForOta();
  if (!secureNetworkCoordinator.acquireOta()) {
    secureNetworkCoordinator.cancelOtaRequest();
    otaInProgress.store(false);
    return false;
  }
  otaTlsLeaseHeld = true;
  otaPreviousGlobalBrightness = FastLED.getBrightness();
  FastLED.setBrightness(
      ota_display::otaGlobalBrightness(otaPreviousGlobalBrightness));
  otaBrightnessOverridden = true;
  clearMatrix(); FastLED.show();        // прежний кадр гасим; дальше рисуем прогресс
  esp_wifi_set_ps(WIFI_PS_NONE);        // power save рвёт приём при flash-операциях
  return true;
}

static void otaAbortUpdateMode() {
  otaInProgress.store(false);
  if (otaTlsLeaseHeld) {
    secureNetworkCoordinator.release(SECURE_NETWORK_OTA);
    otaTlsLeaseHeld = false;
  }
  showOtaError();
  if (otaBrightnessOverridden) {
    FastLED.setBrightness(otaPreviousGlobalBrightness);
    otaBrightnessOverridden = false;
  }
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   // steady state и в STA, и в AP
}

// Upload-callback: вызывается из server.handleClient() порциями ~1.4 КБ.
// Пока идёт приём, loop() заблокирован внутри handleClient — рендер
// останавливается сам, дополнительной синхронизации не нужно.
void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START: {
      otaResetState();
      otaUploadError[0] = '\0';
      if (!authorizeLocalOtaUpload(false)) {
        strlcpy(otaUploadError, "ota_not_authorized", sizeof(otaUploadError));
        break;
      }
      otaUploadAuthorized = true;
      if (!otaEnterUpdateMode()) {
        strlcpy(otaUploadError, "secure_network_busy", sizeof(otaUploadError));
        break;
      }
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
      if (!Update.begin(expected)) {
        strlcpy(otaUploadError, "update_begin_failed", sizeof(otaUploadError));
        Update.printError(Serial);
      }
      break;
    }
    case UPLOAD_FILE_WRITE: {
      if (!otaUploadAuthorized || !Update.isRunning()) break;
      sampleMemoryMetrics();
      if (up.totalSize == 0 && up.currentSize >= 0x24) {
        // Первый чанк: отличаем app-образ от bootloader/merged.bin — оба
        // начинаются с 0xE9, но esp_app_desc_t (magic 0xABCD5432 по смещению
        // 0x20) есть только у приложения. merged.bin влезает в слот и без этой
        // проверки прошёл бы Update до битого слота.
        uint32_t descMagic = 0;
        memcpy(&descMagic, up.buf + 0x20, sizeof(descMagic));
        if (up.buf[0] != 0xE9 || descMagic != ESP_APP_DESC_MAGIC_WORD) {
          Serial.println("[OTA] Отклонено: не app-образ (загружайте .ino.bin, не merged.bin)");
          strlcpy(otaUploadError, "not_application_image", sizeof(otaUploadError));
          Update.abort();
          break;
        }
      }
      if (Update.write(up.buf, up.currentSize) != up.currentSize) {
        strlcpy(otaUploadError, "flash_write_failed", sizeof(otaUploadError));
        Update.printError(Serial);      // ошибка запомнена в Update, отдадим в final
      } else {
        size_t total = Update.size();
        showOtaProgress((total && total != UPDATE_SIZE_UNKNOWN)
          ? (uint8_t)((uint64_t)Update.progress() * 100 / total) : (uint8_t)255);
      }
      break;
    }
    case UPLOAD_FILE_END: {
      if (Update.end(true)) { // commit boot partition; boot-guard страхует битый образ
        Serial.printf("[OTA] Образ принят: %u байт\n", up.totalSize);
        showOtaDone();                  // зелёная заливка висит до перезагрузки
      } else {
        strlcpy(otaUploadError, "image_validation_failed", sizeof(otaUploadError));
        Update.printError(Serial);
      }
      break;
    }
    case UPLOAD_FILE_ABORTED:
      Update.abort();
      strlcpy(otaUploadError, "upload_aborted", sizeof(otaUploadError));
      otaResetState();
      otaAbortUpdateMode();
      Serial.println("[OTA] Upload прерван клиентом");
      break;
    default: break;
  }
}

void handleUpdateFinal() {
  if (!otaUploadAuthorized) {
    otaResetState();
    if (otaInProgress.load() || otaTlsLeaseHeld) otaAbortUpdateMode();
    server.send(403, "application/json", "{\"error\":\"forbidden\"}");
    return;
  }
  if (Update.hasError() || !Update.isFinished() || otaUploadError[0]) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = otaUploadError[0] ? otaUploadError : Update.errorString();
    Update.abort();
    otaResetState();
    otaAbortUpdateMode();
    sendJsonDocument(400, doc);
    return;
  }
  // Новая прошивка не должна унаследовать счётчик, если OTA прошло в первые 60 с
  xSemaphoreTake(prefsMtx, portMAX_DELAY);
  bootGuardReset();
  xSemaphoreGive(prefsMtx);
  server.send(200, "application/json", "{\"ok\":true}");
  otaResetState();
  otaInProgress.store(false);
  if (otaTlsLeaseHeld) {
    secureNetworkCoordinator.release(SECURE_NETWORK_OTA);
    otaTlsLeaseHeld = false;
  }
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

static void configureAccessPoint(bool keepStaConnected) {
  WiFi.mode(keepStaConnected ? WIFI_AP_STA : WIFI_AP);
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_NAME, nullptr, 6, 0, 4);
  // В AP fallback не нужен постоянно бодрствующий радиомодуль: это только греет кристалл.
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  apMode.store(true);
  Serial.printf("[AP] Запущена: %s → %s  (open, ch6, %s, ps=min)\n",
    AP_NAME, WiFi.softAPIP().toString().c_str(), keepStaConnected ? "AP+STA" : "AP");
}

// Старт fallback AP — вызывать только когда WiFi уже в WIFI_OFF или WIFI_AP.
static void startAP() {
  configureAccessPoint(false);
}

// ════════════════════════════════════════════════════════════
//  НЕБЛОКИРУЮЩЕЕ ПОДКЛЮЧЕНИЕ К WiFi В LOOP TASK
// ════════════════════════════════════════════════════════════
static void serviceWifiConnectStateMachine() {
  uint32_t now = millis();
  if (wifiConnectPhase == WifiConnectPhase::Idle && wifiConnPending.exchange(false)) {
    taskENTER_CRITICAL(&wifiConnMux);
    strlcpy(wifiActiveSsid, wifiPendSsid, sizeof(wifiActiveSsid));
    strlcpy(wifiActivePass, wifiPendPass, sizeof(wifiActivePass));
    secureZero(wifiPendPass, sizeof(wifiPendPass));
    taskEXIT_CRITICAL(&wifiConnMux);
    Serial.printf("[WiFi] Подключаемся к '%s' (loop state machine)...\n", wifiActiveSsid);
    wifiConnInProgress.store(true);
    wifiConnected.store(false);
    WiFi.mode(WIFI_OFF);
    wifiConnectDeadlineMs = now + 300;
    wifiConnectPhase = WifiConnectPhase::RadioOffWait;
    return;
  }

  if (wifiConnectPhase == WifiConnectPhase::RadioOffWait &&
      static_cast<int32_t>(now - wifiConnectDeadlineMs) >= 0) {
    WiFi.mode(WIFI_AP_STA);
    wifiConnectDeadlineMs = now + 300;
    wifiConnectPhase = WifiConnectPhase::StaStartWait;
    return;
  }

  if (wifiConnectPhase == WifiConnectPhase::StaStartWait &&
      static_cast<int32_t>(now - wifiConnectDeadlineMs) >= 0) {
    IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_NAME, nullptr, 6, 0, 4);
    apMode.store(true);
    dnsServer.start(53, "*", apIP);
    configureStaNetworkPolicy();
    WiFi.begin(wifiActiveSsid, wifiActivePass);
    esp_wifi_set_ps(WIFI_PS_NONE);
    wifiConnectStartedMs = now;
    wifiConnectPhase = WifiConnectPhase::Connecting;
    return;
  }

  if (wifiConnectPhase != WifiConnectPhase::Connecting) return;
  int status = WiFi.status();
  if (status != WL_CONNECTED && now - wifiConnectStartedMs < 15000UL) return;

  if (status == WL_CONNECTED) {
    bool sunMode;
    xSemaphoreTake(cfgMtx, portMAX_DELAY);
    strlcpy(cfg.ssid, wifiActiveSsid, sizeof(cfg.ssid));
    strlcpy(cfg.wifiPass, wifiActivePass, sizeof(cfg.wifiPass));
    sunMode = cfg.autoBrSunMode;
    xSemaphoreGive(cfgMtx);
    hasSsid.store(true);
    scheduleDeferredSave();
    wifiConnected.store(true);
    apOffMs.store(0);
    ntpPending.store(true);
    weatherRequested.store(true);
    wakeBambuTask();
    if (sunMode) sunFetchRequested.store(true);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    wifiReconnectPolicy.reset();
    wifiConnFailStatus.store(0);
    wifiConnResult.store(2);
    Serial.println("[WiFi] Подключено!");
  } else {
    wifiConnected.store(false);
    wifiConnFailStatus.store(status);
    wifiConnResult.store(3);
    startAP();
    dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
    if (hasSsid.load()) wifiReconnectPolicy.scheduleAfterFailure(now);
    Serial.printf("[WiFi] Ошибка подключения, статус=%d\n", status);
  }
  secureZero(wifiActivePass, sizeof(wifiActivePass));
  wifiConnInProgress.store(false);
  wifiConnectPhase = WifiConnectPhase::Idle;
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
    // Тумблер гейтит и прямой Cloud (MQTT/TLS/auto-refresh), не только
    // relay-рендер — A/B 2026-09-03 показал безусловный service().
    bambuCloud.service(enabled, wifiConnected.load(), otaInProgress.load());

    if (!enabled) {
      relayHttpSource.configure(false, configured, "Bambu выключен");
      bambuSleepOrWake(2000);
      continue;
    }

    if (!configured) {
      relayHttpSource.configure(true, false, "Заполните Relay URL");
      bambuSleepOrWake(2000);
      continue;
    }

    relayHttpSource.configure(true, true, "Bambu relay ждёт WiFi");

    if (!wifiConnected.load()) {
      relayHttpSource.markUnavailable(false, PRINTER_UNKNOWN,
                                      "Bambu Relay ждёт WiFi");
      bambuSleepOrWake(2000);
      continue;
    }

    uint32_t nextPollMs = relayHttpSource.poll(relayUrl, relayKey);
    observePrinterShadow();
    bambuSleepOrWake(nextPollMs);
  }
}

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  loopTaskHandle = xTaskGetCurrentTaskHandle();
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

  // Soak-журнал: как можно раньше, чтобы краш в ранней инициализации тоже
  // попал в кольцо boot-записей с причиной ресета.
  soakRecordBoot();

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 2500);
  clearMatrix(); FastLED.show();

  randomSeed(esp_random());

  cfgMtx  = xSemaphoreCreateMutex();
  prefsMtx = xSemaphoreCreateMutex();
  bambuCfgMtx = xSemaphoreCreateMutex();
  if (!cfgMtx || !prefsMtx || !bambuCfgMtx) fatalHalt("Failed to create mutexes");

  loadConfig();
  initializeLocalIdentity();
  if (!bambuCloud.begin()) fatalHalt("Failed to initialize Bambu Cloud");
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
      wifiReconnectPolicy.scheduleInitial(millis());
    }
  } else {
    Serial.println("[WiFi] Сеть не задана → AP");
    startAP();
    wifiReconnectPolicy.reset();
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
  server.on("/api/bambu/auth/status",    HTTP_GET,  handleBambuAuthStatus);
  server.on("/api/bambu/auth/login",     HTTP_POST, handleBambuAuthLogin);
  server.on("/api/bambu/auth/verify",    HTTP_POST, handleBambuAuthVerify);
  server.on("/api/bambu/printers",       HTTP_GET,  handleBambuPrinters);
  server.on("/api/bambu/printer/select", HTTP_POST, handleBambuPrinterSelect);
  server.on("/api/bambu/logout",         HTTP_POST, handleBambuLogout);
  server.on("/api/settings",  HTTP_GET,  handleGetSettings);
  server.on("/api/settings",  HTTP_POST, handlePostSettings);
  server.on("/api/status",    HTTP_GET,  handleStatus);
  server.on("/api/soak",      HTTP_GET,  handleSoakGet);
  server.on("/api/soak/reset",HTTP_POST, handleSoakReset);
  server.on("/api/scan",      HTTP_GET,  handleScanWifi);
  server.on("/api/connect",   HTTP_POST, handleConnectWifi);
  server.on("/api/disconnect",HTTP_POST, handleDisconnectWifi);

  server.on("/api/reboot",    HTTP_POST, handleReboot);
  server.on("/api/weather/refresh", HTTP_POST, handleWeatherRefresh);
  server.on("/api/update",    HTTP_POST, handleUpdateFinal, handleUpdateUpload);
  server.onNotFound([](){ server.sendHeader("Location","/"); server.send(302); });
  server.begin();
  Serial.println("Server started");

  Serial.println("[OTA] ArduinoOTA disabled; web OTA without signature");

  // ESP32-C3 одноядерный: фоновые задачи time-slice на том же ядре, WebServer остаётся в loop task.
  BaseType_t weatherTaskOk = xTaskCreatePinnedToCore(
    weatherTask,   // погода в background task — HTTPClient блокирует до 8 сек
    "weather",
    20480,         // стек: HTTPClient + ArduinoJson + WiFiClientSecure/mbedTLS (~12 KB TLS handshake)
    NULL,
    1,             // низкий приоритет
    &weatherTaskHandle,
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
  if (bambuTaskOk != pdPASS) fatalHalt("Failed to create Bambu task");
  Serial.println("Background tasks started; WebServer and WiFi state machine stay in loop task");
}

// ════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════
void loop() {
  sampleMemoryMetrics();
  // Отложенная перезагрузка: ждём 500 мс после ответа браузеру, чтобы TCP успел сброситься
  if (rebootPending.load() && millis() - rebootMs.load() > 1000) ESP.restart();

  // Веб-сервер в loop task (WebServer не потокобезопасен)
  dnsServer.processNextRequest();
  server.handleClient();

  // WebServer invokes the upload callback once per received chunk and then
  // returns here. Keep normal rendering from replacing the OTA frame between
  // chunks, and keep the final green frame intact while the HTTP response is
  // flushed before reboot.
  if (!ota_display::shouldRenderNormalFrame(otaInProgress.load(),
                                             rebootPending.load())) {
    delay(5);
    return;
  }
  serviceWifiConnectStateMachine();

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

  // Успех сбрасывает backoff, следующая реальная потеря связи снова начнётся с 5 с.
  if (wifiConnected.load() && wifiReconnectPolicy.isScheduled()) {
    wifiReconnectPolicy.reset();
  }

  // Автопереподключение к сохранённой сети: bounded exponential backoff
  // 5 → 10 → 20 → 40 → 60 с, затем раз в минуту. Таймер считается от потери
  // связи/окончания неудачной попытки, а не от старта устройства.
  {
    uint32_t now = millis();
    if (!wifiConnected.load() && hasSsid.load() &&
        !wifiConnPending.load() && !wifiConnInProgress.load() &&
        wifiReconnectPolicy.takeDue(now)) {
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
        wifiConnPending.store(true); // заберёт loop state machine
      } else {
        wifiReconnectPolicy.reset();
      }
    }
  }

  // Open onboarding/fallback AP remains available for 60 seconds after STA
  // recovery, then the radio returns to STA-only steady state.
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
    if (wifiConnected.load() && WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Соединение потеряно — перейдём в AP, первая попытка через 5 сек");
      wifiConnected.store(false);
      wifiConnFailStatus.store(WL_CONNECTION_LOST);
      wifiConnResult.store(3);
      wifiReconnectPolicy.scheduleInitial(millis());
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

  // NVS-запись — только из loop task.
  if (savePending.load() && !flushDeferredSave(false) && !savePending.load()) {
    Serial.println("[NVS] Deferred save failed");
  }

  // Автономный soak-журнал: сэмпл раз в минуту, NVS-чекпоинт раз в 10 минут.
  soakServicePass();

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

  // 4 FPS нужны только мигающим/пульсирующим состояниям Bambu. Статичный экран
  // проверяем раз в секунду; showClockTemp() дополнительно пропускает FastLED.show(),
  // если минута, температура, настройки и состояние принтера не изменились.
  PrinterStateSnapshot printer = selectedPrinterSnapshot();
  bool animatedOverlay = printer.visibleLifecycle == PRINTER_PREPARING ||
                         printer.visibleLifecycle == PRINTER_PAUSED ||
                         printer.visibleLifecycle == PRINTER_FINISHED ||
                         printer.visibleLifecycle == PRINTER_ERROR;
  uint32_t drawIntervalMs = animatedOverlay ? 250UL : 1000UL;
  if (millis() - lastDrawMs >= drawIntervalMs) {
    lastDrawMs = millis();
    showClockTemp();
  }

  // 20 Гц достаточно для WebServer/Wi-Fi state machine и снижает число пустых проходов
  // loop() в 2.5 раза относительно предыдущих 50 Гц.
  delay(50);
}
