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

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/ui/d3d12/d3d12_util.h"
#include "xenia/ui/d3d12/neural/depth_provider.h"
#include "xenia/ui/d3d12/neural/motion_estimator.h"
#include "xenia/ui/d3d12/neural/nvidia_optical_flow_estimator.h"
#include "xenia/ui/d3d12/neural/synthetic_ngx_session.h"
#include <dxgi1_6.h>
#include <algorithm>
#include <cmath>
#include <psapi.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#pragma comment(lib, "psapi.lib")

DEFINE_string(d3d12_neural_output_mode, "passthrough",
              "Neural rendering presentation mode: passthrough | "
              "contract_only | full | split.",
              "GPU");

DEFINE_bool(
    d3d12_neural_synthetic_color_test, false,
    "Execute diagnostic non-gameplay synthetic color transfer test with solid patches and ramps.",
    "D3D12");

DECLARE_bool(d3d12_neural_rendering);
DECLARE_bool(d3d12_neural_depth_debug_view);

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

  depth_provider_ = std::make_unique<DepthProvider>();
  if (depth_provider_->Initialize(device_, direct_queue_)) {
    XELOGI("NeuralRenderingManager: DepthProvider initialized");
  } else {
    XELOGE("NeuralRenderingManager: Failed to initialize DepthProvider");
  }
}

NeuralRenderingManager::~NeuralRenderingManager() {
  XELOGI("NeuralRenderingManager: === SHUTDOWN TELEMETRY REPORT ===");
  std::string g_title = "Unknown";
  uint32_t g_title_id = 0;
  xe::kernel::KernelState* ks = xe::kernel::KernelState::shared();
  if (ks && ks->emulator()) {
    g_title = ks->emulator()->title_name();
    g_title_id = ks->emulator()->title_id();
    if (g_title_id == 0) {
      g_title_id = ks->title_id();
    }
  }
  XELOGI("  Guest title:                 {}", g_title);
  XELOGI("  Guest Title ID:              {:08X}", g_title_id);
  if (ngx_session_) {
    XELOGI("  Pipeline Operating Level:    {}", ngx_session_->GetLevelString());
    XELOGI("  Interception Confirmed:      {}",
           ngx_session_->IsInterceptionConfirmed() ? "YES (Level 3)" : "NO");
    XELOGI("  Active Consumer:             {}", ngx_session_->GetConsumerString());
    if (ngx_session_->active_consumer() == NeuralConsumer::kDeepFriedChicken) {
      XELOGI("  Deep Fried Chicken state:    {}",
             uint32_t(ngx_session_->dfc_state()));
      XELOGI("  Deep Fried Chicken ABI:      {}", ngx_session_->dfc_abi());
    } else if (ngx_session_->active_consumer() == NeuralConsumer::kRenoDx) {
      XELOGI("  RenoDX DLSS5 addon loaded:   {}",
             ngx_session_->renodx_info().loaded ? "YES" : "NO");
    }
    XELOGI("  nvngx_dlssnr.dll loaded:     {}",
           ngx_session_->dlssnr_info().loaded ? "YES" : "NO");
    XELOGI("  NGX feature creates:         {}", ngx_session_->feature_create_count());
    XELOGI("  NGX feature releases:        {}", ngx_session_->feature_release_count());
  }
  if (depth_provider_) {
    XELOGI("  DepthInverted transitions:   {}", depth_provider_->depth_inverted_transitions());
    XELOGI("  Candidate switches:          {}", depth_provider_->candidate_switch_count());
    XELOGI("  MSAA heuristic frames:       {}", depth_provider_->msaa_heuristic_frames());
  }
  XELOGI("  Neural D3D12 resources:      {}", GetNeuralOwnedResourceCount());
  XELOGI("  Neural descriptor count:     {}", GetNeuralDescriptorCount());
  XELOGI("  Total guest frames:          {}", total_frames_);
  XELOGI("  Evaluated neural frames:     {}", evaluated_frames_);
  XELOGI("  Bypassed frames:             {}", bypassed_frames_);
  XELOGI("  History reset frames:        {}", reset_frames_);
  XELOGI("  Contract failures:           {}", contract_failures_);
  XELOGI("  Split-screen blit passes:    {}", split_blits_);
  if (evaluated_frames_ > 0) {
    XELOGI("  Average Optical Flow time:   {:.3f} ms", avg_motion_time_ms());
    XELOGI("  Average Depth process time:  {:.3f} ms", avg_depth_time_ms());
    if (ngx_session_ &&
        ngx_session_->neural_level() == NeuralLevel::kLevel3_NeuralRenderingConfirmed) {
      XELOGI("  Neural-enabled NGX dependency interval: {:.3f} ms", avg_ngx_time_ms());
    } else {
      XELOGI("  Plain DLAA NGX dependency interval:     {:.3f} ms", avg_ngx_time_ms());
    }
    if (split_blits_ > 0) {
      XELOGI("  Average Split blit time:     {:.3f} ms", avg_blit_time_ms());
    }
    XELOGI("  Average Total pipeline time: {:.3f} ms", avg_total_pipeline_time_ms());
  }
  if (pixel_diff_metrics_.measured) {
    XELOGI("  Output Pixel Diff (RMS):     {:.6f}", pixel_diff_metrics_.rms);
    XELOGI("  Output Pixel Diff (MAD):     {:.6f}", pixel_diff_metrics_.mad);
    XELOGI("  Output Pixel Diff (Max):     {:.6f}", pixel_diff_metrics_.max_diff);
    XELOGI("  Output Pixels Changed:       {:.2f}% ({}/{} px)",
           pixel_diff_metrics_.pct_changed,
           pixel_diff_metrics_.changed_pixels,
           pixel_diff_metrics_.total_pixels);
  }
  if (ngx_session_) {
    XELOGI("  NGX total evaluations:       {}", ngx_session_->evaluate_count());
    XELOGI("  NGX successful evaluations:  {}", ngx_session_->evaluate_success_count());
    XELOGI("  NGX failed evaluations:      {}", ngx_session_->evaluate_failure_count());
    XELOGI("  NGX exceptions caught:       {}", ngx_session_->evaluate_exception_count());
  }
  XELOGI("NeuralRenderingManager: ================================");

  readback_original_buffer_.Reset();
  readback_processed_buffer_.Reset();
  readback_fence_.Reset();

  split_output_resource_.Reset();
  depth_provider_.reset();
  motion_estimator_.reset();
  ngx_session_.reset();
  XELOGI("NeuralRenderingManager: Shutdown complete");
}

bool NeuralRenderingManager::EnsureSplitResource(uint32_t width,
                                                 uint32_t height,
                                                 DXGI_FORMAT format) {
  if (split_output_resource_ && split_width_ == width &&
      split_height_ == height && split_format_ == format) {
    return true;
  }
  split_output_resource_.Reset();
  split_width_ = width;
  split_height_ = height;
  split_format_ = format;
  split_resource_state_ = D3D12_RESOURCE_STATE_COMMON;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Format = format;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  HRESULT hr = device_->CreateCommittedResource(
      &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_COMMON, nullptr,
      IID_PPV_ARGS(&split_output_resource_));
  if (FAILED(hr)) {
    XELOGE("NeuralRenderingManager: Failed to create split output texture: {:#x}",
           hr);
    return false;
  }
  return true;
}

