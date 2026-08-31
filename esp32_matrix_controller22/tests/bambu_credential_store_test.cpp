#include <assert.h>
#include <string.h>

#include "../src/bambu_cloud_credentials.hpp"

static BambuCloudCredential credential(const char* uid, const char* serial,
                                       const char* access, const char* refresh) {
  BambuCloudCredential value;
  assert(bambuCredentialSet(value, uid, serial, access, refresh, 100, 200));
  return value;
}

int main() {
  BambuCredentialSlots persisted;
  BambuCredentialTransaction tx(persisted);

  BambuCloudCredential first = credential("123", "SERIAL-A", "access-a", "refresh-a");
  assert(tx.begin(first));
  assert(tx.commit());
  BambuCloudCredential active;
  assert(tx.active(active));
  assert(strcmp(active.accessToken, "access-a") == 0);

  BambuCloudCredential failed = credential("456", "SERIAL-B", "access-b", "refresh-b");
  assert(tx.begin(failed));
  BambuCredentialTransaction afterReboot(persisted);
  afterReboot.recover();
  assert(afterReboot.active(active));
  assert(strcmp(active.accountUid, "123") == 0);
  assert(strcmp(active.serial, "SERIAL-A") == 0);
  assert(strcmp(active.accessToken, "access-a") == 0);
  assert(!afterReboot.hasCandidate());

  BambuCloudCredential rotated;
  assert(bambuBuildRefreshCandidate(active, "access-c", "refresh-c", 300, 400,
                                    rotated));
  assert(afterReboot.begin(rotated));
  assert(afterReboot.commit());
  assert(afterReboot.active(active));
  assert(strcmp(active.accessToken, "access-c") == 0);
  assert(strcmp(active.refreshToken, "refresh-c") == 0);

  BambuCloudCredential notRotated;
  assert(bambuBuildRefreshCandidate(active, "access-d", "", 500, 600,
                                    notRotated));
  assert(strcmp(notRotated.refreshToken, "refresh-c") == 0);

  BambuRefreshRecovery recovery;
  assert(bambuRefreshRecoverySet(recovery, "uid-a", "rotated-refresh"));
  assert(bambuRefreshRecoveryValid(recovery));
  assert(bambuRefreshRecoveryMatches(recovery, "uid-a"));
  assert(!bambuRefreshRecoveryMatches(recovery, "uid-b"));
  assert(strcmp(recovery.refreshToken, "rotated-refresh") == 0);
  recovery.refreshToken[0] = 'X';
  assert(!bambuRefreshRecoveryValid(recovery));

  afterReboot.logout();
  assert(!afterReboot.active(active));
  assert(bambuCredentialMemoryIsZero(persisted));
  return 0;
}
