# WrathFall

Windows x64 static recompilation of title `43430817`, media ID `11DFADD6`, using ReXGlue 0.11.0. TU1 changes the executable from `0.0.0.5` to `0.0.1.5`. The build and the runtime load the same patched image.

## Build the launcher

This repository contains source. Install Visual Studio 2022 Build Tools with the Desktop development with C++ workload, x64 tools, a Windows SDK, and the CMake tools. Place the Windows x64 ReXGlue 0.11.0 package at `.tools/rexglue-0.11.0/win-amd64/`, matching SDK sources at `.tools/rexglue-sdk-0.11.0/`, and LLVM 20.1.8 at `.tools/llvm-20.1.8/`. Run `.\host\launcher\Build-Launcher.ps1` to create `WrathFall.exe`.

For optional Direct3D 12 upscaling, see [renderer build instructions](host/renderer_trace/README.md). Vulkan runs the game at its native internal resolution. Vulkan DLSS and FSR 3 are under development.

## Open the launcher

Open **WrathFall.exe** from the project folder. The launcher opens on the display under your mouse pointer. In **Game files**, select your Xbox 360 `.iso` or single-ISO `.7z`, add the matching TU1, and select any DLC ZIPs or unpacked Xbox content packages. Open the **Play** page and choose **Build** to install. Once the status reads **Installed**, choose **Play**. Progress appears under **Activity** on the Play screen and in `out/logs/`.

On first play, follow the game’s own save prompts. On the Xbox 360 system prompt, use the D-pad to select a choice, A to confirm, or B to go back. The launcher keeps your profile and saves in `userdata/` at the project root. Existing launcher profiles from `out/user/` are copied and verified when setting up this folder. Keep `userdata/` when rebuilding or updating the project.

| Manual input | Place it here |
| --- | --- |
| Extracted game tree with `default.xex` and `BCGame/` | `local/game/` |
| Matching TU1 `default.xexp` | `local/update/` |
| Raw DLC packages with their original 42-character Xbox filenames | `local/dlc/` |

The build stages a verified TU1 copy beside `default.xex`. On launch, ReXGlue registers the DLC packages attached to this title and mounts each package from its local file when the game opens it.

The game runs offline. Xbox 360 socket requests return a network-down error before a Windows socket is opened. Local saves, achievements, and imported DLC are available offline. Windows Firewall access is unnecessary for normal play.

## Graphics and shader cache

The **Graphics** panel offers windowed, borderless windowed, and exclusive fullscreen. **Output resolution** lists supported resolutions detected on connected displays, plus the original 1280 × 720. With the optional temporal runtime installed, 1440p uses DLSS or FSR 3 to upscale from the original 720p. It also offers 60, 120, and 144 FPS targets as experimental options. Settings are saved when you click **Play** or close the launcher. **Reset graphics** restores the default selections.

**Precompile Shaders** prepares the selected renderer's captured pipelines in a separate run without entering gameplay. The button becomes available once the renderer has recorded pipelines under `out/cache/`. Select **Recompile Shaders** to warm the recorded pipelines again after a driver update.

Cache coverage grows as the renderer encounters new draws. Full precompilation requires a verified corpus covering the base game, DLC, menus, and graphics variants, plus a persistent native pipeline cache. Play remains available with partial coverage. New pipelines compile synchronously to preserve their draws.

## Record a gameplay issue

The **Gameplay collector** checkbox next to **Play** controls whether capture starts with the game. It starts checked and remembers your choice. When it’s on, the launcher shows **Collector active (L to mark)** once capture is ready. Press **L** in the game to record an issue. The collector saves nearby game-window frames and recent game logs locally under `out/diagnostics/`. Collector activity is logged under `out/logs/gameplay-collector-*.log`. The collector closes after the game exits, including when the game crashes.

The collector also saves **possible-frame-stutter** and **possible-fps-drop** reports automatically. It compares the game window’s compositor frame timestamps with its recent cadence while the game is in focus. Open `frame-times.csv` for the intervals before and after the mark. The timing is for the frames Windows captures from the game window. Loading transitions and changes in a cutscene’s intended frame rate can also produce reports.

When DLSS or FSR 3 unexpectedly falls back during a 3D scene, the collector groups the affected renderer frames in an **upscaler-fallback** report. Open `event.txt` for the first and last game frame, reason, and screenshot timestamp. Open `upscaler-details.tsv` for the associated draw and motion shader IDs. Runtime startup failures may have no shader IDs. The nearest available game-window image is saved as `screenshot.png`. The process-specific raw event log is under `out/logs/upscaler-*.tsv`. Routine menu and loading-card presentation changes are excluded. Use **L** for visible issues such as ghosting or trails.

## Build from PowerShell

```powershell
.\Build.ps1 -Stage Build
.\Build.ps1 -Stage PrepareShaders -Renderer d3d12
.\Build.ps1 -Stage Run -FrameRate 60 -DisplayMode borderless
```

The project uses the pinned SDK and portable Clang 20.1.8 in `.tools/`, along with the installed x64 Visual Studio Build Tools, the Windows SDK, CMake, and Ninja. The custom prompt builds against the matching SDK sources under `.tools/rexglue-sdk-0.11.0/`. Run `Build.ps1 -Stage Codegen` to regenerate the C++ explicitly. Generated sources, game inputs, user data, and build outputs are git-ignored.

See [Xbox 360 accuracy](ACCURACY.md) for the memory check and the remaining game-behavior comparisons.

[ReXGlue 0.11.0](https://github.com/rexglue/rexglue-sdk/releases/tag/v0.11.0) · [ReXGlue shader and storage documentation](https://docs.rexglue.com/filesystem/folders-and-storage/)
