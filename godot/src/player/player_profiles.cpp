#include "player/player_profiles.h"

#include "object/avatar_database.h"
#include "rtxt/rtxt_string_file.h"
#include "simulation/weapon_profile_summary.h"
#include "util/string_convert.h"

#include <formats/playersav/player_sav.h>
#include <runtime/inmatch/character_registry.h>
#include <runtime/profile/profile_controls.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace godot;

namespace {

namespace playersav = opennova::playersav;

std::vector<uint8_t> to_bytes(const PackedByteArray &p_bytes) {
	std::vector<uint8_t> out(static_cast<size_t>(p_bytes.size()));
	if (!out.empty()) std::memcpy(out.data(), p_bytes.ptr(), out.size());
	return out;
}

PackedByteArray to_packed(const std::vector<uint8_t> &p_bytes) {
	PackedByteArray out;
	out.resize(static_cast<int64_t>(p_bytes.size()));
	if (!p_bytes.empty()) std::memcpy(out.ptrw(), p_bytes.data(), p_bytes.size());
	return out;
}

// A save file's bytes: false when there is none; `r_error` the read's error
// when one exists but cannot be read.
bool read_save_file(const String &p_path, std::vector<uint8_t> &r_bytes, Error &r_error) {
	if (p_path.is_empty() || !FileAccess::file_exists(p_path)) return false;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		r_error = FileAccess::get_open_error();
		return false;
	}
	r_bytes = to_bytes(file->get_buffer(static_cast<int64_t>(file->get_length())));
	file->close();
	return true;
}

