#include <assert.h>
#include <stdint.h>

#include "../src/wifi_reconnect_policy.hpp"

int main() {
  // Retry quickly after a router disappears, then back off to a bounded
  // one-minute cadence while it remains unavailable.
  assert(wifiReconnectDelayMs(0) == 5000UL);
  assert(wifiReconnectDelayMs(1) == 10000UL);
  assert(wifiReconnectDelayMs(2) == 20000UL);
  assert(wifiReconnectDelayMs(3) == 40000UL);
  assert(wifiReconnectDelayMs(4) == 60000UL);
  assert(wifiReconnectDelayMs(20) == 60000UL);

  WifiReconnectPolicy policy;
  const uint32_t lostAt = 100000UL;
  policy.scheduleInitial(lostAt);
  assert(!policy.isDue(lostAt + 4999UL));
  assert(policy.takeDue(lostAt + 5000UL));
  assert(!policy.takeDue(lostAt + 5000UL));

  const uint32_t firstFailureAt = lostAt + 21000UL;
  policy.scheduleAfterFailure(firstFailureAt);
  assert(!policy.isDue(firstFailureAt + 9999UL));
  assert(policy.takeDue(firstFailureAt + 10000UL));

  uint32_t failedAt = firstFailureAt + 25000UL;
  const uint32_t remainingDelays[] = {20000UL, 40000UL, 60000UL, 60000UL};
  for (uint32_t delay : remainingDelays) {
    policy.scheduleAfterFailure(failedAt);
    assert(!policy.isDue(failedAt + delay - 1UL));
    assert(policy.takeDue(failedAt + delay));
    failedAt += delay + 15000UL;
  }

  // uint32_t subtraction keeps scheduling correct across millis() rollover.
  policy.reset();
  policy.scheduleInitial(UINT32_MAX - 2000UL);
  assert(!policy.isDue(1999UL));
  assert(policy.isDue(2999UL));

  policy.reset();
  assert(!policy.isScheduled());
  return 0;
}
