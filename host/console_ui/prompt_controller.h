#pragma once

struct SDL_Gamepad;

namespace asura::prompt {

enum Button : unsigned {
  kPrevious = 1 << 0,
  kNext = 1 << 1,
  kConfirm = 1 << 2,
  kBack = 1 << 3,
};

unsigned ReadGamepad(SDL_Gamepad* gamepad);
unsigned ReadControllers();

// Snapshot held buttons on opening the prompt, then react only to new presses.
constexpr unsigned JustPressed(unsigned current, int previous) {
  return previous < 0 ? 0 : current & ~static_cast<unsigned>(previous);
}

}  // namespace asura::prompt
