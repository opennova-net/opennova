#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/world/deploy_screen_feed.h>

namespace godot {

// The DEATH screen's spawn-zone feed (Simulation.get_deploy_spawn_zones):
// one row per team-owned spawn zone with its SECURED verdict, the 0x6E wave
// countdown and the queued occupants, and the compiled SPAWNPOINTS_LIST
// rows (get_deploy_list_rows) the engine builder emits over them. Each is a
// value wrapper over the engine feed row (world/deploy_screen_feed.h carries
// the witnesses; ADR 0043 d10); the occupants read through indexed
// forwarders, never a nested per-occupant record.

// One team-owned spawn zone: `param` is the pick parameter (registry index
// + 1), `letter` 'A' + index, `name_key` the WPNames/STRWPNAME%03d key.
class DeployZoneRow : public RefCounted {
	GDCLASS(DeployZoneRow, RefCounted)

	opennova::world::DeployZoneRow value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::DeployZoneRow &p_value) { value_ = p_value; }
	const opennova::world::DeployZoneRow &value() const { return value_; }

	int get_param() const { return value_.index + 1; }
	String get_letter() const { return String::chr(value_.letter); }
	String get_name_key() const;
	bool is_secured() const { return value_.secured; }
	int get_wave_countdown() const { return static_cast<int>(value_.wave_countdown); }
	// The queued wave members: the roster name and the local-player marker
	// the builder renders as "<b><cFF4040>** name **".
	int get_occupant_count() const { return static_cast<int>(value_.occupants.size()); }
};

// One compiled SPAWNPOINTS_LIST row: the tagged text and the pick value
// (0 default, index + 1 zone, -1 occupant/blank — never a pick).
class DeployListRow : public RefCounted {
	GDCLASS(DeployListRow, RefCounted)

	opennova::world::DeployListRow value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::DeployListRow &p_value) { value_ = p_value; }

	String get_text() const;
	int get_value() const { return value_.value; }
};

} // namespace godot
