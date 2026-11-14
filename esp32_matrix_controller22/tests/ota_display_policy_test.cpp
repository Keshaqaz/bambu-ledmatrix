#include <assert.h>

#include "../src/ota_display_policy.hpp"

static constexpr uint16_t kPixels = 16 * 16;

static void test_full_matrix_ota_frames_stay_inside_power_budget() {
  // Saturated amber uses at most two colour channels at kProgressValue.
  const uint32_t progressMilliAmps = ota_display::estimateWs2812MilliAmps(
      kPixels, 2u * ota_display::kProgressValue);
  const uint32_t doneMilliAmps = ota_display::estimateWs2812MilliAmps(
      kPixels, ota_display::kDoneGreen + ota_display::kDoneBlue);

  assert(progressMilliAmps <= ota_display::kSafeAnimationMilliAmps);
  assert(doneMilliAmps <= ota_display::kSafeAnimationMilliAmps);
}

static void test_ota_brightness_has_a_visible_floor() {
  assert(ota_display::otaGlobalBrightness(0) ==
         ota_display::kMinVisibleGlobalBrightness);
  assert(ota_display::otaGlobalBrightness(1) ==
         ota_display::kMinVisibleGlobalBrightness);
  assert(ota_display::otaGlobalBrightness(
             ota_display::kMinVisibleGlobalBrightness) ==
         ota_display::kMinVisibleGlobalBrightness);
  assert(ota_display::otaGlobalBrightness(80) == 80);

  // Conservative integer scaling: the dimmest solid OTA channel must still
  // produce at least one hardware brightness step.
  assert((ota_display::kProgressValue *
          ota_display::kMinVisibleGlobalBrightness) /
             255u >=
         1u);
}

static void test_ota_owns_the_display_until_reboot() {
  assert(ota_display::shouldRenderNormalFrame(false, false));
  assert(!ota_display::shouldRenderNormalFrame(true, false));
  assert(!ota_display::shouldRenderNormalFrame(false, true));
}

int main() {
  test_full_matrix_ota_frames_stay_inside_power_budget();
  test_ota_brightness_has_a_visible_floor();
  test_ota_owns_the_display_until_reboot();
  return 0;
}
