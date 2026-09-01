#pragma once

#include <stdint.h>

// Reliability-first local access: the LAN/AP caller needs no user password.
// OTA authorization is carried by the firmware image's ECDSA signature.
static constexpr uint8_t LOCAL_SECURITY_LEVEL = 0;
static constexpr bool OTA_PASSWORD_REQUIRED = false;
static constexpr bool OTA_SIGNATURE_REQUIRED = true;

inline bool authorizeLocalOtaUpload(bool signatureHeaderValid) {
  return !OTA_PASSWORD_REQUIRED && OTA_SIGNATURE_REQUIRED && signatureHeaderValid;
}
