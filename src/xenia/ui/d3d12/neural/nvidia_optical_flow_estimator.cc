/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik and the Xenia Edge Community. All rights reserved.*
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/d3d12/neural/nvidia_optical_flow_estimator.h"

#if XE_PLATFORM_WIN32

#include <cmath>

#include "xenia/base/logging.h"

namespace shaders {
#include "xenia/ui/shaders/bytecode/d3d12_dxil/neural_color_convert_cs.h"
#include "xenia/ui/shaders/bytecode/d3d12_dxil/neural_vector_expand_cs.h"
}  // namespace shaders

namespace xe {
namespace ui {
namespace d3d12 {
namespace neural {

namespace {

// Descriptor heap layout:
// Slot 0: SRV for current_color
// Slot 1: UAV for input_buffers_[0] (B8G8R8A8_UNORM)
// Slot 2: UAV for input_buffers_[1] (B8G8R8A8_UNORM)
// Slot 3: SRV for raw_flow_buffer_ (R16G16_SINT)
// Slot 4: UAV for full_flow_buffer_ (R16G16_FLOAT)
// Slot 5: SRV for full_flow_buffer_ (R16G16_FLOAT)
constexpr UINT kDescriptorSlotCurrentColorSRV = 0;
constexpr UINT kDescriptorSlotInputBuffer0UAV = 1;
constexpr UINT kDescriptorSlotInputBuffer1UAV = 2;
constexpr UINT kDescriptorSlotRawFlowSRV = 3;
constexpr UINT kDescriptorSlotFullFlowUAV = 4;
constexpr UINT kDescriptorSlotFullFlowSRV = 5;
constexpr UINT kTotalDescriptors = 8;

struct ColorConvertConstants {
  uint32_t width;
  uint32_t height;
};

struct VectorExpandConstants {
  uint32_t full_width;
  uint32_t full_height;
  uint32_t grid_width;
  uint32_t grid_height;
  uint32_t grid_size;
  float scale_factor;
};

}  // namespace

std::unique_ptr<NvidiaOpticalFlowEstimator> NvidiaOpticalFlowEstimator::Create(
    ID3D12Device* device, ID3D12CommandQueue* direct_queue) {
  if (!device || !direct_queue) {
    return nullptr;
  }
  auto estimator =
      std::make_unique<NvidiaOpticalFlowEstimator>(device, direct_queue);
  if (!estimator->LoadNvOfApi()) {
    XELOGW("NvidiaOpticalFlowEstimator: Failed to load NVIDIA Optical Flow API");
    return nullptr;
  }
  return estimator;
}

NvidiaOpticalFlowEstimator::NvidiaOpticalFlowEstimator(
    ID3D12Device* device, ID3D12CommandQueue* direct_queue)
    : device_(device), direct_queue_(direct_queue) {}

NvidiaOpticalFlowEstimator::~NvidiaOpticalFlowEstimator() {
  Shutdown();
}

bool NvidiaOpticalFlowEstimator::LoadNvOfApi() {
  h_nvof_dll_ = LoadLibraryA("nvofapi64.dll");
  if (!h_nvof_dll_) {
    XELOGI("NvidiaOpticalFlowEstimator: nvofapi64.dll not found on system");
    return false;
  }

  auto pfn_get_max_ver = reinterpret_cast<PFN_NvOFGetMaxSupportedApiVersion>(
      GetProcAddress(h_nvof_dll_, "NvOFGetMaxSupportedApiVersion"));
  auto pfn_create_instance = reinterpret_cast<PFN_NvOFAPICreateInstanceD3D12>(
      GetProcAddress(h_nvof_dll_, "NvOFAPICreateInstanceD3D12"));

  if (!pfn_get_max_ver || !pfn_create_instance) {
    XELOGW("NvidiaOpticalFlowEstimator: Failed to locate entry points in nvofapi64.dll");
    FreeLibrary(h_nvof_dll_);
    h_nvof_dll_ = nullptr;
    return false;
  }

  uint32_t max_ver = 0;
  NV_OF_STATUS status = pfn_get_max_ver(&max_ver);
  if (status != NV_OF_SUCCESS) {
    XELOGW("NvidiaOpticalFlowEstimator: NvOFGetMaxSupportedApiVersion failed: {}",
           static_cast<int>(status));
    FreeLibrary(h_nvof_dll_);
    h_nvof_dll_ = nullptr;
    return false;
  }

  XELOGI(
      "NvidiaOpticalFlowEstimator: NvOF API detected, maxVersion=0x{:X} "
      "(Major: {}, Minor: {})",
      max_ver, (max_ver >> 4), (max_ver & 0xF));

  status = pfn_create_instance(max_ver, &of_api_);
  if (status != NV_OF_SUCCESS) {
    XELOGW("NvidiaOpticalFlowEstimator: NvOFAPICreateInstanceD3D12 failed: {}",
           static_cast<int>(status));
    FreeLibrary(h_nvof_dll_);
    h_nvof_dll_ = nullptr;
    return false;
  }

  // Create test Optical Flow instance on device to verify hardware support
  status = of_api_.nvCreateOpticalFlowD3D12(device_, &of_handle_);
  if (status != NV_OF_SUCCESS || !of_handle_) {
    XELOGW(
        "NvidiaOpticalFlowEstimator: nvCreateOpticalFlowD3D12 failed: status={}",
        static_cast<int>(status));
    FreeLibrary(h_nvof_dll_);
    h_nvof_dll_ = nullptr;
    return false;
  }

  // Resolve internal object and unregister function
  internal_d3d12_obj_ =
      *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(of_handle_) + 0x30);
  if (internal_d3d12_obj_) {
    void** internal_vtable = *reinterpret_cast<void***>(internal_d3d12_obj_);
    pfn_internal_unregister_ =
        reinterpret_cast<PFN_NvOFInternalUnregister>(internal_vtable[10]);
  }

