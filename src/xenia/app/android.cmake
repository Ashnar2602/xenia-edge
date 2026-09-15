# Android uses the existing Java/JNI window implementation, not wxWidgets.
add_library(xenia-app SHARED xenia_main_android.cc game_icon.cc game_settings_android.cc
  features_android.cc game_compat_db.cc game_title_db.cc game_library.cc
  ../gpu/vulkan/vulkan_trace_viewer_main.cc)
target_include_directories(xenia-app PRIVATE "${PROJECT_SOURCE_DIR}/third_party/rapidjson/include"
  "${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include")
xe_embed_compressed_bundle(xenia-app "${PROJECT_SOURCE_DIR}/build/data_repos/game-compatibility" game_compat)
xe_embed_compressed_bundle(xenia-app "${PROJECT_SOURCE_DIR}/build/data_repos/x360db/games.json" game_db)
target_sources(xenia-app PRIVATE $<$<CONFIG:Debug>:android_input_test.cc>)
set_target_properties(xenia-app PROPERTIES OUTPUT_NAME xenia-app)
target_link_options(xenia-app PRIVATE "-Wl,--no-undefined")
target_link_libraries(xenia-app PRIVATE
  "-Wl,--start-group"
  xenia-core xenia-base xenia-cpu xenia-cpu-backend-a64
  xenia-apu xenia-apu-sdl xenia-gpu xenia-gpu-vulkan xenia-gpu-null
  xenia-hid xenia-hid-nop xenia-hid-keyboard xenia-hid-portal xenia-kernel xenia-patcher
  xenia-ui xenia-ui-vulkan xenia-vfs
  aes_128 capstone fmt glslang-spirv imgui libavcodec libavutil
  mspack snappy xxhash
  "-Wl,--end-group")
xe_target_defaults(xenia-app)

option(XENIA_ANDROID_DISCORD "Use the official Android Discord Social SDK" OFF)
if(XENIA_ANDROID_DISCORD)
  find_package(discord_partner_sdk REQUIRED CONFIG)
  target_sources(xenia-app PRIVATE discord/discord_presence_android.cc)
  target_link_libraries(xenia-app PRIVATE discord_partner_sdk::discord_partner_sdk)
endif()
