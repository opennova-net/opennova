// Unit tests for engine/runtime/controls: the JO input-binding catalog + Controls table model.
// Validates the byte-witnessed catalog (names/tokens/class), the VK key-name decoder,
// the binding format rules, and the per-device row build against the canonical JO
// defaults. [orig: aAbsoluteTurnLe @ 0x8159cb; KeyBinding_* @ 0x494c60/0x559a10;
// UI_PopulateControlMappingList @ 0x55c0c0]
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

#include <runtime/controls/binding_set.h>
#include <runtime/controls/controls.h>

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

  // The seat rows share the digit keys with the weapon categories and differ
  // only by the slot-1 modifier word (+24): Ctrl+1 = seat1, bare 1 = Knife
  // [orig: rows @0x8160D8 / @0x81657C; Input_ProcessKeyboardEvents
  //  @0x49d35b modifier pass, @0x49d3ba fallback].
  const ActionDef *seat1 = find_action("seat1");
  CHECK(seat1 != nullptr && seat1->default_key == 0x31 && seat1->default_mod == 0x11,
        "seat1 = Ctrl+1");
  const ActionDef *seat10 = find_action("seat10");
  CHECK(seat10 != nullptr && seat10->default_key == 0x30 && seat10->default_mod == 0x11,
        "seat10 = Ctrl+0");
  const ActionDef *knife = find_action("Knife");
  CHECK(knife != nullptr && knife->default_key == 0x31 && knife->default_mod == 0,
        "Knife = bare 1");
  const ActionDef *gtalk = find_action("gtalk");
  CHECK(gtalk != nullptr && gtalk->default_key == 0x54 && gtalk->default_mod == 0x11,
        "gtalk = Ctrl+T");
  CHECK(reload->default_mod == 0, "reload carries no modifier");
  int ctrl_rows = 0;
  for (std::size_t i = 0; i < n; ++i) {
    CHECK(cat[i].default_mod == 0 || cat[i].default_mod == 0x11,
          "the only witnessed slot-1 modifier is VK_CONTROL");
    if (cat[i].default_mod == 0x11) ++ctrl_rows;
  }
  CHECK(ctrl_rows == 18, "eighteen catalog rows default to a Ctrl chord");
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
  // Modifier-word prefixes [orig: KeyBinding_FormatBindingString @ 0x559a10 —
  // word 17 "Ctrl-", word 16 "Shift-"].
  CHECK(format_binding(0x57, 0x26, 17, 0) == "Ctrl - W or Up",
        "slot-1 Ctrl prefix");
  CHECK(format_binding(0x57, 0x26, 0, 16) == "W or Shift - Up",
        "slot-2 Shift prefix");
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
    if (r.action == "Prone") CHECK(r.control == "Middle", "middle button defaults to prone");
    if (r.action == "Cycle Weapon Prev") CHECK(r.control == "Mouse Whl Up", "wheel default");
  }
  return true;
}

// The live-record assignment semantics [orig: KeyBinding_HandleKeyAssignment
// @ 0x55bb20; CLEAR_KEY @ 0x55bfd0; DEFAULTS @ 0x55bd90; mouse capture
// @ 0x55c780].
bool test_mouse_dispatch() {
  BindingSet set;
  bool ctrl = false;
  auto held = [&ctrl](int vk) { return ctrl && vk == 17; };
  auto token = [&set](const char *name) { return set.index_of_token(name); };
  CHECK(set.mouse_event_action(kMouseWheelUp, held) == token("cycleweaponP"), "bare wheel cycles");
  CHECK(set.mouse_event_action(kMouseWheelDown, held) == token("cycleweaponN"), "bare reverse wheel cycles");
  ctrl = true;
  CHECK(set.mouse_event_action(kMouseWheelUp, held) == token("ScopeZeroInc"), "Ctrl wheel increments zero only");
  CHECK(set.mouse_event_action(kMouseWheelDown, held) == token("ScopeZeroDec"), "Ctrl wheel decrements zero only");
  CHECK(set.mouse_event_action(kMouseRight, held) == token("scope"), "RMB event toggles scope");
  CHECK(set.pressed_mouse(token("FreeLook"), kMouseRight, held), "RMB held also supplies free look");
  CHECK(!set.pressed_mouse(token("FreeLook"), 0, held), "release clears free look");
  CHECK(set.mouse_event_action(kMouseLeft, held, true) == token("IncSpectatorTarget"), "death mode skips fire");
  CHECK(set.record(token("look_up"))->joy_button == 0x81, "static joystick axis copied");
  CHECK(set.record(token("attack_1"))->joy_button == 1, "static joystick fire button copied");
  return true;
}