  XELOGI(
      "NvidiaOpticalFlowEstimator: Successfully created NVOFA D3D12 instance "
      "handle: {:p}",
      static_cast<void*>(of_handle_));

  if (!CreatePipelines()) {
    XELOGE("NvidiaOpticalFlowEstimator: Failed to create compute pipelines");
    of_api_.nvOFDestroy(of_handle_);
    of_handle_ = nullptr;
    FreeLibrary(h_nvof_dll_);
    h_nvof_dll_ = nullptr;
    return false;
  }

  return true;
}

bool NvidiaOpticalFlowEstimator::CreatePipelines() {
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
  heap_desc.NumDescriptors = kTotalDescriptors;
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  HRESULT hr = device_->CreateDescriptorHeap(
      &heap_desc, IID_PPV_ARGS(&descriptor_heap_));
  if (FAILED(hr)) {
    XELOGE("NvidiaOpticalFlowEstimator: Failed to create descriptor heap: 0x{:08X}",
           hr);
    return false;
  }
  descriptor_size_ = device_->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  // 1. Color Convert Root Signature:
  // Root Param 0: 32-bit constants (2 DWORDs: width, height) at b0
  // Root Param 1: Descriptor Table (1 SRV at t0)
  // Root Param 2: Descriptor Table (1 UAV at u0)
  D3D12_DESCRIPTOR_RANGE srv_range = {};
  srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  srv_range.NumDescriptors = 1;
  srv_range.BaseShaderRegister = 0;

  D3D12_DESCRIPTOR_RANGE uav_range = {};
  uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  uav_range.NumDescriptors = 1;
  uav_range.BaseShaderRegister = 0;

  D3D12_ROOT_PARAMETER root_params_cc[3] = {};
  root_params_cc[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  root_params_cc[0].Constants.ShaderRegister = 0;
  root_params_cc[0].Constants.Num32BitValues = 2;
  root_params_cc[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  root_params_cc[1].ParameterType =
      D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_params_cc[1].DescriptorTable.NumDescriptorRanges = 1;
  root_params_cc[1].DescriptorTable.pDescriptorRanges = &srv_range;
  root_params_cc[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  root_params_cc[2].ParameterType =
      D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_params_cc[2].DescriptorTable.NumDescriptorRanges = 1;
  root_params_cc[2].DescriptorTable.pDescriptorRanges = &uav_range;
  root_params_cc[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC rs_desc_cc = {};
  rs_desc_cc.NumParameters = 3;
  rs_desc_cc.pParameters = root_params_cc;
  rs_desc_cc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  HMODULE d3d12_mod = GetModuleHandleA("d3d12.dll");
  if (!d3d12_mod) {
    d3d12_mod = LoadLibraryA("d3d12.dll");
  }
  auto pfn_serialize_rs = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
      GetProcAddress(d3d12_mod, "D3D12SerializeRootSignature"));
  if (!pfn_serialize_rs) {
    XELOGE("NvidiaOpticalFlowEstimator: Failed to locate D3D12SerializeRootSignature");
    return false;
  }

  Microsoft::WRL::ComPtr<ID3DBlob> blob, error_blob;
  hr = pfn_serialize_rs(&rs_desc_cc, D3D_ROOT_SIGNATURE_VERSION_1, &blob,
                        &error_blob);
  if (FAILED(hr)) {
    XELOGE("NvidiaOpticalFlowEstimator: D3D12SerializeRootSignature (color_convert) failed");
    return false;
  }
  hr = device_->CreateRootSignature(0, blob->GetBufferPointer(),
                                    blob->GetBufferSize(),
                                    IID_PPV_ARGS(&color_convert_root_sig_));
  if (FAILED(hr)) {
    XELOGE("NvidiaOpticalFlowEstimator: CreateRootSignature (color_convert) failed");
    return false;
  }

  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc_cc = {};
  pso_desc_cc.pRootSignature = color_convert_root_sig_.Get();
  pso_desc_cc.CS.pShaderBytecode = shaders::neural_color_convert_cs;
  pso_desc_cc.CS.BytecodeLength = sizeof(shaders::neural_color_convert_cs);
  hr = device_->CreateComputePipelineState(
      &pso_desc_cc, IID_PPV_ARGS(&color_convert_pso_));
  if (FAILED(hr)) {
    XELOGE("NvidiaOpticalFlowEstimator: CreateComputePipelineState (color_convert) failed");
    return false;
  }

  // 2. Vector Expand Root Signature:
  // Root Param 0: 32-bit constants (6 DWORDs) at b0
  // Root Param 1: Descriptor Table (1 SRV at t0)
  // Root Param 2: Descriptor Table (1 UAV at u0)
  D3D12_ROOT_PARAMETER root_params_ve[3] = {};
  root_params_ve[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  root_params_ve[0].Constants.ShaderRegister = 0;
  root_params_ve[0].Constants.Num32BitValues = 6;
  root_params_ve[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  root_params_ve[1].ParameterType =
      D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_params_ve[1].DescriptorTable.NumDescriptorRanges = 1;
  root_params_ve[1].DescriptorTable.pDescriptorRanges = &srv_range;
  root_params_ve[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  root_params_ve[2].ParameterType =
      D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_params_ve[2].DescriptorTable.NumDescriptorRanges = 1;
  root_params_ve[2].DescriptorTable.pDescriptorRanges = &uav_range;
  root_params_ve[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC rs_desc_ve = {};
  rs_desc_ve.NumParameters = 3;
  rs_desc_ve.pParameters = root_params_ve;
  rs_desc_ve.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  blob.Reset();
  error_blob.Reset();
  hr = pfn_serialize_rs(&rs_desc_ve, D3D_ROOT_SIGNATURE_VERSION_1, &blob,
                        &error_blob);
  if (FAILED(hr)) {
    XELOGE("NvidiaOpticalFlowEstimator: D3D12SerializeRootSignature (vector_expand) failed");
    return false;
  }
  hr = device_->CreateRootSignature(0, blob->GetBufferPointer(),
                                    blob->GetBufferSize(),
                                    IID_PPV_ARGS(&vector_expand_root_sig_));
  if (FAILED(hr)) {
    XELOGE("NvidiaOpticalFlowEstimator: CreateRootSignature (vector_expand) failed");
    return false;
  }

  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc_ve = {};
  pso_desc_ve.pRootSignature = vector_expand_root_sig_.Get();
  pso_desc_ve.CS.pShaderBytecode = shaders::neural_vector_expand_cs;
  pso_desc_ve.CS.BytecodeLength = sizeof(shaders::neural_vector_expand_cs);
  hr = device_->CreateComputePipelineState(
      &pso_desc_ve, IID_PPV_ARGS(&vector_expand_pso_));
  if (FAILED(hr)) {
    XELOGE("NvidiaOpticalFlowEstimator: CreateComputePipelineState (vector_expand) failed");
    return false;
  }

  // 3. Command Allocators and Command List
  for (size_t i = 0; i < kNumAllocators; ++i) {
    hr = device_->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&command_allocators_[i]));
    if (FAILED(hr)) {
      XELOGE("NvidiaOpticalFlowEstimator: CreateCommandAllocator failed");
      return false;
    }
  }

  hr = device_->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocators_[0].Get(),
      nullptr, IID_PPV_ARGS(&estimator_command_list_));
  if (FAILED(hr)) {
    XELOGE("NvidiaOpticalFlowEstimator: CreateCommandList failed");
    return false;
  }
  estimator_command_list_->Close();

  hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&of_fence_));
  if (FAILED(hr)) {
    XELOGE("NvidiaOpticalFlowEstimator: CreateFence failed");
    return false;
  }

  return true;
}

bool NvidiaOpticalFlowEstimator::Initialize(ID3D12Device* device,
                                           ID3D12CommandQueue* command_queue,
                                           uint32_t width, uint32_t height) {
  if (device != device_) {
    XELOGE("NvidiaOpticalFlowEstimator: Device mismatch during initialize");
    return false;
  }
  direct_queue_ = command_queue;

  if (is_initialized_ && width_ == width && height_ == height) {
    return true;
  }

  if (is_initialized_) {
    ReleaseResources();
  }

  if (!AllocateResources(width, height)) {
    XELOGE("NvidiaOpticalFlowEstimator: Failed to allocate resources for {}x{}",
           width, height);
    return false;
  }

  is_initialized_ = true;
  has_previous_frame_ = false;
  XELOGI(
      "NvidiaOpticalFlowEstimator: Initialized for {}x{} (NVOFA Grid: {}, "
      "Flow Grid: {}x{})",
      width_, height_, static_cast<uint32_t>(grid_size_), grid_width_,
      grid_height_);
  return true;
}

bool NvidiaOpticalFlowEstimator::AllocateResources(uint32_t width,
                                                  uint32_t height) {
  width_ = width;
  height_ = height;
  grid_size_ = NV_OF_OUTPUT_VECTOR_GRID_SIZE_2;
  grid_width_ = (width_ + static_cast<uint32_t>(grid_size_) - 1) /
                static_cast<uint32_t>(grid_size_);
  grid_height_ = (height_ + static_cast<uint32_t>(grid_size_) - 1) /
                 static_cast<uint32_t>(grid_size_);

  NV_OF_INIT_PARAMS init_params = {};
  init_params.width = width_;
  init_params.height = height_;
  init_params.outGridSize = grid_size_;
  init_params.mode = NV_OF_MODE_OPTICALFLOW;
  init_params.perfLevel = NV_OF_PERF_LEVEL_FAST;
  init_params.enableExternalHints = NV_OF_FALSE;
  init_params.enableOutputCost = NV_OF_FALSE;

  NV_OF_STATUS status = of_api_.nvOFInit(of_handle_, &init_params);
  if (status != NV_OF_SUCCESS) {
    XELOGE("NvidiaOpticalFlowEstimator: nvOFInit failed: status={}",
           static_cast<int>(status));
    return false;
  }

  D3D12_HEAP_PROPERTIES heap_props = {};
  heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC color_desc = {};
  color_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  color_desc.Width = width_;
  color_desc.Height = height_;
  color_desc.DepthOrArraySize = 1;
  color_desc.MipLevels = 1;
  color_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  color_desc.SampleDesc.Count = 1;
  color_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  color_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  for (size_t i = 0; i < 2; ++i) {
    HRESULT hr = device_->CreateCommittedResource(
        &heap_props, D3D12_HEAP_FLAG_NONE, &color_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&input_buffers_[i]));
    if (FAILED(hr)) {
      XELOGE("NvidiaOpticalFlowEstimator: Failed to create input ping-pong buffer {}",
             i);
      return false;
    }

    NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 reg = {};
    reg.resource = input_buffers_[i].Get();
    reg.hOFGpuBuffer = &h_input_buffers_[i];
    status = of_api_.nvOFRegisterResourceD3D12(of_handle_, &reg,
                                              &h_input_buffers_[i]);
    if (status != NV_OF_SUCCESS || !h_input_buffers_[i]) {
      XELOGE("NvidiaOpticalFlowEstimator: Failed to register input buffer {}", i);
      return false;
    }

    // Create UAV in descriptor heap (slots 1 and 2)
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_h =
        descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
    cpu_h.ptr += (kDescriptorSlotInputBuffer0UAV + i) * descriptor_size_;
    device_->CreateUnorderedAccessView(input_buffers_[i].Get(), nullptr,
                                       &uav_desc, cpu_h);
  }

  // Create raw flow buffer (DXGI_FORMAT_R16G16_SINT at grid resolution)
  D3D12_RESOURCE_DESC raw_flow_desc = color_desc;
  raw_flow_desc.Width = grid_width_;
  raw_flow_desc.Height = grid_height_;
  raw_flow_desc.Format = DXGI_FORMAT_R16G16_SINT;
  raw_flow_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  HRESULT hr = device_->CreateCommittedResource(
      &heap_props, D3D12_HEAP_FLAG_NONE, &raw_flow_desc,
      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&raw_flow_buffer_));
  if (FAILED(hr)) {
    XELOGE("NvidiaOpticalFlowEstimator: Failed to create raw flow buffer");
    return false;
  }

  NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 reg_flow = {};
  reg_flow.resource = raw_flow_buffer_.Get();
  reg_flow.hOFGpuBuffer = &h_raw_flow_buffer_;
  status = of_api_.nvOFRegisterResourceD3D12(of_handle_, &reg_flow,
                                            &h_raw_flow_buffer_);
  if (status != NV_OF_SUCCESS || !h_raw_flow_buffer_) {
    XELOGE("NvidiaOpticalFlowEstimator: Failed to register raw flow buffer");
    return false;
  }

  // Create SRV for raw_flow_buffer_ (slot 3)
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = DXGI_FORMAT_R16G16_SINT;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_h =
        descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
    cpu_h.ptr += kDescriptorSlotRawFlowSRV * descriptor_size_;
    device_->CreateShaderResourceView(raw_flow_buffer_.Get(), &srv_desc, cpu_h);
  }

