#pragma once

#include <stdint.h>
#include <string.h>

enum PrinterLifecycle : uint8_t {
  PRINTER_UNKNOWN   = 0,
  PRINTER_IDLE      = 1,
  PRINTER_PREPARING = 2,
  PRINTER_PRINTING  = 3,
  PRINTER_PAUSED    = 4,
  PRINTER_FINISHED  = 5,
  PRINTER_ERROR     = 6,
  PRINTER_OFFLINE   = 7,
};

struct PrinterStateSnapshot {
  bool enabled;
  bool configured;
  bool transportConnected;
  bool printerOnline;
  PrinterLifecycle lifecycle;
  PrinterLifecycle visibleLifecycle;
  uint8_t progress;
  uint16_t currentLayer;
  uint16_t totalLayers;
  uint32_t remainingSeconds;
  char jobName[96];
  char stage[48];
  char detail[96];

  PrinterStateSnapshot()
      : enabled(false), configured(false), transportConnected(false),
        printerOnline(false), lifecycle(PRINTER_UNKNOWN),
        visibleLifecycle(PRINTER_UNKNOWN), progress(0), currentLayer(0),
        totalLayers(0), remainingSeconds(0) {
    jobName[0] = '\0';
    stage[0] = '\0';
    detail[0] = '\0';
  }
};

class PrinterStatusSource {
 public:
  virtual ~PrinterStatusSource() {}
  virtual PrinterStateSnapshot snapshot() const = 0;
};

enum PrinterStatePatchField : uint16_t {
  PRINTER_PATCH_TRANSPORT_CONNECTED = 1u << 0,
  PRINTER_PATCH_PRINTER_ONLINE       = 1u << 1,
  PRINTER_PATCH_LIFECYCLE            = 1u << 2,
  PRINTER_PATCH_PROGRESS             = 1u << 3,
  PRINTER_PATCH_CURRENT_LAYER        = 1u << 4,
  PRINTER_PATCH_TOTAL_LAYERS         = 1u << 5,
  PRINTER_PATCH_REMAINING_SECONDS    = 1u << 6,
  PRINTER_PATCH_JOB_NAME             = 1u << 7,
  PRINTER_PATCH_STAGE                = 1u << 8,
  PRINTER_PATCH_DETAIL               = 1u << 9,
};

struct PrinterStatePatch {
  uint16_t present;
  bool transportConnected;
  bool printerOnline;
  PrinterLifecycle lifecycle;
  uint8_t progress;
  uint16_t currentLayer;
  uint16_t totalLayers;
  uint32_t remainingSeconds;
  char jobName[96];
  char stage[48];
  char detail[96];

  PrinterStatePatch()
      : present(0), transportConnected(false), printerOnline(false),
        lifecycle(PRINTER_UNKNOWN), progress(0), currentLayer(0),
        totalLayers(0), remainingSeconds(0) {
    jobName[0] = '\0';
    stage[0] = '\0';
    detail[0] = '\0';
  }
};

inline bool printerHasVisibleOverlay(PrinterLifecycle lifecycle) {
  return lifecycle == PRINTER_PREPARING || lifecycle == PRINTER_PRINTING ||
         lifecycle == PRINTER_PAUSED || lifecycle == PRINTER_FINISHED ||
         lifecycle == PRINTER_ERROR;
}

inline PrinterLifecycle printerVisibleLifecycleForUpdate(
    PrinterLifecycle current, PrinterLifecycle next) {
  if (printerHasVisibleOverlay(next)) return next;
  if (next == PRINTER_IDLE) return PRINTER_UNKNOWN;
  return printerHasVisibleOverlay(current) ? current : PRINTER_UNKNOWN;
}

class PrinterStatusReducer {
 public:
  static const uint32_t kFinishedGraceMs = 30000UL;

  PrinterStateSnapshot snapshot() const { return state_; }

  void configure(bool enabled, bool configured, const char* detail) {
    const bool becameAvailable = enabled && configured &&
                                 (!state_.enabled || !state_.configured);
    state_.enabled = enabled;
    state_.configured = configured;
    if (!enabled || !configured || becameAvailable) {
      resetOperationalState();
      copyText(state_.detail, detail);
    }
  }

