#include "xenia/app/game_icon.h"

#include <cstring>
#include <span>
#include <stdexcept>

#include "third_party/crypto/TinySHA1.hpp"
#include "third_party/zarchive/include/zarchive/zarchivecommon.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/cpu/lzx.h"
#include "xenia/cpu/xex_crypto.h"
#include "xenia/kernel/util/xex2_info.h"
#include "xenia/vfs/gdfx_util.h"
#include "xenia/vfs/stfs_metadata.h"
#include "xenia/vfs/zar_metadata.h"

namespace xe::app {
namespace {
constexpr size_t kMaxImage = 128 * 1024 * 1024;
constexpr size_t kMaxIcon = 4 * 1024 * 1024;
using Bytes = std::span<const uint8_t>;

// Every offset, length and allocation from the file passes through these
// checks.
Bytes Slice(Bytes b, size_t offset, size_t size) {
  if (offset > b.size() || size > b.size() - offset) {
    throw std::runtime_error("Invalid icon resource bounds");
  }
  return b.subspan(offset, size);
}
uint32_t Be32(Bytes b, size_t offset) {
  auto p = Slice(b, offset, 4);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | p[3];
}
uint16_t Be16(Bytes b, size_t offset) {
  auto p = Slice(b, offset, 2);
  return (uint16_t(p[0]) << 8) | p[1];
}
std::vector<uint8_t> Png(Bytes data) {
  static constexpr uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (data.size() < 24 || data.size() > kMaxIcon ||
      std::memcmp(data.data(), signature, sizeof(signature))) {
    return {};
  }
  return {data.begin(), data.end()};
}
std::vector<uint8_t> SpaIcon(Bytes spa) {
  if (spa.size() < 24 || Be32(spa, 0) != 0x58444246) {
    return {};
  }
  size_t count = Be32(spa, 8), used = Be32(spa, 12);
  if (used > count) {
    return {};
  }
  auto entries = Slice(spa, 24, count * 18);
  size_t data_offset = 24 + entries.size() + size_t(Be32(spa, 16)) * 8;
  if (data_offset > spa.size()) {
    return {};
  }
  auto data = spa.subspan(data_offset);
  for (size_t i = 0; i < used; ++i) {
    auto e = entries.subspan(i * 18, 18);
    if (Be16(e, 0) == 2 && Be32(e, 2) == 0 && Be32(e, 6) == 0x8000) {
      return Png(Slice(data, Be32(e, 10), Be32(e, 14)));
    }
  }
  return {};
}

std::vector<uint8_t> DecodeImage(Bytes payload, Bytes format,
                                 size_t image_size) {
  switch (Be16(format, 6)) {
    case 0:
      if (payload.size() > kMaxImage) {
        return {};
      }
      return {payload.begin(), payload.end()};
    case 1: {
      if ((format.size() - 8) % 8) {
        return {};
      }
      std::vector<uint8_t> image(image_size);
      size_t source = 0, dest = 0;
      for (size_t i = 8; i < format.size(); i += 8) {
        size_t data = Be32(format, i), zero = Be32(format, i + 4);
        auto block = Slice(payload, source, data);
        if (dest > image.size() || data > image.size() - dest ||
            zero > image.size() - dest - data) {
          return {};
        }
        std::memcpy(image.data() + dest, block.data(), data);
        source += data;
        dest += data + zero;
      }
      return image;
    }
    case 2: {
      uint32_t window = Be32(format, 8);
      if (window < 32768 || window > (1u << 21) || (window & (window - 1))) {
        return {};
      }
      auto descriptor = Slice(format, 12, 24);
      std::vector<uint8_t> compressed;
      size_t offset = 0;
      while (size_t size = Be32(descriptor, 0)) {
        auto block = Slice(payload, offset, size);
        sha1::SHA1 hash;
        hash.processBytes(block.data(), block.size());
        uint8_t digest[20];
        hash.finalize(digest);
        if (std::memcmp(digest, descriptor.data() + 4, 20)) {
          return {};
        }
        descriptor = Slice(block, 0, 24);
        size_t pos = 24;
        while (true) {
          size_t length = Be16(block, pos);
          pos += 2;
          if (!length) {
            break;
          }
          auto chunk = Slice(block, pos, length);
          compressed.insert(compressed.end(), chunk.begin(), chunk.end());
          pos += length;
        }
        offset += size;
      }
      if (compressed.empty()) {
        return {};
      }
      std::vector<uint8_t> image(image_size);
      if (lzx_decompress(compressed.data(), compressed.size(), image.data(),
                         image.size(), window, nullptr, 0)) {
        return {};
      }
      return image;
    }
    default:
      return {};  // Delta patches are not standalone executables.
  }
}

std::vector<uint8_t> XexIcon(Bytes xex) {
  if (xex.size() < 24 || xex.size() > kMaxImage) {
    return {};
  }
  uint32_t magic = Be32(xex, 0);
  if (magic != 0x58455831 && magic != 0x58455832) {
    return {};
  }
  size_t header_size = Be32(xex, 8);
  if (header_size < 24 || header_size > 16 * 1024 * 1024) {
    return {};
  }
  auto header = Slice(xex, 0, header_size);
  auto opts = Slice(header, 24, size_t(Be32(header, 20)) * 8);
  Bytes format, resources;
  std::optional<uint32_t> image_base;
  for (size_t i = 0; i < opts.size(); i += 8) {
    uint32_t key = Be32(opts, i), offset = Be32(opts, i + 4);
    if (key == XEX_HEADER_IMAGE_BASE_ADDRESS) {
      image_base = offset;
    }
    if (key == XEX_HEADER_FILE_FORMAT_INFO || key == XEX_HEADER_RESOURCE_INFO) {
      auto info = Slice(header, offset, Be32(header, offset));
      if (key == XEX_HEADER_FILE_FORMAT_INFO) {
        format = info;
      } else {
        resources = info;
      }
    }
  }
  if (format.size() < 8 || resources.size() < 20 ||
      (resources.size() - 4) % 16) {
    return {};
  }
  size_t security = Be32(header, 16);
  size_t key_offset = magic == 0x58455832
                          ? offsetof(xex2_security_info, aes_key)
                          : offsetof(xex1_security_info, aes_key);
  size_t base_offset = magic == 0x58455832
                           ? offsetof(xex2_security_info, load_address)
                           : offsetof(xex1_security_info, load_address);
  uint32_t base = image_base.value_or(Be32(header, security + base_offset));
  size_t image_size = Be32(header, security + 4);
  if (image_size < 2 || image_size > kMaxImage) {
    return {};
  }
  auto encrypted_key = Slice(header, security + key_offset, 16);
  auto payload = xex.subspan(header_size);
  uint16_t encryption = Be16(format, 4);
  if (encryption > 1 || (encryption && payload.size() % 16)) {
    return {};
  }
  const uint8_t* keys[] = {xe_xex2_retail_key, xe_xex2_devkit_key,
                           xe_xex1_retail_key, xe_xex1_devkit_key};
  int attempts = !encryption ? 1 : magic == 0x58455831 ? 4 : 2;
  for (int attempt = 0; attempt < attempts; ++attempt) {
    std::vector<uint8_t> decrypted;
    Bytes source = payload;
    if (encryption) {
      uint8_t session[16];
      aes_decrypt_buffer(keys[attempt], encrypted_key.data(), 16, session, 16);
      decrypted.resize(payload.size());
      aes_decrypt_buffer(session, payload.data(), payload.size(),
                         decrypted.data(), decrypted.size());
      source = decrypted;
    }
    auto image = DecodeImage(source, format, image_size);
    if (image.size() < 2 || image[0] != 'M' || image[1] != 'Z') {
      continue;
    }
    for (size_t i = 4; i < resources.size(); i += 16) {
      uint32_t address = Be32(resources, i + 8),
               length = Be32(resources, i + 12);
      if (address < base || address - base > image.size() ||
          length > image.size() - (address - base)) {
        continue;
      }
      auto icon = SpaIcon(Slice(image, address - base, length));
      if (!icon.empty()) {
        return icon;
      }
    }
  }
  return {};
}
std::vector<uint32_t> XexExecutionInfo(Bytes xex) {
  if (xex.size() < 24 ||
      (Be32(xex, 0) != 0x58455831 && Be32(xex, 0) != 0x58455832)) {
    return {};
  }
  size_t header_size = Be32(xex, 8);
  if (header_size < 24 || header_size > 16 * 1024 * 1024) {
    return {};
  }
  auto header = Slice(xex, 0, header_size);
  auto options = Slice(header, 24, size_t(Be32(header, 20)) * 8);
  for (size_t i = 0; i < options.size(); i += 8) {
    if (Be32(options, i) != XEX_HEADER_EXECUTION_INFO) {
      continue;
    }
    auto info =
        Slice(header, Be32(options, i + 4), sizeof(xex2_opt_execution_info));
    return {Be32(info, 12), Be32(info, 0), Be32(info, 4),
            Be32(info, 8),  info[18],      info[19]};
  }
  return {};
}

// Share format detection and bounds checks between artwork and title details.
template <typename XexReader, typename StfsReader>
auto ReadGameData(const std::filesystem::path& path, bool header_only,
                  XexReader read_xex, StfsReader read_stfs)
    -> decltype(read_xex(Bytes{})) {
  try {
    auto map = MappedMemory::Open(path, MappedMemory::Mode::kRead);
    if (!map || map->size() < 4) {
      return {};
    }
    Bytes file(map->data(), map->size());
    uint32_t magic = Be32(file, 0);
    if (file.size() >= sizeof(_ZARCHIVE::Footer) &&
        Be32(file, file.size() - 4) == _ZARCHIVE::Footer::kMagic) {
      _ZARCHIVE::Footer footer;
      std::memcpy(&footer, file.data() + file.size() - sizeof(footer),
                  sizeof(footer));
      _ZARCHIVE::Footer::Deserialize(&footer, &footer);
      // Bound the archive reader's metadata allocations before opening it.
      for (const auto& section :
           {footer.sectionOffsetRecords, footer.sectionNames,
            footer.sectionFileTree, footer.sectionMetaDirectory,
            footer.sectionMetaData}) {
        if (section.size > 16 * 1024 * 1024 || section.offset > file.size() ||
            section.size > file.size() - section.offset) {
          return {};
        }
      }
      auto compressed = footer.sectionCompressedData;
      if (compressed.offset > file.size() ||
          compressed.size > file.size() - compressed.offset) {
        return {};
      }
      auto tree = Slice(file, footer.sectionFileTree.offset,
                        footer.sectionFileTree.size);
      if (tree.size() % 16) {
        return {};
      }
      for (size_t i = 0; i < tree.size(); i += 16) {
        uint32_t name = Be32(tree, i) & 0x7fffffff;
        if (name != 0x7fffffff && name >= footer.sectionNames.size) {
          return {};
        }
        if (!(Be32(tree, i) & 0x80000000)) {
          size_t start = Be32(tree, i + 4), count = Be32(tree, i + 8);
          if (start > tree.size() / 16 || count > tree.size() / 16 - start) {
            return {};
          }
        }
      }
      auto records = Slice(file, footer.sectionOffsetRecords.offset,
                           footer.sectionOffsetRecords.size);
      if (records.size() % 40) {
        return {};
      }
      for (size_t i = 0; i < records.size(); i += 40) {
        uint64_t base =
            (uint64_t(Be32(records, i)) << 32) | Be32(records, i + 4);
        if (base > compressed.size) {
          return {};
        }
      }
      return read_xex(vfs::ReadZarExecutable(path, kMaxImage, header_only));
    }
    if (magic == 0x434F4E20 || magic == 0x4C495645 || magic == 0x50495253) {
      auto metadata = vfs::ExtractStfsMetadata(path);
      if (!metadata) {
        return {};
      }
      return read_stfs(*metadata);
    }
    if (magic == 0x58455831 || magic == 0x58455832) {
      return read_xex(file);
    }
    if (auto partition = vfs::GdfxFindPartition(file.data(), file.size())) {
      if (auto xex = vfs::GdfxFindFile(file.data(), file.size(), *partition,
                                       "default.xex")) {
        return read_xex(Slice(file, xex->offset, xex->length));
      }
    }
  } catch (const std::exception&) {
    // Corrupt/unsupported media must not prevent browsing the library.
  }
  return {};
}
}  // namespace

std::vector<uint8_t> ReadGameIcon(const std::filesystem::path& path) {
  return ReadGameData(path, false, XexIcon, [](const vfs::StfsMetadata& m) {
    return Png(m.icon_data);
  });
}

std::vector<uint32_t> ReadGameExecutionInfo(const std::filesystem::path& path) {
  return ReadGameData(
      path, true, XexExecutionInfo, [](const vfs::StfsMetadata& m) {
        return std::vector<uint32_t>{m.title_id, m.media_id,    m.version,
                                     0,          m.disc_number, m.disc_count};
      });
}
}  // namespace xe::app
