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
  assert(errorPatch.lifecycle == PRINTER_ERROR);
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
