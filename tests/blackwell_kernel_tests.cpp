#include <cstdlib>
#include <iostream>
#include <string>

#include "../src/addons/mfgunlock/blackwell.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition      \
                << '\n';                                                        \
      return EXIT_FAILURE;                                                      \
    }                                                                           \
  } while (false)

int main() {
  using namespace mfgunlock::blackwell;

  CHECK(internal::RoleFromSharedMemory(7776) == KernelRole::MotionVector);
  CHECK(internal::RoleFromSharedMemory(3920) == KernelRole::Inpaint);
  CHECK(internal::RoleFromSharedMemory(784) == KernelRole::InpaintDecision);
  CHECK(internal::RoleFromSharedMemory(0) == KernelRole::Unknown);

#if MFGUNLOCK_HAS_GENERATED_BLACKWELL_CUBINS
  CHECK(HasGeneratedCubins());
  CHECK(generated::kCubinsBuiltFor[0] != '\0');
  for (const auto& replacement : generated::kCubinPatches) {
    CHECK(replacement.data != nullptr);
    CHECK(replacement.size != 0);
    CHECK(replacement.size <= replacement.orig_size);
    CHECK(internal::RoleFromSharedMemory(replacement.shared) != KernelRole::Unknown);
  }
#else
  CHECK(!HasGeneratedCubins());
#endif

#if MFGUNLOCK_HAS_GENERATED_THIN_GEOMETRY_CUBINS
  bool found_intermediate_scatter = false;
  bool found_silhouette_guard = false;
  bool found_aggressive_silhouette_guard = false;
  bool found_adaptive_geometry = false;
  bool found_adaptive_inpaint = false;
  for (const auto& replacement : generated_thin_geometry::kThinGeometryCubins) {
    CHECK(replacement.data != nullptr);
    CHECK(replacement.size != 0);
    CHECK(replacement.size <= replacement.slot_size);
    CHECK(replacement.source_fnv1a64 != 0);
    const std::string mechanism = replacement.mechanism;
    CHECK(mechanism == "intermediate_scatter" ||
          mechanism == "geometry_motion" ||
          mechanism == "geometry_motion_depth" ||
          mechanism == "geometry_motion_depth_refined" ||
          mechanism == "geometry_support_smooth_v2" ||
          mechanism == "geometry_motion_depth_aggressive" ||
          mechanism == "adaptive_quality_geometry_v1" ||
          mechanism == "adaptive_inpaint_decision_v1");
    found_intermediate_scatter |= mechanism == "intermediate_scatter";
    found_silhouette_guard |= mechanism == "geometry_motion_depth";
    found_aggressive_silhouette_guard |=
        mechanism == "geometry_motion_depth_aggressive";
    found_adaptive_geometry |=
        mechanism == "adaptive_quality_geometry_v1";
    found_adaptive_inpaint |=
        mechanism == "adaptive_inpaint_decision_v1";
    if (mechanism == "adaptive_quality_geometry_v1" ||
        mechanism == "adaptive_inpaint_decision_v1") {
      internal::ElfFingerprint compiled{};
      CHECK(internal::FingerprintElf(replacement.data, replacement.size,
                                     compiled));
      CHECK(compiled.shared == replacement.source_shared);
      CHECK(compiled.text <= replacement.source_text);
      CHECK(compiled.registers <=
            (mechanism == "adaptive_quality_geometry_v1" ? 40u : 48u));
    }
  }
  CHECK(found_intermediate_scatter);
  CHECK(found_silhouette_guard);
  CHECK(found_aggressive_silhouette_guard);
  CHECK(found_adaptive_geometry);
  CHECK(found_adaptive_inpaint);
#endif

  const mfgunlock::blackwell::Result defaults;
  CHECK(!defaults.silhouette_guard_requested);
  CHECK(!defaults.silhouette_guard);
  CHECK(!defaults.silhouette_guard_fallback);
  CHECK(defaults.silhouette_guard_mode_requested ==
        mfgunlock::blackwell::SilhouetteGuardMode::Off);
  CHECK(defaults.silhouette_guard_mode_selected ==
        mfgunlock::blackwell::SilhouetteGuardMode::Off);
  CHECK(std::string(mfgunlock::blackwell::SilhouetteGuardMechanism(
            mfgunlock::blackwell::SilhouetteGuardMode::Balanced)) ==
        "geometry_motion_depth");
  CHECK(std::string(mfgunlock::blackwell::SilhouetteGuardMechanism(
            mfgunlock::blackwell::SilhouetteGuardMode::Aggressive)) ==
        "geometry_motion_depth_aggressive");
  g_refinement_enabled = true;
  CHECK(std::string(SilhouetteGuardMechanism(SilhouetteGuardMode::Balanced)) ==
        "geometry_motion_depth_refined");
  g_geometry_confidence_v2_enabled = true;
  CHECK(std::string(SilhouetteGuardMechanism(SilhouetteGuardMode::Balanced)) ==
        "geometry_support_smooth_v2");
  CHECK(std::string(SilhouetteGuardMechanism(SilhouetteGuardMode::Aggressive)) ==
        "geometry_motion_depth_aggressive");
  g_geometry_confidence_v2_enabled = false;
  g_refinement_enabled = false;
  g_adaptive_quality_enabled = true;
  CHECK(std::string(SilhouetteGuardMechanism(SilhouetteGuardMode::Balanced)) ==
        "adaptive_quality_geometry_v1");
  g_adaptive_quality_enabled = false;

  std::cout << "blackwell kernel tests passed\n";
  return EXIT_SUCCESS;
}
