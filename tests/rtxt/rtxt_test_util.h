// Shared helpers for the RTXT C++ tests.
#pragma once

#include <cstdio>

#include <rtxt/rtxt.h>

namespace rtxt_test {

inline bool expect(bool condition, const char *message) {
  if (condition) {
    return true;
  }
  std::fprintf(stderr, "FAIL: %s\n", message);
  return false;
}

// A small but representative table: three sections, mixed positions, a {hot}
// marker, an empty-text entry, and a duplicate-cased key to exercise lookup.
inline opennova::rtxt::File make_sample() {
  using namespace opennova::rtxt;
  File f;
  f.sections = {
      {"menu_main", 2},
      {"menu_options", 1},
      {"hud", 1},
  };
  f.entries = {
      {"BTN_NEW_GAME", "{hot}New Game", {10, 20}, 0},
      {"BTN_QUIT", "Quit", {10, 48}, 0},
      {"OPT_DIFFICULTY", "Difficulty", {0, 0}, 1},
      {"HUD_AMMO", "", {-32, -16}, 2},
  };
  f.build_lookup();
  return f;
}

}  // namespace rtxt_test
