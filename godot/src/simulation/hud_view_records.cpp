#include "simulation/hud_view_records.h"

#include "util/record_bind.h"
#include "util/string_convert.h"

using namespace godot;
using opennova::to_gd;

// --- WaypointHudView --------------------------------------------------------

Vector3 WaypointHudView::get_position() const {
	if (value_.current < 0) return Vector3();
	// Fixed 16.16 mission (x,y,z) -> Godot (x, z, -y), like every entity read.
	return Vector3(value_.entry.x / 65536.0f, value_.entry.z / 65536.0f,
			-(value_.entry.y / 65536.0f));
}

void WaypointHudView::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(WaypointHudView, Variant::BOOL, show)
	OPENNOVA_RECORD_READ_ONLY(WaypointHudView, Variant::INT, count)
	OPENNOVA_RECORD_READ_ONLY(WaypointHudView, Variant::INT, current)
	OPENNOVA_RECORD_READ_ONLY(WaypointHudView, Variant::INT, number)
	OPENNOVA_RECORD_READ_ONLY(WaypointHudView, Variant::INT, name_id)
	OPENNOVA_RECORD_READ_ONLY(WaypointHudView, Variant::VECTOR3, position)
	OPENNOVA_RECORD_READ_ONLY(WaypointHudView, Variant::BOOL, done)
}

// --- HudMapGridOrigin -------------------------------------------------------

Vector3 HudMapGridOrigin::get_position() const {
	return value_.present
			? Vector3(value_.x_q16 / 65536.0f, 0.0f, -(value_.y_q16 / 65536.0f))
			: Vector3();
}

void HudMapGridOrigin::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(HudMapGridOrigin, Variant::BOOL, present)
	OPENNOVA_RECORD_READ_ONLY(HudMapGridOrigin, Variant::VECTOR3, position)
}

// --- HudMapOverlays ---------------------------------------------------------

void HudMapOverlays::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(HudMapOverlays, Variant::INT, pool_entity_count)
	OPENNOVA_RECORD_READ_ONLY(HudMapOverlays, Variant::INT, location_name_count)
	OPENNOVA_RECORD_READ_ONLY(HudMapOverlays, Variant::INT, name_count)
	OPENNOVA_RECORD_READ_ONLY(HudMapOverlays, Variant::INT, tracked_ticks)
	OPENNOVA_RECORD_READ_ONLY(HudMapOverlays, Variant::INT, player_slot_count)
	OPENNOVA_RECORD_READ_ONLY(HudMapOverlays, Variant::INT, zone_score_delta)
}

// --- VehiclePanelView -------------------------------------------------------

void VehiclePanelView::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(VehiclePanelView, Variant::BOOL, shown)
	OPENNOVA_RECORD_READ_ONLY(VehiclePanelView, Variant::INT, item_id)
}

// --- ScoreFeedback ----------------------------------------------------------

String ScoreFeedback::get_tone() const {
	return String(opennova::hud::score_tone_set_name(value_.tone));
}

void ScoreFeedback::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(ScoreFeedback, Variant::INT, score)
	OPENNOVA_RECORD_READ_ONLY(ScoreFeedback, Variant::INT, delta)
	OPENNOVA_RECORD_READ_ONLY(ScoreFeedback, Variant::STRING, tone)
}

// --- ScoreboardHeader -------------------------------------------------------

String ScoreboardHeader::get_server() const { return to_gd(value_.server_name); }
String ScoreboardHeader::get_mission() const { return to_gd(value_.mission_name); }

void ScoreboardHeader::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(ScoreboardHeader, Variant::BOOL, known)
	OPENNOVA_RECORD_READ_ONLY(ScoreboardHeader, Variant::BOOL, team_mode)
	OPENNOVA_RECORD_READ_ONLY(ScoreboardHeader, Variant::BOOL, timed)
	OPENNOVA_RECORD_READ_ONLY(ScoreboardHeader, Variant::INT, players)
	OPENNOVA_RECORD_READ_ONLY(ScoreboardHeader, Variant::INT, in_game)
	OPENNOVA_RECORD_READ_ONLY(ScoreboardHeader, Variant::INT, spectators)
	OPENNOVA_RECORD_READ_ONLY(ScoreboardHeader, Variant::INT, game_type)
	OPENNOVA_RECORD_READ_ONLY(ScoreboardHeader, Variant::STRING, server)
	OPENNOVA_RECORD_READ_ONLY(ScoreboardHeader, Variant::STRING, mission)
}

// --- EndRoundOverlay --------------------------------------------------------

PackedStringArray EndRoundOverlay::get_texts() const {
	PackedStringArray out;
	for (const opennova::hud::EndRoundResolvedLine &l : value_.lines) out.push_back(to_gd(l.text));
	return out;
}

PackedInt32Array EndRoundOverlay::get_ys() const {
	PackedInt32Array out;
	for (const opennova::hud::EndRoundResolvedLine &l : value_.lines) out.push_back(l.y);
	return out;
}

void EndRoundOverlay::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(EndRoundOverlay, Variant::PACKED_STRING_ARRAY, texts)
	OPENNOVA_RECORD_READ_ONLY(EndRoundOverlay, Variant::PACKED_INT32_ARRAY, ys)
	OPENNOVA_RECORD_READ_ONLY(EndRoundOverlay, Variant::INT, top)
	OPENNOVA_RECORD_READ_ONLY(EndRoundOverlay, Variant::INT, bottom)
}

// --- EndRoundStatistics -----------------------------------------------------

