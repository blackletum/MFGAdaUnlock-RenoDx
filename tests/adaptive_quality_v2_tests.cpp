#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

#include "../src/addons/mfgunlock/adaptive_quality.hpp"
#include "../src/addons/mfgunlock/adaptive_quality_v2.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition      \
                << '\n';                                                        \
      return EXIT_FAILURE;                                                      \
    }                                                                           \
  } while (false)

constexpr uint64_t Fnv1a64(const char* text) {
  uint64_t hash = UINT64_C(14695981039346656037);
  while (*text != '\0') {
    hash ^= static_cast<unsigned char>(*text++);
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

int main() {
  namespace aq = mfgunlock::adaptivequality;
  namespace aq2 = mfgunlock::adaptivequalityv2;

  // Release-candidate payload fingerprints. These intentionally freeze the
  // user-validated endpoint fast paths (14-register synthetic V2 allocation)
  // so later UI/latency work cannot silently change visual PTX.
  CHECK(Fnv1a64(aq2::kSmoothWarpConfidence) ==
        UINT64_C(0x66abc7e3a697a65f));
  CHECK(Fnv1a64(aq2::kBorderConfidence) ==
        UINT64_C(0x2adb5a0dd7cdf2e2));
  CHECK(Fnv1a64(aq2::kCandidateArbitration) ==
        UINT64_C(0xb0e724b196e70779));

  CHECK(aq::NormalizeProfile(0) == aq::Profile::kStableV1);
  CHECK(aq::kDefaultProfile == aq::Profile::kFlickerReducedV2);
  CHECK(aq::NormalizeProfile(1) == aq::Profile::kStableV1);
  CHECK(aq::NormalizeProfile(2) == aq::Profile::kFlickerReducedV2);
  CHECK(aq::NormalizeProfile(999) == aq::Profile::kStableV1);
  CHECK(aq::SelectComponentVersion(aq::Profile::kStableV1, true, true) ==
        aq::ComponentVersion::kV1);
  CHECK(aq::SelectComponentVersion(aq::Profile::kStableV1, true, false) ==
        aq::ComponentVersion::kNative);
  CHECK(aq::SelectComponentVersion(aq::Profile::kStableV1, false, true) ==
        aq::ComponentVersion::kV1);
  CHECK(aq::SelectComponentVersion(aq::Profile::kStableV1, false, false) ==
        aq::ComponentVersion::kNative);
  CHECK(aq::SelectComponentVersion(aq::Profile::kFlickerReducedV2, true,
                                   true) == aq::ComponentVersion::kV2);
  CHECK(aq::SelectComponentVersion(aq::Profile::kFlickerReducedV2, true,
                                   false) == aq::ComponentVersion::kV2);
  CHECK(aq::SelectComponentVersion(aq::Profile::kFlickerReducedV2, false,
                                   true) == aq::ComponentVersion::kV1);
  CHECK(aq::SelectComponentVersion(aq::Profile::kFlickerReducedV2, false,
                                   false) == aq::ComponentVersion::kNative);
  CHECK(aq::MergeComponentVersions(aq::ComponentVersion::kNative,
                                   aq::ComponentVersion::kV2) ==
        aq::ComponentVersion::kV2);
  CHECK(aq::MergeComponentVersions(aq::ComponentVersion::kV2,
                                   aq::ComponentVersion::kV1) ==
        aq::ComponentVersion::kMixed);

  constexpr float kNative = 0.2f;
  constexpr float kV1Target = 0.85f;
  CHECK(aq2::EffectiveWarpWeight(kNative, kV1Target, 0.0f) == 0.0f);
  CHECK(std::abs(aq2::EffectiveWarpWeight(kNative, kV1Target, 1.0f) -
                 kV1Target) < 0.00001f);
  float previous = 0.0f;
  for (int index = 0; index <= 1000; ++index) {
    const float confidence = static_cast<float>(index) / 1000.0f;
    const float weight =
        aq2::EffectiveWarpWeight(kNative, kV1Target, confidence);
    CHECK(std::isfinite(weight));
    CHECK(weight >= 0.0f && weight <= kV1Target);
    CHECK(weight + 0.000001f >= previous);
    previous = weight;
  }
  const float before = aq2::EffectiveWarpWeight(kNative, kV1Target, 0.4999f);
  const float after = aq2::EffectiveWarpWeight(kNative, kV1Target, 0.5001f);
  CHECK(after >= before);
  CHECK(after - before < 0.001f);

  CHECK(aq2::GeometryRelaxation(0.0f, 0.0f) == 0.0f);
  CHECK(std::abs(aq2::GeometryRelaxation(1.0f, 0.0f) - 0.25f) <
        0.00001f);
  CHECK(std::abs(aq2::GeometryRelaxation(1.0f, 1.0f) - 0.5f) <
        0.00001f);
  float prior_relaxation = 0.0f;
  for (int index = 0; index <= 1000; ++index) {
    const float second = static_cast<float>(index) / 1000.0f;
    const float relaxation = aq2::GeometryRelaxation(1.0f, second);
    CHECK(relaxation >= 0.25f && relaxation <= 0.5f);
    CHECK(relaxation + 0.000001f >= prior_relaxation);
    prior_relaxation = relaxation;
  }

  CHECK(!aq2::NeedsInpaintV2(-1.0f));
  CHECK(!aq2::NeedsInpaintV2(-0.0f));
  CHECK(!aq2::NeedsInpaintV2(0.0f));
  CHECK(aq2::NeedsInpaintV2(0.0001f));
  CHECK(aq2::NeedsInpaintV2(std::numeric_limits<float>::infinity()));
  CHECK(aq2::NeedsInpaintV2(-std::numeric_limits<float>::infinity()));
  CHECK(aq2::NeedsInpaintV2(std::numeric_limits<float>::quiet_NaN()));

  std::cout << "adaptive quality V2 tests passed\n";
  return EXIT_SUCCESS;
}
