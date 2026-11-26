#include "prompt_controller.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_stdinc.h>
#include <windows.h>
#include <Xinput.h>

namespace asura::prompt {

unsigned ReadGamepad(SDL_Gamepad* gamepad) {
  unsigned buttons = 0;
  if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) ||
      SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP)) {
    buttons |= kPrevious;
  }
  if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ||
      SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) {
    buttons |= kNext;
  }
  if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH)) {
    buttons |= kConfirm;
  }
  if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST)) {
    buttons |= kBack;
  }
  return buttons;
}

unsigned ReadControllers() {
  unsigned buttons = 0;
  bool using_sdl = false;
  SDL_LockJoysticks();
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  for (int i = 0; ids && i < count; ++i) {
    if (auto* gamepad = SDL_GetGamepadFromID(ids[i])) {
      buttons |= ReadGamepad(gamepad);
      using_sdl = true;
    }
  }
  SDL_free(ids);
  SDL_UnlockJoysticks();

  if (!using_sdl) {
    // The XInput driver loads this DLL when active. Loading it as a dependency
    // of the UI module can fail before the runtime finishes initialization.
    const HMODULE xinput = GetModuleHandleW(L"xinput1_4.dll");
    const auto get_state = xinput ? reinterpret_cast<decltype(&XInputGetState)>(
                                        GetProcAddress(xinput, "XInputGetState"))
                                  : nullptr;
    if (!get_state) return buttons;
    for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
      XINPUT_STATE state{};
      if (get_state(user, &state) != ERROR_SUCCESS) {
        continue;
      }
      const WORD raw = state.Gamepad.wButtons;
      if (raw & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_UP)) buttons |= kPrevious;
      if (raw & (XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_DOWN)) buttons |= kNext;
      if (raw & XINPUT_GAMEPAD_A) buttons |= kConfirm;
      if (raw & XINPUT_GAMEPAD_B) buttons |= kBack;
    }
  }
  return buttons;
}

}  // namespace asura::prompt