Error replace_file_atomic(const String &temp_path, const String &target_path) {
#ifdef _WIN32
	const CharWideString temp = temp_path.wide_string();
	const CharWideString target = target_path.wide_string();
	// MoveFileExW with REPLACE_EXISTING is the Windows atomic same-volume rename
	// primitive; WRITE_THROUGH keeps the save from returning before metadata lands.
	if (::MoveFileExW(temp.get_data(), target.get_data(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
		return ERR_FILE_CANT_WRITE;
#else
	const CharString temp = temp_path.utf8();
	const CharString target = target_path.utf8();
	if (std::rename(temp.get_data(), target.get_data()) != 0) return ERR_FILE_CANT_WRITE;
#endif
	return OK;
}

// The file replaced whole: a temp sibling written and renamed over it, so a
// failed write leaves the previous file.
Error write_save_file(const String &p_path, const std::vector<uint8_t> &p_bytes) {
	if (p_path.is_empty()) return ERR_INVALID_PARAMETER;
	const String base_dir = p_path.get_base_dir();
	if (!base_dir.is_empty()) {
		const Error dir_error = DirAccess::make_dir_recursive_absolute(base_dir);
		if (dir_error != OK) return dir_error;
	}
	const String temp_path = vformat("%s.tmp.%d", p_path, OS::get_singleton()->get_process_id());
	Ref<FileAccess> file = FileAccess::open(temp_path, FileAccess::WRITE);
	if (file.is_null()) return FileAccess::get_open_error();
	file->store_buffer(to_packed(p_bytes));
	file->flush();
	const Error write_error = file->get_error();
	file->close();
	if (write_error != OK) {
		DirAccess::remove_absolute(temp_path);
		return write_error;
	}
	const Error rename_error = replace_file_atomic(temp_path, p_path);
	if (rename_error != OK) DirAccess::remove_absolute(temp_path);
	return rename_error;
}

bool avatar_selection_from_dictionary(const Dictionary &profile, int side,
		uint8_t &avatar_a, uint8_t &avatar_b, uint16_t &avatar_packed) {
	if (profile.is_empty() || !profile.has("avatar_a") || !profile.has("avatar_b") ||
			!profile.has("avatar_packed"))
		return false;
	const int a = profile.get("avatar_a", -1);
	const int b = profile.get("avatar_b", -1);
	const int packed = profile.get("avatar_packed", -1);
	if (a < 0 || a > 0xff || b < 0 || b > 0xff || packed < 0 || packed > 0xffff ||
			((packed >> 15) & 1) != side)
		return false;
	avatar_a = static_cast<uint8_t>(a);
	avatar_b = static_cast<uint8_t>(b);
	avatar_packed = static_cast<uint16_t>(packed);
	return true;
}

} // namespace

PlayerProfiles::PlayerProfiles() {
	defaults_.bindings = opennova::profile::default_binding_table();
	defaults_.macros = opennova::profile::default_macros(nullptr, nullptr);
}

void PlayerProfiles::set_defaults(const Ref<AvatarDatabase> &p_avatars,
		const Ref<RtxtStringFile> &p_override_table, const Ref<RtxtStringFile> &p_menu_table,
		int p_joystick_caps) {
	defaults_.bindings = opennova::profile::default_binding_table();
	defaults_.macros = opennova::profile::default_macros(
			p_override_table.is_valid() ? &p_override_table->get_native() : nullptr,
			p_menu_table.is_valid() ? &p_menu_table->get_native() : nullptr);
	defaults_.joystick_caps = static_cast<uint32_t>(p_joystick_caps);
	defaults_.avatars = {};
	if (p_avatars.is_valid() && p_avatars->is_loaded()) {
		const opennova::inmatch::CharacterRegistry &registry = p_avatars->character_registry();
		for (int side = 0; side < 2; ++side) {
			const uint16_t packed = registry.first_character_id(side);
			opennova::profile::ProfileDefaults::Avatar &avatar = defaults_.avatars[side];
			avatar.packed = packed;
			if (const opennova::inmatch::CharacterEntry *entry = registry.find_by_packed_id(packed)) {
				avatar.nationality = static_cast<uint8_t>(entry->nationality_id);
				avatar.division = static_cast<uint8_t>(entry->division_id);
			}
		}
	}
}

Error PlayerProfiles::load(const String &p_dir, const String &p_expansion) {
	std::vector<uint8_t> player_sav;
	std::vector<uint8_t> weapon_sav;
	Error error = OK;
	const bool has_player =
			!p_dir.is_empty() && read_save_file(p_dir.path_join("player.sav"), player_sav, error);
	const bool has_weapon = !p_dir.is_empty() &&
			read_save_file(p_dir.path_join(weapon_sav_relpath(p_expansion)), weapon_sav, error);
	opennova::profile::PlayerProfiles::LoadInput input;
	input.player_sav = has_player ? &player_sav : nullptr;
	input.weapon_sav = has_weapon ? &weapon_sav : nullptr;
	input.user_name = opennova::to_std(account_name());
	store_.load(input, defaults_);
	return error;
}

void PlayerProfiles::load_bytes(const PackedByteArray &p_player_sav, bool p_has_player_sav,
		const PackedByteArray &p_weapon_sav, bool p_has_weapon_sav, const String &p_user_name) {
	const std::vector<uint8_t> player_sav = to_bytes(p_player_sav);
	const std::vector<uint8_t> weapon_sav = to_bytes(p_weapon_sav);
	opennova::profile::PlayerProfiles::LoadInput input;
	input.player_sav = p_has_player_sav ? &player_sav : nullptr;
	input.weapon_sav = p_has_weapon_sav ? &weapon_sav : nullptr;
	input.user_name = opennova::to_std(p_user_name);
	store_.load(input, defaults_);
}

Error PlayerProfiles::save(const String &p_dir, const String &p_expansion) {
	if (p_dir.is_empty()) return ERR_INVALID_PARAMETER;
	const Error player_error = write_save_file(p_dir.path_join("player.sav"), store_.player_sav_bytes());
	const Error weapon_error =
			write_save_file(p_dir.path_join(weapon_sav_relpath(p_expansion)), store_.weapon_sav_bytes());
	return player_error != OK ? player_error : weapon_error;
}

PackedByteArray PlayerProfiles::player_sav_bytes() const {
	return to_packed(store_.player_sav_bytes());
}

PackedByteArray PlayerProfiles::weapon_sav_bytes() const {
	return to_packed(store_.weapon_sav_bytes());
}

String PlayerProfiles::weapon_sav_relpath(const String &p_expansion) {
	return opennova::to_gd(playersav::weapon_sav_relpath(opennova::to_std(p_expansion)));
}

String PlayerProfiles::account_name() {
	// The web platform has no account to ask.
	if (OS::get_singleton()->has_feature("web")) return String();
	const String windows = OS::get_singleton()->get_environment("USERNAME");
	return windows.is_empty() ? OS::get_singleton()->get_environment("USER") : windows;
}

String PlayerProfiles::get_slot_name(int p_slot) const {
	if (p_slot < 0 || p_slot >= static_cast<int>(playersav::kProfileSlots)) return String();
	return opennova::to_gd(store_.player_sav().slots[static_cast<size_t>(p_slot)].name);
}

bool PlayerProfiles::is_slot_nameless(int p_slot) const {
	if (p_slot < 0 || p_slot >= static_cast<int>(playersav::kProfileSlots)) return true;
	return (store_.player_sav().slots[static_cast<size_t>(p_slot)].flags &
				   playersav::kRecordFlagNoName) != 0;
}

String PlayerProfiles::get_player_name() const {
	return opennova::to_gd(store_.current().name);
}

bool PlayerProfiles::is_nameless() const {
	return (store_.current().flags & playersav::kRecordFlagNoName) != 0;
}

void PlayerProfiles::commit_name(const String &p_text) {
	store_.commit_name(p_text.utf8().get_data());
}

PackedStringArray PlayerProfiles::word_names() {
	PackedStringArray out;
	size_t count = 0;
	const playersav::RecordWord *words = playersav::record_words(&count);
	for (size_t i = 0; i < count; ++i) out.push_back(String(words[i].name));
	return out;
}

PackedStringArray PlayerProfiles::get_macros() const {
	PackedStringArray out;
	for (const std::string &macro : store_.current().macros) out.push_back(opennova::to_gd(macro));
	return out;
}

int PlayerProfiles::get_voice(int p_side) const {
	if (p_side < 0 || p_side > 1) return 0;
	return store_.current().voice[static_cast<size_t>(p_side)];
}

bool PlayerProfiles::is_auto_reload_checked() const {
	return opennova::profile::auto_reload_checked(store_.current());
}

bool PlayerProfiles::is_auto_medic_checked() const {
	return opennova::profile::auto_medic_checked(store_.current());
}

void PlayerProfiles::set_auto_reload_checked(bool p_checked) {
	opennova::profile::set_auto_reload(store_.current(), p_checked);
}

void PlayerProfiles::set_auto_medic_checked(bool p_checked) {
	opennova::profile::set_auto_medic(store_.current(), p_checked);
}

bool PlayerProfiles::set_word(const String &p_name, int p_value) {
	const playersav::RecordWord *word = playersav::find_record_word(opennova::to_std(p_name));
	if (word == nullptr) return false;
	store_.current().*(word->member) = p_value;
	return true;
}

int PlayerProfiles::get_word(const String &p_name) const {
	const playersav::RecordWord *word = playersav::find_record_word(opennova::to_std(p_name));
	return word != nullptr ? store_.current().*(word->member) : 0;
}

Ref<WeaponProfileSummary> PlayerProfiles::character_summary() const {
	Ref<WeaponProfileSummary> out;
	out.instantiate();
	out->assign(store_.current_weapons(), OK, true);
	return out;
}

Error PlayerProfiles::apply_character_selection(const Dictionary &p_profile) {
	if (p_profile.is_empty()) return ERR_INVALID_PARAMETER;
	// The ACCEPT snapshot (PlayerCharacterSelectionState.snapshot): the shared
	// PLAYERCLASS value plus side_profiles[blue, red], each carrying the side's
	// authored nationality/division ids and packed character id; the engine's
	// update_avatar_selection is the dialog's write.
	const int player_class = p_profile.get("player_class", -1);
	if (player_class < playersav::kMinPlayerClass || player_class > playersav::kMaxPlayerClass)
		return ERR_INVALID_PARAMETER;
	const Array side_profiles = p_profile.get("side_profiles", Array());
	playersav::File &file = store_.weapon_sav();
	const size_t slot = static_cast<size_t>(store_.current_slot());
	// Every input is checked before the record changes.
	uint8_t avatar_a[2] = {};
	uint8_t avatar_b[2] = {};
	uint16_t avatar_packed[2] = {};
	bool has_side[2] = {};
	for (int side = 0; side < 2; ++side) {
		Dictionary selected;
		if (side < side_profiles.size() && side_profiles[side].get_type() == Variant::DICTIONARY)
			selected = side_profiles[side];
		if (selected.is_empty()) continue;
		if (!avatar_selection_from_dictionary(selected, side, avatar_a[side], avatar_b[side],
					avatar_packed[side]))
			return ERR_INVALID_PARAMETER;
		has_side[side] = true;
	}
	if (!has_side[0] && !has_side[1]) return ERR_INVALID_PARAMETER;
	// The edited side's kit page as the PLAYER screen serializes it; an absent
	// "kit" key (no weapon.def loaded) leaves the pages untouched.
	bool has_kit = false;
	int team = -1;
	playersav::KitPage page;
	if (p_profile.has("kit")) {
		team = p_profile.get("team", -1);
		const Array kit = p_profile.get("kit", Array());
		if (team < 0 || team > 1) return ERR_INVALID_PARAMETER;
		for (int i = 0; i < kit.size(); ++i) {
			if (kit[i].get_type() != Variant::DICTIONARY) return ERR_INVALID_PARAMETER;
			const Dictionary entry = kit[i];
			const String name = entry.get("name", String());
			if (name.is_empty()) return ERR_INVALID_PARAMETER;
			playersav::KitEntry out;
			out.name = opennova::to_std(name);
			out.ammo_primary = int32_t(int(entry.get("ammo_primary", -1)));
			out.ammo_secondary = int32_t(int(entry.get("ammo_secondary", -1)));
			out.flags = int32_t(int(entry.get("flags", -1)));
			page.entries.push_back(std::move(out));
		}
		has_kit = true;
	}
	for (int side = 0; side < 2; ++side) {
		if (!has_side[side]) continue;
		playersav::update_avatar_selection(file, slot,
				side == 0 ? playersav::SideId::Blue : playersav::SideId::Red,
				static_cast<uint8_t>(player_class), avatar_a[side], avatar_b[side],
				avatar_packed[side]);
	}
	if (has_kit) {
		playersav::Side &side = file.slots[slot].side(
				team == 0 ? playersav::SideId::Blue : playersav::SideId::Red);
		side.pages[static_cast<size_t>(player_class) - playersav::kMinPlayerClass] = std::move(page);
	}
	return OK;
}

void PlayerProfiles::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_defaults", "avatars", "override_table", "menu_table",
								 "joystick_caps"),
			&PlayerProfiles::set_defaults);
	ClassDB::bind_method(D_METHOD("load", "dir", "expansion"), &PlayerProfiles::load);
	ClassDB::bind_method(D_METHOD("load_bytes", "player_sav", "has_player_sav", "weapon_sav",
								 "has_weapon_sav", "user_name"),
			&PlayerProfiles::load_bytes);
	ClassDB::bind_method(D_METHOD("save", "dir", "expansion"), &PlayerProfiles::save);
	ClassDB::bind_method(D_METHOD("player_sav_bytes"), &PlayerProfiles::player_sav_bytes);
	ClassDB::bind_method(D_METHOD("weapon_sav_bytes"), &PlayerProfiles::weapon_sav_bytes);
	ClassDB::bind_static_method("PlayerProfiles", D_METHOD("weapon_sav_relpath", "expansion"),
			&PlayerProfiles::weapon_sav_relpath);
	ClassDB::bind_static_method("PlayerProfiles", D_METHOD("account_name"),
			&PlayerProfiles::account_name);
	ClassDB::bind_method(D_METHOD("get_current_slot"), &PlayerProfiles::get_current_slot);
	ClassDB::bind_method(D_METHOD("select_slot", "slot"), &PlayerProfiles::select_slot);
	ClassDB::bind_method(D_METHOD("get_slot_name", "slot"), &PlayerProfiles::get_slot_name);
	ClassDB::bind_method(D_METHOD("is_slot_nameless", "slot"), &PlayerProfiles::is_slot_nameless);
	ClassDB::bind_method(D_METHOD("get_player_name"), &PlayerProfiles::get_player_name);
	ClassDB::bind_method(D_METHOD("is_nameless"), &PlayerProfiles::is_nameless);
	ClassDB::bind_method(D_METHOD("is_first_run"), &PlayerProfiles::is_first_run);
	ClassDB::bind_method(D_METHOD("rename", "text"), &PlayerProfiles::rename);
	ClassDB::bind_method(D_METHOD("commit_name", "text"), &PlayerProfiles::commit_name);
	ClassDB::bind_method(D_METHOD("reset_current"), &PlayerProfiles::reset_current);
	ClassDB::bind_static_method("PlayerProfiles", D_METHOD("word_names"), &PlayerProfiles::word_names);
	ClassDB::bind_method(D_METHOD("set_word", "name", "value"), &PlayerProfiles::set_word);
	ClassDB::bind_method(D_METHOD("get_word", "name"), &PlayerProfiles::get_word);
	ClassDB::bind_method(D_METHOD("get_flags"), &PlayerProfiles::get_flags);
	ClassDB::bind_method(D_METHOD("get_macros"), &PlayerProfiles::get_macros);
	ClassDB::bind_method(D_METHOD("get_voice", "side"), &PlayerProfiles::get_voice);
	ClassDB::bind_method(D_METHOD("get_binding_count"), &PlayerProfiles::get_binding_count);
	ClassDB::bind_method(D_METHOD("is_auto_reload_checked"), &PlayerProfiles::is_auto_reload_checked);
	ClassDB::bind_method(D_METHOD("is_auto_medic_checked"), &PlayerProfiles::is_auto_medic_checked);
	ClassDB::bind_method(D_METHOD("set_auto_reload_checked", "checked"),
			&PlayerProfiles::set_auto_reload_checked);
	ClassDB::bind_method(D_METHOD("set_auto_medic_checked", "checked"),
			&PlayerProfiles::set_auto_medic_checked);
	ClassDB::bind_method(D_METHOD("character_summary"), &PlayerProfiles::character_summary);
	ClassDB::bind_method(D_METHOD("apply_character_selection", "profile"),
			&PlayerProfiles::apply_character_selection);
	ClassDB::bind_method(D_METHOD("record_mission_start", "campaign"),
			&PlayerProfiles::record_mission_start);
	ClassDB::bind_method(D_METHOD("begin_session"), &PlayerProfiles::begin_session);
	ClassDB::bind_method(D_METHOD("record_round_end", "in_session", "round_over", "campaign",
								 "campaign_mission", "won"),
			&PlayerProfiles::record_round_end);
	ClassDB::bind_method(D_METHOD("clear_intro_pending"), &PlayerProfiles::clear_intro_pending);
}
