// SPDX-License-Identifier: MIT
// 1.7 (plan I.7-I.9): HUD layer and per-element HUD motion for HUD
// interpolation.
//
// LayerCS: the HUD of one real frame, premultiplied: rgb * coverage and the
// coverage, where coverage is how much the back buffer differs from the
// game's HUD-less image.
//
// MotionCS: one thread per 8x8 block of the current HUD layer. Blocks with
// HUD search the previous layer for the best match (sum of absolute
// differences over rgb and coverage): around no motion (screen-fixed HUD) and
// around the scene's own motion (world-anchored HUD), coarse then refined.
// It writes the block's candidate vector and its confidence, and starts the
// element labels.
//
// LabelCS (repeated): connected HUD blocks (8-neighbours) form one element;
// each block converges to the smallest block index of its element.
// RepresentCS: the element's size and bounds. ScoreCS / MinCS / ElectCS:
// each block's vector scored over the whole element; the best one wins.
// AgreeCS: how many of the element's blocks the representative's vector
// matches about as well as their own best.
// FinalCS: one vector per element: the representative's, when the element
// is small and at least 3/4 of its blocks agree; otherwise no motion, so
// large panels (minimap) cross-fade whole and an element never splits.
Texture2D<float4> InputA : register(t0);
Texture2D<float4> InputB : register(t1);
Texture2D<float2> SceneMotion : register(t2); // the game's motion vectors
RWTexture2D<float4> LayerOut : register(u0);
RWTexture2D<int2> Raw : register(u1);       // per block: candidate vector
RWTexture2D<int2> SceneOut : register(u2);  // per block: the scene's motion
RWTexture2D<uint> Label : register(u3);     // per block: element label
RWTexture2D<uint> Quality : register(u4);   // per block: match cost ratio (float bits)
RWTexture2D<uint> Element : register(u5);   // 8 rows of blocks: best score, representative, agree, total, bounds (x0, y0, x1, y1)
RWTexture2D<int2> Final : register(u6);     // per block: the element's vector
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
static const uint kNone = 0xFFFFFFFFu;
static const uint kMaxElementBlocks = 48;

// t0 back buffer, t1 HUD-less, u0 layer.
[numthreads(8, 8, 1)]
void LayerCS(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= size)) return;
  const float3 back = InputA[id.xy].rgb;
  const float3 d = abs(back - InputB[id.xy].rgb);
  const float coverage = saturate((max(d.r, max(d.g, d.b)) - tolerance) * ramp);
  LayerOut[id.xy] = float4(back * coverage, coverage);
}

uint2 Blocks() { return (size + 7) >> 3; }
uint BlockIndex(uint2 b) { return b.y * Blocks().x + b.x; }
uint2 BlockOf(uint index) { return uint2(index % Blocks().x, index / Blocks().x); }
uint2 Row(uint2 b, uint row) { return uint2(b.x, b.y + row * Blocks().y); }

float4 Fetch(Texture2D<float4> layer, int2 p) {
  return any(p < 0) || any(p >= int2(size)) ? 0 : layer.Load(int3(p, 0));
}
float Cost(float4 a, float4 b) {
  const float4 d = abs(a - b);
  return d.r + d.g + d.b + 2 * d.a;
}
float CoarseCost(int2 origin, int2 offset) {
  float sad = 0;
  [unroll] for (int s = 0; s < 16; ++s) {
    const int2 q = origin + int2((s & 3) * 2, (s >> 2) * 2);
    sad += Cost(Fetch(InputA, q), Fetch(InputB, q + offset));
  }
  return sad;
}
float FullCost(int2 origin, int2 offset) {
  float sad = 0;
  [loop] for (int p = 0; p < 64; ++p) {
    const int2 q = origin + int2(p & 7, p >> 3);
    sad += Cost(Fetch(InputA, q), Fetch(InputB, q + offset));
  }
  return sad;
}

