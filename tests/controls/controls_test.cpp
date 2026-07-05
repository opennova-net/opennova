// Unit tests for libs/controls: the JO input-binding catalog + Controls table model.
// Validates the byte-witnessed catalog (names/tokens/class), the VK key-name decoder,
// the binding format rules, and the per-device row build against the canonical JO
// defaults. [orig: aAbsoluteTurnLe @ 0x8159cb; KeyBinding_* @ 0x494c60/0x559a10;
// UI_PopulateControlMappingList @ 0x55c0c0]
#include <cstdlib>
#include <iostream>
#include <string>

#include "controls/controls.h"

namespace {

using namespace opennova::controls;

#define CHECK(cond, msg)                                      \
  do {                                                        \
    if (!(cond)) {                                            \
      std::cerr << "FAIL: " << msg << " at line " << __LINE__ \
                << "\n";                                      \
      return false;                                           \
    }                                                         \
  } while (0)

const ActionDef *find_action(const char *token) {
  std::size_t n = 0;
  const ActionDef *cat = catalog(&n);
  for (std::size_t i = 0; i < n; ++i) {
    if (std::string(cat[i].token) == token) {
      return &cat[i];
    }
  }
  return nullptr;
}

// The catalog is present and the well-known movement actions carry their canonical
// JO defaults (Forward=W/Up, Back=S/Down, Strafe A/D, Jump=Space, Reload=R).
bool test_catalog_defaults() {
  std::size_t n = 0;
  const ActionDef *cat = catalog(&n);
  CHECK(cat != nullptr && n > 80, "catalog should hold the full action table");

  const ActionDef *fwd = find_action("move_forward");
  CHECK(fwd != nullptr, "move_forward present");
  CHECK(std::string(fwd->name) == "Forward", "forward name");
  CHECK(fwd->cls == ActionClass::Movement, "forward is Movement");
  CHECK(fwd->default_key == 0x57, "forward primary = W (0x57)");
  CHECK(fwd->default_key2 == 0x26, "forward secondary = Up (0x26)");

  const ActionDef *back = find_action("move_back");
  CHECK(back != nullptr && back->default_key == 0x53, "back = S");
  const ActionDef *sl = find_action("strafe_left");
  CHECK(sl != nullptr && sl->default_key == 0x41, "strafe_left = A");
  const ActionDef *sr = find_action("strafe_right");
  CHECK(sr != nullptr && sr->default_key == 0x44, "strafe_right = D");
  const ActionDef *jump = find_action("move_jump");
  CHECK(jump != nullptr && jump->default_key == 0x20, "jump = Space");
  const ActionDef *reload = find_action("magazine");
  CHECK(reload != nullptr && reload->default_key == 0x52, "reload = R");
  return true;
}

// Class id -> name mapping. [orig: KeyBinding_BuildCategoryPages @ 0x4966c0]
bool test_class_names() {
  CHECK(std::string(action_class_name(ActionClass::Movement)) == "Movement", "Movement");
  CHECK(std::string(action_class_name(ActionClass::Weapons)) == "Weapons", "Weapons");
  CHECK(std::string(action_class_name(ActionClass::Communications)) == "Communications",
        "Communications");
  CHECK(std::string(action_class_name(ActionClass::Spectator)) == "Spectator", "Spectator");
  return true;
}

// VK -> display name. [orig: KeyBinding_GetKeyNameAndDisplayName @ 0x494c60]
bool test_key_names() {
  CHECK(key_name(0x57) == "W", "letter W");
  CHECK(key_name(0x31) == "1", "digit 1");
  CHECK(key_name(0x20) == "Space", "space");
  CHECK(key_name(0x26) == "Up", "up arrow");
  CHECK(key_name(0x01) == "Mouse 1", "mouse 1");
  CHECK(key_name(0x70) == "F1", "F1");
  CHECK(key_name(0x73) == "F4", "F4");
  CHECK(key_name(0x60) == "Numpad 0", "numpad 0");
  CHECK(key_name(0x1B) == "Esc", "escape");
  CHECK(key_name(0x52) == "R", "letter R");
  CHECK(key_name(0).empty(), "vk 0 is empty");
  return true;
}

// Binding format: primary alone, primary + secondary joined by " or ", unbound empty.
// [orig: KeyBinding_FormatBindingString @ 0x559a10]
bool test_format_binding() {
  CHECK(format_binding(0x57, 0x26) == "W or Up", "two-slot join");
  CHECK(format_binding(0x52, 0x00) == "R", "single key");
  CHECK(format_binding(0x00, 0x00).empty(), "unbound empty");
  return true;
}

// The keyboard rows are populated with class/action/control and exclude the
// admin/internal classes. [orig: UI_PopulateControlMappingList @ 0x55c0c0]
bool test_build_rows_keyboard() {
  const std::vector<ControlRow> rows = build_rows(Device::Keyboard);
  CHECK(rows.size() > 40, "keyboard rows populated");

  bool found_forward = false;
  bool saw_server = false;
  bool saw_abs_turn = false;
  bool saw_last_move = false;
  bool found_spectator = false;
  for (const ControlRow &r : rows) {
    CHECK(!r.cls.empty() && !r.action.empty(), "class+action non-empty");
    if (r.action == "Forward") {
      found_forward = true;
      CHECK(r.cls == "Movement", "forward class");
      CHECK(r.control == "W or Up", "forward control");
    }
    if (r.cls == "Server" || r.cls == "Null" || r.cls == "Cheat") {
      saw_server = true;
    }
    if (r.action == "Absolute Turn Left" || r.action == "Look Pitch") {
      saw_abs_turn = true;
    }
    if (r.action == "Last_move") {
      saw_last_move = true;
    }
    if (r.action == "Cycle Spectator Mode") {
      found_spectator = true;
    }
  }
  CHECK(found_forward, "forward row present");
  CHECK(!saw_server, "admin/internal classes are hidden");
  // The witnessed per-entry gate, not the class approximation: retail hides
  // the analog-only movement entries (0xC100425/0xC200425 — bit 0x800 clear,
  // bit 0x20 set) and Last_move (0xC000405 — bit 0x800 clear), while the
  // spectator entries (0x4000800) show [orig: catalog flags @ 0x8159AC + 108*id].
  CHECK(!saw_abs_turn, "analog-only movement entries hidden (witnessed flags)");
  CHECK(!saw_last_move, "Last_move hidden (witnessed flags)");
  CHECK(found_spectator, "spectator entries shown (witnessed flags)");
  return true;
}

// Mouse/joystick share the action list but have no static default bindings (D-CTRL-1).
bool test_build_rows_other_devices() {
  const std::vector<ControlRow> kb = build_rows(Device::Keyboard);
  const std::vector<ControlRow> mouse = build_rows(Device::Mouse);
  CHECK(kb.size() == mouse.size(), "same action set across devices");
  for (const ControlRow &r : mouse) {
    CHECK(r.control.empty(), "mouse controls blank for now");
  }
  return true;
}

}  // namespace

int main() {
  int failed = 0;

#define RUN_TEST(name)                      \
  do {                                      \
    std::cout << "Running " #name "... ";   \
    if (name()) {                           \
      std::cout << "OK\n";                  \
    } else {                                \
      std::cout << "FAILED\n";              \
      ++failed;                             \
    }                                       \
  } while (0)

  RUN_TEST(test_catalog_defaults);
  RUN_TEST(test_class_names);
  RUN_TEST(test_key_names);
  RUN_TEST(test_format_binding);
  RUN_TEST(test_build_rows_keyboard);
  RUN_TEST(test_build_rows_other_devices);

  if (failed > 0) {
    std::cerr << "\n" << failed << " test(s) FAILED\n";
    return EXIT_FAILURE;
  }

  std::cout << "\nAll tests passed!\n";
  return EXIT_SUCCESS;
}
