// Mass Effect for Nintendo Switch: the ReXGlue application object (paths, GPU plugin choice, early configuration).

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/rex_app.h>

#if REX_PLATFORM_SWITCH
#if __has_include(<rex/ui/switch_apm.h>)
#include <rex/ui/switch_apm.h>
#else
extern "C" {
void RexSwitchApmRequestGpuMhz(int mhz);
void RexSwitchApmAllowRam1600(int allow);
void RexSwitchApmApply(void);
void RexSwitchApmCpuBoost(int mode);
}
#endif
#endif

#include "me_packaged.h"
#include "native/me_native_system.h"

extern "C" bool g_me_lockfree_atomics;  // src/native/me_ring_wait.cpp: guest atomics without the global lock

// src/me_usb_files.cpp: developer USB file channel, nothing unless dev_usb_files = true (docs/usb-files.md).
namespace me::usb_files {
void Start();
}  // namespace me::usb_files

class MassEffectApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<MassEffectApp>(new MassEffectApp(ctx, "masseffect",
        PPCImageConfig));
  }

  void OnPostInitLogging() override {
    me::packaged::LogStatus();
    me::usb_files::Start();  // developer USB file channel, off unless dev_usb_files = true
    // The runtime defaults to no GPU plugin: select ReXGlue's Xenos implementation (the native renderer replaces
    // it in OnPreSetup), preserving an explicit command-line or config-file choice.
    if (!rex::cvar::HasNonDefaultValue("gpu_plugin")) {
      rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
  }

  // The Switch has no command line: when --game_data_root is not given, use a
  // game_root/ folder next to the executable (sdmc:/switch/masseffect-nx/game_root
  // on the SD card). An explicit path always wins.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    // Installed full NSP: game data, shaders and toml come from the RomFS (no-op for the NRO; me_packaged.h).
    me::packaged::ConfigurePaths(paths);
    if (paths.game_data_root.empty()) {
      std::error_code ec;
      const auto folder = rex::filesystem::GetExecutableFolder();
      const auto extracted = folder / "game_root";
      if (!folder.empty() && std::filesystem::is_directory(extracted, ec)) {
        paths.game_data_root = extracted;
      }
    }
    me::packaged::ReportPaths(paths);  // one line in rex_stderr.log: what was chosen and why
  }

  // Analysis aid: with MASSEFFECT_DUMP_IMAGE=<file>, write the loaded guest image
  // (image_base..image_base+image_size) so code pointers stored in data can be scanned for.
  // Does nothing unless the variable is set.
  void OnPostLoadXexImage() override {
    me::native::StartHangWatchdog();  // diagnostics, off unless masseffect_hang_watchdog = true
    if (!runtime() || !runtime()->memory()) {
      return;
    }
    PatchCoalescedHashForLocalTesting();
    const char* path = std::getenv("MASSEFFECT_DUMP_IMAGE");
    if (!path) {
      return;
    }
    const uint8_t* image =
        runtime()->memory()->TranslateVirtual<const uint8_t*>(PPCImageConfig.image_base);
    if (std::FILE* f = std::fopen(path, "wb")) {
      std::fwrite(image, 1, PPCImageConfig.image_size, f);
      std::fclose(f);
    }
  }

  // Testing aid for a modified Coalesced.ini (e.g. without the startup movies): the game
  // verifies it against a SHA-1 table in the image (FSHAVerify, "dirty disc" on mismatch).
  // With the masseffect_coalesced_sha1 cvar (or MASSEFFECT_COALESCED_SHA1) = 40 hex digits,
  // replace the stored hash of the original file, but only if the expected original bytes are
  // present at the known location. Does nothing when unset.
  void PatchCoalescedHashForLocalTesting() {
    const char* hex = std::getenv("MASSEFFECT_COALESCED_SHA1");
    const std::string from_cvar = rex::cvar::GetFlagByName("masseffect_coalesced_sha1");
    if ((!hex || !*hex) && from_cvar.size() == 40) hex = from_cvar.c_str();
    if (!hex || std::strlen(hex) != 40) {
      return;
    }
    static constexpr uint32_t kHashAddress = 0x82F80B1C;
    static constexpr uint8_t kOriginal[20] = {0xB9, 0x1B, 0x8B, 0x7D, 0x73, 0x9A, 0x6A,
                                              0x2F, 0xE3, 0xB0, 0x74, 0x56, 0xE9, 0x08,
                                              0x23, 0xBA, 0x5F, 0x23, 0x05, 0xCC};
    uint8_t* stored = runtime()->memory()->TranslateVirtual<uint8_t*>(kHashAddress);
    if (std::memcmp(stored, kOriginal, sizeof(kOriginal)) != 0) {
      REXLOG_WARN("Coalesced.ini hash override skipped: unexpected bytes at {:08X}",
                  kHashAddress);
      return;
    }
    for (int i = 0; i < 20; ++i) {
      unsigned value = 0;
      std::sscanf(hex + 2 * i, "%2x", &value);
      stored[i] = uint8_t(value);
    }
    REXLOG_INFO("Coalesced.ini hash overridden for local map testing");
  }

  // Early configuration: environment for Mesa/NVK (read when the Vulkan device is created), CPU/GPU clocks, and the
  // app's own graphics system, which replaces ReXGlue's Xenos emulation plugin (cvar masseffect_renderer_native).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    g_me_lockfree_atomics = rex::cvar::GetFlagByName("masseffect_lockfree_atomics") == "true";
    // Cold-start test: Mesa reads this when the Vulkan device is created (after this point).
    if (rex::cvar::GetFlagByName("masseffect_cold_startup") == "true") setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
    {
      std::string tile = rex::cvar::GetFlagByName("masseffect_diag_trace_tile");
      std::erase(tile, '"');  // string cvars come back quoted
      if (!tile.empty()) setenv("MASSEFFECT_EDRAM_TRACE_TILE", tile.c_str(), 1);
    }
    if (rex::cvar::GetFlagByName("masseffect_nvk_cache_per_stage") == "true")
      setenv("NVK_SWITCH_STAGE_CACHE", "true", 1);
#if REX_PLATFORM_SWITCH
    // Official Nintendo 460.8 MHz handheld GPU profile (+50% GPU fillrate); CPU boost mode from masseffect_cpu_boost.
    RexSwitchApmRequestGpuMhz(460);
    RexSwitchApmApply();
    RexSwitchApmCpuBoost(int(rex::cvar::GetFlagByName("masseffect_cpu_boost") == "1") +
                         int(rex::cvar::GetFlagByName("masseffect_cpu_boost") == "2") * 2);
#endif
    if (me::native::Enabled()) {
      config.graphics = me::native::CreateGraphicsSystem();
    }
  }
};
