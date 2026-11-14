#pragma once

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "printer_status_source.hpp"

inline bool bambuAsciiEqualIgnoreCase(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    if (toupper(static_cast<unsigned char>(*a++)) !=
        toupper(static_cast<unsigned char>(*b++))) return false;
  }
  return *a == '\0' && *b == '\0';
}

// Ошибка на матрице = принтер сам остановился (FAILED или PAUSE с
// hms/print_error). Bambu держит в hms и print_error информационные
// уведомления (влажность AMS, обслуживание, проверка первого слоя), которые
// принтер не считает ошибкой; при RUNNING/IDLE/FINISH они игнорируются,
// иначе дельта-репорты с hms давали короткие ложные красные вспышки.
inline PrinterLifecycle bambuLifecycleFromReport(const char* gcodeState,
                                                 bool hasError) {
  if (bambuAsciiEqualIgnoreCase(gcodeState, "RUNNING")) return PRINTER_PRINTING;
  if (bambuAsciiEqualIgnoreCase(gcodeState, "PREPARE")) return PRINTER_PREPARING;
  if (bambuAsciiEqualIgnoreCase(gcodeState, "PAUSE") ||
      bambuAsciiEqualIgnoreCase(gcodeState, "PAUSED"))
    return hasError ? PRINTER_ERROR : PRINTER_PAUSED;
  if (bambuAsciiEqualIgnoreCase(gcodeState, "FINISH")) return PRINTER_FINISHED;
  if (bambuAsciiEqualIgnoreCase(gcodeState, "IDLE")) return PRINTER_IDLE;
  if (bambuAsciiEqualIgnoreCase(gcodeState, "FAILED")) return PRINTER_ERROR;
  return PRINTER_UNKNOWN;
}

inline const char* bambuStageLabel(long stage) {
  switch (stage) {
    case 0: return "printing";
    case 1: return "auto_bed_leveling";
    case 2: return "heatbed_preheating";
    case 4: return "changing_filament";
    case 5: return "m400_pause";
    case 6: return "paused_filament_runout";
    case 7: return "heating_hotend";
    case 10: return "inspecting_first_layer";
    case 16: return "paused_user";
    case 22: return "filament_unloading";
    case 24: return "filament_loading";
    case 29: return "cooling_chamber";
    case 35: return "paused_nozzle_clog";
    case 49: return "heating_chamber";
    case -1:
    case 255: return "idle";
    default: return "";
  }
}

class BambuReportParser {
 public:
  static constexpr size_t kMaximumPayloadBytes = 16384;
  static constexpr size_t kMaximumDepth = 10;

  BambuReportParser() { reset(); }

  bool begin(size_t totalLength) {
    reset();
    expectedLength_ = totalLength;
    if (totalLength == 0 || totalLength > kMaximumPayloadBytes) {
      failed_ = true;
      return false;
    }
    started_ = true;
    return true;
  }

  bool feed(const char* data, size_t length, size_t offset,
            size_t totalLength) {
    if (!started_ || failed_ || !data || totalLength != expectedLength_ ||
        offset != receivedLength_ || length > expectedLength_ - receivedLength_) {
      failed_ = true;
      return false;
    }
    for (size_t i = 0; i < length; ++i) {
      if (!consume(data[i])) {
        failed_ = true;
        return false;
      }
    }
    receivedLength_ += length;
    return true;
  }

  bool finish(PrinterStatePatch& output) {
    if (!started_ || failed_ || receivedLength_ != expectedLength_) return false;
    if (lex_ == LEX_NUMBER || lex_ == LEX_LITERAL) {
      if (!finishScalar()) return false;
    }
    if (lex_ != LEX_NORMAL || depth_ != 0 || !rootComplete_) return false;
    finalizePatch();
    if (!meaningful_) return false;
    output = patch_;
    return true;
  }

 private:
  enum LexState : uint8_t {
    LEX_NORMAL,
    LEX_STRING,
    LEX_ESCAPE,
    LEX_UNICODE,
    LEX_NUMBER,
    LEX_LITERAL,
  };
  enum Expect : uint8_t {
    OBJECT_KEY_OR_END,
    OBJECT_KEY,
    OBJECT_COLON,
    OBJECT_VALUE,
    OBJECT_COMMA_OR_END,
    ARRAY_VALUE_OR_END,
    ARRAY_VALUE,
    ARRAY_COMMA_OR_END,
  };
  struct Frame {
    char type;
    Expect expect;
    char key[32];
  };