  void apply(const PrinterStatePatch& patch, uint32_t nowMs = 0) {
    if (patch.present & PRINTER_PATCH_TRANSPORT_CONNECTED)
      state_.transportConnected = patch.transportConnected;
    if (patch.present & PRINTER_PATCH_PRINTER_ONLINE)
      state_.printerOnline = patch.printerOnline;

    const PrinterLifecycle previousVisible = state_.visibleLifecycle;
    bool deferFinishedIdle = false;
    if ((patch.present & PRINTER_PATCH_LIFECYCLE) &&
        patch.lifecycle == PRINTER_IDLE && finishedGraceActive_ &&
        static_cast<uint32_t>(nowMs - finishedSinceMs_) < kFinishedGraceMs) {
      deferFinishedIdle = true;
    }
    if ((patch.present & PRINTER_PATCH_LIFECYCLE) && !deferFinishedIdle) {
      state_.lifecycle = patch.lifecycle;
      state_.visibleLifecycle = printerVisibleLifecycleForUpdate(
          state_.visibleLifecycle, patch.lifecycle);
      if (patch.lifecycle == PRINTER_FINISHED) {
        if (!finishedGraceActive_) {
          finishedGraceActive_ = true;
          finishedSinceMs_ = nowMs;
        }
      } else if (patch.lifecycle != PRINTER_UNKNOWN &&
                 patch.lifecycle != PRINTER_OFFLINE) {
        finishedGraceActive_ = false;
      }
    }

    const bool retainingOverlay =
        (state_.lifecycle == PRINTER_UNKNOWN ||
         state_.lifecycle == PRINTER_OFFLINE) &&
        printerHasVisibleOverlay(previousVisible);
    if ((patch.present & PRINTER_PATCH_PROGRESS) && !retainingOverlay &&
        !deferFinishedIdle)
      state_.progress = patch.progress > 100 ? 100 : patch.progress;
    if (state_.lifecycle == PRINTER_FINISHED) state_.progress = 100;
    if (state_.lifecycle == PRINTER_IDLE) state_.progress = 0;
    if (!deferFinishedIdle) {
      if (patch.present & PRINTER_PATCH_CURRENT_LAYER)
        state_.currentLayer = patch.currentLayer;
      if (patch.present & PRINTER_PATCH_TOTAL_LAYERS)
        state_.totalLayers = patch.totalLayers;
      if (patch.present & PRINTER_PATCH_REMAINING_SECONDS)
        state_.remainingSeconds = patch.remainingSeconds;
      if (patch.present & PRINTER_PATCH_JOB_NAME)
        copyText(state_.jobName, patch.jobName);
      if (patch.present & PRINTER_PATCH_STAGE)
        copyText(state_.stage, patch.stage);
      if (patch.present & PRINTER_PATCH_DETAIL)
        copyText(state_.detail, patch.detail);
    }
  }

  void advance(uint32_t nowMs) {
    if (!finishedGraceActive_ || state_.lifecycle != PRINTER_FINISHED ||
        static_cast<uint32_t>(nowMs - finishedSinceMs_) < kFinishedGraceMs) {
      return;
    }
    state_.lifecycle = PRINTER_IDLE;
    state_.visibleLifecycle = PRINTER_UNKNOWN;
    state_.progress = 0;
    copyText(state_.stage, "idle");
    copyText(state_.detail, "idle");
    finishedGraceActive_ = false;
  }

  void markUnavailable(bool transportConnected, PrinterLifecycle lifecycle,
                       const char* detail) {
    PrinterStatePatch patch;
    patch.present = PRINTER_PATCH_TRANSPORT_CONNECTED |
                    PRINTER_PATCH_PRINTER_ONLINE |
                    PRINTER_PATCH_LIFECYCLE |
                    PRINTER_PATCH_DETAIL;
    patch.transportConnected = transportConnected;
    patch.printerOnline = false;
    patch.lifecycle = lifecycle;
    copyText(patch.detail, detail);
    apply(patch);
  }

 private:
  template <size_t N>
  static void copyText(char (&destination)[N], const char* source) {
    if (!source) source = "";
    strncpy(destination, source, N - 1);
    destination[N - 1] = '\0';
  }

  void resetOperationalState() {
    state_.transportConnected = false;
    state_.printerOnline = false;
    state_.lifecycle = PRINTER_UNKNOWN;
    state_.visibleLifecycle = PRINTER_UNKNOWN;
    state_.progress = 0;
    state_.currentLayer = 0;
    state_.totalLayers = 0;
    state_.remainingSeconds = 0;
    state_.jobName[0] = '\0';
    state_.stage[0] = '\0';
    finishedGraceActive_ = false;
    finishedSinceMs_ = 0;
  }

  PrinterStateSnapshot state_;
  bool finishedGraceActive_ = false;
  uint32_t finishedSinceMs_ = 0;
};