  // Create final full-resolution motion vector texture (DXGI_FORMAT_R16G16_FLOAT)
  D3D12_RESOURCE_DESC full_flow_desc = color_desc;
  full_flow_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
  full_flow_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  hr = device_->CreateCommittedResource(
      &heap_props, D3D12_HEAP_FLAG_NONE, &full_flow_desc,
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
      IID_PPV_ARGS(&full_flow_buffer_));
  if (FAILED(hr)) {
    XELOGE("NvidiaOpticalFlowEstimator: Failed to create full flow buffer");
    return false;
  }

  // Create UAV for full_flow_buffer_ (slot 4)
  {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_h =
        descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
    cpu_h.ptr += kDescriptorSlotFullFlowUAV * descriptor_size_;
    device_->CreateUnorderedAccessView(full_flow_buffer_.Get(), nullptr,
                                       &uav_desc, cpu_h);
  }

  // Create SRV for full_flow_buffer_ (slot 5)
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_h =
        descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
    cpu_h.ptr += kDescriptorSlotFullFlowSRV * descriptor_size_;
    device_->CreateShaderResourceView(full_flow_buffer_.Get(), &srv_desc, cpu_h);
  }

  return true;
}

void NvidiaOpticalFlowEstimator::ReleaseResources() {
  if (of_handle_) {
    if (pfn_internal_unregister_ && internal_d3d12_obj_) {
      if (h_input_buffers_[0]) {
        pfn_internal_unregister_(internal_d3d12_obj_, &h_input_buffers_[0]);
        h_input_buffers_[0] = nullptr;
      }
      if (h_input_buffers_[1]) {
        pfn_internal_unregister_(internal_d3d12_obj_, &h_input_buffers_[1]);
        h_input_buffers_[1] = nullptr;
      }
      if (h_raw_flow_buffer_) {
        pfn_internal_unregister_(internal_d3d12_obj_, &h_raw_flow_buffer_);
        h_raw_flow_buffer_ = nullptr;
      }
    }
  }

  input_buffers_[0].Reset();
  input_buffers_[1].Reset();
  raw_flow_buffer_.Reset();
  full_flow_buffer_.Reset();
  is_initialized_ = false;
  has_previous_frame_ = false;
}

void NvidiaOpticalFlowEstimator::Shutdown() {
  ReleaseResources();

  if (of_handle_ && of_api_.nvOFDestroy) {
    of_api_.nvOFDestroy(of_handle_);
    of_handle_ = nullptr;
  }

  descriptor_heap_.Reset();
  color_convert_pso_.Reset();
  color_convert_root_sig_.Reset();
  vector_expand_pso_.Reset();
  vector_expand_root_sig_.Reset();
  estimator_command_list_.Reset();
  for (size_t i = 0; i < kNumAllocators; ++i) {
    command_allocators_[i].Reset();
  }
  of_fence_.Reset();

  if (h_nvof_dll_) {
    FreeLibrary(h_nvof_dll_);
    h_nvof_dll_ = nullptr;
  }
  XELOGI("NvidiaOpticalFlowEstimator: Shutdown complete");
}

bool NvidiaOpticalFlowEstimator::EstimateMotion(
    ID3D12GraphicsCommandList* command_list, ID3D12Resource* current_color,
    uint64_t frame_index) {
  if (!is_initialized_ || !current_color || !direct_queue_) {
    return false;
  }

  const uint32_t curr_slot = static_cast<uint32_t>(frame_index % 2);
  const uint32_t prev_slot = 1 - curr_slot;
  const uint32_t alloc_idx = static_cast<uint32_t>(frame_index % kNumAllocators);

  // Recycle command allocator for this frame slot
  command_allocators_[alloc_idx]->Reset();
  estimator_command_list_->Reset(command_allocators_[alloc_idx].Get(), nullptr);

  // Dynamically update SRV for current_color at descriptor slot 0
  {
    D3D12_RESOURCE_DESC cur_desc = current_color->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = cur_desc.Format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_h =
        descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
    cpu_h.ptr += kDescriptorSlotCurrentColorSRV * descriptor_size_;
    device_->CreateShaderResourceView(current_color, &srv_desc, cpu_h);
  }

  // --- PASS 1: Color Conversion (R10G10B10A2 -> B8G8R8A8_UNORM) ---
  D3D12_RESOURCE_BARRIER barrier_pre_convert = {};
  barrier_pre_convert.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier_pre_convert.Transition.pResource = input_buffers_[curr_slot].Get();
  barrier_pre_convert.Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier_pre_convert.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  barrier_pre_convert.Transition.StateAfter =
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  estimator_command_list_->ResourceBarrier(1, &barrier_pre_convert);

  ID3D12DescriptorHeap* descriptor_heaps[] = {descriptor_heap_.Get()};
  estimator_command_list_->SetDescriptorHeaps(1, descriptor_heaps);

  estimator_command_list_->SetComputeRootSignature(
      color_convert_root_sig_.Get());
  estimator_command_list_->SetPipelineState(color_convert_pso_.Get());

  ColorConvertConstants cc_consts = {width_, height_};
  estimator_command_list_->SetComputeRoot32BitConstants(
      0, 2, &cc_consts, 0);

  D3D12_GPU_DESCRIPTOR_HANDLE gpu_h_start =
      descriptor_heap_->GetGPUDescriptorHandleForHeapStart();

  D3D12_GPU_DESCRIPTOR_HANDLE gpu_srv = gpu_h_start;
  gpu_srv.ptr += kDescriptorSlotCurrentColorSRV * descriptor_size_;
  estimator_command_list_->SetComputeRootDescriptorTable(1, gpu_srv);

  D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = gpu_h_start;
  gpu_uav.ptr += (kDescriptorSlotInputBuffer0UAV + curr_slot) * descriptor_size_;
  estimator_command_list_->SetComputeRootDescriptorTable(2, gpu_uav);

  estimator_command_list_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);