  Frame stack_[kMaximumDepth];
  size_t depth_;
  size_t printDepth_;
  size_t hmsDepth_;
  size_t stageDepth_;
  size_t expectedLength_;
  size_t receivedLength_;
  bool started_;
  bool failed_;
  bool rootComplete_;
  bool tokenIsKey_;
  bool tokenOverflow_;
  bool meaningful_;
  bool hasError_;
  bool gcodeSeen_;
  bool explicitStage_;
  LexState lex_;
  char token_[128];
  size_t tokenLength_;
  uint16_t unicodeValue_;
  uint8_t unicodeDigits_;
  char gcodeState_[24];
  PrinterStatePatch patch_;

  void reset() {
    memset(stack_, 0, sizeof(stack_));
    depth_ = printDepth_ = hmsDepth_ = stageDepth_ = 0;
    expectedLength_ = receivedLength_ = 0;
    started_ = failed_ = rootComplete_ = tokenIsKey_ = false;
    tokenOverflow_ = meaningful_ = hasError_ = gcodeSeen_ = false;
    explicitStage_ = false;
    lex_ = LEX_NORMAL;
    token_[0] = '\0';
    tokenLength_ = 0;
    unicodeValue_ = 0;
    unicodeDigits_ = 0;
    gcodeState_[0] = '\0';
    patch_ = PrinterStatePatch();
  }

  Frame* top() { return depth_ ? &stack_[depth_ - 1] : nullptr; }

  bool expectingValue() const {
    if (depth_ == 0) return !rootComplete_;
    const Frame& frame = stack_[depth_ - 1];
    return frame.expect == OBJECT_VALUE || frame.expect == ARRAY_VALUE_OR_END ||
           frame.expect == ARRAY_VALUE;
  }

  const char* valueKey() const {
    if (depth_ == 0 || stack_[depth_ - 1].type != '{') return "";
    return stack_[depth_ - 1].key;
  }

  bool inReportObject() const {
    return depth_ == 1 || (printDepth_ != 0 && depth_ == printDepth_);
  }

  void markValueStarted() {
    if (depth_ == 0) return;
    Frame& frame = stack_[depth_ - 1];
    frame.expect = frame.type == '{' ? OBJECT_COMMA_OR_END : ARRAY_COMMA_OR_END;
  }

  bool push(char type, const char* parentKey) {
    if (depth_ >= kMaximumDepth) return false;
    if (hmsDepth_ && depth_ >= hmsDepth_) {
      hasError_ = true;
      meaningful_ = true;
    }
    markValueStarted();
    Frame& frame = stack_[depth_++];
    memset(&frame, 0, sizeof(frame));
    frame.type = type;
    frame.expect = type == '{' ? OBJECT_KEY_OR_END : ARRAY_VALUE_OR_END;
    if (parentKey && strcmp(parentKey, "print") == 0 && type == '{')
      printDepth_ = depth_;
    if (parentKey && strcmp(parentKey, "hms") == 0 && type == '[' &&
        (depth_ - 1 == 1 || (printDepth_ && depth_ - 1 == printDepth_)))
      hmsDepth_ = depth_;
    if (parentKey && strcmp(parentKey, "stage") == 0 && type == '{' &&
        (depth_ - 1 == 1 || (printDepth_ && depth_ - 1 == printDepth_)))
      stageDepth_ = depth_;
    return true;
  }

  bool pop(char type) {
    if (!depth_ || stack_[depth_ - 1].type != type) return false;
    Frame& frame = stack_[depth_ - 1];
    const bool validEnd = type == '{'
        ? (frame.expect == OBJECT_KEY_OR_END || frame.expect == OBJECT_COMMA_OR_END)
        : (frame.expect == ARRAY_VALUE_OR_END || frame.expect == ARRAY_COMMA_OR_END);
    if (!validEnd) return false;
    const size_t closingDepth = depth_;
    --depth_;
    if (printDepth_ == closingDepth) printDepth_ = 0;
    if (hmsDepth_ == closingDepth) hmsDepth_ = 0;
    if (stageDepth_ == closingDepth) stageDepth_ = 0;
    if (depth_ == 0) rootComplete_ = true;
    return true;
  }

  bool beginToken(LexState state, bool isKey) {
    tokenLength_ = 0;
    token_[0] = '\0';
    tokenOverflow_ = false;
    tokenIsKey_ = isKey;
    lex_ = state;
    return true;
  }

  void appendToken(char value) {
    if (tokenLength_ + 1 < sizeof(token_)) {
      token_[tokenLength_++] = value;
      token_[tokenLength_] = '\0';
    } else {
      tokenOverflow_ = true;
    }
  }

