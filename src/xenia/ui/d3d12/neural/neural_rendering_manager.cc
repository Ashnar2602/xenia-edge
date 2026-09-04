/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik and the Xenia Edge Community. All rights reserved.*
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/d3d12/neural/neural_rendering_manager.h"

#if XE_PLATFORM_WIN32

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/d3d12/neural/nvidia_optical_flow_estimator.h"
#include "xenia/ui/d3d12/neural/synthetic_ngx_session.h"

DECLARE_bool(d3d12_neural_rendering);

namespace xe {
namespace ui {
namespace d3d12 {
namespace neural {

std::unique_ptr<NeuralRenderingManager> NeuralRenderingManager::Create(
    ID3D12Device* device, ID3D12CommandQueue* direct_queue) {
  if (!device) {
    return nullptr;
  }
  return std::make_unique<NeuralRenderingManager>(device, direct_queue);
}

NeuralRenderingManager::NeuralRenderingManager(ID3D12Device* device,
                                               ID3D12CommandQueue* direct_queue)
    : device_(device), direct_queue_(direct_queue) {
  XELOGI(
      "NeuralRenderingManager: Initialized on D3D12 device {:#x}",
      reinterpret_cast<uintptr_t>(device_));

  ngx_session_ = SyntheticNgxSession::Create(device_, direct_queue);
  motion_estimator_ =
      NvidiaOpticalFlowEstimator::Create(device_, direct_queue_);
  if (motion_estimator_) {
    XELOGI(
        "NeuralRenderingManager: Native D3D12 hardware optical flow estimator "
        "created");
  } else {
    XELOGI(
        "NeuralRenderingManager: Optical flow estimator not available on this "
        "hardware");
  }
}

NeuralRenderingManager::~NeuralRenderingManager() {
  motion_estimator_.reset();
  ngx_session_.reset();
  XELOGI("NeuralRenderingManager: Shutdown");
}

ID3D12Resource* NeuralRenderingManager::Process(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* input_guest_output) {
  if (!input_guest_output) {
    return nullptr;
  }

  // Fast path: if feature is disabled via CVar, return immediately.
  if (!cvars::d3d12_neural_rendering) {
    return input_guest_output;
  }

  const D3D12_RESOURCE_DESC desc = input_guest_output->GetDesc();
  const uint32_t width = uint32_t(desc.Width);
  const uint32_t height = desc.Height;
  const DXGI_FORMAT format = desc.Format;

  // Track dynamic changes in guest output dimensions or format.
  if (width != current_width_ || height != current_height_ ||
      format != current_format_) {
    XELOGI(
        "NeuralRenderingManager: Guest output format/resolution changed: "
        "{}x{} (fmt {}) -> {}x{} (fmt {})",
        current_width_, current_height_, uint32_t(current_format_), width,
        height, uint32_t(format));

    current_width_ = width;
    current_height_ = height;
    current_format_ = format;

    if (ngx_session_) {
      ngx_session_->Invalidate();
    }
    if (motion_estimator_) {
      motion_estimator_->Initialize(device_, direct_queue_, width, height);
    }
  }

  if (!logged_first_frame_) {
    logged_first_frame_ = true;
    XELOGI(
        "NeuralRenderingManager: First frame captured: {}x{} format {}",
        current_width_, current_height_, uint32_t(current_format_));
  }

  // Execute motion estimation for the current frame.
  if (motion_estimator_ && motion_estimator_->is_valid()) {
    motion_estimator_->EstimateMotion(command_list, input_guest_output,
                                      frame_index_++);
  }

  // Ensure the synthetic NGX session feature is initialized for current resolution.
  if (ngx_session_ && !ngx_session_->IsUnavailable() && command_list) {
    ngx_session_->EnsureFeature(command_list, current_width_, current_height_,
                                current_format_);
  }

  // Commit 3 requirement: Session remains passthrough, do NOT replace gameplay output yet.
  return input_guest_output;
}

}  // namespace neural
}  // namespace d3d12
}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WIN32
