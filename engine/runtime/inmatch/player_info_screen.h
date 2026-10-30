#pragma once

// The PLAYER_INFO screen (player.mnu): what the game's code does with its controls by NAME, the
// screen controller the menu shell hands every document holding NATIONALITY and COMBO_LIST
// (menu::MenuCommands owner "player_info"). Its registrations [orig: PlayerInfo_RegisterAllControls
// @0x561470]: the side radios, the nationality -> division -> combo -> voice cascade, the voice
// preview, the class pick, the loadout's weapon / count / type / grenade picks, the name edit's
// rule and the ACCEPT's save; its populate [orig: PlayerInfo_InitProfileSelector @0x5611b0 ->
// PlayerInfo_PopulateAllControls @0x5606f0]: the shown side's lists and its saved selection, the
// class, the auto boxes, the name, then the loadout and the side's kit page
// [orig: PlayerInfo_PopulateWeaponSlotLists @0x560430]. The models are the engine's
// (menu::PlayerInfoAvatars, menu::PlayerInfoLoadout, menu/player_info_kit.h, the character memory
// PlayerInfoCharacterState, the profile store profile::PlayerProfiles); this holds their order. It
// lives in inmatch because the character memory reads the character registry (the packed id is
// the wire's). The device work (the PLAYER_PREVIEW portrait, the slot icons, the voice's sound)
// is the host's (PlayerInfoScreenHost). Witness record: docs/playerinfo/avatars-re.md.

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <formats/avatars/avatars.h>
#include <formats/def/def.h>
#include <formats/playersav/weapon_sav.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/inmatch/character_registry.h>
#include <runtime/inmatch/player_info_character.h>
#include <runtime/menu/menu_screen_controller.h>
#include <runtime/menu/player_info_avatars.h>
#include <runtime/menu/player_info_loadout.h>

namespace opennova::profile {
class PlayerProfiles;
}

namespace opennova::inmatch {

// The dialog as its save reads it back (the ACCEPT, a side radio, a class pick): the character
// half (the shown side's selection remembered, the class stamped across both sides) and, with a
// weapon table loaded, the loadout and the side's kit page.
struct PlayerInfoSnapshot {
	bool has_name = false; // PLAYERNAME is authored
	std::string name; // its text as the record takes it (the code page's bytes)
	int team = 0; // the shown side: 0 blue, 1 red
	int nationality = -1; // the Avatars.def tree indices the lists show
	int division = -1;
	int combo = -1;
	int player_class = 8; // PLAYERCLASS's value, else the side's remembered class, else 8
	std::optional<PlayerInfoSideSelection> current; // the shown side, when it resolves
	std::optional<PlayerInfoSideSelection> sides[2];
	bool has_loadout = false; // a weapon table is loaded
	int32_t weapon[3] = { -1, -1, -1 }; // PRIMARY, SECONDARY, ACCESSORY: weapon.def rows, -1 NONE
	int32_t clips[3] = { -1, -1, -1 }; // the recorded clip picks, -1 untouched
	int32_t ammo_type[2] = { 0, 0 }; // PRIMARY, SECONDARY: the team's type byte
	std::vector<playersav::KitEntry> kit; // the page in retail's order
	int32_t kit_class = 0; // the page the populate restored
};

// What the screen asks of its embedder.
class PlayerInfoScreenHost {
public:
	virtual ~PlayerInfoScreenHost() = default;
	// The screen opens: the tables it reads (PlayerInfoScreen::set_avatars, set_weapons,
	// set_profiles, set_text) are given now, ahead of the populate.
	virtual void opening() {}
	// The populate ran: the device mounts (the portrait over PLAYER_PREVIEW, the slot icons).
	virtual void opened() {}
	// Another document took the menu: the mounts go.
	virtual void closed() {}
	// The portrait shows another combo (PlayerInfoScreen::preview_combo).
	virtual void preview_changed() {}
	// The pointer came onto or left PLAYER_PREVIEW: the portrait's zoom and sway
	// [orig: PlayerInfo_UpdatePlayerPreviewAnimation @0x55dba0, the active test].
	virtual void preview_hovered(bool hovered) { (void)hovered; }
	// The loadout's selections changed: the slot icons follow (PlayerInfoScreen::selected_weapon).
	virtual void loadout_changed() {}
	// A screen shown: the mounts follow their widgets.
	virtual void relayout() {}
	// TESTPLAYERVOICE: the voice's sound plays.
	virtual void play_voice(const std::string &bank, const std::string &trigger) {
		(void)bank;
		(void)trigger;
	}
	// The ACCEPT saved the dialog into the record: the selection for the next spawn.
	virtual void chosen(const PlayerInfoSnapshot &snapshot) { (void)snapshot; }
	// The record refused the dialog's save (a value out of its range): the record is untouched.
	virtual void save_refused() {}
};

class PlayerInfoScreen : public menu::MenuScreenController {
public:
	PlayerInfoScreen();