  bool scalarAllowed() const {
    if (depth_ == 0) return !rootComplete_;
    const Frame& frame = stack_[depth_ - 1];
    return frame.expect == OBJECT_VALUE || frame.expect == ARRAY_VALUE_OR_END ||
           frame.expect == ARRAY_VALUE;
  }

  bool consume(char value) {
    if (lex_ == LEX_STRING) {
      if (value == '"') return finishString();
      if (value == '\\') { lex_ = LEX_ESCAPE; return true; }
      if (static_cast<unsigned char>(value) < 0x20) return false;
      appendToken(value);
      return true;
    }
    if (lex_ == LEX_ESCAPE) {
      switch (value) {
        case '"': case '\\': case '/': appendToken(value); lex_ = LEX_STRING; return true;
        case 'b': appendToken('\b'); lex_ = LEX_STRING; return true;
        case 'f': appendToken('\f'); lex_ = LEX_STRING; return true;
        case 'n': appendToken('\n'); lex_ = LEX_STRING; return true;
        case 'r': appendToken('\r'); lex_ = LEX_STRING; return true;
        case 't': appendToken('\t'); lex_ = LEX_STRING; return true;
        case 'u': unicodeValue_ = 0; unicodeDigits_ = 0; lex_ = LEX_UNICODE; return true;
        default: return false;
      }
    }
    if (lex_ == LEX_UNICODE) {
      uint8_t digit;
      if (value >= '0' && value <= '9') digit = value - '0';
      else if (value >= 'a' && value <= 'f') digit = value - 'a' + 10;
      else if (value >= 'A' && value <= 'F') digit = value - 'A' + 10;
      else return false;
      unicodeValue_ = static_cast<uint16_t>((unicodeValue_ << 4) | digit);
      if (++unicodeDigits_ == 4) {
        appendToken(unicodeValue_ <= 0x7f ? static_cast<char>(unicodeValue_) : '?');
        lex_ = LEX_STRING;
      }
      return true;
    }
    if (lex_ == LEX_NUMBER) {
      if (isdigit(static_cast<unsigned char>(value)) || value == '-' || value == '+' ||
          value == '.' || value == 'e' || value == 'E') {
        appendToken(value);
        return true;
      }
      if (!finishScalar()) return false;
      return consume(value);
    }
    if (lex_ == LEX_LITERAL) {
      if (isalpha(static_cast<unsigned char>(value))) {
        appendToken(value);
        return true;
      }
      if (!finishScalar()) return false;
      return consume(value);
    }

    if (isspace(static_cast<unsigned char>(value))) return true;
    if (rootComplete_) return false;
    Frame* frame = top();
    if (value == '{' || value == '[') {
      if (!expectingValue()) return false;
      const char key[32] = {0};
      char parentKey[32] = {0};
      if (frame && frame->type == '{') strncpy(parentKey, frame->key, sizeof(parentKey) - 1);
      (void)key;
      return push(value, parentKey);
    }
    if (value == '}') return pop('{');
    if (value == ']') return pop('[');
    if (value == ':') {
      if (!frame || frame->type != '{' || frame->expect != OBJECT_COLON) return false;
      frame->expect = OBJECT_VALUE;
      return true;
    }
    if (value == ',') {
      if (!frame) return false;
      if (frame->type == '{' && frame->expect == OBJECT_COMMA_OR_END)
        frame->expect = OBJECT_KEY;
      else if (frame->type == '[' && frame->expect == ARRAY_COMMA_OR_END)
        frame->expect = ARRAY_VALUE;
      else return false;
      return true;
    }
    if (value == '"') {
      const bool isKey = frame && frame->type == '{' &&
                         (frame->expect == OBJECT_KEY_OR_END ||
                          frame->expect == OBJECT_KEY);
      if (!isKey && !scalarAllowed()) return false;
      return beginToken(LEX_STRING, isKey);
    }
    if (value == '-' || isdigit(static_cast<unsigned char>(value))) {
      if (!scalarAllowed()) return false;
      beginToken(LEX_NUMBER, false);
      appendToken(value);
      return true;
    }
    if (value == 't' || value == 'f' || value == 'n') {
      if (!scalarAllowed()) return false;
      beginToken(LEX_LITERAL, false);
      appendToken(value);
      return true;
    }
    return false;
  }

  bool finishString() {
    if (tokenIsKey_) {
      Frame* frame = top();
      if (!frame || frame->type != '{' ||
          (frame->expect != OBJECT_KEY_OR_END && frame->expect != OBJECT_KEY))
        return false;
      if (tokenOverflow_) frame->key[0] = '\0';
      else {
        strncpy(frame->key, token_, sizeof(frame->key) - 1);
        frame->key[sizeof(frame->key) - 1] = '\0';
      }
      frame->expect = OBJECT_COLON;
    } else {
      if (!scalarAllowed()) return false;
      applyScalar(valueKey(), token_, true);
      markValueStarted();
      if (depth_ == 0) rootComplete_ = true;
    }
    lex_ = LEX_NORMAL;
    return true;
  }

