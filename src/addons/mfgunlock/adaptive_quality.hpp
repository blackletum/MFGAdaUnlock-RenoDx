/* SPDX-License-Identifier: MIT
 * Unified, restart-scoped Adaptive Quality experiment.
 *
 * This fragment runs after the existing smooth-confidence and border stages.
 * It arbitrates only the addon's extra forward/inverse warp influence. NVIDIA's
 * native per-candidate weights remain the lower bound, so disagreement can
 * never make a candidate less trusted than the unmodified provider made it.
 */
#pragma once

namespace mfgunlock::adaptivequality {

inline constexpr const char* kCandidateArbitration = R"PTX(
// MFGUNLOCK_CANDIDATE_ARBITRATION_V2
// qf9 is the already-computed RGB disagreement between the two valid warps.
// Reuse the validated-warp agreement interval: no arbitration below 0.05,
// full disagreement evidence at 0.15. qv3 requires both candidates to be valid.
sub.f32 %qf2, %qf9, 0f3D4CCCCD;
mul.sat.f32 %qf2, %qf2, 0f41200000;
@!%qv3 mov.f32 %qf2, 0f00000000;
// MFGUNLOCK_AGREEMENT_FAST_PATH_V1
// With no disagreement evidence, arbitration is an identity operation. Skip
// the dominance/smoothstep/reconstruction path and preserve the current
// weights bit-for-bit in the common agreeing-candidate case.
setp.le.f32 %qv2, %qf2, 0f00000000;
@%qv2 bra MFGUNLOCK_CANDIDATE_ARBITRATION_DONE_V2;

// A near tie must not choose a direction. Scale disagreement by the absolute
// confidence gap, then use smoothstep so the winner changes continuously.
sub.f32 %qf3, %qf0, %qf1;
abs.f32 %qf4, %qf3;
mul.sat.f32 %qf2, %qf2, %qf4;
fma.rn.f32 %qf4, %qf2, 0fC0000000, 0f40400000;
mul.f32 %qf2, %qf2, %qf2;
mul.f32 %qf2, %qf2, %qf4;

// Attenuate only the weaker candidate's addon-added delta, anchored to the
// provider's original weights qf8/qf10. The stronger candidate is untouched.
fma.rn.f32 %qf4, %qf2, 0fBF400000, 0f3F800000;
setp.ge.f32 %qv2, %qf3, 0f00000000;
// Select the weaker candidate and its native anchor first. This preserves the
// V1 curve while avoiding two parallel delta/FMA paths and shortening live
// temporary ranges in the provider kernel.
selp.f32 %qf2, %qf1, %qf0, %qv2;
selp.f32 %qf3, %qf10, %qf8, %qv2;
sub.f32 %qf2, %qf2, %qf3;
fma.rn.f32 %qf2, %qf4, %qf2, %qf3;
@%qv2 mov.f32 %qf1, %qf2;
@!%qv2 mov.f32 %qf0, %qf2;
MFGUNLOCK_CANDIDATE_ARBITRATION_DONE_V2:
)PTX";

// Scalar reference used by unit/property tests. `weaker` and `native` are
// expected in [0, 1], disagreement is RGB L1 error, and dominance is the
// normalized confidence gap. The runtime PTX applies the same expression.
inline float ArbitrateExtraWeight(float native, float weaker,
                                  float disagreement, float dominance) {
  const auto clamp01 = [](float value) {
    if (!(value > 0.0f)) return 0.0f;
    return value < 1.0f ? value : 1.0f;
  };
  native = clamp01(native);
  weaker = clamp01(weaker);
  disagreement = clamp01((disagreement - 0.05f) * 10.0f);
  dominance = clamp01(dominance);
  float confidence = disagreement * dominance;
  confidence = confidence * confidence * (3.0f - 2.0f * confidence);
  const float attenuation = 1.0f - 0.75f * confidence;
  return native + attenuation * (weaker - native);
}

}  // namespace mfgunlock::adaptivequality
