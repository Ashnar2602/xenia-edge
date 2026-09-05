/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_D3D12_D3D12_RENDER_TARGET_CACHE_H_
#define XENIA_GPU_D3D12_D3D12_RENDER_TARGET_CACHE_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/gpu/d3d12/d3d12_shared_memory.h"
#include "xenia/gpu/d3d12/d3d12_texture_cache.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/edram_dump_shader.h"
#include "xenia/gpu/edram_transfer_shader.h"
#include "xenia/gpu/render_target_cache.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/gpu/xenos.h"
#include "xenia/memory.h"
#include "xenia/ui/d3d12/d3d12_cpu_descriptor_pool.h"
#include "xenia/ui/d3d12/d3d12_provider.h"
#include "xenia/ui/d3d12/d3d12_upload_buffer_pool.h"
#include "xenia/ui/d3d12/d3d12_util.h"

namespace xe {
namespace gpu {
namespace d3d12 {

class D3D12CommandProcessor;

class D3D12RenderTargetCache final : public RenderTargetCache {
 public:
  D3D12RenderTargetCache(const RegisterFile& register_file,
                         const Memory& memory, TraceWriter& trace_writer,
                         uint32_t draw_resolution_scale_x,
                         uint32_t draw_resolution_scale_y,
                         D3D12CommandProcessor& command_processor)
      : RenderTargetCache(register_file, memory, &trace_writer,
                          draw_resolution_scale_x, draw_resolution_scale_y),
        command_processor_(command_processor),
        trace_writer_(trace_writer) {}
  ~D3D12RenderTargetCache() override;

  // Shader code for resolve copy operations.
  struct ResolveCopyShaderCode {
    const void* unscaled;
    size_t unscaled_size;
    const void* scaled;
    size_t scaled_size;
  };

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  void CompletedSubmissionUpdated();
  void BeginSubmission();

  Path GetPath() const override { return path_; }

  bool Update(bool is_rasterization_done,
              reg::RB_DEPTHCONTROL normalized_depth_control,
              uint32_t normalized_color_mask,
              const Shader& vertex_shader) override;

  void InvalidateCommandListRenderTargets() {
    are_current_command_list_render_targets_valid_ = false;
  }

  bool msaa_2x_supported() const { return msaa_2x_supported_; }

