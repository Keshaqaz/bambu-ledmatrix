#include "bambu_cloud.hpp"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

namespace {

static constexpr size_t kMaximumCloudJsonBytes = 16384;
static constexpr uint32_t kOperationTtlMs = 5UL * 60UL * 1000UL;
static constexpr uint32_t kCandidateReportTtlMs = 90UL * 1000UL;
static constexpr uint32_t kDirectReportStaleMs = 70UL * 1000UL;

// Current api.bambulab.com and *.mqtt.bambulab.com chains converge on this
// DigiCert Global Root G2 CA. Unlike the public weather path, Cloud credentials
// are never sent through setInsecure().
static const char kBambuCloudCa[] PROGMEM = R"PEM(-----BEGIN CERTIFICATE-----
MIIEfjCCA2agAwIBAgIQD+Ayq4RNAzEGxQyOE8iwaDANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBD
QTAeFw0yNDAxMTgwMDAwMDBaFw0zMTExMDkyMzU5NTlaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI
2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx
1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ
q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz
tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ
vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo4IBMDCC
ASwwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUTiJUIBiV5uNu5g/6+rkS7QYX
jzkwHwYDVR0jBBgwFoAUA95QNVbRTLtm8KPiGxvDl7I90VUwDgYDVR0PAQH/BAQD
AgGGMHQGCCsGAQUFBwEBBGgwZjAjBggrBgEFBQcwAYYXaHR0cDovL29jc3AuZGln
aWNlcnQuY24wPwYIKwYBBQUHMAKGM2h0dHA6Ly9jYWNlcnRzLmRpZ2ljZXJ0LmNu
L0RpZ2lDZXJ0R2xvYmFsUm9vdENBLmNydDBABgNVHR8EOTA3MDWgM6Axhi9odHRw
Oi8vY3JsLmRpZ2ljZXJ0LmNuL0RpZ2lDZXJ0R2xvYmFsUm9vdENBLmNybDARBgNV
HSAECjAIMAYGBFUdIAAwDQYJKoZIhvcNAQELBQADggEBAHRBl3jN7+XHBUK0dZnu
hMdoNwD1nCROU3BTIh1TNzRI0bQ0m5+C/dCRzzlqoSAFHUlOi+OiDltWkXTzmQn6
Z8bH5PFBy5sYpc/8cNPoSzhyqcpvvEZvv/Ivc0Up+dzma7vBDJC9WrMRUUlSFSQp
kdXSmphDNkXJsgARmxzc18IN6LYMRiOWlY7RE2F900pPW60BvJHHNCX0bbSRj/Ql
bmVq8wuftBD++D+RS8K++ujpMjFBROyWfBX+woQDGsMazkmgulQdnZrdj476elOL
axRvrSgEorju1kJM7M65z2RUZrfzQYW/1rs8mRUXin6iEtad/Rv1ZI1WGYmWPyBm
pbo=
-----END CERTIFICATE-----
)PEM";

class CloudBoundedStream : public Stream {
 public:
  CloudBoundedStream(Stream& upstream, size_t maximum)
      : upstream_(upstream), maximum_(maximum), read_(0), overflow_(false) {}
  int available() override {
    int count = upstream_.available();
    size_t remaining = read_ < maximum_ ? maximum_ - read_ : 0;
    return count > static_cast<int>(remaining) ? static_cast<int>(remaining) : count;
  }
  int read() override {
    if (read_ >= maximum_) { overflow_ = upstream_.available() > 0; return -1; }
    int value = upstream_.read();
    if (value >= 0) ++read_;
    return value;
  }
  int peek() override {
    if (read_ >= maximum_) return -1;
    return upstream_.peek();
  }
  void flush() override { upstream_.flush(); }
  size_t write(uint8_t) override { return 0; }
  bool overflowed() const { return overflow_ || (read_ >= maximum_ && upstream_.available() > 0); }

 private:
  Stream& upstream_;
  size_t maximum_;
  size_t read_;
  bool overflow_;
};

static void copyCode(char* destination, size_t size, const char* source) {
  if (!destination || size == 0) return;
  strlcpy(destination, source ? source : "cloud_error", size);
}

static JsonVariantConst responseData(const JsonDocument& document) {
  JsonVariantConst data = document["data"];
  return data.is<JsonObjectConst>() ? data : document.as<JsonVariantConst>();
}

static const char* firstString(JsonVariantConst object, const char* a,
                               const char* b = nullptr,
                               const char* c = nullptr) {
  const char* value = object[a] | "";
  if (!value[0] && b) value = object[b] | "";
  if (!value[0] && c) value = object[c] | "";
  return value;
}

