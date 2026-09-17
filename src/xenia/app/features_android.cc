// Android bindings to desktop profile, content, patch and statistics services.
#include <algorithm>
#include <charconv>
#include <chrono>
#include "third_party/stb/stb_image.h"
#include "xenia/app/android_features.h"
#include "xenia/app/game_compat_db.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/string_util.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/user_tracker.h"
#include "xenia/patcher/patch_db.h"
#include "xenia/patcher/patch_file_editor.h"
#include "xenia/ui/profile_options.h"
#include "xenia/vfs/devices/xcontent_container_device.h"

namespace {
// Only the management worker installs packages. The UI may poll/cancel without
// accessing its emulator. Shared ownership keeps the entry alive across polls;
// generation IDs prevent late callbacks from cancelling a subsequent batch.
std::mutex install_mutex;
uint64_t install_generation = 0, install_session = 0;
bool install_cancelled = false;
std::shared_ptr<xe::Emulator::ContentInstallEntry> install_entry;
using Rows = std::vector<std::vector<std::string>>;
std::string Text(JNIEnv* env, jstring text) {
  if (!text) {
    return {};
  }
  const auto* chars = env->GetStringChars(text, nullptr);
  if (!chars) {
    throw std::runtime_error("Unable to read text");
  }
  auto result = xe::to_utf8(std::u16string_view(
      reinterpret_cast<const char16_t*>(chars), env->GetStringLength(text)));
  env->ReleaseStringChars(text, chars);
  return result;
}
jobjectArray JavaRows(JNIEnv* env, const Rows& rows) {
  auto array_class = env->FindClass("[Ljava/lang/String;");
  auto string_class = env->FindClass("java/lang/String");
  auto result = env->NewObjectArray(jsize(rows.size()), array_class, nullptr);
  for (jsize i = 0; result && i < jsize(rows.size()); ++i) {
    auto row =
        env->NewObjectArray(jsize(rows[i].size()), string_class, nullptr);
    if (!row) {
      break;
    }
    for (jsize j = 0; j < jsize(rows[i].size()); ++j) {
      auto value = xe::to_utf16(rows[i][j]);
      auto s = env->NewString(reinterpret_cast<const jchar*>(value.data()),
                              jsize(value.size()));
      env->SetObjectArrayElement(row, j, s);
      env->DeleteLocalRef(s);
    }
    env->SetObjectArrayElement(result, i, row);
    env->DeleteLocalRef(row);
  }
  env->DeleteLocalRef(array_class);
  env->DeleteLocalRef(string_class);
  return result;
}
uint64_t Number(std::string_view value, int base = 10) {
  uint64_t result = 0;
  auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result, base);
  if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size()) {
    throw std::invalid_argument("Invalid number");
  }
  return result;
}
void Require(bool ok, const char* error) {
  if (!ok) {
    throw std::runtime_error(error);
  }
}
void Error(JNIEnv* env, const std::exception& error) {
  auto cls = env->FindClass("java/io/IOException");
  env->ThrowNew(cls, error.what());
  env->DeleteLocalRef(cls);
}
auto ProfileManager(xe::Emulator* emulator) {
  return emulator->kernel_state()->xam_state()->profile_manager();
}
using xe::kernel::xam::UserSettingId;
const std::map<std::string, std::pair<UserSettingId, size_t>> kProfileStrings =
    {{"gamer_name", {UserSettingId::XPROFILE_GAMERCARD_USER_NAME, 130}},
     {"motto", {UserSettingId::XPROFILE_GAMERCARD_MOTTO, 22}},
     {"bio", {UserSettingId::XPROFILE_GAMERCARD_USER_BIO, 500}}};
