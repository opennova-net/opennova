#include "player/player_spawn_loadout.h"

#include "util/string_convert.h"

#include <godot_cpp/variant/array.hpp>

using namespace godot;
using opennova::to_std;

namespace {

int dict_int(const Dictionary &d, const char *key, int def) {
	return d.has(key) ? (int)d[key] : def;
}

} // namespace

Ref<PlayerSpawnLoadout> PlayerSpawnLoadout::from_profile(const Dictionary &p_profile) {
	Ref<PlayerSpawnLoadout> out;
	out.instantiate();
	for (int i = 0; i < opennova::world::kSpawnLoadoutSlotCount; ++i) {
		const String key = opennova::world::kSpawnLoadoutSlotKeys[i];
		if (!p_profile.has(key)) {
			continue;
		}
		out->set_slot(i, String(p_profile[key]));
		const String clips_key = key + String("_clips");
		out->slots_[i].clips = p_profile.has(clips_key) ? (int)p_profile[clips_key] : -1;
	}
	if (p_profile.has("player_class")) {
		out->set_player_class((int)p_profile["player_class"]);
	}
	// The two side selections the PLAYER_INFO profile carries (the same reads
	// AvatarDatabase.character_join_profile performs over the Dictionary form).
	const Array sides = p_profile.has("side_profiles") ? (Array)p_profile["side_profiles"] : Array();
	for (int side = 0; side < 2 && side < sides.size(); ++side) {
		if (sides[side].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary sd = sides[side];
		if (sd.is_empty()) {
			continue;
		}
		SideSelection &selection = out->sides_[side];
		selection.present = true;
		selection.nationality = dict_int(sd, "nationality", -1);
		selection.division = dict_int(sd, "division", -1);
		selection.combo = dict_int(sd, "combo", -1);
		selection.player_class = dict_int(sd, "player_class",
				opennova::inmatch::kJoinDefaultPlayerClass);
	}
	return out;
}

#define PLAYER_SPAWN_SIDE_IMPL(m_name, m_default)                                     \
	int PlayerSpawnLoadout::side_##m_name(int p_side) const {                          \
		return valid_side(p_side) ? sides_[p_side].m_name : (m_default);               \
	}                                                                                  \
	void PlayerSpawnLoadout::set_side_##m_name(int p_side, int p_value) {              \
		if (valid_side(p_side)) {                                                      \
			sides_[p_side].m_name = p_value;                                           \
			sides_[p_side].present = true;                                             \
		}                                                                              \
	}
PLAYER_SPAWN_SIDE_FIELDS(PLAYER_SPAWN_SIDE_IMPL)
#undef PLAYER_SPAWN_SIDE_IMPL

opennova::world::SpawnLoadoutInput PlayerSpawnLoadout::engine_input() const {
	opennova::world::SpawnLoadoutInput input;
	for (int i = 0; i < opennova::world::kSpawnLoadoutSlotCount; ++i) {
		input.slots[i].present = slots_[i].present;
		input.slots[i].name = to_std(slots_[i].name);
		input.slots[i].clips = slots_[i].clips;
	}
	input.has_player_class = has_player_class_;
	input.player_class = player_class_;
	return input;
}

void PlayerSpawnLoadout::fill_join_sides(
		opennova::inmatch::JoinSideSelection (&r_sides)[2]) const {
	for (int side = 0; side < 2; ++side) {
		r_sides[side] = opennova::inmatch::JoinSideSelection();
		if (!sides_[side].present) {
			continue;
		}
		r_sides[side].present = true;
		r_sides[side].nationality_index = sides_[side].nationality;
		r_sides[side].division_index = sides_[side].division;
		r_sides[side].combo_index = sides_[side].combo;
		r_sides[side].player_class = sides_[side].player_class;
	}
}

void PlayerSpawnLoadout::_bind_methods() {
	ClassDB::bind_static_method("PlayerSpawnLoadout", D_METHOD("from_profile", "profile"),
			&PlayerSpawnLoadout::from_profile);
#define PLAYER_SPAWN_SLOT_BIND(m_slot)                                                              \
	ClassDB::bind_method(D_METHOD("get_" #m_slot), &PlayerSpawnLoadout::get_##m_slot);              \
	ClassDB::bind_method(D_METHOD("set_" #m_slot, "name"), &PlayerSpawnLoadout::set_##m_slot);      \
	ADD_PROPERTY(PropertyInfo(Variant::STRING, #m_slot), "set_" #m_slot, "get_" #m_slot);           \
	ClassDB::bind_method(D_METHOD("has_" #m_slot), &PlayerSpawnLoadout::has_##m_slot);              \
	ClassDB::bind_method(D_METHOD("get_" #m_slot "_clips"), &PlayerSpawnLoadout::get_##m_slot##_clips); \
	ClassDB::bind_method(D_METHOD("set_" #m_slot "_clips", "clips"),                                \
			&PlayerSpawnLoadout::set_##m_slot##_clips);                                             \
	ADD_PROPERTY(PropertyInfo(Variant::INT, #m_slot "_clips"), "set_" #m_slot "_clips",             \
			"get_" #m_slot "_clips");
	PLAYER_SPAWN_SLOT_BIND(primary)
	PLAYER_SPAWN_SLOT_BIND(secondary)
	PLAYER_SPAWN_SLOT_BIND(accessory)
#undef PLAYER_SPAWN_SLOT_BIND
	ClassDB::bind_method(D_METHOD("get_player_class"), &PlayerSpawnLoadout::get_player_class);
	ClassDB::bind_method(D_METHOD("set_player_class", "player_class"),
			&PlayerSpawnLoadout::set_player_class);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "player_class"), "set_player_class", "get_player_class");
#define PLAYER_SPAWN_SIDE_BIND(m_name, m_default)                                                  \
	ClassDB::bind_method(D_METHOD("side_" #m_name, "side"), &PlayerSpawnLoadout::side_##m_name);  \
	ClassDB::bind_method(D_METHOD("set_side_" #m_name, "side", "value"),                           \
			&PlayerSpawnLoadout::set_side_##m_name);
	PLAYER_SPAWN_SIDE_FIELDS(PLAYER_SPAWN_SIDE_BIND)
#undef PLAYER_SPAWN_SIDE_BIND
	BIND_ENUM_CONSTANT(SIDE_BLUE);
	BIND_ENUM_CONSTANT(SIDE_RED);
}
