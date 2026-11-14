#pragma once

// Decision gate for entering the direct Cloud MQTT transport connect.
//
// Why this exists (2026-09-03): esp-tls error teardown after a failed
// broker-hostname resolve panics on esp32 core 3.3.8 (Store access fault /
// lwIP udp_remove "Required to lock TCPIP core functionality!") when the
// connect is attempted before the Wi-Fi stack's resolver is usable —
// getaddrinfo() returns 202 and the teardown of the half-built transport
// faults. WL_CONNECTED (got IP) is NOT proof that DNS answers yet; at warm
// reboots the window is ~0.5-3 s and esp-mqtt enters the broken path almost
// every boot. The gate requires a resolver confirmation (WiFi.hostByName on
// the broker host) before esp-mqtt is allowed to connect; a failed resolve
// is fed into the existing BoundedRecoveryBackoff as RECOVERY_DNS
// (5 -> 60 s) instead of a crash-reboot roulette.
enum class MqttConnectGate {
  kConnect = 0,   // broker hostname resolved: safe to start esp-mqtt
  kAwaitDns = 1,  // resolver not usable: retry via RECOVERY_DNS backoff
};

inline MqttConnectGate mqttConnectGate(bool wifiAvailable,
                                       bool brokerHostnameResolved) {
  if (!wifiAvailable) return MqttConnectGate::kAwaitDns;
  return brokerHostnameResolved ? MqttConnectGate::kConnect
                                : MqttConnectGate::kAwaitDns;
}
