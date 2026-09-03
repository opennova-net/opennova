#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/session/client_replica_card.h>
#include <runtime/world/inspect.h>

namespace godot {

// One seat row of an entity debug card (world::inspect::SeatRow).
class EntityCardSeat : public RefCounted {
	GDCLASS(EntityCardSeat, RefCounted)

	opennova::world::inspect::SeatRow value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::inspect::SeatRow &p_value) { value_ = p_value; }
	// The seat's index in its entity's seat table (-1 for a def-level spec row).
	int get_index() const { return value_.index; }
	// world::SeatType.
	int get_type() const { return value_.type; }
	// Fixed retail mountHandles slot (passengers 0..7, control 8, UseGun 9).
	int get_retail_slot() const { return value_.retail_slot; }
	// The 1-based USRP table row the wire byte names.
	int get_bone_index() const { return value_.bone_index; }
	int get_pose_index() const { return value_.pose_index; }
	String get_source_name() const;
	// The authored seat offset, raw mission components (Z-up).
	Vector3 get_local() const;
	bool is_occupied() const { return value_.occupied; }
};

// The full per-entity debug card (world::inspect::EntityCard, ADR 0042 d5):
// the registry/world half, the AI half, and — on a joiner — the decoded
// replica section (np::ClientReplicaCard). Typed getters prefer the AI half
// when present, matching the old per-shape Dictionary getters; to_json_value()
// exists for the MCP boundary and carries the card key sets docs/mcp.md names.
class EntityCard : public RefCounted {
	GDCLASS(EntityCard, RefCounted)

	opennova::world::inspect::EntityCard value_;
	opennova::np::ClientReplicaCard replica_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::inspect::EntityCard &p_value) { value_ = p_value; }
	void assign_replica(const opennova::np::ClientReplicaCard &p_value) { replica_ = p_value; }
	bool native_valid() const { return value_.valid || replica_.valid; }

	bool has_ai() const { return value_.has_ai; }
	bool has_world() const { return value_.has_world; }

	// --- identity (AI half preferred, the detail-card precedence) ----
	int get_wire_handle() const { return static_cast<int>(value_.handle); }
	int get_ai_index() const { return value_.ai_index; }
	int get_kind() const;
	int get_source_index() const;
	int get_bms_id() const;
	int get_net_id() const;
	int get_item_id() const;
	String get_name() const;
	int get_team() const;
	int get_pool() const;
	int get_health() const;
	bool is_alive() const;
	bool is_hidden() const;
	int get_vehicle_family() const;
	Vector3 get_mission_position() const;
	// The engine axis map: mission (x, y, z) -> presentation (x, z, -y).
	Vector3 get_position() const;

	// --- the AI half --------------------------------------------------------
	int get_ai_health() const { return value_.ai.ai_health; }
	double get_yaw_deg() const { return value_.ai.yaw_deg; }
	int get_state() const { return value_.ai.state; }
	String get_state_name() const;
	int get_profile_type() const { return value_.ai.profile_type; }
	// The engine-running claimant latch (world::Entity::primary_occupant).
	bool has_primary_occupant() const { return value_.ai.primary_occupant; }
	int get_character_anim_slot() const { return value_.ai.character_anim_slot; }
	int get_minimap_net_id() const { return value_.ai.minimap_net_id; }
	bool is_infantry() const { return value_.ai.infantry; }
	String get_adm_name() const;
	int get_anim_state() const { return value_.ai.anim_state; }
	String get_anim_key() const;
	bool is_mounted() const { return value_.ai.mounted; }
	int get_mount_target_net_id() const { return value_.ai.mount_target_net_id; }
	int get_mount_seat() const { return value_.ai.mount_seat; }
	int get_mount_type() const { return value_.ai.mount_type; }
	bool is_mount_config_valid() const { return value_.ai.mount_config_valid; }
	int get_mount_config() const { return value_.ai.mount_config; }
	int get_mount_seat_bone() const { return value_.ai.mount_seat_bone; }
	int get_mount_seat_pose_index() const { return value_.ai.mount_seat_pose_index; }
	String get_mount_seat_source_name() const;
	Vector3 get_mount_seat_local() const;
	int get_mount_seat_yaw_offset() const { return value_.ai.mount_seat_yaw_offset; }
	bool is_mount_target_config_valid() const { return value_.ai.mount_target_config_valid; }
	int get_mount_target_config() const { return value_.ai.mount_target_config; }
	TypedArray<EntityCardSeat> get_mount_target_seats() const;

	// --- the world half -----------------------------------------------------
	int get_primary_weapon_clip() const { return value_.world.primary_weapon_clip; }
	int get_primary_weapon_reserve() const { return value_.world.primary_weapon_reserve; }
	TypedArray<EntityCardSeat> get_seats() const;
	// The registry row's items.def facts (the per-entity ItemDefAttrib words a
	// debug override rewrites, the def display name, healthMax); zero/empty
	// without a world half.
	int64_t get_item_attrib() const { return value_.world.item_attrib; }
	int64_t get_item_attrib2() const { return value_.world.item_attrib2; }
	String get_item_name() const;
	int get_health_max() const { return value_.world.health_max; }

	// The MCP boundary conversion only: the get_entity_debug /
	// get_world_entity_debug key set for this card's shape, with the joiner's
	// decoded replica attached as "client_entity_debug".
	Dictionary to_json_value() const;
};

} // namespace godot
