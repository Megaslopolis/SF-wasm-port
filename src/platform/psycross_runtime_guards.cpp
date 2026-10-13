#include "psycross_runtime_guards.hpp"

#include <SDL.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace sf::platform::detail {

RelativeMouseCapture::~RelativeMouseCapture() { set(false); }

void RelativeMouseCapture::set(bool enabled) noexcept {
#ifdef __EMSCRIPTEN__
  // The page acquires and releases pointer lock (Module.sfPointer); this only
  // reports whether the canvas currently holds it.
  const auto captured = enabled && EM_ASM_INT({
    return document.pointerLockElement === Module['canvas'] ? 1 : 0;
  }) != 0;
  if (enabled_ != captured) {
    enabled_ = captured;
    SDL_GetRelativeMouseState(nullptr, nullptr);
  }
#else
  if (enabled_ == enabled) {
    return;
  }
  if (SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE) == 0) {
    enabled_ = enabled;
  }
  SDL_GetRelativeMouseState(nullptr, nullptr);
#endif
}

} // namespace sf::platform::detail
