/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/zar_metadata.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

#include "third_party/zarchive/include/zarchive/zarchivereader.h"

namespace xe {
namespace vfs {

namespace {

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); i++) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// Recursively search for default.xex in the archive.
ZArchiveNodeHandle FindDefaultXex(ZArchiveReader* reader,
                                  const std::string& dir_path, int max_depth,
                                  size_t& budget) {
  if (max_depth <= 0) {
    return ZARCHIVE_INVALID_NODE;
  }

  ZArchiveNodeHandle dir_handle = reader->LookUp(dir_path, false, true);
  if (dir_handle == ZARCHIVE_INVALID_NODE || !reader->IsDirectory(dir_handle)) {
    return ZARCHIVE_INVALID_NODE;
  }

  uint32_t entry_count = reader->GetDirEntryCount(dir_handle);
  for (uint32_t i = 0; i < entry_count && budget; i++) {
    --budget;
    ZArchiveReader::DirEntry entry;
    if (!reader->GetDirEntry(dir_handle, i, entry)) {
      continue;
    }

    if (entry.isFile && EqualsIgnoreCase(entry.name, "default.xex")) {
      std::string file_path = dir_path;
      if (!file_path.empty() && file_path.back() != '/') {
        file_path += '/';
      }
      file_path += std::string(entry.name);
      return reader->LookUp(file_path, true, false);
    }

    if (entry.isDirectory) {
      std::string subdir_path = dir_path;
      if (!subdir_path.empty() && subdir_path.back() != '/') {
        subdir_path += '/';
      }
      subdir_path += std::string(entry.name);

      ZArchiveNodeHandle result =
          FindDefaultXex(reader, subdir_path, max_depth - 1, budget);
      if (result != ZARCHIVE_INVALID_NODE) {
        return result;
      }
    }
  }

  return ZARCHIVE_INVALID_NODE;
}

}  // namespace

std::vector<uint8_t> ReadZarExecutable(const std::filesystem::path& path,
                                       size_t limit, bool header_only) {
  std::unique_ptr<ZArchiveReader> reader(ZArchiveReader::OpenFromFile(path));
  if (!reader) {
    return {};
  }
  size_t budget = 65536;
  auto handle = FindDefaultXex(reader.get(), "", 2, budget);
  if (handle == ZARCHIVE_INVALID_NODE || !reader->IsFile(handle)) {
    return {};
  }
  auto size = reader->GetFileSize(handle);
  if (size < 24) {
    return {};
  }
  if (header_only) {
    uint8_t header[24];
    if (reader->ReadFromFile(handle, 0, sizeof(header), header) !=
        sizeof(header)) {
      return {};
    }
    uint32_t header_size = (uint32_t(header[8]) << 24) |
                           (uint32_t(header[9]) << 16) |
                           (uint32_t(header[10]) << 8) | header[11];
    if (header_size < 24 || header_size > size ||
        header_size > 16 * 1024 * 1024) {
      return {};
    }
    size = header_size;
  }
  if (size > limit) {
    return {};
  }
  std::vector<uint8_t> data(size);
  if (reader->ReadFromFile(handle, 0, size, data.data()) != size) {
    return {};
  }
  return data;
}

std::optional<XexMetadata> ExtractZarMetadata(
    const std::filesystem::path& path) {
  std::unique_ptr<ZArchiveReader> reader(ZArchiveReader::OpenFromFile(path));
  if (!reader) {
    return std::nullopt;
  }

  size_t budget = 65536;
  ZArchiveNodeHandle handle = FindDefaultXex(reader.get(), "/", 2, budget);
  if (handle == ZARCHIVE_INVALID_NODE) {
    handle = FindDefaultXex(reader.get(), "", 2, budget);
  }
  if (handle == ZARCHIVE_INVALID_NODE || !reader->IsFile(handle)) {
    return std::nullopt;
  }

  uint64_t file_size = reader->GetFileSize(handle);
  if (file_size < 24) {
    return std::nullopt;
  }

  // Read base header to get header_size (big-endian at offset 0x08).
  uint8_t base_header[24];
  if (reader->ReadFromFile(handle, 0, sizeof(base_header), base_header) !=
      sizeof(base_header)) {
    return std::nullopt;
  }

  uint32_t magic = (base_header[0] << 24) | (base_header[1] << 16) |
                   (base_header[2] << 8) | base_header[3];
  if (magic != 0x58455831 && magic != 0x58455832) {
    return std::nullopt;
  }

  uint32_t header_size = (base_header[8] << 24) | (base_header[9] << 16) |
                         (base_header[10] << 8) | base_header[11];
  if (header_size < 24 || header_size > file_size) {
    return std::nullopt;
  }

  std::vector<uint8_t> header_data(header_size);
  if (reader->ReadFromFile(handle, 0, header_size, header_data.data()) !=
      header_size) {
    return std::nullopt;
  }

  return ExtractXexMetadata(header_data.data(), header_data.size());
}

}  // namespace vfs
}  // namespace xe
