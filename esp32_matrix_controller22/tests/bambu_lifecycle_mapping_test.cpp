#include <assert.h>
#include <string.h>

#include "../src/bambu_report_parser.hpp"

int main() {
  assert(bambuLifecycleFromReport("RUNNING", false) == PRINTER_PRINTING);
  assert(bambuLifecycleFromReport("PREPARE", false) == PRINTER_PREPARING);
  assert(bambuLifecycleFromReport("PAUSED", false) == PRINTER_PAUSED);
  assert(bambuLifecycleFromReport("FINISH", false) == PRINTER_FINISHED);
  assert(bambuLifecycleFromReport("IDLE", false) == PRINTER_IDLE);
  assert(bambuLifecycleFromReport("FAILED", false) == PRINTER_ERROR);
  // Ошибка = принтер сам остановился. hms/print_error без остановки
  // (информационные уведомления) не красят матрицу в красное.
  assert(bambuLifecycleFromReport("RUNNING", true) == PRINTER_PRINTING);
  assert(bambuLifecycleFromReport("PREPARE", true) == PRINTER_PREPARING);
  assert(bambuLifecycleFromReport("IDLE", true) == PRINTER_IDLE);
  assert(bambuLifecycleFromReport("FINISH", true) == PRINTER_FINISHED);
  assert(bambuLifecycleFromReport("PAUSE", true) == PRINTER_ERROR);
  assert(bambuLifecycleFromReport("PAUSED", true) == PRINTER_ERROR);
  assert(bambuLifecycleFromReport("PAUSE", false) == PRINTER_PAUSED);
  assert(bambuLifecycleFromReport("", true) == PRINTER_UNKNOWN);
  assert(bambuLifecycleFromReport("new_future_state", false) == PRINTER_UNKNOWN);

  PrinterStatusReducer reducer;
  reducer.configure(true, true, "direct cloud ready");
  PrinterStatePatch printing;
  printing.present = PRINTER_PATCH_LIFECYCLE | PRINTER_PATCH_PROGRESS |
                     PRINTER_PATCH_JOB_NAME;
  printing.lifecycle = PRINTER_PRINTING;
  printing.progress = 42;
  strcpy(printing.jobName, "known job");
  reducer.apply(printing);

  PrinterStatePatch offline;
  offline.present = PRINTER_PATCH_LIFECYCLE;
  offline.lifecycle = PRINTER_OFFLINE;
  reducer.apply(offline);
  PrinterStateSnapshot snapshot = reducer.snapshot();
  assert(snapshot.visibleLifecycle == PRINTER_PRINTING);
  assert(snapshot.progress == 42);
  assert(strcmp(snapshot.jobName, "known job") == 0);

  PrinterStatePatch idle;
  idle.present = PRINTER_PATCH_LIFECYCLE;
  idle.lifecycle = PRINTER_IDLE;
  reducer.apply(idle);
  snapshot = reducer.snapshot();
  assert(snapshot.visibleLifecycle == PRINTER_UNKNOWN);
  assert(snapshot.progress == 0);
  return 0;
}
