#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include <rex/app/title_app.h>
#include <rex/filesystem/file_system.h>
#include <rex/gpu/graphics_system.h>
#include <rex/kernel/title_paths.h>
#include <rex/kernel/title_runtime.h>
#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>
#include <rex/platform/process.h>
#include <rex/ppc/hook.h>
#include <rex/xam/content_manager.h>

#ifdef _WIN32
#include <Windows.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <rex/ui/ui_system.h>
#include "wrathfall_icon.h"
#endif

#include "generated/default/asuras_wrath_init.h"

void ResetXamWorkspaceTracking();

namespace {

bool HasEnvironmentFlag(const char* name) {
  return rex::platform::GetEnvironmentValue(name) == "1";
}

#ifdef _WIN32
int FindDisplayIndex(const std::string& requested) {
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return 1;
  int count = 0;
  auto* displays = SDL_GetDisplays(&count);
  int selected = 0;
  // Match Windows' device name to the monitor behind the SDL display ID.
  for (int i = 0; displays && i < std::min(count, 16); ++i) {
    const auto properties = SDL_GetDisplayProperties(displays[i]);
    auto handle = static_cast<HMONITOR>(
        SDL_GetPointerProperty(properties, SDL_PROP_DISPLAY_WINDOWS_HMONITOR_POINTER, nullptr));
    MONITORINFOEXA info{};
    info.cbSize = sizeof(info);
    if (handle && GetMonitorInfoA(handle, reinterpret_cast<MONITORINFO*>(&info)) &&
        _stricmp(info.szDevice, requested.c_str()) == 0) {
      selected = i + 1;
      break;
    }
  }
  SDL_free(displays);
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
  if (selected) {
    REXLOG_INFO("Selected Windows display {} at SDL index {}.", requested, selected);
  } else {
    REXLOG_WARN("Windows display {} unavailable, using the primary display.", requested);
  }
  return selected ? selected : 1;
}
#endif

rex::Result<void> MountGraphicsOverride(rex::kernel::TitleRuntime& title) {
  const auto setting = rex::platform::GetEnvironmentValue("ASURAS_WRATH_GRAPHICS_OVERRIDE");
  if (!setting || setting->empty()) return rex::Ok();

  const auto directory = std::filesystem::u8path(*setting);
  auto& files = title.GetFileSystem();
  const auto game_mount = files.FindLink("game:");
  if (!game_mount) {
    return rex::Err(rex::ErrorCategory::kValidation, "The game files are not mounted.");
  }
  constexpr std::string_view override_mount = "\\Device\\AsuraGraphics";
  if (auto mounted = files.Mount({std::string(override_mount),
                                 rex::filesystem::ProviderType::kHostDirectory,
                                 directory, rex::filesystem::MountFlags::kNone, {}});
      !mounted) {
    return mounted;
  }
  const auto cooked = title.GetTitlePaths().game_root.Get() / "BCGame" / "CookedXbox360";
  for (const auto& entry : std::filesystem::directory_iterator(cooked)) {
    const auto name = entry.path().filename().string();
    if (!name.starts_with("Coalesced_") || !name.ends_with(".bin")) continue;
    if (!std::filesystem::is_regular_file(directory / entry.path().filename())) {
      return rex::Err(rex::ErrorCategory::kValidation, "Missing graphics override for " + name);
    }
    const auto source = *game_mount + "\\BCGame\\CookedXbox360\\" + name;
    const auto target = std::string(override_mount) + "\\" + name;
    if (auto link = files.Link(source, target); !link) return link;
  }
  REXLOG_INFO("Motion blur is disabled by the launcher.");
  return rex::Ok();
}

// ReXGlue 0.11 reports its cache completion from the GPU worker.
class ShaderPreparationLog final : public rex::log::ILogSink {
 public:
  void Write(const rex::log::Message& message) override {
    if (message.category != "gpu") return;
    if (message.level >= rex::log::Level::kError) {
      failed = true;
      return;
    }
    auto text = message.text;
    if (!text.starts_with("Created ") ||
        !(text.contains("pipelines (not including reading the descriptions) from the storage") ||
          text.contains("pipelines from Vulkan storage"))) {
      return;
    }
    text.remove_prefix(8);
    unsigned created = 0;
    const auto count = std::from_chars(text.data(), text.data() + text.size(), created);
    if (count.ec == std::errc{} && created > 0) {
      pipelines = created;
    }
  }

  void Flush() override {}

  std::atomic<unsigned> pipelines{};
  std::atomic<bool> failed{};
};

}  // namespace

// The graphics device attaches in TitleRuntime::Launch. Skip the game's entry
// point during cache preparation so no gameplay or title save code runs.
REX_HOOK_RAW(xstart) {
  if (HasEnvironmentFlag("ASURAS_WRATH_PREPARE_SHADERS")) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__xstart(ctx, base);
}

class AsurasWrathApp final : public rex::app::TitleApp {
 public:
  AsurasWrathApp() : TitleApp("asuras_wrath", kPPCImageConfig) {}

