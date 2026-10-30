#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/inmatch/player_info_screen.h>

#include <memory>

#include "mnu/menu_driver.h"

namespace godot {

class AvatarComboRow;
class AvatarDatabase;
class PlayerProfiles;
class RtxtStringFile;
class WeaponDatabase;

// The PLAYER_INFO screen's controller (engine inmatch::PlayerInfoScreen, which carries the
// witnesses) as the menu shell's "player_info" delegate: the tables it reads (the avatar and
// weapon tables, the profile store, the text tables), the persisted profile it opens over, the
// read seams the shell's PlayerInfoMenuCompanion and the tests take, and the device work it asks
// for as signals (the portrait, the slot icons, the voice's sound, the ACCEPT's hand-off).
class PlayerInfoScreen : public MenuScreenBinding {
	GDCLASS(PlayerInfoScreen, MenuScreenBinding)

	class Host;
	std::unique_ptr<Host> host_;
	opennova::inmatch::PlayerInfoScreen screen_;
	Ref<AvatarDatabase> db_;
	Ref<WeaponDatabase> weapons_;
	Ref<PlayerProfiles> profiles_;
	Ref<RtxtStringFile> gametext_;
	Ref<RtxtStringFile> menutxt_;
	Ref<RtxtStringFile> gameui_;

protected:
	static void _bind_methods();

public:
	PlayerInfoScreen();
	~PlayerInfoScreen() override;

	opennova::menu::MenuScreenController *screen_controller() override { return &screen_; }

	// The tables (each null for none), given before the next open or in its `opening` signal.
	void set_database(const Ref<AvatarDatabase> &p_db);
	Ref<AvatarDatabase> get_database() const { return db_; }
	void set_weapon_database(const Ref<WeaponDatabase> &p_weapons);
	Ref<WeaponDatabase> get_weapon_database() const { return weapons_; }
	void set_profiles(const Ref<PlayerProfiles> &p_profiles);
	void set_text_tables(const Ref<RtxtStringFile> &p_gametext,
			const Ref<RtxtStringFile> &p_menutxt, const Ref<RtxtStringFile> &p_gameui);
	// The persisted profile the screen opens over: its shown side and class when it names them,
	// and each side's saved selection, PERSISTED_* order (empty: never saved).
	enum PersistedField {
		PERSISTED_TEAM,
		PERSISTED_NATIONALITY,
		PERSISTED_DIVISION,
		PERSISTED_COMBO,
		PERSISTED_PLAYER_CLASS,
		PERSISTED_AVATAR_A,
		PERSISTED_AVATAR_B,
		PERSISTED_AVATAR_PACKED,
		PERSISTED_FIELD_COUNT,
	};
	// The class a side's record names none of (inmatch/join_character_profile.h).
	enum { DEFAULT_PLAYER_CLASS = 8 };
	void set_persisted(bool p_has_team, int p_team, bool p_has_class, int p_player_class,
			const PackedInt32Array &p_blue, const PackedInt32Array &p_red);

	bool is_open() const { return screen_.is_open(); }
	int team() const { return screen_.team(); }
	int selected_voice() const { return screen_.selected_voice(); }
	// A category's selected weapon.def row (-1 NONE): the slot icon's.
	int selected_weapon(const String &p_control) const;
	// The combo the portrait shows (null for none).
	Ref<AvatarComboRow> preview_combo() const;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::PlayerInfoScreen::PersistedField);
