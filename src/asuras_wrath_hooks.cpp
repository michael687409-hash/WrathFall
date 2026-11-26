#include <rex/platform/process.h>
#include <rex/ppc/midasm.h>

namespace {

const auto& FrameRateSetting() {
  static const auto setting = rex::platform::GetEnvironmentValue("ASURAS_WRATH_FRAME_RATE");
  return setting;
}

double FrameRateTarget() {
  static const double target = [] {
    const auto& value = FrameRateSetting();
    if (value == "0") return 0.0;
    if (value == "60") return 60.0;
    if (value == "120") return 120.0;
    if (value == "144") return 144.0;
    return 30.0;
  }();
  return target;
}

}  // namespace

// TU1 returns the title's maximum tick rate. Zero leaves the engine on wall-clock timing.
REX_MIDASM_HOOK(AsuraAdjustTickRate, (PPCRegister& rate)) {
  if (rate.f64 == 30.0) {
    rate.f64 = FrameRateTarget();
  }
}
REX_MIDASM_AT(AsuraAdjustTickRate, 0x824D71C8, after, f1);

// TU1: choose one vblank per presentation in the game's two-vblank mode
// without changing the nominal refresh rate for the user's 30 FPS selection.
REX_MIDASM_HOOK(AsuraAdjustPresentInterval, (PPCRegister& interval)) {
  if (FrameRateSetting() == "0" || FrameRateTarget() > 30.0) {
    interval.u64 = 1;
  }
}
REX_MIDASM_AT(AsuraAdjustPresentInterval, 0x8299A71C, after, r11);
