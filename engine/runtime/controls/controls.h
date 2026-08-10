#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Joint Operations input-binding catalog and the Options -> Controls (CONTROL_MAPPING)
// table model.
//
// A faithful port of the original engine's key-binding catalog and the per-device
// table population. The static action catalog (display name, config token, category,
// default keyboard binding) is the byte-witnessed table at
// [orig: aAbsoluteTurnLe @ 0x8159cb] (108-byte stride). Class names come from
// [orig: KeyBinding_BuildCategoryPages @ 0x4966c0]; VK key-name decoding from
// [orig: KeyBinding_GetKeyNameAndDisplayName @ 0x494c60]; binding formatting from
// [orig: KeyBinding_FormatBindingString @ 0x559a10]; the per-device row build from
// [orig: UI_PopulateControlMappingList @ 0x55c0c0].
//
// This models what the Controls tab DISPLAYS, read-only. Live rebinding, DEFAULTS /
// CLEAR, and profile persistence are deferred. Default key VALUES are the byte-exact
// catalog defaults (validated against the canonical JO scheme: Forward=W/Up,
// Reload=R, Jump=Space, ...); see docs/mnu/menu-re.md D-CTRL-* for the documented
// gaps (per-entry visibility flag and the mouse/joystick default arrays).

namespace opennova::controls {

// Input device selector. [orig: dword_25DB7D8; sub_55BCD0 @ 0x55bcd0]
enum class Device {
  Keyboard = 0,
  Mouse = 1,
  Joystick = 2,
};

// Action category id (the Class column). [orig: KeyBinding_BuildCategoryPages @ 0x4966c0]
enum class ActionClass {
  Null = 0,
  Movement = 1,
  Weapons = 2,
  Camera = 3,
  Map = 4,
  Communications = 5,
  Server = 6,
  NovaLogic = 7,
  Cheat = 9,
  System = 10,
  Debug = 11,
  TeammateMenu = 12,
  Spectator = 13,
};

// Marker-stripped English class name (the engine's "!Movement"-style fallback with
// the leading marker removed). [orig: KeyBinding_BuildCategoryPages @ 0x4966c0]
const char *action_class_name(ActionClass cls);

// One catalog action. [orig: aAbsoluteTurnLe @ 0x8159cb, 108-byte stride]
struct ActionDef {
  int id;             // catalog index
  const char *token;  // config key ("move_forward")
  const char *name;   // display name, marker-stripped ("Forward")
  ActionClass cls;    // category / Class column
  int default_key;    // default primary keyboard VK code (0 = unbound)
  int default_key2;   // default secondary keyboard VK code (0 = none)
  // The witnessed per-entry flag word (the static catalog's flags dword,
  // read at 0x8159AC + 108*id; it reaches the profile record at +1816 and
  // the UI table's gate word): bit 0x800 = player-visible in the remap
  // table, bit 0x20 = force-hidden, bit 0x4000000 = included in the
  // default.key filtered table [orig: UI_PopulateControlMappingList
  // @ 0x55c0c0; KeyBinding_BuildFilteredTable @ 0x54c2b0;
  // UI_BuildKeyBindingLoadoutTable @ 0x559e50].
  uint32_t flags;
};

// The full static catalog (pointer + element count).
const ActionDef *catalog(std::size_t *out_count);

// Whether an action is shown in the player-facing remap table — the witnessed
// per-entry gate (*entry & 0x20) == 0 && (*entry & 0x800) != 0, applied to the
// catalog flag word (D-CTRL-2 closed: the class-category approximation is
// replaced) [orig: UI_PopulateControlMappingList @ 0x55c0c0].
bool is_player_visible(const ActionDef &action);

// Windows VK code -> display key name ("Mouse 1", "Up", "Space", "F1", "W", ...).
// [orig: KeyBinding_GetKeyNameAndDisplayName @ 0x494c60]
std::string key_name(int vk);

// Format a binding for the Control column: the primary key, optionally joined to a
// secondary with " or ". Empty when unbound.
// [orig: KeyBinding_FormatBindingString @ 0x559a10]
std::string format_binding(int key, int key2);

// One produced display row for the CONTROL_MAPPING table.
struct ControlRow {
  std::string cls;      // Class column
  std::string action;   // Action column
  std::string control;  // Control column (formatted binding, "" when unbound)
};

// Build the Class/Action/Control rows for a device, mirroring the engine's per-device
// table population. Keyboard shows the byte-exact catalog defaults; mouse/joystick
// share the action list but have no static default bindings (D-CTRL-2).
// [orig: UI_PopulateControlMappingList @ 0x55c0c0]
std::vector<ControlRow> build_rows(Device device);

}  // namespace opennova::controls