static bool cloudJsonRequest(SecureNetworkCoordinator& coordinator,
                             const char* url, const char* method,
                             const char* body, const char* bearer,
                             JsonDocument& response, char* cookieToken,
                             size_t cookieTokenSize, char* error,
                             size_t errorSize) {
  SecureNetworkLease lease(coordinator, SECURE_NETWORK_CLOUD_HTTP);
  if (!lease.acquired()) { copyCode(error, errorSize, "tls_busy"); return false; }
  WiFiClientSecure client;
  client.setCACert(kBambuCloudCa);
  client.setTimeout(12000);
  client.setHandshakeTimeout(12);
  HTTPClient http;
  http.setConnectTimeout(12000);
  http.setTimeout(12000);
  http.setReuse(false);
  http.useHTTP10(true);
  if (!http.begin(client, url)) { copyCode(error, errorSize, "https_begin_failed"); return false; }
  const char* collected[] = {"Set-Cookie"};
  http.collectHeaders(collected, 1);
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "bambu_network_agent/02.07.01.51");
  http.addHeader("X-BBL-Client-Name", "BambuStudio");
  http.addHeader("X-BBL-Client-Type", "slicer");
  http.addHeader("X-BBL-Client-Version", "02.07.01.62");
  http.addHeader("X-BBL-Language", "en-US");
  http.addHeader("X-BBL-OS-Type", "linux");
  http.addHeader("X-BBL-OS-Version", "esp32");
  http.addHeader("X-BBL-Agent-Version", "02.07.01.51");
  if (bearer && bearer[0]) http.addHeader("Authorization", String("Bearer ") + bearer);
  int code = strcmp(method, "GET") == 0 ? http.GET() :
             http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(body ? body : "{}")),
                       body ? strlen(body) : 2);
  if (code != 200) {
    snprintf(error, errorSize, "http_%d", code);
    http.end();
    return false;
  }
  const int contentLength = http.getSize();
  if (contentLength > static_cast<int>(kMaximumCloudJsonBytes)) {
    copyCode(error, errorSize, "response_oversized");
    http.end();
    return false;
  }
  CloudBoundedStream input(http.getStream(), kMaximumCloudJsonBytes);
  DeserializationError jsonError = deserializeJson(
      response, input, DeserializationOption::NestingLimit(8));
  if (jsonError || input.overflowed()) {
    copyCode(error, errorSize,
             jsonError == DeserializationError::NoMemory ? "json_no_memory" : "bad_response");
    http.end();
    return false;
  }
  if (cookieToken && cookieTokenSize) {
    cookieToken[0] = '\0';
    String cookie = http.header("Set-Cookie");
    const char* cursor = cookie.c_str();
    while (cursor && *cursor) {
      while (*cursor == ' ' || *cursor == ';') ++cursor;
      if (strncmp(cursor, "token=", 6) == 0) {
        cursor += 6;
        const char* end = strchr(cursor, ';');
        size_t length = end ? static_cast<size_t>(end - cursor) : strlen(cursor);
        if (length && length < cookieTokenSize) {
          memcpy(cookieToken, cursor, length);
          cookieToken[length] = '\0';
        }
        break;
      }
      cursor = strchr(cursor, ';');
      if (cursor) ++cursor;
    }
    if (cookie.length()) bambuSecureZero(cookie.begin(), cookie.length());
  }
  http.end();
  return true;
}

static uint32_t expiryTimestamp(uint32_t lifetimeSeconds) {
  const time_t now = time(nullptr);
  return now >= 1609459200 ? static_cast<uint32_t>(now) + lifetimeSeconds : 0;
}

class SemaphoreGuard {
 public:
  explicit SemaphoreGuard(SemaphoreHandle_t semaphore) : semaphore_(semaphore) {
    xSemaphoreTake(semaphore_, portMAX_DELAY);
  }
  ~SemaphoreGuard() { xSemaphoreGive(semaphore_); }

 private:
  SemaphoreHandle_t semaphore_;
};

}  // namespace

DirectCloudSource::DirectCloudSource() {
  reducer_.configure(true, false, "Direct Cloud не настроен");
}

PrinterStateSnapshot DirectCloudSource::snapshot() const {
  portENTER_CRITICAL(&mux_);
  PrinterStateSnapshot copy = reducer_.snapshot();
  portEXIT_CRITICAL(&mux_);
  return copy;
}

void DirectCloudSource::configure(bool configured, const char* detail) {
  portENTER_CRITICAL(&mux_);
  reducer_.configure(true, configured, detail);
  portEXIT_CRITICAL(&mux_);
}

void DirectCloudSource::setTransportConnected(bool connected, const char* detail) {
  portENTER_CRITICAL(&mux_);
  if (connected) {
    PrinterStatePatch patch;
    patch.present = PRINTER_PATCH_TRANSPORT_CONNECTED | PRINTER_PATCH_DETAIL;
    patch.transportConnected = true;
    strlcpy(patch.detail, detail ? detail : "MQTT подключен", sizeof(patch.detail));
    reducer_.apply(patch, millis());
  } else {
    reducer_.markUnavailable(false, PRINTER_OFFLINE,
                             detail ? detail : "MQTT отключен");
  }
  portEXIT_CRITICAL(&mux_);
}

bool DirectCloudSource::acceptFragment(const char* data, size_t length,
                                       size_t offset, size_t totalLength,
                                       uint32_t nowMs) {
  uint32_t previous = maximumReportBytes_.load();
  while (totalLength > previous &&
         !maximumReportBytes_.compare_exchange_weak(previous, totalLength)) {}
  if (offset == 0 && !parser_.begin(totalLength)) {
    rejectedReports_++;
    return false;
  }
  if (!parser_.feed(data, length, offset, totalLength)) {
    rejectedReports_++;
    return false;
  }
  if (offset + length != totalLength) return true;
  PrinterStatePatch patch;
  if (!parser_.finish(patch)) {
    rejectedReports_++;
    return false;
  }
  portENTER_CRITICAL(&mux_);
  reducer_.apply(patch, nowMs);
  portEXIT_CRITICAL(&mux_);
  firstValidReport_.store(true);
  lastValidReportMs_.store(nowMs);
  staleMarked_.store(false);
  return true;
}

void DirectCloudSource::resetValidation() {
  firstValidReport_.store(false);
  lastValidReportMs_.store(0);
  staleMarked_.store(false);
}

void DirectCloudSource::service(uint32_t nowMs) {
  portENTER_CRITICAL(&mux_);
  reducer_.advance(nowMs);
  const uint32_t lastReport = lastValidReportMs_.load();
  if (lastReport && !staleMarked_.load() &&
      static_cast<uint32_t>(nowMs - lastReport) >= kDirectReportStaleMs) {
    reducer_.markUnavailable(true, PRINTER_OFFLINE, "Direct MQTT report stale");
    staleMarked_.store(true);
  }
  portEXIT_CRITICAL(&mux_);
}

BambuCloud::BambuCloud(SecureNetworkCoordinator& coordinator)
    : coordinator_(coordinator), mutex_(nullptr), operationMutex_(nullptr),
      credentialTx_(credentialSlots_),
      operationDeadlineMs_(0), mqttClient_(nullptr), mqttLeaseHeld_(false),
      mqttUsingCandidate_(false), initialized_(false) {
  bambuSecureZero(&request_, sizeof(request_));
  bambuSecureZero(&pendingAuth_, sizeof(pendingAuth_));
  bambuSecureZero(&status_, sizeof(status_));
  status_.phase = BAMBU_AUTH_SIGNED_OUT;
}