std::vector<std::string> Choices(const std::string& key) {
  std::vector<std::string> values;
  auto add = [&](auto& names) {
    for (size_t i = 0; i < std::size(names); ++i) {
      if (!i || names[i]) {
        values.push_back(std::to_string(i) + ":" +
                         (names[i] ? names[i] : "Default"));
      }
    }
  };
  if (key == "country") {
    add(xe::ui::kCountryNames);
  }
  if (key == "language") {
    add(xe::ui::kLanguageNames);
  }
  if (key == "subscription") {
    add(xe::ui::kSubscriptionTierNames);
  }
  if (key == "zone") {
    add(xe::ui::kGamerZoneNames);
  }
  if (key == "live") {
    values = {"false:Off", "true:On"};
  }
  return values;
}
// Resolve only names supplied by our content list, never arbitrary paths.
std::filesystem::path Child(const std::filesystem::path& root,
                            const std::string& name) {
  auto child = xe::to_path(name);
  Require(
      !name.empty() && child.filename() == child && name != "." && name != "..",
      "Invalid file name");
  return root / child;
}
auto PatchFiles(xe::Emulator* emulator, uint32_t title) {
  auto files = xe::patcher::EnumerateBundledPatchesForTitle(title);
  for (auto& local : xe::patcher::EnumerateLocalPatchesForTitle(
           emulator->storage_root() / "patches", title)) {
    if (std::none_of(files.begin(), files.end(), [&](const auto& file) {
          return file.filename == local.filename;
        })) {
      files.push_back(std::move(local));
    }
  }
  return files;
}
auto PatchEditor(xe::Emulator* emulator,
                 const xe::patcher::PatchSourceFile& file, bool temporary) {
  auto path = Child(emulator->storage_root() / "patches", file.filename);
  auto source = std::filesystem::exists(path)
                    ? xe::filesystem::ReadAllText(path)
                    : file.toml_content;
  Require(!source.empty(), "Cannot read patch file");
  if (temporary) {
    path += ".tmp";
  }
  return xe::patcher::PatchFileEditor(source, path);
}
auto Patch(xe::Emulator* emulator, uint32_t title, const std::string& filename,
           bool temporary = false) {
  for (const auto& file : PatchFiles(emulator, title)) {
    if (file.filename == filename) {
      return PatchEditor(emulator, file, temporary);
    }
  }
  throw std::invalid_argument("Unknown patch file");
}
Rows List(xe::Emulator* emulator, const std::string& mode, uint32_t title,
          uint64_t xuid) {
  Rows rows;
  auto* pm = ProfileManager(emulator);
  if (mode == "profiles") {
    for (const auto& [id, account] : *pm->GetAccounts()) {
      auto slot = pm->GetUserIndexAssignedToProfile(id);
      rows.push_back({fmt::format("{:016X}", id), account.GetGamertagString(),
                      slot < 4 ? std::to_string(slot + 1) : "0"});
    }
  } else if (mode == "profile") {
    auto* account = pm->GetAccount(xuid);
    Require(account != nullptr, "Profile not found");
    auto domain_bytes = account->GetOnlineDomain();
    std::string domain(domain_bytes.data(), domain_bytes.size());
    if (auto end = domain.find('\0'); end != std::string::npos) {
      domain.resize(end);
    }
    rows = {
        {"gamertag", account->GetGamertagString()},
        {"country", std::to_string(uint32_t(account->GetCountry()))},
        {"language", std::to_string(uint32_t(account->GetLanguage()))},
        {"live", account->IsLiveEnabled() ? "true" : "false"},
        {"subscription",
         std::to_string(uint32_t(account->GetSubscriptionTier()))},
        {"online_xuid",
         xe::string_util::to_hex_string(account->GetOnlineXUID()), "readonly"},
        {"online_domain", domain, "readonly"}};
    auto* profile = pm->GetProfile(xuid);
    if (profile) {
      auto* tracker = emulator->kernel_state()->xam_state()->user_tracker();
      for (const auto& [key, spec] : kProfileStrings) {
        auto setting = tracker->GetSetting(profile, xe::kernel::kDashboardID,
                                           uint32_t(spec.first));
        std::string value;
        if (setting) {
          auto data = setting->get_host_data();
          if (auto* text = std::get_if<std::u16string>(&data)) {
            auto end = text->find(u'\0');
            value = xe::to_utf8(std::u16string_view(*text).substr(0, end));
          }
        }
        rows.push_back({key, value});
      }
      auto zone =
          tracker->GetSetting(profile, xe::kernel::kDashboardID,
                              uint32_t(UserSettingId::XPROFILE_GAMERCARD_ZONE));
      int32_t value = 0;
      if (zone) {
        auto data = zone->get_host_data();
        if (auto* number = std::get_if<int32_t>(&data)) {
          value = *number;
        }
      }
      rows.push_back({"zone", std::to_string(value)});
      rows.push_back({"icon", ""});
    }
    for (auto& row : rows) {
      auto choices = Choices(row[0]);
      row.insert(row.end(), choices.begin(), choices.end());
    }
  } else if (mode == "patches") {
    for (const auto& file : PatchFiles(emulator, title)) {
      auto editor = PatchEditor(emulator, file, false);
      for (size_t i = 0; i < editor.patches().size(); ++i) {
        const auto& patch = editor.patches()[i];
        rows.push_back(
            {file.filename, patch.name, patch.description + "\n" + patch.author,
             std::to_string(i), patch.is_enabled ? "true" : "false"});
      }
    }
  } else if (mode == "content" || mode == "saves") {
    std::vector<std::pair<uint64_t, xe::XContentType>> locations;
    if (mode == "content") {
      locations = {{0, xe::XContentType::kInstaller},
                   {0, xe::XContentType::kMarketplaceContent}};
    } else {
      locations.push_back({0, xe::XContentType::kSavedGame});
      for (const auto& [id, account] : *pm->GetAccounts()) {
        locations.push_back({id, xe::XContentType::kSavedGame});
      }
    }
    for (auto [owner, type] : locations) {
      auto root = pm->GetProfileContentPath(owner, title, type);
      for (const auto& data :
           emulator->kernel_state()->content_manager()->ListContent(
               0, owner, title, type)) {
        auto filename = data.file_name();
        auto file = root / xe::to_path(filename);
        auto name = xe::to_utf8(data.display_name());
        std::error_code ec;
        uintmax_t size = 0;
        if (std::filesystem::is_regular_file(file, ec)) {
          auto bytes = std::filesystem::file_size(file, ec);
          if (!ec) {
            size = bytes;
          }
        } else if (std::filesystem::is_directory(file, ec)) {
          for (auto it = std::filesystem::recursive_directory_iterator(
                   file,
                   std::filesystem::directory_options::skip_permission_denied,
                   ec);
               !ec && it != std::filesystem::recursive_directory_iterator();
               it.increment(ec)) {
            if (it->is_regular_file(ec)) {
              auto bytes = it->file_size(ec);
              if (!ec) {
                size += bytes;
              }
            }
          }
        }
        rows.push_back({filename, name.empty() ? filename : name,
                        fmt::format("{:016X}/{:08X}", owner, uint32_t(type)),
                        xe::path_to_utf8(file),
                        std::filesystem::is_symlink(file) ? "link" : "file",
                        std::to_string(size)});
      }
    }
  } else if (mode == "stats") {
    auto urls = xe::app::GetCompatUrls(title);
    rows.push_back({"compatibility",
                    std::to_string(int(xe::app::GetCompatState(title))),
                    urls.canary, urls.master});
    auto* tracker = emulator->kernel_state()->xam_state()->user_tracker();
    for (uint8_t slot = 0; slot < 4; ++slot) {
      auto* profile = pm->GetProfile(slot);
      if (!profile) {
        continue;
      }
      auto info = tracker->GetUserTitleInfo(profile->xuid(), title);
      if (info) {
        rows.push_back({"achievements", profile->name(),
                        fmt::format("{}/{}", info->unlocked_achievements_count,
                                    info->achievements_count),
                        fmt::format("{}/{} G", info->title_earned_gamerscore,
                                    info->gamerscore_amount)});
      }
    }
  } else {
    throw std::invalid_argument("Unknown page");
  }
  return rows;
}

