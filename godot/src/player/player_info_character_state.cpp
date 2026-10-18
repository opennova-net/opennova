#include "player/player_info_character_state.h"

#include "object/avatar_database.h"
#include "util/record_bind.h"

using namespace godot;

Ref<PlayerInfoSideSelection> PlayerInfoSideSelection::from_native(
		const std::optional<opennova::inmatch::PlayerInfoSideSelection> &p_value) {
	Ref<PlayerInfoSideSelection> out;
	if (!p_value) return out;
	out.instantiate();
	out->team_ = p_value->team;
	out->nationality_ = p_value->nationality;
	out->division_ = p_value->division;
	out->combo_ = p_value->combo;
	out->player_class_ = p_value->player_class;
	out->avatar_a_ = p_value->avatar_a;
	out->avatar_b_ = p_value->avatar_b;
	out->avatar_packed_ = p_value->avatar_packed;
	return out;
}

opennova::inmatch::PlayerInfoSideSelection PlayerInfoSideSelection::to_native() const {
	opennova::inmatch::PlayerInfoSideSelection out;
	out.team = team_;
	out.nationality = nationality_;
	out.division = division_;
	out.combo = combo_;
	out.player_class = player_class_;
	out.avatar_a = avatar_a_;
	out.avatar_b = avatar_b_;
	out.avatar_packed = avatar_packed_;
	return out;
}

void PlayerInfoSideSelection::_bind_methods() {
	PLAYER_INFO_SIDE_SELECTION_FIELDS(OPENNOVA_RECORD_FIELD)
}

void PlayerInfoCharacterState::set_persisted(bool p_has_team, int p_team, bool p_has_class,
		int p_player_class, const Ref<PlayerInfoSideSelection> &p_blue,
		const Ref<PlayerInfoSideSelection> &p_red) {
	std::optional<opennova::inmatch::PlayerInfoSideSelection> sides[2];
	if (p_blue.is_valid()) sides[0] = p_blue->to_native();
	if (p_red.is_valid()) sides[1] = p_red->to_native();
	value_.set_persisted(p_has_team ? std::optional<int>(p_team) : std::nullopt,
			p_has_class ? std::optional<int>(p_player_class) : std::nullopt, sides);
}

void PlayerInfoCharacterState::set_database(const Ref<AvatarDatabase> &p_db) {
	value_.set_registry(p_db.is_valid() ? p_db->character_registry() : opennova::inmatch::CharacterRegistry());
}

Ref<PlayerInfoSideSelection> PlayerInfoCharacterState::side_selection(int p_side) const {
	return PlayerInfoSideSelection::from_native(value_.side_selection(p_side));
}

TypedArray<PlayerInfoSideSelection> PlayerInfoCharacterState::snapshot(int p_team, int p_nationality,
		int p_division, int p_combo, int p_selected_class) {
	const opennova::inmatch::PlayerInfoCharacterState::Snapshot snap =
			value_.snapshot(p_team, p_nationality, p_division, p_combo, p_selected_class);
	TypedArray<PlayerInfoSideSelection> out;
	out.push_back(PlayerInfoSideSelection::from_native(snap.current));
	out.push_back(PlayerInfoSideSelection::from_native(snap.sides[0]));
	out.push_back(PlayerInfoSideSelection::from_native(snap.sides[1]));
	return out;
}

void PlayerInfoCharacterState::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_persisted", "has_team", "team", "has_class", "player_class", "blue", "red"),
			&PlayerInfoCharacterState::set_persisted);
	ClassDB::bind_method(D_METHOD("set_database", "db"), &PlayerInfoCharacterState::set_database);
	ClassDB::bind_method(D_METHOD("initial_team", "authored_team"), &PlayerInfoCharacterState::initial_team);
	ClassDB::bind_method(D_METHOD("side_selection", "side"), &PlayerInfoCharacterState::side_selection);
	ClassDB::bind_method(D_METHOD("player_class", "side"), &PlayerInfoCharacterState::player_class);
	ClassDB::bind_method(D_METHOD("snapshot", "team", "nationality", "division", "combo", "selected_class"),
			&PlayerInfoCharacterState::snapshot);
}