bool test_binding_set_assignment() {
  BindingSet set;
  const int fwd = set.index_of_token("move_forward");
  CHECK(fwd >= 0, "move_forward is in the catalog");
  const BindingRecord *r = set.record(fwd);
  CHECK(r != nullptr && r->primary != 0, "defaults seed the primary slot");
  const uint16_t default_primary = r->primary;
  const uint16_t default_secondary = r->secondary;

  // Both slots full: a new key replaces the PRIMARY.
  CHECK(set.assign_key(fwd, 0x47, false, false, false, false), "assign G");
  r = set.record(fwd);
  CHECK(r->primary == 0x47 && r->secondary == default_secondary,
        "both-full assignment replaces the primary");

  // Assigning an already-held key collapses the record to it alone.
  CHECK(set.assign_key(fwd, static_cast<int>(default_secondary), false, false,
                       false, false),
        "assign the held secondary");
  r = set.record(fwd);
  CHECK(r->primary == default_secondary && r->secondary == 0,
        "duplicate assignment collapses to the sole primary");

  // One empty slot: the new key fills it.
  CHECK(set.assign_key(fwd, 0x48, false, false, false, false), "assign H");
  r = set.record(fwd);
  CHECK(r->primary == default_secondary && r->secondary == 0x48,
        "the empty secondary slot fills");

  // Rejected events [orig: KeyBinding_HandleKeyAssignment @ 0x55bb26 —
  // every Ctrl press arrives Ctrl-flagged (the state byte is set before its
  // own event enqueues), so VK 0x11 never assigns through the capture].
  CHECK(!set.assign_key(fwd, 0xDE, false, false, false, false),
        "VK 0xDE never assigns");
  CHECK(!set.assign_key(fwd, 0x11, true, false, false, false),
        "a Ctrl press never assigns (its own flag is already set)");

  // The Ctrl- combo: a key with Ctrl held ALONE records modifier 17; any
  // extra flag (shift/extended/repeat) defeats the exact-0x800 compare
  // [orig: @ 0x55bb4f..0x55bb51; Input_QueueKeyEvent @ 0x760c10].
  CHECK(set.assign_key(fwd, 0x59, true, false, false, false),
        "Ctrl+Y assigns");
  r = set.record(fwd);
  CHECK(r->primary == 0x59 && r->primary_mod == 17,
        "Ctrl-held-alone records modifier 17");
  CHECK(set.control_text(fwd, Device::Keyboard).rfind("Ctrl - Y", 0) == 0,
        "the Ctrl- prefix renders");
  CHECK(set.assign_key(fwd, 0x59, true, true, false, false),
        "Ctrl+Shift+Y assigns");
  r = set.record(fwd);
  CHECK(r->primary == 0x59 && r->primary_mod == 0,
        "a second flag defeats the modifier and the new (scan,0) replaces");
  CHECK(set.assign_key(fwd, 0x25, true, false, true, false),
        "Ctrl+Left (extended) assigns");
  r = set.record(fwd);
  CHECK(r->primary == 0x25 && r->primary_mod == 0,
        "an extended key records no modifier even with Ctrl held");

  // CLEAR_KEY per device; DEFAULTS restores the catalog values.
  set.clear(fwd, Device::Keyboard);
  r = set.record(fwd);
  CHECK(r->primary == 0 && r->secondary == 0, "keyboard clear empties both slots");
  set.assign_mouse(fwd, kMouseRight);
  CHECK(set.record(fwd)->mouse_mask == kMouseRight, "mouse capture stores the mask");
  CHECK(set.control_text(fwd, Device::Mouse) == "Right", "mouse control text");
  set.clear(fwd, Device::Mouse);
  CHECK(set.record(fwd)->mouse_mask == 0, "mouse clear");
  set.restore_defaults();
  r = set.record(fwd);
  CHECK(r->primary == default_primary && r->secondary == default_secondary,
        "DEFAULTS restores the catalog binding");
  const int seat1 = set.index_of_token("seat1");
  const int knife = set.index_of_token("Knife");
  CHECK(seat1 >= 0 && knife >= 0, "seat1 and Knife rows exist");
  CHECK(set.record(seat1)->primary == 0x31 && set.record(seat1)->primary_mod == 17,
        "the default seat1 record is the Ctrl+1 chord");
  CHECK(set.record(knife)->primary == 0x31 && set.record(knife)->primary_mod == 0,
        "the default Knife record is a bare 1");
  CHECK(set.control_text(seat1, Device::Keyboard).rfind("Ctrl - 1", 0) == 0,
        "the Options table shows the seat chord");

  // Live rows mirror the static build and consume edits.
  const std::vector<ControlRow> stat = build_rows(Device::Keyboard);
  const std::vector<ControlRow> live = set.build_rows(Device::Keyboard);
  CHECK(stat.size() == live.size(), "live rows match the static row set");
  bool all_equal = true;
  for (std::size_t i = 0; i < stat.size(); ++i) {
    if (stat[i].control != live[i].control) {
      all_equal = false;
    }
  }
  CHECK(all_equal, "default live rows equal the static rows");
  const int row0_action = set.action_index_for_row(0);
  CHECK(row0_action >= 0, "row 0 maps to a catalog action");
  const std::vector<int> keys = set.keys_for_token("move_forward");
  CHECK(keys.size() == 2 && keys[0] == default_primary,
        "keys_for_token yields the live VKs");
  return true;
}