extern "C" JNIEXPORT void JNICALL
Java_jp_xenia_emulator_AndroidTools_iconNative(JNIEnv* env, jclass,
                                               jlong context, jlong xuid,
                                               jbyteArray png) {
  try {
    auto* emulator = xe::app::AndroidEmulator(context);
    auto* profile = ProfileManager(emulator)->GetProfile(uint64_t(xuid));
    Require(profile != nullptr, "Sign in before changing the profile image");
    size_t size = png ? env->GetArrayLength(png) : 0;
    Require(size <= 65536, "Profile image is too large");
    std::vector<uint8_t> bytes(size);
    if (size) {
      env->GetByteArrayRegion(png, 0, jsize(size),
                              reinterpret_cast<jbyte*>(bytes.data()));
    }
    if (size) {
      int width = 0, height = 0, channels = 0;
      Require(stbi_info_from_memory(bytes.data(), int(size), &width, &height,
                                    &channels) &&
                  width == height && (width == 32 || width == 64),
              "Choose a 32 or 64 pixel profile image");
    }
    auto* fs = emulator->kernel_state()->file_system();
    fs->DeletePath(fmt::format("User_{:016X}:\\tile_64.png", uint64_t(xuid)));
    fs->DeletePath(fmt::format("User_{:016X}:\\tile_32.png", uint64_t(xuid)));
    for (auto type : {xe::kernel::xam::XTileType::kGamerTile,
                      xe::kernel::xam::XTileType::kGamerTileSmall,
                      xe::kernel::xam::XTileType::kPersonalGamerTile,
                      xe::kernel::xam::XTileType::kPersonalGamerTileSmall}) {
      profile->ClearProfileIcon(type);
    }
    if (size) {
      Require(
          emulator->kernel_state()->xam_state()->user_tracker()->UpdateUserIcon(
              uint64_t(xuid), bytes),
          "Could not save profile image");
    }
  } catch (const std::exception& e) {
    Error(env, e);
  }
}

