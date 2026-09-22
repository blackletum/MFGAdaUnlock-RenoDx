/* SPDX-License-Identifier: MIT
 * Research-only border confidence for valid warped-color candidates.
 * Attenuates this addon's extra warp-weight boost, never NVIDIA's native weight.
 * The two-pixel transition covers the bilinear sample footprint past the
 * provider's half-pixel validity bound; it is not a copied screen percentage.
 */
#pragma once

namespace mfgunlock::qualityborder {
inline constexpr const char* kBorderWeights = R"PTX(
// MFGUNLOCK_BORDER_CONFIDENCE_V1
cvt.rn.f32.u32 %qf2, %r10;
cvt.rn.f32.u32 %qf3, %r11;
// Forward candidate: distance from the already validated half-pixel bound.
sub.f32 %qf4, 0f3F800000, %f123;
min.f32 %qf4, %qf4, %f123;
mul.f32 %qf4, %qf4, %qf2;
sub.f32 %qf5, 0f3F800000, %f124;
min.f32 %qf5, %qf5, %f124;
mul.f32 %qf5, %qf5, %qf3;
min.f32 %qf4, %qf4, %qf5;
sub.f32 %qf4, %qf4, 0f3F000000;
mul.sat.f32 %qf4, %qf4, 0f3F000000;
fma.rn.f32 %qf5, %qf4, 0fC0000000, 0f40400000;
mul.f32 %qf4, %qf4, %qf4;
mul.f32 %qf4, %qf4, %qf5;
sub.f32 %qf5, %qf0, %qf8;
fma.rn.f32 %qf0, %qf4, %qf5, %qf8;
// Inverse candidate uses the same distance and weight rule.
sub.f32 %qf4, 0f3F800000, %f129;
min.f32 %qf4, %qf4, %f129;
mul.f32 %qf4, %qf4, %qf2;
sub.f32 %qf5, 0f3F800000, %f130;
min.f32 %qf5, %qf5, %f130;
mul.f32 %qf5, %qf5, %qf3;
min.f32 %qf4, %qf4, %qf5;
sub.f32 %qf4, %qf4, 0f3F000000;
mul.sat.f32 %qf4, %qf4, 0f3F000000;
fma.rn.f32 %qf5, %qf4, 0fC0000000, 0f40400000;
mul.f32 %qf4, %qf4, %qf4;
mul.f32 %qf4, %qf4, %qf5;
sub.f32 %qf5, %qf1, %qf10;
fma.rn.f32 %qf1, %qf4, %qf5, %qf10;
)PTX";
} // namespace mfgunlock::qualityborder
