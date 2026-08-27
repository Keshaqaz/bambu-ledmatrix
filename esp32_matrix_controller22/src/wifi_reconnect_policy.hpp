#pragma once

#include <stdint.h>

// Kept separate from the ESP32 WiFi calls so the reconnect cadence can be
// verified on the host without hardware.
static const uint8_t WIFI_RECONNECT_MAX_DELAY_INDEX = 4;

inline uint32_t wifiReconnectDelayMs(uint8_t failureCount) {
  static const uint32_t delays[] = {
    5000UL, 10000UL, 20000UL, 40000UL, 60000UL
  };
  const uint8_t last = (uint8_t)(sizeof(delays) / sizeof(delays[0]) - 1);
  return delays[failureCount < last ? failureCount : last];
}

class WifiReconnectPolicy {
 public:
  WifiReconnectPolicy()
      : scheduledAtMs_(0), nextDelayIndex_(0), scheduled_(false) {}

  // A newly detected outage always gets a quick first retry.
  void scheduleInitial(uint32_t nowMs) {
    nextDelayIndex_ = 0;
    scheduledAtMs_ = nowMs;
    scheduled_ = true;
  }

  // Called after an in-flight attempt has failed. takeDue() already advanced
  // the index, so this schedules the next, longer delay from failure time.
  void scheduleAfterFailure(uint32_t nowMs) {
    scheduledAtMs_ = nowMs;
    scheduled_ = true;
  }

  bool isDue(uint32_t nowMs) const {
    return scheduled_ &&
           (uint32_t)(nowMs - scheduledAtMs_) >=
               wifiReconnectDelayMs(nextDelayIndex_);
  }

  // Returns true once per scheduled attempt and advances the bounded backoff.
  bool takeDue(uint32_t nowMs) {
    if (!isDue(nowMs)) return false;
    scheduled_ = false;
    if (nextDelayIndex_ < WIFI_RECONNECT_MAX_DELAY_INDEX) ++nextDelayIndex_;
    return true;
  }

  void reset() {
    scheduledAtMs_ = 0;
    nextDelayIndex_ = 0;
    scheduled_ = false;
  }

  bool isScheduled() const { return scheduled_; }
  uint32_t nextDelayMs() const {
    return wifiReconnectDelayMs(nextDelayIndex_);
  }

 private:
  uint32_t scheduledAtMs_;
  uint8_t nextDelayIndex_;
  bool scheduled_;
};