extern "C" JNIEXPORT jlong JNICALL
Java_jp_xenia_emulator_AndroidTools_beginInstallNative(JNIEnv* env, jclass) {
  try {
    std::lock_guard lock(install_mutex);
    Require(!install_session, "An installation is already running");
    install_session = ++install_generation;
    install_cancelled = false;
    install_entry.reset();
    return jlong(install_session);
  } catch (const std::exception& e) {
    Error(env, e);
    return 0;
  }
}
extern "C" JNIEXPORT jlongArray JNICALL
Java_jp_xenia_emulator_AndroidTools_installProgressNative(JNIEnv* env, jclass,
                                                          jlong session,
                                                          jboolean cancel) {
  jlong values[3] = {};
  {
    std::lock_guard lock(install_mutex);
    if (session && uint64_t(session) == install_session) {
      install_cancelled |= bool(cancel);
      values[2] = install_cancelled;
      if (install_entry) {
        if (install_cancelled) {
          install_entry->cancelled_.store(true);
        }
        values[0] = jlong(install_entry->currently_installed_size_.load());
        values[1] = jlong(install_entry->content_size_.load());
      }
    }
  }
  auto result = env->NewLongArray(3);
  if (result) {
    env->SetLongArrayRegion(result, 0, 3, values);
  }
  return result;
}
extern "C" JNIEXPORT void JNICALL
Java_jp_xenia_emulator_AndroidTools_endInstallNative(JNIEnv*, jclass,
                                                     jlong session) {
  std::lock_guard lock(install_mutex);
  if (session && uint64_t(session) == install_session) {
    install_session = 0;
    install_entry.reset();
  }
}

// False means the package needs mutable storage and the caller must ask before
// extracting. Read-only packages are registered in place; originals are
// untouched.
extern "C" JNIEXPORT jboolean JNICALL
Java_jp_xenia_emulator_AndroidTools_installPackageNative(JNIEnv* env, jclass,
                                                         jlong context,
                                                         jstring path,
                                                         jboolean extract,
                                                         jlong session) {
  try {
    auto* emulator = xe::app::AndroidEmulator(context);
    auto source = std::filesystem::canonical(xe::to_path(Text(env, path)));
    auto active = std::make_shared<xe::Emulator::ContentInstallEntry>(source);
    auto& entry = *active;
    if (session) {
      std::lock_guard lock(install_mutex);
      Require(uint64_t(session) == install_session && !install_cancelled,
              "Installation cancelled");
      install_entry = active;
    }
    Require(XSUCCEEDED(emulator->ProcessContentPackageHeader(source, entry)),
            "Invalid content package");
    Require(!entry.cancelled_.load(), "Installation cancelled");
    bool mutable_data = entry.content_type_ == xe::XContentType::kProfile ||
                        entry.content_type_ == xe::XContentType::kSavedGame;
    if (mutable_data && !extract) {
      return false;
    }
    auto root = emulator->content_root();
    auto destination = root / entry.data_installation_path_;
    // Refuse symlink parents and existing data, including dangling links.
    for (auto parent = destination.parent_path(); parent != root;
         parent = parent.parent_path()) {
      Require(!std::filesystem::is_symlink(parent),
              "Linked destination folder is not writable");
    }
    if (!mutable_data && std::filesystem::is_symlink(destination) &&
        std::filesystem::canonical(destination) == source) {
      return true;
    }
    Require(
        !std::filesystem::exists(std::filesystem::symlink_status(destination)),
        "Content already exists; no data was replaced");
    std::filesystem::create_directories(destination.parent_path());
    if (!mutable_data) {
      std::filesystem::create_symlink(source, destination);
      return true;
    }
    auto header = root / entry.header_installation_path_;
    header += ".header";
    Require(!std::filesystem::exists(std::filesystem::symlink_status(header)),
            "Content header already exists; no data was replaced");
    for (auto parent = header.parent_path(); parent != root;
         parent = parent.parent_path()) {
      Require(!std::filesystem::is_symlink(parent),
              "Linked destination folder is not writable");
    }
    // Extract with the desktop installer into a unique staging directory, then
    // publish complete data. A failed import never leaves a partial profile.
    auto staging =
        root /
        (".import-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(std::filesystem::create_directory(staging),
            "Could not create import directory");
    auto cleanup = [&]() {
      std::error_code ec;
      std::filesystem::remove_all(staging, ec);
    };
    entry.data_installation_path_ =
        xe::path_to_utf8(staging.filename() / "data");
    entry.header_installation_path_ =
        xe::path_to_utf8(staging.filename() / "headers" / source.filename());
    try {
      auto result = emulator->InstallContentPackage(source, entry);
      Require(XSUCCEEDED(result), entry.cancelled_.load()
                                      ? "Installation cancelled"
                                      : "Content extraction failed");
      std::filesystem::create_directories(header.parent_path());
      std::filesystem::rename(
          staging / "headers" /
              (xe::path_to_utf8(source.filename()) + ".header"),
          header);
      try {
        std::filesystem::rename(staging / "data", destination);
      } catch (...) {
        std::filesystem::remove(header);
        throw;
      }
      cleanup();
      if (entry.content_type_ == xe::XContentType::kProfile) {
        ProfileManager(emulator)->ReloadProfiles();
      }
    } catch (...) {
      cleanup();
      throw;
    }
    return true;
  } catch (const std::exception& e) {
    Error(env, e);
    return false;
  }
}
}  // namespace

