#pragma once

#include <stdint.h>
#include <string.h>

// Персистящий латч «финиш уже объявлен» (после перезагрузки матрицы).
//
// Cloud-поток продолжает нести FINISH для незакрытой на принтере работы
// (gcode_state=FINISH живёт, пока работу не dismiss'нули на plates), а RAM-латч
// редьюсера сбрасывается при каждом буте — первый FINISH-репорт после включения
// питания заново показывал зелёную полосу «финиша» для многодневней работы.
// Запись в NVS переживает бут: совпадающий jobKey → подавить анонс, новая
// работа (jobKey отличается либо была активность печати) → анонсировать.
struct FinishAnnouncementRecord {
  uint8_t version;
  char jobKey[96];

  FinishAnnouncementRecord() : version(0) { jobKey[0] = '\0'; }
};

static const uint8_t kFinishAnnouncementRecordVersion = 1;

// Ключ идентификации работы: имя работы из репорта; пустое имя кодируется
// сентинелом, чтобы «пусто == пусто» матчилось детерминированно.
inline void finishJobKey(const char* jobName, char* out, size_t outSize) {
  if (!jobName || !jobName[0]) {
    if (outSize < 1) return;
    out[0] = '\0';
    strncat(out, "<none>", outSize - 1);
    return;
  }
  strncpy(out, jobName, outSize - 1);
  out[outSize - 1] = '\0';
}

inline bool finishAnnouncementRecordValid(const FinishAnnouncementRecord& r) {
  return r.version == kFinishAnnouncementRecordVersion && r.jobKey[0] != '\0';
}

// Показывать ли зелёную полосу для FINISH-репорта, пришедшего первым после
// бута: только если этот финиш ещё не был объявлен (нет записи либо имя
// работы отличается от сохранённого).
inline bool shouldAnnounceBootFinished(const FinishAnnouncementRecord& r,
                                       const char* jobKey) {
  if (!finishAnnouncementRecordValid(r)) return true;
  return strcmp(r.jobKey, jobKey) != 0;
}
