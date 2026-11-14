#include <assert.h>
#include <string.h>

#include "../src/secure_network_coordinator.hpp"
#include "../src/local_access_policy.hpp"
#include "../src/t05_resilience.hpp"

static PrinterStateSnapshot representativeSnapshot() {
  PrinterStateSnapshot value;
  value.enabled = true;
  value.configured = true;
  value.transportConnected = true;
  value.printerOnline = true;
  value.lifecycle = PRINTER_PRINTING;
  value.visibleLifecycle = PRINTER_PRINTING;
  value.progress = 37;
  value.currentLayer = 12;
  value.totalLayers = 80;
  value.remainingSeconds = 3600;
  strcpy(value.jobName, "representative-job");
  strcpy(value.stage, "printing");
  strcpy(value.detail, "printing");
  return value;
}

static void test_local_ota_policy_is_open_and_unsigned() {
  static_assert(LOCAL_SECURITY_LEVEL == 0, "local WebUI/API must stay open");
  static_assert(!OTA_PASSWORD_REQUIRED, "OTA must not require a user password");
  static_assert(!OTA_SIGNATURE_REQUIRED, "owner decision: OTA accepts unsigned images");
  assert(authorizeLocalOtaUpload(false));
  assert(authorizeLocalOtaUpload(true));
}

static void test_ota_blocks_new_tls_owners() {
  SecureNetworkCoordinator coordinator;
  assert(coordinator.acquire(SECURE_NETWORK_CLOUD_MQTT));
  coordinator.requestOta();
  assert(!coordinator.acquire(SECURE_NETWORK_WEATHER));
  coordinator.release(SECURE_NETWORK_CLOUD_MQTT);
  assert(coordinator.acquireOta());
  assert(!coordinator.acquire(SECURE_NETWORK_CLOUD_HTTP));
  coordinator.release(SECURE_NETWORK_OTA);
  assert(coordinator.acquire(SECURE_NETWORK_SUNRISE));
}

static void test_recovery_is_bounded_and_wrap_safe() {
  BoundedRecoveryBackoff recovery;
  uint32_t now = 0xfffffff0UL;
  recovery.fail(RECOVERY_MQTT, now);
  assert(!recovery.ready(now));
  assert(recovery.ready(now + 5000));
  for (int i = 0; i < 10; ++i) recovery.fail(RECOVERY_DNS, now);
  assert(BoundedRecoveryBackoff::delayMs(recovery.failures()) == 60000);
  assert(recovery.reason() == RECOVERY_DNS);
  recovery.reset();
  assert(recovery.ready(now));
}

static void test_shadow_compares_every_normalized_field_without_storing_values() {
  const PrinterStateSnapshot baseline = representativeSnapshot();
  PrinterShadowDiagnostics diagnostics;
  diagnostics.observe(baseline, baseline);
  assert(diagnostics.matches == 1 && diagnostics.lastReasonMask == 0);

  const uint16_t expected[] = {
      SHADOW_ENABLED, SHADOW_CONFIGURED, SHADOW_TRANSPORT,
      SHADOW_PRINTER_ONLINE, SHADOW_LIFECYCLE, SHADOW_VISIBLE_LIFECYCLE,
      SHADOW_PROGRESS, SHADOW_CURRENT_LAYER, SHADOW_TOTAL_LAYERS,
      SHADOW_REMAINING_SECONDS, SHADOW_JOB_NAME, SHADOW_STAGE, SHADOW_DETAIL};
  static_assert(sizeof(expected) / sizeof(expected[0]) == SHADOW_REASON_COUNT,
                "every shadow reason needs a field mutation");
  for (uint8_t i = 0; i < SHADOW_REASON_COUNT; ++i) {
    PrinterStateSnapshot changed = baseline;
    switch (i) {
      case 0: changed.enabled = false; break;
      case 1: changed.configured = false; break;
      case 2: changed.transportConnected = false; break;
      case 3: changed.printerOnline = false; break;
      case 4: changed.lifecycle = PRINTER_PAUSED; break;
      case 5: changed.visibleLifecycle = PRINTER_PAUSED; break;
      case 6: changed.progress++; break;
      case 7: changed.currentLayer++; break;
      case 8: changed.totalLayers++; break;
      case 9: changed.remainingSeconds++; break;
      case 10: strcpy(changed.jobName, "other"); break;
      case 11: strcpy(changed.stage, "other"); break;
      case 12: strcpy(changed.detail, "other"); break;
    }
    assert(printerShadowMismatchMask(baseline, changed) == expected[i]);
    diagnostics.observe(baseline, changed);
  }
  assert(diagnostics.comparisons == 14);
  assert(diagnostics.mismatches == SHADOW_REASON_COUNT);
  for (uint8_t i = 0; i < SHADOW_REASON_COUNT; ++i)
    assert(diagnostics.reasonCounts[i] == 1);
}

static void test_source_mode_contract() {
  PrinterSourceMode mode = PRINTER_SOURCE_CLOUD_DIRECT;
  assert(parsePrinterSourceMode("relay", mode) && mode == PRINTER_SOURCE_RELAY);
  assert(parsePrinterSourceMode("cloud_direct", mode) &&
         mode == PRINTER_SOURCE_CLOUD_DIRECT);
  assert(!parsePrinterSourceMode("lan", mode));
  assert(effectivePrinterSourceMode(PRINTER_SOURCE_CLOUD_DIRECT, false) ==
         PRINTER_SOURCE_RELAY);
  assert(effectivePrinterSourceMode(PRINTER_SOURCE_CLOUD_DIRECT, true) ==
         PRINTER_SOURCE_CLOUD_DIRECT);
  bool ready = updateDirectSourceReadiness(false, true, true);
  assert(ready);
  ready = updateDirectSourceReadiness(ready, false, false);
  assert(!ready);
  assert(effectivePrinterSourceMode(PRINTER_SOURCE_CLOUD_DIRECT, ready) ==
         PRINTER_SOURCE_RELAY);
}

int main() {
  test_local_ota_policy_is_open_and_unsigned();
  test_ota_blocks_new_tls_owners();
  test_recovery_is_bounded_and_wrap_safe();
  test_shadow_compares_every_normalized_field_without_storing_values();
  test_source_mode_contract();
  return 0;
}
