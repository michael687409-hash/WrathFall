# Direct3D 12 temporal upscaling development

This optional build contains the ReXGlue Xenos resolve tracing provider and a standalone GPU test for the temporal upscaler. Native Play uses the regular renderer. The local launcher's 1440p upscalers use a separate runtime with the optional renderer. The test creates synthetic textures and verifies the GPU output by reading it back.

## Build and run the GPU test

Use the existing x64 Visual Studio developer environment and portable Clang. The build expects ReXGlue SDK and source 0.11.0 under `.tools`, its pinned xxHash header under `.tools/renderer-deps/xxhash`, and the following extracts under `.tools/renderer-deps`.

- `fsr-2.4.0/FidelityFX-SDK-2.4.0`. Keep the signed AMD loader and upscaler DLLs together in the SDK's `Kits/FidelityFX/signedbin` directory. [AMD FSR SDK 2.4.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v2.4.0)
- `streamline-2.14.16`. Use the signed x64 runtime in `bin/x64`. [NVIDIA Streamline 2.14.16](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16)

From the project root, with CMake and Ninja available, run the following.

```powershell
$source = 'host/renderer_trace'
$build = 'out/build/renderer-trace'
$compiler = (Resolve-Path '.tools/llvm-20.1.8/bin/clang++.exe').Path
$rexglue = (Resolve-Path '.tools/rexglue-0.11.0/win-amd64/lib/cmake/rexglue').Path
cmake -S $source -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_COMPILER=$compiler" "-Drexglue_DIR=$rexglue" -DASURA_TEMPORAL_PROBE=ON
cmake --build $build --target temporal_probe asura_d3d12_trace

$nv = '.tools/renderer-deps/streamline-2.14.16/bin/x64'
$amd = '.tools/renderer-deps/fsr-2.4.0/FidelityFX-SDK-2.4.0/Kits/FidelityFX/signedbin'
& "$build/temporal_probe.exe" dlss J $nv quality
& "$build/temporal_probe.exe" dlss K $nv balanced
& "$build/temporal_probe.exe" dlss M $nv performance
& "$build/temporal_probe.exe" dlss L $nv ultra-performance
& "$build/temporal_probe.exe" fsr3 default $amd quality
& "$build/temporal_probe.exe" fsr4 default $amd quality
```

The probe reports source and output dimensions, plus the active AMD provider when applicable. FSR 4 reports an unsupported adapter when the requested generation is absent from the vendor query. The runtime loader expects a trusted directory containing the unmodified vendor binaries.

To verify the fixed-input plan, pass `fixed-720p` and a target resolution. The DLSS probe selects a vendor mode that accepts 1280 × 720 internally. It retains the requested letter preset for that mode. The FSR probe explicitly creates its context for the fixed input.

```powershell
& "$build/temporal_probe.exe" dlss K $nv fixed-720p 1440p
& "$build/temporal_probe.exe" fsr3 default $amd fixed-720p 1440p
```

On the RTX 5070 Ti, DLSS preset K and FSR 3.1.6 passed GPU output checks at 1080p, 1440p, and 2160p with the same 1280 × 720 input. FSR 4 was unavailable on that adapter.

## Test with captured game scene inputs

The optional final probe argument is the directory containing the Episode 1 fixture. Export the 1280 × 720 HDR scene color at event 4703 of capture 8584 to `scene-rgba16f.bin`, keeping just the first 720 rows. For `depth-r32f.bin`, extract the float depth plane from the matching depth-stencil target. The opaque draws at events 1920, 1931, and 2031 use GreaterEqual depth tests and write reversed depth. The scene fixture uses the 0–1 device depth range.

```powershell
& "$build/temporal_probe.exe" dlss K $nv fixed-720p 1440p $sceneFolder
& "$build/temporal_probe.exe" fsr3 default $amd fixed-720p 1440p $sceneFolder
```

