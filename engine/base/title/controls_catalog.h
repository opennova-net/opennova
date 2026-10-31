#pragma once

// The static input-binding catalog's plain data, a title's own (Title::controls): the action
// classes, one catalog row and a title's catalog of them, with its by-code and by-token reads. The
// Title holds it and a game's title file writes its rows (games/dfx2/dfx2_title.cpp), so it lives
// beside the Title, below runtime/; the binding model over it (Jointops.exe's catalog, the
// formatters, the CONTROL_MAPPING rows, the live records) is runtime/controls (controls.h,
// binding_set.h).

#include <cstddef>
#include <cstdint>

namespace opennova::controls {

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

// One catalog action. [orig: aAbsoluteTurnLe @ 0x8159cb, 108-byte stride]
struct ActionDef {
	int id;            // catalog index
	const char *token; // config key ("move_forward")
	const char *name;  // display name, marker-stripped ("Forward")
	ActionClass cls;   // category / Class column
	int default_key;   // default primary keyboard VK code (0 = unbound)
	int default_key2;  // default secondary keyboard VK code (0 = none)
	// The witnessed per-entry flag word (the static catalog's flags dword,
	// read at 0x8159AC + 108*id; it reaches the profile record at +1816 and
	// the UI table's gate word): bit 0x800 = player-visible in the remap
	// table, bit 0x20 = force-hidden, bit 0x4000000 = included in the
	// default.key filtered table [orig: UI_PopulateControlMappingList
	// @ 0x55c0c0; KeyBinding_BuildFilteredTable @ 0x54c2b0;
	// UI_BuildKeyBindingLoadoutTable @ 0x559e50].
	uint32_t flags;
	// The slot-1 modifier VK the catalog defaults carry (row base 0x8159A8 +
	// 108*id: +28 = slot-1 modifier, +30 = slot-2 modifier, zero on every row;
	// +24 is the MOUSE mask and +32 the mouse modifier — the "+24" below is
	// measured from the row's FLAGS word @0x8159AC): 17 = VK_CONTROL on seat1..seat10,
	// ScopeZeroInc, nvggainup/down, gtalk, sqtalk, respawn, command2 and
	// hudcolor. The keyboard dispatcher's first pass matches only rows whose
	// modifier key is held; modifier-less rows fire from its fallback pass, so
	// Ctrl+1 reaches seat1 while a bare 1 reaches Knife
	// [orig: Input_ProcessKeyboardEvents @0x49d35b..0x49d3a7 (modifier pass),
	//  @0x49d3ba..0x49d488 (fallback); seat1's row @0x8160D8 +24 = 0x11].
	// Rows that omit the field aggregate-initialize it to 0.
	int default_mod = 0;
	// Static row +24/+26/+32/+34: mouse mask, joystick binding (1-based
	// button or 0x81..0x84 direction), mouse modifier VK, joystick modifier.
	// [orig: KeyBinding_BuildFilteredTable @0x54c2b0]
	uint16_t default_mouse = 0;
	uint8_t default_joy = 0;
	uint16_t default_mouse_mod = 0;
	uint8_t default_joy_mod = 0;
	uint32_t modes = 3; // +8: alive bit 1, death-screen bit 2 [orig: @0x497ea0]
	// Static row +16: the row's help text id, which rides the player profile's
	// binding table (entry +20) [orig: KeyBinding_BuildFilteredTable @0x54c2b0
	// @0x54c307].
	uint32_t help = 0;
	// Static row +0x00: the action code the row's record dispatches, the case
	// Input_HandleActionBinding switches on (e.g. MedicReq 217, escape 18, the
	// weapon rows 200..210), and the slot the boot's re-lay puts the record in.
	// [orig: g_BindingRows + 108 * row]
	int code = 0;
};

// A title's static catalog: its executable's rows in their static order. Each
// title's table omits its one unreachable row (code 36, flags 0, mode 3, class 0,
// "!Null", an empty token and no key: no console name walk meets it, the CRC walks
// meet an earlier zero record first and nothing queues 36 by constant, so its
// case's exit reason 5 never runs). Every by-code reader resolves through
// for_code: the boot's sort, run once at start-up, re-lays the 768-record binding
// table so record i holds the static row whose code is i (a code no row carries
// leaves the zero record there, whose display string is empty): the dispatcher's
// flags word, the death screen's MedicReq record 217, the HUDLS key label's
// record 200 + category.
// [orig: row 66 @0x817580; Input_HandleActionBinding @0x49AD40 - case 36 @0x49B780,
//  the flags read @0x49ad8d; KeyBinding_SortBySequentialId @0x498260
//  (Game_InitSubsystems @0x4a6de7), its walk to aNull @0x829DA8 (768 rows);
//  g_BindingRowMedicReq @0x81B534 = record 217; HUD_DrawWeaponSlotBar
//  @0x599e8c..0x599e9f] [orig: row 64 @0x8163A0; KeyBinding_SortBySequentialId
//  @0x498270, its walk to aNull @0x828CA0 (dfx2.exe)]
struct Catalog {
	const ActionDef *rows = nullptr;
	std::size_t count = 0;
	// The row whose code is `code`, nullptr for none (or past the 768 records).
	const ActionDef *for_code(int code) const;
	// The row whose config token is `token`, compared exactly; nullptr for none.
	const ActionDef *for_token(const char *token) const;
};

} // namespace opennova::controls
