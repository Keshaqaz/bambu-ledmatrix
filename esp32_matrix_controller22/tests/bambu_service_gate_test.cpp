#include <assert.h>

#include "../src/bambu_service_gate.hpp"

int main() {
  DirectCloudServiceGate gate;

  // Тумблер включён: каждый проход — полный сервис (MQTT, refresh, запросы).
  assert(gate.decide(true) == DirectCloudServiceMode::kActive);
  assert(gate.decide(true) == DirectCloudServiceMode::kActive);
  assert(!gate.tornDown());

  // Тумблер выключили (A/B 2026-09-03: service() звался безусловно и держал
  // direct-MQTT/TLS при bambuEnabled=false): первый проход — teardown
  // транспорта, дальше — тишина без cloud-активности.
  assert(gate.decide(false) == DirectCloudServiceMode::kTeardown);
  for (int i = 0; i < 100; ++i)
    assert(gate.decide(false) == DirectCloudServiceMode::kIdle);
  assert(gate.tornDown());

  // Повторное включение возобновляет сервис; следующее выключение снова
  // даёт ровно один teardown-проход.
  assert(gate.decide(true) == DirectCloudServiceMode::kActive);
  assert(!gate.tornDown());
  assert(gate.decide(false) == DirectCloudServiceMode::kTeardown);
  assert(gate.decide(false) == DirectCloudServiceMode::kIdle);
  return 0;
}