PackedStringArray EndRoundStatistics::get_label_keys() const {
	PackedStringArray out;
	for (const opennova::hud::EndRoundStatisticsRow &row : value_.rows)
		out.push_back(String::utf8(row.label_key));
	return out;
}

PackedStringArray EndRoundStatistics::get_values() const {
	PackedStringArray out;
	for (const opennova::hud::EndRoundStatisticsRow &row : value_.rows) out.push_back(to_gd(row.value));
	return out;
}

void EndRoundStatistics::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(EndRoundStatistics, Variant::BOOL, raised)
	OPENNOVA_RECORD_READ_ONLY(EndRoundStatistics, Variant::PACKED_STRING_ARRAY, label_keys)
	OPENNOVA_RECORD_READ_ONLY(EndRoundStatistics, Variant::PACKED_STRING_ARRAY, values)
}

// --- RoundOutcome -----------------------------------------------------------

Ref<RoundOutcome> RoundOutcome::make(bool p_ended, int p_winner_team) {
	opennova::world::RoundOutcomeView v;
	v.ended = p_ended;
	v.winner_team = p_winner_team;
	Ref<RoundOutcome> out;
	out.instantiate();
	out->assign(v);
	return out;
}

void RoundOutcome::_bind_methods() {
	ClassDB::bind_static_method("RoundOutcome", D_METHOD("make", "ended", "winner_team"),
			&RoundOutcome::make);
	OPENNOVA_RECORD_READ_ONLY(RoundOutcome, Variant::BOOL, ended)
	OPENNOVA_RECORD_READ_ONLY(RoundOutcome, Variant::INT, winner_team)
	OPENNOVA_RECORD_READ_ONLY(RoundOutcome, Variant::INT, bluekills)
	OPENNOVA_RECORD_READ_ONLY(RoundOutcome, Variant::INT, greenkills)
	OPENNOVA_RECORD_READ_ONLY(RoundOutcome, Variant::INT, enemy_kills)
	OPENNOVA_RECORD_READ_ONLY(RoundOutcome, Variant::INT, team_kills_by_others)
	OPENNOVA_RECORD_READ_ONLY(RoundOutcome, Variant::INT, friendly_kills_by_others)
	OPENNOVA_RECORD_READ_ONLY(RoundOutcome, Variant::INT, enemy_kills_by_others)
	OPENNOVA_RECORD_READ_ONLY(RoundOutcome, Variant::INT, humans)
	OPENNOVA_RECORD_READ_ONLY(RoundOutcome, Variant::BOOL, mp_session)
}

// --- EndRoundColumn ---------------------------------------------------------

String EndRoundColumn::get_header() const { return to_gd(value_.header); }
String EndRoundColumn::get_header_key() const { return to_gd(value_.header_key); }
String EndRoundColumn::get_header_fallback() const { return to_gd(value_.header_fallback); }
String EndRoundColumn::get_literal() const { return to_gd(value_.literal); }

void EndRoundColumn::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(EndRoundColumn, Variant::STRING, header)
	OPENNOVA_RECORD_READ_ONLY(EndRoundColumn, Variant::STRING, header_key)
	OPENNOVA_RECORD_READ_ONLY(EndRoundColumn, Variant::STRING, header_fallback)
	OPENNOVA_RECORD_READ_ONLY(EndRoundColumn, Variant::STRING, literal)
	OPENNOVA_RECORD_READ_ONLY(EndRoundColumn, Variant::INT, width)
	OPENNOVA_RECORD_READ_ONLY(EndRoundColumn, Variant::INT, field_id)
}

// --- EndRoundRow ------------------------------------------------------------

String EndRoundRow::get_name() const { return to_gd(value_.name); }
String EndRoundRow::get_squad() const { return to_gd(value_.squad); }
PackedStringArray EndRoundRow::get_cells() const {
	PackedStringArray out;
	for (const std::string &c : value_.cells) out.push_back(to_gd(c));
	return out;
}

void EndRoundRow::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(EndRoundRow, Variant::INT, slot)
	OPENNOVA_RECORD_READ_ONLY(EndRoundRow, Variant::INT, team)
	OPENNOVA_RECORD_READ_ONLY(EndRoundRow, Variant::STRING, name)
	OPENNOVA_RECORD_READ_ONLY(EndRoundRow, Variant::STRING, squad)
	OPENNOVA_RECORD_READ_ONLY(EndRoundRow, Variant::PACKED_STRING_ARRAY, cells)
	OPENNOVA_RECORD_READ_ONLY(EndRoundRow, Variant::INT, color)
	OPENNOVA_RECORD_READ_ONLY(EndRoundRow, Variant::BOOL, selected)
}

// --- DeployStatus -----------------------------------------------------------

void DeployStatus::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::BOOL, permanent_death)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::BOOL, show_instruction)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::BOOL, show_instruction2)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::BOOL, replace_instruction)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::STRING, instruction_text)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::STRING, instruction2_text)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::BOOL, show_round_status)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::STRING, round_text)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::STRING, remaining_players_text)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::STRING, respawn_text)

	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::INT, penalty_seconds)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::INT, revive_seconds)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::INT, hold_seconds)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::INT, queued_kind)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::INT, queued_zone_index)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::INT, queued_seconds)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::BOOL, queued_numbered)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::BOOL, show_psp_respawn)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::BOOL, show_medic)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::STRING, psp_respawn_text)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::STRING, medic_timer_text)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::STRING, call_medic_text)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::INT, medic_cooldown_ticks)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::INT, medic_request_serial)
	OPENNOVA_RECORD_READ_ONLY(DeployStatus, Variant::BOOL, show_team_buttons)
}
