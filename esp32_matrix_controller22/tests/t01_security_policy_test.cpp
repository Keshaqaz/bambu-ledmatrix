#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/bambu_legacy_migration.hpp"
#include "../src/security_policy.hpp"

int main() {
  const uint8_t tokenA[] = {1, 2, 3, 4};
  const uint8_t tokenB[] = {1, 2, 3, 4};
  const uint8_t tokenC[] = {1, 2, 3, 5};
  assert(secureBytesEqual(tokenA, tokenB, sizeof(tokenA)));
  assert(!secureBytesEqual(tokenA, tokenC, sizeof(tokenA)));

  char session[17] = {};
  assert(extractCookieValue("theme=dark; matrix_session=0123456789abcdef; x=1",
                            "matrix_session", session, sizeof(session)));
  assert(strcmp(session, "0123456789abcdef") == 0);
  assert(!extractCookieValue("matrix_session=too-long-for-buffer", "matrix_session",
                             session, sizeof(session)));
  assert(!extractCookieValue("not_matrix_session=value", "matrix_session",
                             session, sizeof(session)));

  assert(isValidAdminPassword("correct horse battery staple"));
  assert(!isValidAdminPassword("short"));
  assert(!isValidAdminPassword("          "));
  assert(strcmp(redactSecretForApi("cloud-secret"), "***") == 0);
  assert(strcmp(redactSecretForApi(""), "") == 0);
  assert(isRedactedSecretValue("***"));
  assert(!isRedactedSecretValue("cloud-secret"));

  LegacyBambuConfigV2 legacy = {};
  legacy.enabled = true;
  strcpy(legacy.email, "must-not-survive@example.invalid");
  strcpy(legacy.password, "must-not-survive");
  legacy.relayEnabled = false;
  strcpy(legacy.relayUrl, "http://192.168.1.1:8787/status");
  strcpy(legacy.relayKey, "relay-key");

  BambuRelayConfig migrated = migrateLegacyBambuConfig(legacy);
  assert(migrated.enabled);
  assert(migrated.relayEnabled);
  assert(strcmp(migrated.relayUrl, legacy.relayUrl) == 0);
  assert(strcmp(migrated.relayKey, legacy.relayKey) == 0);
  return 0;
}
