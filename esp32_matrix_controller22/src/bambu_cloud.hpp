#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <atomic>
#include <esp_event.h>
#include <mqtt_client.h>

#include "bambu_cloud_credentials.hpp"
#include "bambu_refresh_policy.hpp"
#include "bambu_report_parser.hpp"
#include "printer_status_source.hpp"
#include "secure_network_coordinator.hpp"
#include "t05_resilience.hpp"

enum BambuAuthPhase : uint8_t {
  BAMBU_AUTH_SIGNED_OUT = 0,
  BAMBU_AUTH_SUBMITTING_CREDENTIALS,
  BAMBU_AUTH_VERIFICATION_EMAIL,
  BAMBU_AUTH_VERIFICATION_SMS,
  BAMBU_AUTH_VERIFICATION_TOTP,
  BAMBU_AUTH_FETCHING_PRINTERS,
  BAMBU_AUTH_PRINTER_SELECTION,
  BAMBU_AUTH_CONNECTING,
  BAMBU_AUTH_READY,
  BAMBU_AUTH_RECOVERABLE_ERROR,
};

struct BambuCloudPrinter {
  char serial[BAMBU_CLOUD_SERIAL_CAPACITY];
  char name[64];
};

struct BambuCloudPublicStatus {
  BambuAuthPhase phase;
  char operationId[17];
  uint32_t operationExpiresIn;
  char error[64];
  bool hasActiveCredential;
  bool candidatePending;
  bool mqttConnected;
  bool firstValidReport;
  uint8_t printerCount;
  BambuCloudPrinter printers[8];
  uint32_t mqttReconnectCount;
  uint32_t rejectedReports;
  uint32_t maximumReportBytes;
  uint8_t automaticRefreshFailures;
  uint32_t nextRefreshAttemptIn;
  bool automaticRefreshPending;
  uint8_t recoveryReason;
  uint8_t recoveryFailures;
  uint32_t nextRecoveryAttemptIn;
  uint32_t mqttTaskStackReserve;
};

class DirectCloudSource : public PrinterStatusSource {
 public:
  DirectCloudSource();
  PrinterStateSnapshot snapshot() const override;
  void configure(bool configured, const char* detail);
  void setTransportConnected(bool connected, const char* detail);
  bool acceptFragment(const char* data, size_t length, size_t offset,
                      size_t totalLength, uint32_t nowMs);
  void service(uint32_t nowMs);
  void resetValidation();
  bool firstValidReport() const { return firstValidReport_.load(); }
  uint32_t rejectedReports() const { return rejectedReports_.load(); }
  uint32_t maximumReportBytes() const { return maximumReportBytes_.load(); }

 private:
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  PrinterStatusReducer reducer_;
  BambuReportParser parser_;
  std::atomic<bool> firstValidReport_{false};
  std::atomic<uint32_t> rejectedReports_{0};
  std::atomic<uint32_t> maximumReportBytes_{0};
  std::atomic<uint32_t> lastValidReportMs_{0};
  std::atomic<bool> staleMarked_{false};
};

class BambuCloud {
 public:
  explicit BambuCloud(SecureNetworkCoordinator& coordinator);
  bool begin();
  void service(bool wifiConnected, bool otaInProgress);
  void stopForOta();
  bool pauseForExternalTls();
  void resumeAfterExternalTls();

  bool submitLogin(const char* email, const char* password, bool chinaRegion,
                   char operationId[17]);
  bool submitVerification(const char* operationId, const char* code);
  bool selectPrinter(const char* operationId, const char* serial);
  bool logout();

  BambuCloudPublicStatus publicStatus() const;
  DirectCloudSource& source() { return source_; }

 private:
  enum PendingAction : uint8_t {
    ACTION_NONE = 0,
    ACTION_LOGIN,
    ACTION_VERIFY,
    ACTION_SELECT,
    ACTION_REFRESH,
    ACTION_LOGOUT,
  };
  struct PendingRequest {
    PendingAction action;
    bool chinaRegion;
    char operationId[17];
    char account[96];
    char password[96];
    char code[16];
    char serial[BAMBU_CLOUD_SERIAL_CAPACITY];
  };
  struct PendingAuth {
    char account[96];
    char tfaKey[192];
    char accessToken[BAMBU_CLOUD_TOKEN_CAPACITY];
    char refreshToken[BAMBU_CLOUD_TOKEN_CAPACITY];
    char uid[BAMBU_CLOUD_UID_CAPACITY];
    uint32_t accessExpiresIn;
    uint32_t refreshExpiresIn;
    bool chinaRegion;
  };

  static void mqttEventThunk(void* handlerArgs, esp_event_base_t base,
                             int32_t eventId, void* eventData);
  void handleMqttEvent(esp_mqtt_event_handle_t event);
  void processRequest(PendingRequest& request);
  void processLogin(PendingRequest& request);
  void processVerification(PendingRequest& request);
  void processSelection(PendingRequest& request);
  void processRefresh();
  void processLogout();

  bool postLogin(const PendingRequest& request, bool verification,
                 char* error, size_t errorSize);
  bool postTotp(const char* code, char* error, size_t errorSize);
  bool sendVerificationCode(bool sms, char* error, size_t errorSize);
  bool discoverAccountAndPrinters(char* error, size_t errorSize);
  bool startMqtt(const BambuCloudCredential& credential);
  void stopMqtt();
  void finalizeCandidateIfValidated();
  void fail(const char* reason);
  void setPhase(BambuAuthPhase phase, const char* error = nullptr);
  bool operationMatches(const char* operationId) const;
  bool operationExpired() const;
  void clearPendingAuth();
  void clearPendingRequest();
  void makeOperationId(char output[17]);
  void maybeScheduleAutomaticRefresh();
  void noteRefreshFailure();
  void noteRefreshSuccess(bool refreshed);

  bool loadCredentials();
  bool persistCandidate();
  bool persistPromotion();
  bool persistRefreshRecovery(const char* accountUid,
                              const char* refreshToken);
  void clearRefreshRecovery();
  void removeCandidateFromNvs();
  void eraseCredentialNvs();

  SecureNetworkCoordinator& coordinator_;
  DirectCloudSource source_;
  mutable SemaphoreHandle_t mutex_;
  SemaphoreHandle_t operationMutex_;
  BambuCredentialSlots credentialSlots_;
  BambuRefreshRecovery refreshRecovery_;
  BambuCredentialTransaction credentialTx_;
  PendingRequest request_;
  PendingAuth pendingAuth_;
  BambuCloudPublicStatus status_;
  uint32_t operationDeadlineMs_;
  uint32_t nextRefreshAttemptMs_;
  uint32_t lastSuccessfulRefreshMs_;
  uint8_t refreshFailureCount_;
  bool refreshRetryScheduled_;
  bool refreshCandidatePending_;
  bool activeCredentialBlocked_;
  esp_mqtt_client_handle_t mqttClient_;
  bool mqttLeaseHeld_;
  std::atomic<bool> mqttUsingCandidate_{false};
  std::atomic<bool> candidateTransportFailed_{false};
  std::atomic<bool> activeCredentialRejected_{false};
  std::atomic<bool> activeTransportFailed_{false};
  std::atomic<uint8_t> activeTransportFailureReason_{RECOVERY_MQTT};
  std::atomic<bool> mqttStopping_{false};
  std::atomic<bool> externalTlsPaused_{false};
  bool initialized_;
  BoundedRecoveryBackoff mqttRecovery_;
  bool wifiWasAvailable_;
  std::atomic<uint32_t> mqttTaskMinimumReserve_{UINT32_MAX};
};