The probe dispatches a single reset frame and checks that the normalized captured depth and zero initial motion reach the vendor pass. It verifies finite, nonuniform GPU output. The fixture may include `scene-depth-range.txt` with the producing viewport's minimum and maximum device depth. Without this file, the probe assumes a 0–1 viewport. The Episode 1 opaque pass uses 0–0.5 reversed depth. Game-side motion, jitter, and HUD composition still need live validation.

## Gameplay frame replay and motion

The diagnostic DLL adds RenderDoc markers for Xenos resolves with the EDRAM tile, guest memory range, resolve format, and logical size. Use it in a separate copy of the Direct3D 12 build with a separate user, log, and cache directory.

- In RenderDoc captures `8545` and `8584`, tile 0 resolves the 1280×720 depth region to guest address `0x14978000`. That depth resolve uses Xbox 360 format 23, with 20e4 depth and 8-bit stencil.
- Tile 720 resolves 1280×720 color to `0x14240000` before the later compositor and final `0x1F5F8000` resolve. The earlier color resource shows the 3D scene before the captured presentation overlays. It is overwritten multiple times, so the address alone can identify several points in the frame.
- The 640×360 target resolved to `0x13980000` carries the game's motion blur data during the draw at event 6108 in capture `8584`. The vertex shader projects two object positions and passes their difference. The pixel shader applies camera reprojection and encodes a bounded vector into the first two channels. Replay of event 6201 confirms the live render target has red and green data with blue and alpha zero. The later draw at event 6320 is ReXGlue's EDRAM alias conversion and writes depth.
- An Episode 2 ground-combat replay in capture `31485` shows the same primary depth, color, and half-resolution resolve destinations. The Xbox 360 configuration inside `Coalesced_INT.bin` sets `MotionBlur=True` and `bAllowTemporalAA=False`. The cooked cache contains velocity shader names. Their use in these scenes is unverified.

`temporal_camera.cpp` and `depth_motion.hlsl` build Halton projection jitter and reproject scene depth for camera motion. The pass writes normalized device depth alongside motion and accepts a validity mask and full-precision object vectors to replace the camera estimate. Motion is current-to-previous in 1280×720 input pixels. The offscreen GPU probe covers the camera and object displacements, 0–0.5 depth normalization, jitter reuse for repeated presents, and camera cuts.

For the in-game connection, use the captures under `out/validation/temporal-trace-20261120-1704/`. In capture `8584`, the current projection is in vertex float rows 0–3 on draws 1920, 1931, 2031, and 6108. The motion pixel shader at 6108 reads the previous world-to-clip matrix relative to the current draw's world origin. Pass this matrix to `CameraHistory::BeginFrame` to preserve camera motion across world-origin changes. Apply consistent projection jitter to the scene draws before composing the HUD. [NVIDIA DLSS programming guide](https://github.com/NVIDIA-RTX/Streamline/blob/v2.14.16/docs/ProgrammingGuideDLSS.md) [AMD FSR 3.1 integration guide](https://gpuopen.com/download/FidelityFX_Super_Resolution_3-1_Release-Overview_and_Integration.pdf)

The Episode 1 replay recovers the prior skinned pose in the current camera's clip space. For each vertex, multiply it by the inverse current world-to-clip matrix and then by the rebased previous world-to-clip matrix. Divide each position by its own W and take previous minus current in render pixels, with Y increasing down the image. The blur pass instead uses a common Z denominator and a radial clamp. The replay matches on-screen hardware positions and the original skinned-pose numerator on two captured vertex shader variants. The game still needs a depth-tested pass that writes these vectors and a validity mask for the scene before SDK dispatch.

The reusable Episode 2 QA profile is under `out/validation/checkpoints/episode-02/user/`. Copy that profile into a test run's user directory so that each test starts at the same checkpoint.

## Isolated live DLSS test

Configure this renderer with `ASURA_LIVE_DLSS_QA=ON`. Run its DLL in an isolated game copy with `ASURA_LIVE_DLSS_QA` set to the absolute output folder and `ASURA_LIVE_DLSS_RUNTIME` set to the pinned Streamline runtime directory. Use D3D12 RTV at 1× render scale and the 30 FPS baseline. This build requests preset K at 2560×1440 from a 1280×720 scene.

