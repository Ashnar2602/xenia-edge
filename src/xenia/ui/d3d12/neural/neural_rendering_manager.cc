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
#include "xenia/ui/d3d12/d3d12_util.h"
#include "xenia/ui/d3d12/neural/depth_provider.h"
#include "xenia/ui/d3d12/neural/motion_estimator.h"
#include "xenia/ui/d3d12/neural/nvidia_optical_flow_estimator.h"
#include "xenia/ui/d3d12/neural/synthetic_ngx_session.h"
#include <algorithm>
#include <cmath>

DEFINE_string(d3d12_neural_output_mode, "passthrough",
              "Neural rendering presentation mode: passthrough | "
              "contract_only | full | split.",
              "GPU");

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
  if (ngx_session_) {
    XELOGI("  Pipeline Operating Level:    {}", ngx_session_->GetLevelString());
    XELOGI("  Interception Confirmed:      {}",
           ngx_session_->IsInterceptionConfirmed() ? "YES (Level 3)" : "NO");
    XELOGI("  Deep Fried Chicken state:    {}",
           uint32_t(ngx_session_->dfc_state()));
    XELOGI("  Deep Fried Chicken ABI:      {}", ngx_session_->dfc_abi());
    XELOGI("  nvngx_dlssnr.dll loaded:     {}",
           ngx_session_->dlssnr_info().loaded ? "YES" : "NO");
  }
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
        ngx_session_->neural_level() >= NeuralLevel::kLevel3_ConsumerArmed) {
      XELOGI("  Average Neural evaluate time:{:.3f} ms", avg_ngx_time_ms());
    } else {
      XELOGI("  Average Native DLAA dispatch:{:.3f} ms", avg_ngx_time_ms());
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
    XELOGI(
        "NeuralRenderingManager: First frame captured: {}x{} format {}",
        current_width_, current_height_, uint32_t(current_format_));
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
    DfcState dfc_state = ngx_session_->dfc_state();
    int dfc_int = static_cast<int>(dfc_state);
    if (dfc_int != last_dfc_state_) {
      last_dfc_state_ = dfc_int;
      need_history_reset_ = true;
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
  } else if (ngx_session_->dfc_state() != DfcState::kArmed &&
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
          "depth_trusted={}, DFC={}, reason={}, timings: OF={:.3f}ms, Depth={:.3f}ms",
          total_frames_, guest_generation, contract.valid, contract.depth_trusted,
          ngx_session_ ? uint32_t(ngx_session_->dfc_state()) : 0,
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
            "timings: OF={:.3f}ms, Depth={:.3f}ms, NGX={:.3f}ms, Total={:.3f}ms "
            "(eval={}, bypassed={}, fails={})",
            ngx_session_ ? ngx_session_->GetLevelString() : "UNKNOWN",
            total_frames_, guest_generation, time_motion_ms, time_depth_ms,
            time_ngx_ms, time_total_ms, evaluated_frames_, bypassed_frames_,
            contract_failures_);
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

}  // namespace neural
}  // namespace d3d12
}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WIN32
