/*
 * Presentation-pacing policy shared by the addon and its regression test.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>

namespace mfgunlock::pacing {

enum class LatencyGuardMode : uint32_t {
  kOff = 0,
  kMonitor = 1,
  kAutomatic = 2,
};

enum class MarkerHealth : uint32_t {
  kUnavailable = 0,
  kWaiting = 1,
  kHealthy = 2,
  kMissingSleep = 3,
  kIncomplete = 4,
  kInvalidOrder = 5,
  kUnstableTiming = 6,
};

struct LatencyGuardRecommendation {
  uint32_t output_target_fps = 0;
  uint32_t source_cap_fps = 0;
  uint32_t estimated_source_fps = 0;
  uint32_t projected_output_fps = 0;
  uint32_t suggested_total_multiplier = 0;
  bool target_from_vsync = false;
  bool data_complete = false;
  bool source_oversubscribed = false;
  bool sustained_queue_pressure = false;
  bool multiplier_may_be_higher_than_needed = false;
};

// Modern Streamline pacing is the native/default path and needs no memory
// patch. Only an explicit request for legacy software-flip compatibility makes
// the old metering-field patch a prerequisite for raising the multiplier.
inline constexpr bool IsReady(bool legacy_software_flip_requested,
                              bool legacy_patch_applied) {
  return !legacy_software_flip_requested || legacy_patch_applied;
}

// Reflex expresses its limiter as an integer frame interval in microseconds.
// Round to nearest instead of truncating so common targets (60/100/120/144)
// do not acquire a systematic high-FPS bias.
inline constexpr uint32_t TargetFpsToFrameLimitUs(uint32_t target_fps) {
  if (target_fps == 0) return 0;
  return static_cast<uint32_t>((1000000ull + target_fps / 2u) / target_fps);
}

inline constexpr uint32_t FrameLimitUsToFps(uint32_t frame_limit_us) {
  if (frame_limit_us == 0) return 0;
  return static_cast<uint32_t>((1000000ull + frame_limit_us / 2u) /
                               frame_limit_us);
}

// Resolve the final-output target without pretending that DynamicTargetFPS is
// authoritative under VSync. Streamline 2.14.1 follows the active display in
// that case. The driver-observed target is a fallback for integrations where
// the display refresh cannot be queried reliably.
inline constexpr uint32_t ResolveOutputTargetFps(
    bool vsync_active, uint32_t display_refresh_fps,
    uint32_t configured_dynamic_target_fps,
    uint32_t driver_dynamic_target_us) {
  if (vsync_active && display_refresh_fps != 0) return display_refresh_fps;
  if (!vsync_active && configured_dynamic_target_fps != 0)
    return configured_dynamic_target_fps;
  if (driver_dynamic_target_us != 0)
    return FrameLimitUsToFps(driver_dynamic_target_us);
  return display_refresh_fps;
}

// A latency-first limiter must not divide the display refresh by the generated
// multiplier. Doing so trades away real frames (and therefore input response)
// merely to make all generated frames fit below VSync. Instead, when a robust
// Reflex window proves that the render queue is accumulating, trim only a
// small amount from the sustainable real-frame rate. The native Reflex sleep
// point remains responsible for the actual just-in-time scheduling.
inline constexpr uint32_t RecommendQueueTrimSourceCapFps(
    uint32_t observed_source_interval_us, uint32_t queue_wait_us,
    uint32_t gpu_frame_time_us, bool source_timing_confident,
    uint32_t trim_percent = 3) {
  if (!source_timing_confident || observed_source_interval_us == 0 ||
      queue_wait_us < 1500 || gpu_frame_time_us == 0 || trim_percent == 0 ||
      trim_percent >= 10)
    return 0;
  // Require queueing to account for at least 25% of a real-frame interval.
  // Tiny queues are normal and do not justify reducing real-frame throughput.
  if (static_cast<uint64_t>(queue_wait_us) * 4ull < gpu_frame_time_us) return 0;
  const uint32_t source_fps = FrameLimitUsToFps(observed_source_interval_us);
  if (source_fps < 30 || source_fps > 1000) return 0;
  const uint32_t cap = static_cast<uint32_t>(
      (static_cast<uint64_t>(source_fps) * (100u - trim_percent) + 99ull) / 100ull);
  // Require a meaningful but bounded change. One-FPS adjustments mostly add
  // limiter churn, while the percentage bound prevents the old 58-FPS cliff.
  return source_fps > cap && source_fps - cap >= 2 ? cap : 0;
}

inline constexpr LatencyGuardRecommendation BuildLatencyGuardRecommendation(
    bool vsync_active, uint32_t display_refresh_fps,
    uint32_t configured_dynamic_target_fps,
    uint32_t driver_dynamic_target_us, uint32_t total_multiplier,
    uint32_t observed_source_interval_us, uint32_t queue_wait_us,
    uint32_t gpu_frame_time_us, bool source_timing_confident) {
  LatencyGuardRecommendation result{};
  result.output_target_fps = ResolveOutputTargetFps(
      vsync_active, display_refresh_fps, configured_dynamic_target_fps,
      driver_dynamic_target_us);
  result.target_from_vsync = vsync_active && display_refresh_fps != 0;
  const uint32_t source_fps = FrameLimitUsToFps(observed_source_interval_us);
  result.estimated_source_fps = source_fps;
  if (source_fps != 0 && total_multiplier >= 2) {
    result.projected_output_fps = source_fps * total_multiplier;
    if (result.output_target_fps != 0) {
      const uint32_t required = static_cast<uint32_t>(
          (static_cast<uint64_t>(result.output_target_fps) + source_fps - 1u) /
          source_fps);
      if (required >= 2 && required <= 6) {
        result.suggested_total_multiplier = required;
        result.multiplier_may_be_higher_than_needed =
            total_multiplier > required;
      }
    }
  }
  result.sustained_queue_pressure =
      source_timing_confident && queue_wait_us >= 1500 &&
      gpu_frame_time_us != 0 &&
      static_cast<uint64_t>(queue_wait_us) * 4ull >= gpu_frame_time_us;
  result.source_cap_fps = RecommendQueueTrimSourceCapFps(
      observed_source_interval_us, queue_wait_us, gpu_frame_time_us,
      source_timing_confident);
  result.data_complete = source_timing_confident && source_fps != 0 &&
                         total_multiplier >= 2;
  result.source_oversubscribed =
      result.projected_output_fps != 0 && result.output_target_fps != 0 &&
      static_cast<uint64_t>(result.projected_output_fps) * 100ull >
          static_cast<uint64_t>(result.output_target_fps) * 102ull;
  return result;
}

// A native game cap is never relaxed. The automatic guard may only keep it or
// make it stricter, while the old explicit source-cap option retains its exact
// opt-in behavior for backward compatibility.
inline constexpr uint32_t PreserveStricterNativeLimit(
    uint32_t native_limit_us, uint32_t guard_limit_us) {
  if (guard_limit_us == 0) return native_limit_us;
  if (native_limit_us == 0) return guard_limit_us;
  return native_limit_us > guard_limit_us ? native_limit_us : guard_limit_us;
}

inline constexpr bool ShouldApplyAutomaticLatencyCap(
    LatencyGuardMode mode, MarkerHealth marker_health, bool reflex_hooked,
    bool reflex_options_seen, const LatencyGuardRecommendation& recommendation,
    bool sustained_queue_pressure = false) {
  return mode == LatencyGuardMode::kAutomatic &&
         marker_health == MarkerHealth::kHealthy && reflex_hooked &&
         reflex_options_seen && recommendation.data_complete &&
         recommendation.source_cap_fps != 0 &&
         (recommendation.sustained_queue_pressure || sustained_queue_pressure);
}

inline constexpr bool ShouldApplyReflexSourceCap(
    bool dynamic_enabled, bool d3d12, bool support_seen, bool supported,
    bool dynamic_applied, bool explicit_source_cap, uint32_t target_fps) {
  return dynamic_enabled && d3d12 && support_seen && supported &&
         dynamic_applied && explicit_source_cap && target_fps != 0;
}

}  // namespace mfgunlock::pacing
