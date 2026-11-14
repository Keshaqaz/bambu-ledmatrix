#include "bambu_cloud.hpp"

#include <WiFi.h>
#include "mqtt_dns_gate.hpp"
#include "mqtt_pause_policy.hpp"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_tls_errors.h>
#ifdef BAMBU_AUTH_TRANSPORT_PROBE
#include <esp_transport.h>
#include <esp_transport_ssl.h>
#endif
#ifdef BAMBU_AUTH_TRANSPORT_PROBE
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#endif
#ifdef BAMBU_AUTH_WOLFSSL_PROBE
#include <errno.h>
#include <fcntl.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <sys/ioctl.h>
#include <wolfssl.h>
#endif
#include <stdlib.h>
#include <strings.h>
#include <time.h>

#include "bambu_auth_contract.hpp"

namespace {

static constexpr size_t kMaximumCloudJsonBytes = 16384;
#ifdef BAMBU_AUTH_TRANSPORT_PROBE
static constexpr size_t kMaximumAuthProbeResponseBytes = 2048;
#endif
static constexpr size_t kMaximumPortalCookieBytes = 512;
static constexpr size_t kMaximumPortalTokenJsonBytes = 2048;
static constexpr size_t kMaximumPortalHeaderLineBytes = 768;
static constexpr size_t kMaximumPortalHeadersBytes = 4096;
static constexpr uint32_t kOperationTtlMs = 5UL * 60UL * 1000UL;
static constexpr uint32_t kCandidateReportTtlMs = 90UL * 1000UL;
static constexpr uint32_t kDirectReportStaleMs = 70UL * 1000UL;
static constexpr uint32_t kFallbackTokenLifetimeSeconds =
    90UL * 24UL * 60UL * 60UL;
static constexpr const char* kRefreshRecoveryNvsKey = "refreshNext";

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
// The production anchor above is the server-sent cross-signed "DigiCert
// Global Root G2" (issuer: DigiCert Global Root CA). Mbed TLS accepts a
// non-self-signed trusted anchor, but wolfSSL keeps resolving the chain to a
// self-signed root and failed with ASN_NO_SIGNER_E on exactly that lookup.
// The probe therefore trusts the two self-signed public roots, which cover
// both the received intermediate and the received cross certificate without
// weakening validation.
#ifdef BAMBU_AUTH_WOLFSSL_PROBE
static const char kWolfSslProbeCa[] PROGMEM = R"PEM(-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBhMQswCQYDVQQG
EwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3d3cuZGlnaWNlcnQuY29tMSAw
HgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBHMjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUx
MjAwMDBaMGExCzAJBgNVBAYTAlVTMRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3
dy5kaWdpY2VydC5jb20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkq
hkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI2/Ou8jqJ
kTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx1x7e/dfgy5SDN67sH0NO
3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQq2EGnI/yuum06ZIya7XzV+hdG82MHauV
BJVJ8zUtluNJbd134/tJS7SsVQepj5WztCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyM
UNGPHgm+F6HmIcr9g+UQvIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQAB
o0IwQDAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV5uNu
5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY1Yl9PMWLSn/pvtsr
F9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4NeF22d+mQrvHRAiGfzZ0JFrabA0U
WTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NGFdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBH
QRFXGU7Aj64GxJUTFy8bJZ918rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/
iyK5S9kJRaTepLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIDrzCCApegAwIBAgIQCDvgVpBCRrGhdWrJWZHHSjANBgkqhkiG9w0BAQUFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBD
QTAeFw0wNjExMTAwMDAwMDBaFw0zMTExMTAwMDAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IENBMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEA4jvhEXLeqKTTo1eqUKKPC3eQyaKl7hLOllsB
CSDMAZOnTjC3U/dDxGkAV53ijSLdhwZAAIEJzs4bg7/fzTtxRuLWZscFs3YnFo97
nh6Vfe63SKMI2tavegw5BmV/Sl0fvBf4q77uKNd0f3p4mVmFaG5cIzJLv07A6Fpt
43C/dxC//AH2hdmoRBBYMql1GNXRor5H4idq9Joz+EkIYIvUX7Q6hL+hqkpMfT7P
T19sdl6gSzeRntwi5m3OFBqOasv+zbMUZBfHWymeMr/y7vrTC0LUq7dBMtoM1O/4
gdW7jVg/tRvoSSiicNoxBN33shbyTApOB6jtSj1etX+jkMOvJwIDAQABo2MwYTAO
BgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUA95QNVbR
TLtm8KPiGxvDl7I90VUwHwYDVR0jBBgwFoAUA95QNVbRTLtm8KPiGxvDl7I90VUw
DQYJKoZIhvcNAQEFBQADggEBAMucN6pIExIK+t1EnE9SsPTfrgT1eXkIoyQY/Esr
hMAtudXH/vTBH1jLuG2cenTnmCmrEbXjcKChzUyImZOMkXDiqw8cvpOp/2PV5Adg
06O/nVsJ8dWO41P0jmP6P6fbtGbfYmbW0W5BjfIttep3Sp+dWOIrWcBAI+0tKIJF
PnlUkiaY4IBIqDfv8NZ5YBberOgOzW6sRBc4L0na4UU+Krk2U886UAb3LujEV0ls
YSEY1QSteDwsOoBrp+uvFRTp2InBuThs4pFsiv9kuXclVzDAGySj4dzp30d8tbQk
CAUw7C29C79Fv1C5qfPrmAESrciIxpg0X40KPMbp1ZWVbd4=
-----END CERTIFICATE-----)PEM";
#endif
#ifdef BAMBU_AUTH_TRANSPORT_CAPTURE
// Capture-only observation trust anchor (self-signed, local throwaway key).
// Both capture paths verify the observation endpoint's certificate against
// this CA - validation stays on, merely anchored to the local endpoint.
static const char kCaptureObservationCa[] PROGMEM = R"PEM(-----BEGIN CERTIFICATE-----
MIIDFzCCAf+gAwIBAgIUAs900otT1hqWT5hpv9EDVDeoR4YwDQYJKoZIhvcNAQEL
BQAwGzEZMBcGA1UEAwwQYXBpLmJhbWJ1bGFiLmNvbTAeFw0yNjA5MDIxMjU3MTha
Fw0yNjA5MDQxMjU3MThaMBsxGTAXBgNVBAMMEGFwaS5iYW1idWxhYi5jb20wggEi
MA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDyedffJ/Edipi0IQ+l7RtRrgYY
mKsGJn9+PCNNW+s4kR9Yz6DKeByhq8N0eeNuiD/BB+zoLpGq91MZmY7/Mta4IkSD
ICP1E60Y85oDUphY5eYMwxE8HCUVqKAQiJTbJYxdnIPAThblYCLAb9fgD0fmbQ8J
oRw6DBwHyGMfc+VG0O67CK28KzPwhV37ml5b844jjllCXpIgfUKOYRGy6o+ACnQi
dLGuBNkR/OUGOCVfdBvdsdDf0njAQfJB/hg4OCN7EPuFkqSZvIB0n57M6+O+rI6q
tnZnEtWKM5mAyRKKguj8sE+vdOUda8lgTm4qup9bkjrEHlUdVCIRdW8Fr3pTAgMB
AAGjUzBRMB0GA1UdDgQWBBR9+7JEdNArnMWnbS4YHwAaDEDWOjAfBgNVHSMEGDAW
gBR9+7JEdNArnMWnbS4YHwAaDEDWOjAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3
DQEBCwUAA4IBAQAe+8ZY5fSTXCeUPbFKSz+H8bnCfRO6mYMNXOXjr58LH1niqqCv
38LKVsg884ljgXU394dFpRKnYeLutKDTzn/kFdr1RtLJMRPWJs0uPHRYEO++j2RM
K4lCMeGxev2uixYnL0K+69URgrVlISs2paltXh3JTM4pZV5DPkohK/s6jio/2TWh
6uFP2YAD9JxmqZA2m0zlh9ONJLkrTwVdeP5jZesXwlDLoI5t3sH0rhzjEnkPi3cG
qmiRSvgIBCuK4TcdvdrpAO6fKGC6P7PqHGiy7hArZsOxOWY9CsQ2FifnjP49EqaC
65ft7qW0PwX9m5wteFj+AQDm6stB8Zmzd8Oq
-----END CERTIFICATE-----)PEM";
#endif

#ifdef BAMBU_AUTH_TRANSPORT_CAPTURE
// Capture-only observation build (AT04 step 1): diverts the transport connect
// to the local openssl s_server observation endpoint while keeping the
// production SNI, cipher configuration and extension set byte-identical. The
// ClientHello is recorded before the handshake is rejected locally against the
// observation certificate; this build never contacts Bambu Cloud and carries
// no other wire changes. Never define for production images.
class CaptureObservationClient : public NetworkClientSecure {
 public:
  using NetworkClientSecure::connect;
  int connect(const char*, uint16_t) override { return connectObservation(); }
  int connect(const char*, uint16_t, int32_t) override {
    return connectObservation();
  }

 private:
  int connectObservation() {
    IPAddress observationIp;
    if (!observationIp.fromString("192.168.1.166")) return 0;
    return connect(observationIp, 8443, "api.bambulab.com",
                   kCaptureObservationCa, nullptr, nullptr);
  }
};
#endif

#ifdef BAMBU_AUTH_TRANSPORT_PROBE
void bambuProbeNoteTail(uint8_t value);
#endif

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
#ifdef BAMBU_AUTH_TRANSPORT_PROBE
    bambuProbeNoteTail(static_cast<uint8_t>(value));
#endif
    return value;
  }
  int peek() override {
    if (read_ >= maximum_) return -1;
    return upstream_.peek();
  }
  void flush() override { upstream_.flush(); }
  size_t write(uint8_t) override { return 0; }
  bool overflowed() const { return overflow_ || (read_ >= maximum_ && upstream_.available() > 0); }
  size_t readCount() const { return read_; }

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

#ifdef BAMBU_AUTH_TRANSPORT_PROBE
// Probe-only bounded diagnostic of the HTTP-200 body path: numeric metadata
// only (sizes, first byte class, JSON error enum, key-presence bitmask).
// Never body content, never values.
static volatile int32_t gProbe200Length = -2;  // -2 = no attempt yet
static volatile int32_t gProbe200Read = 0;
static volatile int32_t gProbe200First = -1;
static volatile uint8_t gProbe200Error = 255;  // 255 = no attempt yet
static volatile int32_t gProbe200Code = -2;
static volatile uint16_t gProbe200Shape = 0;
// Bounded key-name capture: root-level (up to 8) then data-level (up to 12
// total). Names only, truncated to 19 chars; values are never stored.
static char gProbeKeys[12][20];
static volatile uint8_t gProbeKeyCount = 0;
// Char-class fingerprint of the last 16 consumed bytes (syntax context of a
// parse failure; classes only, no byte values):
// 0=control,1=space,2=digit,3=alpha,4={}[]:,,5=quote,6=backslash,7=other
static uint8_t gProbeTail[16];
static volatile uint8_t gProbeTailLen = 0;
void bambuProbeNoteTail(uint8_t value) {
  uint8_t cls;
  if (value < 0x20 || value == 0x7f) cls = 0;
  else if (value == ' ') cls = 1;
  else if (value >= '0' && value <= '9') cls = 2;
  else if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')) cls = 3;
  else if (value == '{' || value == '}' || value == '[' || value == ']' ||
           value == ':' || value == ',') cls = 4;
  else if (value == '"') cls = 5;
  else if (value == '\\') cls = 6;
  else cls = 7;
  if (gProbeTailLen < sizeof(gProbeTail)) gProbeTail[gProbeTailLen++] = cls;
}
void bambuProbeCaptureKeys(JsonVariantConst root) {
  gProbeKeyCount = 0;
  memset(gProbeKeys, 0, sizeof(gProbeKeys));
  if (JsonObjectConst obj = root.as<JsonObjectConst>()) {
    for (JsonPairConst p : obj) {
      if (gProbeKeyCount >= 8) break;
      strlcpy(gProbeKeys[gProbeKeyCount++], p.key().c_str(),
              sizeof(gProbeKeys[0]));
    }
  }
  if (JsonObjectConst data = root["data"].as<JsonObjectConst>()) {
    for (JsonPairConst p : data) {
      if (gProbeKeyCount >= 12) break;
      strlcpy(gProbeKeys[gProbeKeyCount++], p.key().c_str(),
              sizeof(gProbeKeys[0]));
    }
  }
}
// Key-presence coordinates (structural metadata only; AT01 precedent).
// bit0 root object; bit1 root.data object; bit2..6 root uid/userId/user_id/
//   accountId/account_id strings; bit7..11 same under data;
// bit12 data.devices array; bit13 root.devices array;
// bit14 root.error string; bit15 root.code present.
uint16_t bambuProbeShapeMask(JsonVariantConst root) {
  uint16_t mask = 0;
  const bool rootObject = root.is<JsonObjectConst>();
  if (rootObject) mask |= 1 << 0;
  JsonVariantConst data = root["data"];
  const bool dataObject = data.is<JsonObjectConst>();
  if (dataObject) mask |= 1 << 1;
  static constexpr const char* kRootKeys[] = {"uid", "userId", "user_id",
                                              "accountId", "account_id"};
  for (unsigned i = 0; i < 5; ++i) {
    if (root[kRootKeys[i]].is<const char*>()) mask |= 1 << (2 + i);
    if (dataObject && data[kRootKeys[i]].is<const char*>())
      mask |= 1 << (7 + i);
  }
  if (data["devices"].is<JsonArrayConst>()) mask |= 1 << 12;
  if (root["devices"].is<JsonArrayConst>()) mask |= 1 << 13;
  if (root["error"].is<const char*>()) mask |= 1 << 14;
  if (!root["code"].isNull()) mask |= 1 << 15;
  return mask;
}
#endif

