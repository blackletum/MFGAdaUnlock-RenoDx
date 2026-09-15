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
  for (const auto& replacement : generated_thin_geometry::kThinGeometryCubins) {
    CHECK(replacement.data != nullptr);
    CHECK(replacement.size != 0);
    CHECK(replacement.size <= replacement.slot_size);
    CHECK(replacement.source_fnv1a64 != 0);
    const std::string mechanism = replacement.mechanism;
    CHECK(mechanism == "intermediate_scatter" ||
          mechanism == "geometry_motion" ||
          mechanism == "geometry_motion_depth" ||
          mechanism == "geometry_motion_depth_aggressive");
    found_intermediate_scatter |= mechanism == "intermediate_scatter";
    found_silhouette_guard |= mechanism == "geometry_motion_depth";
    found_aggressive_silhouette_guard |=
        mechanism == "geometry_motion_depth_aggressive";
  }
  CHECK(found_intermediate_scatter);
  CHECK(found_silhouette_guard);
  CHECK(found_aggressive_silhouette_guard);
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

  std::cout << "blackwell kernel tests passed\n";
  return EXIT_SUCCESS;
}
