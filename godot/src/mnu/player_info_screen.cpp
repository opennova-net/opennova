#include "mnu/player_info_screen.h"

#include "mnu/mnu_document.h"

#include "object/avatar_database.h"
#include "object/avatar_records.h"
#include "object/weapon_database.h"
#include "player/player_profiles.h"
#include "rtxt/rtxt_string_file.h"
#include "util/string_convert.h"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <runtime/inmatch/join_character_profile.h>

namespace godot {

namespace {

using opennova::inmatch::PlayerInfoSideSelection;

Dictionary selection_to_dict(const PlayerInfoSideSelection &p_selection) {
	Dictionary out;
	out["team"] = p_selection.team;
	out["nationality"] = p_selection.nationality;
	out["division"] = p_selection.division;
	out["combo"] = p_selection.combo;
	out["player_class"] = p_selection.player_class;
	out["avatar_a"] = p_selection.avatar_a;
	out["avatar_b"] = p_selection.avatar_b;
	out["avatar_packed"] = p_selection.avatar_packed;
	return out;
}

// One side's persisted selection from its PERSISTED_* row; absent for an empty row.
std::optional<PlayerInfoSideSelection> selection_from_row(const PackedInt32Array &p_row) {
	if (p_row.size() < PlayerInfoScreen::PERSISTED_FIELD_COUNT)
		return std::nullopt;
	PlayerInfoSideSelection selection;
	selection.team = p_row[PlayerInfoScreen::PERSISTED_TEAM];
	selection.nationality = p_row[PlayerInfoScreen::PERSISTED_NATIONALITY];
	selection.division = p_row[PlayerInfoScreen::PERSISTED_DIVISION];
	selection.combo = p_row[PlayerInfoScreen::PERSISTED_COMBO];
	selection.player_class = p_row[PlayerInfoScreen::PERSISTED_PLAYER_CLASS];
	selection.avatar_a = p_row[PlayerInfoScreen::PERSISTED_AVATAR_A];
	selection.avatar_b = p_row[PlayerInfoScreen::PERSISTED_AVATAR_B];
	selection.avatar_packed = p_row[PlayerInfoScreen::PERSISTED_AVATAR_PACKED];
	return selection;
}

void put_kit(Dictionary &p_profile, const std::vector<opennova::playersav::KitEntry> &p_kit) {
	Array kit;
	for (const opennova::playersav::KitEntry &entry : p_kit) {
		Dictionary row;
		row["name"] = opennova::to_gd(entry.name);
		row["ammo_primary"] = entry.ammo_primary;
		row["ammo_secondary"] = entry.ammo_secondary;
		row["flags"] = entry.flags;
		kit.push_back(row);
	}
	p_profile["kit"] = kit;
}

// The ACCEPT's snapshot as the profile Dictionary the shell's spawn and the profile's load speak
// (the avatar_chosen signal's): the shown side's resolved selection (its raw indices when it does
// not resolve), the name, the class, both side records, and with a weapon table loaded the
// loadout keys (primary, primary_clips, primary_ammo_type, secondary, ..., accessory_clips, kit,
// kit_class).
Dictionary profile_of(const opennova::inmatch::PlayerInfoSnapshot &p_snapshot,
		const Ref<WeaponDatabase> &p_weapons) {
	Dictionary profile;
	if (p_snapshot.current) {
		profile = selection_to_dict(*p_snapshot.current);
	} else {
		profile["team"] = p_snapshot.team;
		profile["nationality"] = p_snapshot.nationality;
		profile["division"] = p_snapshot.division;
		profile["combo"] = p_snapshot.combo;
	}
	profile["name"] = opennova::cp1252_to_gd(p_snapshot.name);
	profile["player_class"] = p_snapshot.player_class;
	Array sides;
	for (const std::optional<PlayerInfoSideSelection> &side : p_snapshot.sides)
		sides.push_back(side ? selection_to_dict(*side) : Dictionary());
	profile["side_profiles"] = sides;
	if (!p_snapshot.has_loadout || p_weapons.is_null())
		return profile;
	const opennova::def::DefWeaponsFile &weapons = p_weapons->native_file();
	const auto weapon_name = [&weapons](int32_t p_row) -> String {
		return p_row >= 0 && static_cast<size_t>(p_row) < weapons.count
				? String(weapons.entries[p_row].weapon_name)
				: String();
	};
	static const char *const kSlots[3] = { "primary", "secondary", "accessory" };
	for (int i = 0; i < 3; ++i) {
		profile[kSlots[i]] = weapon_name(p_snapshot.weapon[i]);
		profile[String(kSlots[i]) + "_clips"] = p_snapshot.clips[i];
	}
	profile["primary_ammo_type"] = p_snapshot.ammo_type[0];
	profile["secondary_ammo_type"] = p_snapshot.ammo_type[1];
	put_kit(profile, p_snapshot.kit);
	profile["kit_class"] = p_snapshot.kit_class;
	return profile;
}

static_assert(PlayerInfoScreen::DEFAULT_PLAYER_CLASS == opennova::inmatch::kJoinDefaultPlayerClass,
		"DEFAULT_PLAYER_CLASS mirrors the engine's class default");

} // namespace

// The controller's requests, as the binding's signals (synchronous: the `opening` handler gives the
// tables before the populate reads them).
class PlayerInfoScreen::Host : public opennova::inmatch::PlayerInfoScreenHost {
	PlayerInfoScreen *owner_;

public:
	explicit Host(PlayerInfoScreen *p_owner) : owner_(p_owner) {}
	void opening() override { owner_->emit_signal("opening"); }
	void opened() override { owner_->emit_signal("opened"); }
	void closed() override { owner_->emit_signal("closed"); }
	void preview_changed() override { owner_->emit_signal("preview_changed"); }
	void preview_hovered(bool p_hovered) override {
		owner_->emit_signal("preview_hovered", p_hovered);
	}
	void loadout_changed() override { owner_->emit_signal("loadout_changed"); }
	void relayout() override { owner_->emit_signal("relayout"); }
	void play_voice(const std::string &p_bank, const std::string &p_trigger) override {
		owner_->emit_signal("voice_requested", opennova::to_gd(p_trigger), opennova::to_gd(p_bank));
	}
	void chosen(const opennova::inmatch::PlayerInfoSnapshot &p_snapshot) override {
		owner_->emit_signal("avatar_chosen", profile_of(p_snapshot, owner_->weapons_));
	}
	void save_refused() override {
		UtilityFunctions::push_warning(
				"PlayerInfoScreen: the dialog was not saved into the profile");
	}
};

PlayerInfoScreen::PlayerInfoScreen() : host_(std::make_unique<Host>(this)) {
	screen_.set_host(host_.get());
}

PlayerInfoScreen::~PlayerInfoScreen() {
	screen_.set_host(nullptr);
}

void PlayerInfoScreen::set_database(const Ref<AvatarDatabase> &p_db) {
	db_ = p_db;
	screen_.set_avatars(db_.is_valid() && db_->is_loaded() ? &db_->native_file() : nullptr);
}

void PlayerInfoScreen::set_weapon_database(const Ref<WeaponDatabase> &p_weapons) {
	weapons_ = p_weapons;
	screen_.set_weapons(weapons_.is_valid() ? &weapons_->native_file() : nullptr,
			weapons_.is_valid() && weapons_->is_loaded());
}

void PlayerInfoScreen::set_profiles(const Ref<PlayerProfiles> &p_profiles) {
	profiles_ = p_profiles;
	screen_.set_profiles(profiles_.is_valid() ? &profiles_->native() : nullptr);
}

void PlayerInfoScreen::set_text_tables(const Ref<RtxtStringFile> &p_gametext,
		const Ref<RtxtStringFile> &p_menutxt, const Ref<RtxtStringFile> &p_gameui) {
	gametext_ = p_gametext;
	menutxt_ = p_menutxt;
	gameui_ = p_gameui;
	screen_.set_text(
			game_text_lookup(gametext_), game_text_lookup(menutxt_), game_text_lookup(gameui_));
}

void PlayerInfoScreen::set_persisted(bool p_has_team, int p_team, bool p_has_class,
		int p_player_class, const PackedInt32Array &p_blue, const PackedInt32Array &p_red) {
	const std::optional<PlayerInfoSideSelection> sides[2] = { selection_from_row(p_blue),
		selection_from_row(p_red) };
	screen_.set_persisted(p_has_team ? std::optional<int>(p_team) : std::nullopt,
			p_has_class ? std::optional<int>(p_player_class) : std::nullopt, sides);
}

int PlayerInfoScreen::selected_weapon(const String &p_control) const {
	return screen_.selected_weapon(opennova::to_std(p_control));
}

Ref<AvatarComboRow> PlayerInfoScreen::preview_combo() const {
	const int combo = screen_.preview_combo();
	if (combo < 0 || db_.is_null())
		return {};
	return db_->get_combo(screen_.nationality(), screen_.division(), combo);
}

void PlayerInfoScreen::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_database", "db"), &PlayerInfoScreen::set_database);
	ClassDB::bind_method(D_METHOD("get_database"), &PlayerInfoScreen::get_database);
	ClassDB::bind_method(
			D_METHOD("set_weapon_database", "weapons"), &PlayerInfoScreen::set_weapon_database);
	ClassDB::bind_method(D_METHOD("get_weapon_database"), &PlayerInfoScreen::get_weapon_database);
	ClassDB::bind_method(D_METHOD("set_profiles", "profiles"), &PlayerInfoScreen::set_profiles);
	ClassDB::bind_method(D_METHOD("set_text_tables", "gametext", "menutxt", "gameui"),
			&PlayerInfoScreen::set_text_tables);
	ClassDB::bind_method(D_METHOD("set_persisted", "has_team", "team", "has_class", "player_class",
								 "blue", "red"),
			&PlayerInfoScreen::set_persisted);
	BIND_ENUM_CONSTANT(PERSISTED_TEAM);
	BIND_ENUM_CONSTANT(PERSISTED_NATIONALITY);
	BIND_ENUM_CONSTANT(PERSISTED_DIVISION);
	BIND_ENUM_CONSTANT(PERSISTED_COMBO);
	BIND_ENUM_CONSTANT(PERSISTED_PLAYER_CLASS);
	BIND_ENUM_CONSTANT(PERSISTED_AVATAR_A);
	BIND_ENUM_CONSTANT(PERSISTED_AVATAR_B);
	BIND_ENUM_CONSTANT(PERSISTED_AVATAR_PACKED);
	BIND_ENUM_CONSTANT(PERSISTED_FIELD_COUNT);
	BIND_CONSTANT(DEFAULT_PLAYER_CLASS);
	ClassDB::bind_method(D_METHOD("is_open"), &PlayerInfoScreen::is_open);
	ClassDB::bind_method(D_METHOD("team"), &PlayerInfoScreen::team);
	ClassDB::bind_method(D_METHOD("selected_voice"), &PlayerInfoScreen::selected_voice);
	ClassDB::bind_method(
			D_METHOD("selected_weapon", "control"), &PlayerInfoScreen::selected_weapon);
	ClassDB::bind_method(D_METHOD("preview_combo"), &PlayerInfoScreen::preview_combo);

	// The screen opens: the tables are given now, before the populate reads them.
	ADD_SIGNAL(MethodInfo("opening"));
	// The populate ran: the portrait and the slot icons mount.
	ADD_SIGNAL(MethodInfo("opened"));
	// Another document took the menu: the mounts go.
	ADD_SIGNAL(MethodInfo("closed"));
	ADD_SIGNAL(MethodInfo("preview_changed"));
	ADD_SIGNAL(MethodInfo("preview_hovered", PropertyInfo(Variant::BOOL, "hovered")));
	ADD_SIGNAL(MethodInfo("loadout_changed"));
	ADD_SIGNAL(MethodInfo("relayout"));
	// TESTPLAYERVOICE: the voice's trigger in the menu bank (MenuDriver.play_widget_sound plays
	// it).
	ADD_SIGNAL(MethodInfo("voice_requested", PropertyInfo(Variant::STRING, "trigger"),
			PropertyInfo(Variant::STRING, "bank")));
	// The ACCEPT saved the dialog into the profile record: the selection for the next spawn.
	ADD_SIGNAL(MethodInfo("avatar_chosen", PropertyInfo(Variant::DICTIONARY, "profile")));
}

} // namespace godot
