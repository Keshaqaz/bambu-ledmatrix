#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../src/bambu_auth_contract.hpp"

static void test_extracts_only_cookie_pair() {
  char cookie[96] = {};
  assert(bambuExtractSessionCookie(
      "__Secure-next-auth.session-token=session-secret; Path=/; HttpOnly; Secure",
      cookie, sizeof(cookie)));
  assert(strcmp(cookie,
                "__Secure-next-auth.session-token=session-secret") == 0);
}

static void test_rejects_unbounded_or_injectable_cookie() {
  char cookie[16];
  memset(cookie, 'x', sizeof(cookie));
  assert(!bambuExtractSessionCookie("session=0123456789abcdef", cookie,
                                    sizeof(cookie)));
  assert(cookie[0] == '\0');

  assert(!bambuExtractSessionCookie("session=ok\r\nX-Leak: secret", cookie,
                                    sizeof(cookie)));
  assert(cookie[0] == '\0');
}

static void test_selects_session_cookie_independently_of_header_order() {
  char cookie[128] = {};
  int priority = 0;
  assert(!bambuSelectSessionCookie(
      "__Host-next-auth.csrf-token=csrf-secret; Path=/", cookie,
                                   sizeof(cookie), priority));
  assert(cookie[0] == '\0');
  assert(bambuSelectSessionCookie(
      "__Secure-next-auth.session-token=session-secret; Path=/; HttpOnly",
      cookie, sizeof(cookie), priority));
  assert(strcmp(cookie,
                "__Secure-next-auth.session-token=session-secret") == 0);
  assert(!bambuSelectSessionCookie("theme=dark; Path=/", cookie,
                                   sizeof(cookie), priority));
  assert(strcmp(cookie,
                "__Secure-next-auth.session-token=session-secret") == 0);

  char sessionFirst[128] = {};
  priority = 0;
  assert(bambuSelectSessionCookie(
      "__Secure-next-auth.session-token=session-first; Path=/", sessionFirst,
      sizeof(sessionFirst), priority));
  assert(!bambuSelectSessionCookie(
      "__Host-next-auth.csrf-token=csrf-later; Path=/", sessionFirst,
      sizeof(sessionFirst), priority));
  assert(strcmp(sessionFirst,
                "__Secure-next-auth.session-token=session-first") == 0);

  char reverse[128] = {};
  priority = 0;
  assert(bambuSelectSessionCookie("token=preferred; Path=/", reverse,
                                  sizeof(reverse), priority));
  assert(!bambuSelectSessionCookie(
      "__Secure-next-auth.session-token=later; Path=/", reverse,
      sizeof(reverse), priority));
  assert(strcmp(reverse, "token=preferred") == 0);
}

static void test_accepts_portal_token_envelope() {
  BambuAuthEnvelopeInput input = {};
  input.token = "access-from-portal";
  input.refreshToken = "refresh-from-portal";
  input.expiresIn = 3600;
  input.refreshExpiresIn = 2592000;

  char access[32] = {};
  char refresh[32] = {};
  uint32_t accessLifetime = 0;
  uint32_t refreshLifetime = 0;
  assert(bambuCopyAuthEnvelope(input, access, sizeof(access), refresh,
                               sizeof(refresh), accessLifetime,
                               refreshLifetime));
  assert(strcmp(access, "access-from-portal") == 0);
  assert(strcmp(refresh, "refresh-from-portal") == 0);
  assert(accessLifetime == 3600);
  assert(refreshLifetime == 2592000);
}

static void test_missing_refresh_fails_closed() {
  BambuAuthEnvelopeInput input = {};
  input.accessToken = "access-only";

  char access[32] = "old-access";
  char refresh[32] = "old-refresh";
  uint32_t accessLifetime = 123;
  uint32_t refreshLifetime = 456;
  assert(!bambuCopyAuthEnvelope(input, access, sizeof(access), refresh,
                                sizeof(refresh), accessLifetime,
                                refreshLifetime));
  assert(access[0] == '\0');
  assert(refresh[0] == '\0');
  assert(accessLifetime == 0);
  assert(refreshLifetime == 0);
}

int main() {
  test_extracts_only_cookie_pair();
  test_rejects_unbounded_or_injectable_cookie();
  test_selects_session_cookie_independently_of_header_order();
  test_accepts_portal_token_envelope();
  test_missing_refresh_fails_closed();
  return 0;
}
