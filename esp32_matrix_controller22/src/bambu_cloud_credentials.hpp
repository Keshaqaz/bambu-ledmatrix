#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static constexpr uint32_t BAMBU_CREDENTIAL_MAGIC = 0x42434C44UL;  // BCLD
static constexpr uint16_t BAMBU_CREDENTIAL_VERSION = 1;
static constexpr uint32_t BAMBU_REFRESH_RECOVERY_MAGIC = 0x42525246UL;  // BRRF
static constexpr uint16_t BAMBU_REFRESH_RECOVERY_VERSION = 1;
static constexpr size_t BAMBU_CLOUD_UID_CAPACITY = 40;
static constexpr size_t BAMBU_CLOUD_SERIAL_CAPACITY = 40;
static constexpr size_t BAMBU_CLOUD_TOKEN_CAPACITY = 512;

inline void bambuSecureZero(void* memory, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(memory);
  while (length--) *bytes++ = 0;
}

inline uint32_t bambuCredentialChecksumBytes(const uint8_t* bytes, size_t length) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

struct BambuCloudCredential {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t generation;
  char accountUid[BAMBU_CLOUD_UID_CAPACITY];
  char serial[BAMBU_CLOUD_SERIAL_CAPACITY];
  char accessToken[BAMBU_CLOUD_TOKEN_CAPACITY];
  char refreshToken[BAMBU_CLOUD_TOKEN_CAPACITY];
  uint32_t accessExpiresAt;
  uint32_t refreshExpiresAt;
  bool chinaRegion;
  uint32_t checksum;

  BambuCloudCredential() { bambuSecureZero(this, sizeof(*this)); }
};

// The Cloud may invalidate the posted refresh token as soon as it returns a
// rotated one. Keep that newest token independently from the active/candidate
// credential transaction so a reboot or failed MQTT validation can retry
// without falling back to an already invalidated token.
struct BambuRefreshRecovery {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  char accountUid[BAMBU_CLOUD_UID_CAPACITY];
  char refreshToken[BAMBU_CLOUD_TOKEN_CAPACITY];
  uint32_t checksum;

  BambuRefreshRecovery() { bambuSecureZero(this, sizeof(*this)); }
};

inline uint32_t bambuRefreshRecoveryChecksum(
    const BambuRefreshRecovery& value) {
  return bambuCredentialChecksumBytes(
      reinterpret_cast<const uint8_t*>(&value),
      offsetof(BambuRefreshRecovery, checksum));
}

inline bool bambuRefreshRecoverySet(BambuRefreshRecovery& value,
                                    const char* accountUid,
                                    const char* refreshToken) {
  BambuRefreshRecovery candidate;
  if (!accountUid || !refreshToken) return false;
  const size_t uidLength = strlen(accountUid);
  const size_t tokenLength = strlen(refreshToken);
  if (uidLength == 0 || uidLength >= sizeof(candidate.accountUid) ||
      tokenLength == 0 || tokenLength >= sizeof(candidate.refreshToken)) {
    bambuSecureZero(&candidate, sizeof(candidate));
    return false;
  }
  memcpy(candidate.accountUid, accountUid, uidLength + 1);
  memcpy(candidate.refreshToken, refreshToken, tokenLength + 1);
  candidate.magic = BAMBU_REFRESH_RECOVERY_MAGIC;
  candidate.version = BAMBU_REFRESH_RECOVERY_VERSION;
  candidate.size = sizeof(BambuRefreshRecovery);
  candidate.checksum = bambuRefreshRecoveryChecksum(candidate);
  value = candidate;
  bambuSecureZero(&candidate, sizeof(candidate));
  return true;
}

inline bool bambuRefreshRecoveryValid(const BambuRefreshRecovery& value) {
  return value.magic == BAMBU_REFRESH_RECOVERY_MAGIC &&
         value.version == BAMBU_REFRESH_RECOVERY_VERSION &&
         value.size == sizeof(BambuRefreshRecovery) &&
         value.accountUid[0] != '\0' &&
         value.refreshToken[0] != '\0' &&
         value.checksum == bambuRefreshRecoveryChecksum(value);
}

inline bool bambuRefreshRecoveryMatches(const BambuRefreshRecovery& value,
                                        const char* accountUid) {
  return bambuRefreshRecoveryValid(value) && accountUid && accountUid[0] &&
         strcmp(value.accountUid, accountUid) == 0;
}

inline uint32_t bambuCredentialChecksum(const BambuCloudCredential& value) {
  return bambuCredentialChecksumBytes(
      reinterpret_cast<const uint8_t*>(&value),
      offsetof(BambuCloudCredential, checksum));
}

inline void bambuCredentialSeal(BambuCloudCredential& value) {
  value.magic = BAMBU_CREDENTIAL_MAGIC;
  value.version = BAMBU_CREDENTIAL_VERSION;
  value.size = sizeof(BambuCloudCredential);
  value.checksum = bambuCredentialChecksum(value);
}

inline bool bambuCredentialValid(const BambuCloudCredential& value) {
  return value.magic == BAMBU_CREDENTIAL_MAGIC &&
         value.version == BAMBU_CREDENTIAL_VERSION &&
         value.size == sizeof(BambuCloudCredential) &&
         value.generation != 0 && value.accountUid[0] != '\0' &&
         value.serial[0] != '\0' && value.accessToken[0] != '\0' &&
         value.refreshToken[0] != '\0' &&
         value.checksum == bambuCredentialChecksum(value);
}

