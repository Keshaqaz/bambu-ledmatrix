#pragma once

#include <stdint.h>

static constexpr uint32_t BAMBU_REFRESH_VALID_EPOCH = 1609459200UL;
static constexpr uint32_t BAMBU_REFRESH_WARNING_SECONDS =
    7UL * 24UL * 60UL * 60UL;
static constexpr uint32_t BAMBU_REFRESH_URGENT_SECONDS =
    24UL * 60UL * 60UL;
static constexpr uint32_t BAMBU_REFRESH_UNKNOWN_STARTUP_GRACE_MS =
    5UL * 60UL * 1000UL;
static constexpr uint32_t BAMBU_REFRESH_UNKNOWN_INTERVAL_MS =
    30UL * 24UL * 60UL * 60UL * 1000UL;

enum BambuRefreshDecision : uint8_t {
  BAMBU_REFRESH_WAIT = 0,
  BAMBU_REFRESH_PROACTIVE,
  BAMBU_REFRESH_URGENT,
  BAMBU_REFRESH_UNKNOWN_EXPIRY,
};

struct BambuRefreshPolicyInput {
  uint32_t nowEpoch = 0;
  uint32_t uptimeMs = 0;
  uint32_t accessExpiresAt = 0;
  uint32_t refreshExpiresAt = 0;
  uint32_t lastSuccessfulRefreshMs = 0;
  uint32_t nextAttemptMs = 0;
  bool retryScheduled = false;
  bool printerBusy = false;
};

inline bool bambuRefreshDeadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
  return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

inline uint32_t bambuRefreshRetryDelayMs(uint8_t failureCount) {
  switch (failureCount) {
    case 0:
    case 1: return 60UL * 1000UL;
    case 2: return 5UL * 60UL * 1000UL;
    case 3: return 15UL * 60UL * 1000UL;
    case 4: return 60UL * 60UL * 1000UL;
    default: return 6UL * 60UL * 60UL * 1000UL;
  }
}

inline BambuRefreshDecision bambuRefreshDecision(
    const BambuRefreshPolicyInput& input) {
  if (input.retryScheduled) {
    return bambuRefreshDeadlineReached(input.uptimeMs, input.nextAttemptMs)
               ? BAMBU_REFRESH_URGENT
               : BAMBU_REFRESH_WAIT;
  }

  const bool expiryUnknown = input.accessExpiresAt == 0 ||
                             input.refreshExpiresAt == 0;
  uint32_t expiry = input.accessExpiresAt;
  if (input.refreshExpiresAt &&
      (!expiry || input.refreshExpiresAt < expiry)) {
    expiry = input.refreshExpiresAt;
  }
  if (input.nowEpoch < BAMBU_REFRESH_VALID_EPOCH || expiryUnknown) {
    if (input.lastSuccessfulRefreshMs == 0) {
      return input.uptimeMs >= BAMBU_REFRESH_UNKNOWN_STARTUP_GRACE_MS
                 ? BAMBU_REFRESH_UNKNOWN_EXPIRY
                 : BAMBU_REFRESH_WAIT;
    }
    return static_cast<uint32_t>(input.uptimeMs -
                                 input.lastSuccessfulRefreshMs) >=
                   BAMBU_REFRESH_UNKNOWN_INTERVAL_MS
               ? BAMBU_REFRESH_UNKNOWN_EXPIRY
               : BAMBU_REFRESH_WAIT;
  }

  if (expiry <= input.nowEpoch ||
      expiry - input.nowEpoch <= BAMBU_REFRESH_URGENT_SECONDS) {
    return BAMBU_REFRESH_URGENT;
  }
  if (expiry - input.nowEpoch <= BAMBU_REFRESH_WARNING_SECONDS &&
      !input.printerBusy) {
    return BAMBU_REFRESH_PROACTIVE;
  }
  return BAMBU_REFRESH_WAIT;
}
