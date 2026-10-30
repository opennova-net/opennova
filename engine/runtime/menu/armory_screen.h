#pragma once

// The in-game armory: weapon.mnu's WEAPON screen as the game drives it by control NAME
// (menu_screen_controller.h). The code registers exactly these controls on the WEAPON screen,
// once, from the screen's INIT, and fills the class spin then
// [orig: UI_InitWeaponClassSelection @0x567250 -> WeaponDef_RegisterUICallbacks @0x567020]:
//   PLAYER_CLASS (spin)                      -> UI_HandleTeamClassSelection @0x566f60
//   PRIMARY / SECONDARY / ACCESSORY (combos) -> UI_OnPrimaryWeaponTypeChanged @0x5662d0 /
//                                               UI_OnSecondaryWeaponChanged @0x566670 /
//                                               UI_OnWeaponAmmoSlotChanged @0x566a10
//   the _AMMO1 / _AMMO2 / _AMMO1_TYPE and GRENADE_AMMO1..3 combos -> the count and type writes
//   ACCEPT / CANCEL (buttons)                -> WeaponLoadout_ApplyFromBuffer @0x565cd0 (arg 0 / 1)
// Each show runs the on-show [orig: UI_InitTeamClassSelection @0x567370]: the class resolved and
// the spin's rows enabled by the host's class mask and selected by value, the class pages staged
// under the team gate, the class pair shown in a session, ACCEPT's hotkeys set to the use-item
// keys, then the three category lists and the class's page over them, the grenade positions and
// the weight readout with the slot icons (UI_PopulateThreeCategoryLists @0x566db0, whose tail is
// UI_UpdateWeaponWeightDisplay @0x565640). The screen's state, the per-class pages among it, is
// ArmoryLoadout (armory_loadout.h, D-MNU-9), which this controller holds for the process.
//
// The screen is a live overlay over play, drawn after the HUD, the world ticking under it (in an
// MP session the team scoreboard draws over it) [orig: Render_ProcessMainSceneFrame @0x5ca0f0 -
// the scoreboard @0x5cae1c].
//
// What the screen reads of the running game, and what its ACCEPT does to it, is the host's
// (ArmoryScreenHost): the embedder serves it over its world. The icons are the embedder's to
// draw (the slots' loadout_menu_icon names, ArmoryIcons).

#include <runtime/hud/game_text_lookup.h>
#include <runtime/menu/armory_loadout.h>
#include <runtime/menu/menu_commands.h>
#include <runtime/menu/menu_screen_controller.h>

#include <cstdint>
#include <string>

namespace opennova::menu {

// The file and the screen a mission opens the armory by [orig: UI_OpenMenuScreen("weapon.mnu",
// "WEAPON") @0x49b8de].
inline constexpr const char *kArmoryMenuFile = "weapon.mnu";
inline constexpr const char *kArmoryScreenName = "WEAPON";

// The icon each category's *_ICON window shows: the selected weapon.def row's
// loadout_menu_icon (+144), "" for NONE, no selection or no table; PRIMARY, SECONDARY,
// ACCESSORY in turn. GRENADE_ICON keeps its authored static
// [orig: UI_UpdateWeaponWeightDisplay @0x5657a8..0x5658a3, the icon table @0x2540D70].
struct ArmoryIcons {
	std::string slot[3];
	bool operator==(const ArmoryIcons &other) const {
		return slot[0] == other.slot[0] && slot[1] == other.slot[1] && slot[2] == other.slot[2];
	}
};

class ArmoryScreenHost {
public:
	virtual ~ArmoryScreenHost() = default;
	// The loadout catalog (weapon.def; null: none, the lists stay empty).
	virtual const def::DefWeaponsFile *weapons() = 0;
	// The text tables: gametext (the WepDes names and rounds), the menu's own resource
	// (Game.bin: NONE, CHARCLASS_*) and menutxt (the weight tokens ahead of Game.bin).
	virtual hud::GameTextLookup gametext() = 0;
	virtual hud::GameTextLookup menu_ui() = 0;
	virtual hud::GameTextLookup menutxt() = 0;
	// The local player as the show reads it: its entity team (1 / 3 blue, 2 / 4 red), its class
	// (5..9; 0 unclassed) and the host's class mask (the S2C 0x76 u16, 0x3FF none received).
	virtual int entity_team() = 0;
	virtual int player_class() = 0;
	virtual uint32_t class_allow_mask() = 0;
	// Whether the class pair (PLAYER_CLASS, STATIC_PLAYER_CLASS) shows: retail's in a network
	// session alone; OpenNova's in offline play too (D-MNU-10).
	virtual bool class_selection() = 0;
	// The stage's sources: the profile's current weapon.sav record (null: an empty record) and
	// the spawn buffer (the sim's resident kit).
	virtual const playersav::Record *profile_record() = 0;
	virtual playersav::KitPage spawn_page() = 0;
	// A weapon's availability (0 banned, which drops it from the lists); empty: all allowed.
	virtual WeaponAvailability availability() = 0;
	// The USE-ITEM press latch (g_WeaponScreenOpenDebounce): while held ACCEPT and CANCEL do
	// nothing (world::LocalPlayer::armory_press_latched).
	virtual bool press_latched() = 0;
	// ACCEPT's and CANCEL's head past the latch: every input state reset (Input_ResetAllState,
	// inmatch Role::reset_all_input_state).
	virtual void reset_all_input_state() = 0;
	// ACCEPT's apply of the class's page (in a session the 0x2F submit alone, out of one the
	// slot rebuild and the select); false when nothing applied.
	virtual bool apply_loadout(const playersav::KitPage &kit, int player_class) = 0;
	virtual bool in_session() = 0;
	// An out-of-session apply's tail: the viewmodel shown for the slot the apply mounted.
	virtual void present_equipped() = 0;
	// The screen closes (g_WeaponScreenOpen cleared, Game_CloseInGameScreens).
	virtual void close_screen() = 0;
	// The slot icons changed (the weight readout's refresh).
	virtual void icons_changed(const ArmoryIcons &icons) { (void)icons; }
};

class ArmoryScreen : public MenuScreenController {
public:
	void set_host(ArmoryScreenHost *host) { host_ = host; }

