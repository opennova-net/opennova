#pragma once

#include <formats/def/def.h>
#include <formats/playersav/weapon_sav.h>
#include <runtime/hud/game_text_lookup.h>

#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova::menu {

class MenuRuntime;

// Recorded counts are keyed by the parent weapon-table index, including the
// secondary count for its subclass. An absent pick means the authored default.
using LoadoutAmmoCounts = std::unordered_map<int, int>;
using LoadoutParentIndices = std::array<int, 3>; // PRIMARY, SECONDARY, ACCESSORY; -1 = NONE
using WeaponAvailability = std::function<int(const std::string &)>;

// Populate the existing native menu surface without emitting user selections.
// PLAYER_INFO retains hidden rows; its ammo-type statics stay authored, show the
// saved byte's row and lock under NOAMMOTYPES, and the returned byte reflects that
// reset. The WEAPON screen's fill is armory_fill_ammo (below).
int player_info_fill_ammo(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const std::string &control, int parent, int saved_primary, int saved_secondary,
		int saved_type, const hud::GameTextLookup &text);

// First three filtered grenade definitions in table order, including positions
// whose controls are absent. Availability restricts the armory's count rows only
// after a grenade owns its position; it never compacts the returned indices.
std::vector<int> player_info_fill_grenades(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		int class_mask, int team_mask, const LoadoutAmmoCounts &saved,
		const hud::GameTextLookup &text);

// One loadout category list as its fill left it: the weapon.def row each list
// row carries, row 0 the NONE row (-1).
using LoadoutSlotRows = std::vector<int32_t>;

// Fill the category list `control` (PRIMARY / SECONDARY / ACCESSORY, weapon
// slot `slot`) and select its row 0: the slot filter's rows for the class and
// team masks (world::weapon_slot_indices), less any whose availability reads 0
// when `availability` is set (the WEAPON screen's map term), labelled
// (weapon_label) and in the screens' row order (armory_slot_order), under the
// NONE row `none_label` at row 0. An absent control fills nothing and returns
// no rows.
// [orig: PlayerInfo_PopulateWeaponSlotLists @0x560430 -- the NONE inserts
//  @0x56058f / @0x56059d / @0x5605ab; UI_PopulateThreeCategoryLists @0x566db0 --
//  the !g_ArmoryWeaponAvailability[i] skip @0x566e6b, NONE at row 0 @0x566f15..0x566f18;
//  the sorts loadout_labels.h armory_slot_order carries]
LoadoutSlotRows loadout_fill_slot(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const std::string &control, int slot, int class_mask, int team_mask,
		const WeaponAvailability &availability, const std::string &none_label,
		const hud::GameTextLookup &gametext);

// The weapon.def row a category list's selection carries; -1 for the NONE row,
// no selection or an absent control.
int32_t loadout_selected_weapon(const MenuRuntime &menu, const std::string &control,
		const LoadoutSlotRows &rows);

// The loadout screens' select of a saved page's weapon: the list row carrying the
// weapon.def row `weapon_row`, else no row at all
// [orig: UIList_SelectByValue @0x645240, which clears every row first, from
//  PlayerInfo_PopulateWeaponAccessoryAmmoUI @0x55eab2 / @0x55ea3d / @0x55ea79 and
//  UI_PopulateAmmoTypeComboBoxes @0x564b33].
void loadout_select_weapon_row(MenuRuntime &menu, const std::string &control,
		const LoadoutSlotRows &rows, int32_t weapon_row);

// The WEAPON screen's AMMO1 row + 1 for a category, -1 with no AMMO1 control, no
// selection or no rows (a read seam; the page carries the screen's counts,
// armory_serialize_page).
int armory_selected_clips(const MenuRuntime &menu, const std::string &control);

// Keep each screen's control-presence/visibility gates and summation order.
// Indices name the selected definitions; the menu supplies the UI state.
double player_info_screen_weight(const MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const LoadoutParentIndices &parents, const std::vector<int> &grenades,
		const LoadoutAmmoCounts &primary, const LoadoutAmmoCounts &secondary);
// The WEAPON screen's: each category's weapon weight, then its AMMO1 and AMMO2 terms
// while shown and selected, (row + 1) times the weapon's clip weight, then the grenade
// position's while shown and selected, its row times its clip weight, a float sum.
double armory_screen_weight(const MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const LoadoutParentIndices &parents, const std::vector<int> &grenades);

// --- the WEAPON screen's per-class loadout memory (D-MNU-9) -------------------------
// The game keeps a 2048-byte kit page per class, the screen's counts by weapon and its
// two ammo-type bytes in globals that outlive the screen and the mission: the screen
// shows the selected class's page, a class flip saves the page it leaves, the ACCEPT
// saves and applies the page it shows, and a CANCEL saves nothing
// [orig: g_ArmoryLoadoutBufferByClass @0x25DD740 + 2048 * class; UI_HandleTeamClassSelection
//  @0x566f60; WeaponLoadout_ApplyFromBuffer @0x565cd0]. Witness record: docs/mnu/menu-re.md.

