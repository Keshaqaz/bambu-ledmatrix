#pragma once

#include <string.h>

struct LegacyBambuConfigV2 {
  bool enabled = false;
  char email[96] = "";
  char password[96] = "";
  bool relayEnabled = true;
  char relayUrl[160] = "";
  char relayKey[64] = "";
};

struct BambuRelayConfig {
  bool enabled = false;
  bool relayEnabled = true;
  char relayUrl[160] = "";
  char relayKey[64] = "";
};

inline BambuRelayConfig migrateLegacyBambuConfig(const LegacyBambuConfigV2& legacy) {
  BambuRelayConfig current;
  current.enabled = legacy.enabled;
  current.relayEnabled = true;
  strncpy(current.relayUrl, legacy.relayUrl, sizeof(current.relayUrl) - 1);
  strncpy(current.relayKey, legacy.relayKey, sizeof(current.relayKey) - 1);
  current.relayUrl[sizeof(current.relayUrl) - 1] = '\0';
  current.relayKey[sizeof(current.relayKey) - 1] = '\0';
  return current;
}