	// The mission start's stage of the class pages from the profile's record and the spawn
	// buffer, which every playing process runs (a dedicated host has no screen)
	// [orig: Game_StartMission's Armory_StageLoadoutBuffers @0x526195 behind is_mp_session_peer
	// @0x526189].
	void stage_mission_start();

	void open(MenuRuntime &menu, const std::string &file, const std::string &screen) override;
	void on_event(MenuRuntime &menu, const MenuEvent &event) override;
	void close(MenuRuntime &menu) override;
	bool is_open() const { return menu_ != nullptr; }

	// ACCEPT (a click, or the hotkey accelerator: the use-item keys the on-show registers) and
	// CANCEL, past the press latch: the input reset, then ACCEPT's page serialized into the
	// selected class's buffer and applied (CANCEL applies nothing), then the close
	// [orig: WeaponLoadout_ApplyFromBuffer @0x565cd0 - the gate @0x565cf8, Input_ResetAllState
	// @0x565d01, skip_apply @0x565d0e, the serialize @0x565d22, the session test @0x565d4d, the
	// close @0x56629f..0x5662a9]. Nothing with the screen closed.
	void accept();
	void cancel();

	// The class the lists, the shown page and ACCEPT run against (g_ArmorySelectedClass): the
	// on-show's resolve, a flip's pick. An unclassed resolve keeps it outside 5..9 while the
	// spin shows row 0.
	int selected_class() const { return selected_class_; }
	// A category list's selected weapon.def row (-1 NONE, no selection, closed screen).
	int32_t selected_weapon(const std::string &control) const;
	// A category's AMMO1 row + 1, -1 with none (armory_selected_clips).
	int selected_clips(const std::string &control) const;
	const ArmoryIcons &icons() const { return icons_; }
	// The class pages, the counts and the type bytes (D-MNU-9).
	const ArmoryLoadout &loadout() const { return loadout_; }
	// The last ACCEPT's page (what it handed apply_loadout).
	const playersav::KitPage &last_accepted() const { return accepted_; }

private:
	void fill_classes_(MenuRuntime &menu);
	void populate_(MenuRuntime &menu);
	void flip_class_(MenuRuntime &menu, int row);
	void refresh_weight_(MenuRuntime &menu);
	bool apply_from_buffer_(bool skip_apply);
	std::string none_label_();

	ArmoryScreenHost *host_ = nullptr;
	MenuRuntime *menu_ = nullptr;
	ArmoryLoadout loadout_;
	MenuCommands commands_;
	ArmoryIcons icons_;
	playersav::KitPage accepted_;
	int entity_team_ = 1;
	int selected_class_ = 0;
	bool populating_ = false;
};

} // namespace opennova::menu