  void WriteEdramRawSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle);
  void WriteEdramRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle);
  void WriteEdramUintPow2SRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                       uint32_t element_size_bytes_pow2);
  void WriteEdramUintPow2UAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                       uint32_t element_size_bytes_pow2);

  // Performs the resolve to a shared memory area according to the current
  // register values, and also clears the render targets if needed. Must be in a
  // frame for calling. copy_dest_info_out, if not null, receives the
  // destination info with the format normalized to the xenos::TextureFormat
  // the copy was actually performed with (only meaningful when a nonzero
  // length was written).
  // written_scaled_out: whether the data went to the scaled resolve address
  // space rather than shared memory (native resolves don't).
  bool Resolve(const Memory& memory, D3D12SharedMemory& shared_memory,
               D3D12TextureCache& texture_cache, uint32_t& written_address_out,
               uint32_t& written_length_out,
               reg::RB_COPY_DEST_INFO* copy_dest_info_out = nullptr,
               bool* written_scaled_out = nullptr);

  void RestoreEdramSnapshot(const void* snapshot);

  // For host render targets.

  bool gamma_render_target_as_unorm16() const {
    return gamma_render_target_as_unorm16_;
  }

  // Using R16G16[B16A16]_SNORM, which are -1...1, not the needed -32...32.
  // Persistent data doesn't depend on this, so can be overriden by per-game
  // configuration.
  bool IsFixed16TruncatedToMinus1To1() const {
    return GetPath() == Path::kHostRenderTargets &&
           !cvars::snorm16_render_target_full_range;
  }

  bool depth_float24_round() const { return depth_float24_round_; }
  bool depth_float24_convert_in_pixel_shader() const {
    return depth_float24_convert_in_pixel_shader_;
  }

  DXGI_FORMAT GetColorResourceDXGIFormat(
      xenos::ColorRenderTargetFormat format) const;
  DXGI_FORMAT GetColorDrawDXGIFormat(
      xenos::ColorRenderTargetFormat format) const;
  DXGI_FORMAT GetColorOwnershipTransferDXGIFormat(
      xenos::ColorRenderTargetFormat format,
      bool* is_integer_out = nullptr) const;
  static DXGI_FORMAT GetDepthResourceDXGIFormat(
      xenos::DepthRenderTargetFormat format);
  static DXGI_FORMAT GetDepthDSVDXGIFormat(
      xenos::DepthRenderTargetFormat format);
  static DXGI_FORMAT GetDepthSRVDepthDXGIFormat(
      xenos::DepthRenderTargetFormat format);
  static DXGI_FORMAT GetDepthSRVStencilDXGIFormat(
      xenos::DepthRenderTargetFormat format);

  enum class DepthDirectionConfidence : uint32_t {
    kUnknown = 0,
    kHeuristic = 1,
    kKnown = 2,
  };

  struct DepthCandidateInfo {
    ID3D12Resource* resource = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT dxgi_format = DXGI_FORMAT_UNKNOWN;
    xenos::DepthRenderTargetFormat format =
        xenos::DepthRenderTargetFormat::kD24S8;
    uint32_t sample_count = 1;
    bool is_float = false;

    // Viewport / subrect
    uint32_t vp_x = 0;
    uint32_t vp_y = 0;
    uint32_t vp_width = 0;
    uint32_t vp_height = 0;
    uint32_t color_width = 0;
    uint32_t color_height = 0;
    ID3D12Resource* color_resource = nullptr;

    // Depth bounds and direction
    float depth_near = 0.0f;
    float depth_far = 1.0f;
    bool inverted = false;
    DepthDirectionConfidence confidence = DepthDirectionConfidence::kUnknown;

    // Heuristic scoring & ambiguity
    float score = 0.0f;
    float second_best_score = 0.0f;
    float score_margin = 0.0f;
    std::string reason;
    bool valid = false;
    uint32_t explicit_sample = 0xFFFFFFFF;
  };

  DepthCandidateInfo FindBestDepthCandidate(uint32_t guest_width,
                                            uint32_t guest_height);

 protected:
  bool IsGammaFormatHostStorageSeparate() const override;

  uint32_t GetMaxRenderTargetWidth() const override {
    return D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
  }
  uint32_t GetMaxRenderTargetHeight() const override {
    return D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
  }

  RenderTarget* CreateRenderTarget(RenderTargetKey key) override;

  bool IsHostDepthEncodingDifferent(
      xenos::DepthRenderTargetFormat format) const override;

  void RequestPixelShaderInterlockBarrier() override;

 private:
  enum class EdramBufferModificationStatus {
    // The values are ordered by how strong the barrier conditions are.
    // No uncommitted ROV/UAV writes.
    kUnmodified,
    // Need to commit before the next ROV usage with overlap.
    kAsROV,
    // Need to commit before any next ROV usage.
    kAsUAV,
  };
  void TransitionEdramBuffer(D3D12_RESOURCE_STATES new_state);
  void MarkEdramBufferModified(
      EdramBufferModificationStatus modification_status =
          EdramBufferModificationStatus::kAsUAV);
  void CommitEdramBufferUAVWrites(EdramBufferModificationStatus commit_status =
                                      EdramBufferModificationStatus::kAsROV);

  D3D12CommandProcessor& command_processor_;
  TraceWriter& trace_writer_;

  Path path_ = Path::kHostRenderTargets;

  // For host render targets, an EDRAM-sized scratch buffer for:
  // - Guest render target data copied from host render targets during copying
  //   in resolves and in frame trace creation.
  // - Host float32 depth in ownership transfers when the host depth texture and
  //   the destination are the same.
  // For rasterizer-ordered view, the buffer containing the EDRAM data.
  // (Note that if a hybrid RTV / DSV + ROV approach to color render targets is
  //  added, which is, however, unlikely as it would have very complicated
  //  interaction with depth / stencil testing, host depth will need to be
  //  copied to a different buffer - the same range may have ROV-owned color and
  //  host float32 depth at the same time).
  ID3D12Resource* edram_buffer_ = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS edram_buffer_gpu_address_ = 0;
  D3D12_RESOURCE_STATES edram_buffer_state_;
  EdramBufferModificationStatus edram_buffer_modification_status_ =
      EdramBufferModificationStatus::kUnmodified;

  // Non-shader-visible descriptor heap containing pre-created SRV and UAV
  // descriptors of the EDRAM buffer, for faster binding (by copying rather
  // than creation).
  enum class EdramBufferDescriptorIndex : uint32_t {
    kRawSRV,
    kR32UintSRV,
    kR32G32UintSRV,
    kR32G32B32A32UintSRV,
    kRawUAV,
    kR32UintUAV,
    kR32G32UintUAV,
    kR32G32B32A32UintUAV,

    kCount,
  };
  ID3D12DescriptorHeap* edram_buffer_descriptor_heap_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE edram_buffer_descriptor_heap_start_;

  // Resolve copying root signature and pipelines.
  // Parameter 0 - draw_util::ResolveCopyShaderConstants or its ::DestRelative.
  // Parameter 1 - destination (shared memory or a part of it).
  // Parameter 2 - source (EDRAM).
  ID3D12RootSignature* resolve_copy_root_signature_ = nullptr;
  ID3D12PipelineState* resolve_copy_pipelines_[size_t(
      draw_util::ResolveCopyShaderIndex::kCount)] = {};
  // Unscaled variants for fully native resolves. Only created with resolution
  // scaling, otherwise the main set is already unscaled. A separate set because
  // the scaled shaders can't just run at 1x1, their root constants and
  // destination space assume the scaled resolve buffer.
  ID3D12RootSignature* resolve_copy_native_root_signature_ = nullptr;
  ID3D12PipelineState* resolve_copy_native_pipelines_[size_t(
      draw_util::ResolveCopyShaderIndex::kCount)] = {};

  // For traces.
  ID3D12Resource* edram_snapshot_download_buffer_ = nullptr;
  bool edram_snapshot_download_mapped_ = false;
  std::unique_ptr<ui::d3d12::D3D12UploadBufferPool>
      edram_snapshot_restore_pool_;

  // For host render targets.

  class D3D12RenderTarget final : public RenderTarget {
   public:
    // descriptor_load is present when the DXGI formats are different for
    // drawing and bit-exact loading (for NaN pattern preservation across EDRAM
    // tile ownership transfers in floating-point formats, and to distinguish
    // between two -1 representations in snorm formats).
    D3D12RenderTarget(
        RenderTargetKey key, ID3D12Resource* resource,
        ui::d3d12::D3D12CpuDescriptorPool::Descriptor&& descriptor_draw,
        ui::d3d12::D3D12CpuDescriptorPool::Descriptor&&
            descriptor_load_separate,
        ui::d3d12::D3D12CpuDescriptorPool::Descriptor&& descriptor_srv,
        ui::d3d12::D3D12CpuDescriptorPool::Descriptor&& descriptor_srv_stencil,
        D3D12_RESOURCE_STATES resource_state)
        : RenderTarget(key),
          resource_(resource),
          descriptor_draw_(std::move(descriptor_draw)),
          descriptor_load_separate_(std::move(descriptor_load_separate)),
          descriptor_srv_(std::move(descriptor_srv)),
          descriptor_srv_stencil_(std::move(descriptor_srv_stencil)),
          resource_state_(resource_state) {}

    ID3D12Resource* resource() const { return resource_.Get(); }
    const ui::d3d12::D3D12CpuDescriptorPool::Descriptor& descriptor_draw()
        const {
      return descriptor_draw_;
    }
    const ui::d3d12::D3D12CpuDescriptorPool::Descriptor& descriptor_srv()
        const {
      return descriptor_srv_;
    }
    const ui::d3d12::D3D12CpuDescriptorPool::Descriptor&
    descriptor_srv_stencil() const {
      return descriptor_srv_stencil_;
    }
    const ui::d3d12::D3D12CpuDescriptorPool::Descriptor&
    descriptor_load_separate() const {
      return descriptor_load_separate_;
    }

    D3D12_RESOURCE_STATES SetResourceState(D3D12_RESOURCE_STATES new_state) {
      D3D12_RESOURCE_STATES old_state = resource_state_;
      resource_state_ = new_state;
      return old_state;
    }

    uint32_t temporary_srv_descriptor_index() const {
      return temporary_srv_descriptor_index_;
    }
    void SetTemporarySRVDescriptorIndex(uint32_t index) {
      temporary_srv_descriptor_index_ = index;
    }
    uint32_t temporary_srv_descriptor_index_stencil() const {
      return temporary_srv_descriptor_index_stencil_;
    }
    void SetTemporarySRVDescriptorIndexStencil(uint32_t index) {
      temporary_srv_descriptor_index_stencil_ = index;
    }
    uint32_t temporary_sort_index() const { return temporary_sort_index_; }
    void SetTemporarySortIndex(uint32_t index) {
      temporary_sort_index_ = index;
    }

   private:
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
    ui::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_draw_;
    ui::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_load_separate_;
    // Texture SRV non-shader-visible descriptors, to prepare shader-visible
    // descriptors faster, by copying rather than by creating every time.
    // TODO(Triang3l): With bindless resources, persistently store them in the
    // heap.
    ui::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_srv_;
    ui::d3d12::D3D12CpuDescriptorPool::Descriptor descriptor_srv_stencil_;
    D3D12_RESOURCE_STATES resource_state_;
    // Temporary storage for indices in operations like transfers and dumps.
    uint32_t temporary_srv_descriptor_index_ = UINT32_MAX;
    uint32_t temporary_srv_descriptor_index_stencil_ = UINT32_MAX;
    uint32_t temporary_sort_index_ = 0;
  };

  // Root parameters of the ownership transfer pixel shaders, in the register
  // layout Mesa's spirv_to_dxil emits for the emitter's descriptor sets: each
  // set becomes the register space of the same number, the host depth copy's
  // storage buffer becomes a UAV, and the push constants land in Mesa's push
  // constant CBV.
  enum TransferUsedRootParameter : uint32_t {
    // The address, the host depth address and the stencil mask (changed 8
    // times per transfer) are dwords of Mesa's one push constant CBV.
    kTransferUsedRootParameterPushConstants,
    kTransferUsedRootParameterColorSRV,
    // Mutually exclusive with ColorSRV.
    kTransferUsedRootParameterDepthSRV,
    // Mutually exclusive with ColorSRV.
    kTransferUsedRootParameterStencilSRV,
    kTransferUsedRootParameterHostDepthSRV,
    // Mutually exclusive with HostDepthSRV - the copy modes read the previous
    // owner's depth back out of the EDRAM buffer, which the emitter declares
    // as a read-only storage buffer, so Mesa gives it a raw buffer SRV rather
    // than the UAV the written EDRAM buffer of a dump shader gets.
    kTransferUsedRootParameterHostDepthBuffer,
    kTransferUsedRootParameterCount,

    kTransferUsedRootParameterPushConstantsBit =
        uint32_t(1) << kTransferUsedRootParameterPushConstants,
    kTransferUsedRootParameterColorSRVBit =
        uint32_t(1) << kTransferUsedRootParameterColorSRV,
    kTransferUsedRootParameterDepthSRVBit =
        uint32_t(1) << kTransferUsedRootParameterDepthSRV,
    kTransferUsedRootParameterStencilSRVBit =
        uint32_t(1) << kTransferUsedRootParameterStencilSRV,
    kTransferUsedRootParameterHostDepthSRVBit =
        uint32_t(1) << kTransferUsedRootParameterHostDepthSRV,
    kTransferUsedRootParameterHostDepthBufferBit =
        uint32_t(1) << kTransferUsedRootParameterHostDepthBuffer,
  };

  // Mesa sizes the push constant CBV from the dwords the shader actually
  // loads, rounded up to a 16-byte row - no layout uses more than two.
  static constexpr uint32_t kTransferRootPushConstantDwords = 4;

  // What one mode's root signature has to declare: which of the root
  // parameters above its shader uses, and the register space Mesa gives each
  // of the emitter's descriptor sets. The mode fixes both, since
  // use_stencil_reference_output_ doesn't change for the lifetime of the
  // cache.
  struct TransferRootSignatureInfo {
    uint32_t used_root_parameters;
    // Of the color or the depth / stencil source textures.
    uint32_t space_source;
    // Of the host depth source, texture or buffer.
    uint32_t space_host_depth;
  };
  TransferRootSignatureInfo GetTransferRootSignatureInfo(
      EdramTransferMode mode) const;

  struct TransferInvocation {
    Transfer transfer;
    EdramTransferShaderKey shader_key;
    TransferInvocation(const Transfer& transfer,
                       const EdramTransferShaderKey& shader_key)
        : transfer(transfer), shader_key(shader_key) {}
    bool operator<(const TransferInvocation& other_invocation) const {
      // TODO(Triang3l): See if it may be better to sort by the source in the
      // first place, especially when reading the same data multiple times (like
      // to write the stencil bits after depth) for better read locality.
      // Sort by the shader key primarily to reduce pipeline state (context)
      // switches.
      if (shader_key != other_invocation.shader_key) {
        return shader_key < other_invocation.shader_key;
      }
      // Host depth render targets are changed rarely if they exist, won't save
      // many binding changes, ignore them for simplicity (their existence is
      // caught by the shader key change).
      assert_not_null(transfer.source);
      assert_not_null(other_invocation.transfer.source);
      uint32_t source_index =
          static_cast<const D3D12RenderTarget*>(transfer.source)
              ->temporary_sort_index();
      uint32_t other_source_index = static_cast<const D3D12RenderTarget*>(
                                        other_invocation.transfer.source)
                                        ->temporary_sort_index();
      if (source_index != other_source_index) {
        return source_index < other_source_index;
      }
      return transfer.start_tiles < other_invocation.transfer.start_tiles;
    }
    bool CanBeMergedIntoOneDraw(
        const TransferInvocation& other_invocation) const {
      return shader_key == other_invocation.shader_key &&
             transfer.AreSourcesSame(other_invocation.transfer);
    }
  };

  enum {
    kHostDepthStoreRootParameterConstants,
    kHostDepthStoreRootParameterSource,
    kHostDepthStoreRootParameterDest,
    kHostDepthStoreRootParameterCount,
  };

  // Descriptor sets of the dump shaders, which Mesa's spirv_to_dxil turns into
  // the register spaces of the same number.
  enum DumpDescriptorSet : uint32_t {
    kDumpDescriptorSetEdram,
    kDumpDescriptorSetSource,
  };

  // Mesa sizes the push constant CBV from the dwords the shader actually
  // loads, rounded up to a 16-byte row. A direct resolve reads all of them,
  // which is what the root constants have to cover - a plain dump reads only
  // the pitches and the offsets and leaves the rest of the rows unread.
  static constexpr uint32_t kDumpRootPushConstantDwords =
      (kEdramDumpShaderPushConstantCount + 3) & ~uint32_t(3);

  // Root parameters of the dump compute shaders, in the register layout Mesa
  // emits for the emitter's descriptor sets: the EDRAM buffer is set 0
  // binding 0, so UAV u0 space0; the source and its stencil are set 1 bindings
  // 0 and 1, so SRV t0 and t1 of space1; the push constants land in Mesa's
  // push constant CBV at b1 space31.
  enum DumpRootParameter : uint32_t {
    // Pitches and offsets in one root constants parameter - the offsets may be
    // changed multiple times for the same source.
    kDumpRootParameterPushConstants,
    // One resolve may need multiple sources.
    kDumpRootParameterSource,

    // Not changed.
    kDumpRootParameterColorEdram = kDumpRootParameterSource + 1,

    kDumpRootParameterColorCount,

    // Same change frequency than the source (t1 of the same space, but in a
    // table of its own because the command processor can't contiguously
    // allocate multiple descriptors with bindless).
    kDumpRootParameterDepthStencil = kDumpRootParameterSource + 1,
    kDumpRootParameterDepthEdram,

    kDumpRootParameterDepthCount,
  };

  struct DumpInvocation {
    ResolveCopyDumpRectangle rectangle;
    EdramDumpShaderKey pipeline_key;
    DumpInvocation(const ResolveCopyDumpRectangle& rectangle,
                   const EdramDumpShaderKey& pipeline_key)
        : rectangle(rectangle), pipeline_key(pipeline_key) {}
    bool operator<(const DumpInvocation& other_invocation) const {
      // Sort by the pipeline key primarily to reduce pipeline state (context)
      // switches.
      if (pipeline_key != other_invocation.pipeline_key) {
        return pipeline_key < other_invocation.pipeline_key;
      }
      assert_not_null(rectangle.render_target);
      uint32_t render_target_index =
          static_cast<const D3D12RenderTarget*>(rectangle.render_target)
              ->temporary_sort_index();
      const ResolveCopyDumpRectangle& other_rectangle =
          other_invocation.rectangle;
      uint32_t other_render_target_index =
          static_cast<const D3D12RenderTarget*>(other_rectangle.render_target)
              ->temporary_sort_index();
      if (render_target_index != other_render_target_index) {
        return render_target_index < other_render_target_index;
      }
      if (rectangle.row_first != other_rectangle.row_first) {
        return rectangle.row_first < other_rectangle.row_first;
      }
      return rectangle.row_first_start < other_rectangle.row_first_start;
    }
  };

  // Returns:
  // - A pointer to 1 pipeline for writing color or depth (or stencil via
  //   SV_StencilRef).
  // - A pointer to 8 pipelines for writing stencil by discarding samples
  //   depending on whether they have one bit set, from 1 << 0 to 1 << 7.
  // - Null if failed to create.
  ID3D12PipelineState* const* GetOrCreateTransferPipelines(
      EdramTransferShaderKey key);

  // Do ownership transfers for render targets - each render target / vector may
  // be null / empty in case there's nothing to do for them.
  // resolve_clear_rectangle is expected to be provided by
  // PrepareHostRenderTargetsResolveClear which should do all the needed size
  // bound checks.
  void PerformTransfersAndResolveClears(
      uint32_t render_target_count, RenderTarget* const* render_targets,
      const std::vector<Transfer>* render_target_transfers,
      const uint64_t* render_target_resolve_clear_values = nullptr,
      const Transfer::Rectangle* resolve_clear_rectangle = nullptr);

  // Accepts an array of (1 + xenos::kMaxColorRenderTargets) render targets,
  // first depth, then color.
  void SetCommandListRenderTargets(
      RenderTarget* const* depth_and_color_render_targets);

  ID3D12PipelineState* GetOrCreateDumpPipeline(EdramDumpShaderKey key);

  // Assigns temporary sort and SRV descriptor indices to the render targets of
  // dump_rectangles_ and uploads their descriptors to a shader-visible heap.
  // Returns false if the heap request failed, in which case nothing has been
  // written to the command list yet.
  bool PrepareDumpSourceDescriptors();

  // Reads the render targets owning the resolve source straight into shared
  // memory in the guest texture layout, doing in one pass what dumping to the
  // EDRAM buffer and copying back out of it do in two. Returns false without
  // encoding anything if the resolve can't be done this way, leaving the
  // caller to take the round trip.
  bool DirectResolveRenderTargets(
      const draw_util::ResolveInfo& resolve_info,
      const draw_util::ResolveCopyShaderConstants& copy_shader_constants,
      uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
      uint32_t dump_pitch, bool copy_dest_scaled,
      D3D12SharedMemory& shared_memory, D3D12TextureCache& texture_cache);

  // Writes contents of host render targets within rectangles from
  // ResolveInfo::GetCopyEdramTileSpan to edram_buffer_ - with the plain 1x1
  // tile layout if native_layout is set.
  void DumpRenderTargets(uint32_t dump_base, uint32_t dump_row_length_used,
                         uint32_t dump_rows, uint32_t dump_pitch,
                         bool native_layout);

  void DumpAllRenderTargetsToEdram() override;
  bool BeginEdramSnapshotReadback() override;
  const void* MapEdramSnapshotReadback() override;
  void EndEdramSnapshotReadback() override;

  bool use_stencil_reference_output_ = false;

  bool gamma_render_target_as_unorm16_ = false;

  bool depth_float24_round_ = false;
  bool depth_float24_convert_in_pixel_shader_ = false;

  bool msaa_2x_supported_ = false;

  std::shared_ptr<ui::d3d12::D3D12CpuDescriptorPool> descriptor_pool_color_;
  std::shared_ptr<ui::d3d12::D3D12CpuDescriptorPool> descriptor_pool_depth_;
  std::shared_ptr<ui::d3d12::D3D12CpuDescriptorPool> descriptor_pool_srv_;
  ui::d3d12::D3D12CpuDescriptorPool::Descriptor null_rtv_descriptor_ss_;
  ui::d3d12::D3D12CpuDescriptorPool::Descriptor null_rtv_descriptor_ms_;

  // Possible tile ownership transfer paths:
  // - To color:
  //   - From color: 1 SRV (color).
  //   - From depth: 2 SRVs (depth, stencil).
  // - To depth / stencil (with SV_StencilRef):
  //   - From color: 1 SRV (color).
  //   - From depth: 2 SRVs (depth, stencil).
  //   - From color and float32 depth: 2 SRVs (color with stencil, depth).
  //     - Different depth buffer: depth SRV is a texture.
  //     - Same depth buffer: depth SRV is a buffer (pre-copied).
  // - To depth (no SV_StencilRef):
  //   - From color: 1 SRV (color).
  //   - From depth: 1 SRV (depth).
  //   - From color and float32 depth: 2 SRVs (color, depth).
  //     - Different depth buffer: depth SRV is a texture.
  //     - Same depth buffer: depth SRV is a buffer (pre-copied).
  // - To stencil (no SV_StencilRef):
  //   - From color: 1 SRV (color).
  //   - From depth: 1 SRV (stencil).

  const RenderTarget* const*
      current_command_list_render_targets_[1 + xenos::kMaxColorRenderTargets];
  bool are_current_command_list_render_targets_valid_ = false;

  // Temporary storage for descriptors used in PerformTransfersAndResolveClears
  // and DumpRenderTargets.
  std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> current_temporary_descriptors_cpu_;
  std::vector<ui::d3d12::util::DescriptorCpuGpuHandlePair>
      current_temporary_descriptors_gpu_;

  ID3D12RootSignature* host_depth_store_root_signature_ = nullptr;
  ID3D12PipelineState*
      host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k4X) + 1] = {};

  std::unique_ptr<ui::d3d12::D3D12UploadBufferPool>
      transfer_vertex_buffer_pool_;

  // One per transfer mode - the mode is what fixes the shader's bindings.
  ID3D12RootSignature*
      transfer_root_signatures_[size_t(EdramTransferMode::kCount)] = {};
  std::unordered_map<EdramTransferShaderKey, ID3D12PipelineState*,
                     EdramTransferShaderKey::Hasher>
      transfer_pipelines_;
  std::unordered_map<EdramTransferShaderKey,
                     std::array<ID3D12PipelineState*, 8>,
                     EdramTransferShaderKey::Hasher>
      transfer_stencil_bit_pipelines_;

  // Temporary storage for PerformTransfersAndResolveClears.
  std::vector<TransferInvocation> current_transfer_invocations_;

  // Temporary storage for DumpRenderTargets.
  std::vector<ResolveCopyDumpRectangle> dump_rectangles_;
  std::vector<DumpInvocation> dump_invocations_;

  ID3D12RootSignature* dump_root_signature_color_ = nullptr;
  ID3D12RootSignature* dump_root_signature_depth_ = nullptr;
  // Compute pipelines for copying host render target contents to the EDRAM
  // buffer. May be null if failed to create.
  std::unordered_map<EdramDumpShaderKey, ID3D12PipelineState*,
                     EdramDumpShaderKey::Hasher>
      dump_pipelines_;

  // Parameter 0 - 2 root constants (red, green).
  ID3D12RootSignature* uint32_rtv_clear_root_signature_ = nullptr;
  // [32 or 32_32][MSAA samples].
  ID3D12PipelineState*
      uint32_rtv_clear_pipelines_[2][size_t(xenos::MsaaSamples::k4X) + 1] = {};

  std::vector<Transfer> clear_transfers_[2];

  // For rasterizer-ordered view (pixel shader interlock).

  ID3D12RootSignature* resolve_rov_clear_root_signature_ = nullptr;
  // Clearing 32bpp color or depth.
  ID3D12PipelineState* resolve_rov_clear_32bpp_pipeline_ = nullptr;
  // Clearing 64bpp color.
  ID3D12PipelineState* resolve_rov_clear_64bpp_pipeline_ = nullptr;

  struct DepthUsageStats {
    uint32_t draw_count = 0;
    uint32_t last_draw_sequence = 0;
    bool has_z_writes = false;
    xenos::CompareFunction last_zfunc = xenos::CompareFunction::kNever;
    uint32_t last_color_width = 0;
    uint32_t last_color_height = 0;
    ID3D12Resource* last_color_resource = nullptr;

    // Viewport & Z transformation from registers at draw time
    uint32_t vp_x = 0;
    uint32_t vp_y = 0;
    uint32_t vp_width = 0;
    uint32_t vp_height = 0;
    float depth_near = 0.0f;
    float depth_far = 1.0f;
    bool depth_inverted = false;
    bool z_params_valid = false;
    uint32_t explicit_sample = 0xFFFFFFFF;
  };
  std::unordered_map<ID3D12Resource*, DepthUsageStats> frame_depth_stats_;
  uint32_t current_frame_draw_sequence_ = 0;
};

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_D3D12_D3D12_RENDER_TARGET_CACHE_H_
