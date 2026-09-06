/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik and the Xenia Edge Community. All rights reserved.*
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_D3D12_NEURAL_NVIDIA_OPTICAL_FLOW_ESTIMATOR_H_
#define XENIA_UI_D3D12_NEURAL_NVIDIA_OPTICAL_FLOW_ESTIMATOR_H_

#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

#include "xenia/ui/d3d12/neural/motion_estimator.h"
#include "xenia/ui/d3d12/neural/nvidia_optical_flow_abi.h"

namespace xe {
namespace ui {
namespace d3d12 {
namespace neural {

class NvidiaOpticalFlowEstimator final : public MotionEstimator {
 public:
  static std::unique_ptr<NvidiaOpticalFlowEstimator> Create(
      ID3D12Device* device, ID3D12CommandQueue* direct_queue);

  explicit NvidiaOpticalFlowEstimator(ID3D12Device* device,
                                      ID3D12CommandQueue* direct_queue);
  ~NvidiaOpticalFlowEstimator() override;

  MotionEstimatorType type() const override {
    return MotionEstimatorType::kOpticalFlowHardware;
  }
  bool is_valid() const override { return is_initialized_; }

  bool Initialize(ID3D12Device* device, ID3D12CommandQueue* command_queue,
                  uint32_t width, uint32_t height) override;
  void Shutdown() override;

  bool EstimateMotion(ID3D12GraphicsCommandList* command_list,
                      ID3D12Resource* current_color,
                      uint64_t frame_index) override;

  ID3D12Resource* GetMotionVectors() const override {
    return full_flow_buffer_.Get();
  }

  ID3D12Fence* GetOutputFence() const override { return of_fence_.Get(); }
  uint64_t GetOutputFenceValue() const override {
    return last_ready_fence_value_;
  }

  void GetMotionVectorScale(float& out_scale_x,
                            float& out_scale_y) const override {
    out_scale_x = 1.0f;
    out_scale_y = 1.0f;
  }

  uint32_t grid_size() const { return static_cast<uint32_t>(grid_size_); }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }

  uint32_t GetResourceCount() const override {
    uint32_t count = 0;
    if (input_buffers_[0]) count++;
    if (input_buffers_[1]) count++;
    if (raw_flow_buffer_) count++;
    if (full_flow_buffer_) count++;
    return count;
  }
  uint32_t GetDescriptorCount() const override {
    return descriptor_heap_ ? 4 : 0;
  }

 private:
  bool LoadNvOfApi();
  bool CreatePipelines();
  bool AllocateResources(uint32_t width, uint32_t height);
  void ReleaseResources();

  ID3D12Device* device_ = nullptr;
  ID3D12CommandQueue* direct_queue_ = nullptr;

  HMODULE h_nvof_dll_ = nullptr;
  NV_OF_D3D12_API_FUNCTION_LIST of_api_ = {};
  NvOFHandle of_handle_ = nullptr;

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  NV_OF_OUTPUT_VECTOR_GRID_SIZE grid_size_ = NV_OF_OUTPUT_VECTOR_GRID_SIZE_2;
  uint32_t grid_width_ = 0;
  uint32_t grid_height_ = 0;

  bool is_initialized_ = false;
  uint64_t fence_value_ = 0;
  uint64_t last_ready_fence_value_ = 0;
  Microsoft::WRL::ComPtr<ID3D12Fence> of_fence_;

  static constexpr size_t kNumAllocators = 3;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator>
      command_allocators_[kNumAllocators];
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> estimator_command_list_;
  uint64_t allocator_fence_values_[kNumAllocators] = {};

  // Ping-pong input textures (B8G8R8A8_UNORM)
  Microsoft::WRL::ComPtr<ID3D12Resource> input_buffers_[2];
  NvOFGPUBufferHandle h_input_buffers_[2] = {};

  // Raw NVOFA flow buffer (R16G16_SINT at grid resolution)
  Microsoft::WRL::ComPtr<ID3D12Resource> raw_flow_buffer_;
  NvOFGPUBufferHandle h_raw_flow_buffer_ = nullptr;

  // Final full-resolution motion vectors (R16G16_FLOAT at full resolution)
  Microsoft::WRL::ComPtr<ID3D12Resource> full_flow_buffer_;

  // Compute pipelines for format conversion & grid expansion
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap_;
  UINT descriptor_size_ = 0;

  Microsoft::WRL::ComPtr<ID3D12RootSignature> color_convert_root_sig_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> color_convert_pso_;

  Microsoft::WRL::ComPtr<ID3D12RootSignature> vector_expand_root_sig_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> vector_expand_pso_;

  bool has_previous_frame_ = false;
};

}  // namespace neural
}  // namespace d3d12
}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WIN32

#endif  // XENIA_UI_D3D12_NEURAL_NVIDIA_OPTICAL_FLOW_ESTIMATOR_H_