static void makeCloudDeviceId(char output[37]) {
  const uint64_t chipId = ESP.getEfuseMac() & 0x0000ffffffffffffULL;
  snprintf(output, 37, "00000000-0000-4000-8000-%012llx",
           static_cast<unsigned long long>(chipId));
}

static JsonVariantConst responseData(const JsonDocument& document) {
  JsonVariantConst data = document["data"];
  return data.is<JsonObjectConst>() ? data : document.as<JsonVariantConst>();
}

#ifdef BAMBU_AUTH_TRANSPORT_PROBE
static constexpr uint8_t kMaximumAuthTransportProfile = 12;
static const char* kHttp11Alpn[] = {"http/1.1", nullptr};
static const int kDesktopTls12CipherOrder[] = {
    0xC02C, 0xC030, 0x009F, 0xCCA9, 0xCCA8, 0xCCAA, 0xC02B,
    0xC02F, 0x009E, 0xC024, 0xC028, 0x006B, 0xC023, 0xC027,
    0x0067, 0xC00A, 0xC014, 0x0039, 0xC009, 0xC013, 0x0033,
    0x009D, 0x009C, 0x003D, 0x003C, 0x0035, 0x002F, 0,
};
// AT04 wire variants: profile 6 keeps only modern AEAD suites in the ESP
// order; profile 7 offers a single suite so the resulting JA3 differs
// maximally from the default mbed TLS fingerprint.
static const int kStrongTls12CipherOrder[] = {
    0xC02C, 0xC030, 0xCCA9, 0xCCA8, 0xC02B, 0xC02F,
    0xC024, 0xC028, 0xC00A, 0xC014, 0x009D, 0x009C, 0,
};
static const int kSingleTls12CipherOrder[] = {0xC030, 0};
#endif

static const char* firstString(JsonVariantConst object, const char* a,
                               const char* b = nullptr,
                               const char* c = nullptr) {
  const char* value = object[a] | "";
  if (!value[0] && b) value = object[b] | "";
  if (!value[0] && c) value = object[c] | "";
  return value;
}

struct PortalResponseMetadata {
  int statusCode = 0;
  int contentLength = -1;
  bool chunked = false;
};

class SecureWipeGuard {
 public:
  SecureWipeGuard(void* memory, size_t length)
      : memory_(memory), length_(length) {}
  ~SecureWipeGuard() { bambuSecureZero(memory_, length_); }

 private:
  void* memory_;
  size_t length_;
};

static bool secureWrite(NetworkClient& client, const char* data, size_t length) {
  while (length) {
    const size_t written = client.write(
        reinterpret_cast<const uint8_t*>(data), length);
    if (written == 0) return false;
    data += written;
    length -= written;
  }
  return true;
}

static bool secureWrite(NetworkClient& client, const char* text) {
  return secureWrite(client, text, strlen(text));
}

static bool readBoundedHttpLine(NetworkClient& client, char* line,
                                size_t lineSize, size_t& totalBytes,
                                size_t maximumBytes) {
  if (!line || lineSize < 2) return false;
  size_t length = 0;
  for (;;) {
    char value = 0;
    if (client.readBytes(&value, 1) != 1 || ++totalBytes > maximumBytes) {
      line[0] = '\0';
      return false;
    }
    if (value == '\n') {
      if (length && line[length - 1] == '\r') --length;
      line[length] = '\0';
      return true;
    }
    if (length + 1 >= lineSize) {
      line[0] = '\0';
      return false;
    }
    line[length++] = value;
  }
}

static bool headerValueContains(const char* value, const char* needle) {
  const size_t needleLength = strlen(needle);
  for (const char* cursor = value; cursor && *cursor; ++cursor) {
    if (strncasecmp(cursor, needle, needleLength) == 0) return true;
  }
  return false;
}

static bool sendPortalRequest(NetworkClient& client, const char* method,
                              const char* path, const char* body,
                              const char* sessionCookie) {
  static const char kPortalHeaders[] =
      " HTTP/1.0\r\n"
      "Host: bambulab.com\r\n"
      "Accept: application/json\r\n"
      "Content-Type: application/json\r\n"
      "User-Agent: bambu_network_agent/02.07.01.51\r\n"
      "X-BBL-Client-Name: BambuStudio\r\n"
      "X-BBL-Client-Type: slicer\r\n"
      "X-BBL-Client-Version: 02.07.01.62\r\n"
      "X-BBL-Language: en-US\r\n"
      "X-BBL-OS-Type: linux\r\n"
      "X-BBL-OS-Version: esp32\r\n"
      "X-BBL-Agent-Version: 02.07.01.51\r\n"
      "Accept-Encoding: identity\r\n"
      "Connection: close\r\n";
  if (!secureWrite(client, method) || !secureWrite(client, " ") ||
      !secureWrite(client, path) || !secureWrite(client, kPortalHeaders)) {
    return false;
  }
  if (sessionCookie && sessionCookie[0] &&
      (!secureWrite(client, "Cookie: ") ||
       !secureWrite(client, sessionCookie) || !secureWrite(client, "\r\n"))) {
    return false;
  }
  if (body) {
    char contentLength[24] = {};
    snprintf(contentLength, sizeof(contentLength), "%u",
             static_cast<unsigned>(strlen(body)));
    if (!secureWrite(client, "Content-Length: ") ||
        !secureWrite(client, contentLength) || !secureWrite(client, "\r\n")) {
      return false;
    }
  }
  if (!secureWrite(client, "\r\n")) return false;
  return !body || secureWrite(client, body);
}

static bool readPortalHeaders(NetworkClient& client,
                              PortalResponseMetadata& metadata,
                              char* sessionCookie, size_t sessionCookieSize,
                              char* error, size_t errorSize) {
  char line[kMaximumPortalHeaderLineBytes] = {};
  SecureWipeGuard lineGuard(line, sizeof(line));
  size_t totalBytes = 0;
  if (!readBoundedHttpLine(client, line, sizeof(line), totalBytes,
                           kMaximumPortalHeadersBytes)) {
    copyCode(error, errorSize, "response_headers_oversized");
    return false;
  }
  const char* status = strchr(line, ' ');
  metadata.statusCode = status ? atoi(status + 1) : 0;
  if (metadata.statusCode <= 0) {
    copyCode(error, errorSize, "bad_response");
    return false;
  }

  int cookiePriority = 0;
  for (;;) {
    if (!readBoundedHttpLine(client, line, sizeof(line), totalBytes,
                             kMaximumPortalHeadersBytes)) {
      if (sessionCookie) bambuSecureZero(sessionCookie, sessionCookieSize);
      copyCode(error, errorSize, "response_headers_oversized");
      return false;
    }
    if (!line[0]) break;
    if (strncasecmp(line, "Content-Length:", 15) == 0) {
      const char* value = line + 15;
      while (*value == ' ' || *value == '\t') ++value;
      char* end = nullptr;
      const unsigned long length = strtoul(value, &end, 10);
      if (end == value || (*end && *end != ' ' && *end != '\t') ||
          length > 0x7fffffffUL) {
        copyCode(error, errorSize, "bad_response");
        return false;
      }
      metadata.contentLength = static_cast<int>(length);
    } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
      metadata.chunked = headerValueContains(line + 18, "chunked");
    } else if (sessionCookie &&
               strncasecmp(line, "Set-Cookie:", 11) == 0) {
      const char* value = line + 11;
      while (*value == ' ' || *value == '\t') ++value;
      bambuSelectSessionCookie(value, sessionCookie, sessionCookieSize,
                               cookiePriority);
    }
  }
  if (metadata.statusCode != 200) {
    if (sessionCookie) bambuSecureZero(sessionCookie, sessionCookieSize);
    snprintf(error, errorSize, "http_%d", metadata.statusCode);
    return false;
  }
  return true;
}

static bool readExact(NetworkClient& client, char* destination, size_t length) {
  return length == 0 || client.readBytes(destination, length) == length;
}

static bool readPortalBody(NetworkClient& client,
                           const PortalResponseMetadata& metadata,
                           char* output, size_t outputSize, size_t& outputLength,
                           char* error, size_t errorSize) {
  outputLength = 0;
  if (!output || outputSize < 2) return false;
  const size_t capacity = outputSize - 1;
  if (metadata.chunked) {
    size_t framingBytes = 0;
    char line[64] = {};
    SecureWipeGuard lineGuard(line, sizeof(line));
    for (;;) {
      if (!readBoundedHttpLine(client, line, sizeof(line), framingBytes, 1024)) {
        copyCode(error, errorSize, "bad_response");
        return false;
      }
      char* end = nullptr;
      const unsigned long chunkLength = strtoul(line, &end, 16);
      if (end == line || (*end && *end != ';') ||
          chunkLength > capacity - outputLength) {
        copyCode(error, errorSize,
                 chunkLength > capacity - outputLength ? "response_oversized" :
                                                         "bad_response");
        return false;
      }
      if (chunkLength == 0) {
        do {
          if (!readBoundedHttpLine(client, line, sizeof(line), framingBytes,
                                   1024)) {
            copyCode(error, errorSize, "bad_response");
            return false;
          }
        } while (line[0]);
        break;
      }
      if (!readExact(client, output + outputLength,
                     static_cast<size_t>(chunkLength))) {
        copyCode(error, errorSize, "bad_response");
        return false;
      }
      outputLength += static_cast<size_t>(chunkLength);
      char terminator[2] = {};
      if (!readExact(client, terminator, sizeof(terminator)) ||
          terminator[0] != '\r' || terminator[1] != '\n') {
        copyCode(error, errorSize, "bad_response");
        return false;
      }
    }
  } else if (metadata.contentLength >= 0) {
    if (static_cast<size_t>(metadata.contentLength) > capacity) {
      copyCode(error, errorSize, "response_oversized");
      return false;
    }
    outputLength = static_cast<size_t>(metadata.contentLength);
    if (!readExact(client, output, outputLength)) {
      copyCode(error, errorSize, "bad_response");
      return false;
    }
  } else {
    uint32_t lastDataMs = millis();
    while (client.connected() || client.available()) {
      const int available = client.available();
      if (available <= 0) {
        if (millis() - lastDataMs > 12000) {
          copyCode(error, errorSize, "bad_response");
          return false;
        }
        delay(1);
        continue;
      }
      if (outputLength == capacity) {
        copyCode(error, errorSize, "response_oversized");
        return false;
      }
      const size_t wanted = min(static_cast<size_t>(available),
                                capacity - outputLength);
      const int received = client.read(
          reinterpret_cast<uint8_t*>(output + outputLength), wanted);
      if (received <= 0) {
        copyCode(error, errorSize, "bad_response");
        return false;
      }
      outputLength += static_cast<size_t>(received);
      lastDataMs = millis();
    }
  }
  output[outputLength] = '\0';
  return true;
}

static bool openPortalConnection(SecureNetworkCoordinator& coordinator,
                                 SecureNetworkLease& lease,
                                 WiFiClientSecure& client,
                                 char* error, size_t errorSize) {
  (void)coordinator;
  if (!lease.acquired()) {
    copyCode(error, errorSize, "tls_busy");
    return false;
  }
  client.setCACert(kBambuCloudCa);
  client.setTimeout(12000);
  client.setHandshakeTimeout(12);
  if (!client.connect("bambulab.com", 443)) {
    copyCode(error, errorSize, "https_connect_failed");
    return false;
  }
  return true;
}