inline bool bambuCredentialCopy(char* destination, size_t capacity,
                                const char* source) {
  if (!destination || capacity == 0 || !source) return false;
  const size_t length = strlen(source);
  if (length == 0 || length >= capacity) return false;
  memcpy(destination, source, length + 1);
  return true;
}

inline bool bambuCredentialSet(BambuCloudCredential& value, const char* uid,
                               const char* serial, const char* accessToken,
                               const char* refreshToken,
                               uint32_t accessExpiresAt,
                               uint32_t refreshExpiresAt,
                               bool chinaRegion = false) {
  BambuCloudCredential candidate;
  candidate.generation = 1;
  if (!bambuCredentialCopy(candidate.accountUid, sizeof(candidate.accountUid), uid) ||
      !bambuCredentialCopy(candidate.serial, sizeof(candidate.serial), serial) ||
      !bambuCredentialCopy(candidate.accessToken, sizeof(candidate.accessToken), accessToken) ||
      !bambuCredentialCopy(candidate.refreshToken, sizeof(candidate.refreshToken), refreshToken)) {
    bambuSecureZero(&candidate, sizeof(candidate));
    return false;
  }
  candidate.accessExpiresAt = accessExpiresAt;
  candidate.refreshExpiresAt = refreshExpiresAt;
  candidate.chinaRegion = chinaRegion;
  bambuCredentialSeal(candidate);
  value = candidate;
  bambuSecureZero(&candidate, sizeof(candidate));
  return true;
}

inline bool bambuBuildRefreshCandidate(const BambuCloudCredential& active,
                                       const char* newAccessToken,
                                       const char* newRefreshToken,
                                       uint32_t accessExpiresAt,
                                       uint32_t refreshExpiresAt,
                                       BambuCloudCredential& output) {
  if (!bambuCredentialValid(active) || !newAccessToken || !newAccessToken[0])
    return false;
  const char* refresh = newRefreshToken && newRefreshToken[0]
                            ? newRefreshToken
                            : active.refreshToken;
  return bambuCredentialSet(output, active.accountUid, active.serial,
                            newAccessToken, refresh, accessExpiresAt,
                            refreshExpiresAt, active.chinaRegion);
}

struct BambuCredentialSlots {
  BambuCloudCredential active[2];
  BambuCloudCredential candidate;
  bool candidatePresent;

  BambuCredentialSlots() : candidatePresent(false) {}
};

class BambuCredentialTransaction {
 public:
  explicit BambuCredentialTransaction(BambuCredentialSlots& slots)
      : slots_(slots) {}

  bool active(BambuCloudCredential& output) const {
    const bool valid0 = bambuCredentialValid(slots_.active[0]);
    const bool valid1 = bambuCredentialValid(slots_.active[1]);
    if (!valid0 && !valid1) return false;
    const size_t selected = !valid0 ? 1 : (!valid1 ? 0 :
        (slots_.active[1].generation > slots_.active[0].generation ? 1 : 0));
    output = slots_.active[selected];
    return true;
  }

  int activeSlot() const {
    const bool valid0 = bambuCredentialValid(slots_.active[0]);
    const bool valid1 = bambuCredentialValid(slots_.active[1]);
    if (!valid0 && !valid1) return -1;
    if (!valid0) return 1;
    if (!valid1) return 0;
    return slots_.active[1].generation > slots_.active[0].generation ? 1 : 0;
  }

  bool begin(const BambuCloudCredential& candidate) {
    if (!bambuCredentialValid(candidate)) return false;
    BambuCloudCredential current;
    uint32_t nextGeneration = active(current) ? current.generation + 1 : 1;
    if (nextGeneration == 0) nextGeneration = 1;
    slots_.candidate = candidate;
    slots_.candidate.generation = nextGeneration;
    bambuCredentialSeal(slots_.candidate);
    slots_.candidatePresent = true;
    bambuSecureZero(&current, sizeof(current));
    return true;
  }

  bool commit() {
    if (!hasCandidate()) return false;
    const int current = activeSlot();
    const int target = current == 0 ? 1 : 0;
    slots_.active[target] = slots_.candidate;
    bambuCredentialSeal(slots_.active[target]);
    rollback();
    return true;
  }

  void rollback() {
    bambuSecureZero(&slots_.candidate, sizeof(slots_.candidate));
    slots_.candidatePresent = false;
  }

  void recover() { rollback(); }

  bool hasCandidate() const {
    return slots_.candidatePresent && bambuCredentialValid(slots_.candidate);
  }

  const BambuCloudCredential* candidate() const {
    return hasCandidate() ? &slots_.candidate : nullptr;
  }

  void logout() { bambuSecureZero(&slots_, sizeof(slots_)); }

 private:
  BambuCredentialSlots& slots_;
};

inline bool bambuCredentialMemoryIsZero(const BambuCredentialSlots& slots) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&slots);
  for (size_t i = 0; i < sizeof(slots); ++i) {
    if (bytes[i] != 0) return false;
  }
  return true;
}
