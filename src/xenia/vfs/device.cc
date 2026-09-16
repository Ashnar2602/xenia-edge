/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/device.h"

#include <algorithm>
#include <cmath>

#include "xenia/base/clock.h"
#include "xenia/base/logging.h"

namespace xe {
namespace vfs {

namespace {
// Command overhead and rotational latency, paid before any data moves. An
// absolute duration, since scaling it down reintroduces the races it prevents.
constexpr double kRequestFloorMs = 6.0;
// A 360 hard disk, which is what installed content is read from.
constexpr double kBytesPerMs = 35000.0;
// Caps the backlog so a mis-modeled title runs slow rather than compounding.
// Must stay above the cost of one large request, around a second.
constexpr uint64_t kMaxQueuedAheadMs = 4000;
}  // namespace

void DriveTiming::Configure() { enabled_ = true; }

uint64_t DriveTiming::Reserve(size_t length) {
  if (!enabled_) {
    return 0;
  }
  const double ms = kRequestFloorMs + double(length) / kBytesPerMs;

  std::lock_guard<std::mutex> lock(lock_);
  const uint64_t now = Clock::QueryHostUptimeMillis();
  // Past the cap this moves the queue backwards, deliberately dropping what an
  // in-flight request still had reserved.
  const uint64_t start =
      std::min(std::max(now, free_at_ms_), now + kMaxQueuedAheadMs);
  free_at_ms_ = start + uint64_t(std::ceil(ms));
  return free_at_ms_;
}

Device::Device(const std::string_view mount_path) : mount_path_(mount_path) {}
Device::~Device() = default;

}  // namespace vfs
}  // namespace xe
