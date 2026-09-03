#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "../src/soak_log.hpp"

int main() {
  // Свежее состояние: валидно, минимумы не установлены, счётчики нулевые.
  SoakState state = soakFresh();
  assert(soakValid(state));
  assert(state.minHeap == SOAK_HEAP_UNSET);
  assert(state.minLargest == SOAK_HEAP_UNSET);
  assert(state.minStackLoop == SOAK_STACK_UNSET);
  assert(state.soakUptimeSeconds == 0);
  assert(state.bootCount == 0);
  assert(state.shadowMismatchesTotal == 0);
  assert(soakLatestBoot(state) == nullptr);

  // Складывание heap: реальный минимум опускает планку; нули и «нет данных»
  // не затирают уже накопленные значения.
  soakFoldHeap(state, 60000, 40000);
  assert(state.minHeap == 60000 && state.minLargest == 40000);
  soakFoldHeap(state, 49704, 38900);
  assert(state.minHeap == 49704 && state.minLargest == 38900);
  soakFoldHeap(state, SOAK_HEAP_UNSET, SOAK_HEAP_UNSET);
  soakFoldHeap(state, 0, 0);
  assert(state.minHeap == 49704 && state.minLargest == 38900);

  // Складывание стеков: нулевые (неизвестные) резервы игнорируются.
  soakFoldStack(state, 3024, 17232, 7160, 3148);
  soakFoldStack(state, 2900, 0, 7000, 0);
  assert(state.minStackLoop == 2900);
  assert(state.minStackWeather == 17232);
  assert(state.minStackPrinter == 7000);
  assert(state.minStackMqtt == 3148);

  // Дельты счётчиков: монотонный рост учитывается, сброс на ребуте — нет.
  soakFoldCounters(state, 10, 0, 2, 0);
  assert(state.shadowMismatchesTotal == 10);
  assert(state.mqttReconnectsTotal == 2);
  soakFoldCounters(state, 25, 10, 2, 2);
  assert(state.shadowMismatchesTotal == 25);
  assert(state.mqttReconnectsTotal == 2);
  soakFoldCounters(state, 3, 25, 0, 2);
  assert(state.shadowMismatchesTotal == 25);
  assert(state.mqttReconnectsTotal == 2);

  // Накопление аптайма и число чекпоинтов.
  soakAccumulateUptime(state, 600);
  soakAccumulateUptime(state, 600);
  assert(state.soakUptimeSeconds == 1200);
  assert(state.checkpointCount == 2);

  // Кольцо boot-записей: пустые слоты заполняются, переполнение вытесняет
  // самую старую запись, bootCount растёт монотонно.
  for (uint32_t i = 0; i < SOAK_BOOT_RING_CAPACITY + 4; ++i) {
    soakPushBoot(state, 3, i % 2, 13, i * 600);
  }
  assert(state.bootCount == SOAK_BOOT_RING_CAPACITY + 4);
  const SoakBootRecord* latest = soakLatestBoot(state);
  assert(latest != nullptr);
  assert(latest->seq == SOAK_BOOT_RING_CAPACITY + 4);
  assert(latest->resetReason == 3);
  assert(latest->soakUptimeAtBoot == (SOAK_BOOT_RING_CAPACITY + 3) * 600);
  // Самая старая запись в кольце имеет seq == 5 (первые четыре вытеснены).
  uint32_t minSeq = UINT32_MAX;
  for (size_t i = 0; i < SOAK_BOOT_RING_CAPACITY; ++i) {
    assert(state.boots[i].seq != 0);
    if (state.boots[i].seq < minSeq) minSeq = state.boots[i].seq;
  }
  assert(minSeq == 5);

  // Контрольная сумма: любое повреждение байта инвалидирует состояние.
  soakSeal(state);
  assert(soakValid(state));
  uint8_t* raw = reinterpret_cast<uint8_t*>(&state);
  size_t probe = offsetof(SoakState, minHeap);
  raw[probe] ^= 0x01;
  assert(!soakValid(state));

  // Побайтная сериализация/восстановление (как NVS blob) сохраняет валидность.
  SoakState restored = soakFresh();
  memcpy(&restored, &state, sizeof(SoakState));
  assert(!soakValid(restored));
  raw[probe] ^= 0x01;
  soakSeal(state);
  assert(soakValid(state));
  memcpy(&restored, &state, sizeof(SoakState));
  assert(soakValid(restored));
  assert(restored.soakUptimeSeconds == 1200);

  return 0;
}
