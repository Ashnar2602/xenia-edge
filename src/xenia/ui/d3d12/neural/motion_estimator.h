/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik and the Xenia Edge Community. All rights reserved.*
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_D3D12_NEURAL_MOTION_ESTIMATOR_H_
#define XENIA_UI_D3D12_NEURAL_MOTION_ESTIMATOR_H_

#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <memory>

namespace xe {
namespace ui {
namespace d3d12 {
namespace neural {

enum class MotionEstimatorType {
  kNone = 0,
  kOpticalFlowHardware = 1,  // NVIDIA Optical Flow Accelerator (D3D12 native)
  kComputeShader = 2,        // Compute shader motion estimation fallback
};

class MotionEstimator {
 public:
  virtual ~MotionEstimator() = default;

  virtual MotionEstimatorType type() const = 0;
  virtual bool is_valid() const = 0;

  // Allocates resources and initializes session for width x height.
  virtual bool Initialize(ID3D12Device* device,
                          ID3D12CommandQueue* command_queue, uint32_t width,
                          uint32_t height) = 0;

  // Releases all GPU resources and terminates session.
  virtual void Shutdown() = 0;

  // Estimates motion between previous and current frame.
  // command_list may be used for recording conversion passes on direct_queue.
  virtual bool EstimateMotion(ID3D12GraphicsCommandList* command_list,
                              ID3D12Resource* current_color,
                              uint64_t frame_index) = 0;

  // Full-resolution motion vector resource (DXGI_FORMAT_R16G16_FLOAT).
  // Direction: current -> previous (NGX contract).
  virtual ID3D12Resource* GetMotionVectors() const = 0;

  // GPU fence and completion value for motion vector availability.
  // Consumers must wait on this fence GPU-side before reading GetMotionVectors().
  virtual ID3D12Fence* GetOutputFence() const = 0;
  virtual uint64_t GetOutputFenceValue() const = 0;

  // Motion vector scale factor according to NGX contract: (1.0f, 1.0f).
  virtual void GetMotionVectorScale(float& out_scale_x,
                                    float& out_scale_y) const {
    out_scale_x = 1.0f;
    out_scale_y = 1.0f;
  }
};

}  // namespace neural
}  // namespace d3d12
}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WIN32

#endif  // XENIA_UI_D3D12_NEURAL_MOTION_ESTIMATOR_H_
