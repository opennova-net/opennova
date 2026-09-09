#include "simulation/entity_card.h"
#include "util/axes.h"

#include "simulation/simulation_internal.h" // mission_to_godot, the ONE axis map

#include <godot_cpp/variant/array.hpp>

namespace godot {

namespace {

using opennova::world::inspect::AiDetail;
using opennova::world::inspect::SeatRow;
using opennova::world::inspect::WorldDetail;

Vector3 raw_mission(const opennova::world::Vec3 &p) {
	return Vector3(p.x, p.y, p.z);
}

// The AI card's MCP JSON key set (docs/mcp.md).
Dictionary ai_json(const AiDetail &d) {
	Dictionary out;
	out["kind"] = d.kind;
	out["index"] = d.source_index;
	out["bms_id"] = d.bms_id;
	out["item_id"] = d.item_id;
	out["name"] = String(d.name.c_str());
	out["group_id"] = d.group_id;
	out["team"] = d.team;
	out["pool"] = d.pool;
	out["engine_flags"] = static_cast<int64_t>(d.engine_flags);
	out["bms_flags"] = static_cast<int64_t>(d.bms_flags);
	out["waypoint_id"] = d.waypoint_id;
	out["wp_number"] = d.wp_number;
	out["health"] = d.health;
	out["alive"] = d.alive;
	out["hidden"] = d.hidden;
	out["held"] = d.held;
	out["disabled"] = d.disabled;
	out["vehicle_family"] = d.vehicle_family;
	out["body_anim_slot"] = d.body_anim_slot;
	out["character_anim_slot"] = d.character_anim_slot;
	out["minimap_net_id"] = d.minimap_net_id;
	out["mounted"] = d.mounted;
	out["mount_target_net_id"] = d.mount_target_net_id;
	out["mount_seat"] = d.mount_seat;
	out["mount_type"] = d.mount_type;
	out["mount_config_valid"] = d.mount_config_valid;
	out["mount_config"] = d.mount_config;
	out["mount_seat_bone"] = d.mount_seat_bone;
	out["mount_seat_pose_index"] = d.mount_seat_pose_index;
	out["mount_seat_source_name"] = String(d.mount_seat_source_name.c_str());
	out["mount_seat_local"] = raw_mission(d.mount_seat_local);
	out["mount_seat_yaw_offset"] = d.mount_seat_yaw_offset;
	out["mount_target_config_valid"] = d.mount_target_config_valid;
	out["mount_target_config"] = d.mount_target_config;
	out["mount_target_seat_count"] = d.mount_target_seat_count;
	Array target_seats;
	for (const SeatRow &seat : d.mount_target_seats) {
		Dictionary sd;
		sd["index"] = seat.index;
		sd["type"] = seat.type;
		sd["retail_slot"] = seat.retail_slot;
		sd["bone_index"] = seat.bone_index;
		sd["pose_index"] = seat.pose_index;
		sd["source_name"] = String(seat.source_name.c_str());
		sd["local"] = raw_mission(seat.local);
		sd["yaw_offset"] = seat.yaw_offset;
		sd["occupied"] = seat.occupied;
		target_seats.push_back(sd);
	}
	out["mount_target_seats"] = target_seats;
	out["net_id"] = d.net_id;
	out["wire_handle"] = d.wire_handle;
	out["ai_health"] = d.ai_health;
	out["position"] = mission_to_godot(d.mission_position);
	out["yaw_deg"] = d.yaw_deg;
	out["state"] = d.state;
	out["state_name"] = String(d.state_name.c_str());
	out["pending_state"] = d.pending_state;
	out["alert"] = d.alert;
	out["alert_brain"] = d.alert_brain;
	out["wp_channel"] = d.wp_channel;
	out["wp_node"] = d.wp_node;
	out["wp_distance"] = d.wp_distance;
	out["out_speed"] = d.out_speed;
	if (d.has_vehicle_block) {
		out["rotor_speed"] = d.rotor_speed;
		out["rotor_phase"] = d.rotor_phase;
		out["rotor_rate"] = d.rotor_rate;
		out["profile_type"] = d.profile_type;
		out["primary_occupant"] = d.primary_occupant;
		out["veh_family"] = d.veh_family;
		out["player_control"] = d.player_control;
		out["vp"] = d.vp;
		out["vr"] = d.vr;
		out["mspd"] = d.mspd;
		Array wc;
		for (int i = 0; i < 4; ++i) wc.append(d.wc[i]);
		out["wc"] = wc;
		Array pd;
		for (int i = 0; i < 4; ++i) pd.append(d.pd[i]);
		out["pd"] = pd;
		out["macc"] = d.macc;
		out["mgnd"] = d.mgnd;
		out["cmd_fwd"] = d.cmd_fwd;
		out["cmd_lat"] = d.cmd_lat;
		out["alt_tgt"] = d.alt_tgt;
		out["engine_on"] = d.engine_on;
		out["pilot_move"] = d.pilot_move;
	}
	out["infantry"] = d.infantry;
	out["adm_id"] = d.adm_id;
	out["adm_name"] = String::utf8(d.adm_name.c_str());
	out["infantry_move_mode"] = d.infantry_move_mode;
	out["root_dx"] = d.root_dx;
	out["root_dy"] = d.root_dy;
	out["res_dx"] = d.res_dx;
	out["res_dy"] = d.res_dy;
	out["contact_item"] = d.contact_item;
	out["parent"] = d.parent;
	out["ground"] = d.ground;
	out["s35"] = d.s35;
	out["s37"] = d.s37;
	out["s38"] = d.s38;
	out["fires_aimed"] = d.fires_aimed;
	out["fires_body"] = d.fires_body;
	out["ground_cache"] = d.ground_cache;
	out["ground_valid"] = d.ground_valid;
	out["airborne"] = d.airborne;
	out["anim_state"] = d.anim_state;
	out["anim_key"] = String(d.anim_key.c_str());
	out["sight_range_u"] = d.sight_range_u;
	out["attack_range_u"] = d.attack_range_u;
	out["ammo_primary"] = d.ammo_primary;
	out["clip_size"] = d.clip_size;
	out["magazine"] = d.magazine;
	out["combat_target_valid"] = d.combat_target_valid;
	out["muzzle_valid"] = d.muzzle_valid;
	out["muzzle"] = d.muzzle_valid ? mission_to_godot(d.muzzle) : Vector3();
	out["death_anim_state"] = d.death_anim_state;
	out["corpse_timer"] = d.corpse_timer;
	out["deathtime_ticks"] = d.deathtime_ticks;
	out["leave_corpse"] = d.leave_corpse;
	return out;
}

// The world card's MCP JSON key set (docs/mcp.md).
Dictionary world_json(const WorldDetail &d) {
	Dictionary out;
	out["net_id"] = d.net_id;
	out["bms_id"] = d.bms_id;
	out["pool"] = d.pool;
	out["kind"] = d.kind;
	out["index"] = d.source_index;
	out["item_id"] = d.item_id;
	out["name"] = String(d.name.c_str());
	out["item_name"] = String(d.item_name.c_str());
	out["team"] = d.team;
	out["alive"] = d.alive;
	out["hidden"] = d.hidden;
	out["health"] = d.health;
	out["health_max"] = d.health_max;
	out["has_item_def"] = d.has_item_def;
	out["handle"] = d.handle;
	out["item_type"] = d.item_type;
	out["item_unit_type"] = d.item_unit_type;
	out["item_attrib"] = static_cast<int64_t>(d.item_attrib);
	out["item_attrib2"] = static_cast<int64_t>(d.item_attrib2);
	out["vehicle_family"] = d.vehicle_family;
	out["has_minimap_model_marker"] = d.has_minimap_model_marker;
	out["is_capture_trigger"] = d.is_capture_trigger;
	out["is_spawn_point"] = d.is_spawn_point;
	out["zone_number"] = d.zone_number;
	out["zone_radius"] = d.zone_radius;
	out["zone_control"] = d.zone_control;
	out["zone_chain_index"] = d.zone_chain_index;
	out["mission_position"] = raw_mission(d.mission_position);
	out["position"] = mission_to_godot(d.mission_position);
	out["yaw"] = d.yaw;
	out["pitch"] = d.pitch;
	out["roll"] = d.roll;
	out["primary_weapon_clip"] = d.primary_weapon_clip;
	out["primary_weapon_reserve"] = d.primary_weapon_reserve;
	out["seat_count"] = static_cast<int>(d.seats.size());
	Array seats;
	for (const SeatRow &seat : d.seats) {
		Dictionary sd;
		sd["type"] = seat.type;
		sd["occupied"] = seat.occupied;
		sd["local"] = raw_mission(seat.local);
		sd["name"] = String(seat.source_name.c_str());
		seats.push_back(sd);
	}
	out["seats"] = seats;
	return out;
}

// The client card's MCP JSON key set (docs/mcp.md).
Dictionary replica_json(const opennova::inmatch::ClientReplicaCard &d) {
	Dictionary out;
	out["handle"] = d.handle;
	out["type_id"] = d.type_id;
	out["cls"] = d.cls;
	out["net_id"] = d.net_id;
	out["name"] = String(d.name.c_str());
	out["carrier_handle"] = d.carrier_handle;
	out["mount_bone"] = d.mount_bone;
	out["seat_type"] = d.seat_type;
	out["net_seat_valid"] = d.net_seat_valid;
	out["heading_bam"] = d.heading_bam;
	out["heading_deg"] = d.heading_deg;
	out["heading_known"] = d.heading_known;
	out["pitch_bam"] = d.pitch_bam;
	out["yaw_byte"] = d.yaw_byte;
	out["mission_position"] = raw_mission(d.mission_position);
	out["anim_state_id"] = d.anim_state_id;
	out["state_flags"] = d.state_flags;
	out["state_flags_known"] = d.state_flags_known;
	out["team"] = d.team;
	out["compact_revision"] = static_cast<int64_t>(d.compact_revision);
	out["spawn_revision"] = static_cast<int64_t>(d.spawn_revision);
	return out;
}

} // namespace

String EntityCardSeat::get_source_name() const {
	return String(value_.source_name.c_str());
}

Vector3 EntityCardSeat::get_local() const {
	return raw_mission(value_.local);
}

void EntityCardSeat::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_index"), &EntityCardSeat::get_index);
	ClassDB::bind_method(D_METHOD("get_type"), &EntityCardSeat::get_type);
	ClassDB::bind_method(D_METHOD("get_retail_slot"), &EntityCardSeat::get_retail_slot);
	ClassDB::bind_method(D_METHOD("get_bone_index"), &EntityCardSeat::get_bone_index);
	ClassDB::bind_method(D_METHOD("get_pose_index"), &EntityCardSeat::get_pose_index);
	ClassDB::bind_method(D_METHOD("get_source_name"), &EntityCardSeat::get_source_name);
	ClassDB::bind_method(D_METHOD("get_local"), &EntityCardSeat::get_local);
	ClassDB::bind_method(D_METHOD("is_occupied"), &EntityCardSeat::is_occupied);
}