bool BambuCloud::begin() {
  mutex_ = xSemaphoreCreateMutex();
  operationMutex_ = xSemaphoreCreateMutex();
  if (!mutex_ || !operationMutex_) return false;
  if (!loadCredentials()) return false;
  BambuCloudCredential active;
  const bool hasActive = credentialTx_.active(active);
  xSemaphoreTake(mutex_, portMAX_DELAY);
  status_.hasActiveCredential = hasActive;
  status_.candidatePending = false;
  status_.phase = hasActive ? BAMBU_AUTH_CONNECTING : BAMBU_AUTH_SIGNED_OUT;
  xSemaphoreGive(mutex_);
  source_.configure(hasActive, hasActive ? "Direct Cloud ждёт MQTT" : "Direct Cloud не настроен");
  bambuSecureZero(&active, sizeof(active));
  initialized_ = true;
  return true;
}

void BambuCloud::makeOperationId(char output[17]) {
  const uint32_t a = esp_random();
  const uint32_t b = esp_random();
  snprintf(output, 17, "%08lx%08lx", static_cast<unsigned long>(a),
           static_cast<unsigned long>(b));
}

bool BambuCloud::submitLogin(const char* email, const char* password,
                             bool chinaRegion, char operationId[17]) {
  if (!initialized_ || !email || !email[0] || !password ||
      strlen(email) >= sizeof(request_.account) || strlen(password) < 1 ||
      strlen(password) >= sizeof(request_.password)) return false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (request_.action != ACTION_NONE ||
      (status_.phase != BAMBU_AUTH_SIGNED_OUT &&
       status_.phase != BAMBU_AUTH_READY &&
       status_.phase != BAMBU_AUTH_RECOVERABLE_ERROR)) {
    xSemaphoreGive(mutex_);
    return false;
  }
  bambuSecureZero(&request_, sizeof(request_));
  request_.action = ACTION_LOGIN;
  request_.chinaRegion = chinaRegion;
  strlcpy(request_.account, email, sizeof(request_.account));
  strlcpy(request_.password, password, sizeof(request_.password));
  makeOperationId(request_.operationId);
  strlcpy(status_.operationId, request_.operationId, sizeof(status_.operationId));
  strlcpy(operationId, request_.operationId, 17);
  status_.error[0] = '\0';
  status_.phase = BAMBU_AUTH_SUBMITTING_CREDENTIALS;
  operationDeadlineMs_ = millis() + kOperationTtlMs;
  xSemaphoreGive(mutex_);
  return true;
}

bool BambuCloud::operationMatches(const char* operationId) const {
  return operationId && operationId[0] &&
         strcmp(operationId, status_.operationId) == 0;
}

bool BambuCloud::operationExpired() const {
  return operationDeadlineMs_ != 0 &&
         static_cast<int32_t>(millis() - operationDeadlineMs_) >= 0;
}

bool BambuCloud::submitVerification(const char* operationId, const char* code) {
  if (!initialized_ || !code || strlen(code) < 4 || strlen(code) >= sizeof(request_.code))
    return false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool phaseOk = status_.phase == BAMBU_AUTH_VERIFICATION_EMAIL ||
                       status_.phase == BAMBU_AUTH_VERIFICATION_SMS ||
                       status_.phase == BAMBU_AUTH_VERIFICATION_TOTP;
  if (request_.action != ACTION_NONE || !phaseOk || !operationMatches(operationId) ||
      operationExpired()) {
    xSemaphoreGive(mutex_);
    return false;
  }
  request_.action = ACTION_VERIFY;
  strlcpy(request_.operationId, operationId, sizeof(request_.operationId));
  strlcpy(request_.code, code, sizeof(request_.code));
  xSemaphoreGive(mutex_);
  return true;
}

bool BambuCloud::selectPrinter(const char* operationId, const char* serial) {
  if (!initialized_ || !serial || !serial[0] ||
      strlen(serial) >= sizeof(request_.serial)) return false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (request_.action != ACTION_NONE || status_.phase != BAMBU_AUTH_PRINTER_SELECTION ||
      !operationMatches(operationId) || operationExpired()) {
    xSemaphoreGive(mutex_);
    return false;
  }
  bool found = false;
  for (uint8_t i = 0; i < status_.printerCount; ++i)
    found = found || strcmp(status_.printers[i].serial, serial) == 0;
  if (!found) { xSemaphoreGive(mutex_); return false; }
  request_.action = ACTION_SELECT;
  strlcpy(request_.operationId, operationId, sizeof(request_.operationId));
  strlcpy(request_.serial, serial, sizeof(request_.serial));
  xSemaphoreGive(mutex_);
  return true;
}

bool BambuCloud::requestRefresh() {
  if (!initialized_) return false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (request_.action != ACTION_NONE || !status_.hasActiveCredential) {
    xSemaphoreGive(mutex_);
    return false;
  }
  request_.action = ACTION_REFRESH;
  status_.phase = BAMBU_AUTH_CONNECTING;
  xSemaphoreGive(mutex_);
  return true;
}

bool BambuCloud::logout() {
  if (!initialized_) return false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (request_.action != ACTION_NONE) { xSemaphoreGive(mutex_); return false; }
  request_.action = ACTION_LOGOUT;
  xSemaphoreGive(mutex_);
  return true;
}

BambuCloudPublicStatus BambuCloud::publicStatus() const {
  BambuCloudPublicStatus copy;
  bambuSecureZero(&copy, sizeof(copy));
  if (!mutex_) return copy;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  copy = status_;
  if (operationDeadlineMs_ && !operationExpired())
    copy.operationExpiresIn = (operationDeadlineMs_ - millis()) / 1000UL;
  else
    copy.operationExpiresIn = 0;
  copy.rejectedReports = source_.rejectedReports();
  copy.maximumReportBytes = source_.maximumReportBytes();
  xSemaphoreGive(mutex_);
  return copy;
}

