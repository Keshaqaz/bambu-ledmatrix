#pragma once

#include <stdint.h>

// Decision for one BambuCloud::service() pass given the bambuEnabled
// toggle. The toggle is the integration master switch: while it is off
// the direct Cloud path must not own MQTT/TLS sessions or schedule
// automatic refreshes (A/B 2026-09-03: service() used to run
// unconditionally and kept direct-MQTT alive with the toggle off).
enum class DirectCloudServiceMode : uint8_t {
  kActive = 0,    // full service: transport, refresh, queued requests
  kTeardown = 1,  // first pass after disable: stop transport, cancel work
  kIdle = 2,      // disabled steady state: no Cloud activity
};

class DirectCloudServiceGate {
 public:
  DirectCloudServiceMode decide(bool enabled) {
    if (enabled) {
      tornDown_ = false;
      return DirectCloudServiceMode::kActive;
    }
    if (tornDown_) return DirectCloudServiceMode::kIdle;
    tornDown_ = true;
    return DirectCloudServiceMode::kTeardown;
  }

  bool tornDown() const { return tornDown_; }

 private:
  bool tornDown_ = false;
};
