#!/usr/bin/env python3
"""Property checks for the silhouette-boundary guard's scalar contract."""

from __future__ import annotations

import math
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import build_thin_geometry_variants as builder  # noqa: E402


def effective_scale(center_motion, center_depth, neighbors, native_threshold=1.0):
    strongest = 0.0
    threshold = max(float(native_threshold), 1.0)
    for motion, depth in neighbors:
        dx = motion[0] - center_motion[0]
        dy = motion[1] - center_motion[1]
        support = 1.0 - (dx * dx + dy * dy) / threshold
        depth_delta = abs(depth - center_depth)
        if not math.isfinite(support) or not math.isfinite(depth_delta):
            support = 0.0
        if support <= 0.0 or not depth_delta < 3.0:
            support = 0.0
        strongest = max(strongest, min(support, 1.0))
    return 1.0 - 0.5 * strongest


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main() -> None:
    center = (4.0, -2.0)
    incompatible = [((20.0, 20.0), 0.0)] * 4
    check(effective_scale(center, 0.0, incompatible) == 1.0,
          "unsupported boundary must retain the native divisor")

    one_thin_neighbor = [((4.0, -2.0), 0.0)] + incompatible[1:]
    check(effective_scale(center, 0.0, one_thin_neighbor) == 0.5,
          "one coherent same-depth neighbor must preserve full added retention")

    wrong_surface = [((4.0, -2.0), 3.0)] + incompatible[1:]
    check(effective_scale(center, 0.0, wrong_surface) == 1.0,
          "the provider's depth boundary must exclude cross-surface support")

    partial = [((4.5, -2.0), 0.0)] + incompatible[1:]
    check(0.5 < effective_scale(center, 0.0, partial) < 1.0,
          "partial motion support must produce a bounded continuous response")

    unordered = [((math.nan, -2.0), 0.0)] + incompatible[1:]
    check(effective_scale(center, 0.0, unordered) == 1.0,
          "unordered motion must not add retention")

    for direction in builder._SILHOUETTE_NEIGHBORS:
        program = builder._silhouette_support_program(direction)
        check("MFGUNLOCK_SILHOUETTE_BOUNDARY_GUARD_V1" in program,
              "generated PTX marker is missing")
        check(program.count("setp.lt.and.f32") == 4,
              "each cardinal neighbor must have one depth-membership gate")
        check(program.count("max.f32 %qgf0") == 4,
              "the guard must preserve the strongest thin-geometry support")
        check("mul.ftz.f32 %f2, %f2, %qgf11;" in program,
              "the final bounded divisor update is missing")

    print("silhouette guard tests passed")


if __name__ == "__main__":
    main()
