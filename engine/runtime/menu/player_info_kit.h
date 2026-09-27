#pragma once

// The PLAYER_INFO screen's kit model (ADR 0040 ladder E4): the voice-list
// rows and the persisted-override reset, and the weapon.sav kit page in the
// order retail serializes it. The masks, slot filters, clip rows and weights
// live in runtime/world/player_loadout.h and formats/def; native menu
// controllers apply these to MenuRuntime, while the shell supplies resources.

#include <formats/playersav/weapon_sav.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace opennova::menu {

// --- the PLAYERVOICE list --------------------------------------------------------

// One row of the voice table: enabled, the CHARVOICE id, and the sex it
// belongs to (0 male / 1 female — the head part's `sex` keyword).
// [orig: the 11 12-byte rows based at 0x83C7A8; the walk starts at the id
//  word 0x83C7AC, strides 12 and stops at g_PlayerPreviewAnimTable @ 0x83C830, testing
//  `*(ptr - 1) != 0 && avatar_sex == ptr[1]`; row id 9 is the one DISABLED row]
struct PlayerVoiceRow {
	bool enabled;
	int32_t id;
	int32_t sex;
};
inline constexpr PlayerVoiceRow kPlayerVoiceTable[11] = {
	{ true, 1, 0 }, { true, 2, 0 }, { true, 3, 0 }, { true, 4, 0 }, { true, 5, 0 },
	{ true, 6, 0 }, { true, 7, 1 }, { true, 8, 1 }, { false, 9, 0 }, { true, 10, 0 },
	{ true, 11, 1 },
};

// The DEFAULT_VOICE row's value, and the value a rejected persisted override
// falls back to [orig: UIList_AddRow(list, DEFAULT_VOICE, 0, 0, -1) @ 0x55dd76
// and `if (!found) profile[team + 1532] = 0` @ 0x55de21].
inline constexpr int32_t kDefaultVoiceValue = 0;

// The values the PLAYERVOICE list carries for a head's sex byte: DEFAULT_VOICE
// first, then every ENABLED table row of that sex, in table order
// [orig: PlayerInfo_PopulatePlayerVoiceCombo @ 0x55dce0].
std::vector<int32_t> player_info_voice_values(int32_t sex);

// The list selection: the persisted override when the list carries it, else
// DEFAULT_VOICE — the reset retail writes back to the profile [orig: @ 0x55de21
// -> UIList_SelectByValue(list, profile[team + 1532], 1)].
int32_t player_info_voice_selection(int32_t saved, const std::vector<int32_t> &values);

// --- the weapon.sav kit page -------------------------------------------------------

// The kit page's filler value: retail writes the literal string "-1" (@ 0x7C3328)
// for every count/flags slot it has no number for, and the reader decodes a
// missing value as -1 all the same.
inline constexpr int32_t kKitFiller = -1;
// The knife every page leads with, per side, then the medic's medpack
// [orig: "WPN_KNIFE" @ 0x7C3584 / "WPN_KNIFE2" @ 0x55e4ec / "WPN_MEDPACK" @ 0x7D5CA8].
inline constexpr const char *kKitKnifeBlue = "WPN_KNIFE";
inline constexpr const char *kKitKnifeRed = "WPN_KNIFE2";
inline constexpr const char *kKitMedpack = "WPN_MEDPACK";
// The PLAYERCLASS value that earns the medpack entry (retail tests == 5).
inline constexpr int32_t kMedicPlayerClass = 5;

// One serialized slot: the weapon-table index (0 = the NONE row, which retail
// serializes as weapon-table entry 0 — its own quirk), the recorded
// interleaved count pair (-1 = untouched) and the flags word (the team's
// ammo-type byte for PRIMARY/SECONDARY; the filler elsewhere).
struct KitSlotPick {
	int32_t weapon_index = 0;
	int32_t ammo_primary = kKitFiller;
	int32_t ammo_secondary = kKitFiller;
	int32_t flags = kKitFiller;
};

struct PlayerInfoKitSelection {
	int32_t team_mask = 0;   // player_info_team_mask(team): 2 blue / 1 red
	int32_t player_class = 0; // the PLAYERCLASS value 5..9
	KitSlotPick primary;
	KitSlotPick secondary;
	KitSlotPick accessory;
	KitSlotPick grenades[3]; // the FIXED three grenade slots (entry 0 when empty)
};

// The weapon-table name for an index ("" for an absent entry).
using WeaponNameLookup = std::function<std::string(int32_t index)>;

// The kit page exactly as retail serializes it: the side's knife, the medic's
// medpack, the three loadout categories, then ALWAYS three grenade slots
// [orig: PlayerInfo_SerializeWeaponLoadout @ 0x55e4b0 — knife first (@0x55e4dc picks
//  the blade off g_PlayerInfoTeamMask: the BLUE mask 2, and the defensive
//  mask-zero leg, take WPN_KNIFE, the RED mask 1 WPN_KNIFE2), the class-5
//  medpack block (@0x55e624), the PRIMARY/SECONDARY/ACCESSORY selections
//  (@0x55e6bb), then the fixed g_PlayerInfoGrenadeSlots[0..2] walk (@0x55e7e0,
//  bounded by g_PlayerInfoAmmoPriCounts @ 0x25DC560); the three grenade slots
//  are zeroed before the ammo fill, so an empty slot serializes weapon-table
//  entry 0 (the zero store @ 0x55e8d0-0x55e8da in
//  PlayerInfo_PopulateWeaponAccessoryAmmoUI @ 0x55e8b0)].
std::vector<playersav::KitEntry> player_info_kit_entries(const PlayerInfoKitSelection &selection,
		const WeaponNameLookup &weapon_name);

} // namespace opennova::menu
