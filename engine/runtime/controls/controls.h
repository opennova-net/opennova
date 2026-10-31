#pragma once

#include <base/title/controls_catalog.h> // ActionClass, ActionDef, Catalog (a title's own data)

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The input-binding catalog and the Options -> Controls (CONTROL_MAPPING) table model.
//
// A faithful port of the original engine's key-binding catalog and the per-device
// table population. The static action catalog (action code, display name, config token,
// category, default bindings) is the byte-witnessed table at
// [orig: aAbsoluteTurnLe @ 0x8159cb] (108-byte stride), each title's own: Jointops.exe's
// is default_catalog, dfx2.exe's its title's (Title::controls, title_catalog). Class names come from
// [orig: KeyBinding_BuildCategoryPages @ 0x4966c0]; VK key-name decoding from
// [orig: KeyBinding_GetKeyNameAndDisplayName @ 0x494c60]; binding formatting from
// [orig: KeyBinding_FormatBindingString @ 0x559a10]; the per-device row build from
// [orig: UI_PopulateControlMappingList @ 0x55c0c0].
//
// This models the static catalog and formatting; the LIVE records behind
// rebinding / DEFAULTS / CLEAR_KEY live in binding_set.h. Default key VALUES are the byte-exact
// catalog defaults (validated against the canonical JO scheme: Forward=W/Up,
// Reload=R, Jump=Space, ...); see docs/mnu/menu-re.md D-CTRL-* for the documented
// gaps (per-entry visibility flag and the mouse/joystick default arrays).

namespace opennova {
struct Title; // base/title/title.h
} // namespace opennova

namespace opennova::controls {

// Input device selector. [orig: dword_25DB7D8; UI_SelectControlsInputDevice @ 0x55bcd0]
enum class Device {
  Keyboard = 0,
  Mouse = 1,
  Joystick = 2,
};

// Marker-stripped English class name (the engine's "!Movement"-style fallback with
// the leading marker removed). [orig: KeyBinding_BuildCategoryPages @ 0x4966c0]
const char *action_class_name(ActionClass cls);

// Jointops.exe's catalog, the 119 static rows at g_BindingRows but row 66: the one a
// title with no catalog of its own runs (Title::controls null).
const Catalog &default_catalog();
// The catalog `title`'s executable compiles in: Title::controls, else
// default_catalog.
const Catalog &title_catalog(const Title &title);

// Every weapon row fires its category, the code - 200: Jointops.exe's Knife /
// Secondary / Primary / Flashbang / FragGrenade / SmokeGrenade / Accessory /
// Detonator / medpack 201..209; dfx2.exe's Powerup0 200, Knife / Secondary /
// Primary 201..203, Grenades 204, Accessory 205, Powerup6..9 206..209 and
// Detonator 210. Both executables' dispatchers carry the eleven cases;
// cycleweaponP / N (212 / 214) cycle.
// [orig: Input_HandleActionBinding_0 @0x4E0420 - cases 200-210 @0x4e1144 ->
//  Player_SwitchToWeaponByHandle((action-200)*65); cases 212/214 ->
//  Player_CycleWeaponSlot @0x4dfe70] [orig: Input_HandleActionBinding_0 @0x4DD4C0 - cases 200-210
//  @0x4de239..0x4de24d (dfx2.exe)]
inline constexpr int kWeaponCategoryCodeFirst = 200;
inline constexpr int kWeaponCategoryCodeLast = 210;
// Jointops.exe's nine categories by the code its rows fire (code - 200).
enum class WeaponCategory : int {
  kKnife = 1,
  kSecondary = 2,
  kPrimary = 3,
  kFlashbang = 4,
  kFragGrenade = 5,
  kSmokeGrenade = 6,
  kAccessory = 7,
  kDetonator = 8,
  kMedpack = 9,
};
// The HUDLS slot bar's key label reads binding record 200 + the weapon
// category (0..11) [orig: HUD_DrawWeaponSlotBar @0x599e8c..0x599e9f].
inline constexpr int kSlotBarKeyRecordBase = 200;

// Whether an action is shown in the player-facing remap table — the witnessed
// per-entry gate (*entry & 0x20) == 0 && (*entry & 0x800) != 0, applied to the
// catalog flag word (D-CTRL-2 closed: the class-category approximation is
// replaced) [orig: UI_PopulateControlMappingList @ 0x55c0c0].
bool is_player_visible(const ActionDef &action);

// The pair the VK switch writes: the "Keys" table lookup key (the binding
// name, "LBUTTON" / "F1" / "PRINT" / "W") and the untranslated display
// fallback ("XXMouse 1" / "XXF1" / "XXPrint Screen" / "W"). A printable VK
// is its own character in both; every other unlisted VK is
// "<KEY label> <vk>" in both (e.g. "Key 0", "Key 226") through the same table
// [orig: KeyBinding_GetKeyNameAndDisplayName @ 0x494c60 -- the default arm
//  @0x496235..0x49629f].
struct KeyNames {
  std::string binding;  // the "Keys" lookup key
  std::string display;  // the fallback the lookup returns on a miss
};
KeyNames key_binding_names(int vk);

// Windows VK code -> the label every formatter appends: the "Keys" table entry
// for the binding name, else the display fallback ("Mouse 1", "Up", "Space",
// "F1", "W", "Print Screen", "Key 226", ...) [orig: the
// KeyHelp_GetStringWithFallback("Keys", keyName, displayName) calls
// @0x496d61/@0x496f01/@0x559b61 over KeyBinding_GetKeyNameAndDisplayName
// @ 0x494c60; key_strings.h].
std::string key_name(int vk);

// Format a binding for the Control column: up to two key slots, each behind
// its "Ctrl-"/"Shift-" prefix (modifier word 17 / 16), the second joined by
// the localized "OR" separator. Every prefix, the separator and the key names
// come from the "Keys" table (key_strings.h). Empty when unbound.
// [orig: KeyBinding_FormatBindingString @ 0x559a10]
std::string format_binding(int key, int key2, int modifier = 0,
                           int modifier2 = 0);

// One produced display row for the CONTROL_MAPPING table.
struct ControlRow {
  std::string cls;      // Class column
  std::string action;   // Action column
  std::string control;  // Control column (formatted binding, "" when unbound)
};

// Build the Class/Action/Control rows of `catalog` for a device from its defaults,
// mirroring the engine's per-device table population.
// [orig: UI_PopulateControlMappingList @ 0x55c0c0]
std::vector<ControlRow> build_rows(const Catalog &catalog, Device device);

}  // namespace opennova::controls