 protected:
  rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override {
    if (auto configured = TitleApp::OnConfigure(settings); !configured) return configured;
#ifdef _WIN32
    const auto monitor = rex::platform::GetEnvironmentValue("ASURAS_WRATH_DISPLAY_MONITOR");
    if (monitor && !monitor->empty()) {
      auto result = settings.SetText(rex::SettingLayer::kCommandLine, "ui.monitor",
                                     std::to_string(FindDisplayIndex(*monitor)));
      if (!result) return result;
    }
#endif
    return settings.SetText(rex::SettingLayer::kTitle, "ui.title", "WrathFall");
  }

  rex::AppSysRequirements GetRequirements() const override {
    auto requirements = TitleApp::GetRequirements();
    if (HasEnvironmentFlag("ASURAS_WRATH_PREPARE_SHADERS")) requirements.ui = false;
    return requirements;
  }

  void OnRun() override {
#ifdef _WIN32
    if (const auto window = GetUI().GetNativeWindow();
        window && window->type == rex::NativeWindow::Type::kWin32 && window->window) {
      const auto set_icon = [&](WPARAM type, int width, int height) {
        const auto icon = LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(WRATHFALL_ICON),
                                     IMAGE_ICON, GetSystemMetrics(width), GetSystemMetrics(height),
                                     LR_SHARED);
        if (icon) SendMessageW(static_cast<HWND>(window->window), WM_SETICON, type,
                               reinterpret_cast<LPARAM>(icon));
      };
      set_icon(ICON_BIG, SM_CXICON, SM_CYICON);
      set_icon(ICON_SMALL, SM_CXSMICON, SM_CYSMICON);
    }
#endif
    if (!HasEnvironmentFlag("ASURAS_WRATH_MEASURE_FPS")) {
      TitleApp::OnRun();
      return;
    }
    using Clock = std::chrono::steady_clock;
    auto next = Clock::now();
    while (RunFrame()) {
      const auto now = Clock::now();
      if (now >= next) {
        const auto stats = GetGraphics().GetFrameStats();
        REXLOG_INFO("Frame sample: {:.1f} FPS, {:.2f} ms, {} frames",
                    stats.fps, stats.frame_time_ms, stats.frame_count);
        next = now + std::chrono::seconds(1);
      }
    }
  }

  rex::Result<void> OnTitleLaunch(rex::kernel::TitleRuntime& title) override {
    ResetXamWorkspaceTracking();
    if (auto graphics = MountGraphicsOverride(title); !graphics) return graphics;
    const auto dlc = title.GetTitlePaths().game_root.Get().parent_path() / "dlc";
    if (!std::filesystem::exists(dlc)) {
      return rex::Ok();
    }

    auto& content = title.GetContentManager();
    const auto installed = content.ListInstalledPackages(title.GetTitleID());
    for (const auto& entry : std::filesystem::directory_iterator(dlc)) {
      const std::string name = entry.path().filename().string();
      if (!entry.is_regular_file() || name.size() != 42 ||
          name.find_first_not_of("0123456789ABCDEF") != std::string::npos) {
        continue;
      }

      const auto package = content.InspectPackage(entry.path());
      if (!package) {
        return rex::Err(package.error());
      }
      if (package->title_id != title.GetTitleID() ||
          package->content_type != rex::kernel::XContentType::kMarketplaceContent) {
        return rex::Err(rex::ErrorCategory::kValidation, "Unexpected DLC package: " + name);
      }

      if (std::ranges::any_of(installed, [&](const auto& item) {
            return item.in_place && item.data.GetFileName() == name &&
                   item.info.content_id == package->content_id;
          })) {
        continue;
      }

      const auto result = content.InstallContent(entry.path(), {.extract = false});
      if (!result) {
        return rex::Err(result.error());
      }
      REXLOG_INFO("Registered Asura's Wrath DLC package {}", name);
    }
    return rex::Ok();
  }
};

static int RunAsurasWrath(AsurasWrathApp& app, const rex::CLIArgList& args) {
  if (!app.Init(args)) return app.Shutdown();
  if (!HasEnvironmentFlag("ASURAS_WRATH_PREPARE_SHADERS")) {
    if (app.LaunchTitle()) app.Run();
    return app.Shutdown();
  }

  ShaderPreparationLog preparation;
  const auto guard = REX_LOGGER().AddSink(preparation);
  auto& title = app.GetTitleRuntime();
  if (const auto loaded = title.Load(); !loaded) {
    REXLOG_ERROR("Shader preparation could not load the game image: {}", loaded.error().message);
    app.Shutdown();
    return 1;
  }
  if (const auto launched = title.Launch(); !launched) {
    REXLOG_ERROR("Shader preparation could not attach the graphics device: {}", launched.error().message);
    app.Shutdown();
    return 1;
  }

  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::seconds(60);
  while (!preparation.pipelines && !preparation.failed && Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  const unsigned count = preparation.pipelines;
  const bool success = count && !preparation.failed;
  if (success) {
    REXLOG_INFO("Prepared {} recorded graphics pipelines without starting the game.", count);
  } else {
    REXLOG_ERROR("Shader preparation failed or found no compatible recorded graphics pipelines.");
  }
  title.Unload();
  const int shutdown = app.Shutdown();
  return success ? shutdown : 1;
}

REX_TITLE_APP_MAIN(AsurasWrathApp, RunAsurasWrath)
