/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_ZAR_METADATA_H_
#define XENIA_VFS_ZAR_METADATA_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "xenia/vfs/xex_metadata.h"

namespace xe {
namespace vfs {

// Reads only the executable into bounded transient memory, never stages the
// archive.
std::vector<uint8_t> ReadZarExecutable(const std::filesystem::path& path,
                                       size_t limit, bool header_only = false);

std::optional<XexMetadata> ExtractZarMetadata(
    const std::filesystem::path& path);

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_ZAR_METADATA_H_
