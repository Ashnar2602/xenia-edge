/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik and the Xenia Edge Community. All rights reserved.*
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

cbuffer XeNeuralVectorExpandConstants : register(b0) {
  uint2 xe_full_resolution;
  uint2 xe_grid_resolution;
  uint xe_grid_size;
  float xe_scale_factor;
};

Texture2D<int2> xe_grid_flow : register(t0);
RWTexture2D<float2> xe_expanded_flow : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  [branch] if (any(id.xy >= xe_full_resolution)) {
    return;
  }

  float2 grid_pos = (float2(id.xy) + 0.5f) / float(xe_grid_size) - 0.5f;
  int2 p0 = clamp(int2(floor(grid_pos)), int2(0, 0), int2(xe_grid_resolution) - 1);
  int2 p1 = clamp(p0 + int2(1, 0), int2(0, 0), int2(xe_grid_resolution) - 1);
  int2 p2 = clamp(p0 + int2(0, 1), int2(0, 0), int2(xe_grid_resolution) - 1);
  int2 p3 = clamp(p0 + int2(1, 1), int2(0, 0), int2(xe_grid_resolution) - 1);

  float2 f = frac(grid_pos);

  float2 v0 = float2(xe_grid_flow.Load(int3(p0, 0))) * xe_scale_factor;
  float2 v1 = float2(xe_grid_flow.Load(int3(p1, 0))) * xe_scale_factor;
  float2 v2 = float2(xe_grid_flow.Load(int3(p2, 0))) * xe_scale_factor;
  float2 v3 = float2(xe_grid_flow.Load(int3(p3, 0))) * xe_scale_factor;

  float2 top = lerp(v0, v1, f.x);
  float2 bot = lerp(v2, v3, f.x);
  float2 flow = lerp(top, bot, f.y);

  xe_expanded_flow[id.xy] = flow;
}