  D3D12_RESOURCE_BARRIER barrier_post_convert = {};
  barrier_post_convert.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier_post_convert.Transition.pResource = input_buffers_[curr_slot].Get();
  barrier_post_convert.Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier_post_convert.Transition.StateBefore =
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  barrier_post_convert.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
  estimator_command_list_->ResourceBarrier(1, &barrier_post_convert);

  estimator_command_list_->Close();

  ID3D12CommandList* cmd_lists[] = {estimator_command_list_.Get()};
  direct_queue_->ExecuteCommandLists(1, cmd_lists);

  const uint64_t in_fence_val = ++fence_value_;
  direct_queue_->Signal(of_fence_.Get(), in_fence_val);

  // If first frame, no previous reference frame is available yet
  if (!has_previous_frame_) {
    has_previous_frame_ = true;
    last_ready_fence_value_ = in_fence_val;
    allocator_fence_values_[alloc_idx] = in_fence_val;
    return true;
  }

  // --- PASS 2: NVIDIA Hardware Optical Flow Execution ---
  NV_OF_FENCE_POINT in_fence_point = {of_fence_.Get(), in_fence_val};
  const uint64_t of_out_fence_val = ++fence_value_;
  NV_OF_FENCE_POINT out_fence_point = {of_fence_.Get(), of_out_fence_val};

