/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik and the Xenia Edge Community. All rights reserved.*
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

cbuffer XeNeuralColorConvertConstants : register(b0) {
  uint2 xe_resolution;
};

Texture2D<float4> xe_input_color : register(t0);
RWTexture2D<float4> xe_output_color : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  [branch] if (any(id.xy >= xe_resolution)) {
    return;
  }
  float4 c = xe_input_color.Load(int3(id.xy, 0));
  xe_output_color[id.xy] = float4(c.rgb, 1.0f);
}
