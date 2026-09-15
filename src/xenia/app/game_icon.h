#ifndef XENIA_APP_GAME_ICON_H_
#define XENIA_APP_GAME_ICON_H_
#include <cstdint>
#include <filesystem>
#include <vector>

namespace xe::app {
// Embedded title image, without starting emulation or writing game data.
// Missing, unsupported or invalid resources return an empty vector.
std::vector<uint8_t> ReadGameIcon(const std::filesystem::path& path);
// Title ID, media ID, version, base version, disc number and disc count.
// Empty if unavailable. Reads headers only, without image
// decryption/decompression.
std::vector<uint32_t> ReadGameExecutionInfo(const std::filesystem::path& path);
}  // namespace xe::app
#endif
