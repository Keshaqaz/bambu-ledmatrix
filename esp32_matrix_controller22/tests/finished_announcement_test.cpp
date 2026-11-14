#include <assert.h>
#include <string.h>

#include "../src/finished_announcement.hpp"
#include "../src/printer_status_replay.hpp"

// Живой эпизод 2026-09-04: после переподключения питания матрица показала
// зелёную полосу «финиша», хотя принтер не печатал давно — cloud-поток несёт
// FINISH для незакрытой работы, а RAM-латч сбрасывается при каждом буте.
int main() {
  // ── Политика персистентности ──
  FinishAnnouncementRecord none;
  assert(!finishAnnouncementRecordValid(none));
  char benchy[96], cube[96], empty[96];
  finishJobKey("benchy.gcode", benchy, sizeof(benchy));
  finishJobKey("cube", cube, sizeof(cube));
  finishJobKey("", empty, sizeof(empty));
  assert(strcmp(empty, "<none>") == 0);

  // Нет записи — первый финиш анонсируется.
  assert(shouldAnnounceBootFinished(none, benchy));

  FinishAnnouncementRecord announced;
  announced.version = kFinishAnnouncementRecordVersion;
  strcpy(announced.jobKey, benchy);
  assert(finishAnnouncementRecordValid(announced));
  // Тот же job после ребута — не анонсировать (ложный «свежий финиш»).
  assert(!shouldAnnounceBootFinished(announced, benchy));
  // Другой job — анонсировать (новая работа).
  assert(shouldAnnounceBootFinished(announced, cube));
  // Пустое имя матчится само с собой (незакрытая работа без subtask_name).
  FinishAnnouncementRecord announcedEmpty;
  announcedEmpty.version = kFinishAnnouncementRecordVersion;
  strcpy(announcedEmpty.jobKey, "<none>");
  assert(!shouldAnnounceBootFinished(announcedEmpty, empty));
  // Битая версия записи — считаем записи нет, анонсируем.
  FinishAnnouncementRecord stale;
  stale.version = 0;
  strcpy(stale.jobKey, benchy);
  assert(!finishAnnouncementRecordValid(stale));
  assert(shouldAnnounceBootFinished(stale, benchy));

  // ── Редьюсер: подавление анонса до apply(FINISHED) ──
  ScriptedPrinterStatusSource source;
  source.configure(true, true, "direct mqtt");

  PrinterStatePatch report = {};
  report.present = PRINTER_PATCH_TRANSPORT_CONNECTED |
                   PRINTER_PATCH_PRINTER_ONLINE | PRINTER_PATCH_LIFECYCLE |
                   PRINTER_PATCH_PROGRESS | PRINTER_PATCH_JOB_NAME;
  report.transportConnected = true;
  report.printerOnline = true;
  report.progress = 100;
  strcpy(report.jobName, "benchy.gcode");

  // Подавили (запись в NVS совпала): FINISH сразу коллапсирует в idle,
  // зелёной полосы нет вообще.
  report.lifecycle = PRINTER_FINISHED;
  source.suppressFinishedAnnouncement();
  source.replay(report);
  PrinterStateSnapshot snapshot = source.snapshot();
  assert(snapshot.lifecycle == PRINTER_IDLE);
  assert(snapshot.visibleLifecycle == PRINTER_UNKNOWN);
  assert(snapshot.progress == 0);
  assert(strcmp(snapshot.stage, "idle") == 0);
  assert(strcmp(snapshot.detail, "idle") == 0);
  assert(strcmp(snapshot.jobName, "benchy.gcode") == 0);

  // Новая печать сбрасывает подавление: её финиш анонсируется один раз.
  report.lifecycle = PRINTER_PRINTING;
  report.progress = 10;
  source.replay(report);
  assert(source.snapshot().lifecycle == PRINTER_PRINTING);
  report.lifecycle = PRINTER_FINISHED;
  report.progress = 100;
  source.replay(report);
  snapshot = source.snapshot();
  assert(snapshot.lifecycle == PRINTER_FINISHED);
  assert(snapshot.visibleLifecycle == PRINTER_FINISHED);
  assert(snapshot.progress == 100);
  source.advanceBy(PrinterStatusReducer::kFinishedGraceMs + 1);
  assert(source.snapshot().lifecycle == PRINTER_IDLE);
  source.replay(report);
  assert(source.snapshot().lifecycle == PRINTER_IDLE);
  return 0;
}