static bool portalTotpRequest(SecureNetworkCoordinator& coordinator,
                              const char* body, char* sessionCookie,
                              size_t sessionCookieSize, char* error,
                              size_t errorSize) {
  if (sessionCookie && sessionCookieSize)
    bambuSecureZero(sessionCookie, sessionCookieSize);
  SecureNetworkLease lease(coordinator, SECURE_NETWORK_CLOUD_HTTP);
  WiFiClientSecure client;
  if (!openPortalConnection(coordinator, lease, client, error, errorSize))
    return false;
  if (!sendPortalRequest(client, "POST", "/api/sign-in/tfa", body, nullptr)) {
    copyCode(error, errorSize, "https_write_failed");
    client.stop();
    return false;
  }
  PortalResponseMetadata metadata;
  const bool ok = readPortalHeaders(client, metadata, sessionCookie,
                                    sessionCookieSize, error, errorSize);
  client.stop();
  if (!ok && sessionCookie)
    bambuSecureZero(sessionCookie, sessionCookieSize);
  return ok;
}

static bool portalTokenRequest(SecureNetworkCoordinator& coordinator,
                               const char* sessionCookie, char* accessToken,
                               size_t accessTokenSize, char* refreshToken,
                               size_t refreshTokenSize,
                               uint32_t& accessLifetime,
                               uint32_t& refreshLifetime, char* error,
                               size_t errorSize) {
  if (accessToken && accessTokenSize)
    bambuSecureZero(accessToken, accessTokenSize);
  if (refreshToken && refreshTokenSize)
    bambuSecureZero(refreshToken, refreshTokenSize);
  accessLifetime = 0;
  refreshLifetime = 0;
  SecureNetworkLease lease(coordinator, SECURE_NETWORK_CLOUD_HTTP);
  WiFiClientSecure client;
  if (!openPortalConnection(coordinator, lease, client, error, errorSize))
    return false;
  if (!sendPortalRequest(client, "GET", "/api/auth/token", nullptr,
                         sessionCookie)) {
    copyCode(error, errorSize, "https_write_failed");
    client.stop();
    return false;
  }
  PortalResponseMetadata metadata;
  if (!readPortalHeaders(client, metadata, nullptr, 0, error, errorSize)) {
    client.stop();
    return false;
  }
  char body[kMaximumPortalTokenJsonBytes + 1] = {};
  size_t bodyLength = 0;
  bool ok = readPortalBody(client, metadata, body, sizeof(body), bodyLength,
                           error, errorSize);
  client.stop();
  if (!ok) {
    bambuSecureZero(body, sizeof(body));
    return false;
  }

  JsonDocument response;
  const DeserializationError jsonError = deserializeJson(
      response, body, bodyLength, DeserializationOption::NestingLimit(4));
  JsonVariantConst data = responseData(response);
  BambuAuthEnvelopeInput envelope;
  envelope.token = data["token"] | "";
  envelope.accessToken = data["accessToken"] | "";
  envelope.access_token = data["access_token"] | "";
  envelope.refreshToken = data["refreshToken"] | "";
  envelope.refresh_token = data["refresh_token"] | "";
  envelope.expiresIn = data["expiresIn"] | 0;
  envelope.refreshExpiresIn = data["refreshExpiresIn"] | 0;
  ok = !jsonError && bambuCopyAuthEnvelope(
      envelope, accessToken, accessTokenSize, refreshToken, refreshTokenSize,
      accessLifetime, refreshLifetime);
  const char* secrets[] = {envelope.token, envelope.accessToken,
                           envelope.access_token, envelope.refreshToken,
                           envelope.refresh_token};
  for (const char* secret : secrets) {
    if (secret && secret[0])
      bambuSecureZero(const_cast<char*>(secret), strlen(secret));
  }
  bambuSecureZero(body, sizeof(body));
  if (jsonError) {
    copyCode(error, errorSize,
             jsonError == DeserializationError::NoMemory ? "json_no_memory" :
                                                           "bad_response");
  } else if (!ok) {
    copyCode(error, errorSize, "tfa_token_envelope_incomplete");
  }
  return ok;
}

static bool cloudJsonRequestWithClient(NetworkClient& client,
                                       const char* url, const char* method,
                                       const char* body, const char* bearer,
                                       JsonDocument& response, char* error,
                                       size_t errorSize,
                                       bool* applicationJson) {
  if (applicationJson) *applicationJson = false;
  HTTPClient http;
  http.setConnectTimeout(12000);
  http.setTimeout(12000);
  http.setReuse(false);
  http.useHTTP10(true);
  if (!http.begin(client, url)) { copyCode(error, errorSize, "https_begin_failed"); return false; }
  char deviceId[37] = {};
  makeCloudDeviceId(deviceId);
  // User-Agent задаётся только через setUserAgent(): addHeader молча
  // отбрасывает его (allow-list ядра), и HTTPClient отправляет дефолтный
  // "ESP32HTTPClient", который Cloudflare блокирует (см. AT04/AT05/AT06).
  http.setUserAgent("bambu_network_agent/02.07.01.51");
  http.addHeader("X-BBL-Client-Name", "BambuStudio");
  http.addHeader("X-BBL-Client-Type", "slicer");
  http.addHeader("X-BBL-Client-Version", "02.07.01.62");
  http.addHeader("X-BBL-Device-ID", deviceId);
  http.addHeader("X-BBL-Language", "en-US");
  http.addHeader("X-BBL-OS-Type", "linux");
  http.addHeader("X-BBL-OS-Version", "esp32");
  http.addHeader("X-BBL-Agent-Version", "02.07.01.51");
  http.addHeader("X-BBL-Executable-info", "{}");
  http.addHeader("X-BBL-Agent-OS-Type", "linux");
  http.addHeader("X-BBL-Executable-Env", "false");
  http.addHeader("Accept", "application/json");
  http.addHeader("Content-Type", "application/json");
  if (bearer && bearer[0]) http.addHeader("Authorization", String("Bearer ") + bearer);
  int code = strcmp(method, "GET") == 0 ? http.GET() :
             http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(body ? body : "{}")),
                       body ? strlen(body) : 2);
  if (code != 200) {
    static constexpr size_t kMaximumErrorBodyBytes = 512;
    char errorBody[kMaximumErrorBodyBytes + 1] = {};
    NetworkClient& errorStream = http.getStream();
    size_t received = 0;
    uint32_t lastDataMs = millis();
    while (received < kMaximumErrorBodyBytes &&
           (errorStream.connected() || errorStream.available())) {
      const int available = errorStream.available();
      if (available <= 0) {
        if (millis() - lastDataMs >= 2000) break;
        delay(1);
        continue;
      }
      const size_t wanted = min(static_cast<size_t>(available),
                                kMaximumErrorBodyBytes - received);
      const int count = errorStream.read(
          reinterpret_cast<uint8_t*>(errorBody + received), wanted);
      if (count <= 0) break;
      received += static_cast<size_t>(count);
      lastDataMs = millis();
    }
    errorBody[received] = '\0';
    if (applicationJson) {
      JsonDocument errorDocument;
      const DeserializationError parseError = deserializeJson(
          errorDocument, errorBody, received,
          DeserializationOption::NestingLimit(4));
      JsonVariantConst root = errorDocument.as<JsonVariantConst>();
      const bool parsedObject = !parseError && root.is<JsonObjectConst>();
      const bool hasTypedCode = parsedObject &&
          (root["code"].is<int>() || root["code"].is<const char*>());
      const bool hasTypedError = parsedObject && root["error"].is<const char*>();
      *applicationJson = bambuAuthRejectedApplicationJsonCoordinates(
          code, parsedObject, hasTypedCode, hasTypedError);
      errorDocument.clear();
    }
    bambuAuthHttpFailureCode(code, errorBody, error, errorSize);
    bambuSecureZero(errorBody, sizeof(errorBody));
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
#ifdef BAMBU_AUTH_TRANSPORT_PROBE
  const int probe200FirstByte = input.peek();
  gProbeTailLen = 0;
#endif
  DeserializationError jsonError = deserializeJson(
      response, input, DeserializationOption::NestingLimit(8));
  // Пустой 200 (Content-Length: 0, ноль байт, без overflow) — валидный
  // ответ action-эндпоинтов (например, sendemail/code): считаем его пустым
  // документом; вызывающие проверки формы данных не ослабляются (AT06).
  const bool emptySuccessResponse = code == 200 && !input.overflowed() &&
      jsonError == DeserializationError::EmptyInput;
  if (jsonError || input.overflowed()) {
    if (!emptySuccessResponse) {
#ifdef BAMBU_AUTH_TRANSPORT_PROBE
      gProbe200Length = contentLength;
      gProbe200Read = static_cast<int32_t>(input.readCount());
      gProbe200First = probe200FirstByte;
      gProbe200Error = jsonError.code() == DeserializationError::Ok && input.overflowed()
          ? static_cast<uint8_t>(DeserializationError::NoMemory)
          : static_cast<uint8_t>(jsonError.code());
      gProbe200Code = code;
      gProbe200Shape = 0;
#endif
      copyCode(error, errorSize,
               jsonError == DeserializationError::NoMemory ? "json_no_memory" : "bad_response");
      http.end();
      return false;
    }
    response.clear();
  }
#ifdef BAMBU_AUTH_TRANSPORT_PROBE
  gProbe200Code = code;
  gProbe200Shape = bambuProbeShapeMask(response.as<JsonVariantConst>());
  bambuProbeCaptureKeys(response.as<JsonVariantConst>());
#endif
  if (applicationJson) {
    JsonVariantConst data = responseData(response);
    const bool hasAuthData = data.is<JsonObjectConst>() &&
        (data["loginType"].is<const char*>() ||
         data["accessToken"].is<const char*>() ||
         data["access_token"].is<const char*>() ||
         data["refreshToken"].is<const char*>() ||
         data["refresh_token"].is<const char*>());
    *applicationJson = code == 200 &&
        bambuAuthSuccessfulApplicationJsonCoordinates(true, hasAuthData);
  }
  http.end();
  return true;
}

#ifdef BAMBU_AUTH_WOLFSSL_PROBE
class WolfSslAuthClient : public NetworkClient {
 public:
  WolfSslAuthClient() = default;
  ~WolfSslAuthClient() override { stop(); }