ID3D12Resource* NeuralRenderingManager::BlitSplitOutput(
    ID3D12GraphicsCommandList* command_list, ID3D12Resource* original,
    ID3D12Resource* neural, uint32_t width, uint32_t height) {
  if (!EnsureSplitResource(width, height, current_format_)) {
    return neural;
  }

  // Pre-copy transitions:
  // original: from PIXEL_SHADER_RESOURCE to COPY_SOURCE
  // neural: from PIXEL_SHADER_RESOURCE to COPY_SOURCE
  // split_output: from split_resource_state_ to COPY_DEST
  D3D12_RESOURCE_BARRIER pre_barriers[3] = {};
  pre_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  pre_barriers[0].Transition.pResource = original;
  pre_barriers[0].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  pre_barriers[0].Transition.StateBefore =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  pre_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

  pre_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  pre_barriers[1].Transition.pResource = neural;
  pre_barriers[1].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  pre_barriers[1].Transition.StateBefore =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  pre_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

  pre_barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  pre_barriers[2].Transition.pResource = split_output_resource_.Get();
  pre_barriers[2].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  pre_barriers[2].Transition.StateBefore = split_resource_state_;
  pre_barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

  command_list->ResourceBarrier(3, pre_barriers);

  // Left half: [0, width / 2) from original color
  uint32_t half_w = width / 2;
  D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
  dst_loc.pResource = split_output_resource_.Get();
  dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst_loc.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION src_orig_loc = {};
  src_orig_loc.pResource = original;
  src_orig_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src_orig_loc.SubresourceIndex = 0;

  D3D12_BOX left_box = {};
  left_box.left = 0;
  left_box.top = 0;
  left_box.front = 0;
  left_box.right = half_w;
  left_box.bottom = height;
  left_box.back = 1;

  command_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_orig_loc, &left_box);

  // Right half: [width / 2, width) from neural output
  D3D12_TEXTURE_COPY_LOCATION src_neural_loc = {};
  src_neural_loc.pResource = neural;
  src_neural_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src_neural_loc.SubresourceIndex = 0;

  D3D12_BOX right_box = {};
  right_box.left = half_w;
  right_box.top = 0;
  right_box.front = 0;
  right_box.right = width;
  right_box.bottom = height;
  right_box.back = 1;

  command_list->CopyTextureRegion(&dst_loc, half_w, 0, 0, &src_neural_loc,
                                  &right_box);

  // Post-copy transitions:
  // original: from COPY_SOURCE back to PIXEL_SHADER_RESOURCE
  // neural: from COPY_SOURCE back to PIXEL_SHADER_RESOURCE
  // split_output: from COPY_DEST to PIXEL_SHADER_RESOURCE
  D3D12_RESOURCE_BARRIER post_barriers[3] = {};
  post_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  post_barriers[0].Transition.pResource = original;
  post_barriers[0].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  post_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  post_barriers[0].Transition.StateAfter =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  post_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  post_barriers[1].Transition.pResource = neural;
  post_barriers[1].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  post_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  post_barriers[1].Transition.StateAfter =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  post_barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  post_barriers[2].Transition.pResource = split_output_resource_.Get();
  post_barriers[2].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  post_barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  post_barriers[2].Transition.StateAfter =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  command_list->ResourceBarrier(3, post_barriers);
  split_resource_state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  return split_output_resource_.Get();
}

