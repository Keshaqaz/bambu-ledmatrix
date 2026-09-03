#pragma once

#include <stddef.h>
#include <stdint.h>

// Автономный soak-журнал T05: прошивка сама накапливает минимумы heap/стеков,
// счётчики shadow/reconnect, аптайм и кольцо boot-записей (с esp_reset_reason)
// в NVS. Журнал переживает краши и откаты слота, вердикт читается через
// GET /api/soak без внешних наблюдателей (Mac/pve/роутер не участвуют).

static constexpr uint32_t SOAK_MAGIC = 0x534F414BUL;  // SOAK
static constexpr uint16_t SOAK_VERSION = 1;
static constexpr size_t SOAK_BOOT_RING_CAPACITY = 16;
static constexpr uint32_t SOAK_HEAP_UNSET = UINT32_MAX;
static constexpr uint16_t SOAK_STACK_UNSET = UINT16_MAX;

inline void soakSecureZero(void* memory, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(memory);
  while (length--) *bytes++ = 0;
}

struct SoakBootRecord {
  uint32_t seq;              // монотонный номер загрузки внутри soak-прогона
  uint32_t soakUptimeAtBoot; // накопленные soak-секунды до старта этой загрузки
  uint8_t resetReason;       // сырой код esp_reset_reason()
  uint8_t partition;         // 0=app0, 1=app1, 255=неизвестно
  uint8_t fwMinor;           // минорная версия 4.3.x (13 для 4.3.13)
  uint8_t reserved;          // выравнивание/будущее
};

struct SoakState {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t soakUptimeSeconds;
  uint32_t bootCount;
  uint32_t seqNext;
  uint32_t minHeap;
  uint32_t minLargest;
  uint16_t minStackLoop;
  uint16_t minStackWeather;
  uint16_t minStackPrinter;
  uint16_t minStackMqtt;
  uint32_t shadowMismatchesTotal;
  uint32_t mqttReconnectsTotal;
  uint32_t checkpointCount;
  SoakBootRecord boots[SOAK_BOOT_RING_CAPACITY];
  uint32_t checksum;

  SoakState() { soakSecureZero(this, sizeof(*this)); }
};

inline uint32_t soakChecksumBytes(const uint8_t* bytes, size_t length) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

inline uint32_t soakStateChecksum(const SoakState& value) {
  return soakChecksumBytes(reinterpret_cast<const uint8_t*>(&value),
                           offsetof(SoakState, checksum));
}

inline SoakState soakFresh() {
  SoakState value;
  value.magic = SOAK_MAGIC;
  value.version = SOAK_VERSION;
  value.size = sizeof(SoakState);
  value.minHeap = SOAK_HEAP_UNSET;
  value.minLargest = SOAK_HEAP_UNSET;
  value.minStackLoop = SOAK_STACK_UNSET;
  value.minStackWeather = SOAK_STACK_UNSET;
  value.minStackPrinter = SOAK_STACK_UNSET;
  value.minStackMqtt = SOAK_STACK_UNSET;
  value.checksum = soakStateChecksum(value);
  return value;
}

inline bool soakValid(const SoakState& value) {
  return value.magic == SOAK_MAGIC && value.version == SOAK_VERSION &&
         value.size == sizeof(SoakState) &&
         value.checksum == soakStateChecksum(value);
}

inline void soakSeal(SoakState& value) {
  value.checksum = soakStateChecksum(value);
}

inline void soakFoldHeap(SoakState& value, uint32_t bootMinHeap,
                         uint32_t bootMinLargest) {
  if (bootMinHeap != 0 && bootMinHeap != SOAK_HEAP_UNSET &&
      bootMinHeap < value.minHeap)
    value.minHeap = bootMinHeap;
  if (bootMinLargest != 0 && bootMinLargest != SOAK_HEAP_UNSET &&
      bootMinLargest < value.minLargest)
    value.minLargest = bootMinLargest;
}

inline void soakFoldStack(SoakState& value, uint16_t loop, uint16_t weather,
                          uint16_t printer, uint16_t mqtt) {
  if (loop != 0 && loop < value.minStackLoop) value.minStackLoop = loop;
  if (weather != 0 && weather < value.minStackWeather) value.minStackWeather = weather;
  if (printer != 0 && printer < value.minStackPrinter) value.minStackPrinter = printer;
  if (mqtt != 0 && mqtt < value.minStackMqtt) value.minStackMqtt = mqtt;
}

inline void soakFoldCounters(SoakState& value, uint32_t shadowMismatches,
                             uint32_t lastShadowMismatches,
                             uint32_t mqttReconnects,
                             uint32_t lastMqttReconnects) {
  if (shadowMismatches >= lastShadowMismatches)
    value.shadowMismatchesTotal += shadowMismatches - lastShadowMismatches;
  if (mqttReconnects >= lastMqttReconnects)
    value.mqttReconnectsTotal += mqttReconnects - lastMqttReconnects;
}

inline void soakAccumulateUptime(SoakState& value, uint32_t deltaSeconds) {
  value.soakUptimeSeconds += deltaSeconds;
  value.checkpointCount++;
}

inline void soakPushBoot(SoakState& value, uint8_t resetReason,
                         uint8_t partition, uint8_t fwMinor,
                         uint32_t soakUptimeAtBoot) {
  SoakBootRecord record;
  record.seq = ++value.seqNext;
  record.soakUptimeAtBoot = soakUptimeAtBoot;
  record.resetReason = resetReason;
  record.partition = partition;
  record.fwMinor = fwMinor;
  // Кольцо: слот с минимальным seq вытесняется. Пока кольцо не заполнено —
  // первый слот с seq == 0.
  size_t victim = 0;
  for (size_t i = 0; i < SOAK_BOOT_RING_CAPACITY; ++i) {
    if (value.boots[i].seq == 0) { victim = i; break; }
    if (value.boots[i].seq < value.boots[victim].seq) victim = i;
  }
  value.boots[victim] = record;
  value.bootCount++;
}

// Новейшая запись (максимальный seq) или nullptr, если кольцо пусто.
inline const SoakBootRecord* soakLatestBoot(const SoakState& value) {
  const SoakBootRecord* latest = nullptr;
  for (size_t i = 0; i < SOAK_BOOT_RING_CAPACITY; ++i) {
    if (value.boots[i].seq == 0) continue;
    if (!latest || value.boots[i].seq > latest->seq) latest = &value.boots[i];
  }
  return latest;
}
