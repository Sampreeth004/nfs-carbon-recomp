#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#if defined(__ANDROID__)
#include "android_aaudio.h"
#include "thread_affinity.h"

extern "C" const char* SDL_GetAndroidInternalStoragePath(void);
extern "C" const char* SDL_GetAndroidExternalStoragePath(void);
#endif

class NfsCarbonApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<NfsCarbonApp>(new NfsCarbonApp(ctx, "nfscarbon",
        PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
#if defined(__ANDROID__)
    const char* internal = SDL_GetAndroidInternalStoragePath();
    const char* external = SDL_GetAndroidExternalStoragePath();
    const std::filesystem::path internal_dir = internal ? internal : "";
    const std::filesystem::path external_dir = external ? external : "";
    // The default user folder resolves to "/data/.local" on Android, which is
    // not writable; everything persistent goes under the app's files dir.
    if (!internal_dir.empty()) {
      user_data_root_ = internal_dir;
      paths.user_data_root = internal_dir;
      paths.cache_root = internal_dir / "cache";
      paths.config_path = internal_dir / "nfscarbon.toml";
    }
    if (paths.game_data_root.empty()) {
      const std::filesystem::path candidates[] = {
          external_dir / "game",
          internal_dir / "game",
      };
      for (const auto& candidate : candidates) {
        std::error_code ec;
        if (!candidate.empty() && std::filesystem::is_directory(candidate / "NFS", ec)) {
          paths.game_data_root = candidate;
          break;
        }
      }
    }
#else
    if (!paths.game_data_root.empty()) {
      return;
    }
    const std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
    const std::filesystem::path candidates[] = {
        exe_dir / "game",
        std::filesystem::current_path() / "game",
        (exe_dir / ".." / ".." / ".." / "game").lexically_normal(),
    };
    for (const auto& candidate : candidates) {
      std::error_code ec;
      if (std::filesystem::is_directory(candidate / "NFS", ec)) {
        paths.game_data_root = candidate;
        return;
      }
    }
#endif
  }

  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults,
      std::function<void(rex::PathConfig)> resume) override {
    (void)resume;
#if defined(__ANDROID__)
    // SetupEnvironment loads the TOML only after OnConfigurePaths has already
    // built the path defaults, so the game_data_root written by the launcher
    // must be applied here, once the config is live and before the runtime is
    // constructed.
    rex::PathConfig paths = defaults;
    std::string configured_root = rex::cvar::GetFlagByName("game_data_root");
    if (!configured_root.empty()) {
      paths.game_data_root = configured_root;
    }
    return paths;
#else
    return defaults;
#endif
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // "carbon" is the native renderer (renderer/); "xenos" the SDK's Xenos
    // emulation, kept as a fallback selectable with gpu_plugin in the TOML
    // (the Android launcher writes it from Settings > Renderer).
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "carbon";
    }

#if defined(__ANDROID__)
    config.audio_factory = REX_AUDIO_BACKEND(carbon::audio::AAudioSystem);
#endif

#ifndef __ANDROID__
    // On Android the launcher writes these keys to the TOML config, and
    // OnPreSetup runs before LoadConfig, so setting defaults here would win
    // with Source::kRuntime and silently override the launcher.
    auto set_default = [](const char* name, const char* value) {
      if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) {
        rex::cvar::SetFlagByName(name, value);
      }
    };
    set_default("input_backend", "sdl");
    set_default("mnk_mode", "true");
    set_default("mnk_mouse", "true");
    set_default("mnk_sensitivity", "1.5");
    set_default("keybind_a", "Space");
    set_default("keybind_b", "F");
    set_default("keybind_x", "LMB");
    set_default("keybind_y", "E");
    set_default("keybind_left_shoulder", "Q");
    set_default("keybind_right_shoulder", "RMB");
    set_default("keybind_left_trigger", "Shift");
    set_default("keybind_right_trigger", "Control");
    set_default("keybind_lstick_up", "W");
    set_default("keybind_lstick_down", "S");
    set_default("keybind_lstick_left", "A");
    set_default("keybind_lstick_right", "D");
    set_default("keybind_lstick_press", "X");
    set_default("keybind_rstick_up", "Up");
    set_default("keybind_rstick_down", "Down");
    set_default("keybind_rstick_left", "Left");
    set_default("keybind_rstick_right", "Right");
    set_default("keybind_rstick_press", "R");
    set_default("keybind_dpad_up", "Shift+Up");
    set_default("keybind_dpad_down", "Shift+Down");
    set_default("keybind_dpad_left", "Shift+Left");
    set_default("keybind_dpad_right", "Shift+Right");
    set_default("keybind_back", "Tab");
    set_default("keybind_start", "Escape");
#endif
  }

  void OnPostSetup() override {
    rex::cvar::SetFlagByName("gpu_allow_invalid_fetch_constants", "true");
#if defined(__ANDROID__)
    // Guest threads keep being created after this; the watchdog rescans.
    thread_affinity_ = nfscarbon::afinidad::Arrancar();
#endif
  }

 private:
  std::filesystem::path user_data_root_;
#if defined(__ANDROID__)
  nfscarbon::afinidad::VigilantePtr thread_affinity_;
#endif
};