// The class pages: classes 0..15, the stage's 0x5800 bytes from class 5 reaching 15.
inline constexpr int kArmoryClassBuffers = 16;

// The team mask the screen filters by and stages under, from the entity's team word: 2 for
// a team of 1 or 3, else 1 [orig: Armory_ResolveSelectedClass @0x5642f0 - g_ArmoryTeamMask
// @0x564330..0x56434e].
inline int32_t armory_team_mask(int entity_team) {
	return entity_team == 1 || entity_team == 3 ? 2 : 1;
}

class ArmoryLoadoutBuffers {
public:
	// Armory_StageLoadoutBuffers: the record's side block (the blue for an entity team of 1
	// or 3, else the red) from its first class page on into class 5's page and up (its five
	// class pages into classes 5..9, then six pages of its tail, which the record does not
	// carry and no class the screen offers reads: classes 10..15 stage empty), and the
	// spawn buffer into class 1's. Every playing process's mission start runs it
	// (is_mp_session_peer, clear on a dedicated host alone), the on-show under its team gate
	// (stage_on_show).
	// [orig: Armory_StageLoadoutBuffers @0x564290 - the side @0x564296..0x5642b3 (32774 * the
	//  red side's flag), the copies @0x5642d2 (0x5800 bytes into 0x25DFF40) / @0x5642e3
	//  (g_SpawnLoadoutBuffer into 0x25DDF40); Game_StartMission's call @0x526195 behind
	//  is_mp_session_peer @0x526189]
	void stage(const playersav::Record &record, int entity_team, const playersav::KitPage &spawn);
	// The on-show's gate: a stage when the team mask differs from the one the last on-show
	// staged under (none at the start), which it keeps; true when it staged
	// [orig: UI_InitTeamClassSelection @0x567370 - g_ArmoryStagedTeamMask (0xFFFFFFFF in the image)
	//  against g_ArmoryTeamMask @0x56740c, the store @0x567414, the stage @0x567419].
	bool stage_on_show(const playersav::Record &record, int entity_team,
			const playersav::KitPage &spawn);
	// A class's page; a class outside 0..15 reads an empty page and stores nothing.
	const playersav::KitPage &page(int player_class) const;
	void store(int player_class, playersav::KitPage page);

private:
	std::array<playersav::KitPage, kArmoryClassBuffers> pages_{};
	int32_t staged_team_mask_ = -1;
};

// The screen's counts by catalog row (weapon.def's row + 1): the AMMO1 count and the
// AMMO2 count, -1 untouched; and the PRIMARY / SECONDARY ammo-type bytes, zero at the
// start, which no populate resets
// [orig: g_ArmoryAmmoCounts / dword_25DCF4C, the 255 pairs up to 0x25DD744; g_ArmoryAmmoTypePri /
//  g_ArmoryAmmoTypeSec].
struct ArmoryScreenPairs {
	std::vector<int32_t> clips;
	std::vector<int32_t> secondary;
	int32_t type[2] = { 0, 0 };
	int32_t clips_of(int32_t catalog_row) const;
	int32_t secondary_of(int32_t catalog_row) const;
	// The handlers' writes: an AMMO1 or AMMO2 pick its row + 1, a grenade pick its row
	// [orig: UI_OnPrimaryAmmoCountSelected / UI_OnSecondaryAmmoCountSelected / UI_OnAccessoryAmmoCountSelected (offset 0 AMMO1, 1 AMMO2),
	//  WeaponDef_UISlotSelectCallback @0x566d70; the TYPE picks UI_OnPrimaryAmmoTypeSelected / UI_OnSecondaryAmmoTypeSelected].
	void set_clips(int32_t catalog_row, int32_t value);
	void set_secondary(int32_t catalog_row, int32_t value);
};