  int connect(IPAddress, uint16_t) override { return 0; }
  int connect(IPAddress, uint16_t, int32_t) override { return 0; }
  int connect(const char* host, uint16_t port) override {
    return connect(host, port, connectionTimeoutMs_);
  }
  int connect(const char* host, uint16_t port, int32_t timeoutMs) override {
    stop();
    lastError_ = 0;
    lastStage_ = 1;
    if (!host || !host[0] || timeoutMs <= 0 || time(nullptr) < 1609459200) {
      lastError_ = BAD_FUNC_ARG;
      return 0;
    }

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portText[6] = {};
    snprintf(portText, sizeof(portText), "%u", static_cast<unsigned>(port));
    struct addrinfo* addresses = nullptr;
    if (getaddrinfo(host, portText, &hints, &addresses) != 0 || !addresses) {
      lastError_ = SOCKET_ERROR_E;
      return 0;
    }

    const uint32_t startedMs = millis();
    for (struct addrinfo* address = addresses; address; address = address->ai_next) {
      socket_ = socket(address->ai_family, address->ai_socktype,
                       address->ai_protocol);
      if (socket_ < 0) continue;
      const int originalFlags = fcntl(socket_, F_GETFL, 0);
      if (originalFlags < 0 ||
          fcntl(socket_, F_SETFL, originalFlags | O_NONBLOCK) < 0) {
        close(socket_);
        socket_ = -1;
        continue;
      }
      int connected = ::connect(socket_, address->ai_addr, address->ai_addrlen);
      if (connected != 0 && errno == EINPROGRESS) {
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(socket_, &writeSet);
        const uint32_t elapsedMs = millis() - startedMs;
        const uint32_t remainingMs = elapsedMs < static_cast<uint32_t>(timeoutMs)
            ? static_cast<uint32_t>(timeoutMs) - elapsedMs
            : 0;
        struct timeval timeout = {
            static_cast<time_t>(remainingMs / 1000),
            static_cast<suseconds_t>((remainingMs % 1000) * 1000)};
        connected = remainingMs &&
                    select(socket_ + 1, nullptr, &writeSet, nullptr, &timeout) > 0
                        ? 0
                        : -1;
        int socketError = 0;
        socklen_t socketErrorSize = sizeof(socketError);
        if (connected == 0 &&
            (getsockopt(socket_, SOL_SOCKET, SO_ERROR, &socketError,
                        &socketErrorSize) != 0 || socketError != 0)) {
          connected = -1;
        }
      }
      fcntl(socket_, F_SETFL, originalFlags);
      if (connected == 0) break;
      close(socket_);
      socket_ = -1;
    }
    freeaddrinfo(addresses);
    if (socket_ < 0) {
      lastError_ = SOCKET_ERROR_E;
      return 0;
    }

    struct timeval socketTimeout = {
        static_cast<time_t>(timeoutMs / 1000),
        static_cast<suseconds_t>((timeoutMs % 1000) * 1000)};
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &socketTimeout,
               sizeof(socketTimeout));
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &socketTimeout,
               sizeof(socketTimeout));

    if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
      lastError_ = MEMORY_E;
      stop();
      return 0;
    }
    initialized_ = true;
    context_ = wolfSSL_CTX_new(wolfTLS_client_method());
    if (!context_ ||
        wolfSSL_CTX_load_verify_buffer(
            context_, reinterpret_cast<const unsigned char*>(kWolfSslProbeCa),
            strlen(kWolfSslProbeCa), WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
      lastError_ = ASN_INPUT_E;
      stop();
      return 0;
    }
    wolfSSL_CTX_set_verify(context_, WOLFSSL_VERIFY_PEER, nullptr);
    ssl_ = wolfSSL_new(context_);
    if (!ssl_ || wolfSSL_set_fd(ssl_, socket_) != WOLFSSL_SUCCESS ||
        wolfSSL_check_domain_name(ssl_, host) != WOLFSSL_SUCCESS ||
        wolfSSL_UseSNI(ssl_, WOLFSSL_SNI_HOST_NAME, host,
                       static_cast<unsigned short>(strlen(host))) !=
            WOLFSSL_SUCCESS) {
      lastError_ = BAD_FUNC_ARG;
      stop();
      return 0;
    }
    if (wolfSSL_connect(ssl_) != WOLFSSL_SUCCESS) {
      lastError_ = wolfSSL_get_error(ssl_, 0);
      stop();
      return 0;
    }
    connected_ = true;
    lastStage_ = 0;
    return 1;
  }

  size_t write(uint8_t data) override { return write(&data, 1); }
  size_t write(const uint8_t* buffer, size_t size) override {
    if (!ssl_ || !connected_ || !buffer || size == 0) return 0;
    const int written = wolfSSL_write(ssl_, buffer, static_cast<int>(size));
    if (written <= 0) {
      lastStage_ = 2;
      lastError_ = wolfSSL_get_error(ssl_, written);
      connected_ = false;
      return 0;
    }
    return static_cast<size_t>(written);
  }
  int available() override {
    if (peeked_ >= 0) return 1;
    if (!ssl_ || !connected_) return 0;
    const int pending = wolfSSL_pending(ssl_);
    if (pending > 0) return pending;
    int encryptedBytes = 0;
    return ioctl(socket_, FIONREAD, &encryptedBytes) == 0 && encryptedBytes > 0
               ? 1
               : 0;
  }
  int read() override {
    uint8_t value = 0;
    return read(&value, 1) == 1 ? value : -1;
  }
  int read(uint8_t* buffer, size_t size) override {
    if (!buffer || size == 0) return 0;
    size_t offset = 0;
    if (peeked_ >= 0) {
      buffer[offset++] = static_cast<uint8_t>(peeked_);
      peeked_ = -1;
      if (offset == size) return static_cast<int>(offset);
    }
    if (!ssl_ || !connected_) return offset ? static_cast<int>(offset) : -1;
    const int received = wolfSSL_read(
        ssl_, buffer + offset, static_cast<int>(size - offset));
    if (received > 0) return static_cast<int>(offset) + received;
    lastStage_ = 3;
    lastError_ = wolfSSL_get_error(ssl_, received);
    if (lastError_ != WOLFSSL_ERROR_WANT_READ &&
        lastError_ != WOLFSSL_ERROR_WANT_WRITE) {
      connected_ = false;
    }
    return offset ? static_cast<int>(offset) : -1;
  }
  int peek() override {
    if (peeked_ >= 0) return peeked_;
    uint8_t value = 0;
    const int received = read(&value, 1);
    if (received != 1) return -1;
    peeked_ = value;
    return peeked_;
  }
  void flush() override {}
  void stop() override {
    connected_ = false;
    peeked_ = -1;
    if (ssl_) {
      wolfSSL_shutdown(ssl_);
      wolfSSL_free(ssl_);
      ssl_ = nullptr;
    }
    if (socket_ >= 0) {
      close(socket_);
      socket_ = -1;
    }
    if (context_) {
      wolfSSL_CTX_free(context_);
      context_ = nullptr;
    }
    if (initialized_) {
      wolfSSL_Cleanup();
      initialized_ = false;
    }
  }
  uint8_t connected() override {
    if (!ssl_ || !connected_) return 0;
    if (peeked_ >= 0 || wolfSSL_pending(ssl_) > 0) return 1;
    uint8_t value = 0;
    const int result = recv(socket_, &value, 1, MSG_PEEK | MSG_DONTWAIT);
    if (result == 0) {
      lastStage_ = 4;
      connected_ = false;
    }
    else if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      connected_ = false;
    return connected_ ? 1 : 0;
  }
  operator bool() override { return connected(); }
  void setConnectionTimeout(uint32_t milliseconds) override {
    connectionTimeoutMs_ = milliseconds;
  }
  int lastError() const { return lastError_; }
  int lastStage() const { return lastStage_; }

 private:
  int socket_ = -1;
  WOLFSSL_CTX* context_ = nullptr;
  WOLFSSL* ssl_ = nullptr;
  int peeked_ = -1;
  int lastError_ = 0;
  int lastStage_ = 0;
  uint32_t connectionTimeoutMs_ = 12000;
  bool initialized_ = false;
  bool connected_ = false;
};

static bool wolfSslProfileLoginRequest(const char* url, const char* body,
                                       JsonDocument& response, char* error,
                                       size_t errorSize,
                                       bool* applicationJson) {
  WolfSslAuthClient client;
  client.setTimeout(12000);
  const bool ok = cloudJsonRequestWithClient(
      client, url, "POST", body, nullptr, response, error, errorSize,
      applicationJson);
  if (!ok && (strcmp(error, "https_begin_failed") == 0 ||
              strncmp(error, "http_-", strlen("http_-")) == 0)) {
    snprintf(error, errorSize, "wolfssl_io_%d_%d", client.lastStage(),
             client.lastError());
  }
  return ok;
}
#endif

#ifdef BAMBU_AUTH_TRANSPORT_PROBE
static bool classifyProbeHttpResponse(char* raw, size_t received,
                                      JsonDocument& response, char* error,
                                      size_t errorSize, bool* applicationJson);
static bool espTlsProfileLoginRequest(const char* body, JsonDocument& response,
                                      char* error, size_t errorSize,
                                      bool* applicationJson,
                                      const int* cipherOrder) {
  if (applicationJson) *applicationJson = false;
  esp_transport_handle_t transport = esp_transport_ssl_init();
  if (!transport) {
    copyCode(error, errorSize, "esp_tls_init_failed");
    return false;
  }
  esp_transport_ssl_set_cert_data(transport, kBambuCloudCa,
                                  strlen(kBambuCloudCa) + 1);
  esp_transport_ssl_set_tls_version(transport, ESP_TLS_VER_TLS_1_2);
  int supportedOrder[32] = {};
  const int* supported = esp_tls_get_ciphersuites_list();
  size_t supportedCount = 0;
  for (const int* candidate = cipherOrder; *candidate; ++candidate) {
    for (const int* available = supported; available && *available; ++available) {
      if (*candidate == *available) {
        supportedOrder[supportedCount++] = *candidate;
        break;
      }
    }
  }
  if (supportedCount == 0) {
    copyCode(error, errorSize, "esp_tls_no_control_ciphers");
    esp_transport_destroy(transport);
    return false;
  }
  esp_transport_ssl_set_ciphersuites_list(transport, supportedOrder);
#ifdef BAMBU_AUTH_TRANSPORT_CAPTURE
  const int connectResult =
      esp_transport_connect(transport, "192.168.1.166", 8443, 12000);
#else
  const int connectResult =
      esp_transport_connect(transport, "api.bambulab.com", 443, 12000);
#endif
  if (connectResult != 0) {
    int tlsCode = 0;
    int tlsFlags = 0;
    esp_tls_error_handle_t errorHandle = esp_transport_get_error_handle(transport);
    if (errorHandle &&
        esp_tls_get_and_clear_last_error(errorHandle, &tlsCode, &tlsFlags) ==
            ESP_OK) {
      snprintf(error, errorSize, "esp_tls_connect_%x_%x_errno_%d",
               static_cast<unsigned>(-tlsCode),
               static_cast<unsigned>(tlsFlags),
               esp_transport_get_errno(transport));
    } else {
      snprintf(error, errorSize, "esp_tls_connect_failed_errno_%d",
               esp_transport_get_errno(transport));
    }
    esp_transport_destroy(transport);
    return false;
  }

  char deviceId[37] = {};
  makeCloudDeviceId(deviceId);
  static constexpr size_t kMaximumAuthProbeRequestBytes = 1536;
  char* request = static_cast<char*>(calloc(kMaximumAuthProbeRequestBytes, 1));
  if (!request) {
    copyCode(error, errorSize, "json_no_memory");
    bambuSecureZero(deviceId, sizeof(deviceId));
    esp_transport_close(transport);
    esp_transport_destroy(transport);
    return false;
  }
  const int requestLength = snprintf(
      request, kMaximumAuthProbeRequestBytes,
      "POST /v1/user-service/user/login HTTP/1.0\r\n"
      "Host: api.bambulab.com\r\n"
      "User-Agent: bambu_network_agent/02.07.01.51\r\n"
      "X-BBL-Client-Name: BambuStudio\r\n"
      "X-BBL-Client-Type: slicer\r\n"
      "X-BBL-Client-Version: 02.07.01.62\r\n"
      "X-BBL-Device-ID: %s\r\n"
      "X-BBL-Language: en-US\r\n"
      "X-BBL-OS-Type: linux\r\n"
      "X-BBL-OS-Version: esp32\r\n"
      "X-BBL-Agent-Version: 02.07.01.51\r\n"
      "X-BBL-Executable-info: {}\r\n"
      "X-BBL-Agent-OS-Type: linux\r\n"
      "X-BBL-Executable-Env: false\r\n"
      "Accept: application/json\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: %u\r\n"
      "Connection: close\r\n\r\n%s",
      deviceId, static_cast<unsigned>(strlen(body)), body);
  bambuSecureZero(deviceId, sizeof(deviceId));
  if (requestLength <= 0 ||
      requestLength >= static_cast<int>(kMaximumAuthProbeRequestBytes)) {
    copyCode(error, errorSize, "request_oversized");
    bambuSecureZero(request, kMaximumAuthProbeRequestBytes);
    free(request);
    esp_transport_close(transport);
    esp_transport_destroy(transport);
    return false;
  }
  size_t sent = 0;
  while (sent < static_cast<size_t>(requestLength)) {
    const int count = esp_transport_write(
        transport, request + sent, requestLength - sent, 12000);
    if (count <= 0) {
      copyCode(error, errorSize, "esp_tls_write_failed");
      bambuSecureZero(request, kMaximumAuthProbeRequestBytes);
      free(request);
      esp_transport_close(transport);
      esp_transport_destroy(transport);
      return false;
    }
    sent += static_cast<size_t>(count);
  }
  bambuSecureZero(request, kMaximumAuthProbeRequestBytes);
  free(request);

  char* raw = static_cast<char*>(calloc(kMaximumAuthProbeResponseBytes + 1, 1));
  if (!raw) {
    copyCode(error, errorSize, "json_no_memory");
    esp_transport_close(transport);
    esp_transport_destroy(transport);
    return false;
  }
  size_t received = 0;
  while (received < kMaximumAuthProbeResponseBytes) {
    const int count = esp_transport_read(
        transport, raw + received, kMaximumAuthProbeResponseBytes - received,
        2000);
    if (count <= 0) break;
    received += static_cast<size_t>(count);
  }
  bool responseOversized = false;
  if (received == kMaximumAuthProbeResponseBytes) {
    char overflowByte = '\0';
    responseOversized = esp_transport_read(transport, &overflowByte, 1, 2000) > 0;
    bambuSecureZero(&overflowByte, sizeof(overflowByte));
  }
  esp_transport_close(transport);
  esp_transport_destroy(transport);
  if (responseOversized) {
    copyCode(error, errorSize, "response_oversized");
    bambuSecureZero(raw, kMaximumAuthProbeResponseBytes + 1);
    free(raw);
    return false;
  }
  if (received == 0) {
    copyCode(error, errorSize, "esp_tls_read_failed");
    free(raw);
    return false;
  }
  return classifyProbeHttpResponse(raw, received, response, error, errorSize,
                                   applicationJson);
}