void NeuralRenderingManager::ScheduleDiagnosticReadback(
    ID3D12GraphicsCommandList* command_list, ID3D12Resource* original,
    ID3D12Resource* processed, uint32_t width, uint32_t height,
    DXGI_FORMAT format) {
  if (readback_scheduled_ || !device_ || !command_list || !original ||
      !processed) {
    return;
  }

  D3D12_RESOURCE_DESC desc = original->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT num_rows = 0;
  UINT64 row_size = 0;
  UINT64 total_bytes = 0;
  device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &num_rows,
                                &row_size, &total_bytes);

  if (total_bytes == 0) {
    return;
  }

  D3D12_RESOURCE_DESC buf_desc = {};
  buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buf_desc.Width = total_bytes;
  buf_desc.Height = 1;
  buf_desc.DepthOrArraySize = 1;
  buf_desc.MipLevels = 1;
  buf_desc.Format = DXGI_FORMAT_UNKNOWN;
  buf_desc.SampleDesc.Count = 1;
  buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  buf_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  HRESULT hr1 = device_->CreateCommittedResource(
      &ui::d3d12::util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE,
      &buf_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
      IID_PPV_ARGS(&readback_original_buffer_));
  HRESULT hr2 = device_->CreateCommittedResource(
      &ui::d3d12::util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE,
      &buf_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
      IID_PPV_ARGS(&readback_processed_buffer_));

  if (FAILED(hr1) || FAILED(hr2)) {
    XELOGW("NeuralRenderingManager: Failed to create readback buffers");
    readback_original_buffer_.Reset();
    readback_processed_buffer_.Reset();
    return;
  }

  if (!readback_fence_) {
    device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                         IID_PPV_ARGS(&readback_fence_));
  }

  D3D12_RESOURCE_BARRIER pre[2] = {};
  pre[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  pre[0].Transition.pResource = original;
  pre[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  pre[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  pre[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

  pre[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  pre[1].Transition.pResource = processed;
  pre[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  pre[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  pre[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

  command_list->ResourceBarrier(2, pre);

  D3D12_TEXTURE_COPY_LOCATION dst_orig = {};
  dst_orig.pResource = readback_original_buffer_.Get();
  dst_orig.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst_orig.PlacedFootprint = footprint;

  D3D12_TEXTURE_COPY_LOCATION src_orig = {};
  src_orig.pResource = original;
  src_orig.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src_orig.SubresourceIndex = 0;

  command_list->CopyTextureRegion(&dst_orig, 0, 0, 0, &src_orig, nullptr);

  D3D12_TEXTURE_COPY_LOCATION dst_proc = {};
  dst_proc.pResource = readback_processed_buffer_.Get();
  dst_proc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst_proc.PlacedFootprint = footprint;

  D3D12_TEXTURE_COPY_LOCATION src_proc = {};
  src_proc.pResource = processed;
  src_proc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src_proc.SubresourceIndex = 0;

  command_list->CopyTextureRegion(&dst_proc, 0, 0, 0, &src_proc, nullptr);

  D3D12_RESOURCE_BARRIER post[2] = {};
  post[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  post[0].Transition.pResource = original;
  post[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  post[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  post[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  post[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  post[1].Transition.pResource = processed;
  post[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  post[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  post[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  command_list->ResourceBarrier(2, post);

  readback_width_ = width;
  readback_height_ = height;
  readback_format_ = format;
  readback_footprint_ = footprint;
  readback_scheduled_ = true;
}

void NeuralRenderingManager::ProcessDiagnosticReadback() {
  if (!readback_scheduled_ || readback_completed_ || !readback_fence_ ||
      !readback_original_buffer_ || !readback_processed_buffer_) {
    return;
  }

  if (readback_fence_->GetCompletedValue() < readback_fence_value_) {
    return;
  }

  void* ptr_orig = nullptr;
  void* ptr_proc = nullptr;
  D3D12_RANGE read_range = {0, static_cast<SIZE_T>(readback_footprint_.Footprint.RowPitch * readback_height_)};
  if (FAILED(readback_original_buffer_->Map(0, &read_range, &ptr_orig)) ||
      FAILED(readback_processed_buffer_->Map(0, &read_range, &ptr_proc))) {
    XELOGW("NeuralRenderingManager: Failed to map diagnostic readback buffers");
    return;
  }

  const uint8_t* p_orig = reinterpret_cast<const uint8_t*>(ptr_orig);
  const uint8_t* p_proc = reinterpret_cast<const uint8_t*>(ptr_proc);
  const uint32_t row_pitch = readback_footprint_.Footprint.RowPitch;

  double sum_sq = 0.0;
  double sum_abs = 0.0;
  double max_diff = 0.0;
  uint64_t changed_px = 0;
  const uint64_t total_px = uint64_t(readback_width_) * readback_height_;

  bool is_bgra = (readback_format_ == DXGI_FORMAT_B8G8R8A8_UNORM ||
                  readback_format_ == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);

  for (uint32_t y = 0; y < readback_height_; ++y) {
    const uint8_t* row_o = p_orig + y * row_pitch;
    const uint8_t* row_p = p_proc + y * row_pitch;

    for (uint32_t x = 0; x < readback_width_; ++x) {
      float ro, go, bo, rp, gp, bp;
      if (is_bgra) {
        bo = row_o[x * 4 + 0] / 255.0f;
        go = row_o[x * 4 + 1] / 255.0f;
        ro = row_o[x * 4 + 2] / 255.0f;
        bp = row_p[x * 4 + 0] / 255.0f;
        gp = row_p[x * 4 + 1] / 255.0f;
        rp = row_p[x * 4 + 2] / 255.0f;
      } else {
        ro = row_o[x * 4 + 0] / 255.0f;
        go = row_o[x * 4 + 1] / 255.0f;
        bo = row_o[x * 4 + 2] / 255.0f;
        rp = row_p[x * 4 + 0] / 255.0f;
        gp = row_p[x * 4 + 1] / 255.0f;
        bp = row_p[x * 4 + 2] / 255.0f;
      }

      double dr = std::abs(ro - rp);
      double dg = std::abs(go - gp);
      double db = std::abs(bo - bp);

      sum_sq += (dr * dr + dg * dg + db * db);
      sum_abs += (dr + dg + db);

      double px_max = std::max(dr, std::max(dg, db));
      if (px_max > max_diff) {
        max_diff = px_max;
      }
      if (px_max > 0.0039 /* 1/255 */) {
        changed_px++;
      }
    }
  }

  // Dump raw readback frames for offline mathematical comparison (Plain DLAA vs Neural)
  const char* proc_dump_name =
      (ngx_session_ &&
       ngx_session_->active_consumer() == NeuralConsumer::kRenoDx)
          ? "readback_proc_neural.raw"
          : "readback_proc_dlaa.raw";
  FILE* fp_p = nullptr;
  if (fopen_s(&fp_p, proc_dump_name, "wb") == 0 && fp_p) {
    for (uint32_t y = 0; y < readback_height_; ++y) {
      fwrite(p_proc + y * row_pitch, 1, readback_width_ * 4, fp_p);
    }
    fclose(fp_p);
    XELOGI("NeuralRenderingManager: Dumped processed frame to {}",
           proc_dump_name);
  }
  FILE* fp_o = nullptr;
  if (fopen_s(&fp_o, "readback_orig.raw", "wb") == 0 && fp_o) {
    for (uint32_t y = 0; y < readback_height_; ++y) {
      fwrite(p_orig + y * row_pitch, 1, readback_width_ * 4, fp_o);
    }
    fclose(fp_o);
  }

  D3D12_RANGE write_range = {0, 0};
  readback_original_buffer_->Unmap(0, &write_range);
  readback_processed_buffer_->Unmap(0, &write_range);

  readback_original_buffer_.Reset();
  readback_processed_buffer_.Reset();

  pixel_diff_metrics_.measured = true;
  pixel_diff_metrics_.evaluated_frame = evaluated_frames_;
  pixel_diff_metrics_.total_pixels = total_px;
  pixel_diff_metrics_.changed_pixels = changed_px;
  pixel_diff_metrics_.rms = std::sqrt(sum_sq / (total_px * 3.0));
  pixel_diff_metrics_.mad = sum_abs / (total_px * 3.0);
  pixel_diff_metrics_.max_diff = max_diff;
  pixel_diff_metrics_.pct_changed = (total_px > 0) ? (double(changed_px) * 100.0 / total_px) : 0.0;
  readback_completed_ = true;

  const char* level_name = ngx_session_ ? ngx_session_->GetLevelString() : "SYNTHETIC DLAA";
  XELOGI(
      "NeuralRenderingManager: [DIAGNOSTIC READBACK] Frame {} Pixel Difference "
      "(Original vs {}): RMS={:.6f}, MAD={:.6f}, MaxDiff={:.6f}, PixelsChanged={:.2f}% ({}/{} px)",
      evaluated_frames_, level_name, pixel_diff_metrics_.rms,
      pixel_diff_metrics_.mad, pixel_diff_metrics_.max_diff,
      pixel_diff_metrics_.pct_changed, pixel_diff_metrics_.changed_pixels,
      pixel_diff_metrics_.total_pixels);
}

void NeuralRenderingManager::RunSyntheticColorTransferTest(
    ID3D12GraphicsCommandList* caller_command_list) {
  if (!device_ || !direct_queue_ || !ngx_session_ || !ngx_session_->IsReady()) {
    return;
  }

  XELOGI("================================================================================");
  XELOGI("NeuralRenderingManager: [FASE H] RUNNING SYNTHETIC COLOR TRANSFER TEST");
  XELOGI("  Active Consumer: {}", ngx_session_->GetConsumerString());
  XELOGI("  AutoExposure:    {}", cvars::d3d12_neural_auto_exposure ? "ON (0x40)" : "OFF");
  XELOGI("  Pre.Exposure:    {:.2f}", cvars::d3d12_neural_pre_exposure);
  XELOGI("  Exposure.Scale:  {:.2f}", cvars::d3d12_neural_exposure_scale);
  XELOGI("================================================================================");

  const uint32_t width = 1280;
  const uint32_t height = 720;
  const uint32_t total_px = width * height;
  const DXGI_FORMAT color_format = DXGI_FORMAT_R10G10B10A2_UNORM;

  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> test_allocator;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> test_cmd;
  if (FAILED(device_->CreateCommandAllocator(
          D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&test_allocator))) ||
      FAILED(device_->CreateCommandList(
          0, D3D12_COMMAND_LIST_TYPE_DIRECT, test_allocator.Get(), nullptr,
          IID_PPV_ARGS(&test_cmd)))) {
    XELOGE("NeuralRenderingManager: [FASE H] Failed to create test command list");
    return;
  }

  Microsoft::WRL::ComPtr<ID3D12Fence> test_fence;
  if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&test_fence)))) {
    XELOGE("NeuralRenderingManager: [FASE H] Failed to create test fence");
    return;
  }
  uint64_t fence_val = 0;

  auto ExecuteAndWait = [&]() -> bool {
    if (FAILED(test_cmd->Close())) {
      return false;
    }
    ID3D12CommandList* pp[] = {test_cmd.Get()};
    direct_queue_->ExecuteCommandLists(1, pp);
    fence_val++;
    direct_queue_->Signal(test_fence.Get(), fence_val);
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (evt) {
      if (SUCCEEDED(test_fence->SetEventOnCompletion(fence_val, evt))) {
        WaitForSingleObject(evt, 5000);
      }
      CloseHandle(evt);
    }
    test_allocator->Reset();
    test_cmd->Reset(test_allocator.Get(), nullptr);
    return true;
  };

  D3D12_RESOURCE_DESC color_desc = {};
  color_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  color_desc.Width = width;
  color_desc.Height = height;
  color_desc.DepthOrArraySize = 1;
  color_desc.MipLevels = 1;
  color_desc.Format = color_format;
  color_desc.SampleDesc.Count = 1;
  color_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  color_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  Microsoft::WRL::ComPtr<ID3D12Resource> color_res;
  if (FAILED(device_->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
          &color_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&color_res)))) {
    XELOGE("NeuralRenderingManager: [FASE H] Failed to create color texture");
    return;
  }

  D3D12_RESOURCE_DESC depth_desc = color_desc;
  depth_desc.Format = DXGI_FORMAT_R32_FLOAT;
  Microsoft::WRL::ComPtr<ID3D12Resource> depth_res;
  if (FAILED(device_->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
          &depth_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&depth_res)))) {
    XELOGE("NeuralRenderingManager: [FASE H] Failed to create depth texture");
    return;
  }

  D3D12_RESOURCE_DESC mv_desc = color_desc;
  mv_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
  Microsoft::WRL::ComPtr<ID3D12Resource> mv_res;
  if (FAILED(device_->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
          &mv_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&mv_res)))) {
    XELOGE("NeuralRenderingManager: [FASE H] Failed to create mv texture");
    return;
  }

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT color_fp = {};
  UINT64 color_upload_size = 0;
  device_->GetCopyableFootprints(&color_desc, 0, 1, 0, &color_fp, nullptr, nullptr,
                                &color_upload_size);

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT depth_fp = {};
  UINT64 depth_upload_size = 0;
  device_->GetCopyableFootprints(&depth_desc, 0, 1, 0, &depth_fp, nullptr, nullptr,
                                &depth_upload_size);

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT mv_fp = {};
  UINT64 mv_upload_size = 0;
  device_->GetCopyableFootprints(&mv_desc, 0, 1, 0, &mv_fp, nullptr, nullptr,
                                &mv_upload_size);

  UINT64 total_upload_size = color_upload_size + depth_upload_size + mv_upload_size;
  D3D12_RESOURCE_DESC upload_desc = {};
  ui::d3d12::util::FillBufferResourceDesc(upload_desc, total_upload_size,
                                          D3D12_RESOURCE_FLAG_NONE);
  Microsoft::WRL::ComPtr<ID3D12Resource> upload_buf;
  if (FAILED(device_->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE,
          &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
          IID_PPV_ARGS(&upload_buf)))) {
    XELOGE("NeuralRenderingManager: [FASE H] Failed to create upload buffer");
    return;
  }

  D3D12_RESOURCE_DESC rb_desc = {};
  ui::d3d12::util::FillBufferResourceDesc(rb_desc, color_upload_size,
                                          D3D12_RESOURCE_FLAG_NONE);
  Microsoft::WRL::ComPtr<ID3D12Resource> rb_buf;
  if (FAILED(device_->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE,
          &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&rb_buf)))) {
    XELOGE("NeuralRenderingManager: [FASE H] Failed to create readback buffer");
    return;
  }

  uint8_t* p_upload = nullptr;
  D3D12_RANGE r_zero = {0, 0};
  if (FAILED(upload_buf->Map(0, &r_zero, reinterpret_cast<void**>(&p_upload)))) {
    XELOGE("NeuralRenderingManager: [FASE H] Failed to map upload buffer");
    return;
  }

  // Populate static depth (0.5f)
  uint8_t* p_depth = p_upload + color_upload_size;
  for (uint32_t y = 0; y < height; ++y) {
    float* row = reinterpret_cast<float*>(p_depth + y * depth_fp.Footprint.RowPitch);
    for (uint32_t x = 0; x < width; ++x) {
      row[x] = 0.5f;
    }
  }

  // Populate zero motion vectors
  uint8_t* p_mv = p_depth + depth_upload_size;
  for (uint32_t y = 0; y < height; ++y) {
    uint32_t* row = reinterpret_cast<uint32_t*>(p_mv + y * mv_fp.Footprint.RowPitch);
    for (uint32_t x = 0; x < width; ++x) {
      row[x] = 0; // 0.0f in FP16
    }
  }

  // Upload depth and MV textures
  D3D12_TEXTURE_COPY_LOCATION dst_depth = {};
  dst_depth.pResource = depth_res.Get();
  dst_depth.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION src_depth = {};
  src_depth.pResource = upload_buf.Get();
  src_depth.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src_depth.PlacedFootprint = depth_fp;
  src_depth.PlacedFootprint.Offset = color_upload_size;
  test_cmd->CopyTextureRegion(&dst_depth, 0, 0, 0, &src_depth, nullptr);

  D3D12_TEXTURE_COPY_LOCATION dst_mv = {};
  dst_mv.pResource = mv_res.Get();
  dst_mv.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION src_mv = {};
  src_mv.pResource = upload_buf.Get();
  src_mv.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src_mv.PlacedFootprint = mv_fp;
  src_mv.PlacedFootprint.Offset = color_upload_size + depth_upload_size;
  test_cmd->CopyTextureRegion(&dst_mv, 0, 0, 0, &src_mv, nullptr);

  D3D12_RESOURCE_BARRIER init_barriers[2] = {};
  init_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  init_barriers[0].Transition.pResource = depth_res.Get();
  init_barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  init_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  init_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

  init_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  init_barriers[1].Transition.pResource = mv_res.Get();
  init_barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  init_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  init_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  test_cmd->ResourceBarrier(2, init_barriers);

  ExecuteAndWait();

  struct TestPatch {
    std::string name;
    float r, g, b;
    bool is_grayscale_ramp;
    bool is_color_ramp;
  };

  std::vector<TestPatch> test_patches = {
      {"Flat Black (0.0)", 0.0f, 0.0f, 0.0f, false, false},
      {"Flat 18% Grey (0.18)", 0.18f, 0.18f, 0.18f, false, false},
      {"Flat 50% Grey (0.50)", 0.50f, 0.50f, 0.50f, false, false},
      {"Flat White (1.0)", 1.0f, 1.0f, 1.0f, false, false},
      {"Primary Red (1,0,0)", 1.0f, 0.0f, 0.0f, false, false},
      {"Primary Green (0,1,0)", 0.0f, 1.0f, 0.0f, false, false},
      {"Primary Blue (0,0,1)", 0.0f, 0.0f, 1.0f, false, false},
      {"Grayscale Ramp", 0.0f, 0.0f, 0.0f, true, false},
      {"Color Ramp", 0.0f, 0.0f, 0.0f, false, true},
  };

  std::string report = "================================================================================\n";
  report += "XENIA EDGE — COMMIT 6 SYNTHETIC COLOR TRANSFER TEST REPORT (FASE H)\n";
  report += "================================================================================\n";
  report += "Consumer:     " + std::string(ngx_session_->GetConsumerString()) + "\n";
  report += "Level:        " + std::string(ngx_session_->GetLevelString()) + "\n";
  report += "AutoExposure: " + std::string(cvars::d3d12_neural_auto_exposure ? "TRUE (0x40)" : "FALSE (0x02)") + "\n";
  report += "Pre.Exposure: " + std::to_string(cvars::d3d12_neural_pre_exposure) + "\n";
  report += "Exposure.Scale: " + std::to_string(cvars::d3d12_neural_exposure_scale) + "\n\n";

  bool color_res_initialized = false;

  for (const auto& patch : test_patches) {
    for (uint32_t y = 0; y < height; ++y) {
      uint32_t* row = reinterpret_cast<uint32_t*>(p_upload + y * color_fp.Footprint.RowPitch);
      for (uint32_t x = 0; x < width; ++x) {
        float pr = patch.r;
        float pg = patch.g;
        float pb = patch.b;
        if (patch.is_grayscale_ramp) {
          pr = pg = pb = float(x) / float(width - 1);
        } else if (patch.is_color_ramp) {
          pr = float(x) / float(width - 1);
          pg = float(y) / float(height - 1);
          pb = 0.5f;
        }
        uint32_t ur = std::min(1023u, uint32_t(pr * 1023.0f + 0.5f));
        uint32_t ug = std::min(1023u, uint32_t(pg * 1023.0f + 0.5f));
        uint32_t ub = std::min(1023u, uint32_t(pb * 1023.0f + 0.5f));
        row[x] = (ur & 0x3FF) | ((ug & 0x3FF) << 10) | ((ub & 0x3FF) << 20) | (3u << 30);
      }
    }

    if (color_res_initialized) {
      D3D12_RESOURCE_BARRIER b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = color_res.Get();
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
      test_cmd->ResourceBarrier(1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dst_c = {};
    dst_c.pResource = color_res.Get();
    dst_c.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src_c = {};
    src_c.pResource = upload_buf.Get();
    src_c.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_c.PlacedFootprint = color_fp;
    test_cmd->CopyTextureRegion(&dst_c, 0, 0, 0, &src_c, nullptr);

    D3D12_RESOURCE_BARRIER b_post = {};
    b_post.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b_post.Transition.pResource = color_res.Get();
    b_post.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b_post.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b_post.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    test_cmd->ResourceBarrier(1, &b_post);
    color_res_initialized = true;

    NeuralFrameContract contract = {};
    contract.color = color_res.Get();
    contract.depth = depth_res.Get();
    contract.motion_vectors = mv_res.Get();
    contract.width = width;
    contract.height = height;
    contract.reset_history = true;
    contract.valid = true;

    ngx_session_->Evaluate(test_cmd.Get(), contract);

    D3D12_RESOURCE_BARRIER b_rb = {};
    b_rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b_rb.Transition.pResource = ngx_session_->output_resource();
    b_rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b_rb.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b_rb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    test_cmd->ResourceBarrier(1, &b_rb);

    D3D12_TEXTURE_COPY_LOCATION dst_rb = {};
    dst_rb.pResource = rb_buf.Get();
    dst_rb.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_rb.PlacedFootprint = color_fp;
    D3D12_TEXTURE_COPY_LOCATION src_rb = {};
    src_rb.pResource = ngx_session_->output_resource();
    src_rb.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    test_cmd->CopyTextureRegion(&dst_rb, 0, 0, 0, &src_rb, nullptr);

    D3D12_RESOURCE_BARRIER b_rb_post = {};
    b_rb_post.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b_rb_post.Transition.pResource = ngx_session_->output_resource();
    b_rb_post.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b_rb_post.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b_rb_post.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    test_cmd->ResourceBarrier(1, &b_rb_post);

    ExecuteAndWait();

    uint8_t* p_rb = nullptr;
    D3D12_RANGE r_read = {0, static_cast<SIZE_T>(color_upload_size)};
    if (SUCCEEDED(rb_buf->Map(0, &r_read, reinterpret_cast<void**>(&p_rb)))) {
      double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0, sum_y = 0.0;
      float min_r = 1.0f, max_r = 0.0f;
      float min_g = 1.0f, max_g = 0.0f;
      float min_b = 1.0f, max_b = 0.0f;

      for (uint32_t y = 0; y < height; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(
            p_rb + y * color_fp.Footprint.RowPitch);
        for (uint32_t x = 0; x < width; ++x) {
          uint32_t px = row[x];
          float pr = (px & 0x3FF) / 1023.0f;
          float pg = ((px >> 10) & 0x3FF) / 1023.0f;
          float pb = ((px >> 20) & 0x3FF) / 1023.0f;
          float py = 0.2126f * pr + 0.7152f * pg + 0.0722f * pb;

          sum_r += pr; sum_g += pg; sum_b += pb; sum_y += py;
          min_r = std::min(min_r, pr); max_r = std::max(max_r, pr);
          min_g = std::min(min_g, pg); max_g = std::max(max_g, pg);
          min_b = std::min(min_b, pb); max_b = std::max(max_b, pb);
        }
      }

      float mean_r = float(sum_r / total_px);
      float mean_g = float(sum_g / total_px);
      float mean_b = float(sum_b / total_px);
      float mean_y = float(sum_y / total_px);

      char line[512] = {};
      if (!patch.is_grayscale_ramp && !patch.is_color_ramp) {
        float in_luma = 0.2126f * patch.r + 0.7152f * patch.g + 0.0722f * patch.b;
        float gain = (in_luma > 0.001f) ? (mean_y / in_luma) : 0.0f;
        sprintf_s(line,
                  "  %-24s: In(R=%.2f,G=%.2f,B=%.2f,Y=%.2f) -> "
                  "Out(R=%.4f,G=%.4f,B=%.4f,Y=%.4f) | Gain=%.4f\n",
                  patch.name.c_str(), patch.r, patch.g, patch.b, in_luma,
                  mean_r, mean_g, mean_b, mean_y, gain);
        XELOGI("{}", line);
        report += line;
      } else if (patch.is_grayscale_ramp) {
        sprintf_s(line, "  %-24s: Mean Y=%.4f (Min=%.4f, Max=%.4f)\n",
                  patch.name.c_str(), mean_y, min_r, max_r);
        XELOGI("{}", line);
        report += line;

        report += "    [Ramp Decile Samples (Input -> Output)]:\n";
        const uint32_t* mid_row = reinterpret_cast<const uint32_t*>(
            p_rb + (height / 2) * color_fp.Footprint.RowPitch);
        for (int d = 0; d <= 10; ++d) {
          uint32_t sample_x = (d == 10) ? (width - 1) : (d * (width - 1) / 10);
          float in_v = float(sample_x) / float(width - 1);
          uint32_t px = mid_row[sample_x];
          float out_r = (px & 0x3FF) / 1023.0f;
          float out_g = ((px >> 10) & 0x3FF) / 1023.0f;
          float out_b = ((px >> 20) & 0x3FF) / 1023.0f;
          float out_y = 0.2126f * out_r + 0.7152f * out_g + 0.0722f * out_b;
          char sample_str[256] = {};
          sprintf_s(sample_str, "      Step %2d%%: In=%.2f -> Out=%.4f (R=%.4f, G=%.4f, B=%.4f) | Ratio=%.4f\n",
                    d * 10, in_v, out_y, out_r, out_g, out_b, (in_v > 0.01f ? (out_y / in_v) : 0.0f));
          report += sample_str;
          XELOGI("{}", sample_str);
        }
      } else if (patch.is_color_ramp) {
        sprintf_s(line, "  %-24s: Mean RGB=(%.4f, %.4f, %.4f), Mean Y=%.4f\n",
                  patch.name.c_str(), mean_r, mean_g, mean_b, mean_y);
        XELOGI("{}", line);
        report += line;
      }

      D3D12_RANGE r_none = {0, 0};
      rb_buf->Unmap(0, &r_none);
    }
  }

  upload_buf->Unmap(0, &r_zero);
  test_cmd->Close();

  FILE* fp = nullptr;
  if (fopen_s(&fp, "scratch/synthetic_color_transfer_report.txt", "w") == 0 && fp) {
    fputs(report.c_str(), fp);
    fclose(fp);
    XELOGI("NeuralRenderingManager: [FASE H] Saved synthetic color report to "
           "scratch/synthetic_color_transfer_report.txt");
  }

  XELOGI("================================================================================");
  XELOGI("NeuralRenderingManager: [FASE H] SYNTHETIC COLOR TRANSFER TEST COMPLETE");
  XELOGI("================================================================================");
}

ID3D12Resource* NeuralRenderingManager::Process(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* input_guest_output,
    uint64_t guest_generation,
    const D3D12Presenter::GuestDepthCandidate& depth_candidate) {
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
  bool format_or_res_changed = false;
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
    format_or_res_changed = true;
    need_history_reset_ = true;

    if (ngx_session_) {
      ngx_session_->Invalidate();
    }
    if (motion_estimator_) {
      motion_estimator_->Initialize(device_, direct_queue_, width, height);
    }
    if (depth_provider_) {
      depth_provider_->Invalidate();
    }
    split_output_resource_.Reset();
  }

  if (!logged_first_frame_) {
    logged_first_frame_ = true;
    std::string g_title = "Unknown";
    uint32_t g_title_id = 0;
    xe::kernel::KernelState* ks = xe::kernel::KernelState::shared();
    if (ks && ks->emulator()) {
      g_title = ks->emulator()->title_name();
      g_title_id = ks->emulator()->title_id();
      if (g_title_id == 0) {
        g_title_id = ks->title_id();
      }
    }
    XELOGI(
        "NeuralRenderingManager: First frame captured: {}x{} format {} | "
        "Guest title: {} | Guest Title ID: {:08X}",
        current_width_, current_height_, uint32_t(current_format_),
        g_title, g_title_id);
  }

  // Check guest frame cadence:
  // Advance history and execute optical flow + depth ONLY when a new guest frame is produced.
  bool is_new_guest_frame =
      (guest_generation != 0 && guest_generation != last_guest_generation_) ||
      format_or_res_changed;

  uint64_t freq = xe::Clock::QueryHostTickFrequency();
  double time_motion_ms = 0.0;
  double time_depth_ms = 0.0;

  if (is_new_guest_frame) {
    total_frames_++;
    last_guest_generation_ = guest_generation;

    // Execute motion estimation only for new guest frames.
    if (motion_estimator_ && motion_estimator_->is_valid()) {
      uint64_t t0 = xe::Clock::QueryHostTickCount();
      motion_estimator_->EstimateMotion(command_list, input_guest_output,
                                        guest_generation);
      uint64_t t1 = xe::Clock::QueryHostTickCount();
      time_motion_ms = (t1 - t0) * 1000.0 / freq;
      last_motion_time_ms_ = time_motion_ms;
      total_motion_time_ms_ += time_motion_ms;
    }

    // Process depth candidate for new guest frame.
    if (depth_provider_ && command_list) {
      uint64_t t0 = xe::Clock::QueryHostTickCount();
      depth_provider_->ProcessFrame(command_list, depth_candidate,
                                    width, height, guest_generation);
      uint64_t t1 = xe::Clock::QueryHostTickCount();
      time_depth_ms = (t1 - t0) * 1000.0 / freq;
      last_depth_time_ms_ = time_depth_ms;
      total_depth_time_ms_ += time_depth_ms;
    }
  }

  // Candidate tracking and history reset conditions
  uint64_t curr_cand_res =
      reinterpret_cast<uint64_t>(depth_candidate.resource);
  if (curr_cand_res != last_depth_candidate_resource_) {
    last_depth_candidate_resource_ = curr_cand_res;
    need_history_reset_ = true;
  }

  bool depth_inverted = depth_candidate.inverted;
  CandidateTrustState trust_state =
      depth_provider_ ? depth_provider_->candidate_trust_state()
                      : CandidateTrustState::kUntrusted;
  int trust_int = static_cast<int>(trust_state);
  if (trust_int != last_trust_state_) {
    last_trust_state_ = trust_int;
    need_history_reset_ = true;
  }

  // Ensure the synthetic NGX session feature is initialized for current resolution.
  if (ngx_session_ && !ngx_session_->IsUnavailable() && command_list) {
    ngx_session_->EnsureFeature(command_list, current_width_, current_height_,
                                current_format_, depth_inverted);
    if (ngx_session_->active_consumer() == NeuralConsumer::kDeepFriedChicken) {
      DfcState dfc_state = ngx_session_->dfc_state();
      int dfc_int = static_cast<int>(dfc_state);
      if (dfc_int != last_dfc_state_) {
        last_dfc_state_ = dfc_int;
        need_history_reset_ = true;
      }
    }
    if (cvars::d3d12_neural_synthetic_color_test && !synthetic_color_test_executed_ &&
        ngx_session_->IsReady()) {
      synthetic_color_test_executed_ = true;
      RunSyntheticColorTransferTest(command_list);
    }
  }

  // Debug view: if enabled, output raw depth buffer for diagnostic inspection.
  if (cvars::d3d12_neural_depth_debug_view && depth_provider_) {
    const auto& depth_frame = depth_provider_->GetCurrentDepthFrame();
    if (depth_frame.resource) {
      return depth_frame.resource;
    }
  }

  // Build the explicit neural rendering frame contract (FASE H)
  NeuralFrameContract contract = {};
  contract.guest_generation = guest_generation;
  contract.color = input_guest_output;
  contract.width = width;
  contract.height = height;
  contract.depth_inverted = depth_inverted;
  contract.depth_trusted = (trust_state == CandidateTrustState::kTrusted);
  contract.reset_history = need_history_reset_;

  if (depth_provider_) {
    const auto& depth_frame = depth_provider_->GetCurrentDepthFrame();
    contract.depth = depth_frame.resource;
    if (depth_frame.depth_history_discontinuity) {
      contract.reset_history = true;
    }
    if (depth_frame.msaa_policy == DepthMsaaPolicy::kUnsupported) {
      contract.depth_trusted = false;
    }
  }
  if (motion_estimator_ && motion_estimator_->is_valid()) {
    contract.motion_vectors = motion_estimator_->GetMotionVectors();
    contract.motion_valid = (contract.motion_vectors != nullptr);
  }
  if (ngx_session_) {
    contract.output = ngx_session_->output_resource();
  }

  // Invariant validation
  bool contract_valid = true;
  const char* bypass_reason = nullptr;

  if (guest_generation == 0) {
    contract_valid = false;
    bypass_reason = "No active guest frame generation";
  } else if (!ngx_session_ || !ngx_session_->IsReady()) {
    contract_valid = false;
    bypass_reason = "NGX session not ready";
  } else if (ngx_session_->has_competing_consumer()) {
    contract_valid = false;
    bypass_reason = "Multiple competing neural consumers detected (conflict)";
  } else if (ngx_session_->active_consumer() == NeuralConsumer::kDeepFriedChicken &&
             ngx_session_->dfc_state() != DfcState::kArmed &&
             ngx_session_->dfc_state() != DfcState::kModuleAbsent) {
    contract_valid = false;
    bypass_reason = "Deep Fried Chicken present but not ARMED";
  } else if (!contract.depth_trusted || !contract.depth) {
    contract_valid = false;
    bypass_reason = "Depth candidate not TRUSTED or missing";
  } else if (!contract.motion_valid || !contract.motion_vectors) {
    contract_valid = false;
    bypass_reason = "Motion vectors invalid or missing";
  } else if (!contract.color || !contract.output) {
    contract_valid = false;
    bypass_reason = "Color or Output resource missing";
  }

  contract.valid = contract_valid;
  contract.bypass_reason = bypass_reason;

  const std::string& mode = cvars::d3d12_neural_output_mode;

  // Mode: contract_only
  if (mode == "contract_only") {
    if (is_new_guest_frame && (total_frames_ % 60 == 1)) {
      XELOGI(
          "NeuralRenderingManager: [CONTRACT_ONLY] frame={}, gen={}, valid={}, "
          "depth_trusted={}, consumer={}, reason={}, timings: OF={:.3f}ms, Depth={:.3f}ms",
          total_frames_, guest_generation, contract.valid, contract.depth_trusted,
          ngx_session_ ? ngx_session_->GetConsumerString() : "None",
          contract.valid ? "CONTRACT_OK" : contract.bypass_reason,
          time_motion_ms, time_depth_ms);
    }
    return input_guest_output;
  }

  // Mode: full
  if (mode == "full") {
    if (!contract.valid) {
      bypassed_frames_++;
      need_history_reset_ = true;
      if (is_new_guest_frame && (total_frames_ % 60 == 1)) {
        XELOGI(
            "NeuralRenderingManager: [FULL] frame={}, gen={}, BYPASSED: {}",
            total_frames_, guest_generation, contract.bypass_reason);
      }
      return input_guest_output;
    }

    uint64_t t_ngx0 = xe::Clock::QueryHostTickCount();
    bool eval_ok = ngx_session_->Evaluate(command_list, contract);
    uint64_t t_ngx1 = xe::Clock::QueryHostTickCount();
    double time_ngx_ms = (t_ngx1 - t_ngx0) * 1000.0 / freq;
    last_ngx_time_ms_ = time_ngx_ms;
    total_ngx_time_ms_ += time_ngx_ms;

    double time_total_ms = time_motion_ms + time_depth_ms + time_ngx_ms;
    last_total_pipeline_time_ms_ = time_total_ms;
    total_pipeline_time_ms_ += time_total_ms;

    if (eval_ok) {
      evaluated_frames_++;
      if (need_history_reset_) {
        reset_frames_++;
        need_history_reset_ = false;
      }
      if (!readback_scheduled_ && evaluated_frames_ >= 30) {
        ScheduleDiagnosticReadback(command_list, input_guest_output,
                                   ngx_session_->output_resource(), width,
                                   height, current_format_);
      }
      if (is_new_guest_frame && (total_frames_ % 60 == 1)) {
        XELOGI(
            "NeuralRenderingManager: [FULL] [{}] frame={}, gen={}, "
            "timings: OF={:.3f}ms, Depth={:.3f}ms, NGX_dep={:.3f}ms, Total={:.3f}ms "
            "(eval={}, bypassed={}, fails={})",
            ngx_session_ ? ngx_session_->GetLevelString() : "UNKNOWN",
            total_frames_, guest_generation, time_motion_ms, time_depth_ms,
            time_ngx_ms, time_total_ms, evaluated_frames_, bypassed_frames_,
            contract_failures_);
      }
      if (is_new_guest_frame && (guest_generation % 500 == 0 || guest_generation % 1000 == 0)) {
        double ws_mb = 0.0, private_mb = 0.0, commit_mb = 0.0;
        PROCESS_MEMORY_COUNTERS_EX pmc_ex = {};
        if (GetProcessMemoryInfo(
                GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc_ex),
                sizeof(pmc_ex))) {
          ws_mb = double(pmc_ex.WorkingSetSize) / (1024.0 * 1024.0);
          private_mb = double(pmc_ex.PagefileUsage) / (1024.0 * 1024.0);
          commit_mb = double(pmc_ex.PrivateUsage) / (1024.0 * 1024.0);
        }

        double dedicated_vram_mb = 0.0, budget_vram_mb = 0.0, shared_vram_mb = 0.0;
        if (device_) {
          Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
          if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
              Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
              if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
                DXGI_QUERY_VIDEO_MEMORY_INFO local_info = {};
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                        0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local_info))) {
                  dedicated_vram_mb = double(local_info.CurrentUsage) / (1024.0 * 1024.0);
                  budget_vram_mb = double(local_info.Budget) / (1024.0 * 1024.0);
                }
                DXGI_QUERY_VIDEO_MEMORY_INFO non_local_info = {};
                if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                        0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non_local_info))) {
                  shared_vram_mb = double(non_local_info.CurrentUsage) / (1024.0 * 1024.0);
                }
              }
            }
          }
        }

        std::string g_title = "Unknown";
        uint32_t g_title_id = 0;
        xe::kernel::KernelState* ks = xe::kernel::KernelState::shared();
        if (ks && ks->emulator()) {
          g_title = ks->emulator()->title_name();
          g_title_id = ks->emulator()->title_id();
          if (g_title_id == 0) {
            g_title_id = ks->title_id();
          }
        }

        uint32_t inv_transitions = depth_provider_ ? depth_provider_->depth_inverted_transitions() : 0;
        uint32_t cand_switches = depth_provider_ ? depth_provider_->candidate_switch_count() : 0;
        uint64_t msaa_heurs = depth_provider_ ? depth_provider_->msaa_heuristic_frames() : 0;
        uint32_t feat_creates = ngx_session_ ? ngx_session_->feature_create_count() : 0;
        uint32_t feat_releases = ngx_session_ ? ngx_session_->feature_release_count() : 0;
        uint32_t res_count = GetNeuralOwnedResourceCount();
        uint32_t desc_count = GetNeuralDescriptorCount();

        XELOGI(
            "NeuralRenderingManager: [PERIODIC TELEMETRY] Title: '{}' (ID: {:08X}) | "
            "gen={}, frames={}, eval={}, bypassed={}, fails={}, resets={}, recreations={}, "
            "depth_fmt={}, inverted={}, score={:.2f}, cand_switches={}, inv_trans={}, msaa_heur={}, "
            "WS={:.1f}MB, Private={:.1f}MB, Commit={:.1f}MB, DedicatedVRAM={:.1f}MB, BudgetVRAM={:.1f}MB, SharedVRAM={:.1f}MB, "
            "NeuralResCount={}, NeuralDescCount={}, FeatureCreates={}, FeatureReleases={}, "
            "OF={:.3f}ms, Depth={:.3f}ms, NGX_dep={:.3f}ms",
            g_title, g_title_id,
            guest_generation, total_frames_, evaluated_frames_, bypassed_frames_,
            contract_failures_, reset_frames_,
            feat_creates > 1 ? (feat_creates - 1) : 0,
            uint32_t(depth_candidate.dxgi_format), depth_candidate.inverted,
            depth_candidate.score, cand_switches, inv_transitions, msaa_heurs,
            ws_mb, private_mb, commit_mb, dedicated_vram_mb, budget_vram_mb, shared_vram_mb,
            res_count, desc_count, feat_creates, feat_releases,
            time_motion_ms, time_depth_ms, time_ngx_ms);
      }
      return ngx_session_->output_resource();
    } else {
      contract_failures_++;
      need_history_reset_ = true;
      if (contract_failures_ <= 5 || (contract_failures_ % 60 == 0)) {
        XELOGW(
            "NeuralRenderingManager: [FULL] frame={}, gen={}, Evaluate FAILED (fails={})",
            total_frames_, guest_generation, contract_failures_);
      }
      return input_guest_output;
    }
  }

  // Mode: split
  if (mode == "split") {
    if (!contract.valid) {
      bypassed_frames_++;
      need_history_reset_ = true;
      if (is_new_guest_frame && (total_frames_ % 60 == 1)) {
        XELOGI(
            "NeuralRenderingManager: [SPLIT] frame={}, gen={}, BYPASSED: {}",
            total_frames_, guest_generation, contract.bypass_reason);
      }
      return input_guest_output;
    }

    uint64_t t_ngx0 = xe::Clock::QueryHostTickCount();
    bool eval_ok = ngx_session_->Evaluate(command_list, contract);
    uint64_t t_ngx1 = xe::Clock::QueryHostTickCount();
    double time_ngx_ms = (t_ngx1 - t_ngx0) * 1000.0 / freq;
    last_ngx_time_ms_ = time_ngx_ms;
    total_ngx_time_ms_ += time_ngx_ms;

    if (eval_ok) {
      evaluated_frames_++;
      if (need_history_reset_) {
        reset_frames_++;
        need_history_reset_ = false;
      }
      if (!readback_scheduled_ && evaluated_frames_ >= 30) {
        ScheduleDiagnosticReadback(command_list, input_guest_output,
                                   ngx_session_->output_resource(), width,
                                   height, current_format_);
      }
      uint64_t t_blit0 = xe::Clock::QueryHostTickCount();
      ID3D12Resource* split_res =
          BlitSplitOutput(command_list, input_guest_output,
                          ngx_session_->output_resource(), width, height);
      uint64_t t_blit1 = xe::Clock::QueryHostTickCount();
      double time_blit_ms = (t_blit1 - t_blit0) * 1000.0 / freq;
      last_blit_time_ms_ = time_blit_ms;
      total_blit_time_ms_ += time_blit_ms;
      split_blits_++;

      double time_total_ms =
          time_motion_ms + time_depth_ms + time_ngx_ms + time_blit_ms;
      last_total_pipeline_time_ms_ = time_total_ms;
      total_pipeline_time_ms_ += time_total_ms;

      if (is_new_guest_frame && (total_frames_ % 60 == 1)) {
        XELOGI(
            "NeuralRenderingManager: [SPLIT] [{}] frame={}, gen={}, "
            "timings: OF={:.3f}ms, Depth={:.3f}ms, NGX={:.3f}ms, Blit={:.3f}ms, Total={:.3f}ms "
            "(eval={}, bypassed={}, split={}, fails={})",
            ngx_session_ ? ngx_session_->GetLevelString() : "UNKNOWN",
            total_frames_, guest_generation, time_motion_ms, time_depth_ms,
            time_ngx_ms, time_blit_ms, time_total_ms, evaluated_frames_,
            bypassed_frames_, split_blits_, contract_failures_);
      }
      return split_res ? split_res : ngx_session_->output_resource();
    } else {
      contract_failures_++;
      need_history_reset_ = true;
      if (contract_failures_ <= 5 || (contract_failures_ % 60 == 0)) {
        XELOGW(
            "NeuralRenderingManager: [SPLIT] frame={}, gen={}, Evaluate FAILED (fails={})",
            total_frames_, guest_generation, contract_failures_);
      }
      return input_guest_output;
    }
  }

  // Default: passthrough
  return input_guest_output;
}