// UI_PopulateAmmoTypeComboBoxes over a page: each category list at its NONE row, the pairs
// reset and seeded by the page's walk, which is the PLAYER screen's (player_info_restore_kit),
// each category list selected by the row its last entry names (one the list lacks leaves it
// with none), the ammo-type bytes a PRIMARY / SECONDARY entry carried, then each category's
// ammo fill (armory_fill_ammo) over its selection. Returns the three grenade positions the walk
// left, as weapon.def rows (-1 for the "None" catalog row): zeroed, then the page's grenades in
// page order, with no bound (a fourth lands in the "None" row's count, the walk's own); the
// grenade fill then overwrites the positions it fills (ArmoryLoadout::populate).
// [orig: UI_PopulateAmmoTypeComboBoxes @0x564930 - the zeroing @0x564951..0x56495d, the append
//  @0x564b57; its twin sub_563890 @0x563890 - @0x5638b1..0x5638bd, @0x563af3 (dfx2.exe)]
// [orig: UI_PopulateAmmoTypeComboBoxes @0x564930 - the NONE selects @0x56499e..0x5649e8, the
//  pair reset @0x564963..0x56497d, the walk @0x5649ed..0x564b71 (the name lookup
//  EntityDef_FindIndexByName @0x564a00..0x564a06, the category switch @0x564afe - the
//  ACCESSORY case @0x564b47 -, the selects UIList_SelectByValue @0x564b33, the type bytes
//  @0x564b0b / @0x564b40), the PRIMARY / SECONDARY / ACCESSORY legs @0x564b86..0x565454]
std::vector<int> armory_populate_page(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const LoadoutSlotRows (&rows)[3], const playersav::KitPage &page, ArmoryScreenPairs &pairs,
		const hud::GameTextLookup &text);

// A category's ammo controls over its selected weapon `parent` (weapon.def row, -1 NONE):
// with no weapon or none with a clip, AMMO1, the TYPE and AMMO2 hide, their rows kept; else
// AMMO1 shows its 1..maxclips rows and selects the one its count names (the last for an
// untouched count, none for another), the TYPE (PRIMARY / SECONDARY) shows at its byte's row
// (0 for -1) and locks under NOAMMOTYPES, which zeroes the byte; then AMMO2 shows the
// weapon's other-round subclass's rows by its second count, or hides
// [orig: the PRIMARY leg @0x564b86..0x564e7f (the gate @0x564be2, the hide
//  @0x564cff..0x564d3f, the TYPE row @0x564c25, NOAMMOTYPES @0x564c49..0x564c58, the rows
//  @0x564c7d..0x564ce4, AMMO2 @0x564d46..0x564e64), the SECONDARY leg @0x564e90.., the ACCESSORY leg @0x565190..; the
//  category handlers' fill UI_OnPrimaryWeaponTypeChanged @0x5662d0 and its siblings]
void armory_fill_ammo(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		const std::string &control, int parent, ArmoryScreenPairs &pairs,
		const hud::GameTextLookup &text);

// The grenade positions: the first three selectable class-3 definitions in table order on
// GRENADE_AMMO1..3, each shown with its 0..maxclips rows (only the zero row when its
// availability reads 0) and the row its count names selected (the def's maxclips for an
// untouched count, none for one past its rows), the controls past them hidden. Returns the
// positions' weapon.def rows.
// [orig: UI_PopulateVehicleAmmoSlotList(3) @0x5643c0 - the gates @0x5647eb, the position
//  @0x5647f7, the show @0x56481f, the rows @0x564837..0x564893, the select @0x564880, the
//  hides @0x5648cf..0x564906]
std::vector<int> armory_fill_grenades(MenuRuntime &menu, const def::DefWeaponsFile &weapons,
		int class_mask, int team_mask, const ArmoryScreenPairs &pairs,
		const WeaponAvailability &availability, const hud::GameTextLookup &text);

// WeaponLoadout_SerializeSelectionsToBuffer: the page the screen shows, as the game
// writes it into the class's buffer: the team's knife (WPN_KNIFE2 for an entity team of 2
// or 4), the medic's medpack (class 5), each category's selected weapon with its two
// counts and its ammo-type byte (-1 for ACCESSORY), then each shown grenade position's
// weapon with its count, every other value -1. With `every_position` (Title::
// death_screen_loadout, DFX2-38) a position writes whenever its weapon resolves, its control
// shown, hidden or absent, as dfx2.exe's serializer reads no grenade control.
// [orig: WeaponLoadout_SerializeSelectionsToBuffer @0x5658b0 - the knife @0x5658ca..0x56595b
//  ("WPN_KNIFE" @0x7C3584, "-1" @0x7C3328), the medpack @0x5659fc..0x565a8d, the categories
//  @0x565a97..0x565bab (AdmDef_GetEntryByIndex @0x565acb, the counts @0x565b10 /
//  @0x565b3d, the bytes @0x565b64..0x565b8d), the grenades @0x565bbc..0x565cb5 (the shown
//  gate @0x565bde, the three positions' bound @0x565cb2)]
// [orig: WeaponLoadout_SerializeSelectionsToBuffer @0x564850 (dfx2.exe) - the positions
//  @0x564b5c..0x564c20, AdmDef_GetEntryByIndex @0x564b66 its one gate]
playersav::KitPage armory_serialize_page(const MenuRuntime &menu,
		const def::DefWeaponsFile &weapons, const LoadoutSlotRows (&rows)[3],
		const std::vector<int> &grenades, const ArmoryScreenPairs &pairs, int entity_team,
		int selected_class, bool every_position = false);

} // namespace opennova::menu