int EntityCard::get_kind() const {
	return value_.has_ai ? value_.ai.kind : value_.world.kind;
}

int EntityCard::get_source_index() const {
	return value_.has_ai ? value_.ai.source_index : value_.world.source_index;
}

int EntityCard::get_bms_id() const {
	return value_.has_ai ? value_.ai.bms_id : value_.world.bms_id;
}

int EntityCard::get_net_id() const {
	return value_.has_ai ? value_.ai.net_id : value_.world.net_id;
}

int EntityCard::get_item_id() const {
	return value_.has_ai ? value_.ai.item_id : value_.world.item_id;
}

String EntityCard::get_name() const {
	return String((value_.has_ai ? value_.ai.name : value_.world.name).c_str());
}

String EntityCard::get_item_name() const {
	return String(value_.world.item_name.c_str());
}

int EntityCard::get_team() const {
	return value_.has_ai ? value_.ai.team : value_.world.team;
}

int EntityCard::get_pool() const {
	return value_.has_ai ? value_.ai.pool : value_.world.pool;
}

int EntityCard::get_health() const {
	return value_.has_ai ? value_.ai.health : value_.world.health;
}

bool EntityCard::is_alive() const {
	return value_.has_ai ? value_.ai.alive : value_.world.alive;
}

bool EntityCard::is_hidden() const {
	return value_.has_ai ? value_.ai.hidden : value_.world.hidden;
}

