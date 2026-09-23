#include <cstdlib>
#include <iostream>

#include "../src/addons/mfgunlock/pacing_policy.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition      \
                << '\n';                                                        \
      return EXIT_FAILURE;                                                      \
    }                                                                           \
  } while (false)

int main() {
  using mfgunlock::pacing::IsReady;

  // Regression: 310.9.1 uses native pacing even though its old metering field
  // no longer has a patchable store. A missing legacy patch must not block MFG.
  CHECK(IsReady(false, false));
  CHECK(IsReady(false, true));
  CHECK(!IsReady(true, false));
  CHECK(IsReady(true, true));

  using mfgunlock::pacing::TargetFpsToFrameLimitUs;
  CHECK(TargetFpsToFrameLimitUs(0) == 0);
  CHECK(TargetFpsToFrameLimitUs(60) == 16667);
  CHECK(TargetFpsToFrameLimitUs(100) == 10000);
  CHECK(TargetFpsToFrameLimitUs(120) == 8333);
  CHECK(TargetFpsToFrameLimitUs(144) == 6944);

  using mfgunlock::pacing::ShouldApplyReflexSourceCap;
  CHECK(!ShouldApplyReflexSourceCap(true, true, true, true, true, false, 100));
  CHECK(ShouldApplyReflexSourceCap(true, true, true, true, true, true, 100));
  CHECK(!ShouldApplyReflexSourceCap(true, true, true, true, true, true, 0));
  CHECK(!ShouldApplyReflexSourceCap(true, false, true, true, true, true, 100));
  CHECK(!ShouldApplyReflexSourceCap(true, true, true, false, true, true, 100));

  using mfgunlock::pacing::BuildLatencyGuardRecommendation;
  using mfgunlock::pacing::FrameLimitUsToFps;
  using mfgunlock::pacing::LatencyGuardMode;
  using mfgunlock::pacing::MarkerHealth;
  using mfgunlock::pacing::PreserveStricterNativeLimit;
  using mfgunlock::pacing::RecommendQueueTrimSourceCapFps;
  using mfgunlock::pacing::ResolveOutputTargetFps;
  using mfgunlock::pacing::ShouldApplyAutomaticLatencyCap;
  using mfgunlock::pacing::ShouldTrialLowerMultiplier;

  CHECK(FrameLimitUsToFps(0) == 0);
  CHECK(FrameLimitUsToFps(16667) == 60);
  CHECK(ResolveOutputTargetFps(true, 240, 100, 10000) == 240);
  CHECK(ResolveOutputTargetFps(false, 240, 100, 16667) == 100);
  CHECK(ResolveOutputTargetFps(false, 240, 0, 8333) == 120);
  CHECK(RecommendQueueTrimSourceCapFps(10000, 3000, 10000, true) == 97);
  CHECK(RecommendQueueTrimSourceCapFps(20833, 170, 20833, true) == 0);
  CHECK(RecommendQueueTrimSourceCapFps(10000, 3000, 10000, false) == 0);

  const auto oversubscribed = BuildLatencyGuardRecommendation(
      true, 240, 100, 0, 4, 10000, 3000, 10000,
      true);  // ~100 source / ~400 output with a meaningful queue.
  CHECK(oversubscribed.output_target_fps == 240);
  CHECK(oversubscribed.source_cap_fps == 97);
  CHECK(oversubscribed.estimated_source_fps == 100);
  CHECK(oversubscribed.projected_output_fps == 400);
  CHECK(oversubscribed.suggested_total_multiplier == 3);
  CHECK(oversubscribed.multiplier_may_be_higher_than_needed);
  CHECK(oversubscribed.target_from_vsync);
  CHECK(oversubscribed.data_complete);
  CHECK(oversubscribed.source_oversubscribed);
  CHECK(ShouldApplyAutomaticLatencyCap(
      LatencyGuardMode::kAutomatic, MarkerHealth::kHealthy, true, true,
      oversubscribed));
  CHECK(!ShouldApplyAutomaticLatencyCap(
      LatencyGuardMode::kMonitor, MarkerHealth::kHealthy, true, true,
      oversubscribed));
  CHECK(!ShouldApplyAutomaticLatencyCap(
      LatencyGuardMode::kAutomatic, MarkerHealth::kMissingSleep, true, true,
      oversubscribed));

  const auto within_target = BuildLatencyGuardRecommendation(
      true, 240, 0, 0, 4, 16667, 200, 16667,
      true);  // ~60 source / ~240 output with no queue pressure.
  CHECK(within_target.suggested_total_multiplier == 4);
  CHECK(!within_target.multiplier_may_be_higher_than_needed);
  CHECK(!within_target.source_oversubscribed);
  CHECK(!ShouldApplyAutomaticLatencyCap(
      LatencyGuardMode::kAutomatic, MarkerHealth::kHealthy, true, true,
      within_target));

  // Output oversubscription alone must never cut real FPS to refresh / MFG.
  const auto no_queue = BuildLatencyGuardRecommendation(
      true, 240, 0, 0, 4, 10000, 200, 10000, true);
  CHECK(no_queue.source_oversubscribed);
  CHECK(no_queue.source_cap_fps == 0);
  CHECK(!ShouldApplyAutomaticLatencyCap(
      LatencyGuardMode::kAutomatic, MarkerHealth::kHealthy, true, true,
      no_queue));
  CHECK(ShouldTrialLowerMultiplier(
      LatencyGuardMode::kAutomatic, MarkerHealth::kHealthy, 4, 3, false,
      true, true));
  CHECK(!ShouldTrialLowerMultiplier(
      LatencyGuardMode::kMonitor, MarkerHealth::kHealthy, 4, 3, false,
      true, true));
  CHECK(!ShouldTrialLowerMultiplier(
      LatencyGuardMode::kAutomatic, MarkerHealth::kHealthy, 4, 3, true,
      true, true));

  CHECK(PreserveStricterNativeLimit(0, 17241) == 17241);
  CHECK(PreserveStricterNativeLimit(20000, 17241) == 20000);
  CHECK(PreserveStricterNativeLimit(10000, 17241) == 17241);

  std::cout << "pacing policy tests passed\n";
  return EXIT_SUCCESS;
}
