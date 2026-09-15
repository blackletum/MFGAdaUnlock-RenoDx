# Silhouette disocclusion guard experiment

Status: experimental, disabled by default, and not intended for release until
matched visual captures and frame-pacing tests are complete.

## Target

This experiment targets foreground/background bleeding and silhouette
stretching when a moving object crosses a depth boundary, such as a character
passing in front of a tree. It does not claim to identify true visibility or
reconstruct pixels that are hidden in both source frames.

The addon has no validated visibility mask at its current intervention points.
The safest available proxy is local motion and processed depth already present
inside NVIDIA's `Kernel_EstimateIntermMvecsScatter` shared tile.

## Behavior

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

## Runtime and fallback

The option is persisted as `SilhouetteBoundaryGuard` and requires a restart.
When enabled it supersedes unconditional Intermediate Scatter Retention for the
same motion-vector kernel. If the exact guard cubin is unavailable and the
regular Intermediate option is enabled, the addon falls back to the released
0.9 retention variant. Otherwise it retains the baseline Blackwell kernel.

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