static bool classifyProbeHttpResponse(char* raw, size_t received,
                                      JsonDocument& response, char* error,
                                      size_t errorSize, bool* applicationJson) {
  raw[received] = '\0';
  char* headerEnd = strstr(raw, "\r\n\r\n");
  int statusCode = 0;
  if (!headerEnd || sscanf(raw, "HTTP/%*u.%*u %d", &statusCode) != 1) {
    copyCode(error, errorSize, "bad_response");
    bambuSecureZero(raw, kMaximumAuthProbeResponseBytes + 1);
    free(raw);
    return false;
  }
  char* bodyStart = headerEnd + 4;
  const char* contentLength = strcasestr(raw, "\r\nContent-Length:");
  if (contentLength && contentLength < headerEnd) {
    contentLength += strlen("\r\nContent-Length:");
    while (*contentLength == ' ' || *contentLength == '\t') ++contentLength;
    char* lengthEnd = nullptr;
    const unsigned long long declaredLength =
        strtoull(contentLength, &lengthEnd, 10);
    const size_t headerBytes = static_cast<size_t>(bodyStart - raw);
    if (lengthEnd != contentLength &&
        declaredLength > kMaximumAuthProbeResponseBytes - headerBytes) {
      copyCode(error, errorSize, "response_oversized");
      bambuSecureZero(raw, kMaximumAuthProbeResponseBytes + 1);
      free(raw);
      return false;
    }
  }
  const size_t bodyLength = received - static_cast<size_t>(bodyStart - raw);
  JsonDocument parsed;
  const DeserializationError parseError = deserializeJson(
      parsed, bodyStart, bodyLength, DeserializationOption::NestingLimit(8));
  JsonVariantConst root = parsed.as<JsonVariantConst>();
  const bool parsedObject = !parseError && root.is<JsonObjectConst>();
  if (statusCode != 200) {
    const bool hasTypedCode = parsedObject &&
        (root["code"].is<int>() || root["code"].is<const char*>());
    const bool hasTypedError = parsedObject && root["error"].is<const char*>();
    if (applicationJson) {
      *applicationJson = bambuAuthRejectedApplicationJsonCoordinates(
          statusCode, parsedObject, hasTypedCode, hasTypedError);
    }
    bambuAuthHttpFailureCode(statusCode, bodyStart, error, errorSize);
    bambuSecureZero(raw, kMaximumAuthProbeResponseBytes + 1);
    free(raw);
    return false;
  }
  if (!parsedObject) {
    copyCode(error, errorSize,
             parseError == DeserializationError::NoMemory ? "json_no_memory" :
                                                             "bad_response");
    bambuSecureZero(raw, kMaximumAuthProbeResponseBytes + 1);
    free(raw);
    return false;
  }
  response.set(parsed);
  JsonVariantConst data = responseData(response);
  const bool hasAuthData = data.is<JsonObjectConst>() &&
      (data["loginType"].is<const char*>() ||
       data["accessToken"].is<const char*>() ||
       data["access_token"].is<const char*>() ||
       data["refreshToken"].is<const char*>() ||
       data["refresh_token"].is<const char*>());
  if (applicationJson)
    *applicationJson = bambuAuthSuccessfulApplicationJsonCoordinates(
        true, hasAuthData);
  bambuSecureZero(raw, kMaximumAuthProbeResponseBytes + 1);
  free(raw);
  return true;
}

// AT04 adapter: stock Mbed TLS driven directly so the auth probe can control
// ClientHello wire groups (cipher suite list) that the Arduino TLS wrappers do
// not expose. Certificate and hostname validation stay enabled; the wire
// profile otherwise matches the production Mbed TLS ClientHello captured in
// AT04 step 1.
static int mbedtlsProbeBioSend(void* ctx, const unsigned char* buf, size_t len) {
  NetworkClient* client = static_cast<NetworkClient*>(ctx);
  const int written = static_cast<int>(client->write(buf, len));
  return written > 0 ? written : MBEDTLS_ERR_SSL_WANT_WRITE;
}

static int mbedtlsProbeBioRecv(void* ctx, unsigned char* buf, size_t len) {
  NetworkClient* client = static_cast<NetworkClient*>(ctx);
  if (!client->connected()) return MBEDTLS_ERR_NET_CONN_RESET;
  if (client->available() <= 0) return MBEDTLS_ERR_SSL_WANT_READ;
  const int received = client->read(buf, len);
  return received > 0 ? received : MBEDTLS_ERR_SSL_WANT_READ;
}

static bool mbedtlsCipherProfileLoginRequest(const char* body,
                                             JsonDocument& response,
                                             char* error, size_t errorSize,
                                             bool* applicationJson,
                                             const int* cipherOrder,
                                             const char* userAgentOverride,
                                             bool emulateWrapperShape) {
  if (applicationJson) *applicationJson = false;
  NetworkClient tcp;
  tcp.setTimeout(12000);
#ifdef BAMBU_AUTH_TRANSPORT_CAPTURE
  const char* observationHost = "192.168.1.166";
  if (!tcp.connect(observationHost, 8443)) {
#else
  const char* observationHost = "api.bambulab.com";
  if (!tcp.connect(observationHost, 443)) {
#endif
    copyCode(error, errorSize, "mbedtls_tcp_failed");
    return false;
  }

  int ret = psa_crypto_init();
  if (ret != PSA_SUCCESS) {
    snprintf(error, errorSize, "mbedtls_psa_%d", static_cast<int>(ret));
    return false;
  }

  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_x509_crt ca;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  mbedtls_ssl_init(&ssl);
  mbedtls_ssl_config_init(&conf);
  mbedtls_x509_crt_init(&ca);

  bool result = false;
  do {
    ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0);
    if (ret != 0) {
      snprintf(error, errorSize, "mbedtls_seed_%d", ret);
      break;
    }
#ifdef BAMBU_AUTH_TRANSPORT_CAPTURE
    ret = mbedtls_x509_crt_parse(&ca, (const unsigned char*)kCaptureObservationCa,
                                 strlen(kCaptureObservationCa) + 1);
#else
    ret = mbedtls_x509_crt_parse(&ca, (const unsigned char*)kBambuCloudCa,
                                 strlen(kBambuCloudCa) + 1);
#endif
    if (ret != 0) {
      snprintf(error, errorSize, "mbedtls_ca_%d", ret);
      break;
    }
    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
      snprintf(error, errorSize, "mbedtls_defaults_%d", ret);
      break;
    }
    mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &ca, nullptr);
    if (cipherOrder) mbedtls_ssl_conf_ciphersuites(&conf, cipherOrder);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0) {
      snprintf(error, errorSize, "mbedtls_setup_%d", ret);
      break;
    }
    ret = mbedtls_ssl_set_hostname(&ssl, "api.bambulab.com");
    if (ret != 0) {
      snprintf(error, errorSize, "mbedtls_hostname_%d", ret);
      break;
    }
    mbedtls_ssl_set_bio(&ssl, &tcp, mbedtlsProbeBioSend, mbedtlsProbeBioRecv,
                        nullptr);

    const uint32_t handshakeDeadline = millis() + 12000;
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
      if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
          ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
        snprintf(error, errorSize, "mbedtls_hs_%d", ret);
        break;
      }
      if (static_cast<int32_t>(millis() - handshakeDeadline) >= 0) {
        copyCode(error, errorSize, "mbedtls_hs_timeout");
        break;
      }
      delay(2);
    }
    if (ret != 0) break;
    if (mbedtls_ssl_get_verify_result(&ssl) != 0) {
      copyCode(error, errorSize, "mbedtls_verify_failed");
      break;
    }

    char deviceId[37] = {};
    makeCloudDeviceId(deviceId);
    static constexpr size_t kMaximumAuthProbeRequestBytes = 1536;
    char* request = static_cast<char*>(calloc(kMaximumAuthProbeRequestBytes, 1));
    if (!request) {
      copyCode(error, errorSize, "json_no_memory");
      bambuSecureZero(deviceId, sizeof(deviceId));
      break;
    }
    const char* userAgent =
        userAgentOverride ? userAgentOverride : "bambu_network_agent/02.07.01.51";
    int requestLength;
    if (emulateWrapperShape) {
      requestLength = snprintf(
          request, kMaximumAuthProbeRequestBytes,
          "POST /v1/user-service/user/login HTTP/1.0\r\n"
          "Host: api.bambulab.com\r\n"
          "User-Agent: %s\r\n"
          "Connection: close\r\n"
          "X-BBL-Client-Name: BambuStudio\r\n"
          "X-BBL-Client-Type: slicer\r\n"
          "X-BBL-Client-Version: 02.07.01.62\r\n"
          "X-BBL-Device-ID: %s\r\n"
          "X-BBL-Language: en-US\r\n"
          "X-BBL-OS-Type: linux\r\n"
          "X-BBL-OS-Version: esp32\r\n"
          "X-BBL-Agent-Version: 02.07.01.51\r\n"
          "X-BBL-Executable-info: {}\r\n"
          "X-BBL-Agent-OS-Type: linux\r\n"
          "X-BBL-Executable-Env: false\r\n"
          "Accept: application/json\r\n"
          "Content-Type: application/json\r\n"
          "Content-Length: %u\r\n\r\n",
          userAgent, deviceId, static_cast<unsigned>(strlen(body)));
    } else {
      requestLength = snprintf(
          request, kMaximumAuthProbeRequestBytes,
          "POST /v1/user-service/user/login HTTP/1.0\r\n"
          "Host: api.bambulab.com\r\n"
          "User-Agent: %s\r\n"
          "X-BBL-Client-Name: BambuStudio\r\n"
          "X-BBL-Client-Type: slicer\r\n"
          "X-BBL-Client-Version: 02.07.01.62\r\n"
          "X-BBL-Device-ID: %s\r\n"
          "X-BBL-Language: en-US\r\n"
          "X-BBL-OS-Type: linux\r\n"
          "X-BBL-OS-Version: esp32\r\n"
          "X-BBL-Agent-Version: 02.07.01.51\r\n"
          "X-BBL-Executable-info: {}\r\n"
          "X-BBL-Agent-OS-Type: linux\r\n"
          "X-BBL-Executable-Env: false\r\n"
          "Accept: application/json\r\n"
          "Content-Type: application/json\r\n"
          "Content-Length: %u\r\n"
          "Connection: close\r\n\r\n%s",
          userAgent, deviceId, static_cast<unsigned>(strlen(body)), body);
    }
    bambuSecureZero(deviceId, sizeof(deviceId));
    if (requestLength <= 0 ||
        requestLength >= static_cast<int>(kMaximumAuthProbeRequestBytes)) {
      copyCode(error, errorSize, "request_oversized");
      bambuSecureZero(request, kMaximumAuthProbeRequestBytes);
      free(request);
      break;
    }
    size_t sent = 0;
    while (sent < static_cast<size_t>(requestLength)) {
      ret = mbedtls_ssl_write(&ssl, (const unsigned char*)request + sent,
                              requestLength - sent);
      if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        delay(2);
        continue;
      }
      if (ret <= 0) break;
      sent += static_cast<size_t>(ret);
    }
    size_t bodySent = 0;
    if (emulateWrapperShape && sent == static_cast<size_t>(requestLength)) {
      while (bodySent < strlen(body)) {
        ret = mbedtls_ssl_write(&ssl, (const unsigned char*)body + bodySent,
                                strlen(body) - bodySent);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
            ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
          delay(2);
          continue;
        }
        if (ret <= 0) break;
        bodySent += static_cast<size_t>(ret);
      }
    }
    bambuSecureZero(request, kMaximumAuthProbeRequestBytes);
    free(request);
    if (sent != static_cast<size_t>(requestLength) ||
        (emulateWrapperShape && bodySent != strlen(body))) {
      copyCode(error, errorSize, "mbedtls_write_failed");
      break;
    }

    char* raw = static_cast<char*>(calloc(kMaximumAuthProbeResponseBytes + 1, 1));
    if (!raw) {
      copyCode(error, errorSize, "json_no_memory");
      break;
    }
    size_t received = 0;
    const uint32_t readDeadline = millis() + 12000;
    while (received < kMaximumAuthProbeResponseBytes) {
      ret = mbedtls_ssl_read(&ssl, (unsigned char*)raw + received,
                             kMaximumAuthProbeResponseBytes - received);
      if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
          ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        if (static_cast<int32_t>(millis() - readDeadline) >= 0) break;
        delay(2);
        continue;
      }
      if (ret <= 0) break;
      received += static_cast<size_t>(ret);
    }
    if (received == 0) {
      copyCode(error, errorSize, "mbedtls_read_failed");
      bambuSecureZero(raw, kMaximumAuthProbeResponseBytes + 1);
      free(raw);
      break;
    }
    result = classifyProbeHttpResponse(raw, received, response, error, errorSize,
                                       applicationJson);
  } while (false);

  mbedtls_ssl_free(&ssl);
  mbedtls_ssl_config_free(&conf);
  mbedtls_x509_crt_free(&ca);
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  tcp.stop();
  return result;
}
#endif

