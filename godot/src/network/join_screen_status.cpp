#include "network/join_screen_status.h"

#include "rtxt/rtxt_string_file.h"
#include "util/string_convert.h"

namespace godot {

Ref<JoinScreenStatus> JoinScreenStatus::make(opennova::inmatch::JoinScreenStage p_stage,
		const opennova::inmatch::JoinQueueRecord &p_queue, uint64_t p_now_ms) {
	Ref<JoinScreenStatus> out;
	out.instantiate();
	out->stage_ = p_stage;
	out->queue_ = p_queue;
	out->now_ms_ = p_now_ms;
	return out;
}

String JoinScreenStatus::message_text(const Ref<RtxtStringFile> &p_override_table,
		const Ref<RtxtStringFile> &p_gameerr) const {
	return opennova::cp1252_to_gd(opennova::inmatch::join_screen_text(stage_, queue_, now_ms_,
			p_override_table.is_valid() ? &p_override_table->get_native() : nullptr,
			p_gameerr.is_valid() ? &p_gameerr->get_native() : nullptr));
}

int JoinScreenStatus::window_count() {
	return opennova::inmatch::kPreGameWindowCount;
}

String JoinScreenStatus::window_name(int p_index) {
	return String(opennova::inmatch::pre_game_window_name(p_index));
}

bool JoinScreenStatus::panel_shows_window(int p_panel, int p_index) {
	if (p_index < 0 || p_index >= opennova::inmatch::kPreGameWindowCount) return false;
	if (p_panel < PANEL_ERROR || p_panel > PANEL_TEAM_PASSWORD) return false;
	const uint32_t windows = opennova::inmatch::pre_game_windows(
			static_cast<opennova::inmatch::PreGamePanel>(p_panel));
	return (windows & (1u << p_index)) != 0;
}

void JoinScreenStatus::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_stage"), &JoinScreenStatus::get_stage);
	ClassDB::bind_method(D_METHOD("message_text", "override_table", "gameerr"),
			&JoinScreenStatus::message_text);
	ClassDB::bind_static_method("JoinScreenStatus", D_METHOD("window_count"),
			&JoinScreenStatus::window_count);
	ClassDB::bind_static_method("JoinScreenStatus", D_METHOD("window_name", "index"),
			&JoinScreenStatus::window_name);
	ClassDB::bind_static_method("JoinScreenStatus",
			D_METHOD("panel_shows_window", "panel", "index"),
			&JoinScreenStatus::panel_shows_window);
	BIND_ENUM_CONSTANT(STAGE_JOINING);
	BIND_ENUM_CONSTANT(STAGE_CONNECTING);
	BIND_ENUM_CONSTANT(STAGE_VERIFYING);
	BIND_ENUM_CONSTANT(STAGE_QUEUED);
	BIND_ENUM_CONSTANT(STAGE_STARTING);
	BIND_ENUM_CONSTANT(PANEL_ERROR);
	BIND_ENUM_CONSTANT(PANEL_PROGRESS);
	BIND_ENUM_CONSTANT(PANEL_GAME_PASSWORD);
	BIND_ENUM_CONSTANT(PANEL_SPECTATE);
	BIND_ENUM_CONSTANT(PANEL_SPECTATOR_PASSWORD);
	BIND_ENUM_CONSTANT(PANEL_TEAM_PASSWORD);
}

} // namespace godot