void BambuCloud::clearPendingRequest() {
  bambuSecureZero(&request_, sizeof(request_));
}

void BambuCloud::clearPendingAuth() {
  bambuSecureZero(&pendingAuth_, sizeof(pendingAuth_));
}

void BambuCloud::setPhase(BambuAuthPhase phase, const char* error) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  status_.phase = phase;
  strlcpy(status_.error, error ? error : "", sizeof(status_.error));
  xSemaphoreGive(mutex_);
}

void BambuCloud::fail(const char* reason) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  clearPendingAuth();
  operationDeadlineMs_ = 0;
  status_.phase = BAMBU_AUTH_RECOVERABLE_ERROR;
  status_.operationId[0] = '\0';
  strlcpy(status_.error, reason ? reason : "cloud_error", sizeof(status_.error));
  xSemaphoreGive(mutex_);
}

void BambuCloud::service(bool wifiAvailable, bool otaActive) {
  if (!initialized_) return;
  source_.service(millis());
  SemaphoreGuard operation(operationMutex_);

  const bool expired = operationExpired();
  if (credentialTx_.hasCandidate() &&
      (candidateTransportFailed_.exchange(false) || expired)) {
    stopMqtt();
    credentialTx_.rollback();
    removeCandidateFromNvs();
    mqttUsingCandidate_.store(false);
    clearPendingAuth();
    fail(expired ? "candidate_report_timeout" : "candidate_mqtt_failed");
  } else if (expired) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool expirable = status_.phase != BAMBU_AUTH_SIGNED_OUT &&
                           status_.phase != BAMBU_AUTH_READY;
    if (expirable) {
      clearPendingRequest();
      clearPendingAuth();
      status_.phase = BAMBU_AUTH_RECOVERABLE_ERROR;
      strlcpy(status_.error, "operation_expired", sizeof(status_.error));
      status_.operationId[0] = '\0';
      operationDeadlineMs_ = 0;
    }
    xSemaphoreGive(mutex_);
  }
  if (otaActive || externalTlsPaused_.load()) return;
  if (!wifiAvailable) {
    if (mqttClient_) stopMqtt();
    return;
  }

  PendingRequest request;
  bambuSecureZero(&request, sizeof(request));
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (request_.action != ACTION_NONE) {
    request = request_;
    clearPendingRequest();
  }
  xSemaphoreGive(mutex_);
  if (request.action != ACTION_NONE) processRequest(request);
  bambuSecureZero(&request, sizeof(request));

  finalizeCandidateIfValidated();
  if (!mqttClient_) {
    const BambuCloudPublicStatus current = publicStatus();
    const bool mayConnect = current.phase == BAMBU_AUTH_CONNECTING ||
                            current.phase == BAMBU_AUTH_READY ||
                            current.phase == BAMBU_AUTH_RECOVERABLE_ERROR;
    BambuCloudCredential credential;
    bool available = false;
    const BambuCloudCredential* candidate = credentialTx_.candidate();
    if (candidate) {
      credential = *candidate;
      available = true;
      mqttUsingCandidate_.store(true);
    } else {
      available = credentialTx_.active(credential);
      mqttUsingCandidate_.store(false);
    }
    if (mayConnect && available && coordinator_.available()) {
      source_.resetValidation();
      if (!startMqtt(credential)) {
        if (candidate) candidateTransportFailed_.store(true);
        else fail("mqtt_start_failed");
      }
    }
    bambuSecureZero(&credential, sizeof(credential));
  } else if (!credentialTx_.hasCandidate() && source_.firstValidReport()) {
    setPhase(BAMBU_AUTH_READY);
  }
}

void BambuCloud::processRequest(PendingRequest& request) {
  switch (request.action) {
    case ACTION_LOGIN: processLogin(request); break;
    case ACTION_VERIFY: processVerification(request); break;
    case ACTION_SELECT: processSelection(request); break;
    case ACTION_REFRESH: processRefresh(); break;
    case ACTION_LOGOUT: processLogout(); break;
    default: break;
  }
}

