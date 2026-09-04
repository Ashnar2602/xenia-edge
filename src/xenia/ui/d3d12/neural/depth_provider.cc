/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik and the Xenia Edge Community. All rights reserved.*
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/d3d12/neural/depth_provider.h"

#include <algorithm>
#include <cmath>

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/d3d12/d3d12_util.h"

namespace shaders {
#include "xenia/ui/shaders/bytecode/d3d12_dxil/neural_depth_resample_cs.h"
#include "xenia/ui/shaders/bytecode/d3d12_dxil/neural_depth_resample_msaa_cs.h"
}  // namespace shaders

DEFINE_string(
    d3d12_neural_depth_mode, "auto",
    "Depth inversion mode for neural rendering: auto | standard | reversed",
    "GPU");

DEFINE_bool(
    d3d12_neural_depth_debug_view, false,
    "Debug view: display raw depth grayscale on screen instead of color",
    "GPU");

namespace xe {
namespace ui {
namespace d3d12 {
namespace neural {

struct DepthResampleConstants {
  uint32_t output_width;
  uint32_t output_height;
  uint32_t input_width;
  uint32_t input_height;
  float vp_scale_x;
  float vp_scale_y;
  float vp_offset_x;
  float vp_offset_y;
  uint32_t is_float24;
  uint32_t sample_count;
  uint32_t inverted;
  uint32_t explicit_sample;
};
static_assert(sizeof(DepthResampleConstants) == 48, "Constant buffer size mismatch");

DepthProvider::DepthProvider() = default;

DepthProvider::~DepthProvider() {
  ReleaseResources();
}

bool DepthProvider::Initialize(ID3D12Device* device,
                               ID3D12CommandQueue* direct_queue) {
  if (!device || !direct_queue) {
    return false;
  }
  device_ = device;
  direct_queue_ = direct_queue;

  if (!CreatePipelines()) {
    XELOGE("DepthProvider: Failed to create compute pipelines");
    return false;
  }

  HRESULT hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(&validation_fence_));
  if (FAILED(hr)) {
    XELOGE("DepthProvider: Failed to create async validation fence");
    return false;
  }
  next_fence_value_ = 0;
  pending_submit_fence_value_ = 0;
  start_time_ticks_ = xe::Clock::QueryHostTickCount();

  is_initialized_ = true;
  return true;
}

bool DepthProvider::CreatePipelines() {
  // Root signature:
  // Param 0: 32-bit constants (12 values: DepthResampleConstants)
  // Param 1: SRV table (1 descriptor: t0)
  // Param 2: UAV table (2 descriptors: u0 output depth, u1 range buffer)

  D3D12_DESCRIPTOR_RANGE srv_range = {};
  srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  srv_range.NumDescriptors = 1;
  srv_range.BaseShaderRegister = 0;
  srv_range.RegisterSpace = 0;
  srv_range.OffsetInDescriptorsFromTableStart = 0;

  D3D12_DESCRIPTOR_RANGE uav_range = {};
  uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  uav_range.NumDescriptors = 2;
  uav_range.BaseShaderRegister = 0;
  uav_range.RegisterSpace = 0;
  uav_range.OffsetInDescriptorsFromTableStart = 0;

  D3D12_ROOT_PARAMETER root_params[3] = {};
  root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  root_params[0].Constants.ShaderRegister = 0;
  root_params[0].Constants.RegisterSpace = 0;
  root_params[0].Constants.Num32BitValues = 12;
  root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_params[1].DescriptorTable.NumDescriptorRanges = 1;
  root_params[1].DescriptorTable.pDescriptorRanges = &srv_range;
  root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  root_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_params[2].DescriptorTable.NumDescriptorRanges = 1;
  root_params[2].DescriptorTable.pDescriptorRanges = &uav_range;
  root_params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC rs_desc = {};
  rs_desc.NumParameters = 3;
  rs_desc.pParameters = root_params;
  rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  HMODULE d3d12_mod = GetModuleHandleA("d3d12.dll");
  if (!d3d12_mod) {
    d3d12_mod = LoadLibraryA("d3d12.dll");
  }
  auto pfn_serialize_rs = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
      GetProcAddress(d3d12_mod, "D3D12SerializeRootSignature"));
  if (!pfn_serialize_rs) {
    XELOGE("DepthProvider: Failed to locate D3D12SerializeRootSignature");
    return false;
  }

