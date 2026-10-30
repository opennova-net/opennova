#include "player/character_selection.h"

#include "simulation/weapon_kit_entry.h"
#include "util/string_convert.h"

#include <godot_cpp/variant/array.hpp>

#include <utility>

using namespace godot;

void CharacterSelection::set_character(int p_avatar_a, int p_avatar_b, int p_avatar_packed) {
	opennova::profile::CharacterSelection::Character character;
	character.avatar_a = p_avatar_a;
	character.avatar_b = p_avatar_b;
	character.avatar_packed = p_avatar_packed;
	value_.character = character;
}

void CharacterSelection::set_kit(int p_page_class, const TypedArray<WeaponKitEntry> &p_entries) {
	opennova::profile::CharacterSelection::Kit kit;
	kit.page_class = p_page_class;
	for (int64_t i = 0; i < p_entries.size(); ++i) {
		const Ref<WeaponKitEntry> row = p_entries[i];
		opennova::playersav::KitEntry entry;
		if (row.is_valid()) {
			entry.name = row->value().name;
			entry.ammo_primary = row->value().ammo_primary;
			entry.ammo_secondary = row->value().ammo_secondary;
			entry.flags = row->value().flags;
		}
		kit.entries.push_back(std::move(entry));
	}
	value_.kit = std::move(kit);
}

TypedArray<WeaponKitEntry> CharacterSelection::get_kit() const {
	TypedArray<WeaponKitEntry> out;
	if (!value_.kit) return out;
	for (const opennova::playersav::KitEntry &entry : value_.kit->entries) {
		out.push_back(WeaponKitEntry::make(opennova::to_gd(entry.name), entry.ammo_primary,
				entry.ammo_secondary, entry.flags));
	}
	return out;
}

// The name crosses as the game's code-page bytes, the bytes the PLAYERNAME edit hands the game
// (D-NET-661; the profile binding's other name legs take the same form).
void CharacterSelection::set_name(const String &p_name) {
	value_.name = opennova::utf8_to_retail_text(opennova::to_std(p_name));
}

String CharacterSelection::get_name() const {
	return value_.name ? opennova::cp1252_to_gd(*value_.name) : String();
}

Ref<CharacterSelection> CharacterSelection::from_player_info_snapshot(const Dictionary &p_snapshot) {
	Ref<CharacterSelection> out;
	out.instantiate();
	opennova::profile::CharacterSelection &selection = out->value_;
	selection.player_class = int(p_snapshot.get("player_class", -1));
	selection.team = int(p_snapshot.get("team", -1));
	const Array side_profiles = p_snapshot.get("side_profiles", Array());
	if (selection.team >= 0 && selection.team < side_profiles.size() &&
			side_profiles[selection.team].get_type() == Variant::DICTIONARY) {
		const Dictionary selected = side_profiles[selection.team];
		if (!selected.is_empty()) {
			out->set_character(int(selected.get("avatar_a", -1)), int(selected.get("avatar_b", -1)),
					int(selected.get("avatar_packed", -1)));
		}
	}
	if (p_snapshot.has("kit")) {
		opennova::profile::CharacterSelection::Kit kit;
		kit.page_class = int(p_snapshot.get("kit_class", -1));
		const Array entries = p_snapshot.get("kit", Array());
		for (int64_t i = 0; i < entries.size(); ++i) {
			if (entries[i].get_type() != Variant::DICTIONARY) return Ref<CharacterSelection>();
			const Dictionary entry = entries[i];
			opennova::playersav::KitEntry row;
			row.name = opennova::to_std(String(entry.get("name", String())));
			row.ammo_primary = int32_t(int(entry.get("ammo_primary", -1)));
			row.ammo_secondary = int32_t(int(entry.get("ammo_secondary", -1)));
			row.flags = int32_t(int(entry.get("flags", -1)));
			kit.entries.push_back(std::move(row));
		}
		selection.kit = std::move(kit);
	}
	if (p_snapshot.has("name")) out->set_name(String(p_snapshot.get("name", String())));
	return out;
}

void CharacterSelection::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_player_class"), &CharacterSelection::get_player_class);
	ClassDB::bind_method(D_METHOD("set_player_class", "value"), &CharacterSelection::set_player_class);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "player_class"), "set_player_class", "get_player_class");
	ClassDB::bind_method(D_METHOD("get_team"), &CharacterSelection::get_team);
	ClassDB::bind_method(D_METHOD("set_team", "value"), &CharacterSelection::set_team);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "team"), "set_team", "get_team");
	ClassDB::bind_method(D_METHOD("set_character", "avatar_a", "avatar_b", "avatar_packed"),
			&CharacterSelection::set_character);
	ClassDB::bind_method(D_METHOD("clear_character"), &CharacterSelection::clear_character);
	ClassDB::bind_method(D_METHOD("has_character"), &CharacterSelection::has_character);
	ClassDB::bind_method(D_METHOD("get_avatar_a"), &CharacterSelection::get_avatar_a);
	ClassDB::bind_method(D_METHOD("get_avatar_b"), &CharacterSelection::get_avatar_b);
	ClassDB::bind_method(D_METHOD("get_avatar_packed"), &CharacterSelection::get_avatar_packed);
	ClassDB::bind_method(D_METHOD("set_kit", "page_class", "entries"), &CharacterSelection::set_kit);
	ClassDB::bind_method(D_METHOD("clear_kit"), &CharacterSelection::clear_kit);
	ClassDB::bind_method(D_METHOD("has_kit"), &CharacterSelection::has_kit);
	ClassDB::bind_method(D_METHOD("get_kit_class"), &CharacterSelection::get_kit_class);
	ClassDB::bind_method(D_METHOD("get_kit"), &CharacterSelection::get_kit);
	ClassDB::bind_method(D_METHOD("set_name", "name"), &CharacterSelection::set_name);
	ClassDB::bind_method(D_METHOD("clear_name"), &CharacterSelection::clear_name);
	ClassDB::bind_method(D_METHOD("has_name"), &CharacterSelection::has_name);
	ClassDB::bind_method(D_METHOD("get_name"), &CharacterSelection::get_name);
	ClassDB::bind_static_method("CharacterSelection",
			D_METHOD("from_player_info_snapshot", "snapshot"),
			&CharacterSelection::from_player_info_snapshot);
}