// Keeps the best candidate and the best one elsewhere (more than 2 px away),
// which tells how unambiguous the best match is.
void Consider(float sad, int2 offset, inout float best, inout int2 bestOffset, inout float second) {
  if (sad < best) {
    if (any(abs(offset - bestOffset) > 2)) second = best;
    best = sad; bestOffset = offset;
  } else if (sad < second && any(abs(offset - bestOffset) > 2)) {
    second = sad;
  }
}

// t0 current layer, t1 previous layer, t2 scene motion.
[numthreads(8, 8, 1)]
void MotionCS(uint3 id : SV_DispatchThreadID) {
  const uint2 blocks = Blocks();
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
  [unroll] for (uint row = 0; row < 8; ++row) Element[Row(id.xy, row)] = row < 2 || row == 4 || row == 5 ? kNone : 0;
  float present = 0;
  [loop] for (int i = 0; i < 64; i += 3) present = max(present, Fetch(InputA, origin + int2(i & 7, i >> 3)).a);
  if (present <= 0) {
    Raw[id.xy] = int2(0, 0); Label[id.xy] = kNone; Quality[id.xy] = asuint(1e30f);
    return;
  }
  Label[id.xy] = BlockIndex(id.xy);

  float best = 1e30, second = 1e30;
  int2 bestOffset = int2(0, 0);
  if (any(toPixels != 0)) {
    [loop] for (int sy = -sceneRadius; sy <= sceneRadius; ++sy) {
      [loop] for (int sx = -sceneRadius; sx <= sceneRadius; ++sx) {
        const int2 offset = seed + int2(sx, sy);
        Consider(CoarseCost(origin, offset), offset, best, bestOffset, second);
      }
    }
  }
  // Step 1 near no motion: a coarser grid aliases with repeating HUD (text),
  // and the element's ends must be able to propose the exact offset.
  [loop] for (int fy = -16; fy <= 16; ++fy) {
    [loop] for (int fx = -16; fx <= 16; ++fx) {
      Consider(CoarseCost(origin, int2(fx, fy)), int2(fx, fy), best, bestOffset, second);
    }
  }
  [loop] for (int y = -radius; y <= radius; y += 4) {
    [loop] for (int x = -radius; x <= radius; x += 4) {
      if (all(abs(int2(x, y)) <= 16)) continue;
      Consider(CoarseCost(origin, int2(x, y)), int2(x, y), best, bestOffset, second);
    }
  }
  float refined = 1e30;
  int2 refinedOffset = bestOffset;
  [loop] for (int level = 0; level < 2; ++level) {
    const int stride = level == 0 ? 2 : 1;
    const int2 center = refinedOffset;
    [loop] for (int ry = -1; ry <= 1; ++ry) {
      [loop] for (int rx = -1; rx <= 1; ++rx) {
        const int2 offset = center + int2(rx, ry) * stride;
        const float sad = FullCost(origin, offset);
        if (sad < refined) { refined = sad; refinedOffset = offset; }
      }
    }
  }
  const float zero = FullCost(origin, int2(0, 0));
  // A displacement must be clearly better than no motion (static HUD whose
  // semi-transparent pixels change with the scene) and good in itself. Cost
  // per pixel is at most 5.
  const bool confident = any(refinedOffset != 0) && refined * 2 + 1 < zero && refined < 64 * 0.25;
  Raw[id.xy] = confident ? refinedOffset : int2(0, 0);
  // The element's representative is its least ambiguous block: in text and
  // other repeating HUD, inner blocks match several offsets equally and only
  // the element's ends match one. Lower is better; screen-fixed blocks rank
  // by how well no motion fits.
  const float ambiguity = (best + 0.25) / (second + 0.25);
  Quality[id.xy] = asuint(confident ? ambiguity : 1 + zero);
}

// One step of label propagation (8-neighbours) and pointer jumping.
[numthreads(8, 8, 1)]
void LabelCS(uint3 id : SV_DispatchThreadID) {
  const uint2 blocks = Blocks();
  if (any(id.xy >= blocks)) return;
  uint label = Label[id.xy];
  if (label == kNone) return;
  [unroll] for (int y = -1; y <= 1; ++y) {
    [unroll] for (int x = -1; x <= 1; ++x) {
      const int2 n = int2(id.xy) + int2(x, y);
      if (any(n < 0) || any(n >= int2(blocks))) continue;
      label = min(label, Label[uint2(n)]);
    }
  }
  label = min(label, Label[BlockOf(label)]);
  Label[id.xy] = label;
}

