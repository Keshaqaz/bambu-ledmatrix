#pragma once

#include <stdint.h>

namespace ota_display {

// Values used by the full-matrix OTA frames. Keep these in a host-testable
// policy: unlike the sparse clock face, a solid 16x16 frame can overload the
// matrix supply even when each individual channel looks moderately dim.
static constexpr uint8_t kProgressValue = 32;
static constexpr uint8_t kDoneGreen = 48;
static constexpr uint8_t kDoneBlue = 16;
// FastLED applies global brightness after the per-pixel channel value. With
// the UI minimum of 1, kProgressValue scales down to zero, so the OTA frame is
// logically present but physically dark. Eight is the smallest conservative
// global value that keeps a channel value of 32 at one visible hardware step.
static constexpr uint8_t kMinVisibleGlobalBrightness = 8;
static constexpr uint16_t kSafeAnimationMilliAmps = 1500;

constexpr uint8_t otaGlobalBrightness(uint8_t currentBrightness) {
  return currentBrightness < kMinVisibleGlobalBrightness
             ? kMinVisibleGlobalBrightness
             : currentBrightness;
}

constexpr uint32_t estimateWs2812MilliAmps(uint16_t pixels,
                                           uint16_t channelSum) {
  // Conservative WS2812 estimate: 20 mA per fully lit colour channel.
  return (static_cast<uint32_t>(pixels) * channelSum * 20u + 254u) / 255u;
}

constexpr bool shouldRenderNormalFrame(bool otaInProgress,
                                       bool rebootPending) {
  return !otaInProgress && !rebootPending;
}

}  // namespace ota_display
