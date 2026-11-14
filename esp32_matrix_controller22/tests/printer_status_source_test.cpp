#include <assert.h>
#include <string.h>

#include "../src/printer_status_replay.hpp"

static void test_periodic_finished_reports_hold_idle_after_demote();

int main() {
  ScriptedPrinterStatusSource source;
  source.configure(true, true, "relay ready");

  PrinterStatePatch printing = {};
  printing.present = PRINTER_PATCH_TRANSPORT_CONNECTED |
                     PRINTER_PATCH_PRINTER_ONLINE |
                     PRINTER_PATCH_LIFECYCLE | PRINTER_PATCH_PROGRESS;
  printing.transportConnected = true;
  printing.printerOnline = true;
  printing.lifecycle = PRINTER_PRINTING;
  printing.progress = 1;
  source.replay(printing);

  PrinterStateSnapshot snapshot = source.snapshot();
  assert(snapshot.lifecycle == PRINTER_PRINTING);
  assert(snapshot.visibleLifecycle == PRINTER_PRINTING);
  assert(snapshot.progress == 1);

  PrinterStatePatch full = {};
  full.present = PRINTER_PATCH_PROGRESS | PRINTER_PATCH_CURRENT_LAYER |
                 PRINTER_PATCH_TOTAL_LAYERS | PRINTER_PATCH_REMAINING_SECONDS |
                 PRINTER_PATCH_JOB_NAME | PRINTER_PATCH_STAGE |
                 PRINTER_PATCH_DETAIL;
  full.progress = 42;
  full.currentLayer = 12;
  full.totalLayers = 80;
  full.remainingSeconds = 3600;
  strcpy(full.jobName, "calibration cube");
  strcpy(full.stage, "outer wall");
  strcpy(full.detail, "Printing 42%");
  source.replay(full);

  PrinterStatePatch partial = {};
  partial.present = PRINTER_PATCH_STAGE;
  strcpy(partial.stage, "infill");
  source.replay(partial);
  snapshot = source.snapshot();
  assert(snapshot.transportConnected);
  assert(snapshot.printerOnline);
  assert(snapshot.progress == 42);
  assert(snapshot.currentLayer == 12);
  assert(snapshot.totalLayers == 80);
  assert(snapshot.remainingSeconds == 3600);
  assert(strcmp(snapshot.jobName, "calibration cube") == 0);
  assert(strcmp(snapshot.stage, "infill") == 0);
  assert(strcmp(snapshot.detail, "Printing 42%") == 0);

  source.replayMalformed("malformed relay response");
  snapshot = source.snapshot();
  assert(!snapshot.transportConnected);
  assert(!snapshot.printerOnline);
  assert(snapshot.lifecycle == PRINTER_UNKNOWN);
  assert(snapshot.visibleLifecycle == PRINTER_PRINTING);
  assert(snapshot.progress == 42);
  assert(strcmp(snapshot.jobName, "calibration cube") == 0);

  source.replayStale("relay snapshot is stale");
  snapshot = source.snapshot();
  assert(snapshot.transportConnected);
  assert(!snapshot.printerOnline);
  assert(snapshot.lifecycle == PRINTER_OFFLINE);
  assert(snapshot.visibleLifecycle == PRINTER_PRINTING);
  assert(snapshot.progress == 42);

  PrinterStatePatch idle = {};
  idle.present = PRINTER_PATCH_TRANSPORT_CONNECTED |
                 PRINTER_PATCH_PRINTER_ONLINE |
                 PRINTER_PATCH_LIFECYCLE;
  idle.transportConnected = true;
  idle.printerOnline = true;
  idle.lifecycle = PRINTER_IDLE;
  source.replay(idle);
  snapshot = source.snapshot();
  assert(snapshot.lifecycle == PRINTER_IDLE);
  assert(snapshot.visibleLifecycle == PRINTER_UNKNOWN);
  assert(snapshot.progress == 0);

  PrinterStatePatch finished = {};
  finished.present = PRINTER_PATCH_LIFECYCLE | PRINTER_PATCH_PROGRESS;
  finished.lifecycle = PRINTER_FINISHED;
  finished.progress = 1;
  source.replay(finished);
  snapshot = source.snapshot();
  assert(snapshot.lifecycle == PRINTER_FINISHED);
  assert(snapshot.visibleLifecycle == PRINTER_FINISHED);
  assert(snapshot.progress == 100);

  source.advanceBy(PrinterStatusReducer::kFinishedGraceMs - 1);
  snapshot = source.snapshot();
  assert(snapshot.lifecycle == PRINTER_FINISHED);
  assert(snapshot.visibleLifecycle == PRINTER_FINISHED);
  source.advanceBy(1);
  snapshot = source.snapshot();
  assert(snapshot.lifecycle == PRINTER_IDLE);
  assert(snapshot.visibleLifecycle == PRINTER_UNKNOWN);

  // Finding A: повторный FINISHED-репорт после grace-демоута — это тот же
  // завершённый job (cloud продолжает его слать) — коллапс в idle, а не
  // ре-вход в FINISHED. Паритет с relay: stage/detail "idle", progress 0,
  // jobName сохраняется.
  source.replay(finished);
  snapshot = source.snapshot();
  assert(snapshot.lifecycle == PRINTER_IDLE);
  assert(snapshot.visibleLifecycle == PRINTER_UNKNOWN);
  assert(snapshot.progress == 0);
  assert(strcmp(snapshot.stage, "idle") == 0);
  assert(strcmp(snapshot.detail, "idle") == 0);
  assert(strcmp(snapshot.jobName, "calibration cube") == 0);

  // Новый запуск печати сбрасывает latch: финиш объявляется заново.
  source.replay(printing);
  snapshot = source.snapshot();
  assert(snapshot.lifecycle == PRINTER_PRINTING);
  source.replay(finished);
  snapshot = source.snapshot();
  assert(snapshot.lifecycle == PRINTER_FINISHED);
  assert(snapshot.progress == 100);

  source.replayMalformed("temporary failure during finished grace");
  snapshot = source.snapshot();
  assert(snapshot.lifecycle == PRINTER_UNKNOWN);
  assert(snapshot.visibleLifecycle == PRINTER_FINISHED);
  assert(snapshot.progress == 100);

  source.advanceBy(PrinterStatusReducer::kFinishedGraceMs);
  snapshot = source.snapshot();
  assert(snapshot.lifecycle == PRINTER_UNKNOWN);
  assert(snapshot.visibleLifecycle == PRINTER_FINISHED);
  assert(snapshot.progress == 100);

  source.replay(idle);
  snapshot = source.snapshot();
  assert(snapshot.lifecycle == PRINTER_IDLE);
  assert(snapshot.visibleLifecycle == PRINTER_UNKNOWN);

  // FAILED is a terminal announcement, not an indefinitely active alarm.
  // It is shown once for a bounded grace and repeated reports stay idle until
  // real printer activity re-arms the announcement.
  PrinterStatePatch failed = {};
  failed.present = PRINTER_PATCH_LIFECYCLE | PRINTER_PATCH_PROGRESS;
  failed.lifecycle = PRINTER_ERROR;
  failed.terminalFailure = true;
  failed.progress = 42;
  source.replay(failed);
  assert(source.snapshot().visibleLifecycle == PRINTER_ERROR);
  source.advanceBy(PrinterStatusReducer::kTerminalFailureGraceMs);
  snapshot = source.snapshot();
  assert(snapshot.lifecycle == PRINTER_IDLE);
  assert(snapshot.visibleLifecycle == PRINTER_UNKNOWN);
  assert(snapshot.progress == 0);
  source.replay(failed);
  assert(source.snapshot().lifecycle == PRINTER_IDLE);
  source.replay(printing);
  source.replay(failed);
  assert(source.snapshot().visibleLifecycle == PRINTER_ERROR);

  // Active alarms remain visible until an explicit lifecycle update clears
  // them; only gcode_state=FAILED gets terminal-announcement semantics.
  source.replay(printing);
  PrinterStatePatch activeError = failed;
  activeError.terminalFailure = false;
  source.replay(activeError);
  source.advanceBy(PrinterStatusReducer::kTerminalFailureGraceMs * 2);
  assert(source.snapshot().visibleLifecycle == PRINTER_ERROR);
  source.replay(idle);
  source.replay(failed);
  source.replay(activeError);
  source.advanceBy(PrinterStatusReducer::kTerminalFailureGraceMs * 2);
  assert(source.snapshot().visibleLifecycle == PRINTER_ERROR);
  test_periodic_finished_reports_hold_idle_after_demote();
  return 0;
}

