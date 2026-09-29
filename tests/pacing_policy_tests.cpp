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

  using mfgunlock::pacing::FixedOutputCapFrameLimitUs;
  using mfgunlock::pacing::IsValidFixedOutputCap;
  CHECK(!IsValidFixedOutputCap(0, 4));
  CHECK(!IsValidFixedOutputCap(9, 4));
  CHECK(!IsValidFixedOutputCap(240, 1));
  CHECK(!IsValidFixedOutputCap(1001, 4));
  CHECK(IsValidFixedOutputCap(10, 2));
  CHECK(IsValidFixedOutputCap(1000, 6));
  CHECK(IsValidFixedOutputCap(240, 4));
  CHECK(FixedOutputCapFrameLimitUs(200, 2) == 10000);
  CHECK(FixedOutputCapFrameLimitUs(200, 3) == 15000);
  CHECK(FixedOutputCapFrameLimitUs(240, 4) == 16667);
  CHECK(FixedOutputCapFrameLimitUs(200, 4) == 20000);
  CHECK(FixedOutputCapFrameLimitUs(200, 5) == 25000);
  CHECK(FixedOutputCapFrameLimitUs(200, 6) == 30000);
  CHECK(FixedOutputCapFrameLimitUs(175, 3) == 17143);
  CHECK(FixedOutputCapFrameLimitUs(240, 0) == 0);

  using mfgunlock::pacing::IsValidSourceFpsCap;
  using mfgunlock::pacing::ResolveSourceCapConfig;
  using mfgunlock::pacing::SourceCapConfigOrigin;
  CHECK(!IsValidSourceFpsCap(0));
  CHECK(!IsValidSourceFpsCap(9));
  CHECK(IsValidSourceFpsCap(10));
  CHECK(IsValidSourceFpsCap(120));
  CHECK(IsValidSourceFpsCap(1000));
  CHECK(!IsValidSourceFpsCap(1001));
  const auto direct_off = ResolveSourceCapConfig(
      true, 0, true, true, 165, true, 120, 4);
  CHECK(direct_off.source_fps == 0 &&
        direct_off.origin == SourceCapConfigOrigin::kConfigured);
  const auto direct = ResolveSourceCapConfig(
      true, 120, false, false, 0, false, 0, 0);
  CHECK(direct.source_fps == 120 &&
        direct.origin == SourceCapConfigOrigin::kConfigured);
  const auto invalid_direct_wins = ResolveSourceCapConfig(
      true, 1001, true, true, 165, true, 120, 4);
  CHECK(invalid_direct_wins.source_fps == 0 &&
        invalid_direct_wins.origin == SourceCapConfigOrigin::kConfigured);
  const auto legacy_dynamic = ResolveSourceCapConfig(
      false, 0, true, true, 165, true, 120, 4);
  CHECK(legacy_dynamic.source_fps == 165 &&
        legacy_dynamic.origin == SourceCapConfigOrigin::kLegacyDynamic);
  const auto legacy_fixed = ResolveSourceCapConfig(
      false, 0, false, false, 0, true, 120, 4);
  CHECK(legacy_fixed.source_fps == 30 &&
        legacy_fixed.origin == SourceCapConfigOrigin::kLegacyFixedOutput);
  const auto dormant_dynamic = ResolveSourceCapConfig(
      false, 0, false, true, 144, false, 0, 0);
  CHECK(dormant_dynamic.source_fps == 144 &&
        dormant_dynamic.origin == SourceCapConfigOrigin::kLegacyDynamic);
  CHECK(ResolveSourceCapConfig(false, 0, false, false, 0, true, 120, 0)
            .source_fps == 0);

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
  CHECK(RecommendQueueTrimSourceCapFps(10000, 3000, true) == 97);
  CHECK(RecommendQueueTrimSourceCapFps(20833, 170, true) == 0);
  CHECK(RecommendQueueTrimSourceCapFps(10000, 3000, false) == 0);

  const auto oversubscribed = BuildLatencyGuardRecommendation(
      true, 240, 100, 0, 4, 10000, 3000,
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
      true, 240, 0, 0, 4, 16667, 200,
      true);  // ~60 source / ~240 output with no queue pressure.
  CHECK(within_target.suggested_total_multiplier == 4);
  CHECK(!within_target.multiplier_may_be_higher_than_needed);
  CHECK(!within_target.source_oversubscribed);
  CHECK(!ShouldApplyAutomaticLatencyCap(
      LatencyGuardMode::kAutomatic, MarkerHealth::kHealthy, true, true,
      within_target));

  // Output oversubscription alone must never cut real FPS to refresh / MFG.
  const auto no_queue = BuildLatencyGuardRecommendation(
      true, 240, 0, 0, 4, 10000, 200, true);
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
