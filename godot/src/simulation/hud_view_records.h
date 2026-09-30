#pragma once

#include <godot_cpp/classes/ref.hpp>
#include "util/string_convert.h"
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/hud/end_round_overlay.h> // EndRoundOverlayLadder
#include <runtime/hud/end_round_statistics.h> // EndRoundStatisticsPanel
#include <runtime/hud/hud_minimap.h> // HudMapGridOrigin
#include <runtime/hud/score_fanfare.h> // ScoreFeedbackView
#include <runtime/inmatch/stat_screen_feed.h> // StatScreenColumn / StatScreenRow
#include <runtime/replication/client_scoreboard_view.h> // ClientScoreboardSession
#include <runtime/world/deploy_screen_feed.h> // DeployScreenStatus
#include <runtime/world/match.h> // RoundOutcomeView
#include <runtime/world/vehicle_panel_feed.h> // VehiclePanelRoot
#include <runtime/world/waypoint_track.h> // WaypointHudView

#include <cstdint>
#include <utility>

// The small per-frame HUD views Simulation hands the GDScript presenters: one
// value wrapper per feed over the engine view it names (ADR 0043 d10 —
// `engine::X value_` plus forwarding getters, never mirrored members). The
// witnesses live on the engine state each builder reads; the one static
// `make(...)` (RoundOutcome) exists because the shell's end flow and its test
// author the record.

namespace godot {

// The current-waypoint slice of the HUD info rebuild plus the scripted show
// gate (Simulation::get_waypoint_hud_view). `current` -1 = no selection yet;
// `position` is Godot space; `number` the 1-based display index.
class WaypointHudView : public RefCounted {
	GDCLASS(WaypointHudView, RefCounted)

	opennova::world::WaypointHudView value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::WaypointHudView &p_value) { value_ = p_value; }
	const opennova::world::WaypointHudView &value() const { return value_; }

	bool get_show() const { return value_.show; }
	int get_count() const { return value_.count; }
	int get_current() const { return value_.current; }
	int get_number() const { return value_.current >= 0 ? value_.current + 1 : 0; }
	int get_name_id() const { return value_.current >= 0 ? value_.entry.name_id : 0; }
	Vector3 get_position() const;
	bool get_done() const { return value_.current >= 0 && value_.entry.done; }
};

// The map grid-label origin (the first type-2043 marker), Godot space;
// `present` false until the marker resolves (Simulation::get_hud_map_grid_origin).
class HudMapGridOrigin : public RefCounted {
	GDCLASS(HudMapGridOrigin, RefCounted)

	opennova::hud::HudMapGridOrigin value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::hud::HudMapGridOrigin &p_value) { value_ = p_value; }

	bool get_present() const { return value_.present; }
	Vector3 get_position() const;
};

// The non-bank map legs' feed (Simulation::get_hud_minimap_overlays): the
// HUD overlay copies the value into its frame state
// (HudOverlay::set_minimap_overlays).
class HudMapOverlays : public RefCounted {
	GDCLASS(HudMapOverlays, RefCounted)

	opennova::hud::HudMinimapOverlays value_;

protected:
	static void _bind_methods();

public:
	void assign(opennova::hud::HudMinimapOverlays &&p_value) { value_ = std::move(p_value); }
	const opennova::hud::HudMinimapOverlays &value() const { return value_; }

	int get_pool_entity_count() const { return static_cast<int>(value_.pool3.size()); }
	int get_location_name_count() const { return static_cast<int>(value_.location_names.size()); }
	int get_name_count() const { return static_cast<int>(value_.names.size()); }
	int get_tracked_ticks() const { return value_.tracked.ticks; }
	int get_player_slot_count() const { return static_cast<int>(value_.player_slots.size()); }
	int get_zone_score_delta() const { return value_.zone_score_delta; }
};

// The vehicle the local player rides, re-rooted from an attached gun child to
// its parent (Simulation::get_vehicle_panel_view); the shell joins `item_id`'s
// items.def sid to its VEHICLE_HUD block.
class VehiclePanelView : public RefCounted {
	GDCLASS(VehiclePanelView, RefCounted)