int EntityCard::get_vehicle_family() const {
	return value_.has_ai ? value_.ai.vehicle_family : value_.world.vehicle_family;
}

Vector3 EntityCard::get_mission_position() const {
	return raw_mission(value_.has_ai ? value_.ai.mission_position
									 : value_.world.mission_position);
}

Vector3 EntityCard::get_position() const {
	return mission_to_godot(value_.has_ai ? value_.ai.mission_position
											: value_.world.mission_position);
}

String EntityCard::get_state_name() const {
	return String(value_.ai.state_name.c_str());
}

String EntityCard::get_adm_name() const {
	return String::utf8(value_.ai.adm_name.c_str());
}

String EntityCard::get_anim_key() const {
	return String(value_.ai.anim_key.c_str());
}

String EntityCard::get_mount_seat_source_name() const {
	return String(value_.ai.mount_seat_source_name.c_str());
}

Vector3 EntityCard::get_mount_seat_local() const {
	return raw_mission(value_.ai.mount_seat_local);
}

TypedArray<EntityCardSeat> EntityCard::get_mount_target_seats() const {
	TypedArray<EntityCardSeat> out;
	for (const SeatRow &seat : value_.ai.mount_target_seats) {
		Ref<EntityCardSeat> row;
		row.instantiate();
		row->assign(seat);
		out.push_back(row);
	}
	return out;
}

