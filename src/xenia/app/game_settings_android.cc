// Android frontend for the same cvar registry and TOML format as desktop.
// No emulator or global cvar state is mutated while editing a title's config.
#include <jni.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <sstream>
#include "version.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/config.h"
#include "xenia/kernel/xam/xdbf/gpd_info_profile.h"
#include "xenia/ui/config_helpers.h"

namespace {
std::string Text(JNIEnv* env, jstring value) {
  if (!value) {
    return {};
  }
  const jchar* chars = env->GetStringChars(value, nullptr);
  if (!chars) {
    throw std::runtime_error("Unable to read text");
  }
  auto text = xe::to_utf8(std::u16string_view(
      reinterpret_cast<const char16_t*>(chars), env->GetStringLength(value)));
  env->ReleaseStringChars(value, chars);
  return text;
}
jstring JavaText(JNIEnv* env, const std::string& value) {
  auto text = xe::to_utf16(value);
  return env->NewString(reinterpret_cast<const jchar*>(text.data()),
                        jsize(text.size()));
}
void Error(JNIEnv* env, const std::exception& error) {
  auto cls = env->FindClass("java/lang/IllegalArgumentException");
  env->ThrowNew(cls, error.what());
  env->DeleteLocalRef(cls);
}
bool Available(cvar::IConfigVar* var) {
  if (var->is_transient()) {
    return false;
  }
  const auto& cat = var->category();
  if (cat == "Config" || cat == "Profiles" || cat == "Window") {
    return false;
  }
  const auto& name = var->name();
  if (name == "storage_root" || name == "gpu" || name == "apu" ||
      name == "hid" || name == "cpu") {
    return false;
  }
  std::string key = cat + "." + name;
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  for (auto unsupported : {"d3d", "direct3d", "dxgi", "dxbc", "dxil", "metal",
                           "moltenvk", "x64", "xaudio", "xinput"}) {
    if (key.find(unsupported) != std::string::npos) {
      return false;
    }
  }
  return true;
}
std::string Type(cvar::IConfigVar* var) {
  if (dynamic_cast<cvar::ConfigVar<bool>*>(var)) {
    return "bool";
  }
  if (dynamic_cast<cvar::ConfigVar<std::string>*>(var)) {
    return "string";
  }
  if (dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(var)) {
    return "path";
  }
  if (dynamic_cast<cvar::ConfigVar<double>*>(var)) {
    return "double";
  }
  return "integer";
}
std::string Value(const toml::node& node) {
  if (auto text = node.value<std::string>()) {
    return *text;
  }
  std::ostringstream out;
  out << toml::toml_formatter(node);
  return out.str();
}
template <typename T>
bool IntegerFits(cvar::IConfigVar* var, int64_t value) {
  if (!dynamic_cast<cvar::ConfigVar<T>*>(var)) {
    return false;
  }
  if constexpr (std::is_unsigned_v<T>) {
    return value >= 0 && uint64_t(value) <= std::numeric_limits<T>::max();
  } else {
    return value >= std::numeric_limits<T>::min() &&
           value <= std::numeric_limits<T>::max();
  }
}
}  // namespace