bool BambuCloud::postLogin(const PendingRequest& request, bool verification,
                           char* error, size_t errorSize) {
  JsonDocument requestDoc;
  requestDoc["account"] = request.account;
  if (verification) requestDoc["code"] = request.code;
  else {
    requestDoc["password"] = request.password;
    requestDoc["apiError"] = "";
  }
  char body[512];
  const size_t written = serializeJson(requestDoc, body, sizeof(body));
  char* secretInDocument = const_cast<char*>(
      requestDoc[verification ? "code" : "password"].as<const char*>());
  if (secretInDocument) bambuSecureZero(secretInDocument, strlen(secretInDocument));
  requestDoc.clear();
  if (!written || written >= sizeof(body)) {
    bambuSecureZero(body, sizeof(body));
    copyCode(error, errorSize, "request_oversized");
    return false;
  }
  const char* host = request.chinaRegion ? "https://api.bambulab.cn" :
                                           "https://api.bambulab.com";
  char url[128];
  snprintf(url, sizeof(url), "%s/v1/user-service/user/login", host);
  JsonDocument response;
  bool ok = cloudJsonRequest(coordinator_, url, "POST", body, nullptr,
                             response, nullptr, 0, error, errorSize);
  bambuSecureZero(body, sizeof(body));
  if (!ok) {
    if (!verification &&
        (strcmp(error, "http_400") == 0 || strcmp(error, "http_401") == 0))
      copyCode(error, errorSize, "invalid_credentials");
    return false;
  }
  JsonVariantConst data = responseData(response);
  const char* loginType = data["loginType"] | "";
  if (!verification && strcmp(loginType, "verifyCode") == 0) {
    clearPendingAuth();
    strlcpy(pendingAuth_.account, request.account, sizeof(pendingAuth_.account));
    pendingAuth_.chinaRegion = request.chinaRegion;
    setPhase(strchr(request.account, '@') ? BAMBU_AUTH_VERIFICATION_EMAIL :
                                           BAMBU_AUTH_VERIFICATION_SMS);
    return true;
  }
  if (!verification && strcmp(loginType, "tfa") == 0) {
    clearPendingAuth();
    strlcpy(pendingAuth_.account, request.account, sizeof(pendingAuth_.account));
    strlcpy(pendingAuth_.tfaKey, data["tfaKey"] | "", sizeof(pendingAuth_.tfaKey));
    pendingAuth_.chinaRegion = request.chinaRegion;
    if (!pendingAuth_.tfaKey[0]) { copyCode(error, errorSize, "tfa_key_missing"); return false; }
    setPhase(BAMBU_AUTH_VERIFICATION_TOTP);
    return true;
  }
  const char* access = firstString(data, "accessToken", "access_token");
  const char* refresh = firstString(data, "refreshToken", "refresh_token");
  if (!access[0] || !refresh[0] ||
      strlen(access) >= sizeof(pendingAuth_.accessToken) ||
      strlen(refresh) >= sizeof(pendingAuth_.refreshToken)) {
    if (access[0]) bambuSecureZero(const_cast<char*>(access), strlen(access));
    if (refresh[0]) bambuSecureZero(const_cast<char*>(refresh), strlen(refresh));
    copyCode(error, errorSize, "token_envelope_incomplete");
    return false;
  }
  clearPendingAuth();
  strlcpy(pendingAuth_.account, request.account, sizeof(pendingAuth_.account));
  strlcpy(pendingAuth_.accessToken, access, sizeof(pendingAuth_.accessToken));
  strlcpy(pendingAuth_.refreshToken, refresh, sizeof(pendingAuth_.refreshToken));
  pendingAuth_.accessExpiresIn = data["expiresIn"] | 0;
  pendingAuth_.refreshExpiresIn = data["refreshExpiresIn"] | 0;
  pendingAuth_.chinaRegion = request.chinaRegion;
  bambuSecureZero(const_cast<char*>(access), strlen(access));
  bambuSecureZero(const_cast<char*>(refresh), strlen(refresh));
  return true;
}

void BambuCloud::processLogin(PendingRequest& request) {
  if (mqttClient_) stopMqtt();
  clearPendingAuth();
  char error[64] = "";
  if (!postLogin(request, false, error, sizeof(error))) {
    fail(error[0] ? error : "login_failed");
    return;
  }
  BambuCloudPublicStatus current = publicStatus();
  if (current.phase == BAMBU_AUTH_VERIFICATION_EMAIL ||
      current.phase == BAMBU_AUTH_VERIFICATION_SMS) {
    if (!sendVerificationCode(current.phase == BAMBU_AUTH_VERIFICATION_SMS,
                              error, sizeof(error))) fail(error);
    return;
  }
  if (current.phase == BAMBU_AUTH_VERIFICATION_TOTP) return;
  setPhase(BAMBU_AUTH_FETCHING_PRINTERS);
  if (!discoverAccountAndPrinters(error, sizeof(error))) fail(error);
}

bool BambuCloud::sendVerificationCode(bool sms, char* error, size_t errorSize) {
  JsonDocument requestDoc;
  if (sms) requestDoc["phone"] = pendingAuth_.account;
  else requestDoc["email"] = pendingAuth_.account;
  requestDoc["type"] = "codeLogin";
  char body[256];
  serializeJson(requestDoc, body, sizeof(body));
  const char* url = sms
      ? "https://bambulab.cn/api/v1/user-service/user/sendsmscode"
      : (pendingAuth_.chinaRegion
             ? "https://api.bambulab.cn/v1/user-service/user/sendemail/code"
             : "https://api.bambulab.com/v1/user-service/user/sendemail/code");
  JsonDocument response;
  bool ok = cloudJsonRequest(coordinator_, url, "POST", body, nullptr,
                             response, nullptr, 0, error, errorSize);
  bambuSecureZero(body, sizeof(body));
  requestDoc.clear();
  return ok;
}

bool BambuCloud::postTotp(const char* code, char* error, size_t errorSize) {
  JsonDocument requestDoc;
  requestDoc["tfaKey"] = pendingAuth_.tfaKey;
  requestDoc["tfaCode"] = code;
  char body[384];
  serializeJson(requestDoc, body, sizeof(body));
  char* codeInDocument =
      const_cast<char*>(requestDoc["tfaCode"].as<const char*>());
  if (codeInDocument) bambuSecureZero(codeInDocument, strlen(codeInDocument));
  requestDoc.clear();
  JsonDocument response;
  char cookieToken[BAMBU_CLOUD_TOKEN_CAPACITY] = "";
  bool ok = cloudJsonRequest(coordinator_, "https://bambulab.com/api/sign-in/tfa",
                             "POST", body, nullptr, response, cookieToken,
                             sizeof(cookieToken), error, errorSize);
  bambuSecureZero(body, sizeof(body));
  if (!ok) { bambuSecureZero(cookieToken, sizeof(cookieToken)); return false; }
  JsonVariantConst data = responseData(response);
  const char* access = firstString(data, "accessToken", "access_token");
  const char* refresh = firstString(data, "refreshToken", "refresh_token");
  if (!access[0]) access = cookieToken;
  if (!access[0] || !refresh[0] ||
      strlen(access) >= sizeof(pendingAuth_.accessToken) ||
      strlen(refresh) >= sizeof(pendingAuth_.refreshToken)) {
    if (access[0] && access != cookieToken)
      bambuSecureZero(const_cast<char*>(access), strlen(access));
    if (refresh[0]) bambuSecureZero(const_cast<char*>(refresh), strlen(refresh));
    bambuSecureZero(cookieToken, sizeof(cookieToken));
    copyCode(error, errorSize, "tfa_refresh_contract_missing");
    return false;
  }
  strlcpy(pendingAuth_.accessToken, access, sizeof(pendingAuth_.accessToken));
  strlcpy(pendingAuth_.refreshToken, refresh, sizeof(pendingAuth_.refreshToken));
  pendingAuth_.accessExpiresIn = data["expiresIn"] | 0;
  pendingAuth_.refreshExpiresIn = data["refreshExpiresIn"] | 0;
  if (access != cookieToken)
    bambuSecureZero(const_cast<char*>(access), strlen(access));
  bambuSecureZero(const_cast<char*>(refresh), strlen(refresh));
  bambuSecureZero(cookieToken, sizeof(cookieToken));
  return true;
}

