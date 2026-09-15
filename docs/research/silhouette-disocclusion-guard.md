# Silhouette disocclusion guard experiment

Status: experimental release candidate. Balanced is the recommended default
for fresh configurations after matched frame-pacing validation. Existing saved
choices are preserved, and Off and Aggressive remain available per game.

## Unified target

This experiment targets foreground/background bleeding and silhouette
stretching when a moving object crosses a depth boundary, such as a character
passing in front of a tree. It does not claim to identify true visibility or
reconstruct pixels that are hidden in both source frames.

The following names describe overlapping symptoms rather than five independent
signals: occlusion-boundary artifacts, foreground/background bleeding,
silhouette/edge stretching, motion-boundary artifacts and depth-discontinuity
artifacts. The implementation deliberately treats them as one boundary-quality
system so the same motion/depth evidence is not penalized repeatedly.

The addon has no validated visibility mask at its current intervention points.
The safest available proxy is local motion and processed depth already present
inside NVIDIA's `Kernel_EstimateIntermMvecsScatter` shared tile.

## Balanced behavior

The released 0.9 Intermediate Scatter Retention halves a motion-consistency
divisor unconditionally. The guard replaces that added relaxation with:

```text
motionSupport = saturate(1 - motionDifferenceSquared / nativeThreshold)
sameSurface   = ordered(abs(neighborDepth - centerDepth) < 3)
support       = maximum motionSupport among same-surface cardinal neighbors
K_effective   = K_native * (1 - 0.5 * support)
```

The processed-depth boundary of `3` is reused from the same provider kernel; it
is not interpreted as meters or normalized game depth. Motion and depth come
from the same shared tile and direction.

- Support `0` restores the provider's native divisor.
- Support `1` reproduces the full 0.9 retention.
- Intermediate support changes continuously between those limits.
- NaN/unordered values cannot add support.
- One cardinal neighbor is sufficient so a narrow two-pixel feature is not
  rejected merely because most surrounding pixels belong to the background.

The guard conditions only this fork's extra intermediate retention. It does not
make NVIDIA's native rejection stricter, change the frame multiplier, alter
Streamline tags, rewrite game depth/motion resources, or touch Present/pacing.

## Aggressive behavior

Aggressive uses the same proven shared-tile inputs but is strictly no more
permissive than Balanced:

```text
sameSurface   = ordered(abs(neighborDepth - centerDepth) < 2)
sumSupport    = sum(motionSupport for same-surface cardinal neighbors)
confidence    = saturate(sumSupport - 1)^2
K_effective   = K_native * (1 - 0.25 * confidence)
```

A single perfect neighbor therefore provides no added relaxation. More than one
neighbor-equivalent of coherent support is required, marginal evidence is
suppressed nonlinearly, and full confidence reaches only `0.75*K_native` versus
Balanced's `0.5*K_native`. This more strongly favors the provider's native
rejection near depth/motion boundaries at the cost of narrow-detail retention.

## Runtime and fallback

The three-state option is persisted as `BoundaryArtifactMitigationMode` and
requires a restart: Off, Balanced and Aggressive. The earlier experimental
`SilhouetteBoundaryGuard` boolean is migrated to Balanced when the new key does
not exist. A selected guard supersedes unconditional Intermediate Scatter
Retention for the same motion-vector kernel. Aggressive falls back to Balanced
when only that exact variant is available; a regular Intermediate selection
then remains the released 0.9 compatibility fallback. Otherwise the baseline
Blackwell kernel is retained.

All selection remains fail-closed behind the existing provider metadata, exact
source cubin hash, role, architecture and slot-size checks. NVIDIA payloads are
generated locally and remain excluded from source control.

## Expected gains and risks

Expected targets are character/weapon outlines, moving objects crossing trees
or walls, and newly exposed background adjacent to a silhouette. Possible
regressions include reduced persistence of diagonal or one-pixel geometry,
remaining native-provider artifacts, and changed scatter contention that could
indirectly affect GPU cost. The guard cannot repair missing or wrong game motion
vectors and cannot eliminate artifacts already present in native 2x FG.

Compare guard off/on after a full restart using the same scene, camera motion,
base FPS, multiplier, DLSS preset and display conditions. Inspect both artifact
reduction and retained thin detail; disappearance caused only by blur or lost
geometry is not a quality win.

## Release-gate validation

Matched manual camera runs were captured in Onimusha: Way of the Sword on
September 15, 2026 with an RTX 4070 SUPER, DLSS-G 310.9.1, Streamline 2.14.1,
fixed 4x and Hardware: Independent Flip. Off and Balanced both measured a
3.998x generated/source cadence.

| Metric | Off | Balanced | Difference |
|---|---:|---:|---:|
| Average output FPS | 208.11 | 208.44 | +0.159% |
| Display p50 | 4.4710 ms | 4.4734 ms | +0.054% |
| Display p95 | 8.8895 ms | 8.8821 ms | -0.083% |
| Display p99 | 9.6438 ms | 9.8289 ms | +1.919% |
| Mean instrumented latency | 22.0437 ms | 22.0099 ms | -0.153% |
| Latency p95 | 29.2175 ms | 29.4150 ms | +0.676% |
| Generated-frame GPU p95 | 1.5508 ms | 1.5510 ms | +0.013% |

This single matched A/B found no measurable throughput, cadence, mean-latency,
or generated-frame-cost regression. The small tail-latency differences remain
within the variability of manually repeated runs and are not a universal
performance claim. Visual effectiveness remains integration- and scene-specific.
