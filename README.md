# WrathFall

WrathFall is a native PC recompilation of the Xbox 360 version of **Asura's Wrath** for 64-bit Windows. Bring your own game disc image and matching title update. You can also import DLC from your own copies.

## Install and play

For the first build, install Visual Studio Build Tools with the **Desktop development with C++** workload, a Windows SDK, and CMake and Ninja. The launcher uses these to build the PC executable from your game files. [Visual Studio Build Tools download](https://visualstudio.microsoft.com/downloads/)

1. Extract the full WrathFall download into a folder. Keep the bundled folders beside `WrathFall.exe`, then open the launcher.
2. Open **Game files**. Select **Choose disc…** and pick your Xbox 360 `.iso` or a `.7z` containing a single ISO.
3. Select **Choose TU1…** and pick the matching Title Update 1 package for media ID `11DFADD6`. You can use a TU1 ZIP, a raw Xbox 360 title update package, or the extracted `default.xexp`.
4. To install optional DLC, choose **Add DLC…**. You can select multiple ZIPs or raw Xbox 360 content packages. Leave raw package filenames as they came from the console.
5. Open **Play** and choose **Build**. When the button reads **Installed**, choose **Play**. Build progress and errors appear under **Activity**.

Follow the game's save prompts on first launch. In an Xbox 360 system prompt, use the D-pad to move, **A** to confirm, and **B** to go back.

Play, saves, achievements, and imported DLC work offline. The game can open imported episodes and packs, including Episode 11.5, Episode 15.5, Part IV, Lost Episode 1, and Lost Episode 2, when you supply their packages.

## Saves and manual file setup

Keep `userdata/` when updating or rebuilding WrathFall. If the launcher finds an older profile in `out/user/`, it copies and verifies that profile when creating `userdata/`.

If you already extracted the Xbox files, place them in the folders below. Use paths relative to `engine/` if your download has that folder. Otherwise, use the project folder beside `WrathFall.exe`.

| File | Destination |
| --- | --- |
| Extracted game tree, including `default.xex` and `BCGame/` | `local/game/` |
| Matching TU1 patch named `default.xexp` | `local/update/` |
| Raw DLC packages with their original 42-character Xbox filenames | `local/dlc/` |

WrathFall verifies and stages the TU1 patch beside `default.xex` before building or running the game.

## Graphics and shaders

- **Display.** In **Graphics**, choose your monitor and use windowed, borderless windowed, or exclusive fullscreen. Borderless follows the selected monitor's desktop resolution. The resolution picker also offers the original 1280 × 720.
- **Upscaling.** With the optional runtime installed, Direct3D 12 supports DLSS or FSR 3 from the original 720p to 2560 × 1440. Choose **Upscaler**, then select a model preset for DLSS. Use the **NVIDIA DLSS setup** card to import the Windows x64 Streamline 2.14.16 ZIP. **Vulkan (experimental upscaling)** is available when its runtime is installed. **DLSS NR** previews settings for a future Neural Rendering integration.
- **Shader preparation.** After the renderer has recorded shaders during play, use **Precompile Shaders** on the **Play** page to prepare them. The process runs without entering gameplay. Use **Recompile Shaders** to refresh the cache after a driver update. Play is available while the cache is incomplete. New shaders may compile the first time you visit a scene.

Graphics settings are saved when you choose **Play** or close the launcher. If setup fails, check **Activity** on the **Play** page or open **Logs** from the sidebar.

## Developer notes

The target is Xbox 360 title ID `43430817` and media ID `11DFADD6`. TU1 updates the executable from `0.0.0.5` to `0.0.1.5`. The build and runtime use the same patched image. ReXGlue registers the DLC and mounts the local package when the game opens it.

The build uses the pinned ReXGlue SDK 0.11.0 and portable Clang 20.1.8 in `.tools/`. The custom Xbox prompt builds against the matching SDK sources in `.tools/rexglue-sdk-0.11.0/`. Generated code, imported game files, user data, and build output are git-ignored.

From the source directory, use Command Prompt to compile the launcher, build the PC version, or regenerate the C++.

```batch
host\launcher\Build-Launcher.cmd
start "" /wait WrathFall.exe --build
start "" /wait WrathFall.exe --codegen
```

- See the verified memory checks and the remaining gameplay comparisons. [Xbox 360 accuracy](ACCURACY.md)
- Build or run the standalone graphics checks. [Direct3D 12 renderer](host/renderer_trace/README.md) · [Vulkan renderer](host/renderer_trace/vulkan/README.md)
- Find the pinned SDK and its storage documentation. [ReXGlue 0.11.0](https://github.com/rexglue/rexglue-sdk/releases/tag/v0.11.0) · [Shader and storage documentation](https://docs.rexglue.com/filesystem/folders-and-storage/)
