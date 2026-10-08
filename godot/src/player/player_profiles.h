#pragma once

// The player profile binding: one engine profile::PlayerProfiles (player.sav's
// and weapon.sav's records in memory, the current record), the defaults a fresh
// record takes, and the two files' device legs — reading them before the load
// and writing them, each replaced atomically, at a save. Every rule is the
// engine's (runtime/profile/player_profiles.h, docs/playerinfo/player-sav-re.md);
// this class marshals.

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/profile/player_profiles.h>

namespace godot {

class AvatarDatabase;
class RtxtStringFile;
class WeaponProfileSummary;

class PlayerProfiles : public RefCounted {
	GDCLASS(PlayerProfiles, RefCounted)

	opennova::profile::PlayerProfiles store_;
	opennova::profile::ProfileDefaults defaults_;

protected:
	static void _bind_methods();

public:
	PlayerProfiles();

	const opennova::profile::PlayerProfiles &native() const { return store_; }
	opennova::profile::PlayerProfiles &native() { return store_; }

	// The defaults a fresh record takes: the character table (null: none), the
	// menu table and the expansion's override table the macros read (either
	// null), and the joystick's capability bits.
	void set_defaults(const Ref<AvatarDatabase> &p_avatars,
			const Ref<RtxtStringFile> &p_override_table,
			const Ref<RtxtStringFile> &p_menu_table, int p_joystick_caps);

	// The load from `dir` (player.sav there, weapon.sav at weapon_sav_relpath
	// under it); a missing file is no error. OK, or the error a present file
	// gave on reading.
	Error load(const String &p_dir, const String &p_expansion);
	// The load over bytes (tests and tools): an empty array with its has_* flag
	// false is a missing file.
	void load_bytes(const PackedByteArray &p_player_sav, bool p_has_player_sav,
			const PackedByteArray &p_weapon_sav, bool p_has_weapon_sav,
			const String &p_user_name);
	// The save into `dir`, both files.
	Error save(const String &p_dir, const String &p_expansion);
	PackedByteArray player_sav_bytes() const;
	PackedByteArray weapon_sav_bytes() const;

	static String weapon_sav_relpath(const String &p_expansion);
	// The account name the load names record 0 for (the OS's user name).
	static String account_name();

	int get_current_slot() const { return store_.current_slot(); }
	void select_slot(int p_slot) { store_.select_slot(p_slot); }
	String get_slot_name(int p_slot) const;
	bool is_slot_nameless(int p_slot) const;
	String get_player_name() const;
	bool is_nameless() const;
	bool is_first_run() const { return store_.first_run(); }

	bool rename(const String &p_text) { return store_.rename(p_text.utf8().get_data()); }
	void commit_name(const String &p_text);
	void reset_current() { store_.reset_current(defaults_); }

	// The current record's fields: its named dwords (playersav::record_words,
	// `word_names` in offset order), flags, macros, voice bytes and binding count.
	static PackedStringArray word_names();
	// One dword of the current record by its name; false for an unknown name.
	bool set_word(const String &p_name, int p_value);
	int get_word(const String &p_name) const;
	int get_flags() const { return static_cast<int>(store_.current().flags); }
	PackedStringArray get_macros() const;
	int get_voice(int p_side) const;
	int get_binding_count() const { return static_cast<int>(store_.current().bindings.size()); }
	// PLAYER_INFO's OPTIONS_AUTORELOAD / OPTIONS_AUTOMEDIC over the current
	// record: the checked states its fill shows and its ACCEPT's writes, the
	// auto-medic box stored inverted (engine profile_controls.h).
	bool is_auto_reload_checked() const;
	bool is_auto_medic_checked() const;
	void set_auto_reload_checked(bool p_checked);
	void set_auto_medic_checked(bool p_checked);

	// The character side of the current weapon.sav record.
	Ref<WeaponProfileSummary> character_summary() const;
	// PLAYER_INFO ACCEPT's character snapshot into the current weapon.sav
	// record: `player_class` (5..9) to both sides, each non-empty
	// `side_profiles[side]` {avatar_a, avatar_b, avatar_packed} to its side, and
	// a `kit` array to the edited side's (`team`) class page.
	// ERR_INVALID_PARAMETER for a snapshot the record cannot take.
	Error apply_character_selection(const Dictionary &p_profile);

	void record_mission_start(int p_campaign) { store_.record_mission_start(p_campaign); }
	void begin_session() { store_.begin_session(); }
	bool record_round_end(bool p_in_session, bool p_round_over, int p_campaign,
			int p_campaign_mission, bool p_won) {
		return store_.record_round_end(p_in_session, p_round_over, p_campaign,
				p_campaign_mission, p_won);
	}
	void clear_intro_pending() { store_.clear_intro_pending(); }
};

} // namespace godot