static bool cloudJsonRequest(SecureNetworkCoordinator& coordinator,
                             const char* url, const char* method,
                             const char* body, const char* bearer,
                             JsonDocument& response, char* error,
                             size_t errorSize,
                             bool* applicationJson = nullptr,
                             uint8_t transportProfile = 0) {
  SecureNetworkLease lease(coordinator, SECURE_NETWORK_CLOUD_HTTP);
  if (!lease.acquired()) {
    copyCode(error, errorSize, "tls_busy");
    return false;
  }
#ifdef BAMBU_AUTH_TRANSPORT_PROBE
  if (transportProfile > kMaximumAuthTransportProfile) {
    copyCode(error, errorSize, "transport_profile_invalid");
    return false;
  }
  if (transportProfile == 6 || transportProfile == 7 || transportProfile == 8 ||
      transportProfile == 9 || transportProfile == 10 ||
      transportProfile == 11 || transportProfile == 12) {
    const int* cipherOrder = transportProfile == 6 ? kStrongTls12CipherOrder
                             : transportProfile == 7 ? kSingleTls12CipherOrder
                                                     : nullptr;
    // Profile 9 reproduces the production wrapper's silently-substituted
    // default User-Agent over the passing adapter, binding the verdict to the
    // HTTP header diff rather than the Mbed TLS ClientHello.
    const char* userAgentOverride = transportProfile == 9
                                        ? "ESP32 HTTP Client/1.1"
                                        : transportProfile == 12
                                              ? "ESP32HTTPClient"
                                              : nullptr;
    const bool emulateWrapperShape =
        transportProfile == 10 || transportProfile == 11;
    return mbedtlsCipherProfileLoginRequest(body, response, error, errorSize,
                                            applicationJson, cipherOrder,
                                            userAgentOverride,
                                            emulateWrapperShape);
  }
  if (transportProfile == 2) {
    return espTlsProfileLoginRequest(body, response, error, errorSize,
                                     applicationJson, kDesktopTls12CipherOrder);
  }
#ifdef BAMBU_AUTH_WOLFSSL_PROBE
  if (transportProfile == 5) {
    return wolfSslProfileLoginRequest(url, body, response, error, errorSize,
                                      applicationJson);
  }
#endif
  if (transportProfile == 4 || transportProfile == 5) {
    copyCode(error, errorSize, "transport_profile_unsupported");
    return false;
  }
#else
  (void)transportProfile;
#endif
#ifdef BAMBU_AUTH_TRANSPORT_CAPTURE
  CaptureObservationClient client;
  client.setTimeout(12000);
  client.setHandshakeTimeout(12);
  return cloudJsonRequestWithClient(client, url, method, body, bearer,
                                    response, error, errorSize,
                                    applicationJson);
#else
  WiFiClientSecure client;
  client.setCACert(kBambuCloudCa);
  client.setTimeout(12000);
  client.setHandshakeTimeout(12);
#ifdef BAMBU_AUTH_TRANSPORT_PROBE
  if (transportProfile == 1) client.setAlpnProtocols(kHttp11Alpn);
#endif
  return cloudJsonRequestWithClient(client, url, method, body, bearer,
                                    response, error, errorSize,
                                    applicationJson);
#endif
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

namespace {
const char* kFinishAnnouncementNamespace = "bambufin";
const char* kTerminalFailureAnnouncementNamespace = "bambufail";
const char* kFinishAnnouncementKey = "ann";

void persistAnnouncementRecord(const char* nameSpace, const char* jobKey) {
  Preferences prefs;
  if (!prefs.begin(nameSpace, false)) return;
  FinishAnnouncementRecord record;
  record.version = kFinishAnnouncementRecordVersion;
  strlcpy(record.jobKey, jobKey, sizeof(record.jobKey));
  prefs.putBytes(kFinishAnnouncementKey, &record, sizeof(record));
  prefs.end();
}

void removeAnnouncementRecord(const char* nameSpace) {
  Preferences prefs;
  if (!prefs.begin(nameSpace, false)) return;
  prefs.remove(kFinishAnnouncementKey);
  prefs.end();
}
}  // namespace

DirectCloudSource::DirectCloudSource() {
  reducer_.configure(true, false, "Direct Cloud не настроен");
}

PrinterStateSnapshot DirectCloudSource::snapshot() const {
  portENTER_CRITICAL(&mux_);
  reducer_.advance(millis());
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
  char jobKey[96];
  finishJobKey(patch.present & PRINTER_PATCH_JOB_NAME ? patch.jobName : "",
               jobKey, sizeof(jobKey));
  const bool finishedPatch =
      (patch.present & PRINTER_PATCH_LIFECYCLE) != 0 &&
      patch.lifecycle == PRINTER_FINISHED;
  const bool terminalFailurePatch =
      (patch.present & PRINTER_PATCH_LIFECYCLE) != 0 &&
      patch.lifecycle == PRINTER_ERROR && patch.terminalFailure;
  const bool activityPatch =
      (patch.present & PRINTER_PATCH_LIFECYCLE) != 0 &&
      (patch.lifecycle == PRINTER_PREPARING ||
       patch.lifecycle == PRINTER_PRINTING ||
       patch.lifecycle == PRINTER_PAUSED ||
       (patch.lifecycle == PRINTER_ERROR && !patch.terminalFailure));
  bool persistAnnouncement = false;
  bool persistTerminalFailureAnnouncement = false;
  bool clearAnnouncement = false;
  bool clearTerminalFailureAnnouncement = false;
  portENTER_CRITICAL(&mux_);
  if (finishedPatch && !reducer_.finishedGraceActive() &&
      !reducer_.finishedAnnounced()) {
    // Первый FINISH этой сессии: если финиш той же работы уже объявляли
    // до перезагрузки (NVS-запись), повторный «свежий финиш» не показываем.
    if (shouldAnnounceBootFinished(finishRecord_, jobKey)) {
      persistAnnouncement = true;
    } else {
      reducer_.suppressFinishedAnnouncement();
    }
  }
  if (terminalFailurePatch && !reducer_.terminalFailureGraceActive() &&
      !reducer_.terminalFailureAnnounced()) {
    if (shouldAnnounceBootFinished(terminalFailureRecord_, jobKey)) {
      persistTerminalFailureAnnouncement = true;
    } else {
      reducer_.suppressTerminalFailureAnnouncement();
    }
  }
  if (activityPatch && finishAnnouncementRecordValid(finishRecord_))
    clearAnnouncement = true;
  if (activityPatch &&
      finishAnnouncementRecordValid(terminalFailureRecord_))
    clearTerminalFailureAnnouncement = true;
  reducer_.apply(patch, nowMs);
  portEXIT_CRITICAL(&mux_);
  // NVS-операции — вне критической секции (flash-запись блокирующая).
  if (persistAnnouncement) {
    persistAnnouncementRecord(kFinishAnnouncementNamespace, jobKey);
    portENTER_CRITICAL(&mux_);
    finishRecord_ = FinishAnnouncementRecord();
    finishRecord_.version = kFinishAnnouncementRecordVersion;
    strlcpy(finishRecord_.jobKey, jobKey, sizeof(finishRecord_.jobKey));
    portEXIT_CRITICAL(&mux_);
  }
  if (persistTerminalFailureAnnouncement) {
    persistAnnouncementRecord(kTerminalFailureAnnouncementNamespace, jobKey);
    portENTER_CRITICAL(&mux_);
    terminalFailureRecord_ = FinishAnnouncementRecord();
    terminalFailureRecord_.version = kFinishAnnouncementRecordVersion;
    strlcpy(terminalFailureRecord_.jobKey, jobKey,
            sizeof(terminalFailureRecord_.jobKey));
    portEXIT_CRITICAL(&mux_);
  }
  if (clearAnnouncement) clearFinishAnnouncementRecord();
  if (clearTerminalFailureAnnouncement) {
    removeAnnouncementRecord(kTerminalFailureAnnouncementNamespace);
    portENTER_CRITICAL(&mux_);
    terminalFailureRecord_ = FinishAnnouncementRecord();
    portEXIT_CRITICAL(&mux_);
  }
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

void DirectCloudSource::loadFinishAnnouncementRecord() {
  const char* namespaces[] = {
      kFinishAnnouncementNamespace,
      kTerminalFailureAnnouncementNamespace,
  };
  FinishAnnouncementRecord* records[] = {
      &finishRecord_,
      &terminalFailureRecord_,
  };
  for (uint8_t i = 0; i < 2; ++i) {
    Preferences prefs;
    if (!prefs.begin(namespaces[i], true)) continue;
    if (prefs.getBytesLength(kFinishAnnouncementKey) == sizeof(*records[i])) {
      FinishAnnouncementRecord loaded;
      prefs.getBytes(kFinishAnnouncementKey, &loaded, sizeof(loaded));
      portENTER_CRITICAL(&mux_);
      *records[i] = loaded;
      portEXIT_CRITICAL(&mux_);
    }
    prefs.end();
  }
}

void DirectCloudSource::clearFinishAnnouncementRecord() {
  removeAnnouncementRecord(kFinishAnnouncementNamespace);
  removeAnnouncementRecord(kTerminalFailureAnnouncementNamespace);
  portENTER_CRITICAL(&mux_);
  finishRecord_ = FinishAnnouncementRecord();
  terminalFailureRecord_ = FinishAnnouncementRecord();
  portEXIT_CRITICAL(&mux_);
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
      operationDeadlineMs_(0), nextRefreshAttemptMs_(0),
      lastSuccessfulRefreshMs_(0), refreshFailureCount_(0),
      refreshRetryScheduled_(false), refreshCandidatePending_(false),
      activeCredentialBlocked_(false),
      mqttClient_(nullptr), mqttLeaseHeld_(false),
      mqttUsingCandidate_(false), initialized_(false), wifiWasAvailable_(false) {
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
  const bool recoveryMatchesActive =
      hasActive && bambuRefreshRecoveryMatches(refreshRecovery_,
                                               active.accountUid);
  if (bambuRefreshRecoveryValid(refreshRecovery_) &&
      !recoveryMatchesActive) {
    clearRefreshRecovery();
  }
  xSemaphoreTake(mutex_, portMAX_DELAY);
  status_.hasActiveCredential = hasActive;
  status_.candidatePending = false;
  status_.phase = hasActive ? BAMBU_AUTH_CONNECTING : BAMBU_AUTH_SIGNED_OUT;
  if (recoveryMatchesActive) {
    // The previous refresh reached the Cloud but did not complete local
    // promotion. Resume it immediately instead of waiting for active expiry.
    refreshRetryScheduled_ = true;
    nextRefreshAttemptMs_ = millis();
  }
  xSemaphoreGive(mutex_);
  source_.configure(hasActive, hasActive ? "Direct Cloud ждёт MQTT" : "Direct Cloud не настроен");
  source_.loadFinishAnnouncementRecord();
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
                             bool chinaRegion, char operationId[17],
                             uint8_t transportProfile) {
  if (!initialized_ || !email || !email[0] || !password ||
      strlen(email) >= sizeof(request_.account) || strlen(password) < 1 ||
      strlen(password) >= sizeof(request_.password)) return false;
#ifdef BAMBU_AUTH_TRANSPORT_PROBE
  if (transportProfile > kMaximumAuthTransportProfile) return false;
#else
  (void)transportProfile;
#endif
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
#ifdef BAMBU_AUTH_TRANSPORT_PROBE
  request_.transportProfile = transportProfile;
#endif
  strlcpy(request_.account, email, sizeof(request_.account));
  strlcpy(request_.password, password, sizeof(request_.password));
  makeOperationId(request_.operationId);
  strlcpy(status_.operationId, request_.operationId, sizeof(status_.operationId));
  strlcpy(operationId, request_.operationId, 17);
  status_.error[0] = '\0';
#ifdef BAMBU_AUTH_TRANSPORT_PROBE
  status_.authTransportApplicationJson = false;
  status_.authTransportProfile = transportProfile;
  status_.authTransport200Length = -2;
  status_.authTransport200Read = 0;
  status_.authTransport200First = -1;
  status_.authTransport200Error = 255;
  status_.authTransport200Code = -2;
  status_.authTransportShape = 0;
  status_.authTransportCall = 0;
  status_.authTransportKeyCount = 0;
  memset(status_.authTransportKeys, 0, sizeof(status_.authTransportKeys));
  status_.authTransportTailLen = 0;
  memset(status_.authTransportTail, 0, sizeof(status_.authTransportTail));
#endif
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
  copy.recoveryReason = mqttRecovery_.reason();
  copy.recoveryFailures = mqttRecovery_.failures();
  const uint32_t mqttReserve = mqttTaskMinimumReserve_.load();
  copy.mqttTaskStackReserve =
      mqttReserve == UINT32_MAX ? 0 : mqttReserve;
  if (mqttRecovery_.deadlineMs() && !mqttRecovery_.ready(millis()))
    copy.nextRecoveryAttemptIn =
        static_cast<uint32_t>(mqttRecovery_.deadlineMs() - millis()) / 1000UL;
  if (refreshRetryScheduled_ &&
      !bambuRefreshDeadlineReached(millis(), nextRefreshAttemptMs_)) {
    copy.nextRefreshAttemptIn =
        static_cast<uint32_t>(nextRefreshAttemptMs_ - millis()) / 1000UL;
  } else {
    copy.nextRefreshAttemptIn = 0;
  }
  xSemaphoreGive(mutex_);
  return copy;
}

#ifdef BAMBU_AUTH_TRANSPORT_PROBE
void BambuCloud::persistProbeTransport(uint8_t callTag) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  status_.authTransport200Length = gProbe200Length;
  status_.authTransport200Read = gProbe200Read;
  status_.authTransport200First = gProbe200First;
  status_.authTransport200Error = gProbe200Error;
  status_.authTransport200Code = gProbe200Code;
  status_.authTransportShape = gProbe200Shape;
  status_.authTransportCall = callTag;
  status_.authTransportKeyCount = gProbeKeyCount;
  memcpy(status_.authTransportKeys, gProbeKeys, sizeof(status_.authTransportKeys));
  status_.authTransportTailLen = gProbeTailLen;
  memcpy(status_.authTransportTail, gProbeTail, sizeof(status_.authTransportTail));
  xSemaphoreGive(mutex_);
}
#endif

void BambuCloud::noteRefreshFailure() {
  mqttRecovery_.fail(RECOVERY_API, millis());
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (refreshFailureCount_ < 255) ++refreshFailureCount_;
  refreshRetryScheduled_ = true;
  nextRefreshAttemptMs_ =
      millis() + bambuRefreshRetryDelayMs(refreshFailureCount_);
  status_.automaticRefreshFailures = refreshFailureCount_;
  status_.automaticRefreshPending = false;
  xSemaphoreGive(mutex_);
}

void BambuCloud::noteRefreshSuccess(bool refreshed) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  refreshFailureCount_ = 0;
  refreshRetryScheduled_ = false;
  nextRefreshAttemptMs_ = 0;
  lastSuccessfulRefreshMs_ = refreshed ? millis() : 0;
  activeCredentialBlocked_ = false;
  status_.automaticRefreshFailures = 0;
  status_.automaticRefreshPending = false;
  xSemaphoreGive(mutex_);
}

void BambuCloud::maybeScheduleAutomaticRefresh() {
  if (credentialTx_.hasCandidate()) return;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool phaseAllowsRefresh =
      status_.phase == BAMBU_AUTH_CONNECTING ||
      status_.phase == BAMBU_AUTH_READY ||
      status_.phase == BAMBU_AUTH_RECOVERABLE_ERROR;
  const bool canSchedule = request_.action == ACTION_NONE &&
                           status_.hasActiveCredential && phaseAllowsRefresh;
  const bool retryScheduled = refreshRetryScheduled_;
  const uint32_t nextAttempt = nextRefreshAttemptMs_;
  const uint32_t lastSuccess = lastSuccessfulRefreshMs_;
  xSemaphoreGive(mutex_);
  if (!canSchedule) return;

  BambuCloudCredential active;
  if (!credentialTx_.active(active)) return;
  const PrinterStateSnapshot printer = source_.snapshot();
  const auto lifecycleIsBusy = [](PrinterLifecycle lifecycle) {
    return lifecycle == PRINTER_PREPARING || lifecycle == PRINTER_PRINTING ||
           lifecycle == PRINTER_PAUSED;
  };
  BambuRefreshPolicyInput input;
  input.nowEpoch = static_cast<uint32_t>(time(nullptr));
  input.uptimeMs = millis();
  input.accessExpiresAt = active.accessExpiresAt;
  input.refreshExpiresAt = active.refreshExpiresAt;
  input.lastSuccessfulRefreshMs = lastSuccess;
  input.nextAttemptMs = nextAttempt;
  input.retryScheduled = retryScheduled;
  input.printerBusy = lifecycleIsBusy(printer.lifecycle) ||
                      lifecycleIsBusy(printer.visibleLifecycle);
  const BambuRefreshDecision decision = bambuRefreshDecision(input);
  bambuSecureZero(&active, sizeof(active));
  if (decision == BAMBU_REFRESH_WAIT) return;

  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (request_.action == ACTION_NONE && status_.hasActiveCredential &&
      !credentialTx_.hasCandidate()) {
    request_.action = ACTION_REFRESH;
    status_.phase = BAMBU_AUTH_CONNECTING;
    status_.automaticRefreshPending = true;
  }
  xSemaphoreGive(mutex_);
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

void BambuCloud::service(bool enabled, bool wifiAvailable, bool otaActive) {
  if (!initialized_) return;
  // Тумблер bambuEnabled — мастер-выключатель интеграции: при выключении
  // останавливаем транспорт и фоновые операции ровно один раз, в тихом
  // режиме никакой Cloud-активности (MQTT/TLS/auto-refresh) не ведём.
  const DirectCloudServiceMode serviceMode = serviceGate_.decide(enabled);
  if (serviceMode == DirectCloudServiceMode::kTeardown) {
    SemaphoreGuard teardown(operationMutex_);
    stopMqtt();
    source_.resetValidation();
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (request_.action != ACTION_NONE) clearPendingRequest();
    if (operationDeadlineMs_ != 0 &&
        status_.phase != BAMBU_AUTH_SIGNED_OUT &&
        status_.phase != BAMBU_AUTH_READY) {
      // Незавершённая операция входа прерывается выключением тумблера.
      clearPendingAuth();
      operationDeadlineMs_ = 0;
      status_.phase = BAMBU_AUTH_RECOVERABLE_ERROR;
      status_.operationId[0] = '\0';
      strlcpy(status_.error, "bambu_disabled", sizeof(status_.error));
    }
    xSemaphoreGive(mutex_);
  }
  source_.service(millis());
  if (serviceMode != DirectCloudServiceMode::kActive) return;
  SemaphoreGuard operation(operationMutex_);

  const bool expired = operationExpired();
  if (credentialTx_.hasCandidate() &&
      (candidateTransportFailed_.exchange(false) || expired)) {
    const bool refreshFailed = refreshCandidatePending_;
    stopMqtt();
    credentialTx_.rollback();
    removeCandidateFromNvs();
    mqttUsingCandidate_.store(false);
    refreshCandidatePending_ = false;
    if (refreshFailed) noteRefreshFailure();
    clearPendingAuth();
    fail(refreshFailed ? "auto_refresh_retry" :
         (expired ? "candidate_report_timeout" : "candidate_mqtt_failed"));
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
    if (wifiWasAvailable_) mqttRecovery_.fail(RECOVERY_WIFI, millis());
    wifiWasAvailable_ = false;
    return;
  }
  if (!wifiWasAvailable_) {
    wifiWasAvailable_ = true;
    mqttRecovery_.reset();
    activeTransportFailureReason_.store(RECOVERY_MQTT);
  }

  if (activeTransportFailed_.exchange(false)) {
    stopMqtt();
  }

  if (activeCredentialRejected_.exchange(false)) {
    stopMqtt();
    xSemaphoreTake(mutex_, portMAX_DELAY);
    activeCredentialBlocked_ = true;
    const bool retryDue = !refreshRetryScheduled_ ||
                          bambuRefreshDeadlineReached(
                              millis(), nextRefreshAttemptMs_);
    if (retryDue && request_.action == ACTION_NONE &&
        status_.hasActiveCredential && !credentialTx_.hasCandidate()) {
      request_.action = ACTION_REFRESH;
      status_.phase = BAMBU_AUTH_CONNECTING;
      status_.automaticRefreshPending = true;
    }
    xSemaphoreGive(mutex_);
  }

  maybeScheduleAutomaticRefresh();

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
    if (mayConnect && available && coordinator_.available() &&
        mqttRecovery_.ready(millis())) {
      if (!candidate && activeCredentialBlocked_) {
        bambuSecureZero(&credential, sizeof(credential));
        return;
      }
      // DNS-race gate: esp-tls teardown after an unresolvable broker host
      // panics on core 3.3.8 (Store access fault / udp_remove TCPIP-lock
      // assert) when Wi-Fi just came up and the resolver is not usable yet.
      // WL_CONNECTED is not proof: confirm the resolve first, failures go to
      // the bounded RECOVERY_DNS backoff instead of crash-reboot roulette.
      char brokerHost[32];
      snprintf(brokerHost, sizeof(brokerHost), "%s.mqtt.bambulab.com",
               credential.chinaRegion ? "cn" : "us");
      IPAddress brokerAddress;
      const bool brokerResolved = WiFi.hostByName(brokerHost, brokerAddress) == 1;
      if (mqttConnectGate(wifiAvailable, brokerResolved) ==
          MqttConnectGate::kAwaitDns) {
        mqttRecovery_.fail(RECOVERY_DNS, millis());
        Serial.printf("[Bambu] DNS gate: %s не разрешается, повтор через %u мс\n",
                      brokerHost,
                      (unsigned)BoundedRecoveryBackoff::delayMs(
                          mqttRecovery_.failures()));
      } else {
        source_.resetValidation();
        if (!startMqtt(credential)) {
          if (candidate) candidateTransportFailed_.store(true);
          else {
            mqttRecovery_.fail(RECOVERY_MQTT, millis());
            fail("mqtt_start_retry");
          }
        }
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
#ifdef BAMBU_AUTH_TRANSPORT_PROBE
  bool applicationJson = false;
  bool ok = cloudJsonRequest(coordinator_, url, "POST", body, nullptr,
                             response, error, errorSize, &applicationJson,
                             request.transportProfile);
  xSemaphoreTake(mutex_, portMAX_DELAY);
  status_.authTransportApplicationJson = applicationJson;
  xSemaphoreGive(mutex_);
  persistProbeTransport(verification ? 6 : 1);
#else
  bool ok = cloudJsonRequest(coordinator_, url, "POST", body, nullptr,
                             response, error, errorSize);
#endif
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
                             response, error, errorSize);
  bambuSecureZero(body, sizeof(body));
  requestDoc.clear();
#ifdef BAMBU_AUTH_TRANSPORT_PROBE
  persistProbeTransport(2);
#endif
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
  char sessionCookie[kMaximumPortalCookieBytes] = "";
  bool ok = portalTotpRequest(coordinator_, body, sessionCookie,
                              sizeof(sessionCookie), error, errorSize);
  bambuSecureZero(body, sizeof(body));
  if (!ok) {
    bambuSecureZero(sessionCookie, sizeof(sessionCookie));
    return false;
  }
  if (!sessionCookie[0]) {
    bambuSecureZero(sessionCookie, sizeof(sessionCookie));
    copyCode(error, errorSize, "tfa_session_cookie_missing");
    return false;
  }

  ok = portalTokenRequest(
      coordinator_, sessionCookie, pendingAuth_.accessToken,
      sizeof(pendingAuth_.accessToken), pendingAuth_.refreshToken,
      sizeof(pendingAuth_.refreshToken), pendingAuth_.accessExpiresIn,
      pendingAuth_.refreshExpiresIn, error, errorSize);
  bambuSecureZero(sessionCookie, sizeof(sessionCookie));
  return ok;
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
                        pendingAuth_.accessToken, profile, error, errorSize))
    return false;
  JsonVariantConst profileData = responseData(profile);
#ifdef BAMBU_AUTH_TRANSPORT_PROBE
  persistProbeTransport(3);
#endif
  const char* uid = firstString(profileData, "uid", "userId", "user_id");
  if (!uid[0]) uid = firstString(profileData, "accountId", "account_id");
  char uidText[BAMBU_CLOUD_UID_CAPACITY] = {};
  if (uid[0]) {
    strlcpy(uidText, uid, sizeof(uidText));
  } else {
    // Bambu отдаёт uid целым числом (измерено в AT06, ключи preference:
    // uid/name/handle/...), принимаем и строку, и неотрицательное целое.
    static constexpr const char* kUidKeys[] = {"uid", "userId", "user_id",
                                               "accountId", "account_id"};
    for (const char* key : kUidKeys) {
      JsonVariantConst candidate = profileData[key];
      if (!candidate.is<int64_t>()) continue;
      const int64_t numeric = candidate.as<int64_t>();
      if (numeric < 0) continue;
      snprintf(uidText, sizeof(uidText), "%llu",
               static_cast<unsigned long long>(numeric));
      break;
    }
  }
  if (!uidText[0] || strlen(uidText) >= sizeof(pendingAuth_.uid)) {
    copyCode(error, errorSize, "account_identity_missing");
    return false;
  }
  strlcpy(pendingAuth_.uid, uidText, sizeof(pendingAuth_.uid));

  snprintf(url, sizeof(url), "%s/v1/iot-service/api/user/bind", apiHost);
  JsonDocument directory;
  if (!cloudJsonRequest(coordinator_, url, "GET", nullptr,
                        pendingAuth_.accessToken, directory, error, errorSize))
    return false;
  JsonVariantConst directoryData = responseData(directory);
#ifdef BAMBU_AUTH_TRANSPORT_PROBE
  persistProbeTransport(4);
#endif
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
  refreshCandidatePending_ = false;
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
  refreshCandidatePending_ = false;
  BambuCloudCredential active;
  if (!credentialTx_.active(active)) {
    noteRefreshFailure();
    fail("no_active_credential");
    return;
  }
  stopMqtt();
  const char* postedRefreshToken =
      bambuRefreshRecoveryMatches(refreshRecovery_, active.accountUid)
          ? refreshRecovery_.refreshToken
          : active.refreshToken;
  JsonDocument requestDoc;
  requestDoc["refreshToken"] = postedRefreshToken;
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
                             response, error, sizeof(error));
  bambuSecureZero(body, sizeof(body));
  BambuCloudCredential candidate;
  if (ok) {
    JsonVariantConst data = responseData(response);
    const char* access = firstString(data, "accessToken", "access_token");
    const char* refresh = firstString(data, "refreshToken", "refresh_token");
    const char* nextRefreshToken = refresh[0] ? refresh : postedRefreshToken;
    const uint32_t accessLifetime = data["expiresIn"] | 0;
    const uint32_t refreshLifetime = data["refreshExpiresIn"] | 0;
    const uint32_t accessExpiresAt = expiryTimestamp(
        accessLifetime ? accessLifetime : kFallbackTokenLifetimeSeconds);
    uint32_t refreshExpiresAt = refreshLifetime
                                    ? expiryTimestamp(refreshLifetime)
                                    : active.refreshExpiresAt;
    const time_t now = time(nullptr);
    if (!refreshExpiresAt ||
        (now >= static_cast<time_t>(BAMBU_REFRESH_VALID_EPOCH) &&
         refreshExpiresAt <= static_cast<uint32_t>(now))) {
      refreshExpiresAt = expiryTimestamp(kFallbackTokenLifetimeSeconds);
    }
    ok = bambuBuildRefreshCandidate(
        active, access, nextRefreshToken, accessExpiresAt, refreshExpiresAt,
        candidate);
    if (ok) {
      ok = persistRefreshRecovery(active.accountUid, nextRefreshToken);
    }
    if (access[0]) bambuSecureZero(const_cast<char*>(access), strlen(access));
    if (refresh[0]) bambuSecureZero(const_cast<char*>(refresh), strlen(refresh));
  }
  if (ok) {
    ok = credentialTx_.begin(candidate) && persistCandidate();
    refreshCandidatePending_ = ok;
  }
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
    refreshCandidatePending_ = false;
    source_.resetValidation();
    if (!activeCredentialBlocked_) startMqtt(active);
    noteRefreshFailure();
    const bool rejected = strcmp(error, "http_400") == 0 ||
                          strcmp(error, "http_401") == 0 ||
                          strcmp(error, "http_403") == 0;
    fail(rejected ? "auto_refresh_rejected" : "auto_refresh_retry");
  }
  bambuSecureZero(&candidate, sizeof(candidate));
  bambuSecureZero(&active, sizeof(active));
}

