// SPDX-License-Identifier: MIT
// 1.7 (plan I.3, I.7): HUD for a generated frame. XeFG interpolated only the
// game's HUD-less scene; this pass draws the HUD over it at phase t between
// the previous real frame (0) and the current one (1).
// Layers are premultiplied HUD (rgb * coverage, coverage) from
// native_hud_motion.hlsl LayerCS. Motion holds, per 8x8 block of the current
// frame, the offset to where that HUD was in the previous frame (MotionCS).
// A HUD element moving from x0 to x1 = x0 - d is at x1 + (1-t)*d at phase t:
// the current layer is read at q - (1-t)*d and the previous at q + t*d, and
// the two are mixed by phase. Static HUD (d = 0) is a plain cross-fade.
// Output is premultiplied: blend ONE, INV_SRC_ALPHA over the generated frame.
Texture2D<float4> Layer0 : register(t0);
Texture2D<float4> Layer1 : register(t1);
Texture2D<int2> Motion : register(t2);
Texture2D<int2> SceneMotion : register(t3); // the scene's motion per block
SamplerState Linear : register(s0);
cbuffer Params : register(b0) {
  float phase;   // t in (0, 1): position of this generated frame
  float valid0;  // 0: no previous real frame; only the current HUD is drawn
  float moving;  // 0: no motion field; cross-fade only
  float unused;
  float2 inverseSize;
};
void FullscreenVS(uint id : SV_VertexID, out float4 position : SV_Position) {
  const float2 uv = float2((id << 1) & 2, id & 2);
  position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 Pair(float2 q, float2 d, out float score) {
  const float4 l0 = Layer0.SampleLevel(Linear, (q + phase * d) * inverseSize, 0);
  const float4 l1 = Layer1.SampleLevel(Linear, (q - (1 - phase) * d) * inverseSize, 0);
  score = l0.a * l1.a;
  return lerp(l0, l1, phase);
}
float4 ComposePS(float4 position : SV_Position) : SV_Target {
  const float2 q = position.xy;
  if (valid0 <= 0) return Layer1.SampleLevel(Linear, q * inverseSize, 0);
  float zeroScore;
  const float4 still = Pair(q, 0, zeroScore);
  if (moving <= 0) return still;
  // A block's vector exists only where its HUD is in the current frame, so a
  // pixel the HUD crosses at phase t may sit in a block without one. Try the
  // vectors of the 3x3 neighbouring blocks and keep the one both frames agree
  // on; no motion wins ties.
  const int2 block = int2(q) >> 3;
  float best = zeroScore + 0.05;
  float4 result = still;
  bool nearbyMotion = false;
  [unroll] for (int y = -1; y <= 1; ++y) {
    [unroll] for (int x = -1; x <= 1; ++x) {
      const float2 d = float2(Motion.Load(int3(block + int2(x, y), 0)));
      if (all(d == 0)) continue;
      nearbyMotion = true;
      float score;
      const float4 moved = Pair(q, d, score);
      if (score > best) { best = score; result = moved; }
    }
  }
  // A world-anchored marker far from its destination block (fast camera
  // turn) moves with the scene: try the scene's motion here as well.
  const float2 sceneD = float2(SceneMotion.Load(int3(block, 0)));
  if (any(sceneD != 0)) {
    // The scene moves here: HUD that no vector explains is a world-anchored
    // marker that already left this pixel.
    nearbyMotion = true;
    float score;
    const float4 moved = Pair(q, sceneD, score);
    if (score > best) { best = score; result = moved; }
  }
  // Moving HUD nearby but no vector explains this pixel: it is where the
  // element was, not where it is at phase t; leave the scene.
  if (nearbyMotion && best < 0.25) return 0;
  return result;
}