  NV_OF_EXECUTE_INPUT_PARAMS_D3D12 exec_in = {};
  exec_in.inputFrame = h_input_buffers_[curr_slot];        // current frame T
  exec_in.referenceFrame = h_input_buffers_[prev_slot];    // previous frame T-1
  exec_in.disableTemporalHints = NV_OF_TRUE;
  exec_in.numFencePoints = 1;
  exec_in.fencePoint = &in_fence_point;

  NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 exec_out = {};
  exec_out.outputBuffer = h_raw_flow_buffer_;
  exec_out.numFencePoints = 1;
  exec_out.fencePoint = &out_fence_point;

  NV_OF_STATUS status =
      of_api_.nvOFExecuteD3D12(of_handle_, &exec_in, &exec_out);
  if (status != NV_OF_SUCCESS) {
    XELOGW("NvidiaOpticalFlowEstimator: nvOFExecuteD3D12 failed: status={}",
           static_cast<int>(status));
    return false;
  }

  // Direct queue waits on NVOFA output fence without CPU stall
  direct_queue_->Wait(of_fence_.Get(), of_out_fence_val);

  // --- PASS 3: Vector Expansion (R16G16_SINT S10.5 -> R16G16_FLOAT full resolution) ---
  estimator_command_list_->Reset(command_allocators_[alloc_idx].Get(), nullptr);

