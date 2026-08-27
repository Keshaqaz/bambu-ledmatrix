#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

inline bool secureBytesEqual(const uint8_t* a, const uint8_t* b, size_t length) {
  if (!a || !b) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < length; ++i) diff |= a[i] ^ b[i];
  return diff == 0;
}

inline bool extractCookieValue(const char* cookieHeader, const char* name,
                               char* output, size_t outputSize) {
  if (!cookieHeader || !name || !name[0] || !output || outputSize < 2) return false;
  const size_t nameLength = strlen(name);
  const char* cursor = cookieHeader;
  while (*cursor) {
    while (*cursor == ' ' || *cursor == ';') ++cursor;
    const char* equals = strchr(cursor, '=');
    if (!equals) break;
    const char* end = strchr(equals + 1, ';');
    if (!end) end = cookieHeader + strlen(cookieHeader);
    const size_t keyLength = static_cast<size_t>(equals - cursor);
    const size_t valueLength = static_cast<size_t>(end - (equals + 1));
    if (keyLength == nameLength && strncmp(cursor, name, nameLength) == 0) {
      if (valueLength == 0 || valueLength >= outputSize) return false;
      memcpy(output, equals + 1, valueLength);
      output[valueLength] = '\0';
      return true;
    }
    cursor = end;
  }
  return false;
}

inline bool isValidAdminPassword(const char* password) {
  if (!password) return false;
  const size_t length = strlen(password);
  if (length < 10 || length > 64) return false;
  for (size_t i = 0; i < length; ++i) {
    if (password[i] != ' ' && password[i] != '\t' && password[i] != '\r' &&
        password[i] != '\n') return true;
  }
  return false;
}

inline const char* redactSecretForApi(const char* secret) {
  return secret && secret[0] ? "***" : "";
}

inline bool isRedactedSecretValue(const char* value) {
  return value && strcmp(value, "***") == 0;
}