	const char *owner() const override { return "player_info"; }
	void open(menu::MenuRuntime &menu, const std::string &file, const std::string &screen) override;
	void on_event(menu::MenuRuntime &menu, const menu::MenuEvent &event) override;
	void close(menu::MenuRuntime &menu) override;

	void set_host(PlayerInfoScreenHost *host) { host_ = host; }
	// The avatar table (null: none, the lists stay empty), the embedder's while the screen holds
	// it.
	void set_avatars(const avatars::AvatarsFile *file);
	// The weapon table (null: none, no loadout); `loaded` false is a table no load filled, which
	// the lists show but the snapshot carries no loadout of. A table that changes drops the count
	// picks (keyed by the old table's rows), and an open screen fills its loadout again.
	void set_weapons(const def::DefWeaponsFile *weapons, bool loaded);
	// The profile store whose current record the screen reads and writes (null: none).
	void set_profiles(profile::PlayerProfiles *profiles) { profiles_ = profiles; }
	// The game-text, menu-text and the menu shell's own (Game.bin) tables; an empty lookup answers
	// its fallback.
	void set_text(
			hud::GameTextLookup gametext, hud::GameTextLookup menutxt, hud::GameTextLookup gameui);
	// The persisted profile the screen opens over (PlayerInfoCharacterState::set_persisted).
	void set_persisted(std::optional<int> team, std::optional<int> player_class,
			const std::optional<PlayerInfoSideSelection> (&sides)[2]);

	// --- read seams (while open) ---
	bool is_open() const { return menu_ != nullptr; }
	int team() const { return team_; }
	// The NATIONALITY list's rows as nationality indices, in row order.
	const std::vector<int32_t> &nationality_rows() const { return avatars_.nationalities; }
	int nationality() const { return avatars_.nationality; }
	int division() const { return avatars_.division; }
	// The combo the portrait shows: its index in the shown division, -1 for none.
	int preview_combo() const;
	// The shown side's voice override in the current record (0 DEFAULT_VOICE).
	int selected_voice() const;
	// A category's selection (weapon.def row, -1 NONE), its recorded clip pick and its type byte.
	int32_t selected_weapon(const std::string &control) const;
	int32_t selected_clips(const std::string &control) const;
	int32_t selected_ammo_type(const std::string &control) const;
	// The page the populate restored.
	int32_t kit_class() const { return loadout_.page_class; }
	// The kit page in retail's order (empty without a loaded table).
	std::vector<playersav::KitEntry> kit_entries() const;
	// The dialog's snapshot; it remembers the shown side's selection (the save's character half).
	PlayerInfoSnapshot snapshot();
	// The ACCEPT: the dialog saved into the record, the snapshot handed to the host.
	void commit();

private:
	void populate_side_();
	void update_avatars_(menu::PlayerInfoAvatars::Change change, int value);
	void restore_character_selection_(int side);
	void restore_player_class_();
	void populate_auto_boxes_();
	void populate_name_();
	void on_name_changed_(const std::string &text);
	void populate_loadout_();
	void populate_slot_ammo_(const std::string &control);
	void update_weight_();
	void set_team_(int team);
	void populate_all_controls_();
	bool save_from_dialog_(PlayerInfoSnapshot &out);
	void on_value_changed_(const menu::MenuEvent &event);
	void on_activated_(const std::string &name);
	int selected_class_mask_() const;
	int selected_player_class_() const;
	std::string menu_text_(const char *key, const char *fallback) const;
	// A text of the open document as the record takes it, and back.
	std::string record_text_(const std::string &text) const;
	const def::DefWeaponsFile &weapons_file_() const;
	const avatars::AvatarsFile &avatars_file_() const;

	PlayerInfoScreenHost *host_ = nullptr;
	menu::MenuRuntime *menu_ = nullptr;
	const avatars::AvatarsFile *avatars_ptr_ = nullptr;
	CharacterRegistry registry_;
	const def::DefWeaponsFile *weapons_ = nullptr;
	bool weapons_loaded_ = false;
	profile::PlayerProfiles *profiles_ = nullptr;
	hud::GameTextLookup gametext_, menutxt_, gameui_;
	PlayerInfoCharacterState character_;
	menu::PlayerInfoAvatars avatars_;
	menu::PlayerInfoLoadout loadout_;
	int team_ = 0; // 0 blue / good, 1 red / evil
	bool opening_ = false;
	// Guards the handlers against the fills' own value changes.
	bool populating_ = false;
	// While a build or a side switch restores the shown side, the cascade's voice results are
	// held and only the last, the fill against the restored selection, is written.
	bool restoring_side_ = false;
	int restored_voice_ = 0;
};

} // namespace opennova::inmatch
