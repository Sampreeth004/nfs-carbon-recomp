// nfscarbon - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>

#include <filesystem>

class NfsCarbonApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<NfsCarbonApp>(new NfsCarbonApp(ctx, "nfscarbon",
        PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;
    }
    const std::filesystem::path candidates[] = {
        rex::filesystem::GetExecutableFolder() / "game",
        std::filesystem::current_path() / "game",
    };
    for (const auto& candidate : candidates) {
      std::error_code ec;
      if (std::filesystem::is_directory(candidate, ec)) {
        paths.game_data_root = candidate;
        return;
      }
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    config.gpu_plugin = "xenos";
  }

  void OnPostSetup() override {
    rex::cvar::SetFlagByName("gpu_allow_invalid_fetch_constants", "true");
  }

  void OnPostLoadXexImage() override {
    static bool dumped = false;
    if (dumped) {
      return;
    }
    dumped = true;
    uint8_t* membase = runtime()->memory()->virtual_membase();
    std::FILE* f = std::fopen("guest_image.bin", "wb");
    if (f) {
      std::fwrite(membase + REX_IMAGE_BASE, 1, REX_IMAGE_SIZE, f);
      std::fclose(f);
    }
  }
};