// The in-game display formatter the death screen's "call a medic" hint uses
// [orig: KeyBinding_FormatDisplayString @0x496bd0]: the three arms in their
// witnessed order, the " or " joiner, the per-slot Ctrl/Shift prefixes, the
// modifier-less reset, the mouse names, and the " *" flag suffix.
bool test_format_display_string() {
  BindingRecord rec;
  rec.primary = 0x39;  // '9' — the MedicReq catalog default
  CHECK(format_display_string(rec) == "9", "a bare key prints its name");
  rec.primary_mod = 17;
  CHECK(format_display_string(rec) == "Ctrl - 9",
        "a modified slot walks arm 1 with the Ctrl prefix");
  rec.secondary = 0x20;
  CHECK(format_display_string(rec) == "Ctrl - 9",
        "arm 2 resets the buffer for the modifier-less second slot and "
        "prints the FIRST key behind either slot's modifier");
  rec.primary_mod = 0;
  rec.secondary_mod = 16;
  CHECK(format_display_string(rec) == "Shift - 9",
        "either slot's Shift lands in front of the first key");
  rec.secondary = 0;
  rec.secondary_mod = 0;
  rec.mouse_mask = 2;
  CHECK(format_display_string(rec) == "9 or Mouse 2",
        "a mouse button joins a keyboard slot with ' or '");
  rec.primary = 0;
  CHECK(format_display_string(rec) == "Mouse 2", "mouse only");
  rec.mouse_mask = 2048;
  CHECK(format_display_string(rec, true) == "Mouse Whl Dn *",
        "the 0x200 flag appends ' *'");
  return true;
}

