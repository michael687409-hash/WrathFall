# Vulkan temporal upscaling

## Build and check the Vulkan renderer

Use an x64 Visual Studio developer shell with CMake, Ninja, and Python. Set the
dependency paths to the pinned ReXGlue 0.11.0 SDK and sources, Vulkan-Headers
1.4.313, VMA 3.3.0, and glslang at
`f4f1d8a352ca1908943aea2ad8c54b39b4879080`.
Set `$vulkanInclude` to the directory containing `vulkan/vulkan.h`,
`$vmaInclude` to the directory containing `vk_mem_alloc.h`, and
`$glslangSource` to the glslang source directory. From the project root:

```powershell
$tools = (Resolve-Path .tools).Path
$build = 'out/build/vulkan-temporal'
cmake -S host/renderer_trace/vulkan -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_CXX_COMPILER=$tools/llvm-20.1.8/bin/clang++.exe" `
  "-Dproject_tools=$tools" "-Drexglue_DIR=$tools/rexglue-0.11.0/win-amd64/lib/cmake/rexglue" `
  "-Dvulkan_include=$vulkanInclude" "-Dvma_include=$vmaInclude" "-Dglslang_source=$glslangSource"
cmake --build $build --target asura_vulkan check_vulkan_motion
```

The live driver and probe both embed the motion shader from the same generated
`temporalMotion.h` in this build. The motion check runs offscreen with
synthetic 720p frames and reads back vectors and the reactive mask. It checks a
static scene, a known camera pan, separate object motion, and history resets on
the first frame, after a camera cut, after a frame gap, and when a camera is
reacquired. The probe and live renderer use the same history-reset decision.
Use `cmake --build $build --target check_vulkan_motion` to repeat the check
without rebuilding the driver.

## Offscreen upscaler checks

The `asura_vulkan_probe` target builds `temporal_probe.exe`. Its provider modes
use a Vulkan 1.3 device and process a known 1280 × 720 image into a 2560 × 1440
storage image. Both FSR 3.1.6 and DLSS preset K returned mean RGB values of
51, 153, and 229 on an RTX 5070 Ti.

```powershell
$vendors = "$tools/renderer-deps"
& "$build/temporal_probe.exe" fsr3 "$vendors/fsr-2.4.0/FidelityFX-SDK-2.4.0/Kits/FidelityFX/signedbin"
& "$build/temporal_probe.exe" dlss "$vendors/streamline-2.14.16/bin/x64"
```

To cross-compile the separate Direct3D 12 depth/motion and FSR reactive-mask
HLSL for experiments, configure the same build with
`-Ddxc_executable=<path to dxc.exe>` and build the `vulkan_hlsl_spirv` target.
It generates SPIR-V and disassembly listings.

## Live renderer integration

Set `WRATHFALL_VK_SCENE_TRACE` to a trace file for draw, resolve, and present
evidence. On a queue that supports timestamps, `gpu-timing` rows record the
elapsed GPU time in milliseconds for input unpacking, motion estimation,
upscaling, HUD composition, and history storage. These cover the temporal
passes and their barriers. The guest frame number is the second column.
`WRATHFALL_VK_DLSS_PROBE_RUNTIME` points to the signed Streamline
directory. The Vulkan instance enables Streamline's extensions before device
creation, and the device enables the reported Vulkan 1.2/1.3 features. It
then binds preset K at 1280 × 720 to 2560 × 1440. Set
`WRATHFALL_VK_FSR_PROBE_RUNTIME` to the signed FidelityFX directory to verify
FSR 3.1.6 on the same game device. Both probe contexts initialize in an
isolated interlock session on the RTX 5070 Ti.

The experimental launcher mode sets `WRATHFALL_VK_TEMPORAL` to `dlss` or
`fsr3` and supplies the matching signed runtime. It copies scene color
and depth on the GPU. The motion pass searches consecutive scene frames
using camera reprojection where available. The presentation pass retains
pixels changed by the guest HUD, applies the guest gamma ramp, and logs
upscaled scene coverage, overlay coverage, and fallback.

Set `WRATHFALL_VK_CAPTURE_FRAME` to the first eligible guest frame to verify
interlock inputs in a local QA replay. This reads the title's 720p D24FS8
EDRAM before its depth resolve and the 720p RGBA8 compositor target before
the first full-size overlay draw. It reports nonuniform pixel counts for a
matched pair. This verification uses a CPU readback and waits on the GPU.

Stock Vulkan's host render targets produced dark red silhouettes in Episode 6
at native 720p. The same scene reproduced with direct host resolves disabled
and with 8-bit gamma targets. Fragment shader interlock restored geometry and
lighting detail. The Vulkan upscaler must follow that path's guest resolves
for scene and depth input because it does not expose host depth attachments.

1. Validate the 80 × 16 tile addressing, reversed halves for depth, the
   depth range, and channel order against the native output. Check the
   guest motion targets and Vulkan viewport Y direction during motion and
   camera cuts.
2. Validate the two-texture HUD draw with guest vertex
   shader `5bf4e342d00f1593` and pixel shader `2c1e4e099e31e037`. It forces
   the older Direct3D 12 path to present the 720p swap image in Episodes 6 and 11.
   Use collector telemetry to distinguish real 1440p presentation from a
   successful dispatch followed by fallback.
3. Verify 720p to 1440p during combat with the HUD visible and capture
   repeated QTE and audio timings. Gameplay verification for the local
   experimental launcher mode is pending.
