/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik and the Xenia Edge Community. All rights reserved.*
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_D3D12_NEURAL_NVIDIA_OPTICAL_FLOW_ABI_H_
#define XENIA_UI_D3D12_NEURAL_NVIDIA_OPTICAL_FLOW_ABI_H_

#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>

namespace xe {
namespace ui {
namespace d3d12 {
namespace neural {

// NVIDIA Optical Flow SDK minimum official API version supporting DirectX 12
constexpr uint32_t kNvOfClientApiVersionD3D12 = 0x30;

// Opaque handles
typedef struct NvOFHandle_st* NvOFHandle;
typedef struct NvOFGPUBufferHandle_st* NvOFGPUBufferHandle;

enum NV_OF_STATUS {
  NV_OF_SUCCESS = 0,
  NV_OF_ERR_OF_NOT_AVAILABLE = 1,
  NV_OF_ERR_UNSUPPORTED_DEVICE = 2,
  NV_OF_ERR_DEVICE_DOES_NOT_EXIST = 3,
  NV_OF_ERR_INVALID_PTR = 4,
  NV_OF_ERR_INVALID_PARAM = 5,
  NV_OF_ERR_INVALID_CALL = 6,
  NV_OF_ERR_INVALID_VERSION = 7,
  NV_OF_ERR_OUT_OF_MEMORY = 8,
  NV_OF_ERR_NOT_INITIALIZED = 9,
  NV_OF_ERR_UNSUPPORTED_FEATURE = 10,
  NV_OF_ERR_GENERIC = 11,
};

enum NV_OF_BOOL {
  NV_OF_FALSE = 0,
  NV_OF_TRUE = 1,
};

enum NV_OF_CAPS {
  NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES = 0,
  NV_OF_CAPS_SUPPORTED_HINT_GRID_SIZES = 1,
  NV_OF_CAPS_SUPPORT_HINT_WITH_OF_MODE = 2,
  NV_OF_CAPS_SUPPORT_HINT_WITH_ST_MODE = 3,
  NV_OF_CAPS_WIDTH_MIN = 4,
  NV_OF_CAPS_HEIGHT_MIN = 5,
  NV_OF_CAPS_WIDTH_MAX = 6,
  NV_OF_CAPS_HEIGHT_MAX = 7,
  NV_OF_CAPS_SUPPORT_ROI = 8,
  NV_OF_CAPS_SUPPORT_ROI_MAX_NUM = 9,
  NV_OF_CAPS_SUPPORT_MAX
};

enum NV_OF_MODE {
  NV_OF_MODE_UNDEFINED = 0,
  NV_OF_MODE_OPTICALFLOW = 1,
  NV_OF_MODE_STEREODISPARITY = 2,
  NV_OF_MODE_MAX
};

enum NV_OF_BUFFER_USAGE {
  NV_OF_BUFFER_USAGE_UNDEFINED = 0,
  NV_OF_BUFFER_USAGE_INPUT = 1,
  NV_OF_BUFFER_USAGE_OUTPUT = 2,
  NV_OF_BUFFER_USAGE_HINT = 3,
  NV_OF_BUFFER_USAGE_COST = 4,
  NV_OF_BUFFER_USAGE_MAX
};

enum NV_OF_OUTPUT_VECTOR_GRID_SIZE {
  NV_OF_OUTPUT_VECTOR_GRID_SIZE_UNDEFINED = 0,
  NV_OF_OUTPUT_VECTOR_GRID_SIZE_1 = 1,
  NV_OF_OUTPUT_VECTOR_GRID_SIZE_2 = 2,
  NV_OF_OUTPUT_VECTOR_GRID_SIZE_4 = 4,
  NV_OF_OUTPUT_VECTOR_GRID_SIZE_MAX
};

enum NV_OF_PERF_LEVEL {
  NV_OF_PERF_LEVEL_UNDEFINED = 0,
  NV_OF_PERF_LEVEL_SLOW = 5,
  NV_OF_PERF_LEVEL_MEDIUM = 10,
  NV_OF_PERF_LEVEL_FAST = 20,
  NV_OF_PERF_LEVEL_MAX
};

// D3D12 Fence synchronization primitive
struct NV_OF_FENCE_POINT {
  ID3D12Fence* fence;
  uint64_t value;
};

// Official layout for NvOFRegisterResourceD3D12
struct NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 {
  ID3D12Resource* resource;
  NV_OF_FENCE_POINT inputFencePoint;
  NvOFGPUBufferHandle* hOFGpuBuffer;
  NV_OF_FENCE_POINT outputFencePoint;
};

// Official layout for NvOFInit
struct NV_OF_INIT_PARAMS {
  uint32_t width;
  uint32_t height;
  NV_OF_OUTPUT_VECTOR_GRID_SIZE outGridSize;
  uint32_t hintGridSize;
  NV_OF_MODE mode;
  NV_OF_PERF_LEVEL perfLevel;
  NV_OF_BOOL enableExternalHints;
  NV_OF_BOOL enableOutputCost;
  void* hPrivData;
  uint32_t disparityRange;
  NV_OF_BOOL enableRoi;
  uint32_t reserved[4];
};

// Official layout for NvOFExecuteD3D12 Input
struct NV_OF_EXECUTE_INPUT_PARAMS_D3D12 {
  NvOFGPUBufferHandle inputFrame;
  NvOFGPUBufferHandle referenceFrame;
  NvOFGPUBufferHandle externalHints;
  NV_OF_BOOL disableTemporalHints;
  uint32_t pad1;
  void* hPrivData;
  uint32_t pad2;
  uint32_t numRois;
  void* roiData;
  uint32_t pad3;
  uint32_t numFencePoints;
  NV_OF_FENCE_POINT* fencePoint;
};

// Official layout for NvOFExecuteD3D12 Output
struct NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 {
  NvOFGPUBufferHandle outputBuffer;
  NvOFGPUBufferHandle bwdOutputBuffer;
  void* hPrivData;
  NvOFGPUBufferHandle outputCostBuffer;
  NvOFGPUBufferHandle bwdOutputCostBuffer;
  NvOFGPUBufferHandle globalFlowBuffer;
  uint32_t pad;
  uint32_t numFencePoints;
  NV_OF_FENCE_POINT* fencePoint;
};

// Official NVOFA D3D12 API Function Table
struct NV_OF_D3D12_API_FUNCTION_LIST {
  NV_OF_STATUS(__stdcall* nvCreateOpticalFlowD3D12)(ID3D12Device* device,
                                                   NvOFHandle* phOF);
  NV_OF_STATUS(__stdcall* nvOFInit)(NvOFHandle hOF,
                                    const NV_OF_INIT_PARAMS* initParams);
  NV_OF_STATUS(__stdcall* nvOFGetSurfaceFormatCountD3D12)(
      NvOFHandle hOF, NV_OF_BUFFER_USAGE bufferUsage, NV_OF_MODE mode,
      uint32_t* pCount);
  NV_OF_STATUS(__stdcall* nvOFGetSurfaceFormatD3D12)(
      NvOFHandle hOF, NV_OF_BUFFER_USAGE bufferUsage, NV_OF_MODE mode,
      DXGI_FORMAT* pFormats);
  NV_OF_STATUS(__stdcall* nvOFRegisterResourceD3D12)(
      NvOFHandle hOF,
      const NV_OF_REGISTER_RESOURCE_PARAMS_D3D12* registerParams);
  NV_OF_STATUS(__stdcall* nvOFUnregisterResourceD3D12)(
      NvOFGPUBufferHandle* phBuffer);
  NV_OF_STATUS(__stdcall* nvOFExecuteD3D12)(
      NvOFHandle hOF,
      const NV_OF_EXECUTE_INPUT_PARAMS_D3D12* executeInParams,
      NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12* executeOutParams);
  NV_OF_STATUS(__stdcall* nvOFDestroy)(NvOFHandle hOF);
  NV_OF_STATUS(__stdcall* nvOFGetLastError)(NvOFHandle hOF, char lastError[],
                                            uint32_t* size);
  NV_OF_STATUS(__stdcall* nvOFGetCaps)(NvOFHandle hOF, NV_OF_CAPS capsParam,
                                       uint32_t* capsVal, uint32_t* size);
};

typedef NV_OF_STATUS(__stdcall* PFN_NvOFGetMaxSupportedApiVersion)(
    uint32_t* version);
typedef NV_OF_STATUS(__stdcall* PFN_NvOFAPICreateInstanceD3D12)(
    uint32_t apiVer, NV_OF_D3D12_API_FUNCTION_LIST* functionList);

}  // namespace neural
}  // namespace d3d12
}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WIN32

#endif  // XENIA_UI_D3D12_NEURAL_NVIDIA_OPTICAL_FLOW_ABI_H_
