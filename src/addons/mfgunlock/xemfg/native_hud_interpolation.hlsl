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
// The current frame's block at `p` was matched with displacement `d`.
bool MatchedAt(float2 p, float2 d) {
  return all(abs(float2(Motion.Load(int3(int2(floor(p)) >> 3, 0))) - d) <= 2);
}
float4 ComposePS(float4 position : SV_Position) : SV_Target {
  const float2 q = position.xy;
  if (valid0 <= 0) return Layer1.SampleLevel(Linear, q * inverseSize, 0);
  const float4 l0 = Layer0.SampleLevel(Linear, q * inverseSize, 0);
  const float4 l1 = Layer1.SampleLevel(Linear, q * inverseSize, 0);
  const float4 still = lerp(l0, l1, phase);
  if (moving <= 0) return still;
  // Candidates: the vectors of the 3x3 neighbouring blocks (a pixel the HUD
  // crosses at phase t may sit in a block without one) and the scene's motion
  // here (a world-anchored marker far from its destination block). A
  // candidate d is only used where the block the element comes from in the
  // current frame, q - (1-t)*d, was matched with d itself: screen-fixed HUD
  // (matched with no motion) is never dragged along with the scene, and two
  // unrelated HUD elements that happen to line up are not joined.
  const int2 block = int2(q) >> 3;
  float best = l0.a * l1.a + 0.05;
  float4 result = still;
  bool moved = false, left = false;
  float2 candidates[10];
  [unroll] for (int k = 0; k < 9; ++k)
    candidates[k] = float2(Motion.Load(int3(block + int2(k % 3 - 1, k / 3 - 1), 0)));
  candidates[9] = float2(SceneMotion.Load(int3(block, 0)));
  [unroll] for (int c = 0; c < 10; ++c) {
    const float2 d = candidates[c];
    if (all(d == 0)) continue;
    // The previous frame's HUD at q left: the current block it moved to,
    // q - d, was matched with d.
    if (l0.a > 0 && MatchedAt(q - d, d)) left = true;
    if (!MatchedAt(q - (1 - phase) * d, d)) continue;
    float score;
    const float4 pair = Pair(q, d, score);
    if (score > best) { best = score; result = pair; moved = true; }
  }
  if (moved) return result;
  // Nothing moves through q at phase t. Drop what is only a trail: HUD of the
  // previous frame that left q, and HUD of the current frame whose block is
  // matched with motion (it has not arrived yet).
  const bool arriving = l1.a > 0 && any(float2(Motion.Load(int3(block, 0))) != 0);
  return lerp(left ? 0 : l0, arriving ? 0 : l1, phase);
}
