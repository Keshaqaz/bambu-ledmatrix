#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct BambuAuthEnvelopeInput {
  const char* token = nullptr;
  const char* accessToken = nullptr;
  const char* access_token = nullptr;
  const char* refreshToken = nullptr;
  const char* refresh_token = nullptr;
  uint32_t expiresIn = 0;
  uint32_t refreshExpiresIn = 0;
};

inline void bambuAuthSecureZero(void* memory, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(memory);
  while (length--) *bytes++ = 0;
}

inline const char* bambuFirstNonEmpty(const char* first, const char* second,
                                      const char* third = nullptr) {
  if (first && first[0]) return first;
  if (second && second[0]) return second;
  return third && third[0] ? third : "";
}

inline bool bambuAsciiContainsCaseInsensitive(const char* haystack,
                                              const char* needle) {
  if (!haystack || !needle || !needle[0]) return false;
  const size_t needleLength = strlen(needle);
  for (const char* start = haystack; *start; ++start) {
    size_t index = 0;
    while (index < needleLength && start[index]) {
      char left = start[index];
      char right = needle[index];
      if (left >= 'A' && left <= 'Z') left = left - 'A' + 'a';
      if (right >= 'A' && right <= 'Z') right = right - 'A' + 'a';
      if (left != right) break;
      ++index;
    }
    if (index == needleLength) return true;
  }
  return false;
}

// Convert a bounded server response into a stable, non-secret diagnostic code.
// Never expose the response body through WebUI/API or logs.
inline void bambuAuthHttpFailureCode(int statusCode, const char* boundedBody,
                                    char* output, size_t outputSize) {
  if (!output || outputSize == 0) return;
  if (statusCode == 403 &&
      bambuAsciiContainsCaseInsensitive(boundedBody, "missing_cookie")) {
    snprintf(output, outputSize, "csrf_missing_cookie");
  } else if (statusCode == 403 &&
             bambuAsciiContainsCaseInsensitive(boundedBody, "cloudflare")) {
    snprintf(output, outputSize, "cloudflare_blocked");
  } else {
    snprintf(output, outputSize, "http_%d", statusCode);
  }
}

inline bool bambuAuthApplicationJsonCoordinates(
    int statusCode, bool parsedObject, bool hasTypedCode,
    bool hasTypedError, bool hasAuthData) {
  if (!parsedObject) return false;
  if ((statusCode == 400 || statusCode == 401) &&
      hasTypedCode && hasTypedError) {
    return true;
  }
  return statusCode == 200 && hasAuthData;
}

// Keep only the RFC cookie-pair needed for the next portal request. Attributes
// such as Path, SameSite and HttpOnly must never be reflected into Cookie.
inline bool bambuExtractSessionCookie(const char* setCookie, char* output,
                                      size_t outputSize) {
  if (!output || outputSize == 0) return false;
  output[0] = '\0';
  if (!setCookie || !setCookie[0] || strchr(setCookie, '\r') ||
      strchr(setCookie, '\n')) {
    return false;
  }

  while (*setCookie == ' ' || *setCookie == '\t') ++setCookie;
  const char* end = strchr(setCookie, ';');
  if (!end) end = setCookie + strlen(setCookie);
  while (end > setCookie && (end[-1] == ' ' || end[-1] == '\t')) --end;
  const char* equals = static_cast<const char*>(
      memchr(setCookie, '=', static_cast<size_t>(end - setCookie)));
  const size_t length = static_cast<size_t>(end - setCookie);
  if (!equals || equals == setCookie || equals + 1 == end ||
      length >= outputSize) {
    return false;
  }
  memcpy(output, setCookie, length);
  output[length] = '\0';
  return true;
}

inline int bambuSessionCookiePriority(const char* cookiePair) {
  if (!cookiePair) return 0;
  const char* equals = strchr(cookiePair, '=');
  if (!equals || equals == cookiePair) return 0;
  char name[64] = {};
  const size_t length = static_cast<size_t>(equals - cookiePair);
  if (length >= sizeof(name)) return 0;
  for (size_t index = 0; index < length; ++index) {
    const char value = cookiePair[index];
    name[index] = value >= 'A' && value <= 'Z' ? value - 'A' + 'a' : value;
  }
  if (strstr(name, "csrf") || strstr(name, "callback")) return 0;
  if (strcmp(name, "token") == 0) return 4;
  if (strstr(name, "session-token") || strstr(name, "session")) return 3;
  if (strstr(name, "token")) return 2;
  if (strstr(name, "auth")) return 1;
  return 0;
}

// A portal response may set unrelated cookies as well. Keep the highest
// confidence auth/session cookie independently of Set-Cookie header order.
inline bool bambuSelectSessionCookie(const char* setCookie, char* output,
                                     size_t outputSize, int& selectedPriority) {
  if (!output || outputSize == 0) return false;
  char candidate[512] = {};
  if (!bambuExtractSessionCookie(setCookie, candidate, sizeof(candidate))) {
    return false;
  }
  const int priority = bambuSessionCookiePriority(candidate);
  if (priority == 0 || priority <= selectedPriority) {
    bambuAuthSecureZero(candidate, sizeof(candidate));
    return false;
  }
  const size_t length = strlen(candidate);
  if (length >= outputSize) {
    bambuAuthSecureZero(candidate, sizeof(candidate));
    return false;
  }
  bambuAuthSecureZero(output, outputSize);
  memcpy(output, candidate, length + 1);
  selectedPriority = priority;
  bambuAuthSecureZero(candidate, sizeof(candidate));
  return true;
}

// Accept the names used by the user-service and by the first-party portal, but
// require both credentials before touching the caller's pending transaction.
inline bool bambuCopyAuthEnvelope(const BambuAuthEnvelopeInput& input,
                                  char* accessOutput, size_t accessOutputSize,
                                  char* refreshOutput, size_t refreshOutputSize,
                                  uint32_t& accessLifetime,
                                  uint32_t& refreshLifetime) {
  if (accessOutput && accessOutputSize) accessOutput[0] = '\0';
  if (refreshOutput && refreshOutputSize) refreshOutput[0] = '\0';
  accessLifetime = 0;
  refreshLifetime = 0;
  if (!accessOutput || accessOutputSize == 0 || !refreshOutput ||
      refreshOutputSize == 0) {
    return false;
  }

  const char* access = bambuFirstNonEmpty(
      input.token, input.accessToken, input.access_token);
  const char* refresh = bambuFirstNonEmpty(
      input.refreshToken, input.refresh_token);
  const size_t accessLength = strlen(access);
  const size_t refreshLength = strlen(refresh);
  if (accessLength == 0 || refreshLength == 0 ||
      accessLength >= accessOutputSize ||
      refreshLength >= refreshOutputSize) {
    return false;
  }

  memcpy(accessOutput, access, accessLength + 1);
  memcpy(refreshOutput, refresh, refreshLength + 1);
  accessLifetime = input.expiresIn;
  refreshLifetime = input.refreshExpiresIn;
  return true;
}
