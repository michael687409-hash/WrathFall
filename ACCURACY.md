# Xbox 360 accuracy

The reference is the Xbox 360 release with title ID `43430817`, media ID `11DFADD6`, and TU1. The PC build recompiles that executable and keeps the stock game logic and scheduling defaults. The launcher defaults to 1× rendering and VSync. Shader compilation on first use is synchronous so an unavailable pipeline does not skip a draw.

## Guest memory

The build runs `guest_layout_check.exe` against the same ReXGlue runtime and generated load/store macros as the game. It checks byte order for 8-, 16-, 32-, and 64-bit access, stack access, signed-zero and NaN payload bits in memory, and the `0xE0000000` view’s `0x1000` physical offset.

## Game behavior

| Area | Comparison required for parity |
| --- | --- |
| Physics and simulation | Replay identical controller input at the same simulation ticks. Compare transforms, collision outcomes, impulses, and QTE timing. |
| Animation | Compare clip selection, blend weights, poses, and the ticks that fire animation events. Include transitions, special attacks, cutscenes, and DLC. |
| Sound | Match playback events, loop points, channel order, and the timing of music, voices, and effects. Compare aligned PCM at the same stage before differences in device output or recording enter the signal. |
| Timing | Check frame delivery and input-to-event timing against the console with the same TV mode. Track GPU pipeline stalls separately from game ticks. |

Boot checks, DLC registration, and memory checks cover only the inputs and behaviors they exercise. Xbox 360 reference captures and per-scene comparisons are still needed for an end-to-end bit-accuracy claim.

## Experimental frame rates

The original mode uses the title's stock timing. The higher targets change the TU1 engine cap and presentation interval. The 120 and 144 FPS targets also raise the vertical blank rate reported to the title.

Direct3D 12 at the original render resolution reached each selected target on the title screen. The elapsed time between the first two logo playback requests varied by 51 ms across the four runs.

| Target | Measured title-screen frame time | First logo interval |
| --- | --- | --- |
| Original | 33.35 ms | 5.066 s |
| 60 FPS | 16.66 ms | 5.032 s |
| 120 FPS | 8.34 ms | 5.015 s |
| 144 FPS | 6.95 ms | 5.019 s |

These measurements cover frame delivery and one prerecorded sequence. Checking physics and quick-time events requires replaying identical inputs through the same combat and button-mashing sequences, then comparing simulation outcomes and timing. Original remains the default.

[Xbox 360 frame-rate patch and QTE notes](https://github.com/xenia-canary/game-patches/blob/main/patches/43430817%20-%20Asura%27s%20Wrath.patch.toml)

[ReXGlue guest memory](https://docs.rexglue.com/runtime/memory/) · [ReXGlue generated code](https://docs.rexglue.com/codegen/generated-code/)
