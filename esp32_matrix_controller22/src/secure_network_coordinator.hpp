#pragma once

#include <atomic>
#include <stdint.h>

enum SecureNetworkOwner : uint8_t {
  SECURE_NETWORK_NONE = 0,
  SECURE_NETWORK_CLOUD_HTTP = 1,
  SECURE_NETWORK_CLOUD_MQTT = 2,
  SECURE_NETWORK_WEATHER = 3,
  SECURE_NETWORK_SUNRISE = 4,
  SECURE_NETWORK_OTA = 5,
};

class SecureNetworkCoordinator {
 public:
  bool acquire(SecureNetworkOwner owner) {
    uint8_t expected = SECURE_NETWORK_NONE;
    return owner != SECURE_NETWORK_NONE &&
           owner_.compare_exchange_strong(expected, owner);
  }

  void release(SecureNetworkOwner owner) {
    uint8_t expected = owner;
    owner_.compare_exchange_strong(expected, SECURE_NETWORK_NONE);
  }

  SecureNetworkOwner owner() const {
    return static_cast<SecureNetworkOwner>(owner_.load());
  }

  bool available() const { return owner() == SECURE_NETWORK_NONE; }

 private:
  std::atomic<uint8_t> owner_{SECURE_NETWORK_NONE};
};

class SecureNetworkLease {
 public:
  SecureNetworkLease(SecureNetworkCoordinator& coordinator,
                     SecureNetworkOwner owner)
      : coordinator_(coordinator), owner_(owner), acquired_(coordinator.acquire(owner)) {}
  ~SecureNetworkLease() { if (acquired_) coordinator_.release(owner_); }
  bool acquired() const { return acquired_; }
  void dismiss() { acquired_ = false; }

 private:
  SecureNetworkCoordinator& coordinator_;
  SecureNetworkOwner owner_;
  bool acquired_;
};
