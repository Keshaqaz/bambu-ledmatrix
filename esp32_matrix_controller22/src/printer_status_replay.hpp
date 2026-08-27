#pragma once

#include "printer_status_source.hpp"

class ScriptedPrinterStatusSource : public PrinterStatusSource {
 public:
  PrinterStateSnapshot snapshot() const override { return reducer_.snapshot(); }

  void configure(bool enabled, bool configured, const char* detail) {
    reducer_.configure(enabled, configured, detail);
  }

  void replay(const PrinterStatePatch& patch) { reducer_.apply(patch, nowMs_); }

  void advanceBy(uint32_t elapsedMs) {
    nowMs_ += elapsedMs;
    reducer_.advance(nowMs_);
  }

  void replayMalformed(const char* detail) {
    reducer_.markUnavailable(false, PRINTER_UNKNOWN, detail);
  }

  void replayStale(const char* detail) {
    reducer_.markUnavailable(true, PRINTER_OFFLINE, detail);
  }

 private:
  PrinterStatusReducer reducer_;
  uint32_t nowMs_ = 0;
};
