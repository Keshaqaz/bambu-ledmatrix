#include <assert.h>

#include "../src/mqtt_pause_policy.hpp"

// Regression contract for the warm-boot MQTT pause race (2026-09-03):
// pausing (stop+destroy) a freshly connected direct Cloud MQTT client
// faults inside the esp-mqtt task's transport_read unwind on core 3.3.8.
// The on-device red/green loop is tools/dnsrace_loop.py; this pins the
// defer decision so the guard cannot be "simplified" away.

static void test_no_client_never_defers() {
  assert(!shouldDeferExternalTlsPause(false, false, false, 0));
  assert(!shouldDeferExternalTlsPause(false, true, false, 0));
}

static void test_active_print_still_defers() {
  assert(shouldDeferExternalTlsPause(true, true, true, 600'000));
}

static void test_connecting_or_unvalidated_client_never_paused() {
  // The exact warm-boot bug shape: client exists, connection established
  // seconds ago, first report has not arrived yet (or never will this
  // session) — a pause here used to crash the firmware.
  assert(shouldDeferExternalTlsPause(true, false, false, 2'000));
  assert(shouldDeferExternalTlsPause(true, false, false, 30'000));
}

static void test_young_validated_connection_deferred() {
  assert(shouldDeferExternalTlsPause(true, false, true, 10'000));
  assert(shouldDeferExternalTlsPause(true, false, true, kMqttPauseGuardMs - 1));
}

static void test_steady_state_pause_still_allowed() {
  // Production behavior preserved: validated, settled, no active print.
  assert(!shouldDeferExternalTlsPause(true, false, true, kMqttPauseGuardMs));
  assert(!shouldDeferExternalTlsPause(true, false, true, 600'000));
}

int main() {
  test_no_client_never_defers();
  test_active_print_still_defers();
  test_connecting_or_unvalidated_client_never_paused();
  test_young_validated_connection_deferred();
  test_steady_state_pause_still_allowed();
  return 0;
}
