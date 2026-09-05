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

struct PixelDiffMetrics {
  bool measured = false;
  uint64_t evaluated_frame = 0;
  double rms = 0.0;
  double mad = 0.0;
  double max_diff = 0.0;
  double pct_changed = 0.0;
  uint64_t total_pixels = 0;
  uint64_t changed_pixels = 0;
};

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

  uint64_t total_frames() const { return total_frames_; }
  uint64_t evaluated_frames() const { return evaluated_frames_; }
  uint64_t bypassed_frames() const { return bypassed_frames_; }
  uint64_t reset_frames() const { return reset_frames_; }
  uint64_t contract_failures() const { return contract_failures_; }
  uint64_t split_blits() const { return split_blits_; }

  const PixelDiffMetrics& pixel_diff_metrics() const {
    return pixel_diff_metrics_;
  }

  double last_motion_time_ms() const { return last_motion_time_ms_; }
  double last_depth_time_ms() const { return last_depth_time_ms_; }
  double last_ngx_time_ms() const { return last_ngx_time_ms_; }
  double last_blit_time_ms() const { return last_blit_time_ms_; }
  double last_total_pipeline_time_ms() const { return last_total_pipeline_time_ms_; }

  double avg_motion_time_ms() const {
    return evaluated_frames_ > 0 ? (total_motion_time_ms_ / evaluated_frames_) : 0.0;
  }
  double avg_depth_time_ms() const {
    return evaluated_frames_ > 0 ? (total_depth_time_ms_ / evaluated_frames_) : 0.0;
  }
  double avg_ngx_time_ms() const {
    return evaluated_frames_ > 0 ? (total_ngx_time_ms_ / evaluated_frames_) : 0.0;
  }
  double avg_blit_time_ms() const {
    return split_blits_ > 0 ? (total_blit_time_ms_ / split_blits_) : 0.0;
  }
  double avg_total_pipeline_time_ms() const {
    return evaluated_frames_ > 0 ? (total_pipeline_time_ms_ / evaluated_frames_) : 0.0;
  }

 private:
  bool EnsureSplitResource(uint32_t width, uint32_t height, DXGI_FORMAT format);
  ID3D12Resource* BlitSplitOutput(ID3D12GraphicsCommandList* command_list,
                                  ID3D12Resource* original,
                                  ID3D12Resource* neural, uint32_t width,
                                  uint32_t height);

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

  Microsoft::WRL::ComPtr<ID3D12Resource> split_output_resource_;
  D3D12_RESOURCE_STATES split_resource_state_ = D3D12_RESOURCE_STATE_COMMON;
  uint32_t split_width_ = 0;
  uint32_t split_height_ = 0;
  DXGI_FORMAT split_format_ = DXGI_FORMAT_UNKNOWN;

  bool need_history_reset_ = true;
  uint64_t last_depth_candidate_resource_ = 0;
  int last_dfc_state_ = -2;
  int last_trust_state_ = 0;

  uint64_t total_frames_ = 0;
  uint64_t evaluated_frames_ = 0;
  uint64_t bypassed_frames_ = 0;
  uint64_t reset_frames_ = 0;
  uint64_t contract_failures_ = 0;
  uint64_t split_blits_ = 0;

  double last_motion_time_ms_ = 0.0;
  double last_depth_time_ms_ = 0.0;
  double last_ngx_time_ms_ = 0.0;
  double last_blit_time_ms_ = 0.0;
  double last_total_pipeline_time_ms_ = 0.0;

  double total_motion_time_ms_ = 0.0;
  double total_depth_time_ms_ = 0.0;
  double total_ngx_time_ms_ = 0.0;
  double total_blit_time_ms_ = 0.0;
  double total_pipeline_time_ms_ = 0.0;

  bool logged_first_frame_ = false;

  void ScheduleDiagnosticReadback(ID3D12GraphicsCommandList* command_list,
                                  ID3D12Resource* original,
                                  ID3D12Resource* processed, uint32_t width,
                                  uint32_t height, DXGI_FORMAT format);
  void ProcessDiagnosticReadback();

  PixelDiffMetrics pixel_diff_metrics_;
  bool readback_scheduled_ = false;
  bool readback_completed_ = false;
  Microsoft::WRL::ComPtr<ID3D12Resource> readback_original_buffer_;
  Microsoft::WRL::ComPtr<ID3D12Resource> readback_processed_buffer_;
  uint32_t readback_width_ = 0;
  uint32_t readback_height_ = 0;
  DXGI_FORMAT readback_format_ = DXGI_FORMAT_UNKNOWN;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT readback_footprint_ = {};
  Microsoft::WRL::ComPtr<ID3D12Fence> readback_fence_;
  uint64_t readback_fence_value_ = 0;
};

}  // namespace neural
}  // namespace d3d12
}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WIN32

#endif  // XENIA_UI_D3D12_NEURAL_NEURAL_RENDERING_MANAGER_H_