  D3D12_RESOURCE_BARRIER barriers_pre_expand[2] = {};
  barriers_pre_expand[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers_pre_expand[0].Transition.pResource = raw_flow_buffer_.Get();
  barriers_pre_expand[0].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barriers_pre_expand[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  barriers_pre_expand[0].Transition.StateAfter =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

  barriers_pre_expand[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers_pre_expand[1].Transition.pResource = full_flow_buffer_.Get();
  barriers_pre_expand[1].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barriers_pre_expand[1].Transition.StateBefore =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  barriers_pre_expand[1].Transition.StateAfter =
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

  estimator_command_list_->ResourceBarrier(2, barriers_pre_expand);

  estimator_command_list_->SetDescriptorHeaps(1, descriptor_heaps);
  estimator_command_list_->SetComputeRootSignature(
      vector_expand_root_sig_.Get());
  estimator_command_list_->SetPipelineState(vector_expand_pso_.Get());

  VectorExpandConstants ve_consts = {
      width_,
      height_,
      grid_width_,
      grid_height_,
      static_cast<uint32_t>(grid_size_),
      1.0f / 32.0f,  // NVOFA S10.5 fixed point conversion to display pixels
  };
  estimator_command_list_->SetComputeRoot32BitConstants(
      0, 6, &ve_consts, 0);

  D3D12_GPU_DESCRIPTOR_HANDLE gpu_raw_srv = gpu_h_start;
  gpu_raw_srv.ptr += kDescriptorSlotRawFlowSRV * descriptor_size_;
  estimator_command_list_->SetComputeRootDescriptorTable(1, gpu_raw_srv);

  D3D12_GPU_DESCRIPTOR_HANDLE gpu_full_uav = gpu_h_start;
  gpu_full_uav.ptr += kDescriptorSlotFullFlowUAV * descriptor_size_;
  estimator_command_list_->SetComputeRootDescriptorTable(2, gpu_full_uav);

  estimator_command_list_->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);

  D3D12_RESOURCE_BARRIER barriers_post_expand[2] = {};
  barriers_post_expand[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers_post_expand[0].Transition.pResource = raw_flow_buffer_.Get();
  barriers_post_expand[0].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barriers_post_expand[0].Transition.StateBefore =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  barriers_post_expand[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;

  barriers_post_expand[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers_post_expand[1].Transition.pResource = full_flow_buffer_.Get();
  barriers_post_expand[1].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barriers_post_expand[1].Transition.StateBefore =
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  barriers_post_expand[1].Transition.StateAfter =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

  estimator_command_list_->ResourceBarrier(2, barriers_post_expand);

  estimator_command_list_->Close();
  direct_queue_->ExecuteCommandLists(1, cmd_lists);

  const uint64_t final_fence_val = ++fence_value_;
  direct_queue_->Signal(of_fence_.Get(), final_fence_val);

  allocator_fence_values_[alloc_idx] = final_fence_val;
  last_ready_fence_value_ = final_fence_val;

  return true;
}

}  // namespace neural
}  // namespace d3d12
}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WIN32