void BambuCloud::processVerification(PendingRequest& request) {
  char error[64] = "";
  BambuCloudPublicStatus current = publicStatus();
  bool ok = false;
  if (current.phase == BAMBU_AUTH_VERIFICATION_TOTP) {
    ok = postTotp(request.code, error, sizeof(error));
  } else {
    PendingRequest verification;
    bambuSecureZero(&verification, sizeof(verification));
    verification.chinaRegion = pendingAuth_.chinaRegion;
    strlcpy(verification.account, pendingAuth_.account, sizeof(verification.account));
    strlcpy(verification.code, request.code, sizeof(verification.code));
    ok = postLogin(verification, true, error, sizeof(error));
    bambuSecureZero(&verification, sizeof(verification));
  }
  if (!ok) { fail(error[0] ? error : "verification_failed"); return; }
  setPhase(BAMBU_AUTH_FETCHING_PRINTERS);
  if (!discoverAccountAndPrinters(error, sizeof(error))) fail(error);
}

bool BambuCloud::discoverAccountAndPrinters(char* error, size_t errorSize) {
  const char* apiHost = pendingAuth_.chinaRegion ? "https://api.bambulab.cn" :
                                                  "https://api.bambulab.com";
  char url[160];
  snprintf(url, sizeof(url), "%s/v1/design-user-service/my/preference", apiHost);
  JsonDocument profile;
  if (!cloudJsonRequest(coordinator_, url, "GET", nullptr,
                        pendingAuth_.accessToken, profile, nullptr, 0,
                        error, errorSize)) return false;
  JsonVariantConst profileData = responseData(profile);
  const char* uid = firstString(profileData, "uid", "userId", "user_id");
  if (!uid[0]) uid = firstString(profileData, "accountId", "account_id");
  if (!uid[0] || strlen(uid) >= sizeof(pendingAuth_.uid)) {
    copyCode(error, errorSize, "account_identity_missing");
    return false;
  }
  strlcpy(pendingAuth_.uid, uid, sizeof(pendingAuth_.uid));

  snprintf(url, sizeof(url), "%s/v1/iot-service/api/user/bind", apiHost);
  JsonDocument directory;
  if (!cloudJsonRequest(coordinator_, url, "GET", nullptr,
                        pendingAuth_.accessToken, directory, nullptr, 0,
                        error, errorSize)) return false;
  JsonVariantConst directoryData = responseData(directory);
  JsonArrayConst devices = directoryData["devices"].as<JsonArrayConst>();
  if (devices.isNull()) devices = directory["devices"].as<JsonArrayConst>();
  uint8_t count = 0;
  BambuCloudPrinter printers[8] = {};
  for (JsonObjectConst device : devices) {
    if (count >= 8) break;
    const char* serial = firstString(device, "dev_id", "devId", "serial");
    if (!serial[0]) serial = device["deviceId"] | "";
    if (!serial[0] || strlen(serial) >= sizeof(printers[count].serial)) continue;
    strlcpy(printers[count].serial, serial, sizeof(printers[count].serial));
    const char* name = firstString(device, "name", "dev_name", "devName");
    strlcpy(printers[count].name, name[0] ? name : "Bambu printer",
            sizeof(printers[count].name));
    ++count;
  }
  if (!count) { copyCode(error, errorSize, "no_printers"); return false; }
  xSemaphoreTake(mutex_, portMAX_DELAY);
  status_.printerCount = count;
  memcpy(status_.printers, printers, sizeof(printers));
  status_.phase = BAMBU_AUTH_PRINTER_SELECTION;
  status_.error[0] = '\0';
  xSemaphoreGive(mutex_);
  return true;
}

void BambuCloud::processSelection(PendingRequest& request) {
  BambuCloudCredential candidate;
  if (!bambuCredentialSet(candidate, pendingAuth_.uid, request.serial,
                          pendingAuth_.accessToken, pendingAuth_.refreshToken,
                          expiryTimestamp(pendingAuth_.accessExpiresIn),
                          expiryTimestamp(pendingAuth_.refreshExpiresIn),
                          pendingAuth_.chinaRegion) || !credentialTx_.begin(candidate) ||
      !persistCandidate()) {
    credentialTx_.rollback();
    removeCandidateFromNvs();
    bambuSecureZero(&candidate, sizeof(candidate));
    fail("candidate_save_failed");
    return;
  }
  bambuSecureZero(&candidate, sizeof(candidate));
  stopMqtt();
  source_.configure(true, "Direct Cloud проверяет candidate");
  source_.resetValidation();
  mqttUsingCandidate_ = true;
  setPhase(BAMBU_AUTH_CONNECTING);
  operationDeadlineMs_ = millis() + kCandidateReportTtlMs;
  const BambuCloudCredential* stored = credentialTx_.candidate();
  if (!stored || !startMqtt(*stored)) {
    credentialTx_.rollback();
    removeCandidateFromNvs();
    mqttUsingCandidate_ = false;
    fail("candidate_mqtt_failed");
  }
}