TypedArray<EntityCardSeat> EntityCard::get_seats() const {
	TypedArray<EntityCardSeat> out;
	for (const SeatRow &seat : value_.world.seats) {
		Ref<EntityCardSeat> row;
		row.instantiate();
		row->assign(seat);
		out.push_back(row);
	}
	return out;
}

Dictionary EntityCard::to_json_value() const {
	// The detail precedence: the AI card when a brain exists, else the
	// world card; a replica-only row (a joiner handle with no local half)
	// starts empty like the old empty-detail rows did.
	Dictionary out;
	if (value_.has_ai) {
		out = ai_json(value_.ai);
		if (value_.has_world) {
			// The registry row's items.def facts ride the AI card too: the
			// per-entity attrib words a set_entity_item_attrib override
			// rewrites are only readable here, and an AI row is the usual
			// target of that override.
			out["item_attrib"] = static_cast<int64_t>(value_.world.item_attrib);
			out["item_attrib2"] = static_cast<int64_t>(value_.world.item_attrib2);
			out["item_name"] = String(value_.world.item_name.c_str());
			out["health_max"] = value_.world.health_max;
		}
	} else if (value_.has_world) {
		out = world_json(value_.world);
	}
    out["facial_available"] = value_.facial.available;
    out["facial_current"] = value_.facial.current;
    out["facial_next"] = value_.facial.next;
    out["facial_override"] = value_.facial.expression_override;
    out["facial_automatic"] = value_.facial.automatic;
    out["facial_override_timer"] = value_.facial.override_timer;
    out["facial_texture_priority"] = value_.facial.texture_priority;
    out["facial_blend"] = value_.facial.blend;
    out["facial_display_frame"] = static_cast<int64_t>(value_.facial.display_frame);
	if (replica_.valid) {
		out["client_entity_debug"] = replica_json(replica_);
	}
	return out;
}

