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
class CharacterProfile;
class CharacterSelection;
class PlayerInfoKitRestore;
class ResourceRoot;
class RtxtStringFile;
class WeaponDatabase;
class WeaponProfileSummary;

class PlayerProfiles : public RefCounted {
	GDCLASS(PlayerProfiles, RefCounted)

	opennova::profile::PlayerProfiles store_;
	opennova::profile::ProfileDefaults defaults_;
	// The expansion the last load read weapon.sav for, which the process's save writes it for.
	String expansion_;

	void set_defaults_native(const AvatarDatabase *p_avatars,
			const opennova::rtxt::File *p_menu_table, int p_joystick_caps);

protected:
	static void _bind_methods();

public:
	PlayerProfiles();

	// --- The process's profile (retail's one in-memory image, docs/playerinfo/player-sav-re.md) ---
	// The store every screen and session leg reads: its first use loads both files from the
	// directory the game runs in (LaunchFlags.working_dir) with a fresh record's defaults.
	static Ref<PlayerProfiles> process();
	// The menu's start on a mount: a fresh record's defaults from the mount's Avatars.def and the
	// registered menu table (rtxt::kTableGameUi), then both files read again from the working
	// directory, weapon.sav for the mount's expansion. OK, or the error a present file gave.
	static Error load_process_for(const Ref<ResourceRoot> &p_root);
	// A save point: both files written where the last load read them. ERR_UNCONFIGURED with no
	// working directory; a failed write is warned and returned.
	static Error save_process();
	// The callsign every session leg rides (engine profile::profile_callsign): the `--callsign`
	// launch override, else the current record's name, each cut to the wire's Name[16].
	static String process_callsign();
	// The profile's character half for the menu screens and the spawn (engine
	// inmatch::character_profile over the current weapon record, the name the callsign, the
	// sides resolved against the mount's Avatars.def).
	static Ref<CharacterProfile> process_character_profile(const Ref<ResourceRoot> &p_root);
	// The module's teardown drops the process's store.
	static void cleanup_statics();

	const opennova::profile::PlayerProfiles &native() const { return store_; }
	opennova::profile::PlayerProfiles &native() { return store_; }

	// The defaults a fresh record takes: the character table (null: none), the
	// menu table the macros read (null: none) after the process's text-override
	// table (rtxt::text_override), and the joystick's capability bits.
	void set_defaults(const Ref<AvatarDatabase> &p_avatars,
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

	bool rename(const String &p_text);
	// The PLAYERNAME edit's change notice over its label: the label, or the
	// cut the edit is set to (engine PlayerProfiles::name_changed).
	String name_changed(const String &p_label);
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
	// PLAYERVOICE's pick (or its fill's reset) into a side's override byte
	// (engine profile::set_voice_override).
	void set_voice(int p_side, int p_value);
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
	// PLAYER_INFO's populate: the side's (0 blue, 1 red) class page of the
	// current record, the one its class byte picks, restored over the weapon
	// table (engine menu::player_info_restore_kit).
	Ref<PlayerInfoKitRestore> restore_player_info_kit(int p_side,
			const Ref<WeaponDatabase> &p_weapons) const;
	// PLAYER_INFO's dialog save (its ACCEPT, a side radio, a class pick) into the
	// current weapon.sav record (engine profile::PlayerProfiles::apply_character_selection
	// holds the rules). ERR_INVALID_PARAMETER for a selection the record cannot take
	// (or none).
	Error apply_character_selection(const Ref<CharacterSelection> &p_selection);
	// The dialog save whole: the character leg, then the PLAYERNAME text through the name
	// rule when the selection carries one (engine PlayerProfiles::accept_player_info).
	Error accept_player_info(const Ref<CharacterSelection> &p_selection);

	void record_mission_start(int p_campaign) { store_.record_mission_start(p_campaign); }
	void begin_session() { store_.begin_session(); }
	void clamp_session_classes() { store_.clamp_session_classes(); }
	bool record_round_end(bool p_in_session, bool p_round_over, int p_campaign,
			int p_campaign_mission, bool p_won) {
		return store_.record_round_end(p_in_session, p_round_over, p_campaign,
				p_campaign_mission, p_won);
	}
	void clear_intro_pending() { store_.clear_intro_pending(); }
};

} // namespace godot