  Microsoft::WRL::ComPtr<ID3DBlob> blob;
  Microsoft::WRL::ComPtr<ID3DBlob> error_blob;
  HRESULT hr = pfn_serialize_rs(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob,
                                &error_blob);
  if (FAILED(hr)) {
    XELOGE("DepthProvider: Failed to serialize root signature");
    return false;
  }

  hr = device_->CreateRootSignature(0, blob->GetBufferPointer(),
                                    blob->GetBufferSize(),
                                    IID_PPV_ARGS(&root_signature_));
  if (FAILED(hr)) {
    XELOGE("DepthProvider: Failed to create root signature");
    return false;
  }

  // 1x pipeline
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc_1x = {};
  pso_desc_1x.pRootSignature = root_signature_.Get();
  pso_desc_1x.CS.pShaderBytecode = shaders::neural_depth_resample_cs;
  pso_desc_1x.CS.BytecodeLength = sizeof(shaders::neural_depth_resample_cs);
  hr = device_->CreateComputePipelineState(&pso_desc_1x,
                                           IID_PPV_ARGS(&pso_1x_));
  if (FAILED(hr)) {
    XELOGE("DepthProvider: Failed to create 1x compute pipeline");
    return false;
  }

  // MSAA pipeline
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc_msaa = {};
  pso_desc_msaa.pRootSignature = root_signature_.Get();
  pso_desc_msaa.CS.pShaderBytecode = shaders::neural_depth_resample_msaa_cs;
  pso_desc_msaa.CS.BytecodeLength = sizeof(shaders::neural_depth_resample_msaa_cs);
  hr = device_->CreateComputePipelineState(&pso_desc_msaa,
                                           IID_PPV_ARGS(&pso_msaa_));
  if (FAILED(hr)) {
    XELOGE("DepthProvider: Failed to create MSAA compute pipeline");
    return false;
  }

  return true;
}

bool DepthProvider::EnsureResources(uint32_t width, uint32_t height) {
  if (output_depth_resource_ && current_width_ == width &&
      current_height_ == height) {
    return true;
  }

  ReleaseResources();

  current_width_ = width;
  current_height_ = height;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R32_FLOAT;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  HRESULT hr = device_->CreateCommittedResource(
      &util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
      nullptr, IID_PPV_ARGS(&output_depth_resource_));
  if (FAILED(hr)) {
    XELOGE("DepthProvider: Failed to create output depth texture {}x{}", width,
           height);
    return false;
  }

  // Range buffer: 8 bytes (min uint32, max uint32)
  D3D12_RESOURCE_DESC buf_desc = {};
  buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buf_desc.Width = 8;
  buf_desc.Height = 1;
  buf_desc.DepthOrArraySize = 1;
  buf_desc.MipLevels = 1;
  buf_desc.Format = DXGI_FORMAT_UNKNOWN;
  buf_desc.SampleDesc.Count = 1;
  buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  buf_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  hr = device_->CreateCommittedResource(
      &util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &buf_desc,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
      IID_PPV_ARGS(&range_buffer_));
  if (FAILED(hr)) {
    XELOGE("DepthProvider: Failed to create range buffer");
    return false;
  }

  // 4 Async readback buffers
  D3D12_RESOURCE_DESC rb_desc = buf_desc;
  rb_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  for (size_t i = 0; i < kMaxAsyncSlots; ++i) {
    hr = device_->CreateCommittedResource(
        &util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE, &rb_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&async_slots_[i].readback_buffer));
    if (FAILED(hr)) {
      XELOGE("DepthProvider: Failed to create async readback buffer slot {}", i);
      return false;
    }
    async_slots_[i].pending = false;
    async_slots_[i].is_valid = false;
    async_slots_[i].guest_generation = 0;
    async_slots_[i].candidate_resource = nullptr;
    async_slots_[i].fence_value = 0;
  }

  // Descriptor heap: 4 descriptors
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = 4;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  hr = device_->CreateDescriptorHeap(&heap_desc,
                                     IID_PPV_ARGS(&descriptor_heap_));
  if (FAILED(hr)) {
    XELOGE("DepthProvider: Failed to create descriptor heap");
    return false;
  }