  bool finishScalar() {
    if (tokenOverflow_ || !scalarAllowed()) return false;
    if (lex_ == LEX_LITERAL && strcmp(token_, "true") != 0 &&
        strcmp(token_, "false") != 0 && strcmp(token_, "null") != 0)
      return false;
    if (lex_ == LEX_NUMBER) {
      if (!validJsonNumber(token_)) return false;
    }
    applyScalar(valueKey(), token_, false);
    markValueStarted();
    if (depth_ == 0) rootComplete_ = true;
    lex_ = LEX_NORMAL;
    return true;
  }

  template <size_t N>
  static void copyText(char (&destination)[N], const char* source) {
    if (!source) source = "";
    strncpy(destination, source, N - 1);
    destination[N - 1] = '\0';
  }

  static bool validJsonNumber(const char* value) {
    if (!value || !value[0]) return false;
    const char* cursor = value;
    if (*cursor == '-') ++cursor;
    if (*cursor == '0') {
      ++cursor;
      if (isdigit(static_cast<unsigned char>(*cursor))) return false;
    } else {
      if (*cursor < '1' || *cursor > '9') return false;
      while (isdigit(static_cast<unsigned char>(*++cursor))) {}
    }
    if (*cursor == '.') {
      ++cursor;
      if (!isdigit(static_cast<unsigned char>(*cursor))) return false;
      while (isdigit(static_cast<unsigned char>(*cursor))) ++cursor;
    }
    if (*cursor == 'e' || *cursor == 'E') {
      ++cursor;
      if (*cursor == '+' || *cursor == '-') ++cursor;
      if (!isdigit(static_cast<unsigned char>(*cursor))) return false;
      while (isdigit(static_cast<unsigned char>(*cursor))) ++cursor;
    }
    return *cursor == '\0';
  }

  static bool numericScalar(const char* value, bool isString) {
    return !isString || validJsonNumber(value);
  }

  void applyScalar(const char* key, const char* value, bool isString) {
    if (hmsDepth_ && depth_ >= hmsDepth_) {
      hasError_ = true;
      meaningful_ = true;
    }
    if (stageDepth_ && depth_ == stageDepth_) {
      if ((strcmp(key, "name") == 0 || strcmp(key, "stage") == 0) && isString) {
        copyText(patch_.stage, value);
        explicitStage_ = true;
        patch_.present |= PRINTER_PATCH_STAGE;
        meaningful_ = true;
      } else if (strcmp(key, "_id") == 0 && numericScalar(value, isString)) {
        const char* label = bambuStageLabel(strtol(value, nullptr, 10));
        if (label[0]) {
          copyText(patch_.stage, label);
          patch_.present |= PRINTER_PATCH_STAGE;
        }
        meaningful_ = true;
      }
      return;
    }
    if (!inReportObject() || !key || !key[0]) return;
    if (strcmp(key, "gcode_state") == 0 && isString) {
      copyText(gcodeState_, value); gcodeSeen_ = meaningful_ = true;
    } else if (strcmp(key, "mc_percent") == 0 && numericScalar(value, isString)) {
      double progress = strtod(value, nullptr);
      if (progress >= 0.0 && progress < 1.0) progress *= 100.0;
      if (progress < 0.0) progress = 0.0;
      if (progress > 100.0) progress = 100.0;
      patch_.progress = static_cast<uint8_t>(lround(progress));
      patch_.present |= PRINTER_PATCH_PROGRESS; meaningful_ = true;
    } else if (strcmp(key, "subtask_name") == 0 && isString) {
      // Relay-паритет: имя работы даёт subtask_name; пустое значение не
      // затирает ранее полученное.
      if (value[0]) {
        copyText(patch_.jobName, value);
        patch_.present |= PRINTER_PATCH_JOB_NAME;
      }
      meaningful_ = true;
    } else if (strcmp(key, "gcode_file") == 0 && isString) {
      // gcode_file — только fallback, если subtask_name не дал имени
      // (живой репорт 2026-09-04: gcode_file="" затирал непустой subtask_name).
      if (value[0] && !(patch_.present & PRINTER_PATCH_JOB_NAME)) {
        copyText(patch_.jobName, value);
        patch_.present |= PRINTER_PATCH_JOB_NAME;
      }
      meaningful_ = true;
    } else if (strcmp(key, "stage") == 0 && isString) {
      copyText(patch_.stage, value); explicitStage_ = true;
      patch_.present |= PRINTER_PATCH_STAGE; meaningful_ = true;
    } else if (strcmp(key, "stg_cur") == 0 && numericScalar(value, isString)) {
      const char* label = bambuStageLabel(strtol(value, nullptr, 10));
      if (label[0]) {
        copyText(patch_.stage, label);
        patch_.present |= PRINTER_PATCH_STAGE;
      }
      meaningful_ = true;
    } else if (strcmp(key, "layer_num") == 0 && numericScalar(value, isString)) {
      long number = strtol(value, nullptr, 10);
      patch_.currentLayer = static_cast<uint16_t>(number < 0 ? 0 : (number > 65535 ? 65535 : number));
      patch_.present |= PRINTER_PATCH_CURRENT_LAYER; meaningful_ = true;
    } else if (strcmp(key, "total_layer_num") == 0 && numericScalar(value, isString)) {
      long number = strtol(value, nullptr, 10);
      patch_.totalLayers = static_cast<uint16_t>(number < 0 ? 0 : (number > 65535 ? 65535 : number));
      patch_.present |= PRINTER_PATCH_TOTAL_LAYERS; meaningful_ = true;
    } else if ((strcmp(key, "mc_remaining_time") == 0 ||
                strcmp(key, "remaining_minutes") == 0) &&
               numericScalar(value, isString)) {
      long minutes = strtol(value, nullptr, 10);
      patch_.remainingSeconds = minutes <= 0 ? 0 :
          (minutes > 71582788L ? UINT32_MAX : static_cast<uint32_t>(minutes) * 60UL);
      patch_.present |= PRINTER_PATCH_REMAINING_SECONDS; meaningful_ = true;
    } else if ((strcmp(key, "remaining_time") == 0 ||
                strcmp(key, "remaining_seconds") == 0) &&
               numericScalar(value, isString)) {
      long seconds = strtol(value, nullptr, 10);
      patch_.remainingSeconds = seconds < 0 ? 0 : static_cast<uint32_t>(seconds);
      patch_.present |= PRINTER_PATCH_REMAINING_SECONDS; meaningful_ = true;
    } else if (strcmp(key, "print_error") == 0) {
      if ((isString && value[0] && strcmp(value, "0") != 0) ||
          (!isString && strtol(value, nullptr, 10) != 0)) hasError_ = true;
      meaningful_ = true;
    }
  }

