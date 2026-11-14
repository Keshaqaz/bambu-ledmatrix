#pragma once

#include <stdint.h>

// Defer policy for pausing the direct Cloud MQTT transport so another TLS
// consumer (weather HTTPS, sunrise) can take the single TLS slot.
//
// Why this exists (2026-09-03): stopMqtt() stops+destroys the esp-mqtt
// client back-to-back. When the client has just connected and is inside its
// first read/validation, stop() aborts the live socket and the client task
// unwinds the transport_read error (esp-tls read error -> mqtt_process_
// receive -2) while destroy() frees the client — Store access fault on
// core 3.3.8. Warm boots hit this deterministically: weather's boot fetch
// fails OWM HTTP (resolver still cold), takes the HTTPS branch and pauses a
// seconds-old MQTT connection. Guard rule: never pause a live client that
// has not yet delivered its first report, or is younger than the grace
// window after CONNECTED. Steady-state pauses keep the old behavior.
inline constexpr uint32_t kMqttPauseGuardMs = 45000;

inline bool shouldDeferExternalTlsPause(bool clientLive,
                                        bool activePrint,
                                        bool firstReportReceived,
                                        uint32_t msSinceConnect) {
  if (!clientLive) return false;  // no client: nothing to protect
  if (activePrint) return true;   // existing production rule
  if (!firstReportReceived) return true;  // connecting / validating: never stop
  return msSinceConnect < kMqttPauseGuardMs;
}