extern "C" {
// Read only the four configured profiles, without constructing an emulator or
// mutating cvars. The Java caller holds DataLock while reading profile files.
JNIEXPORT jlongArray JNICALL Java_jp_xenia_emulator_GameSettings_historyNative(
    JNIEnv* env, jclass, jstring content_root, jstring document) {
  try {
    auto config = toml::parse(Text(env, document));
    auto root = xe::to_path(Text(env, content_root));
    std::map<uint32_t, int64_t> times;
    for (int slot = 0; slot < 4; ++slot) {
      auto id =
          config["Profiles"][fmt::format("logged_profile_slot_{}_xuid", slot)]
              .value_or(std::string());
      uint64_t xuid = 0;
      auto parsed = std::from_chars(id.data(), id.data() + id.size(), xuid, 16);
      if (id.size() != 16 || parsed.ec != std::errc() ||
          parsed.ptr != id.data() + id.size() || !xuid) {
        continue;
      }
      id = fmt::format("{:016X}", xuid);
      auto path = root / id / "FFFE07D1" / "00010000" / id / "FFFE07D1.gpd";
      std::error_code ec;
      auto size = std::filesystem::file_size(path, ec);
      if (ec || !size || size > 64 * 1024 * 1024) {
        continue;
      }
      xe::kernel::xam::GpdInfoProfile dashboard(
          xe::filesystem::ReadAllBytes(path));
      if (!dashboard.IsValid()) {
        continue;
      }
      for (auto* info : dashboard.GetTitlesInfo()) {
        if (!info->last_played.is_valid()) {
          continue;
        }
        auto time = xe::chrono::WinSystemClock::to_sys(
            info->last_played.to_time_point());
        int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         time.time_since_epoch())
                         .count();
        auto& latest = times[info->title_id];
        latest = std::max(latest, ms);
      }
    }
    std::vector<jlong> rows;
    for (auto [title, time] : times) {
      rows.push_back(title);
      rows.push_back(time);
    }
    auto result = env->NewLongArray(jsize(rows.size()));
    if (result && !rows.empty()) {
      env->SetLongArrayRegion(result, 0, jsize(rows.size()), rows.data());
    }
    return result;
  } catch (const std::exception& e) {
    Error(env, e);
    return nullptr;
  }
}
JNIEXPORT jobjectArray JNICALL
Java_jp_xenia_emulator_GameSettings_buildNative(JNIEnv* env, jclass) {
  auto cls = env->FindClass("java/lang/String");
  auto result = env->NewObjectArray(4, cls, nullptr);
  const char* values[] = {
      XE_BUILD_BRANCH, XE_BUILD_COMMIT_SHORT, XE_BUILD_DATE,
#ifdef XE_BUILD_IS_PR
      "https://github.com/has207/xenia-edge/pull/" XE_BUILD_PR_NUMBER
#else
      "https://github.com/has207/xenia-edge/commit/" XE_BUILD_COMMIT
#endif
  };
  for (jsize i = 0; result && i < 4; ++i) {
    auto text = JavaText(env, values[i]);
    env->SetObjectArrayElement(result, i, text);
    env->DeleteLocalRef(text);
  }
  env->DeleteLocalRef(cls);
  return result;
}

JNIEXPORT jobjectArray JNICALL Java_jp_xenia_emulator_GameSettings_rootsNative(
    JNIEnv* env, jclass, jstring document) {
  try {
    auto table = toml::parse(Text(env, document));
    auto cls = env->FindClass("java/lang/String");
    auto result = env->NewObjectArray(4, cls, nullptr);
    const char* keys[] = {"Storage.content_root", "Storage.cache_root",
                          "Logging.log_file", "UI.ui_locale"};
    for (jsize i = 0; i < 4; ++i) {
      auto text = JavaText(env, table.at_path(keys[i]).value_or(std::string()));
      env->SetObjectArrayElement(result, i, text);
      env->DeleteLocalRef(text);
    }
    env->DeleteLocalRef(cls);
    return result;
  } catch (const std::exception& e) {
    Error(env, e);
    return nullptr;
  }
}

// Rows: name, category, description, type, effective value, inherited value,
// has override, advanced, followed by the desktop's enum display options.
JNIEXPORT jobjectArray JNICALL Java_jp_xenia_emulator_GameSettings_listNative(
    JNIEnv* env, jclass, jstring global_text, jstring game_text, jint scale,
    jint fps) {
  try {
    auto global = toml::parse(Text(env, global_text));
    auto game = toml::parse(Text(env, game_text));
    // Match the existing Android general video controls, below title overrides.
    auto* gpu = config::ResolveSectionTable(global, "GPU");
    if (scale >= 0) {
      gpu->insert_or_assign("draw_resolution_scale_x",
                            std::clamp(int(scale), 1, 2));
      gpu->insert_or_assign("draw_resolution_scale_y",
                            std::clamp(int(scale), 1, 2));
    }
    if (fps >= 0) {
      gpu->insert_or_assign("framerate_limit", int(fps));
    }
    std::vector<cvar::IConfigVar*> vars;
    if (cvar::ConfigVars) {
      for (auto [name, var] : *cvar::ConfigVars) {
        if (Available(var)) {
          vars.push_back(var);
        }
      }
    }
    auto row_class = env->FindClass("[Ljava/lang/String;");
    auto string_class = env->FindClass("java/lang/String");
    auto result = env->NewObjectArray(jsize(vars.size()), row_class, nullptr);
    for (jsize i = 0; result && i < jsize(vars.size()); ++i) {
      auto* var = vars[i];
      auto path = toml::path(var->category() + "." + var->name());
      auto fallback = toml::parse("value = " + var->default_value());
      const auto* base = global.at_path(path).node();
      if (!base) {
        base = fallback["value"].node();
      }
      const auto* override = game.at_path(path).node();
      auto display = [&](const toml::node* node) {
        return xe::ui::IntCvarValueToDisplayName(var->name(), Value(*node));
      };
      std::vector<std::string> row = {var->name(),
                                      var->category(),
                                      var->description(),
                                      Type(var),
                                      display(override ? override : base),
                                      display(base),
                                      override ? "true" : "false",
                                      var->is_advanced() ? "true" : "false"};
      auto enums = xe::ui::GetKnownEnumOptions().find(var->name());
      if (enums != xe::ui::GetKnownEnumOptions().end()) {
        row.insert(row.end(), enums->second.begin(), enums->second.end());
      }
      auto values =
          env->NewObjectArray(jsize(row.size()), string_class, nullptr);
      if (!values) {
        break;
      }
      for (jsize j = 0; j < jsize(row.size()); ++j) {
        auto value = JavaText(env, row[j]);
        env->SetObjectArrayElement(values, j, value);
        env->DeleteLocalRef(value);
      }
      env->SetObjectArrayElement(result, i, values);
      env->DeleteLocalRef(values);
    }
    env->DeleteLocalRef(row_class);
    env->DeleteLocalRef(string_class);
    return result;
  } catch (const std::exception& e) {
    Error(env, e);
    return nullptr;
  }
}