  void finalizePatch() {
    PrinterLifecycle lifecycle = bambuLifecycleFromReport(gcodeState_, hasError_);
    if (lifecycle != PRINTER_UNKNOWN) {
      patch_.lifecycle = lifecycle;
      patch_.terminalFailure =
          bambuAsciiEqualIgnoreCase(gcodeState_, "FAILED");
      patch_.present |= PRINTER_PATCH_LIFECYCLE;
    }
    const bool numericIdleDuringActivity =
        !explicitStage_ && (patch_.present & PRINTER_PATCH_STAGE) &&
        strcmp(patch_.stage, "idle") == 0 && lifecycle != PRINTER_IDLE &&
        lifecycle != PRINTER_UNKNOWN;
    if (!explicitStage_ &&
        (!(patch_.present & PRINTER_PATCH_STAGE) || numericIdleDuringActivity)) {
      const char* stage = "";
      switch (lifecycle) {
        case PRINTER_PREPARING: stage = "preparing"; break;
        case PRINTER_PRINTING: stage = "printing"; break;
        case PRINTER_PAUSED: stage = "paused"; break;
        case PRINTER_FINISHED: stage = "finished"; break;
        case PRINTER_ERROR: stage = "error"; break;
        case PRINTER_IDLE: stage = "idle"; break;
        default: break;
      }
      if (stage[0]) {
        copyText(patch_.stage, stage);
        patch_.present |= PRINTER_PATCH_STAGE;
      }
    }
    if (lifecycle == PRINTER_FINISHED &&
        (patch_.present & PRINTER_PATCH_PROGRESS) && patch_.progress < 100)
      patch_.progress = 100;
    patch_.transportConnected = true;
    patch_.printerOnline = true;
    patch_.present |= PRINTER_PATCH_TRANSPORT_CONNECTED |
                      PRINTER_PATCH_PRINTER_ONLINE;
    const char* detail = patch_.jobName[0] ? patch_.jobName :
                         (patch_.stage[0] ? patch_.stage :
                          (gcodeSeen_ ? gcodeState_ : "MQTT report"));
    copyText(patch_.detail, detail);
    patch_.present |= PRINTER_PATCH_DETAIL;
  }
};
