/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik and the Xenia Edge Community. All rights reserved.*
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_D3D12_NEURAL_NEURAL_RENDERING_MANAGER_H_
#define XENIA_UI_D3D12_NEURAL_NEURAL_RENDERING_MANAGER_H_

#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32

#include "xenia/ui/d3d12/d3d12_api.h"
#include <wrl/client.h>

#include <cstdint>
#include <memory>

#include "xenia/ui/d3d12/d3d12_presenter.h"

namespace xe {
namespace ui {
namespace d3d12 {
namespace neural {

class SyntheticNgxSession;
class MotionEstimator;
class DepthProvider;

class NeuralRenderingManager {
 public:
  static std::unique_ptr<NeuralRenderingManager> Create(
      ID3D12Device* device, ID3D12CommandQueue* direct_queue = nullptr);

  explicit NeuralRenderingManager(ID3D12Device* device,
                                  ID3D12CommandQueue* direct_queue = nullptr);
  ~NeuralRenderingManager();

  // Processes the guest output.
  // In Commit 2/3/4, coordinates SyntheticNgxSession, MotionEstimator, and DepthProvider.
  // strictly returns input_guest_output (passthrough) unless debug view is enabled.
  ID3D12Resource* Process(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* input_guest_output,
      uint64_t guest_generation = 0,
      const D3D12Presenter::GuestDepthCandidate& depth_candidate = {});

  void OnFrameSubmitted(ID3D12CommandQueue* direct_queue);

  uint32_t current_width() const { return current_width_; }
  uint32_t current_height() const { return current_height_; }
  DXGI_FORMAT current_format() const { return current_format_; }
  uint64_t last_guest_generation() const { return last_guest_generation_; }

  SyntheticNgxSession* ngx_session() const { return ngx_session_.get(); }
  MotionEstimator* motion_estimator() const { return motion_estimator_.get(); }
  DepthProvider* depth_provider() const { return depth_provider_.get(); }

 private:
  ID3D12Device* device_ = nullptr;
  ID3D12CommandQueue* direct_queue_ = nullptr;
  std::unique_ptr<SyntheticNgxSession> ngx_session_;
  std::unique_ptr<MotionEstimator> motion_estimator_;
  std::unique_ptr<DepthProvider> depth_provider_;

  uint32_t current_width_ = 0;
  uint32_t current_height_ = 0;
  DXGI_FORMAT current_format_ = DXGI_FORMAT_UNKNOWN;
  uint64_t frame_index_ = 0;
  uint64_t last_guest_generation_ = 0;

  bool logged_first_frame_ = false;
};

}  // namespace neural
}  // namespace d3d12
}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WIN32

#endif  // XENIA_UI_D3D12_NEURAL_NEURAL_RENDERING_MANAGER_H_