	opennova::world::VehiclePanelRoot value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::VehiclePanelRoot &p_value) { value_ = p_value; }

	bool get_shown() const { return value_.shown; }
	int get_item_id() const { return value_.item_id; }
};

// One score-delta landing (Simulation::take_score_feedback, null when no new
// edge): the running score, the delta and the tone set name.
class ScoreFeedback : public RefCounted {
	GDCLASS(ScoreFeedback, RefCounted)

	opennova::hud::ScoreFeedbackView value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::hud::ScoreFeedbackView &p_value) { value_ = p_value; }

	int get_score() const { return value_.score; }
	int get_delta() const { return value_.delta; }
	String get_tone() const;
};

// The Tab-board header (Simulation::get_scoreboard): the replication projection's
// counts plus the session game type and names. The rows never round-trip
// through script (HudOverlay pulls them natively).
class ScoreboardHeader : public RefCounted {
	GDCLASS(ScoreboardHeader, RefCounted)

	opennova::replication::ClientScoreboardSession value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::replication::ClientScoreboardSession &p_value) { value_ = p_value; }

	bool get_known() const { return value_.header.known; }
	bool get_team_mode() const { return value_.header.team_mode; }
	bool get_timed() const { return value_.header.timed; }
	int get_players() const { return value_.header.players; }
	int get_in_game() const { return value_.header.in_game; }
	int get_spectators() const { return value_.header.spectators; }
	int64_t get_game_type() const { return static_cast<int64_t>(value_.game_type); }
	String get_server() const;
	String get_mission() const;
};

// The resolved end-of-round overlay ladder (Simulation::get_end_round_overlay):
// one text per line with its design-space y, plus the safe-area top/bottom.
class EndRoundOverlay : public RefCounted {
	GDCLASS(EndRoundOverlay, RefCounted)

	opennova::hud::EndRoundOverlayLadder value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::hud::EndRoundOverlayLadder &p_value) { value_ = p_value; }

	PackedStringArray get_texts() const;
	PackedInt32Array get_ys() const;
	int get_top() const { return value_.top; }
	int get_bottom() const { return value_.bottom; }
};

// The SP Show Score counters (Simulation::get_end_round_statistics, null
// without a host world) or the SP win epilog's lines
// (Simulation::get_epilog_score): parallel Epilog label keys and
// engine-composed values.
class EndRoundStatistics : public RefCounted {
	GDCLASS(EndRoundStatistics, RefCounted)

	opennova::hud::EndRoundStatisticsPanel value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::hud::EndRoundStatisticsPanel &p_value) { value_ = p_value; }
	// The epilog's four lines composed by hud::epilog_score_lines.
	static Ref<EndRoundStatistics> epilog(const opennova::hud::EndRoundStatisticsInput &p_in);
	// The same over authored counters: the shell's sim-less mount and the
	// end-screen tests.
	static Ref<EndRoundStatistics> make_epilog(int p_subgoals_won, int p_subgoals_defined,
			int p_enemy_kills, int p_enemy_unit_total, int p_team_unit_kills,
			int p_friendly_unit_kills);

	bool get_raised() const { return value_.raised; }
	PackedStringArray get_label_keys() const;
	PackedStringArray get_values() const;
};

// The sim-side end-of-round state plus the SP kill-stat buckets the epilog
// score screen and the WAC bluekills/greenkills builtins read
// (Simulation::get_round_outcome_debug; engine: runtime/world/match.h).
class RoundOutcome : public RefCounted {
	GDCLASS(RoundOutcome, RefCounted)

	opennova::world::RoundOutcomeView value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::RoundOutcomeView &p_value) { value_ = p_value; }
	// The shell's sim-less fallback and the end-screen tests author the
	// outcome pair.
	static Ref<RoundOutcome> make(bool p_ended, int p_winner_team);

	bool get_ended() const { return value_.ended; }
	int get_winner_team() const { return value_.winner_team; }
	int get_bluekills() const { return value_.bluekills; }
	int get_greenkills() const { return value_.greenkills; }
	int get_enemy_kills() const { return value_.enemy_kills; }
	int get_team_kills_by_others() const { return value_.team_kills_by_others; }
	int get_friendly_kills_by_others() const { return value_.friendly_kills_by_others; }
	int get_enemy_kills_by_others() const { return value_.enemy_kills_by_others; }
	int get_humans() const { return value_.humans; }
	bool get_mp_session() const { return value_.mp_session; }
};

