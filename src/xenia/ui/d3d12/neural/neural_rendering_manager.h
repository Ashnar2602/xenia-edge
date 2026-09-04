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

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace xe {
namespace ui {
namespace d3d12 {
namespace neural {

class SyntheticNgxSession;
class MotionEstimator;

class NeuralRenderingManager {
 public:
  static std::unique_ptr<NeuralRenderingManager> Create(
      ID3D12Device* device, ID3D12CommandQueue* direct_queue = nullptr);

  explicit NeuralRenderingManager(ID3D12Device* device,
                                  ID3D12CommandQueue* direct_queue = nullptr);
  ~NeuralRenderingManager();

  // Processes the guest output.
  // In Commit 2, bootstraps and coordinates SyntheticNgxSession, but strictly
  // returns input_guest_output (passthrough) since temporal inputs (MV + Depth)
  // are not yet wired up.
  ID3D12Resource* Process(ID3D12GraphicsCommandList* command_list,
                          ID3D12Resource* input_guest_output);

  uint32_t current_width() const { return current_width_; }
  uint32_t current_height() const { return current_height_; }
  DXGI_FORMAT current_format() const { return current_format_; }

  SyntheticNgxSession* ngx_session() const { return ngx_session_.get(); }
  MotionEstimator* motion_estimator() const { return motion_estimator_.get(); }

 private:
  ID3D12Device* device_ = nullptr;
  ID3D12CommandQueue* direct_queue_ = nullptr;
  std::unique_ptr<SyntheticNgxSession> ngx_session_;
  std::unique_ptr<MotionEstimator> motion_estimator_;

  uint32_t current_width_ = 0;
  uint32_t current_height_ = 0;
  DXGI_FORMAT current_format_ = DXGI_FORMAT_UNKNOWN;
  uint64_t frame_index_ = 0;

  bool logged_first_frame_ = false;
};

}  // namespace neural
}  // namespace d3d12
}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WIN32

#endif  // XENIA_UI_D3D12_NEURAL_NEURAL_RENDERING_MANAGER_H_
