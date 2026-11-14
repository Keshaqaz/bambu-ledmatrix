#include <assert.h>
#include <string.h>

#include "../src/bambu_report_parser.hpp"

static PrinterStatePatch parseOneByteFragments(const char* payload) {
  const size_t length = strlen(payload);
  BambuReportParser parser;
  assert(parser.begin(length));
  for (size_t offset = 0; offset < length; ++offset) {
    assert(parser.feed(payload + offset, 1, offset, length));
  }
  PrinterStatePatch patch;
  assert(parser.finish(patch));
  return patch;
}

int main() {
  const char* report =
      "{\"print\":{\"gcode_state\":\"RUNNING\",\"mc_percent\":1,"
      "\"stg_cur\":7,\"subtask_name\":\"cube \\\"A\\\"\","
      "\"layer_num\":12,\"total_layer_num\":80,"
      "\"mc_remaining_time\":60},\"ignored\":{\"huge\":\"value\"}}";
  PrinterStatePatch patch = parseOneByteFragments(report);
  assert((patch.present & PRINTER_PATCH_LIFECYCLE) != 0);
  assert(patch.lifecycle == PRINTER_PRINTING);
  assert((patch.present & PRINTER_PATCH_PROGRESS) != 0);
  assert(patch.progress == 1);
  assert(strcmp(patch.jobName, "cube \"A\"") == 0);
  assert(strcmp(patch.stage, "heating_hotend") == 0);
  assert(patch.currentLayer == 12);
  assert(patch.totalLayers == 80);
  assert(patch.remainingSeconds == 3600);

  BambuReportParser partialParser;
  const char* partial = "{\"print\":{\"stage\":\"infill\"}}";
  assert(partialParser.begin(strlen(partial)));
  assert(partialParser.feed(partial, strlen(partial), 0, strlen(partial)));
  PrinterStatePatch partialPatch;
  assert(partialParser.finish(partialPatch));
  assert((partialPatch.present & PRINTER_PATCH_STAGE) != 0);
  assert((partialPatch.present & PRINTER_PATCH_PROGRESS) == 0);
  assert((partialPatch.present & PRINTER_PATCH_JOB_NAME) == 0);
  assert(strcmp(partialPatch.stage, "infill") == 0);

  PrinterStatePatch errorPatch = parseOneByteFragments(
      "{\"print\":{\"gcode_state\":\"RUNNING\","
      "\"stage\":{\"_id\":10,\"name\":\"first layer check\"},"
      "\"hms\":[{\"attr\":123}]}}" );
  assert(errorPatch.lifecycle == PRINTER_PRINTING);
  assert(strcmp(errorPatch.stage, "first layer check") == 0);

  PrinterStatePatch relayCorpus = parseOneByteFragments(
      "{\"print\":{\"gcode_state\":\"RUNNING\","
      "\"mc_percent\":\"0.51\",\"stg_cur\":255,"
      "\"mc_remaining_time\":\"13\"}}");
  assert(relayCorpus.lifecycle == PRINTER_PRINTING);
  assert(relayCorpus.progress == 51);
  assert(strcmp(relayCorpus.stage, "printing") == 0);
  assert(relayCorpus.remainingSeconds == 780);

  PrinterStatePatch finished = parseOneByteFragments(
      "{\"print\":{\"gcode_state\":\"FINISH\",\"mc_percent\":1}}");
  assert(finished.lifecycle == PRINTER_FINISHED);
  assert(finished.progress == 100);

  // Живой репорт 2026-09-04 (push_status простаивающего принтера со старым
  // FAILED): subtask_name непустой, затем gcode_file="" — пустой gcode_file
  // не должен затирать имя работы (relay-паритет: gcode_file — fallback).
  PrinterStatePatch staleFailed = parseOneByteFragments(
      "{\"print\":{\"gcode_state\":\"FAILED\",\"mc_percent\":0,"
      "\"mc_remaining_time\":59,\"subtask_name\":\"0.2mm layer\","
      "\"gcode_file\":\"\",\"stg_cur\":255,\"total_layer_num\":45}}");
  assert(staleFailed.lifecycle == PRINTER_ERROR);
  assert(staleFailed.terminalFailure);
  assert(staleFailed.progress == 0);
  assert((staleFailed.present & PRINTER_PATCH_JOB_NAME) != 0);
  assert(strcmp(staleFailed.jobName, "0.2mm layer") == 0);
  assert(staleFailed.totalLayers == 45);
  assert(staleFailed.remainingSeconds == 3540);
  // Уведомление hms во время печати — не ошибка (принтер печатает дальше).
  PrinterStatePatch hmsNotice = parseOneByteFragments(
      "{\"print\":{\"gcode_state\":\"RUNNING\",\"hms\":[{\"attr\":123}]}}" );
  assert(hmsNotice.lifecycle == PRINTER_PRINTING);
  PrinterStatePatch idleNotice = parseOneByteFragments(
      "{\"print\":{\"gcode_state\":\"IDLE\",\"print_error\":50348044,"
      "\"hms\":[{\"attr\":50331904,\"code\":262151}]}}" );
  assert(idleNotice.lifecycle == PRINTER_IDLE);
  // Дельта только с hms (без gcode_state) не задаёт lifecycle.
  PrinterStatePatch hmsOnly = parseOneByteFragments(
      "{\"print\":{\"hms\":[{\"attr\":123,\"code\":65537}]}}" );
  assert((hmsOnly.present & PRINTER_PATCH_LIFECYCLE) == 0);
  // Принтер встал на паузу из-за ошибки — это ошибка.
  PrinterStatePatch activeAlarm = parseOneByteFragments(
      "{\"print\":{\"gcode_state\":\"PAUSE\",\"print_error\":117473287}}" );
  assert(activeAlarm.lifecycle == PRINTER_ERROR);
  assert(!activeAlarm.terminalFailure);
  PrinterStatePatch hmsPause = parseOneByteFragments(
      "{\"print\":{\"gcode_state\":\"PAUSE\",\"hms\":[{\"attr\":1}]}}" );
  assert(hmsPause.lifecycle == PRINTER_ERROR);
  PrinterStatePatch userPause = parseOneByteFragments(
      "{\"print\":{\"gcode_state\":\"PAUSE\",\"print_error\":0,\"hms\":[]}}" );
  assert(userPause.lifecycle == PRINTER_PAUSED);
  // gcode_file используется только когда subtask_name не дал имени.
  PrinterStatePatch fileOnly = parseOneByteFragments(
      "{\"print\":{\"gcode_state\":\"IDLE\",\"gcode_file\":\"job.gcode\"}}");
  assert(strcmp(fileOnly.jobName, "job.gcode") == 0);
  // Оба пустые — патч не несёт имени и не затирает прежнее.
  PrinterStatePatch bothEmpty = parseOneByteFragments(
      "{\"print\":{\"gcode_state\":\"IDLE\",\"subtask_name\":\"\","
      "\"gcode_file\":\"\"}}");
  assert((bothEmpty.present & PRINTER_PATCH_JOB_NAME) == 0);

  PrinterStatePatch unknownArrays = parseOneByteFragments(
      "{\"print\":{\"gcode_state\":\"RUNNING\"},"
      "\"unknown\":[1,2,\"a\",\"b\",true,null]}");
  assert(unknownArrays.lifecycle == PRINTER_PRINTING);

  BambuReportParser malformed;
  const char* bad = "{\"print\":{\"mc_percent\":42";
  assert(malformed.begin(strlen(bad)));
  assert(malformed.feed(bad, strlen(bad), 0, strlen(bad)));
  PrinterStatePatch unchanged;
  unchanged.present = PRINTER_PATCH_PROGRESS;
  unchanged.progress = 77;
  assert(!malformed.finish(unchanged));
  assert(unchanged.present == PRINTER_PATCH_PROGRESS);
  assert(unchanged.progress == 77);

  const char* invalidReports[] = {
      "{\"print\":{\"mc_percent\":01}}",
      "{\"print\":{\"mc_percent\":42,}}",
      "{\"print\":[{\"gcode_state\":\"RUNNING\"},]}",
  };
  for (const char* invalid : invalidReports) {
    BambuReportParser invalidParser;
    assert(invalidParser.begin(strlen(invalid)));
    bool accepted = invalidParser.feed(invalid, strlen(invalid), 0, strlen(invalid));
    PrinterStatePatch sentinel;
    sentinel.present = PRINTER_PATCH_PROGRESS;
    sentinel.progress = 77;
    assert(!accepted || !invalidParser.finish(sentinel));
    assert(sentinel.present == PRINTER_PATCH_PROGRESS);
    assert(sentinel.progress == 77);
  }

  BambuReportParser oversized;
  assert(!oversized.begin(BambuReportParser::kMaximumPayloadBytes + 1));
  return 0;
}
