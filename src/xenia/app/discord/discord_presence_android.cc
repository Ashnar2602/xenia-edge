/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#define DISCORDPP_IMPLEMENTATION
#include "discordpp.h"

#include <jni.h>
#include <ctime>
#include <memory>

#include "xenia/app/discord/discord_presence.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"

DEFINE_bool(discord, true, "Enable Discord rich presence", "General");

namespace xe::discord {
namespace {
// All access, including SDK callbacks, is confined to the Android UI thread.
std::unique_ptr<discordpp::Client> client;
}  // namespace

bool DiscordPresence::initialized_ = false;

void DiscordPresence::Initialize() {
  if (initialized_) {
    return;
  }
  client = std::make_unique<discordpp::Client>();
  client->SetApplicationId(1425285186387578910ULL);
  client->SetEngineManagedAudioSession(true);
  initialized_ = true;
}

void DiscordPresence::PlayingTitle(std::string_view title) {
  if (!client) {
    return;
  }
  discordpp::Activity activity;
  activity.SetType(discordpp::ActivityTypes::Playing);
  activity.SetDetails(std::string(title));
  activity.SetState("In Game");
  discordpp::ActivityTimestamps timestamps;
  timestamps.SetStart(std::time(nullptr));
  activity.SetTimestamps(timestamps);
  discordpp::ActivityAssets assets;
  assets.SetLargeImage("app");
  assets.SetLargeText("Xenia Edge - Experimental Testing branch");
  activity.SetAssets(assets);
  client->UpdateRichPresence(activity, [](discordpp::ClientResult result) {
    if (!result.Successful()) {
      XELOGW("Discord presence update failed");
    }
  });
}

void DiscordPresence::Shutdown() {
  if (client) {
    client->ClearRichPresence();
    discordpp::RunCallbacks();
    client.reset();
  }
  initialized_ = false;
}
}  // namespace xe::discord

extern "C" {
JNIEXPORT jboolean JNICALL
Java_jp_xenia_emulator_DiscordBridge_enabledNative(JNIEnv*, jclass) {
  return cvars::discord;
}

JNIEXPORT void JNICALL Java_jp_xenia_emulator_DiscordBridge_updateNative(
    JNIEnv* env, jclass, jstring title) {
  if (!title || !cvars::discord) {
    return;
  }
  const auto* chars = env->GetStringChars(title, nullptr);
  if (!chars) {
    return;
  }
  auto text = xe::to_utf8(std::u16string_view(
      reinterpret_cast<const char16_t*>(chars), env->GetStringLength(title)));
  env->ReleaseStringChars(title, chars);
  xe::discord::DiscordPresence::Initialize();
  xe::discord::DiscordPresence::PlayingTitle(text);
}

JNIEXPORT void JNICALL Java_jp_xenia_emulator_DiscordBridge_pumpNative(JNIEnv*,
                                                                       jclass) {
  discordpp::RunCallbacks();
}

JNIEXPORT void JNICALL
Java_jp_xenia_emulator_DiscordBridge_closeNative(JNIEnv*, jclass) {
  xe::discord::DiscordPresence::Shutdown();
}
}  // extern "C"
