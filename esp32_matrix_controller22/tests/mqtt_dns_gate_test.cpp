#include <assert.h>

#include "../src/mqtt_dns_gate.hpp"
#include "../src/t05_resilience.hpp"

// Regression contract for the early-boot DNS race (2026-09-03): the direct
// Cloud MQTT connect must never be entered while the broker hostname cannot
// be resolved — that path panics in the esp-tls error teardown on core 3.3.8.
// The wiring lives in BambuCloud::service(); the on-device red/green loop is
// tools/dnsrace_loop.py. This host test pins the decision contract so the
// gate cannot be "simplified" away unnoticed.

static void test_wifi_alone_never_permits_connect() {
  // The exact bug pattern: Wi-Fi reports connected (got IP) but the resolver
  // has no answer for the broker host yet.
  assert(mqttConnectGate(true, false) == MqttConnectGate::kAwaitDns);
  assert(mqttConnectGate(false, false) == MqttConnectGate::kAwaitDns);
  assert(mqttConnectGate(false, true) == MqttConnectGate::kAwaitDns);
}

static void test_resolved_broker_permits_connect() {
  assert(mqttConnectGate(true, true) == MqttConnectGate::kConnect);
}

static void test_dns_failure_defers_next_attempt_via_backoff() {
  BoundedRecoveryBackoff recovery;
  uint32_t now = 10'000;
  recovery.fail(RECOVERY_DNS, now);
  // Not ready inside the 5 s first backoff step...
  assert(!recovery.ready(now + 4'999));
  // ...ready after it: the deferred attempt re-probes DNS instead of
  // entering esp-mqtt with a dead resolver.
  assert(recovery.ready(now + 5'000));
  assert(recovery.reason() == RECOVERY_DNS);
}

static void test_repeated_dns_failures_stay_bounded() {
  BoundedRecoveryBackoff recovery;
  uint32_t now = 0;
  for (int i = 0; i < 12; ++i) recovery.fail(RECOVERY_DNS, now = i * 60'000);
  assert(BoundedRecoveryBackoff::delayMs(recovery.failures()) == 60'000);
  assert(recovery.ready(now + 60'000));
}

static void test_wifi_up_transition_rearms_dns_probe_immediately() {
  BoundedRecoveryBackoff recovery;
  uint32_t now = 100'000;
  recovery.fail(RECOVERY_DNS, now);
  assert(!recovery.ready(now + 1'000));
  recovery.reset();  // wifiAvailable false->true edge resets the backoff
  assert(recovery.ready(now + 1'000));
}

int main() {
  test_wifi_alone_never_permits_connect();
  test_resolved_broker_permits_connect();
  test_dns_failure_defers_next_attempt_via_backoff();
  test_repeated_dns_failures_stay_bounded();
  test_wifi_up_transition_rearms_dns_probe_immediately();
  return 0;
}
