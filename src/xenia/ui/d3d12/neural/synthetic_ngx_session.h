/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik and the Xenia Edge Community. All rights reserved.*
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_D3D12_NEURAL_SYNTHETIC_NGX_SESSION_H_
#define XENIA_UI_D3D12_NEURAL_SYNTHETIC_NGX_SESSION_H_

#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

struct ID3D11Resource;

namespace xe {
namespace ui {
namespace d3d12 {
namespace neural {

// Represents the lifecycle state of the synthetic NGX session.
enum class SessionState {
  kUnavailable,    // NGX runtime / NVIDIA hardware not available on system
  kUninitialized,  // Runtime loaded, awaiting feature creation
  kReady,          // Feature and contract resources created and valid
  kFailed,         // Initialization or feature creation failed irreversibly
};

// Represents the interception state of Deep Fried Chicken (Alexander, 1.4.0+ ABI 1)
enum class DfcState : int {
  kModuleAbsent = -2,             // Module not loaded in process
  kAbiUnavailable = -1,          // Module loaded but ABI 1 exports not found
  kDisarmed = 0,                 // DISARMED
  kClaiming = 1,                 // CLAIMING
  kArmed = 2,                    // ARMED
  kConflict = 3,                 // CONFLICT
  kFailed = 4,                   // FAILED
};

// Stable binary ABI representation of NVSDK_NGX_Parameter.
struct NVSDK_NGX_Parameter {
  virtual void Set(const char* InName, unsigned long long InValue) = 0;
  virtual void Set(const char* InName, float InValue) = 0;
  virtual void Set(const char* InName, double InValue) = 0;
  virtual void Set(const char* InName, unsigned int InValue) = 0;
  virtual void Set(const char* InName, int InValue) = 0;
  virtual void Set(const char* InName, ID3D11Resource* InValue) = 0;
  virtual void Set(const char* InName, ID3D12Resource* InValue) = 0;
  virtual void Set(const char* InName, void* InValue) = 0;

  virtual int Get(const char* InName, unsigned long long* OutValue) const = 0;
  virtual int Get(const char* InName, float* OutValue) const = 0;
  virtual int Get(const char* InName, double* OutValue) const = 0;
  virtual int Get(const char* InName, unsigned int* OutValue) const = 0;
  virtual int Get(const char* InName, int* OutValue) const = 0;
  virtual int Get(const char* InName, void** OutValue) const = 0;  // ID3D11Resource**
  virtual int Get(const char* InName, ID3D12Resource** OutValue) const = 0;
  virtual int Get(const char* InName, void** OutValue, bool) const = 0;  // void**

  virtual void Reset() = 0;
};

struct NVSDK_NGX_Handle {
  unsigned int Id;
};

class SyntheticNgxSession {
 public:
  static std::unique_ptr<SyntheticNgxSession> Create(
      ID3D12Device* device, ID3D12CommandQueue* direct_queue = nullptr);

  explicit SyntheticNgxSession(ID3D12Device* device,
                               ID3D12CommandQueue* direct_queue = nullptr);
  ~SyntheticNgxSession();

  SessionState state() const { return state_; }
  bool IsReady() const { return state_ == SessionState::kReady; }
  bool IsUnavailable() const { return state_ == SessionState::kUnavailable; }

  DfcState dfc_state() const { return dfc_state_; }
  bool IsInterceptionConfirmed() const;

  // Ensures synthetic NGX feature and backing textures are initialized for (width, height, format).
  // Re-creates the feature if dimensions or format changed, or if DFC becomes ARMED.
  bool EnsureFeature(ID3D12GraphicsCommandList* command_list, uint32_t width,
                     uint32_t height, DXGI_FORMAT format);

  // Polls Deep Fried Chicken exports and handles warm-up feature recreation when it arms.
  void PollDfcState(ID3D12GraphicsCommandList* command_list);

  // Invalidates the current feature handle and textures upon resolution change or shutdown.
  void Invalidate();