// Finding A (живой каденс 2026-09-03, T05): после демоута periodic cloud
// репорты каждые ~15–65 с снова назначали FINISHED — бесконечное
// finished↔idle колыхание, 100% shadow-mismatch'ей. Редьюсер должен
// держать idle, пока не пройдёт не-finished lifecycle.
static void test_periodic_finished_reports_hold_idle_after_demote() {
  ScriptedPrinterStatusSource source;
  source.configure(true, true, "direct mqtt");

  PrinterStatePatch report = {};
  report.present = PRINTER_PATCH_TRANSPORT_CONNECTED |
                   PRINTER_PATCH_PRINTER_ONLINE | PRINTER_PATCH_LIFECYCLE |
                   PRINTER_PATCH_PROGRESS | PRINTER_PATCH_JOB_NAME |
                   PRINTER_PATCH_STAGE | PRINTER_PATCH_DETAIL;
  report.transportConnected = true;
  report.printerOnline = true;
  report.lifecycle = PRINTER_PRINTING;
  report.progress = 42;
  strcpy(report.jobName, "benchy");
  strcpy(report.stage, "printing");
  strcpy(report.detail, "printing 42%");
  source.replay(report);

  report.lifecycle = PRINTER_FINISHED;
  report.progress = 100;
  strcpy(report.detail, "finished");
  source.replay(report);
  assert(source.snapshot().lifecycle == PRINTER_FINISHED);

  source.advanceBy(PrinterStatusReducer::kFinishedGraceMs + 1);
  PrinterStateSnapshot snapshot = source.snapshot();
  assert(snapshot.lifecycle == PRINTER_IDLE);
  assert(snapshot.progress == 0);

  for (uint8_t reportIndex = 0; reportIndex < 6; ++reportIndex) {
    source.replay(report);
    snapshot = source.snapshot();
    assert(snapshot.transportConnected);
    assert(snapshot.printerOnline);
    assert(snapshot.lifecycle == PRINTER_IDLE);
    assert(snapshot.visibleLifecycle == PRINTER_UNKNOWN);
    assert(snapshot.progress == 0);
    assert(strcmp(snapshot.stage, "idle") == 0);
    assert(strcmp(snapshot.detail, "idle") == 0);
    assert(strcmp(snapshot.jobName, "benchy") == 0);

    // Транзиентный офлайн не должен ре-армить финиш: retention держит
    // видимый overlay, вернувшиеся FINISHED-репорты — по-прежнему idle.
    if (reportIndex == 2) {
      source.replayStale("direct mqtt report stale");
      snapshot = source.snapshot();
      assert(snapshot.lifecycle == PRINTER_OFFLINE);
      assert(snapshot.visibleLifecycle == PRINTER_UNKNOWN);
      continue;
    }
    source.advanceBy(45000);
    assert(source.snapshot().lifecycle == PRINTER_IDLE);
  }

  report.lifecycle = PRINTER_PRINTING;
  report.progress = 7;
  strcpy(report.stage, "printing");
  source.replay(report);
  assert(source.snapshot().lifecycle == PRINTER_PRINTING);
  report.lifecycle = PRINTER_FINISHED;
  report.progress = 100;
  source.replay(report);
  assert(source.snapshot().lifecycle == PRINTER_FINISHED);
  source.advanceBy(PrinterStatusReducer::kFinishedGraceMs + 1);
  assert(source.snapshot().lifecycle == PRINTER_IDLE);
  source.replay(report);
  assert(source.snapshot().lifecycle == PRINTER_IDLE);
}