void NeuralRenderingManager::OnFrameSubmitted(ID3D12CommandQueue* direct_queue) {
  if (readback_scheduled_ && !readback_completed_ && direct_queue &&
      readback_fence_) {
    readback_fence_value_++;
    direct_queue->Signal(readback_fence_.Get(), readback_fence_value_);
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (evt) {
      if (SUCCEEDED(readback_fence_->SetEventOnCompletion(readback_fence_value_,
                                                          evt))) {
        WaitForSingleObject(evt, 2000);
      }
      CloseHandle(evt);
    }
    ProcessDiagnosticReadback();
  }
  if (depth_provider_) {
    depth_provider_->OnFrameSubmitted(direct_queue);
  }
}

uint32_t NeuralRenderingManager::GetNeuralOwnedResourceCount() const {
  uint32_t count = 0;
  if (motion_estimator_) {
    count += motion_estimator_->GetResourceCount();
  }
  if (depth_provider_) {
    count += depth_provider_->GetResourceCount();
  }
  if (ngx_session_) {
    count += ngx_session_->GetResourceCount();
  }
  if (split_output_resource_) count++;
  if (readback_original_buffer_) count++;
  if (readback_processed_buffer_) count++;
  return count;
}

uint32_t NeuralRenderingManager::GetNeuralDescriptorCount() const {
  uint32_t count = 0;
  if (motion_estimator_) {
    count += motion_estimator_->GetDescriptorCount();
  }
  if (depth_provider_) {
    count += depth_provider_->GetDescriptorCount();
  }
  if (ngx_session_) {
    count += ngx_session_->GetDescriptorCount();
  }
  return count;
}

}  // namespace neural
}  // namespace d3d12
}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WIN32