// The keyboard dispatcher's two passes in poll form over the live records
// [orig: Input_ProcessKeyboardEvents @0x49d1f0 -- the modifier pass
// @0x49d327..0x49d3ac; the fallback @0x49d3ba..0x49d488 runs for a key event
// only when the modifier pass matched nothing]: Ctrl+1 fires seat1 and never
// Knife, a bare 1 fires Knife, Ctrl+R fires respawn and never the reload,
// Ctrl over an unclaimed key still reaches its modifier-less row, and a mixed
// record follows the ROW's modifier.
bool test_pressed_key_two_passes() {
  BindingSet set;
  const int seat1 = set.index_of_token("seat1");
  const int knife = set.index_of_token("Knife");
  const int respawn = set.index_of_token("respawn");
  const int reload = set.index_of_token("magazine");
  const int binoculars = set.index_of_token("binoculars");
  CHECK(seat1 >= 0 && knife >= 0 && respawn >= 0 && reload >= 0 && binoculars >= 0,
        "the rows exist");
  std::set<int> down;
  const auto key_down = [&](int vk) { return down.count(vk) != 0; };
  CHECK(set.pressed_key(seat1, key_down) == 0 && set.pressed_key(knife, key_down) == 0,
        "nothing held, nothing fires");
  CHECK(set.pressed_key(-1, key_down) == 0, "an unknown row fires nothing");

  down = {0x31};
  CHECK(set.pressed_key(knife, key_down) == 0x31, "a bare 1 fires Knife (fallback pass)");
  CHECK(set.pressed_key(seat1, key_down) == 0, "a bare 1 never fires the Ctrl+1 seat row");
  down = {0x11, 0x31};
  CHECK(set.pressed_key(seat1, key_down) == 0x31, "Ctrl+1 fires seat1 (modifier pass)");
  CHECK(set.pressed_key(knife, key_down) == 0,
        "the modifier pass claims the key: Knife stays silent");
  down = {0x11};
  CHECK(set.pressed_key(seat1, key_down) == 0 && set.pressed_key(knife, key_down) == 0,
        "Ctrl alone fires neither");

  down = {0x52};
  CHECK(set.pressed_key(reload, key_down) == 0x52, "R reloads");
  CHECK(set.pressed_key(respawn, key_down) == 0, "R alone never respawns");
  down = {0x11, 0x52};
  CHECK(set.pressed_key(respawn, key_down) == 0x52, "Ctrl+R respawns");
  CHECK(set.pressed_key(reload, key_down) == 0, "Ctrl+R never reloads");
  down = {0x11, 0x42};
  CHECK(set.pressed_key(binoculars, key_down) == 0x42,
        "a held modifier over an unclaimed key still fires the modifier-less row");

  // A mixed record: Ctrl+1 primary, bare Z secondary.
  BindingRecord rec;
  rec.primary = 0x31;
  rec.primary_mod = 17;
  rec.secondary = 0x5A;
  CHECK(set.set_record(binoculars, rec), "the mixed record installs");
  down = {0x11, 0x5A};
  CHECK(set.pressed_key(binoculars, key_down) == 0x5A,
        "pass 1 checks the ROW's modifier, then either key: Ctrl+Z fires");
  down = {0x5A};
  CHECK(set.pressed_key(binoculars, key_down) == 0,
        "a bare Z never fires: the fallback wants both modifier words zero");
  down = {0x11, 0x31};
  CHECK(set.pressed_key(binoculars, key_down) == 0x31 && set.pressed_key(seat1, key_down) == 0x31,
        "two Ctrl+1 rows both fire in poll form (retail queues both events)");
  return true;
}

bool test_joystick_pov_dispatch() {
  BindingSet bindings;
  const std::array<int, 8> boundaries{2200, 6800, 11200, 15000, 20200, 24800, 29200, 33800};
  const std::array<uint8_t, 9> masks{1, 9, 8, 10, 2, 6, 4, 5, 1};
  for (size_t i = 0; i < boundaries.size(); ++i) {
    CHECK(joystick_pov_mask(boundaries[i]) == masks[i], "inclusive POV sector boundary");
    CHECK(joystick_pov_mask(boundaries[i] + 1) == masks[i + 1], "next POV sector");
  }
  CHECK(joystick_pov_mask(-1) == 0 && joystick_pov_mask(65535) == 0, "POV center sentinel");
  std::array<int32_t, 4> hats{4500, -1, -1, -1};
  const auto no_buttons = [](int) { return false; };
  CHECK(bindings.pressed_joystick(bindings.index_of_token("look_up"), no_buttons, hats), "default up POV");
  CHECK(bindings.pressed_joystick(bindings.index_of_token("turn_right"), no_buttons, hats), "diagonal also turns right");
  CHECK(!bindings.pressed_joystick(bindings.index_of_token("look_down"), no_buttons, hats), "opposite POV stays released");
  hats[0] = -1;
  CHECK(!bindings.pressed_joystick(bindings.index_of_token("look_up"), no_buttons, hats), "center releases held action");
  const int up = bindings.index_of_token("look_up");
  BindingRecord row = *bindings.record(up);
  row.joy_button = 0x85;
  bindings.set_record(up, row);
  hats[1] = 0;
  CHECK(bindings.pressed_joystick(up, no_buttons, hats), "second hat keeps its own four-bit bank");
  row.joy_button = 128;
  bindings.set_record(up, row);
  CHECK(bindings.pressed_joystick(up, [](int index) { return index == 127; }, hats), "last ordinary button is 128");
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
  RUN_TEST(test_mouse_dispatch);
  RUN_TEST(test_joystick_pov_dispatch);
  RUN_TEST(test_binding_set_assignment);
  RUN_TEST(test_format_display_string);
  RUN_TEST(test_pressed_key_two_passes);

  if (failed > 0) {
    std::cerr << "\n" << failed << " test(s) FAILED\n";
    return EXIT_FAILURE;
  }

  std::cout << "\nAll tests passed!\n";
  return EXIT_SUCCESS;
}
