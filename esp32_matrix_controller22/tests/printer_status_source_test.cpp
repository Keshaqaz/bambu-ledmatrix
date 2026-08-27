#include <assert.h>
#include <string.h>

#include "../src/printer_status_replay.hpp"

int main() {
  ScriptedPrinterStatusSource source;
  source.configure(true, true, "relay ready");

  PrinterStatePatch printing = {};
  printing.present = PRINTER_PATCH_LIFECYCLE | PRINTER_PATCH_PROGRESS;
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

  source.replayMalformed("temporary failure during finished grace");
  snapshot = source.snapshot();
  assert(snapshot.lifecycle == PRINTER_UNKNOWN);
  assert(snapshot.visibleLifecycle == PRINTER_FINISHED);
  assert(snapshot.progress == 100);

  source.replay(idle);
  snapshot = source.snapshot();
  assert(snapshot.lifecycle == PRINTER_IDLE);
  assert(snapshot.visibleLifecycle == PRINTER_UNKNOWN);
  return 0;
}
