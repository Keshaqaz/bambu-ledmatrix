#include <assert.h>
#include <stdint.h>

#include "../src/bambu_refresh_policy.hpp"

static constexpr uint32_t kNow = 1800000000UL;

static BambuRefreshPolicyInput credentialExpiringIn(uint32_t seconds) {
  BambuRefreshPolicyInput input = {};
  input.nowEpoch = kNow;
  input.uptimeMs = 10UL * 60UL * 1000UL;
  input.accessExpiresAt = kNow + seconds;
  input.refreshExpiresAt = kNow + seconds;
  return input;
}

static void test_waits_while_credential_is_fresh() {
  BambuRefreshPolicyInput input = credentialExpiringIn(30UL * 24UL * 60UL * 60UL);
  assert(bambuRefreshDecision(input) == BAMBU_REFRESH_WAIT);
}

static void test_refreshes_inside_warning_window_when_idle() {
  BambuRefreshPolicyInput input = credentialExpiringIn(6UL * 24UL * 60UL * 60UL);
  assert(bambuRefreshDecision(input) == BAMBU_REFRESH_PROACTIVE);
}

static void test_defers_nonurgent_refresh_during_print() {
  BambuRefreshPolicyInput input = credentialExpiringIn(6UL * 24UL * 60UL * 60UL);
  input.printerBusy = true;
  assert(bambuRefreshDecision(input) == BAMBU_REFRESH_WAIT);
}

static void test_refreshes_urgently_during_print() {
  BambuRefreshPolicyInput input = credentialExpiringIn(23UL * 60UL * 60UL);
  input.printerBusy = true;
  assert(bambuRefreshDecision(input) == BAMBU_REFRESH_URGENT);

  input.accessExpiresAt = kNow - 1;
  assert(bambuRefreshDecision(input) == BAMBU_REFRESH_URGENT);
}

static void test_uses_earliest_access_or_refresh_expiry() {
  BambuRefreshPolicyInput input = credentialExpiringIn(30UL * 24UL * 60UL * 60UL);
  input.refreshExpiresAt = kNow + 2UL * 24UL * 60UL * 60UL;
  assert(bambuRefreshDecision(input) == BAMBU_REFRESH_PROACTIVE);
}

static void test_bootstraps_unknown_expiry_once_time_or_uptime_is_ready() {
  BambuRefreshPolicyInput input = {};
  input.uptimeMs = BAMBU_REFRESH_UNKNOWN_STARTUP_GRACE_MS - 1;
  assert(bambuRefreshDecision(input) == BAMBU_REFRESH_WAIT);

  input.uptimeMs = BAMBU_REFRESH_UNKNOWN_STARTUP_GRACE_MS;
  assert(bambuRefreshDecision(input) == BAMBU_REFRESH_UNKNOWN_EXPIRY);

  input.lastSuccessfulRefreshMs = input.uptimeMs;
  input.uptimeMs += BAMBU_REFRESH_UNKNOWN_INTERVAL_MS - 1;
  assert(bambuRefreshDecision(input) == BAMBU_REFRESH_WAIT);
  input.uptimeMs++;
  assert(bambuRefreshDecision(input) == BAMBU_REFRESH_UNKNOWN_EXPIRY);

  input = credentialExpiringIn(30UL * 24UL * 60UL * 60UL);
  input.accessExpiresAt = 0;
  assert(bambuRefreshDecision(input) == BAMBU_REFRESH_UNKNOWN_EXPIRY);
}

static void test_retry_deadline_blocks_refresh_until_due_across_wrap() {
  BambuRefreshPolicyInput input = credentialExpiringIn(30UL * 24UL * 60UL * 60UL);
  input.retryScheduled = true;
  input.uptimeMs = 1000;
  input.nextAttemptMs = 2000;
  assert(bambuRefreshDecision(input) == BAMBU_REFRESH_WAIT);
  input.uptimeMs = 2000;
  assert(bambuRefreshDecision(input) == BAMBU_REFRESH_URGENT);

  input.uptimeMs = 0xfffffff0UL;
  input.nextAttemptMs = 20;
  assert(bambuRefreshDecision(input) == BAMBU_REFRESH_WAIT);
  input.uptimeMs = 20;
  assert(bambuRefreshDecision(input) == BAMBU_REFRESH_URGENT);
}

static void test_retry_backoff_is_bounded() {
  assert(bambuRefreshRetryDelayMs(1) == 60UL * 1000UL);
  assert(bambuRefreshRetryDelayMs(2) == 5UL * 60UL * 1000UL);
  assert(bambuRefreshRetryDelayMs(3) == 15UL * 60UL * 1000UL);
  assert(bambuRefreshRetryDelayMs(4) == 60UL * 60UL * 1000UL);
  assert(bambuRefreshRetryDelayMs(5) == 6UL * 60UL * 60UL * 1000UL);
  assert(bambuRefreshRetryDelayMs(255) == 6UL * 60UL * 60UL * 1000UL);
}

int main() {
  test_waits_while_credential_is_fresh();
  test_refreshes_inside_warning_window_when_idle();
  test_defers_nonurgent_refresh_during_print();
  test_refreshes_urgently_during_print();
  test_uses_earliest_access_or_refresh_expiry();
  test_bootstraps_unknown_expiry_once_time_or_uptime_is_ready();
  test_retry_deadline_blocks_refresh_until_due_across_wrap();
  test_retry_backoff_is_bounded();
  return 0;
}
