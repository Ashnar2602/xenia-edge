/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/portal/hardware_portal.h"
#include "xenia/base/logging.h"

namespace xe {
namespace hid {

HardwarePortal::HardwarePortal() : Portal() {
#if XE_PLATFORM_ANDROID
  libusb_init_option option{};
  option.option = LIBUSB_OPTION_NO_DEVICE_DISCOVERY;
  libusb_init_context(&context_, &option, 1);
#else
  libusb_init(&context_);
  OpenDevice();
#endif
}

HardwarePortal::~HardwarePortal() {
  if (handle_) {
    CloseDevice();
  }

  libusb_exit(context_);
}

bool HardwarePortal::IsConnected() {
#if XE_PLATFORM_ANDROID
  return android_connected_.load();
#else
  return handle_ != nullptr;
#endif
}
#if XE_PLATFORM_ANDROID
bool HardwarePortal::SetAndroidDevice(int fd) {
  std::lock_guard<xe_mutex> guard(lock_);
  CloseDevice();
  if (fd < 0) {
    return true;
  }
  if (!context_ ||
      libusb_wrap_sys_device(context_, intptr_t(fd), &handle_) < 0) {
    return false;
  }
  libusb_set_auto_detach_kernel_driver(handle_, 1);
  if (libusb_claim_interface(handle_, 0) < 0) {
    CloseDevice();
    return false;
  }
  android_connected_.store(true);
  return true;
}
#endif

void HardwarePortal::OpenDevice() {
#if XE_PLATFORM_ANDROID
  return;  // Only Java may discover/open a device, after the user's USB grant.
#else
  if (!context_ || handle_) {
    return;
  }

  // Allow only one portal device at the time.
  for (const auto& entry : kPortalVendorProductIdList) {
    handle_ =
        libusb_open_device_with_vid_pid(context_, entry.first, entry.second);
    if (handle_) {
      libusb_claim_interface(handle_, 0);
      break;
    }
  }
#endif
}

void HardwarePortal::CloseDevice() {
#if XE_PLATFORM_ANDROID
  android_connected_.store(false);
#endif
  if (!handle_) {
    return;
  }

  libusb_release_interface(handle_, 0);
  libusb_close(handle_);
  handle_ = nullptr;
}

X_STATUS HardwarePortal::ReadInternal(std::span<uint8_t> data,
                                      int32_t& read_count) {
  const int result = libusb_interrupt_transfer(
      handle_, read_endpoint, data.data(), static_cast<int>(data.size()),
      &read_count, timeout);

  switch (result) {
    case LIBUSB_ERROR_NO_DEVICE:
      return X_ERROR_DEVICE_NOT_CONNECTED;
      break;
    case LIBUSB_ERROR_TIMEOUT:
      return X_ERROR_SUCCESS;
    default:
      break;
  }

  if (result < 0) {
    XELOGW("Portal[Read] returned error: {:08X}", result);
    return X_ERROR_FUNCTION_FAILED;
  }
  return X_ERROR_SUCCESS;
}

X_STATUS HardwarePortal::WriteInternal(std::span<uint8_t> data) {
  const int result = libusb_interrupt_transfer(
      handle_, write_endpoint, data.data(), static_cast<int>(data.size()),
      nullptr, timeout);

  if (result < 0) {
    XELOGW("Portal[Write] returned error: {:08X}", result);
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  return X_ERROR_SUCCESS;
};

void HardwarePortal::OnDeviceArrival() { OpenDevice(); };

void HardwarePortal::OnDeviceRemoval() { CloseDevice(); };

}  // namespace hid
}  // namespace xe