// The element's size and bounds.
[numthreads(8, 8, 1)]
void RepresentCS(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Blocks())) return;
  const uint label = Label[id.xy];
  if (label == kNone) return;
  const uint2 root = BlockOf(label);
  uint ignored;
  InterlockedAdd(Element[Row(root, 3)], 1, ignored);
  InterlockedMin(Element[Row(root, 4)], id.x, ignored);
  InterlockedMin(Element[Row(root, 5)], id.y, ignored);
  InterlockedMax(Element[Row(root, 6)], id.x, ignored);
  InterlockedMax(Element[Row(root, 7)], id.y, ignored);
}

// Each block proposes its own vector for the whole element; the proposal is
// scored over every block of the element. Text and other repeating HUD match
// several offsets equally inside, and only the element's ends tell them
// apart, so the element's vector is chosen on the element, not on a block.
[numthreads(8, 8, 1)]
void ScoreCS(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Blocks())) return;
  const uint label = Label[id.xy];
  if (label == kNone) return;
  const uint2 root = BlockOf(label);
  const uint2 low = uint2(Element[Row(root, 4)], Element[Row(root, 5)]);
  const uint2 high = uint2(Element[Row(root, 6)], Element[Row(root, 7)]);
  const uint2 extent = high - low + 1;
  float score = 1e30;
  if (Element[Row(root, 3)] <= kMaxElementBlocks && extent.x * extent.y <= 2 * kMaxElementBlocks) {
    const int2 candidate = Raw[id.xy];
    score = 0;
    [loop] for (uint y = low.y; y <= high.y; ++y) {
      [loop] for (uint x = low.x; x <= high.x; ++x) {
        if (Label[uint2(x, y)] == label) score += FullCost(int2(x, y) * 8, candidate);
      }
    }
  }
  Quality[id.xy] = asuint(score);
}

[numthreads(8, 8, 1)]
void MinCS(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Blocks())) return;
  const uint label = Label[id.xy];
  if (label == kNone) return;
  uint ignored;
  InterlockedMin(Element[Row(BlockOf(label), 0)], Quality[id.xy], ignored);
}

[numthreads(8, 8, 1)]
void ElectCS(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Blocks())) return;
  const uint label = Label[id.xy];
  if (label == kNone || Quality[id.xy] != Element[Row(BlockOf(label), 0)]) return;
  uint ignored;
  InterlockedMin(Element[Row(BlockOf(label), 1)], BlockIndex(id.xy), ignored);
}

[numthreads(8, 8, 1)]
void AgreeCS(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Blocks())) return;
  const uint label = Label[id.xy];
  if (label == kNone) return;
  const uint representative = Element[Row(BlockOf(label), 1)];
  if (representative == kNone) return;
  // A block agrees when the representative's vector matches it about as well
  // as its own best: text and other repeating HUD match several offsets
  // equally, and only some blocks (the element's ends) tell them apart.
  const int2 origin = int2(id.xy) * 8;
  const float own = FullCost(origin, Raw[id.xy]);
  const float common = FullCost(origin, Raw[BlockOf(representative)]);
  if (common <= own * 1.25 + 0.5) {
    uint ignored;
    InterlockedAdd(Element[Row(BlockOf(label), 2)], 1, ignored);
  }
}

[numthreads(8, 8, 1)]
void FinalCS(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Blocks())) return;
  const uint label = Label[id.xy];
  int2 chosen = int2(0, 0);
  if (label != kNone) {
    const uint2 root = BlockOf(label);
    const uint representative = Element[Row(root, 1)];
    const uint agree = Element[Row(root, 2)], total = Element[Row(root, 3)];
    if (representative != kNone && total <= kMaxElementBlocks && agree * 4 >= total * 3)
      chosen = Raw[BlockOf(representative)];
  }
  Final[id.xy] = chosen;
}
