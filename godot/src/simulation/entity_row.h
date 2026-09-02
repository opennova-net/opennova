#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/world/inspect.h>

namespace godot {

// One typed row of the engine entity directory (world::inspect::EntityRow,
// ADR 0042 d5). Data only: the join itself runs engine-side
// (Simulation::entity_directory); to_json_value() exists for the MCP boundary
// and reproduces the legacy discovery-row key set.
class EntityRow : public RefCounted {
	GDCLASS(EntityRow, RefCounted)

	opennova::world::inspect::EntityRow value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::inspect::EntityRow &p_value) { value_ = p_value; }

	int get_index() const { return value_.index; }
	int get_ai_index() const { return value_.ai_index; }
	int get_kind() const { return value_.kind; }
	int get_source_index() const { return value_.source_index; }
	int get_bms_id() const { return value_.bms_id; }
	int get_net_id() const { return value_.net_id; }
	int get_wire_handle() const { return static_cast<int>(value_.wire_handle); }
	String get_name() const;
	// The items.def display name (world.item_names); empty without a def.
	String get_item_name() const;
	String get_state_name() const;
	int get_health() const { return value_.health; }
	int get_team() const { return value_.team; }
	bool is_alive() const { return value_.alive; }
	bool is_hidden() const { return value_.hidden; }
	// The engine axis map: mission (x, y, z) -> presentation (x, z, -y).
	Vector3 get_world_position() const;
	Vector3 get_mission_position() const;

	// The MCP boundary conversion only: the legacy discovery-summary keys.
	Dictionary to_json_value() const;
};

} // namespace godot