  return true;
}

void DepthProvider::ReleaseResources() {
  output_depth_resource_.Reset();
  range_buffer_.Reset();
  for (auto& slot : async_slots_) {
    slot.readback_buffer.Reset();
    slot.pending = false;
    slot.is_valid = false;
  }
  descriptor_heap_.Reset();
  current_width_ = 0;
  current_height_ = 0;
  current_frame_ = {};
  last_frame_was_valid_ = false;
  last_candidate_resource_ = nullptr;
}

void DepthProvider::Invalidate() {
  current_frame_.valid = false;
  current_frame_.resource = nullptr;
  last_frame_was_valid_ = false;
}

void DepthProvider::RetireCompletedValidationSlots() {
  if (!validation_fence_) {
    return;
  }
  uint64_t completed = validation_fence_->GetCompletedValue();
  for (auto& slot : async_slots_) {
    if (slot.pending && completed >= slot.fence_value) {
      slot.pending = false;
      if (slot.readback_buffer) {
        D3D12_RANGE read_range = {0, 8};
        void* p_mapped = nullptr;
        if (SUCCEEDED(slot.readback_buffer->Map(0, &read_range, &p_mapped)) &&
            p_mapped) {
          const uint32_t* p = static_cast<const uint32_t*>(p_mapped);
          uint32_t min_u = p[0];
          uint32_t max_u = p[1];
          float min_f = *reinterpret_cast<const float*>(&min_u);
          float max_f = *reinterpret_cast<const float*>(&max_u);
          if (min_u != 0xFFFFFFFF && std::abs(max_f - min_f) >= 0.0001f) {
            slot.is_valid = true;
          } else {
            slot.is_valid = false;
          }
          D3D12_RANGE written_range = {0, 0};
          slot.readback_buffer->Unmap(0, &written_range);
        }
      }
    }
  }
}

bool DepthProvider::IsGenerationValidated(uint64_t guest_generation) const {
  for (const auto& slot : async_slots_) {
    if (!slot.pending && slot.guest_generation == guest_generation) {
      return slot.is_valid;
    }
  }
  return false;
}

void DepthProvider::OnFrameSubmitted(ID3D12CommandQueue* direct_queue) {
  if (pending_submit_fence_value_ > 0 && validation_fence_ && direct_queue) {
    direct_queue->Signal(validation_fence_.Get(), pending_submit_fence_value_);
    pending_submit_fence_value_ = 0;
  }
}

void DepthProvider::ProcessFrame(
    ID3D12GraphicsCommandList* command_list,
    const D3D12Presenter::GuestDepthCandidate& candidate,
    uint32_t target_width, uint32_t target_height,
    uint64_t guest_generation) {
  RetireCompletedValidationSlots();

  // Validate candidate criteria:
  // - Resource and valid flag
  // - Confidence classification (UNKNOWN -> invalid -> bypass)
  // - Ambiguity check (margin < 5.0 -> invalid -> bypass)
  if (!is_initialized_ || !command_list || !candidate.valid ||
      !candidate.resource || candidate.confidence == 0 ||
      (candidate.second_best_score > 0.0f && candidate.score_margin < 5.0f)) {
    Invalidate();
    return;
  }

  if (!EnsureResources(target_width, target_height)) {
    Invalidate();
    return;
  }

  // Temporal stability and discontinuity detection
  bool discontinuity = false;
  if (last_candidate_resource_ != candidate.resource) {
    candidate_switch_count_++;
    uint64_t now_ticks = xe::Clock::QueryHostTickCount();
    if (start_time_ticks_ == 0) {
      start_time_ticks_ = now_ticks;
    }
    double elapsed_minutes =
        double(now_ticks - start_time_ticks_) /
        (double(xe::Clock::QueryHostTickFrequency()) * 60.0);
    if (elapsed_minutes > 0.05) {
      candidate_switches_per_minute_ =
          float(candidate_switch_count_ / elapsed_minutes);
    }
    XELOGI(
        "DepthProvider: Candidate switch #{} -> {}x{} fmt {:X} (switches/min: "
        "{:.2f}, margin: {:.1f})",
        candidate_switch_count_, candidate.width, candidate.height,
        uint32_t(candidate.dxgi_format), candidate_switches_per_minute_,
        candidate.score_margin);
    discontinuity = true;
    last_candidate_resource_ = candidate.resource;
  }

  if (!last_frame_was_valid_) {
    discontinuity = true;
  }
  if (candidate.inverted != last_inverted_) {
    discontinuity = true;
    last_inverted_ = candidate.inverted;
  }
  if (candidate.width != last_cand_width_ ||
      candidate.height != last_cand_height_) {
    discontinuity = true;
    last_cand_width_ = candidate.width;
    last_cand_height_ = candidate.height;
  }
  last_frame_was_valid_ = true;

  // Resolve standard-Z vs reversed-Z (diagnostic override available)
  bool final_inverted = candidate.inverted;
  if (cvars::d3d12_neural_depth_mode == "standard") {
    final_inverted = false;
  } else if (cvars::d3d12_neural_depth_mode == "reversed") {
    final_inverted = true;
  }

  // Viewport and subrect mapping
  float vp_scale_x = 1.0f;
  float vp_scale_y = 1.0f;
  float vp_offset_x = 0.0f;
  float vp_offset_y = 0.0f;
  if (candidate.vp_width > 0 && candidate.vp_height > 0 &&
      candidate.width > 0 && candidate.height > 0) {
    vp_scale_x = float(candidate.vp_width) / float(candidate.width);
    vp_scale_y = float(candidate.vp_height) / float(candidate.height);
    vp_offset_x = float(candidate.vp_x) / float(candidate.width);
    vp_offset_y = float(candidate.vp_y) / float(candidate.height);
  }

  // 1. Transition candidate depth to NON_PIXEL_SHADER_RESOURCE
  D3D12_RESOURCE_BARRIER pre_barriers[2] = {};
  pre_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  pre_barriers[0].Transition.pResource = candidate.resource;
  pre_barriers[0].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  pre_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  pre_barriers[0].Transition.StateAfter =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

  pre_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  pre_barriers[1].Transition.pResource = output_depth_resource_.Get();
  pre_barriers[1].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  pre_barriers[1].Transition.StateBefore =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  pre_barriers[1].Transition.StateAfter =
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

  command_list->ResourceBarrier(2, pre_barriers);

  // 2. Setup descriptors
  uint32_t desc_size = device_->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CPU_DESCRIPTOR_HANDLE cpu_start =
      descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_start =
      descriptor_heap_->GetGPUDescriptorHandleForHeapStart();

  // Descriptor 0: SRV of candidate depth
  D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = candidate.dxgi_format;
  srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (candidate.sample_count > 1) {
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    srv_desc.Texture2D.PlaneSlice = 0;
  }
  device_->CreateShaderResourceView(candidate.resource, &srv_desc, cpu_start);

  // Descriptor 1: UAV of output depth (DXGI_FORMAT_R32_FLOAT)
  D3D12_CPU_DESCRIPTOR_HANDLE uav_depth_cpu = cpu_start;
  uav_depth_cpu.ptr += desc_size;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav_depth_desc = {};
  uav_depth_desc.Format = DXGI_FORMAT_R32_FLOAT;
  uav_depth_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device_->CreateUnorderedAccessView(output_depth_resource_.Get(), nullptr,
                                    &uav_depth_desc, uav_depth_cpu);

  // Descriptor 2: UAV of range buffer (raw ByteAddressBuffer)
  D3D12_CPU_DESCRIPTOR_HANDLE uav_range_cpu = uav_depth_cpu;
  uav_range_cpu.ptr += desc_size;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav_range_desc = {};
  uav_range_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  uav_range_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav_range_desc.Buffer.NumElements = 2;
  uav_range_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device_->CreateUnorderedAccessView(range_buffer_.Get(), nullptr,
                                    &uav_range_desc, uav_range_cpu);

  // 3. Bind and Dispatch
  ID3D12DescriptorHeap* heaps[] = {descriptor_heap_.Get()};
  command_list->SetDescriptorHeaps(1, heaps);
  command_list->SetComputeRootSignature(root_signature_.Get());

  DepthResampleConstants cb{};
  cb.output_width = target_width;
  cb.output_height = target_height;
  cb.input_width = candidate.width;
  cb.input_height = candidate.height;
  cb.vp_scale_x = vp_scale_x;
  cb.vp_scale_y = vp_scale_y;
  cb.vp_offset_x = vp_offset_x;
  cb.vp_offset_y = vp_offset_y;
  cb.is_float24 = candidate.is_float ? 1u : 0u;
  cb.sample_count = candidate.sample_count;
  cb.inverted = final_inverted ? 1u : 0u;
  cb.explicit_sample = candidate.explicit_sample;

  command_list->SetComputeRoot32BitConstants(0, 12, &cb, 0);

  // SRV table at param 1
  command_list->SetComputeRootDescriptorTable(1, gpu_start);

  // UAV table at param 2 (offset by 1 descriptor)
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav_start = gpu_start;
  gpu_uav_start.ptr += desc_size;
  command_list->SetComputeRootDescriptorTable(2, gpu_uav_start);

  if (candidate.sample_count > 1) {
    command_list->SetPipelineState(pso_msaa_.Get());
  } else {
    command_list->SetPipelineState(pso_1x_.Get());
  }

  uint32_t dispatch_x = (target_width + 7) / 8;
  uint32_t dispatch_y = (target_height + 7) / 8;
  command_list->Dispatch(dispatch_x, dispatch_y, 1);

  // 4. UAV Barrier on range buffer and output depth
  D3D12_RESOURCE_BARRIER uav_barriers[2] = {};
  uav_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uav_barriers[0].UAV.pResource = output_depth_resource_.Get();
  uav_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uav_barriers[1].UAV.pResource = range_buffer_.Get();
  command_list->ResourceBarrier(2, uav_barriers);

  // 5. Copy range buffer to designated async readback slot
  size_t slot_idx = current_slot_index_ % kMaxAsyncSlots;
  current_slot_index_++;
  auto& slot = async_slots_[slot_idx];
  slot.guest_generation = guest_generation;
  slot.candidate_resource = candidate.resource;
  slot.fence_value = ++next_fence_value_;
  slot.pending = true;
  slot.is_valid = false;
  pending_submit_fence_value_ = slot.fence_value;

  D3D12_RESOURCE_BARRIER rb_copy_barriers[2] = {};
  rb_copy_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  rb_copy_barriers[0].Transition.pResource = range_buffer_.Get();
  rb_copy_barriers[0].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  rb_copy_barriers[0].Transition.StateBefore =
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  rb_copy_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

  rb_copy_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  rb_copy_barriers[1].Transition.pResource = output_depth_resource_.Get();
  rb_copy_barriers[1].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  rb_copy_barriers[1].Transition.StateBefore =
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  rb_copy_barriers[1].Transition.StateAfter =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  command_list->ResourceBarrier(2, rb_copy_barriers);

  command_list->CopyBufferRegion(slot.readback_buffer.Get(), 0,
                                range_buffer_.Get(), 0, 8);

  // Restore range buffer and candidate depth
  D3D12_RESOURCE_BARRIER restore_barriers[2] = {};
  restore_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  restore_barriers[0].Transition.pResource = range_buffer_.Get();
  restore_barriers[0].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  restore_barriers[0].Transition.StateBefore =
      D3D12_RESOURCE_STATE_COPY_SOURCE;
  restore_barriers[0].Transition.StateAfter =
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

  restore_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  restore_barriers[1].Transition.pResource = candidate.resource;
  restore_barriers[1].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  restore_barriers[1].Transition.StateBefore =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  restore_barriers[1].Transition.StateAfter =
      D3D12_RESOURCE_STATE_DEPTH_WRITE;
  command_list->ResourceBarrier(2, restore_barriers);

  // 6. Record DepthFrame
  // Requirement 8: "not yet validated -> invalid for NGX"
  // IsGenerationValidated queries completed readback slots stamped with this guest_generation.
  current_frame_.resource = output_depth_resource_.Get();
  current_frame_.width = target_width;
  current_frame_.height = target_height;
  current_frame_.inverted = final_inverted;
  current_frame_.guest_generation = guest_generation;
  current_frame_.depth_history_discontinuity = discontinuity;
  current_frame_.depth_near = candidate.depth_near;
  current_frame_.depth_far = candidate.depth_far;
  current_frame_.confidence = candidate.confidence;
  current_frame_.score = candidate.score;
  current_frame_.score_margin = candidate.score_margin;
  current_frame_.valid = IsGenerationValidated(guest_generation);
}

}  // namespace neural
}  // namespace d3d12
}  // namespace ui
}  // namespace xe
