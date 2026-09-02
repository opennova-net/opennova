#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

// The small per-frame HUD views Simulation hands the GDScript presenters: one
// typed RefCounted record per feed (ADR 0042 d5), read-write so a stub sim
// authors one. Each field list is an X-macro so the accessors, members and
// bindings are generated once (HUD_VIEW_RECORD_* in hud_view_records.cpp).
// The witnesses live on the engine state each builder reads.

#define HUD_VIEW_ACCESSORS(m_type, m_name)                    \
	m_type get_##m_name() const { return m_name##_; }        \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
#define HUD_VIEW_MEMBER(m_type, m_name) m_type m_name##_{};

namespace godot {

// The current-waypoint slice of the HUD info rebuild plus the scripted show
// gate (Simulation::get_waypoint_hud_view). `current` -1 = no selection yet;
// `position` is Godot space.
#define WAYPOINT_HUD_VIEW_FIELDS(X) \
	X(bool, show)                   \
	X(int, count)                   \
	X(int, current)                 \
	X(int, number)                  \
	X(int, name_id)                 \
	X(Vector3, position)            \
	X(bool, done)

class WaypointHudView : public RefCounted {
	GDCLASS(WaypointHudView, RefCounted)

public:
	WAYPOINT_HUD_VIEW_FIELDS(HUD_VIEW_ACCESSORS)

protected:
	static void _bind_methods();

private:
	WAYPOINT_HUD_VIEW_FIELDS(HUD_VIEW_MEMBER)
};

// The map grid-label origin (the first type-2043 marker), Godot space;
// `present` false until the marker resolves (Simulation::get_hud_map_grid_origin).
#define HUD_MAP_GRID_ORIGIN_FIELDS(X) \
	X(bool, present)                  \
	X(Vector3, position)

class HudMapGridOrigin : public RefCounted {
	GDCLASS(HudMapGridOrigin, RefCounted)

public:
	HUD_MAP_GRID_ORIGIN_FIELDS(HUD_VIEW_ACCESSORS)

protected:
	static void _bind_methods();

private:
	HUD_MAP_GRID_ORIGIN_FIELDS(HUD_VIEW_MEMBER)
};

// The vehicle the local player rides, re-rooted from an attached gun child to
// its parent (Simulation::get_vehicle_panel_view); the shell joins `item_id`'s
// items.def sid to its VEHICLE_HUD block.
#define VEHICLE_PANEL_VIEW_FIELDS(X) \
	X(bool, shown)                   \
	X(int, item_id)

class VehiclePanelView : public RefCounted {
	GDCLASS(VehiclePanelView, RefCounted)

public:
	VEHICLE_PANEL_VIEW_FIELDS(HUD_VIEW_ACCESSORS)

protected:
	static void _bind_methods();

private:
	VEHICLE_PANEL_VIEW_FIELDS(HUD_VIEW_MEMBER)
};

// One score-delta landing (Simulation::take_score_feedback, null when no new
// edge): the running score, the delta and the tone set name.
#define SCORE_FEEDBACK_FIELDS(X) \
	X(int, score)                \
	X(int, delta)                \
	X(String, tone)

class ScoreFeedback : public RefCounted {
	GDCLASS(ScoreFeedback, RefCounted)

public:
	SCORE_FEEDBACK_FIELDS(HUD_VIEW_ACCESSORS)

protected:
	static void _bind_methods();

private:
	SCORE_FEEDBACK_FIELDS(HUD_VIEW_MEMBER)
};

// The Tab-board header (Simulation::get_scoreboard): netsim's scoreboard_header
// counts plus the session game type and names. The rows never round-trip
// through script (HudOverlay pulls them natively).
#define SCOREBOARD_HEADER_FIELDS(X) \
	X(bool, known)                  \
	X(bool, team_mode)              \
	X(bool, timed)                  \
	X(int, players)                 \
	X(int, in_game)                 \
	X(int, spectators)              \
	X(int64_t, game_type)           \
	X(String, server)               \
	X(String, mission)

class ScoreboardHeader : public RefCounted {
	GDCLASS(ScoreboardHeader, RefCounted)

public:
	SCOREBOARD_HEADER_FIELDS(HUD_VIEW_ACCESSORS)

protected:
	static void _bind_methods();

private:
	SCOREBOARD_HEADER_FIELDS(HUD_VIEW_MEMBER)
};

// The resolved end-of-round overlay ladder (Simulation::get_end_round_overlay):
// one text per line with its design-space y, plus the safe-area top/bottom.
#define END_ROUND_OVERLAY_FIELDS(X) \
	X(PackedStringArray, texts)     \
	X(PackedInt32Array, ys)         \
	X(int, top)                     \
	X(int, bottom)

class EndRoundOverlay : public RefCounted {
	GDCLASS(EndRoundOverlay, RefCounted)

public:
	END_ROUND_OVERLAY_FIELDS(HUD_VIEW_ACCESSORS)

protected:
	static void _bind_methods();

private:
	END_ROUND_OVERLAY_FIELDS(HUD_VIEW_MEMBER)
};

// The SP Show Score counters (Simulation::get_end_round_statistics, null
// without a host world): parallel Epilog label keys and engine-composed values.
#define END_ROUND_STATISTICS_FIELDS(X) \
	X(bool, raised)                    \
	X(PackedStringArray, label_keys)   \
	X(PackedStringArray, values)

class EndRoundStatistics : public RefCounted {
	GDCLASS(EndRoundStatistics, RefCounted)

public:
	END_ROUND_STATISTICS_FIELDS(HUD_VIEW_ACCESSORS)

protected:
	static void _bind_methods();

private:
	END_ROUND_STATISTICS_FIELDS(HUD_VIEW_MEMBER)
};

// One RESULTLIST column (Simulation::get_end_round_columns): the header text
// resolved through the gametext Overlays table plus the engine column's key,
// fallback, literal, width and field id (np::StatScreenColumn).
#define END_ROUND_COLUMN_FIELDS(X) \
	X(String, header)              \
	X(String, header_key)          \
	X(String, header_fallback)     \
	X(String, literal)             \
	X(int, width)                  \
	X(int, field_id)

class EndRoundColumn : public RefCounted {
	GDCLASS(EndRoundColumn, RefCounted)

public:
	END_ROUND_COLUMN_FIELDS(HUD_VIEW_ACCESSORS)

protected:
	static void _bind_methods();

private:
	END_ROUND_COLUMN_FIELDS(HUD_VIEW_MEMBER)
};

// One RESULTLIST row (Simulation::get_end_round_rows): the roster slot and
// team, name and squad, the stat cells in column order, the row colour
// (ARGB) and the local-player selection (np::StatScreenRow).
#define END_ROUND_ROW_FIELDS(X)  \
	X(int, slot)                 \
	X(int, team)                 \
	X(String, name)              \
	X(String, squad)             \
	X(PackedStringArray, cells)  \
	X(int64_t, color)            \
	X(bool, selected)

class EndRoundRow : public RefCounted {
	GDCLASS(EndRoundRow, RefCounted)

public:
	END_ROUND_ROW_FIELDS(HUD_VIEW_ACCESSORS)

protected:
	static void _bind_methods();

private:
	END_ROUND_ROW_FIELDS(HUD_VIEW_MEMBER)
};

// The DEATH screen's STATIC facts (Simulation::get_deploy_status): the
// sub-block-0 timers, the queued status line (world::DeployStatusLine), the
// psp/medic show gates and the medic-call cooldown.
#define DEPLOY_STATUS_FIELDS(X)  \
	X(int, penalty_seconds)      \
	X(int, revive_seconds)       \
	X(int, hold_seconds)         \
	X(int, queued_kind)          \
	X(int, queued_zone_index)    \
	X(int, queued_seconds)       \
	X(bool, queued_numbered)     \
	X(bool, show_psp_respawn)    \
	X(bool, show_medic)          \
	X(int, medic_cooldown_ticks) \
	X(int, medic_request_serial)

class DeployStatus : public RefCounted {
	GDCLASS(DeployStatus, RefCounted)

public:
	DEPLOY_STATUS_FIELDS(HUD_VIEW_ACCESSORS)

protected:
	static void _bind_methods();

private:
	DEPLOY_STATUS_FIELDS(HUD_VIEW_MEMBER)
};

} // namespace godot

#undef HUD_VIEW_ACCESSORS
#undef HUD_VIEW_MEMBER