void BambuCloud::processRefresh() {
  BambuCloudCredential active;
  if (!credentialTx_.active(active)) { fail("no_active_credential"); return; }
  stopMqtt();
  JsonDocument requestDoc;
  requestDoc["refreshToken"] = active.refreshToken;
  char body[768];
  serializeJson(requestDoc, body, sizeof(body));
  char* refreshInDocument =
      const_cast<char*>(requestDoc["refreshToken"].as<const char*>());
  if (refreshInDocument)
    bambuSecureZero(refreshInDocument, strlen(refreshInDocument));
  requestDoc.clear();
  const char* host = active.chinaRegion ? "https://api.bambulab.cn" :
                                         "https://api.bambulab.com";
  char url[160];
  snprintf(url, sizeof(url), "%s/v1/user-service/user/refreshtoken", host);
  JsonDocument response;
  char error[64] = "";
  bool ok = cloudJsonRequest(coordinator_, url, "POST", body, nullptr,
                             response, nullptr, 0, error, sizeof(error));
  bambuSecureZero(body, sizeof(body));
  BambuCloudCredential candidate;
  if (ok) {
    JsonVariantConst data = responseData(response);
    const char* access = firstString(data, "accessToken", "access_token");
    const char* refresh = firstString(data, "refreshToken", "refresh_token");
    ok = bambuBuildRefreshCandidate(
        active, access, refresh, expiryTimestamp(data["expiresIn"] | 0),
        expiryTimestamp(data["refreshExpiresIn"] | 0), candidate);
    if (access[0]) bambuSecureZero(const_cast<char*>(access), strlen(access));
    if (refresh[0]) bambuSecureZero(const_cast<char*>(refresh), strlen(refresh));
  }
  if (ok) ok = credentialTx_.begin(candidate) && persistCandidate();
  if (ok) {
    source_.resetValidation();
    mqttUsingCandidate_ = true;
    operationDeadlineMs_ = millis() + kCandidateReportTtlMs;
    const BambuCloudCredential* stored = credentialTx_.candidate();
    ok = stored && startMqtt(*stored);
  }
  if (!ok) {
    credentialTx_.rollback();
    removeCandidateFromNvs();
    mqttUsingCandidate_ = false;
    source_.resetValidation();
    startMqtt(active);
    fail(error[0] ? error : "refresh_rollback");
  }
  bambuSecureZero(&candidate, sizeof(candidate));
  bambuSecureZero(&active, sizeof(active));
}

void BambuCloud::processLogout() {
  stopMqtt();
  credentialTx_.logout();
  eraseCredentialNvs();
  clearPendingAuth();
  xSemaphoreTake(mutex_, portMAX_DELAY);
  bambuSecureZero(&status_, sizeof(status_));
  status_.phase = BAMBU_AUTH_SIGNED_OUT;
  operationDeadlineMs_ = 0;
  xSemaphoreGive(mutex_);
  source_.configure(false, "Direct Cloud не настроен");
}

bool BambuCloud::startMqtt(const BambuCloudCredential& credential) {
  if (!bambuCredentialValid(credential) || mqttClient_) return false;
  if (!coordinator_.acquire(SECURE_NETWORK_CLOUD_MQTT)) return false;
  mqttLeaseHeld_ = true;
  char uri[96];
  char username[64];
  char clientId[64];
  snprintf(uri, sizeof(uri), "mqtts://%s.mqtt.bambulab.com:8883",
           credential.chinaRegion ? "cn" : "us");
  snprintf(username, sizeof(username), "u_%s", credential.accountUid);
  snprintf(clientId, sizeof(clientId), "ledmatrix-%08lx",
           static_cast<unsigned long>(esp_random()));
  esp_mqtt_client_config_t config = {};
  config.broker.address.uri = uri;
  config.broker.verification.certificate = kBambuCloudCa;
  config.credentials.client_id = clientId;
  config.credentials.username = username;
  config.credentials.authentication.password = credential.accessToken;
  config.session.keepalive = 5;
  config.network.timeout_ms = 12000;
  mqttClient_ = esp_mqtt_client_init(&config);
  if (!mqttClient_) {
    coordinator_.release(SECURE_NETWORK_CLOUD_MQTT);
    mqttLeaseHeld_ = false;
    return false;
  }
  esp_mqtt_client_register_event(mqttClient_, MQTT_EVENT_ANY,
                                 &BambuCloud::mqttEventThunk, this);
  xSemaphoreTake(mutex_, portMAX_DELAY);
  status_.mqttConnected = false;
  status_.firstValidReport = false;
  xSemaphoreGive(mutex_);
  if (esp_mqtt_client_start(mqttClient_) != ESP_OK) {
    esp_mqtt_client_destroy(mqttClient_);
    mqttClient_ = nullptr;
    coordinator_.release(SECURE_NETWORK_CLOUD_MQTT);
    mqttLeaseHeld_ = false;
    return false;
  }
  source_.configure(true, "Direct Cloud подключается");
  return true;
}

void BambuCloud::stopMqtt() {
  mqttStopping_.store(true);
  if (mqttClient_) {
    esp_mqtt_client_stop(mqttClient_);
    esp_mqtt_client_destroy(mqttClient_);
    mqttClient_ = nullptr;
  }
  if (mqttLeaseHeld_) {
    coordinator_.release(SECURE_NETWORK_CLOUD_MQTT);
    mqttLeaseHeld_ = false;
  }
  xSemaphoreTake(mutex_, portMAX_DELAY);
  status_.mqttConnected = false;
  xSemaphoreGive(mutex_);
  source_.setTransportConnected(false, "MQTT остановлен");
  mqttStopping_.store(false);
}

void BambuCloud::stopForOta() {
  if (!operationMutex_) return;
  SemaphoreGuard operation(operationMutex_);
  stopMqtt();
}

void BambuCloud::pauseForExternalTls() {
  externalTlsPaused_.store(true);
  if (!operationMutex_) return;
  SemaphoreGuard operation(operationMutex_);
  stopMqtt();
}

void BambuCloud::resumeAfterExternalTls() {
  externalTlsPaused_.store(false);
}

