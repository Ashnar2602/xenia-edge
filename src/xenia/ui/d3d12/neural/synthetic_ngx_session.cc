/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik and the Xenia Edge Community. All rights reserved.*
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/d3d12/neural/synthetic_ngx_session.h"

#if XE_PLATFORM_WIN32

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/ui/d3d12/d3d12_util.h"

#include <windows.h>
#include <excpt.h>

namespace xe {
namespace ui {
namespace d3d12 {
namespace neural {

namespace {

constexpr int kFeatureSuperSampling = 1;
constexpr unsigned int kPqQuality = 2;
constexpr unsigned int kPqDlaa = 5;

// Deep Fried Chicken (Alexander, 1.4.0+) interop keys
constexpr const char* kDfcKeyContractVersion = "DFC.Feeder.ContractVersion";
constexpr const char* kDfcKeyProviderId = "DFC.Feeder.ProviderId";
constexpr const char* kDfcKeyHostMode = "DFC.Feeder.HostMode";
constexpr const char* kDfcKeyEvaluateCadence = "DFC.Feeder.EvaluateCadence";

constexpr unsigned int kDfcContractVersion = 1u;
constexpr unsigned int kDfcProviderIdDl5f = 0x444C3546u;  // 'DL5F'
constexpr unsigned int kDfcHostModeInProcess = 0u;
constexpr unsigned int kDfcEvaluateCadence = 1u;

// Helper to determine if a surface format can represent HDR / > 1.0 values.
bool FormatCanExceedOne(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      return true;
    default:
      return false;
  }
}

using PFN_Init_Ext = int(WINAPI*)(unsigned long long, const wchar_t*,
                                  ID3D12Device*, int, const void*);
using PFN_Init_ProjectID = int(WINAPI*)(const char*, int, const char*,
                                        const wchar_t*, ID3D12Device*, int,
                                        const void*);
using PFN_CreateFeature = int(WINAPI*)(ID3D12GraphicsCommandList*, int,
                                      NVSDK_NGX_Parameter*,
                                      NVSDK_NGX_Handle**);
using PFN_EvaluateFeature = int(WINAPI*)(ID3D12GraphicsCommandList*,
                                        const NVSDK_NGX_Handle*,
                                        const NVSDK_NGX_Parameter*, void*);
using PFN_ReleaseFeature = int(WINAPI*)(NVSDK_NGX_Handle*);

// SEH guarded wrappers to prevent native faults inside NGX drivers / detoured add-ons
// from terminating the host process.
int GuardedInitExt(PFN_Init_Ext fn, unsigned long long app_id,
                   const wchar_t* path, ID3D12Device* dev, int ver,
                   DWORD* exception_code) {
  if (exception_code) *exception_code = 0;
  __try {
    return fn(app_id, path, dev, ver, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (exception_code) *exception_code = 0xC0000005;  // Exception caught
    return static_cast<int>(0x7FFFFFFF);
  }
}

int GuardedInitProjectID(PFN_Init_ProjectID fn, const char* project_id,
                         int engine_type, const char* engine_version,
                         const wchar_t* path, ID3D12Device* dev, int ver,
                         DWORD* exception_code) {
  if (exception_code) *exception_code = 0;
  __try {
    return fn(project_id, engine_type, engine_version, path, dev, ver, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (exception_code) *exception_code = 0xC0000005;  // Exception caught
    return static_cast<int>(0x7FFFFFFF);
  }
}

int GuardedCreateFeature(PFN_CreateFeature fn, ID3D12GraphicsCommandList* cl,
                         int type, NVSDK_NGX_Parameter* p,
                         NVSDK_NGX_Handle** h, DWORD* exception_code) {
  if (exception_code) *exception_code = 0;
  __try {
    return fn(cl, type, p, h);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (exception_code) *exception_code = 0xC0000005;  // Exception caught
    return static_cast<int>(0x7FFFFFFF);
  }
}

int GuardedEvaluateFeature(PFN_EvaluateFeature fn,
                           ID3D12GraphicsCommandList* cl,
                           const NVSDK_NGX_Handle* h,
                           const NVSDK_NGX_Parameter* p,
                           DWORD* exception_code) {
  if (exception_code) *exception_code = 0;
  __try {
    return fn(cl, h, p, nullptr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (exception_code) *exception_code = 0xC0000005;  // Exception caught
    return static_cast<int>(0x7FFFFFFF);
  }
}

int GuardedReleaseFeature(PFN_ReleaseFeature fn, NVSDK_NGX_Handle* h,
                          DWORD* exception_code) {
  if (exception_code) *exception_code = 0;
  __try {
    return fn(h);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (exception_code) *exception_code = 0xC0000005;  // Exception caught
    return static_cast<int>(0x7FFFFFFF);
  }
}

void PublishDfcInterop(NVSDK_NGX_Parameter* params) {
  if (!params) return;
  params->Set(kDfcKeyContractVersion, kDfcContractVersion);
  params->Set(kDfcKeyProviderId, kDfcProviderIdDl5f);
  params->Set(kDfcKeyHostMode, kDfcHostModeInProcess);
  params->Set(kDfcKeyEvaluateCadence, kDfcEvaluateCadence);
}

const char* DfcStateToString(DfcState state) {
  switch (state) {
    case DfcState::kModuleAbsent:
      return "module absent";
    case DfcState::kAbiUnavailable:
      return "module present but ABI unavailable";
    case DfcState::kDisarmed:
      return "DISARMED";
    case DfcState::kClaiming:
      return "CLAIMING";
    case DfcState::kArmed:
      return "ARMED";
    case DfcState::kConflict:
      return "CONFLICT";
    case DfcState::kFailed:
      return "FAILED";
    default:
      return "UNKNOWN";
  }
}

DfcState ReadDfcExports(unsigned int* out_abi) {
  static HMODULE s_mod = nullptr;
  static const unsigned int* s_abi_p = nullptr;
  static const volatile LONG* s_state_p = nullptr;
  if (!s_abi_p || !s_state_p) {
    s_mod = GetModuleHandleA("deep-fried-chicken.addon64");
    if (!s_mod) {
      return DfcState::kModuleAbsent;
    }
    s_abi_p = reinterpret_cast<const unsigned int*>(
        GetProcAddress(s_mod, "DFC_FeederInteropAbi"));
    s_state_p = reinterpret_cast<const volatile LONG*>(
        GetProcAddress(s_mod, "DFC_Feature1InterceptionState"));
    if (!s_abi_p || !s_state_p) {
      s_abi_p = nullptr;
      s_state_p = nullptr;
      return DfcState::kAbiUnavailable;
    }
  }
  if (out_abi) {
    *out_abi = *s_abi_p;
  }
  return static_cast<DfcState>(*s_state_p);
}

}  // namespace

std::unique_ptr<SyntheticNgxSession> SyntheticNgxSession::Create(
    ID3D12Device* device, ID3D12CommandQueue* direct_queue) {
  if (!device) {
    return nullptr;
  }
  auto session = std::make_unique<SyntheticNgxSession>(device, direct_queue);
  if (session->state() == SessionState::kUnavailable) {
    // NGX is unavailable on this machine/GPU.
    XELOGI("SyntheticNgxSession: NGX runtime is unavailable on this system");
  }
  return session;
}

SyntheticNgxSession::SyntheticNgxSession(ID3D12Device* device,
                                         ID3D12CommandQueue* direct_queue)
    : device_(device), direct_queue_(direct_queue) {
  if (!LoadNgxModule()) {
    state_ = SessionState::kUnavailable;
    return;
  }

  if (!InitializeNgx()) {
    state_ = SessionState::kUnavailable;
    return;
  }

  state_ = SessionState::kUninitialized;
}

void SyntheticNgxSession::AwaitGpuIdle() {
  if (!direct_queue_ || !device_) {
    return;
  }
  if (!sync_fence_) {
    HRESULT hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                      IID_PPV_ARGS(&sync_fence_));
    if (FAILED(hr)) {
      XELOGE("SyntheticNgxSession: Failed to create sync fence: 0x{:08X}", hr);
      return;
    }
  }
  uint64_t fence_val = ++sync_fence_val_;
  HRESULT hr = direct_queue_->Signal(sync_fence_.Get(), fence_val);
  if (SUCCEEDED(hr)) {
    sync_fence_->SetEventOnCompletion(fence_val, nullptr);
  }
}

SyntheticNgxSession::~SyntheticNgxSession() {
  Invalidate();

  if (params_ && pfn_destroy_params_) {
    pfn_destroy_params_(params_);
    params_ = nullptr;
  }

  if (pfn_shutdown1_) {
    pfn_shutdown1_();
  } else if (pfn_shutdown_ && device_) {
    pfn_shutdown_(device_);
  }

  if (owns_ngx_module_ && ngx_module_) {
    FreeLibrary(ngx_module_);
    ngx_module_ = nullptr;
  }

  XELOGI("SyntheticNgxSession: Shutdown and cleaned up");
}

bool SyntheticNgxSession::LoadNgxModule() {
  // First, check if NGX loader is already loaded in the process address space.
  ngx_module_ = GetModuleHandleW(L"_nvngx.dll");
  if (ngx_module_) {
    owns_ngx_module_ = false;
    XELOGI("SyntheticNgxSession: Found existing _nvngx.dll module in process");
  } else {
    // Look up the driver's registered NGXCore path in the registry.
    wchar_t core_path[MAX_PATH] = {};
    DWORD cb_path = sizeof(core_path);
    LSTATUS status =
        RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
                     L"FullPath", RRF_RT_REG_SZ, nullptr, core_path, &cb_path);

    if (status != ERROR_SUCCESS || core_path[0] == 0) {
      XELOGI(
          "SyntheticNgxSession: NVIDIA NGXCore registry key not found "
          "(non-NVIDIA GPU or unsupported driver)");
      return false;
    }

    wchar_t dll_path[MAX_PATH] = {};
    _snwprintf_s(dll_path, _TRUNCATE, L"%ls\\_nvngx.dll", core_path);

    ngx_module_ = LoadLibraryW(dll_path);
    if (!ngx_module_) {
      XELOGW(
          "SyntheticNgxSession: Failed to dynamically load _nvngx.dll from "
          "{}",
          xe::path_to_utf8(dll_path));
      return false;
    }
    owns_ngx_module_ = true;
    XELOGI("SyntheticNgxSession: Loaded _nvngx.dll dynamically from {}",
           xe::path_to_utf8(dll_path));
  }

  // Resolve entry points dynamically.
  pfn_init_ext_ = reinterpret_cast<PFN_Init_Ext>(
      GetProcAddress(ngx_module_, "NVSDK_NGX_D3D12_Init_Ext"));
  pfn_init_project_id_ = reinterpret_cast<PFN_Init_ProjectID>(
      GetProcAddress(ngx_module_, "NVSDK_NGX_D3D12_Init_ProjectID"));
  pfn_shutdown_ = reinterpret_cast<PFN_Shutdown>(
      GetProcAddress(ngx_module_, "NVSDK_NGX_D3D12_Shutdown"));
  pfn_shutdown1_ = reinterpret_cast<PFN_Shutdown1>(
      GetProcAddress(ngx_module_, "NVSDK_NGX_D3D12_Shutdown1"));
  pfn_alloc_params_ = reinterpret_cast<PFN_AllocateParameters>(
      GetProcAddress(ngx_module_, "NVSDK_NGX_D3D12_AllocateParameters"));
  pfn_destroy_params_ = reinterpret_cast<PFN_DestroyParameters>(
      GetProcAddress(ngx_module_, "NVSDK_NGX_D3D12_DestroyParameters"));
  pfn_create_feature_ = reinterpret_cast<PFN_CreateFeature>(
      GetProcAddress(ngx_module_, "NVSDK_NGX_D3D12_CreateFeature"));
  pfn_eval_feature_ = reinterpret_cast<PFN_EvaluateFeature>(
      GetProcAddress(ngx_module_, "NVSDK_NGX_D3D12_EvaluateFeature"));
  pfn_release_feature_ = reinterpret_cast<PFN_ReleaseFeature>(
      GetProcAddress(ngx_module_, "NVSDK_NGX_D3D12_ReleaseFeature"));

  if ((!pfn_init_ext_ && !pfn_init_project_id_) || !pfn_alloc_params_ ||
      !pfn_create_feature_ || !pfn_eval_feature_ || !pfn_release_feature_) {
    XELOGW("SyntheticNgxSession: Required D3D12 NGX exports are missing in DLL");
    if (owns_ngx_module_ && ngx_module_) {
      FreeLibrary(ngx_module_);
      ngx_module_ = nullptr;
    }
    return false;
  }

  return true;
}

bool SyntheticNgxSession::InitializeNgx() {
  wchar_t exe_dir[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
  if (wchar_t* last_slash = wcsrchr(exe_dir, L'\\')) {
    *(last_slash + 1) = L'\0';
  }

  bool initialized = false;
  int successful_version = 0;

  // Negotiate supported SDK version between 0x13 and 0x16.
  for (int ver = 0x13; ver <= 0x16 && !initialized; ++ver) {
    if (pfn_init_ext_) {
      DWORD exception_code = 0;
      int result = GuardedInitExt(pfn_init_ext_, 0x1000000ULL, exe_dir, device_,
                                  ver, &exception_code);
      if (exception_code != 0) {
        XELOGW(
            "SyntheticNgxSession: NVSDK_NGX_D3D12_Init_Ext raised exception "
            "{:#x} (caught safely)",
            exception_code);
      } else if (result == 1 /* NGX_SUCCESS */) {
        initialized = true;
        successful_version = ver;
        break;
      }
    }
    if (!initialized && pfn_init_project_id_) {
      DWORD exception_code = 0;
      int result = GuardedInitProjectID(
          pfn_init_project_id_, "a0f57b54-1daf-4934-90ae-c4035c19df04", 0,
          "1.0", exe_dir, device_, ver, &exception_code);
      if (exception_code != 0) {
        XELOGW(
            "SyntheticNgxSession: NVSDK_NGX_D3D12_Init_ProjectID raised exception "
            "{:#x} (caught safely)",
            exception_code);
      } else if (result == 1 /* NGX_SUCCESS */) {
        initialized = true;
        successful_version = ver;
        break;
      }
    }
  }

  if (!initialized) {
    XELOGW(
        "SyntheticNgxSession: Failed to initialize NGX on D3D12 device across "
        "SDK versions 0x13-0x16");
    return false;
  }

  XELOGI(
      "SyntheticNgxSession: NGX initialized successfully (SDK version {:#x})",
      successful_version);

  // Allocate parameter block.
  int param_result = pfn_alloc_params_(&params_);
  if (param_result != 1 || !params_) {
    XELOGW(
        "SyntheticNgxSession: Failed to allocate NGX parameters (code {:#x})",
        param_result);
    return false;
  }

  // Check if Neural Rendering add-on / hook is present.
  if (!logged_addon_status_) {
    logged_addon_status_ = true;
    HMODULE addon_reno1 = GetModuleHandleA("renodx-dlss5.addon64");
    HMODULE addon_reno2 = GetModuleHandleA("renodx-dlss.addon64");

    const uint8_t* code_bytes =
        reinterpret_cast<const uint8_t*>(pfn_create_feature_);
    bool is_detoured =
        code_bytes && (code_bytes[0] == 0xE9 ||
                       (code_bytes[0] == 0xFF && code_bytes[1] == 0x25));

    unsigned int dfc_abi = 0;
    dfc_state_ = ReadDfcExports(&dfc_abi);
    if (dfc_state_ != DfcState::kModuleAbsent) {
      if (dfc_state_ != DfcState::kAbiUnavailable) {
        XELOGI("SyntheticNgxSession: Deep Fried Chicken detected, ABI {}", dfc_abi);
        dfc_logged_abi_ = true;
      } else {
        XELOGI("SyntheticNgxSession: Deep Fried Chicken detected, ABI unavailable");
      }
      XELOGI("SyntheticNgxSession: DFC state: {}", DfcStateToString(dfc_state_));
    } else if (addon_reno1 || addon_reno2 || is_detoured) {
      XELOGI(
          "SyntheticNgxSession: Neural Rendering add-on / hook detected: RenoDX "
          "(addon module: {}, detoured: {})",
          (addon_reno1 != nullptr || addon_reno2 != nullptr), is_detoured);
    } else {
      XELOGI(
          "SyntheticNgxSession: Synthetic NGX contract initialized. Neural "
          "Rendering interception not independently verifiable.");
    }
  }

  return true;
}

bool SyntheticNgxSession::CreateContractTextures(uint32_t width, uint32_t height,
                                                DXGI_FORMAT format) {
  D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support = {format};
  if (FAILED(device_->CheckFeatureSupport(
          D3D12_FEATURE_FORMAT_SUPPORT, &format_support, sizeof(format_support))) ||
      !(format_support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)) {
    XELOGE("SyntheticNgxSession: Output format {} does not support UAV",
           uint32_t(format));
    return false;
  }

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

  // Output resource for the synthetic 1:1 contract (UAV capable)
  desc.Format = format;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (FAILED(device_->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_COMMON, nullptr,
          IID_PPV_ARGS(&output_resource_)))) {
    XELOGE("SyntheticNgxSession: Failed to create Output contract texture");
    return false;
  }

  output_resource_state_ = D3D12_RESOURCE_STATE_COMMON;
  return true;
}

void SyntheticNgxSession::Invalidate() {
  if (feature_handle_ && pfn_release_feature_) {
    DWORD exception_code = 0;
    GuardedReleaseFeature(pfn_release_feature_, feature_handle_,
                          &exception_code);
    feature_handle_ = nullptr;
    if (exception_code != 0) {
      XELOGW(
          "SyntheticNgxSession: ReleaseFeature raised exception {:#x} (caught "
          "safely)",
          exception_code);
    } else {
      XELOGI("SyntheticNgxSession: Released synthetic NGX feature handle");
    }
  }

  output_resource_.Reset();
  output_resource_state_ = D3D12_RESOURCE_STATE_COMMON;

  width_ = 0;
  height_ = 0;
  format_ = DXGI_FORMAT_UNKNOWN;
  depth_inverted_ = false;

  if (state_ == SessionState::kReady || state_ == SessionState::kSyntheticReady) {
    state_ = SessionState::kUninitialized;
  }
}

bool SyntheticNgxSession::EnsureFeature(ID3D12GraphicsCommandList* command_list,
                                       uint32_t width, uint32_t height,
                                       DXGI_FORMAT format,
                                       bool depth_inverted) {
  if (state_ == SessionState::kUnavailable || state_ == SessionState::kFailed) {
    return false;
  }

  if (IsReady() && width_ == width && height_ == height &&
      format_ == format && depth_inverted_ == depth_inverted) {
    PollDfcState(command_list);
    return true;
  }

  if (IsReady()) {
    XELOGI(
        "SyntheticNgxSession: Output resolution/format/inversion changed ({}x{} "
        "fmt {} inv {} -> {}x{} fmt {} inv {}), recreating synthetic NGX "
        "contract",
        width_, height_, uint32_t(format_), depth_inverted_, width, height,
        uint32_t(format), depth_inverted);
    Invalidate();
  }

  if (!CreateContractTextures(width, height, format)) {
    state_ = SessionState::kFailed;
    Invalidate();
    return false;
  }

  // Populate NGX parameters for the 1:1 synthetic contract.
  params_->Reset();
  params_->Set("Width", width);
  params_->Set("Height", height);
  params_->Set("OutWidth", width);
  params_->Set("OutHeight", height);

  // Flags matching DLSS 5 Feeder reference:
  // MVLowRes (0x02): Motion vectors are at input/render resolution
  // AutoExposure (0x40): NGX internally evaluates exposure
  // DepthInverted (0x08): If inverted Z (near=1, far=0)
  // IsHDR (0x01): false for Xbox 360 SDR UNORM framebuffer
  unsigned int create_flags = 0x02 /* MVLowRes */ | 0x40 /* AutoExposure */;
  if (depth_inverted) {
    create_flags |= 0x08;  // DepthInverted
  }
  params_->Set("DLSS.Feature.Create.Flags", create_flags);

  params_->Set("DLSS.Enable.Output.Subrects", 0u);
  params_->Set("DLSS.Render.Subrect.Dimensions.Width", width);
  params_->Set("DLSS.Render.Subrect.Dimensions.Height", height);
  params_->Set("DLSS.Input.Color.Subrect.Base.X", 0u);
  params_->Set("DLSS.Input.Color.Subrect.Base.Y", 0u);
  params_->Set("DLSS.Input.Depth.Subrect.Base.X", 0u);
  params_->Set("DLSS.Input.Depth.Subrect.Base.Y", 0u);
  params_->Set("DLSS.Input.MV.Subrect.Base.X", 0u);
  params_->Set("DLSS.Input.MV.Subrect.Base.Y", 0u);
  params_->Set("DLSS.Output.Subrect.Base.X", 0u);
  params_->Set("DLSS.Output.Subrect.Base.Y", 0u);

  params_->Set("CreationNodeMask", 1u);
  params_->Set("VisibilityNodeMask", 1u);

  // Output resource is initialized to our owned UAV texture
  params_->Set("Output", output_resource_.Get());

  params_->Set("MV.Scale.X", 1.0f);
  params_->Set("MV.Scale.Y", 1.0f);
  params_->Set("Jitter.Offset.X", 0.0f);
  params_->Set("Jitter.Offset.Y", 0.0f);
  params_->Set("Sharpness", 0.0f);
  params_->Set("DLSS.Pre.Exposure", 1.0f);
  params_->Set("DLSS.Exposure.Scale", 1.0f);
  params_->Set("Reset", 1);

  // Attempt DLAA (5u), fallback to Quality (2u) if DLAA is unsupported.
  unsigned int perf_quality = kPqDlaa;
  params_->Set("PerfQualityValue", perf_quality);

  // Publish Deep Fried Chicken interop keys immediately before feature creation.
  PublishDfcInterop(params_);

  XELOGI(
      "SyntheticNgxSession: Creating synthetic NGX SuperSampling feature: "
      "{}x{} format {} (flags {:#x})",
      width, height, uint32_t(format), create_flags);

  DWORD exception_code = 0;
  int create_result = GuardedCreateFeature(
      pfn_create_feature_, command_list, kFeatureSuperSampling, params_,
      &feature_handle_, &exception_code);

  if (exception_code != 0) {
    XELOGW(
        "SyntheticNgxSession: CreateFeature raised exception {:#x} (caught "
        "safely)",
        exception_code);
    state_ = SessionState::kFailed;
    Invalidate();
    return false;
  }

  if (create_result != 1 && perf_quality == kPqDlaa) {
    XELOGI(
        "SyntheticNgxSession: CreateFeature refused with DLAA (5), attempting "
        "Quality (2) fallback");
    perf_quality = kPqQuality;
    params_->Set("PerfQualityValue", perf_quality);
    create_result = GuardedCreateFeature(
        pfn_create_feature_, command_list, kFeatureSuperSampling, params_,
        &feature_handle_, &exception_code);
    if (exception_code != 0) {
      XELOGW(
          "SyntheticNgxSession: CreateFeature (fallback) raised exception {:#x}",
          exception_code);
      state_ = SessionState::kFailed;
      Invalidate();
      return false;
    }
  }

  if (create_result != 1 || !feature_handle_) {
    XELOGW(
        "SyntheticNgxSession: Native NGX CreateFeature returned code {:#x}, "
        "enabling synthetic contract fallback",
        create_result);
    width_ = width;
    height_ = height;
    format_ = format;
    depth_inverted_ = depth_inverted;
    state_ = SessionState::kSyntheticReady;
    return true;
  }

  width_ = width;
  height_ = height;
  format_ = format;
  depth_inverted_ = depth_inverted;
  state_ = SessionState::kReady;

  XELOGI(
      "SyntheticNgxSession: Synthetic NGX feature created successfully: {}x{} "
      "handle {:#x}",
      width_, height_, reinterpret_cast<uintptr_t>(feature_handle_));

  // Determine if DFC was armed at feature creation time
  unsigned int dfc_abi = 0;
  DfcState dfc_curr = ReadDfcExports(&dfc_abi);
  if (dfc_curr != DfcState::kModuleAbsent &&
      dfc_curr != DfcState::kAbiUnavailable) {
    dfc_state_ = dfc_curr;
    if (dfc_state_ != DfcState::kArmed) {
      dfc_created_unarmed_ = true;
      XELOGI(
          "SyntheticNgxSession: Feature created while DFC state was {}, will "
          "rebuild when ARMED",
          DfcStateToString(dfc_state_));
    } else {
      dfc_created_unarmed_ = false;
      XELOGI("SyntheticNgxSession: Feature created with DFC ARMED");
    }
  }

  return true;
}

bool SyntheticNgxSession::IsInterceptionConfirmed() const {
  if (dfc_state_ == DfcState::kArmed) {
    return true;
  }
  HMODULE addon_reno1 = GetModuleHandleA("renodx-dlss5.addon64");
  HMODULE addon_reno2 = GetModuleHandleA("renodx-dlss.addon64");
  const uint8_t* code_bytes =
      reinterpret_cast<const uint8_t*>(pfn_create_feature_);
  bool is_detoured =
      code_bytes && (code_bytes[0] == 0xE9 ||
                     (code_bytes[0] == 0xFF && code_bytes[1] == 0x25));
  return (addon_reno1 != nullptr || addon_reno2 != nullptr || is_detoured);
}

void SyntheticNgxSession::PollDfcState(ID3D12GraphicsCommandList* command_list) {
  unsigned int abi = 0;
  DfcState current_state = ReadDfcExports(&abi);
  if (current_state == DfcState::kModuleAbsent) {
    return;
  }
  if (!dfc_logged_abi_ && current_state != DfcState::kAbiUnavailable) {
    dfc_logged_abi_ = true;
    XELOGI("SyntheticNgxSession: Deep Fried Chicken detected, ABI {}", abi);
  }
  if (current_state != dfc_state_) {
    XELOGI("SyntheticNgxSession: DFC state: {}", DfcStateToString(current_state));
    dfc_state_ = current_state;
  }
  if (dfc_created_unarmed_ && dfc_state_ == DfcState::kArmed && command_list) {
    XELOGI(
        "SyntheticNgxSession: DFC became ARMED: rebuilding synthetic NGX "
        "feature");
    uint32_t w = width_;
    uint32_t h = height_;
    DXGI_FORMAT fmt = format_;
    bool inv = depth_inverted_;
    AwaitGpuIdle();
    Invalidate();
    dfc_created_unarmed_ = false;
    EnsureFeature(command_list, w, h, fmt, inv);
    XELOGI(
        "SyntheticNgxSession: Synthetic NGX feature recreated for DFC "
        "interception");
  }
}

bool SyntheticNgxSession::Evaluate(ID3D12GraphicsCommandList* command_list,
                                  const NeuralFrameContract& contract) {
  if (!IsReady() || !command_list || !contract.valid) {
    return false;
  }
  if (!contract.color || !contract.depth || !contract.motion_vectors ||
      !output_resource_) {
    return false;
  }

  evaluate_count_++;
  if (contract.reset_history) {
    reset_count_++;
  }

  uint64_t t_start = xe::Clock::QueryHostTickCount();
  uint64_t freq = xe::Clock::QueryHostTickFrequency();

  if (state_ == SessionState::kSyntheticReady) {
    // Synthetic pass-through: Copy contract.color to output_resource_
    D3D12_RESOURCE_BARRIER pre_barriers[2] = {};
    pre_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre_barriers[0].Transition.pResource = contract.color;
    pre_barriers[0].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    pre_barriers[0].Transition.StateBefore =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    pre_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    pre_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre_barriers[1].Transition.pResource = output_resource_.Get();
    pre_barriers[1].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    pre_barriers[1].Transition.StateBefore = output_resource_state_;
    pre_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

    command_list->ResourceBarrier(2, pre_barriers);

    command_list->CopyResource(output_resource_.Get(), contract.color);

    D3D12_RESOURCE_BARRIER post_barriers[2] = {};
    post_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    post_barriers[0].Transition.pResource = contract.color;
    post_barriers[0].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    post_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    post_barriers[0].Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    post_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    post_barriers[1].Transition.pResource = output_resource_.Get();
    post_barriers[1].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    post_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    post_barriers[1].Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    command_list->ResourceBarrier(2, post_barriers);
    output_resource_state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    uint64_t t_end = xe::Clock::QueryHostTickCount();
    last_eval_time_ms_ = (t_end - t_start) * 1000.0 / freq;
    total_eval_time_ms_ += last_eval_time_ms_;
    evaluate_success_count_++;

    if (evaluate_count_ == 1 || (evaluate_count_ % 120 == 1)) {
      XELOGI(
          "SyntheticNgxSession: Synthetic pass-through evaluate #{} completed "
          "({:.3f}ms)",
          evaluate_count_, last_eval_time_ms_);
    }
    return true;
  }

  // Native NGX evaluate (kReady with feature_handle_)
  if (!feature_handle_) {
    return false;
  }

  // Pre-evaluate transition barriers:
  // Color from PIXEL_SHADER_RESOURCE to NON_PIXEL_SHADER_RESOURCE
  // Output to UNORDERED_ACCESS
  D3D12_RESOURCE_BARRIER pre_barriers[2] = {};
  pre_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  pre_barriers[0].Transition.pResource = contract.color;
  pre_barriers[0].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  pre_barriers[0].Transition.StateBefore =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  pre_barriers[0].Transition.StateAfter =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

  pre_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  pre_barriers[1].Transition.pResource = output_resource_.Get();
  pre_barriers[1].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  pre_barriers[1].Transition.StateBefore = output_resource_state_;
  pre_barriers[1].Transition.StateAfter =
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

  command_list->ResourceBarrier(2, pre_barriers);
  output_resource_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

  // Publish Deep Fried Chicken interop keys immediately before evaluate.
  PublishDfcInterop(params_);

  params_->Set("Color", contract.color);
  params_->Set("Output", output_resource_.Get());
  params_->Set("Depth", contract.depth);
  params_->Set("MotionVectors", contract.motion_vectors);

  params_->Set("DLSS.Render.Subrect.Dimensions.Width", contract.width);
  params_->Set("DLSS.Render.Subrect.Dimensions.Height", contract.height);
  params_->Set("DLSS.Input.Color.Subrect.Base.X", 0u);
  params_->Set("DLSS.Input.Color.Subrect.Base.Y", 0u);
  params_->Set("DLSS.Input.Depth.Subrect.Base.X", 0u);
  params_->Set("DLSS.Input.Depth.Subrect.Base.Y", 0u);
  params_->Set("DLSS.Input.MV.Subrect.Base.X", 0u);
  params_->Set("DLSS.Input.MV.Subrect.Base.Y", 0u);
  params_->Set("DLSS.Output.Subrect.Base.X", 0u);
  params_->Set("DLSS.Output.Subrect.Base.Y", 0u);

  params_->Set("MV.Scale.X", 1.0f);
  params_->Set("MV.Scale.Y", 1.0f);
  params_->Set("Jitter.Offset.X", 0.0f);
  params_->Set("Jitter.Offset.Y", 0.0f);
  params_->Set("Sharpness", 0.0f);
  params_->Set("DLSS.Pre.Exposure", 1.0f);
  params_->Set("DLSS.Exposure.Scale", 1.0f);
  params_->Set("Reset", contract.reset_history ? 1 : 0);

  DWORD exception_code = 0;
  int eval_result = GuardedEvaluateFeature(
      pfn_eval_feature_, command_list, feature_handle_, params_,
      &exception_code);

  if (exception_code != 0) {
    evaluate_exception_count_++;
    XELOGW(
        "SyntheticNgxSession: EvaluateFeature raised exception {:#x} (caught "
        "safely)",
        exception_code);
    eval_result = 0;
  }

  if (eval_result != 1) {
    evaluate_failure_count_++;
    if (evaluate_failure_count_ <= 5 || (evaluate_failure_count_ % 120 == 0)) {
      XELOGW(
          "SyntheticNgxSession: EvaluateFeature failed with code {:#x} (fail "
          "#{})",
          eval_result, evaluate_failure_count_);
    }
  } else {
    evaluate_success_count_++;
  }

  // Restore barriers:
  // Color from NON_PIXEL_SHADER_RESOURCE back to PIXEL_SHADER_RESOURCE
  // Output from UNORDERED_ACCESS to PIXEL_SHADER_RESOURCE (for presenter sampling)
  D3D12_RESOURCE_BARRIER post_barriers[2] = {};
  post_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  post_barriers[0].Transition.pResource = contract.color;
  post_barriers[0].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  post_barriers[0].Transition.StateBefore =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  post_barriers[0].Transition.StateAfter =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  post_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  post_barriers[1].Transition.pResource = output_resource_.Get();
  post_barriers[1].Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  post_barriers[1].Transition.StateBefore =
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  post_barriers[1].Transition.StateAfter =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  command_list->ResourceBarrier(2, post_barriers);
  output_resource_state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  uint64_t t_end = xe::Clock::QueryHostTickCount();
  last_eval_time_ms_ = (t_end - t_start) * 1000.0 / freq;
  total_eval_time_ms_ += last_eval_time_ms_;

  if (evaluate_count_ == 1 || (evaluate_count_ % 120 == 1)) {
    XELOGI(
        "SyntheticNgxSession: Native EvaluateFeature #{} completed: "
        "result={:#x} ({:.3f}ms)",
        evaluate_count_, eval_result, last_eval_time_ms_);
  }

  return eval_result == 1;
}

bool SyntheticNgxSession::EvaluateDiagnostic(
    ID3D12GraphicsCommandList* command_list) {
  if (state_ != SessionState::kReady || !feature_handle_ || !command_list ||
      !output_resource_) {
    return false;
  }

  // Diagnostic evaluate using output texture as placeholder
  NeuralFrameContract contract;
  contract.color = output_resource_.Get();
  contract.depth = output_resource_.Get();
  contract.motion_vectors = output_resource_.Get();
  contract.output = output_resource_.Get();
  contract.width = width_;
  contract.height = height_;
  contract.depth_inverted = depth_inverted_;
  contract.depth_trusted = true;
  contract.motion_valid = true;
  contract.reset_history = true;
  contract.valid = true;

  return Evaluate(command_list, contract);
}

}  // namespace neural
}  // namespace d3d12
}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WIN32
