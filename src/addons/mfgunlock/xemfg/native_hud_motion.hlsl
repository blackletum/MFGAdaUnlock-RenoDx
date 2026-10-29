// SPDX-License-Identifier: MIT
// 1.7 (plan I.7): HUD layer and HUD motion for HUD interpolation.
//
// LayerCS: the HUD of one real frame, premultiplied: rgb * coverage and the
// coverage, where coverage is how much the back buffer differs from the
// game's HUD-less image.
//
// MotionCS: one thread per 8x8 block of the current HUD layer. Blocks without
// HUD keep zero motion. Others search the previous layer for the best match
// (sum of absolute differences over rgb and coverage): a coarse search with
// step 4 over +-radius on a 4x4 subsample, then full 8x8 refinements of +-2
// (step 2) and +-1. World-anchored markers move far between real frames.
// Zero motion wins unless a displacement is clearly better, so static HUD
// over a changing scene never jitters. The result is the offset from the
// current block to where it was in the previous frame.
Texture2D<float4> InputA : register(t0);
Texture2D<float4> InputB : register(t1);
Texture2D<float2> SceneMotion : register(t2); // the game's motion vectors
RWTexture2D<float4> LayerOut : register(u0);
RWTexture2D<int2> MotionOut : register(u1);
RWTexture2D<int2> SceneOut : register(u2); // the scene's motion per block
cbuffer Params : register(b0) {
  uint2 size;
  float tolerance;
  float ramp;
  int radius;       // search around no motion (HUD fixed on screen)
  int sceneRadius;  // search around the scene's motion (world-anchored HUD)
  uint2 sceneBase;  // motion-vector region in SceneMotion
  uint2 sceneSize;
  float2 toPixels;  // motion-vector units to back-buffer pixels; 0: no seed
};

// t0 back buffer, t1 HUD-less, u0 layer.
[numthreads(8, 8, 1)]
void LayerCS(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= size)) return;
  const float3 back = InputA[id.xy].rgb;
  const float3 d = abs(back - InputB[id.xy].rgb);
  const float coverage = saturate((max(d.r, max(d.g, d.b)) - tolerance) * ramp);
  LayerOut[id.xy] = float4(back * coverage, coverage);
}

float4 Fetch(Texture2D<float4> layer, int2 p) {
  return any(p < 0) || any(p >= int2(size)) ? 0 : layer.Load(int3(p, 0));
}
float Cost(float4 a, float4 b) {
  const float4 d = abs(a - b);
  return d.r + d.g + d.b + 2 * d.a;
}

// t0 current layer, t1 previous layer, u1 motion (one texel per block).
[numthreads(8, 8, 1)]
void MotionCS(uint3 id : SV_DispatchThreadID) {
  const uint2 blocks = (size + 7) / 8;
  if (any(id.xy >= blocks)) return;
  const int2 origin = int2(id.xy) * 8;
  // The scene's motion at this block, kept for every block: a world-anchored
  // marker crossing an empty block at phase t is found through it.
  int2 seed = int2(0, 0);
  if (any(toPixels != 0)) {
    const float2 uv = (float2(origin) + 4) / float2(size);
    const uint2 texel = sceneBase + min(uint2(uv * float2(sceneSize)), sceneSize - 1);
    seed = int2(round(SceneMotion.Load(int3(texel, 0)) * toPixels));
  }
  SceneOut[id.xy] = seed;
  float present = 0;
  [loop] for (int i = 0; i < 64; i += 3) present = max(present, Fetch(InputA, origin + int2(i & 7, i >> 3)).a);
  if (present <= 0) { MotionOut[id.xy] = int2(0, 0); return; }

  float best = 1e30;
  int2 bestOffset = int2(0, 0);
  // World-anchored HUD (names, health bars, markers) moves with the scene:
  // the scene's motion at this block predicts the offset, however fast the
  // camera turns. The vectors point from the current sample to the previous.
  if (any(toPixels != 0)) {
    [loop] for (int sy = -sceneRadius; sy <= sceneRadius; sy += 2) {
      [loop] for (int sx = -sceneRadius; sx <= sceneRadius; sx += 2) {
        const int2 offset = seed + int2(sx, sy);
        float sad = 0;
        [unroll] for (int s = 0; s < 16; ++s) {
          const int2 q = origin + int2((s & 3) * 2, (s >> 2) * 2);
          sad += Cost(Fetch(InputA, q), Fetch(InputB, q + offset));
        }
        if (sad < best) { best = sad; bestOffset = offset; }
      }
    }
  }
  [loop] for (int y = -radius; y <= radius; y += 4) {
    [loop] for (int x = -radius; x <= radius; x += 4) {
      float sad = 0;
      [unroll] for (int s = 0; s < 16; ++s) {
        const int2 q = origin + int2((s & 3) * 2, (s >> 2) * 2);
        sad += Cost(Fetch(InputA, q), Fetch(InputB, q + int2(x, y)));
      }
      if (sad < best) { best = sad; bestOffset = int2(x, y); }
    }
  }
  float refined = 1e30;
  int2 refinedOffset = bestOffset;
  float zero = 0;
  [loop] for (int level = 0; level < 2; ++level) {
    const int stride = level == 0 ? 2 : 1;
    const int2 center = refinedOffset;
    [loop] for (int ry = -1; ry <= 1; ++ry) {
      [loop] for (int rx = -1; rx <= 1; ++rx) {
        const int2 offset = center + int2(rx, ry) * stride;
        float sad = 0;
        [loop] for (int p = 0; p < 64; ++p) {
          const int2 q = origin + int2(p & 7, p >> 3);
          sad += Cost(Fetch(InputA, q), Fetch(InputB, q + offset));
        }
        if (sad < refined) { refined = sad; refinedOffset = offset; }
      }
    }
  }
  [loop] for (int z = 0; z < 64; ++z) {
    const int2 q = origin + int2(z & 7, z >> 3);
    zero += Cost(Fetch(InputA, q), Fetch(InputB, q));
  }
  // Prefer no motion unless the match is clearly better (static HUD whose
  // semi-transparent pixels change with the scene behind them).
  MotionOut[id.xy] = zero <= refined * 1.25 + 0.5 ? int2(0, 0) : refinedOffset;
}