void EntityCard::_bind_methods() {
	ClassDB::bind_method(D_METHOD("has_facial_animation"), &EntityCard::has_facial_animation);
	ClassDB::bind_method(D_METHOD("get_facial_expression"), &EntityCard::get_facial_expression);
	ClassDB::bind_method(D_METHOD("get_facial_target"), &EntityCard::get_facial_target);
	ClassDB::bind_method(D_METHOD("get_facial_override"), &EntityCard::get_facial_override);
	ClassDB::bind_method(D_METHOD("get_facial_automatic"), &EntityCard::get_facial_automatic);
	ClassDB::bind_method(D_METHOD("get_facial_override_timer"), &EntityCard::get_facial_override_timer);
	ClassDB::bind_method(D_METHOD("get_facial_texture_priority"), &EntityCard::get_facial_texture_priority);
	ClassDB::bind_method(D_METHOD("get_facial_blend"), &EntityCard::get_facial_blend);
	ClassDB::bind_method(D_METHOD("get_facial_display_frame"), &EntityCard::get_facial_display_frame);
	ClassDB::bind_method(D_METHOD("has_ai"), &EntityCard::has_ai);
	ClassDB::bind_method(D_METHOD("has_world"), &EntityCard::has_world);
	ClassDB::bind_method(D_METHOD("get_wire_handle"), &EntityCard::get_wire_handle);
	ClassDB::bind_method(D_METHOD("get_ai_index"), &EntityCard::get_ai_index);
	ClassDB::bind_method(D_METHOD("get_kind"), &EntityCard::get_kind);
	ClassDB::bind_method(D_METHOD("get_source_index"), &EntityCard::get_source_index);
	ClassDB::bind_method(D_METHOD("get_bms_id"), &EntityCard::get_bms_id);
	ClassDB::bind_method(D_METHOD("get_net_id"), &EntityCard::get_net_id);
	ClassDB::bind_method(D_METHOD("get_item_id"), &EntityCard::get_item_id);
	ClassDB::bind_method(D_METHOD("get_name"), &EntityCard::get_name);
	ClassDB::bind_method(D_METHOD("get_team"), &EntityCard::get_team);
	ClassDB::bind_method(D_METHOD("get_pool"), &EntityCard::get_pool);
	ClassDB::bind_method(D_METHOD("get_health"), &EntityCard::get_health);
	ClassDB::bind_method(D_METHOD("is_alive"), &EntityCard::is_alive);
	ClassDB::bind_method(D_METHOD("is_hidden"), &EntityCard::is_hidden);
	ClassDB::bind_method(D_METHOD("get_vehicle_family"), &EntityCard::get_vehicle_family);
	ClassDB::bind_method(D_METHOD("get_mission_position"), &EntityCard::get_mission_position);
	ClassDB::bind_method(D_METHOD("get_position"), &EntityCard::get_position);
	ClassDB::bind_method(D_METHOD("get_ai_health"), &EntityCard::get_ai_health);
	ClassDB::bind_method(D_METHOD("get_yaw_deg"), &EntityCard::get_yaw_deg);
	ClassDB::bind_method(D_METHOD("get_state"), &EntityCard::get_state);
	ClassDB::bind_method(D_METHOD("get_state_name"), &EntityCard::get_state_name);
	ClassDB::bind_method(D_METHOD("get_profile_type"), &EntityCard::get_profile_type);
	ClassDB::bind_method(D_METHOD("has_primary_occupant"), &EntityCard::has_primary_occupant);
	ClassDB::bind_method(D_METHOD("get_character_anim_slot"), &EntityCard::get_character_anim_slot);
	ClassDB::bind_method(D_METHOD("get_minimap_net_id"), &EntityCard::get_minimap_net_id);
	ClassDB::bind_method(D_METHOD("is_infantry"), &EntityCard::is_infantry);
	ClassDB::bind_method(D_METHOD("get_adm_name"), &EntityCard::get_adm_name);
	ClassDB::bind_method(D_METHOD("get_anim_state"), &EntityCard::get_anim_state);
	ClassDB::bind_method(D_METHOD("get_anim_key"), &EntityCard::get_anim_key);
	ClassDB::bind_method(D_METHOD("is_mounted"), &EntityCard::is_mounted);
	ClassDB::bind_method(D_METHOD("get_mount_target_net_id"), &EntityCard::get_mount_target_net_id);
	ClassDB::bind_method(D_METHOD("get_mount_seat"), &EntityCard::get_mount_seat);
	ClassDB::bind_method(D_METHOD("get_mount_type"), &EntityCard::get_mount_type);
	ClassDB::bind_method(D_METHOD("is_mount_config_valid"), &EntityCard::is_mount_config_valid);
	ClassDB::bind_method(D_METHOD("get_mount_config"), &EntityCard::get_mount_config);
	ClassDB::bind_method(D_METHOD("get_mount_seat_bone"), &EntityCard::get_mount_seat_bone);
	ClassDB::bind_method(D_METHOD("get_mount_seat_pose_index"), &EntityCard::get_mount_seat_pose_index);
	ClassDB::bind_method(D_METHOD("get_mount_seat_source_name"), &EntityCard::get_mount_seat_source_name);
	ClassDB::bind_method(D_METHOD("get_mount_seat_local"), &EntityCard::get_mount_seat_local);
	ClassDB::bind_method(D_METHOD("get_mount_seat_yaw_offset"), &EntityCard::get_mount_seat_yaw_offset);
	ClassDB::bind_method(D_METHOD("is_mount_target_config_valid"), &EntityCard::is_mount_target_config_valid);
	ClassDB::bind_method(D_METHOD("get_mount_target_config"), &EntityCard::get_mount_target_config);
	ClassDB::bind_method(D_METHOD("get_mount_target_seats"), &EntityCard::get_mount_target_seats);
	ClassDB::bind_method(D_METHOD("get_primary_weapon_clip"), &EntityCard::get_primary_weapon_clip);
	ClassDB::bind_method(D_METHOD("get_primary_weapon_reserve"), &EntityCard::get_primary_weapon_reserve);
	ClassDB::bind_method(D_METHOD("get_seats"), &EntityCard::get_seats);
	ClassDB::bind_method(D_METHOD("get_item_attrib"), &EntityCard::get_item_attrib);
	ClassDB::bind_method(D_METHOD("get_item_attrib2"), &EntityCard::get_item_attrib2);
	ClassDB::bind_method(D_METHOD("get_item_name"), &EntityCard::get_item_name);
	ClassDB::bind_method(D_METHOD("get_health_max"), &EntityCard::get_health_max);
	ClassDB::bind_method(D_METHOD("to_json_value"), &EntityCard::to_json_value);
}

} // namespace godot