JNIEXPORT jstring JNICALL Java_jp_xenia_emulator_GameSettings_resetNative(
    JNIEnv* env, jclass, jstring document, jobjectArray names) {
  try {
    auto table = toml::parse(Text(env, document));
    if (!names) {
      throw std::invalid_argument("Missing settings to reset");
    }
    for (jsize i = 0; i < env->GetArrayLength(names); ++i) {
      auto item = static_cast<jstring>(env->GetObjectArrayElement(names, i));
      const auto name = Text(env, item);
      env->DeleteLocalRef(item);
      auto it = cvar::ConfigVars->find(name);
      if (it == cvar::ConfigVars->end() || !Available(it->second)) {
        throw std::invalid_argument("Setting is not available on Android");
      }
      auto section = table.at_path(toml::path(it->second->category()));
      if (section && !section.is_table()) {
        throw std::invalid_argument("Invalid configuration section");
      }
      if (auto* values = section.as_table()) {
        values->erase(name);
      }
    }
    std::ostringstream out;
    out << table;
    return JavaText(env, out.str());
  } catch (const std::exception& e) {
    Error(env, e);
    return nullptr;
  }
}

JNIEXPORT jstring JNICALL Java_jp_xenia_emulator_GameSettings_editNative(
    JNIEnv* env, jclass, jstring document, jstring name_text,
    jstring value_text) {
  try {
    const auto name = Text(env, name_text);
    auto it = cvar::ConfigVars->find(name);
    if (it == cvar::ConfigVars->end() || !Available(it->second)) {
      throw std::invalid_argument("Setting is not available on Android");
    }
    auto* var = it->second;
    // Parse strictly: never replace an unreadable existing config with an empty
    // one.
    auto table = toml::parse(Text(env, document));
    auto* section = config::ResolveSectionTable(table, var->category());
    if (!section) {
      throw std::invalid_argument("Invalid configuration section");
    }
    if (!value_text) {
      section->erase(name);
    } else {
      auto value = Text(env, value_text);
      auto enums = xe::ui::GetKnownEnumOptions().find(name);
      if (enums != xe::ui::GetKnownEnumOptions().end() &&
          std::find(enums->second.begin(), enums->second.end(), value) ==
              enums->second.end()) {
        throw std::invalid_argument("Choose one of the available values");
      }
      value = xe::ui::DisplayNameToIntCvarValue(name, value);
      const auto type = Type(var);
      if (type == "string" || type == "path") {
        section->insert_or_assign(name, value);
      } else {
        auto parsed = toml::parse("value = " + value);
        const auto* node = parsed["value"].node();
        if (parsed.size() != 1 || !node) {
          throw std::invalid_argument("Invalid value");
        }
        bool valid = type == "bool" && node->is_boolean();
        if (type == "integer" && node->is_integer()) {
          int64_t v = *node->value<int64_t>();
          valid = IntegerFits<int32_t>(var, v) ||
                  IntegerFits<uint32_t>(var, v) ||
                  IntegerFits<int64_t>(var, v) || IntegerFits<uint64_t>(var, v);
        }
        if (type == "double") {
          auto v = node->value<double>();
          valid = v && std::isfinite(*v);
          if (valid) {
            parsed.insert_or_assign("value", *v);
          }
          node = parsed["value"].node();
        }
        if (!valid) {
          throw std::invalid_argument(
              "Invalid value or number outside its range");
        }
        section->insert_or_assign(name, *node);
      }
    }
    std::ostringstream out;
    out << table;
    return JavaText(env, out.str());
  } catch (const std::exception& e) {
    Error(env, e);
    return nullptr;
  }
}
}
