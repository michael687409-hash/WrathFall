# Vulkan temporal upscaling

The Vulkan provider code currently runs in an offscreen GPU probe. On a GeForce RTX
5070 Ti, both FSR 3.1.6 and DLSS preset K processed a 1280 × 720 input into a
2560 × 1440 storage image. Copying the output to host-visible memory returned
average RGB values of 51, 153, and 229 for a known flat-color test image.
Both tests used the same images and the signed vendor libraries already pinned
by WrathFall. The probe uses Vulkan 1.3 for core feature negotiation.

Use an x64 Visual Studio developer shell. The probe takes the directory that
contains the Khronos Vulkan headers, with `vulkan/vulkan.h` underneath. The
verified build used Vulkan-Headers 1.3.280 and the SPIR-V-enabled DirectX Shader
Compiler 1.8.2505.1, distributed by Microsoft on GitHub.

```powershell
./Build-Probe.ps1 -VulkanInclude <headers> -OutputDirectory <build> -Dxc <dxc.exe>
<build>/temporal_probe.exe fsr3 <FidelityFX signedbin>
<build>/temporal_probe.exe dlss <Streamline bin/x64>
```

The shader build reuses the game's existing depth/motion and FSR reactive-mask
HLSL. The SPIR-V in descriptor set 0 uses a uniform buffer at binding 0,
sampled images at 1–3, and storage images at 4 (motion or reactive mask) and
5 (normalized depth). Reactivity uses a separate linear sampler at binding 6.
The compiled storage formats are RG32F for motion and R32F for depth or
reactivity. The expected camera constant offsets are 0, 64, 72, 80, 88, 92,
96, 100, and 112 bytes. The probe's test uses synthetic depth and motion
inputs. Live Vulkan shader dispatch and camera inputs need in-game validation.

## Live renderer integration

The experimental renderer target builds the pinned ReXGlue 0.11.0 Vulkan
driver. Configure this directory with `project_tools`, `vulkan_include`,
`vma_include`, `glslang_source`, and the installed `rexglue_DIR`. The
verified dependencies are Vulkan-Headers 1.4.313, VMA 3.3.0, and glslang at
`f4f1d8a352ca1908943aea2ad8c54b39b4879080`, the SDK's pinned submodule.
glslang needs Python for its build metadata. Build `asura_vulkan` in Release. Copy the resulting `rexgpu-xenos-vulkan.dll` to `out/vulkan-upscalers/` and rebuild the launcher. With the matching vendor runtime installed, choose **Vulkan (experimental upscaling)** in Graphics, then choose 2560 × 1440 and an upscaler.

Set `WRATHFALL_VK_SCENE_TRACE` to a trace file for draw, resolve, and present
evidence. `WRATHFALL_VK_DLSS_PROBE_RUNTIME` points to the signed Streamline
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