extern "C" JNIEXPORT jobjectArray JNICALL
Java_jp_xenia_emulator_AndroidTools_listNative(JNIEnv* env, jclass,
                                               jlong context, jstring mode,
                                               jint title, jlong xuid) {
  try {
    return JavaRows(env,
                    List(xe::app::AndroidEmulator(context), Text(env, mode),
                         uint32_t(title), uint64_t(xuid)));
  } catch (const std::exception& e) {
    Error(env, e);
    return nullptr;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_jp_xenia_emulator_AndroidTools_changeNative(JNIEnv* env, jclass,
                                                 jlong context,
                                                 jstring operation, jint title,
                                                 jlong xuid, jstring key_text,
                                                 jstring value_text) {
  try {
    auto* emulator = xe::app::AndroidEmulator(context);
    auto* pm = ProfileManager(emulator);
    auto op = Text(env, operation), key = Text(env, key_text),
         value = Text(env, value_text);
    if (op == "create") {
      Require(pm->IsGamertagValid(value), "Invalid Gamertag");
      Require(pm->CreateProfile(value, false), "Could not create profile");
    } else if (op == "login") {
      auto slot = Number(value);
      Require(slot < 4 && pm->GetAccount(uint64_t(xuid)),
              "Invalid profile or player");
      pm->Login(uint64_t(xuid), uint8_t(slot));
      Require(pm->GetProfile(uint8_t(slot)) &&
                  pm->GetProfile(uint8_t(slot))->xuid() == uint64_t(xuid),
              "Could not sign in");
    } else if (op == "logout") {
      auto slot = pm->GetUserIndexAssignedToProfile(uint64_t(xuid));
      Require(slot < 4, "Profile is not signed in");
      pm->Logout(slot);
    } else if (op == "profile") {
      auto choices = Choices(key);
      if (!choices.empty()) {
        Require(std::any_of(choices.begin(), choices.end(),
                            [&](const auto& choice) {
                              return choice.substr(0, choice.find(':')) ==
                                     value;
                            }),
                "Choose a listed profile value");
      }
      auto* tracker = emulator->kernel_state()->xam_state()->user_tracker();
      if (kProfileStrings.count(key) || key == "zone") {
        Require(pm->GetProfile(uint64_t(xuid)) != nullptr,
                "Sign in before editing this profile field");
        if (key == "zone") {
          xe::kernel::xam::UserSetting setting(
              UserSettingId::XPROFILE_GAMERCARD_ZONE, int32_t(Number(value)));
          tracker->UpsertSetting(uint64_t(xuid), xe::kernel::kDashboardID,
                                 &setting);
        } else {
          const auto& spec = kProfileStrings.at(key);
          auto text = xe::to_utf16(value);
          Require(text.size() <= spec.second, "Profile text is too long");
          for (auto& c : text) {
            c = xe::byte_swap(c);
          }
          xe::kernel::xam::UserSetting setting(spec.first, text);
          tracker->UpsertSetting(uint64_t(xuid), xe::kernel::kDashboardID,
                                 &setting);
        }
        return;
      }
      auto* original = pm->GetAccount(uint64_t(xuid));
      Require(original != nullptr, "Profile not found");
      auto account = *original;
      if (key == "gamertag") {
        Require(pm->IsGamertagValid(value), "Invalid Gamertag");
        xe::string_util::copy_and_swap_truncating(
            account.gamertag, xe::to_utf16(value), std::size(account.gamertag));
      } else if (key == "country") {
        auto n = Number(value);
        Require(n <= 255, "Invalid country");
        account.SetCountry(xe::XOnlineCountry(n));
      } else if (key == "language") {
        auto n = Number(value);
        Require(n <= 31, "Invalid language");
        account.SetLanguage(xe::XLanguage(n));
      } else if (key == "live") {
        Require(value == "true" || value == "false", "Invalid value");
        account.ToggleLiveFlag(value == "true");
      } else if (key == "subscription") {
        auto n = Number(value);
        Require(n <= 15, "Invalid subscription");
        account.SetSubscriptionTier(
            xe::kernel::xam::X_XAMACCOUNTINFO::AccountSubscriptionTier(n));
      } else {
        throw std::invalid_argument("Unknown profile field");
      }
      bool mounted = pm->GetProfile(uint64_t(xuid)) != nullptr;
      Require(mounted || pm->MountProfile(uint64_t(xuid)),
              "Cannot open profile");
      bool updated = pm->UpdateAccount(uint64_t(xuid), &account);
      if (!mounted) {
        pm->DismountProfile(uint64_t(xuid));
      }
      Require(updated, "Could not save profile");
    } else if (op == "patch") {
      auto split = value.find(':');
      Require(split != std::string::npos, "Invalid patch toggle");
      auto index = Number(value.substr(0, split));
      auto enabled = value.substr(split + 1);
      Require(enabled == "true" || enabled == "false", "Invalid patch value");
      auto editor = Patch(emulator, uint32_t(title), key, true);
      Require(index < editor.patches().size() &&
                  editor.SetEnabled(size_t(index), enabled == "true"),
              "Could not save patch");
      std::filesystem::rename(editor.storage_path(),
                              Child(emulator->storage_root() / "patches", key));
    } else if (op == "content_add") {
      auto source = std::filesystem::canonical(xe::to_path(value));
      auto header =
          xe::vfs::XContentContainerDevice::ReadContainerHeader(source);
      Require(header && header->content_header.is_magic_valid(),
              "Not a content package");
      auto data =
          xe::vfs::XContentContainerDevice::ContentDataFromHeader(*header);
      Require(data.title_id == uint32_t(title),
              "Package belongs to a different title");
      auto type = data.content_type.get();
      Require(type == xe::XContentType::kInstaller ||
                  type == xe::XContentType::kMarketplaceContent,
              "Choose a DLC or title update package");
      auto version = Number(key);
      Require(
          type != xe::XContentType::kInstaller || !version ||
              !header->content_metadata.execution_info.version_value ||
              header->content_metadata.execution_info.version_value == version,
          "Update belongs to a different game release");
      auto root = pm->GetProfileContentPath(0, uint32_t(title), type);
      std::filesystem::create_directories(root);
      auto destination = Child(root, xe::path_to_utf8(source.filename()));
      if (std::filesystem::is_symlink(destination) &&
          std::filesystem::canonical(destination) == source) {
        return;
      }
      Require(!std::filesystem::exists(
                  std::filesystem::symlink_status(destination)),
              "A package with this name is already installed");
      // Private storage supports symlinks even when the original is on an SD
      // card. The desktop content reader opens the package in place; no media
      // is copied.
      std::filesystem::create_symlink(source, destination);
    } else if (op == "content_remove") {
      auto rows = List(emulator, "content", uint32_t(title), 0);
      auto it = std::find_if(rows.begin(), rows.end(),
                             [&](const auto& row) { return row[3] == key; });
      Require(it != rows.end() && (*it)[4] == "link",
              "Only linked content can be detached here");
      Require(std::filesystem::remove(xe::to_path(key)),
              "Could not detach content");
    } else {
      throw std::invalid_argument("Unknown operation");
    }
  } catch (const std::exception& e) {
    Error(env, e);
  }
}