The test captures depth from the primary resolve and dispatches DLSS after the tone-map draw, before the subsequent guest HUD draws. It applies projection jitter to the primary scene view when the previous frame was accepted. It replays eight skinned and six rigid motion shader variants against the captured depth and fills the remaining pixels with camera reprojection. Projected coordinates behind the previous lens use an out-of-frame history lookup. Frames missing the verified inputs skip the dispatch.

By default the test writes scene captures to the QA directory while the window presents the native game feed. For a presentation test, set `ASURA_LIVE_DLSS_PRESENT=1` in the isolated run. The renderer prepares a 2560×1440 color target from DLSS and carries over scene alpha for original HUD blending. It draws the verified HUD programs into that target using their original shaders, textures, and blend state. It submits the result through the game's gamma ramp and skips host FXAA for those frames.

The renderer uses native presentation whenever the required scene, supported HUD state, or final frontbuffer resolve is missing. The test keeps the normal draw and resolve path active for game memory. The original 720p feed remains available for menus, unsampled scenes, and unsupported HUD effects.

The two-texture blend seen after an Episode 1 camera cut uses native presentation to preserve the effect. The next temporal frame clears history and starts without jitter. The QA log names the blend separately from unfamiliar HUD programs.

Use `live-temporal.tsv` to inspect jitter, motion samples, HUD fallback, and successful 1440p presentation. A `composite-*.bmp` is the 1440p game image with the HUD before display gamma. Its matching `hud-sample` line reports the number of pixels changed by the HUD compared with the DLSS scene.

The Episode 2 compact velocity shader `E7D0056FF4D559EE` supplies the previous projection's X/Y columns. Recover the missing Z/W columns from the preceding 720p view and the change in the game's pre-view translation. The QA path checks the translation sign against native full matrices on moving frames, requires consecutive views, and checks the result against the compact shader's X/Y columns. A verified current view can keep DLSS presenting with cleared history when a previous view is unavailable. A frame with no verified view uses the native presentation path.

The live trace records camera sources as `native` for the full game matrix, `rebased` for compact matrix recovery, and `reset` for a verified current view without usable history. Camera cuts and HUD fallback still invalidate temporal history.

When motion draws omit the current view, the renderer can also read the rebased perspective projection from the main 720p scene's opaque draws. The projection must match the title's aspect, clip-space depth, orthogonal axes, and camera-relative translation. A projection shared by distinct shaders or observed in a verified view takes precedence over a single draw using the standard camera registers. These frames appear as `opaque` and reset temporal history if no previous view is available.

The two verified Episode 2 loading transitions can present from the game's composed 720p color when the 3D inputs are absent. This path checks the known shader boundary, a recently presented 3D scene, the final color resolve, and the guest swap texture's format and channel mapping. It converts the original BGR channel mapping before dispatch and uses constant far depth, zero motion, and cleared history. The trace labels these frames `composed`. Set `ASURA_LIVE_DLSS_TRANSITION_TRACE=1` to record the resolve and swap path around native gaps.

Set `ASURA_LIVE_UPSCALER=fsr3` and point `ASURA_LIVE_DLSS_RUNTIME` at the pinned FidelityFX `signedbin` directory to test live FSR 3. The same 720p scene passes, 1440p compositor, and HUD replay are used. FSR 3 also receives a per-pixel reactive mask based on motion-reprojected color disagreement between consecutive pre-HUD frames. The renderer skips the mask across cuts, history resets, and loading cards. For isolated comparisons, `ASURA_FSR3_REACTIVE=0` disables this mask.

To update the local launcher's temporal renderer after a verified build, copy its `rexgpu-xenos-d3d12.dll` to `out/dlss-runtime/` in the project. The launcher uses the pinned Streamline and FidelityFX runtimes under `.tools/renderer-deps/`. With 1440p output, enable Direct3D 12, 30 FPS, and original motion blur, then select DLSS or FSR 3. Both use the same 1280×720 render input. Use 720p for FXAA High and the regular renderer.