  // Controlled diagnostic evaluate test (for verification only; not used in normal presentation).
  bool EvaluateDiagnostic(ID3D12GraphicsCommandList* command_list);

  ID3D12Resource* color_resource() const { return color_resource_.Get(); }
  ID3D12Resource* output_resource() const { return output_resource_.Get(); }
  ID3D12Resource* depth_resource() const { return depth_resource_.Get(); }
  ID3D12Resource* mv_resource() const {
    return motion_vectors_resource_.Get();
  }

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  DXGI_FORMAT format() const { return format_; }

 private:
  bool LoadNgxModule();
  bool InitializeNgx();
  bool CreateContractTextures(uint32_t width, uint32_t height,
                              DXGI_FORMAT format);

  ID3D12Device* device_ = nullptr;
  SessionState state_ = SessionState::kUninitialized;

  HMODULE ngx_module_ = nullptr;
  bool owns_ngx_module_ = false;

  // NGX function pointers resolved dynamically
  using PFN_Init_Ext = int(WINAPI*)(unsigned long long, const wchar_t*,
                                    ID3D12Device*, int, const void*);
  using PFN_Init_ProjectID = int(WINAPI*)(const char*, int, const char*,
                                          const wchar_t*, ID3D12Device*, int,
                                          const void*);
  using PFN_Shutdown = int(WINAPI*)(ID3D12Device*);
  using PFN_Shutdown1 = int(WINAPI*)(void);
  using PFN_AllocateParameters = int(WINAPI*)(NVSDK_NGX_Parameter**);
  using PFN_DestroyParameters = int(WINAPI*)(NVSDK_NGX_Parameter*);
  using PFN_CreateFeature = int(WINAPI*)(ID3D12GraphicsCommandList*, int,
                                        NVSDK_NGX_Parameter*,
                                        NVSDK_NGX_Handle**);
  using PFN_EvaluateFeature = int(WINAPI*)(ID3D12GraphicsCommandList*,
                                          const NVSDK_NGX_Handle*,
                                          const NVSDK_NGX_Parameter*, void*);
  using PFN_ReleaseFeature = int(WINAPI*)(NVSDK_NGX_Handle*);

  PFN_Init_Ext pfn_init_ext_ = nullptr;
  PFN_Init_ProjectID pfn_init_project_id_ = nullptr;
  PFN_Shutdown pfn_shutdown_ = nullptr;
  PFN_Shutdown1 pfn_shutdown1_ = nullptr;
  PFN_AllocateParameters pfn_alloc_params_ = nullptr;
  PFN_DestroyParameters pfn_destroy_params_ = nullptr;
  PFN_CreateFeature pfn_create_feature_ = nullptr;
  PFN_EvaluateFeature pfn_eval_feature_ = nullptr;
  PFN_ReleaseFeature pfn_release_feature_ = nullptr;

  NVSDK_NGX_Parameter* params_ = nullptr;
  NVSDK_NGX_Handle* feature_handle_ = nullptr;

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;

  // Backing textures for the synthetic 1:1 contract
  Microsoft::WRL::ComPtr<ID3D12Resource> color_resource_;
  Microsoft::WRL::ComPtr<ID3D12Resource> output_resource_;
  Microsoft::WRL::ComPtr<ID3D12Resource> depth_resource_;
  Microsoft::WRL::ComPtr<ID3D12Resource> motion_vectors_resource_;

  void AwaitGpuIdle();

  ID3D12CommandQueue* direct_queue_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D12Fence> sync_fence_;
  uint64_t sync_fence_val_ = 0;

  bool logged_addon_status_ = false;
  bool dfc_logged_abi_ = false;
  bool dfc_created_unarmed_ = false;
  DfcState dfc_state_ = DfcState::kModuleAbsent;
};

}  // namespace neural
}  // namespace d3d12
}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WIN32

#endif  // XENIA_UI_D3D12_NEURAL_SYNTHETIC_NGX_SESSION_H_
