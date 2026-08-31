#pragma once

#include <atomic>
#include <stdint.h>
#include <string.h>

#include "printer_status_source.hpp"

enum PrinterSourceMode : uint8_t {
  PRINTER_SOURCE_RELAY = 0,
  PRINTER_SOURCE_CLOUD_DIRECT = 1,
};

inline const char* printerSourceModeName(PrinterSourceMode mode) {
  return mode == PRINTER_SOURCE_CLOUD_DIRECT ? "cloud_direct" : "relay";
}

inline bool parsePrinterSourceMode(const char* value, PrinterSourceMode& mode) {
  if (value && strcmp(value, "relay") == 0) {
    mode = PRINTER_SOURCE_RELAY;
    return true;
  }
  if (value && strcmp(value, "cloud_direct") == 0) {
    mode = PRINTER_SOURCE_CLOUD_DIRECT;
    return true;
  }
  return false;
}

inline PrinterSourceMode effectivePrinterSourceMode(PrinterSourceMode configured,
                                                     bool directReady) {
  return configured == PRINTER_SOURCE_CLOUD_DIRECT && directReady
             ? PRINTER_SOURCE_CLOUD_DIRECT
             : PRINTER_SOURCE_RELAY;
}

inline bool updateDirectSourceReadiness(bool latched, bool configured,
                                        bool firstValidReport) {
  return configured && (latched || firstValidReport);
}

enum ShadowMismatchReason : uint16_t {
  SHADOW_ENABLED             = 1u << 0,
  SHADOW_CONFIGURED          = 1u << 1,
  SHADOW_TRANSPORT           = 1u << 2,
  SHADOW_PRINTER_ONLINE      = 1u << 3,
  SHADOW_LIFECYCLE           = 1u << 4,
  SHADOW_VISIBLE_LIFECYCLE   = 1u << 5,
  SHADOW_PROGRESS            = 1u << 6,
  SHADOW_CURRENT_LAYER       = 1u << 7,
  SHADOW_TOTAL_LAYERS        = 1u << 8,
  SHADOW_REMAINING_SECONDS   = 1u << 9,
  SHADOW_JOB_NAME            = 1u << 10,
  SHADOW_STAGE               = 1u << 11,
  SHADOW_DETAIL              = 1u << 12,
  SHADOW_REASON_COUNT        = 13,
};

inline uint16_t printerShadowMismatchMask(const PrinterStateSnapshot& relay,
                                          const PrinterStateSnapshot& direct) {
  uint16_t mask = 0;
  if (relay.enabled != direct.enabled) mask |= SHADOW_ENABLED;
  if (relay.configured != direct.configured) mask |= SHADOW_CONFIGURED;
  if (relay.transportConnected != direct.transportConnected) mask |= SHADOW_TRANSPORT;
  if (relay.printerOnline != direct.printerOnline) mask |= SHADOW_PRINTER_ONLINE;
  if (relay.lifecycle != direct.lifecycle) mask |= SHADOW_LIFECYCLE;
  if (relay.visibleLifecycle != direct.visibleLifecycle) mask |= SHADOW_VISIBLE_LIFECYCLE;
  if (relay.progress != direct.progress) mask |= SHADOW_PROGRESS;
  if (relay.currentLayer != direct.currentLayer) mask |= SHADOW_CURRENT_LAYER;
  if (relay.totalLayers != direct.totalLayers) mask |= SHADOW_TOTAL_LAYERS;
  if (relay.remainingSeconds != direct.remainingSeconds) mask |= SHADOW_REMAINING_SECONDS;
  if (strcmp(relay.jobName, direct.jobName) != 0) mask |= SHADOW_JOB_NAME;
  if (strcmp(relay.stage, direct.stage) != 0) mask |= SHADOW_STAGE;
  if (strcmp(relay.detail, direct.detail) != 0) mask |= SHADOW_DETAIL;
  return mask;
}

struct PrinterShadowDiagnostics {
  uint32_t comparisons = 0;
  uint32_t matches = 0;
  uint32_t mismatches = 0;
  uint16_t lastReasonMask = 0;
  uint32_t reasonCounts[SHADOW_REASON_COUNT] = {};

  void observe(const PrinterStateSnapshot& relay,
               const PrinterStateSnapshot& direct) {
    const uint16_t mask = printerShadowMismatchMask(relay, direct);
    ++comparisons;
    lastReasonMask = mask;
    if (!mask) {
      ++matches;
      return;
    }
    ++mismatches;
    for (uint8_t bit = 0; bit < SHADOW_REASON_COUNT; ++bit) {
      if (mask & (1u << bit)) ++reasonCounts[bit];
    }
  }
};

enum RecoveryReason : uint8_t {
  RECOVERY_NONE = 0,
  RECOVERY_WIFI = 1,
  RECOVERY_DNS = 2,
  RECOVERY_MQTT = 3,
  RECOVERY_API = 4,
  RECOVERY_TLS_BUSY = 5,
};

class BoundedRecoveryBackoff {
 public:
  static uint32_t delayMs(uint8_t failureCount) {
    static const uint32_t kDelays[] = {5000, 10000, 20000, 40000, 60000};
    if (!failureCount) return 0;
    const uint8_t index = failureCount > 5 ? 4 : failureCount - 1;
    return kDelays[index];
  }

  void fail(RecoveryReason reason, uint32_t nowMs) {
    uint8_t previous = failures_.load();
    while (previous < 255 &&
           !failures_.compare_exchange_weak(previous, previous + 1)) {}
    reason_.store(reason);
    deadlineMs_.store(nowMs + delayMs(failures_.load()));
  }

  void reset() {
    failures_.store(0);
    deadlineMs_.store(0);
  }

  bool ready(uint32_t nowMs) const {
    const uint32_t deadline = deadlineMs_.load();
    return !deadline || static_cast<int32_t>(nowMs - deadline) >= 0;
  }

  uint8_t failures() const { return failures_.load(); }
  RecoveryReason reason() const {
    return static_cast<RecoveryReason>(reason_.load());
  }
  uint32_t deadlineMs() const { return deadlineMs_.load(); }

 private:
  std::atomic<uint8_t> failures_{0};
  std::atomic<uint8_t> reason_{RECOVERY_NONE};
  std::atomic<uint32_t> deadlineMs_{0};
};