void BambuCloud::mqttEventThunk(void* handlerArgs, esp_event_base_t,
                                int32_t, void* eventData) {
  static_cast<BambuCloud*>(handlerArgs)->handleMqttEvent(
      static_cast<esp_mqtt_event_handle_t>(eventData));
}

void BambuCloud::handleMqttEvent(esp_mqtt_event_handle_t event) {
  if (!event) return;
  if (event->event_id == MQTT_EVENT_CONNECTED) {
    BambuCloudCredential credential;
    const BambuCloudCredential* candidate = credentialTx_.candidate();
    bool available = candidate ? (credential = *candidate, true) : credentialTx_.active(credential);
    if (available) {
      char reportTopic[96];
      char requestTopic[96];
      snprintf(reportTopic, sizeof(reportTopic), "device/%s/report", credential.serial);
      snprintf(requestTopic, sizeof(requestTopic), "device/%s/request", credential.serial);
      esp_mqtt_client_subscribe(mqttClient_, reportTopic, 1);
      esp_mqtt_client_publish(mqttClient_, requestTopic,
          "{\"info\":{\"sequence_id\":\"0\",\"command\":\"get_version\"}}", 0, 1, 0);
      esp_mqtt_client_publish(mqttClient_, requestTopic,
          "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\"}}", 0, 1, 0);
    }
    bambuSecureZero(&credential, sizeof(credential));
    xSemaphoreTake(mutex_, portMAX_DELAY);
    status_.mqttConnected = true;
    xSemaphoreGive(mutex_);
    source_.setTransportConnected(true, "MQTT подключен, ждём report");
  } else if (event->event_id == MQTT_EVENT_DISCONNECTED) {
    if (mqttUsingCandidate_.load() && !mqttStopping_.load() &&
        !source_.firstValidReport())
      candidateTransportFailed_.store(true);
    xSemaphoreTake(mutex_, portMAX_DELAY);
    status_.mqttConnected = false;
    status_.mqttReconnectCount++;
    xSemaphoreGive(mutex_);
    source_.setTransportConnected(false, "MQTT reconnect");
  } else if (event->event_id == MQTT_EVENT_ERROR) {
    if (mqttUsingCandidate_.load() && !mqttStopping_.load() &&
        !source_.firstValidReport())
      candidateTransportFailed_.store(true);
  } else if (event->event_id == MQTT_EVENT_DATA) {
    source_.acceptFragment(event->data, event->data_len,
                           event->current_data_offset, event->total_data_len,
                           millis());
    xSemaphoreTake(mutex_, portMAX_DELAY);
    status_.firstValidReport = source_.firstValidReport();
    xSemaphoreGive(mutex_);
  }
}

void BambuCloud::finalizeCandidateIfValidated() {
  if (!source_.firstValidReport()) return;
  if (credentialTx_.hasCandidate()) {
    if (!persistPromotion()) {
      stopMqtt();
      credentialTx_.rollback();
      removeCandidateFromNvs();
      mqttUsingCandidate_.store(false);
      clearPendingAuth();
      fail("candidate_commit_failed");
      return;
    }
    mqttUsingCandidate_ = false;
    clearPendingAuth();
    xSemaphoreTake(mutex_, portMAX_DELAY);
    status_.candidatePending = false;
    status_.hasActiveCredential = true;
    status_.operationId[0] = '\0';
    status_.printerCount = 0;
    operationDeadlineMs_ = 0;
    xSemaphoreGive(mutex_);
  }
  setPhase(BAMBU_AUTH_READY);
}

bool BambuCloud::loadCredentials() {
  Preferences store;
  if (!store.begin("bambucloud", false)) return false;
  const char* keys[] = {"active0", "active1", "candidate"};
  BambuCloudCredential* values[] = {&credentialSlots_.active[0],
                                    &credentialSlots_.active[1],
                                    &credentialSlots_.candidate};
  for (size_t i = 0; i < 3; ++i) {
    if (store.getBytesLength(keys[i]) == sizeof(BambuCloudCredential) &&
        store.getBytes(keys[i], values[i], sizeof(BambuCloudCredential)) ==
            sizeof(BambuCloudCredential) && bambuCredentialValid(*values[i])) {
      if (i == 2) credentialSlots_.candidatePresent = true;
    } else {
      bambuSecureZero(values[i], sizeof(BambuCloudCredential));
    }
  }
  if (credentialSlots_.candidatePresent) {
    credentialTx_.recover();
    store.remove("candidate");
  }
  store.end();
  return true;
}

bool BambuCloud::persistCandidate() {
  const BambuCloudCredential* candidate = credentialTx_.candidate();
  if (!candidate) return false;
  Preferences store;
  if (!store.begin("bambucloud", false)) return false;
  bool ok = store.putBytes("candidate", candidate, sizeof(*candidate)) == sizeof(*candidate);
  store.end();
  xSemaphoreTake(mutex_, portMAX_DELAY);
  status_.candidatePending = ok;
  xSemaphoreGive(mutex_);
  return ok;
}

bool BambuCloud::persistPromotion() {
  const BambuCloudCredential* candidate = credentialTx_.candidate();
  if (!candidate) return false;
  const int current = credentialTx_.activeSlot();
  const int target = current == 0 ? 1 : 0;
  Preferences store;
  if (!store.begin("bambucloud", false)) return false;
  const char* key = target == 0 ? "active0" : "active1";
  bool ok = store.putBytes(key, candidate, sizeof(*candidate)) == sizeof(*candidate);
  if (ok) {
    ok = credentialTx_.commit();
    store.remove("candidate");
  }
  store.end();
  return ok;
}

void BambuCloud::removeCandidateFromNvs() {
  Preferences store;
  if (store.begin("bambucloud", false)) { store.remove("candidate"); store.end(); }
  xSemaphoreTake(mutex_, portMAX_DELAY);
  status_.candidatePending = false;
  xSemaphoreGive(mutex_);
}

void BambuCloud::eraseCredentialNvs() {
  Preferences store;
  if (store.begin("bambucloud", false)) { store.clear(); store.end(); }
}
