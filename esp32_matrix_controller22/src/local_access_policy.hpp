#pragma once

#include <stdint.h>

// Reliability-first local access: the LAN/AP caller needs no user password.
// OTA accepts unsigned images (owner decision 2026-09-03): image integrity is
// carried by the ESP32 app-image validation plus the boot-guard rollback.
static constexpr uint8_t LOCAL_SECURITY_LEVEL = 0;
static constexpr bool OTA_PASSWORD_REQUIRED = false;
static constexpr bool OTA_SIGNATURE_REQUIRED = false;

inline bool authorizeLocalOtaUpload(bool signatureHeaderValid) {
  return !OTA_PASSWORD_REQUIRED && (!OTA_SIGNATURE_REQUIRED || signatureHeaderValid);
}
