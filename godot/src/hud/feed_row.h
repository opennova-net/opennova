#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <runtime/hud/feed_format.h>

namespace godot {

// One resolved message-feed row (hud::FeedRow, ADR 0040 B3). Data only: the
// fold from a 0x1E game event to this row is the engine's feed_event_rows;
// the presenter resolves `key` (and a camp row's `wpname_key`) against
// gametext and formats the line through Simulation.format_feed_line /
// format_feed_camp_line, then posts it in `color`.
class FeedRow : public RefCounted {
	GDCLASS(FeedRow, RefCounted)

	opennova::hud::FeedRow value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::hud::FeedRow &p_value) { value_ = p_value; }

	int get_event_type() const { return value_.event_type; }
	// hud::GameEventKind as its byte.
	int get_kind() const { return value_.kind; }
	// The compose form: a camp line takes the level's WPNames string, an
	// actor line the attacker/victim names.
	bool is_camp() const { return value_.camp; }
	// The local player took part (attacker or victim).
	bool is_own() const { return value_.own; }
	// The "Canned Msg" template key.
	String get_key() const;
	String get_attacker() const;
	String get_victim() const;
	// The bonus-credited aux actor's name, only when that is the local player
	// (drives the STRCND48 re-compose).
	String get_extra() const;
	// The camp level's "WPNames" key (camp rows only).
	String get_wpname_key() const;
	// Packed ARGB, as the feed sink takes it.
	int64_t get_color() const { return static_cast<int64_t>(value_.color); }
};

} // namespace godot
