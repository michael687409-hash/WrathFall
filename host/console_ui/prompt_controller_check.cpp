#include <cstdio>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>

#include "prompt_controller.h"

using namespace asura::prompt;

int main() {
  if (!SDL_Init(SDL_INIT_GAMEPAD)) return 1;

  SDL_VirtualJoystickDesc description;
  SDL_INIT_INTERFACE(&description);
  description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
  const SDL_JoystickID id = SDL_AttachVirtualJoystick(&description);
  SDL_Gamepad* gamepad = id ? SDL_OpenGamepad(id) : nullptr;
  if (!gamepad) {
    std::fprintf(stderr, "Could not open virtual gamepad: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);
  int previous = -1;
  bool passed = true;
  const auto step = [&](SDL_GamepadButton button, bool down, unsigned expected) {
    const bool set = SDL_SetJoystickVirtualButton(joystick, button, down);
    SDL_UpdateGamepads();
    const unsigned current = ReadGamepad(gamepad);
    const unsigned pressed = JustPressed(current, previous);
    previous = static_cast<int>(current);
    passed = passed && set && pressed == expected;
  };

  step(SDL_GAMEPAD_BUTTON_SOUTH, true, 0);  // A held when the dialog opened.
  step(SDL_GAMEPAD_BUTTON_SOUTH, true, 0);
  step(SDL_GAMEPAD_BUTTON_SOUTH, false, 0);
  step(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true, kNext);
  step(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false, 0);
  step(SDL_GAMEPAD_BUTTON_DPAD_LEFT, true, kPrevious);
  step(SDL_GAMEPAD_BUTTON_DPAD_LEFT, false, 0);
  step(SDL_GAMEPAD_BUTTON_SOUTH, true, kConfirm);
  step(SDL_GAMEPAD_BUTTON_SOUTH, false, 0);
  step(SDL_GAMEPAD_BUTTON_EAST, true, kBack);

  SDL_CloseGamepad(gamepad);
  SDL_DetachVirtualJoystick(id);
  SDL_Quit();
  if (!passed) {
    std::fputs("Prompt controller input regression.\n", stderr);
    return 1;
  }
  std::puts("Passed prompt controller checks: D-pad, A, B and held-button handling.");
}
