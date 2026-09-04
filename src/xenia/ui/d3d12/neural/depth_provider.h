/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik and the Xenia Edge Community. All rights reserved.*
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_D3D12_NEURAL_DEPTH_PROVIDER_H_
#define XENIA_UI_D3D12_NEURAL_DEPTH_PROVIDER_H_

#include "xenia/base/cvar.h"
#include "xenia/ui/d3d12/d3d12_api.h"
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <string>

#include "xenia/ui/d3d12/d3d12_presenter.h"

DECLARE_string(d3d12_neural_depth_mode);
DECLARE_bool(d3d12_neural_depth_debug_view);

namespace xe {
namespace ui {
namespace d3d12 {
namespace neural {

struct DepthFrame {
  ID3D12Resource* resource = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  bool inverted = false;
  bool valid = false;
  uint64_t guest_generation = 0;
  bool depth_history_discontinuity = false;
  float depth_near = 0.0f;
  float depth_far = 1.0f;
  uint32_t confidence = 0;
  float score = 0.0f;
  float score_margin = 0.0f;
};

class DepthProvider {
 public:
  DepthProvider();
  ~DepthProvider();

  bool Initialize(ID3D12Device* device, ID3D12CommandQueue* direct_queue);

  void ProcessFrame(
      ID3D12GraphicsCommandList* command_list,
      const D3D12Presenter::GuestDepthCandidate& candidate,
      uint32_t target_width, uint32_t target_height, uint64_t guest_generation);

  void OnFrameSubmitted(ID3D12CommandQueue* direct_queue);

  const DepthFrame& GetCurrentDepthFrame() const { return current_frame_; }

  bool IsGenerationValidated(uint64_t guest_generation) const;

  uint32_t candidate_switch_count() const { return candidate_switch_count_; }
  float candidate_switches_per_minute() const {
    return candidate_switches_per_minute_;
  }

  void Invalidate();

 private:
  static constexpr size_t kMaxAsyncSlots = 4;

  struct AsyncValidationSlot {
    uint64_t guest_generation = 0;
    ID3D12Resource* candidate_resource = nullptr;
    uint64_t fence_value = 0;
    bool pending = false;
    bool is_valid = false;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_buffer;
  };

  bool CreatePipelines();
  bool EnsureResources(uint32_t width, uint32_t height);
  void ReleaseResources();
  void RetireCompletedValidationSlots();

  ID3D12Device* device_ = nullptr;
  ID3D12CommandQueue* direct_queue_ = nullptr;

  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_1x_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_msaa_;

  Microsoft::WRL::ComPtr<ID3D12Resource> output_depth_resource_;
  Microsoft::WRL::ComPtr<ID3D12Resource> range_buffer_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap_;

  Microsoft::WRL::ComPtr<ID3D12Fence> validation_fence_;
  uint64_t next_fence_value_ = 0;
  uint64_t pending_submit_fence_value_ = 0;
  size_t current_slot_index_ = 0;
  std::array<AsyncValidationSlot, kMaxAsyncSlots> async_slots_;

  uint32_t current_width_ = 0;
  uint32_t current_height_ = 0;
  bool is_initialized_ = false;

  DepthFrame current_frame_;
  ID3D12Resource* last_candidate_resource_ = nullptr;
  bool last_frame_was_valid_ = false;
  bool last_inverted_ = false;
  uint32_t last_cand_width_ = 0;
  uint32_t last_cand_height_ = 0;

  // Temporal stability tracking
  uint32_t candidate_switch_count_ = 0;
  uint64_t start_time_ticks_ = 0;
  float candidate_switches_per_minute_ = 0.0f;
};

}  // namespace neural
}  // namespace d3d12
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_D3D12_NEURAL_DEPTH_PROVIDER_H_
