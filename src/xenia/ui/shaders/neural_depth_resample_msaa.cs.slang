/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik and the Xenia Edge Community. All rights reserved.*
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

cbuffer XeNeuralDepthResampleConstants : register(b0) {
  uint2 xe_output_size;
  uint2 xe_input_size;
  float2 xe_vp_scale;
  float2 xe_vp_offset;
  uint xe_is_float24;
  uint xe_sample_count;
  uint xe_inverted;
  uint xe_explicit_sample;
};

Texture2DMS<float> xe_input_depth_ms : register(t0);
RWTexture2D<float> xe_output_depth : register(u0);
RWByteAddressBuffer xe_depth_range : register(u1);

groupshared uint s_group_min;
groupshared uint s_group_max;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint group_index : SV_GroupIndex) {
  if (group_index == 0) {
    s_group_min = 0xFFFFFFFF;
    s_group_max = 0;
  }
  GroupMemoryBarrierWithGroupSync();

  bool valid_pixel = all(id.xy < xe_output_size);
  if (valid_pixel) {
    float2 uv = (float2(id.xy) + 0.5f) / float2(xe_output_size);
    float2 src_uv = xe_vp_offset + uv * xe_vp_scale;
    int2 src_coord = clamp(int2(src_uv * float2(xe_input_size)), int2(0, 0), int2(xe_input_size) - 1);

    float chosen_d;
    if (xe_explicit_sample < xe_sample_count) {
      // Policy A: Guest specified an explicit single-sample resolve
      chosen_d = xe_input_depth_ms.Load(src_coord, xe_explicit_sample);
    } else {
      // Policy B: Geometric closest-to-camera policy for DLSS reconstruction:
      // Standard-Z: minimum depth (closest surface has smallest Z)
      // Reversed-Z: maximum depth (closest surface has largest Z)
      chosen_d = xe_input_depth_ms.Load(src_coord, 0);
      for (uint s = 1; s < xe_sample_count; ++s) {
        float sample_d = xe_input_depth_ms.Load(src_coord, s);
        if (xe_inverted != 0) {
          chosen_d = max(chosen_d, sample_d);
        } else {
          chosen_d = min(chosen_d, sample_d);
        }
      }
    }

    // kD24FS8 in HostRenderTargets is scaled by 0.5 into [0, 0.5).
    // Multiply by 2.0 to restore the full [0, 1] projected hardware depth range for NGX.
    if (xe_is_float24 != 0) {
      chosen_d = chosen_d * 2.0f;
    }
    chosen_d = saturate(chosen_d);
    xe_output_depth[id.xy] = chosen_d;

    uint d_as_uint = asuint(chosen_d);
    InterlockedMin(s_group_min, d_as_uint);
    InterlockedMax(s_group_max, d_as_uint);
  }

  GroupMemoryBarrierWithGroupSync();

  if (group_index == 0) {
    if (s_group_min != 0xFFFFFFFF) {
      xe_depth_range.InterlockedMin(0, s_group_min);
      xe_depth_range.InterlockedMax(4, s_group_max);
    }
  }
}