// One RESULTLIST column (Simulation::get_end_round_columns;
// inmatch::StatScreenColumn with the embedder-resolved header text).
class EndRoundColumn : public RefCounted {
	GDCLASS(EndRoundColumn, RefCounted)

	opennova::inmatch::StatScreenColumn value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::inmatch::StatScreenColumn &p_value) { value_ = p_value; }

	String get_header() const;
	String get_header_key() const;
	String get_header_fallback() const;
	String get_literal() const;
	int get_width() const { return value_.width; }
	int get_field_id() const { return value_.field_id; }
};

// One RESULTLIST row (Simulation::get_end_round_rows; inmatch::StatScreenRow).
class EndRoundRow : public RefCounted {
	GDCLASS(EndRoundRow, RefCounted)

	opennova::inmatch::StatScreenRow value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::inmatch::StatScreenRow &p_value) { value_ = p_value; }

	int get_slot() const { return static_cast<int>(value_.slot); }
	int get_team() const { return static_cast<int>(value_.team); }
	String get_name() const;
	String get_squad() const;
	PackedStringArray get_cells() const;
	int64_t get_color() const { return static_cast<int64_t>(value_.color_argb); }
	bool get_selected() const { return value_.selected; }
};

// The DEATH screen's STATIC facts (Simulation::get_deploy_status;
// world::DeployScreenStatus).
class DeployStatus : public RefCounted {
	GDCLASS(DeployStatus, RefCounted)

	opennova::world::DeployScreenStatus value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::DeployScreenStatus &p_value) { value_ = p_value; }
	const opennova::world::DeployScreenStatus &value() const { return value_; }


    bool get_permanent_death() const { return value_.instructions.permanent_death; }
    bool get_show_instruction() const { return value_.instructions.show_first; }
    bool get_show_instruction2() const { return value_.instructions.show_second; }
    bool get_replace_instruction() const { return value_.instructions.replace_first; }
    String get_instruction_text() const { return opennova::to_gd(value_.instructions.first_text); }
    String get_instruction2_text() const { return opennova::to_gd(value_.instructions.second_text); }
    bool get_show_round_status() const { return value_.instructions.show_round_status; }
    String get_round_text() const { return opennova::to_gd(value_.instructions.round_text); }
    String get_remaining_players_text() const { return opennova::to_gd(value_.instructions.remaining_players_text); }
    String get_respawn_text() const { return opennova::to_gd(value_.respawn_text); }

	int get_penalty_seconds() const { return value_.penalty_seconds; }
	int get_revive_seconds() const { return value_.revive_seconds; }
	int get_hold_seconds() const { return value_.hold_seconds; }
	int get_queued_kind() const { return static_cast<int>(value_.line.kind); }
	int get_queued_zone_index() const { return value_.line.zone_index; }
	int get_queued_seconds() const { return value_.line.seconds; }
	bool get_queued_numbered() const { return value_.line.numbered; }
	bool get_show_psp_respawn() const { return value_.statics.psp_respawn; }
	bool get_show_medic() const { return value_.statics.medic; }
	// The three statics' texts (world/deploy_screen_feed.h deploy_statics_text).
	String get_psp_respawn_text() const { return opennova::to_gd(value_.statics_text.psp_respawn); }
	String get_medic_timer_text() const { return opennova::to_gd(value_.statics_text.medic_timer); }
	String get_call_medic_text() const { return opennova::to_gd(value_.statics_text.call_medic); }
	int get_medic_cooldown_ticks() const { return value_.medic_cooldown_ticks; }
	int get_medic_request_serial() const { return value_.medic_request_serial; }
};

} // namespace godot