void BambuCloud::processLogout() {
  stopMqtt();
  credentialTx_.logout();
  // Сброс валидности отчётов обязателен: иначе service() продолжает видеть
  // устаревший firstValidReport и возвращает фазу в READY после SIGNED_OUT
  // (обнаружено в AT06: logout «залипал» в ready при полностью стёртых
  // учётных данных).
  source_.resetValidation();
  source_.clearFinishAnnouncementRecord();
  bambuSecureZero(&refreshRecovery_, sizeof(refreshRecovery_));
  eraseCredentialNvs();
  clearPendingAuth();
  xSemaphoreTake(mutex_, portMAX_DELAY);
  bambuSecureZero(&status_, sizeof(status_));
  status_.phase = BAMBU_AUTH_SIGNED_OUT;
  status_.automaticRefreshFailures = 0;
  status_.automaticRefreshPending = false;
  operationDeadlineMs_ = 0;
  nextRefreshAttemptMs_ = 0;
  lastSuccessfulRefreshMs_ = 0;
  refreshFailureCount_ = 0;
  refreshRetryScheduled_ = false;
  refreshCandidatePending_ = false;
  activeCredentialBlocked_ = false;
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
  config.network.reconnect_timeout_ms = 10000;
  config.network.disable_auto_reconnect = true;
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
    // Grace: stop() aborts the socket while the client task may still be
    // unwinding the transport_read error (esp-tls read error ->
    // mqtt_process_receive -2); destroying immediately faults there
    // (Store access fault, core 3.3.8). Let the unwind finish first.
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_mqtt_client_destroy(mqttClient_);
    mqttClient_ = nullptr;
    mqttConnectedAtMs_.store(0);
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

bool BambuCloud::pauseForExternalTls() {
  if (!operationMutex_) return false;
  SemaphoreGuard operation(operationMutex_);
  const PrinterStateSnapshot printer = source_.snapshot();
  const bool activePrint = printer.lifecycle == PRINTER_PREPARING ||
                           printer.lifecycle == PRINTER_PRINTING ||
                           printer.lifecycle == PRINTER_PAUSED ||
                           printer.visibleLifecycle == PRINTER_PREPARING ||
                           printer.visibleLifecycle == PRINTER_PRINTING ||
                           printer.visibleLifecycle == PRINTER_PAUSED;
  // Pause guard: stop+destroy on a freshly connected client races the
  // esp-mqtt task's first-read unwind and faults on core 3.3.8 (warm-boot
  // crash loop: weather's HTTPS branch paused a seconds-old connection).
  if (shouldDeferExternalTlsPause(
          mqttClient_ != nullptr, activePrint, source_.firstValidReport(),
          millis() - mqttConnectedAtMs_.load())) {
    return false;
  }
  externalTlsPaused_.store(true);
  stopMqtt();
  return true;
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
  const uint32_t reserve = uxTaskGetStackHighWaterMark(nullptr);
  uint32_t previousReserve = mqttTaskMinimumReserve_.load();
  while (reserve < previousReserve &&
         !mqttTaskMinimumReserve_.compare_exchange_weak(previousReserve,
                                                         reserve)) {}
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
    mqttConnectedAtMs_.store(millis());
    mqttRecovery_.reset();
    activeTransportFailureReason_.store(RECOVERY_MQTT);
    source_.setTransportConnected(true, "MQTT подключен, ждём report");
  } else if (event->event_id == MQTT_EVENT_DISCONNECTED) {
    if (mqttUsingCandidate_.load() && !mqttStopping_.load() &&
        !source_.firstValidReport())
      candidateTransportFailed_.store(true);
    else if (!mqttStopping_.load()) {
      const RecoveryReason reason = static_cast<RecoveryReason>(
          activeTransportFailureReason_.exchange(RECOVERY_MQTT));
      mqttRecovery_.fail(reason, millis());
      activeTransportFailed_.store(true);
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    status_.mqttConnected = false;
    status_.mqttReconnectCount++;
    xSemaphoreGive(mutex_);
    source_.setTransportConnected(false, "MQTT reconnect");
  } else if (event->event_id == MQTT_EVENT_ERROR) {
    if (mqttUsingCandidate_.load() && !mqttStopping_.load() &&
        !source_.firstValidReport())
      candidateTransportFailed_.store(true);
    else if (!mqttStopping_.load() && event->error_handle &&
             event->error_handle->error_type ==
                 MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
             (event->error_handle->connect_return_code ==
                  MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
              event->error_handle->connect_return_code ==
                  MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED))
      activeCredentialRejected_.store(true);
    else if (!mqttStopping_.load() && !mqttUsingCandidate_.load()) {
      RecoveryReason reason = RECOVERY_MQTT;
      if (event->error_handle &&
          event->error_handle->esp_tls_last_esp_err ==
              ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
        reason = RECOVERY_DNS;
      }
      activeTransportFailureReason_.store(reason);
    }
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
    const BambuCloudCredential* validated = credentialTx_.candidate();
    // Replace any recovery token from an older account before committing a
    // newly validated login. If removal later fails, reboot still sees a token
    // that belongs to the newly promoted active credential.
    const bool recoveryReady =
        validated && persistRefreshRecovery(validated->accountUid,
                                             validated->refreshToken);
    if (!recoveryReady || !persistPromotion()) {
      const bool refreshFailed = refreshCandidatePending_;
      stopMqtt();
      credentialTx_.rollback();
      removeCandidateFromNvs();
      mqttUsingCandidate_.store(false);
      refreshCandidatePending_ = false;
      if (refreshFailed) noteRefreshFailure();
      clearPendingAuth();
      fail(refreshFailed ? "auto_refresh_retry" : "candidate_commit_failed");
      return;
    }
    const bool refreshSucceeded = refreshCandidatePending_;
    refreshCandidatePending_ = false;
    clearRefreshRecovery();
    noteRefreshSuccess(refreshSucceeded);
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
  if (store.getBytesLength(kRefreshRecoveryNvsKey) ==
          sizeof(BambuRefreshRecovery) &&
      store.getBytes(kRefreshRecoveryNvsKey, &refreshRecovery_,
                     sizeof(refreshRecovery_)) == sizeof(refreshRecovery_) &&
      bambuRefreshRecoveryValid(refreshRecovery_)) {
    // A valid recovery token takes precedence over the active credential until
    // a refreshed candidate is promoted.
  } else {
    bambuSecureZero(&refreshRecovery_, sizeof(refreshRecovery_));
    store.remove(kRefreshRecoveryNvsKey);
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

bool BambuCloud::persistRefreshRecovery(const char* accountUid,
                                        const char* refreshToken) {
  if (bambuRefreshRecoveryMatches(refreshRecovery_, accountUid) &&
      refreshToken &&
      strcmp(refreshRecovery_.refreshToken, refreshToken) == 0) {
    return true;
  }
  BambuRefreshRecovery candidate;
  if (!bambuRefreshRecoverySet(candidate, accountUid, refreshToken)) {
    return false;
  }
  Preferences store;
  if (!store.begin("bambucloud", false)) {
    bambuSecureZero(&candidate, sizeof(candidate));
    return false;
  }
  const bool ok =
      store.putBytes(kRefreshRecoveryNvsKey, &candidate, sizeof(candidate)) ==
      sizeof(candidate);
  store.end();
  if (ok) refreshRecovery_ = candidate;
  bambuSecureZero(&candidate, sizeof(candidate));
  return ok;
}

void BambuCloud::clearRefreshRecovery() {
  Preferences store;
  if (store.begin("bambucloud", false)) {
    store.remove(kRefreshRecoveryNvsKey);
    store.end();
  }
  bambuSecureZero(&refreshRecovery_, sizeof(refreshRecovery_));
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
