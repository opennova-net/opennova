// Internal header for the Simulation translation-unit family
// (simulation*.cpp) ONLY — one class, several TUs, split along the
// audit's seams (core / assets / occlusion / player / net / present / bind).
// Carries the family's common includes plus every helper more than one TU
// uses, in namespace sim_internal (each TU opens it with `using`). Not part of
// the engine's public include surface.
#pragma once

#include "simulation/simulation.h"

#include <base/io/perf_clock.h> // the opt-in profiling clock (opennova::io::perf_now_us)
#include <runtime/simassets/mounted_pose.h> // the ONE mounted matrix path (S4b)
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <runtime/wac/compiler.h>
#include <runtime/world/turret_window.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include <net/netsim/connection.h>
#include <net/netsim/entity_wire_bridge.h> // build_player_uplink (joiner-side C2S 0x0C body)

#include <net/npwire/entity_class.h> // class_from_tag (§5.10b *_function -> wire class)
#include <net/npwire/ingame_encode.h> // encode_organic_spawn_batch (+ OrganicSpawnBatch)

#include <net/npruntime/client_replica_present.h> // the client-replica present composition (ADR 0031)
#include <net/npruntime/server_message_dispatch.h> // dispatch_session_replies (local loopback gameplay C2S)
#include <net/npruntime/server_session.h> // set_connection_mode / set_transport_mode / create_session / mark_host_client_in_match
#include <net/npruntime/server_spawn.h>   // Server_ProcessPendingPlayerSpawns (faithful host-player auto-spawn)
#include <net/npruntime/server_tick.h>    // Server_TickUpdate (the single C2S drain + logic tick + 0x0A fan)
#include <net/npruntime/ammo_table_build.h>   // build_ammo_table + round_type resolve (§5.60)
#include <net/npruntime/weapon_table_build.h> // build_weapon_table (weapon.def -> world armory, D-NET-141)
#include <net/npruntime/score_rules_build.h> // build_score_rules (score.ini -> world.score_rules)
#include <net/npwire/game_type.h>              // game_type::for_mission_mode

#include <formats/def/def.h> // def_parse_weapons_memory / def_free_weapons

#include <formats/mission/bms.h>
#include <formats/mission/mission.h>          // kItemIdOffset (wire type id -> items.def id)
#include <runtime/mission/mission_systems.h>
#include <runtime/mission/placement_traits.h> // visual_item_id_for_runtime_type / kPlayerVisualItemId
#include <runtime/anim/aim_overlay.h> // the torso-bend overlay blends [orig: @0x4b1290]
#include <base/io/bam.h>           // bam_add/bam_sar: the FP roll term composition
#include <base/io/strutil.h>       // iequals: the loadout sub-variant ammo-class compare
#include <runtime/world/angle.h>
#include <runtime/simassets/model_builders.h> // the sim-side .3di derivations (ADR 0028)
#include <runtime/simassets/pose_inputs.h>    // seat/mount pose predicates + aim inputs (ADR 0028)
#include <runtime/world/mount_controls.h>     // heat-glow + emplaced turret CTRL sources (ADR 0028)
#include <runtime/world/player_spawn.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/vehicle_attach.h> // player_toggle_vehicle_mount (the USE-ITEM toggle)
#include <runtime/world/vehicle_mount.h>  // resolve_mounted_ammo_slot (phase-8 route)

#include "env/weather_core.h" // kIrisSample* classification codes
#include "object/item_database.h"
#include "object/avatar_database.h"
#include "object/object_data.h" // resolve_collision_instances: the .3di collision IR source
#include "object/skeletal_anim.h"
#include "resource_index/resource_root.h"
#include "terrain/terrain_data.h"

using namespace godot;

using opennova::world::AiBrain;
using opennova::world::AiEntity;
using opennova::world::AiSystem;
using opennova::world::TickContext;
using opennova::world::World;

namespace sim_internal {

inline constexpr double kFixed16 = 65536.0;
// Canonical definition lives in engine/runtime/mission placement_traits.h
// (the presentation visual-item policy home).
inline constexpr int kPlayerVisualItemId = opennova::mission::kPlayerVisualItemId;
// Canonical definition lives in engine/runtime/world/player_spawn.h (shared with the npruntime host).
inline constexpr uint16_t kRetailPlayerMinEntitySlot = opennova::world::kRetailPlayerMinEntitySlot;

inline uint64_t present_effect_origin_key(int kind, int index) {
	return (static_cast<uint64_t>(static_cast<uint32_t>(kind)) << 32) |
	       static_cast<uint32_t>(index);
}

inline int32_t trace_profile_lane(int64_t value) {
	return static_cast<int32_t>(std::clamp<int64_t>(
			value, 0, std::numeric_limits<int32_t>::max()));
}

// The seat/mount pose predicates moved to the engine (ADR 0028):
// engine/runtime/simassets pose_inputs.h. The using declarations keep this
// header's call sites unchanged.
using opennova::simassets::mount_blocks_weapon_channel;
using opennova::simassets::mount_collapses_right_hand_row;

// The policy lives in engine/runtime/mission placement_traits.h; this wrapper
// only answers the "does the catalog carry the player visual?" probe from the
// shell's item database.
inline int visual_item_id_for_runtime_type(int item_id, const Ref<ItemDatabase> &item_db) {
	return opennova::mission::visual_item_id_for_runtime_type(item_id,
			item_db.is_valid() && item_db->has_item(kPlayerVisualItemId));
}
// The mission-side policy keys on the SAME runtime player type the world
// names; a drift would silently break the player visual resolve.
static_assert(opennova::mission::kPlayerRuntimeTypeId ==
		opennova::world::kPlayerInfantryTypeId);

using opennova::simassets::aim_overlay_inputs_for;

// The decoded-row lookup + the mounted-shooter carrier-exclusion rule moved to
// the engine with the joiner bridge (S10a, ADR 0028):
// engine/net/npruntime joiner_world_bridge.h. The using declarations keep this
// family's call sites unchanged.
using opennova::np::client_entity_for_handle;
using opennova::np::wire_carrier_exclusion_for;
// The installed-table probe lives beside the extraction now (ADR 0031).
using opennova::simassets::item_seat_spec_for_type;

inline constexpr char kEmplacedGunYawRegister[] = "EWEAP_GUNYAW";
inline constexpr char kEmplacedGunPitchRegister[] = "EWEAP_GUNPITCH";
inline constexpr char kVehicleSpecial1Register[] = "VEHICLE_SPECIAL1";
inline constexpr char kVehicleSpecial2Register[] = "VEHICLE_SPECIAL2";
inline constexpr char kHeatGlowRegister[] = "HEAT_GLOW";

// The HEAT_GLOW derivation and the emplaced turret CTRL sources moved to the
// engine (ADR 0028): engine/runtime/world mount_controls.h. The using
// declarations keep this header's call sites unchanged.
using opennova::world::world_model_heat_glow_for;

inline void write_present_world_model_heat_glow(float *record,
		const opennova::world::World &world,
		const opennova::world::Entity &entity) {
	int32_t heat_glow = 0;
	if (!world_model_heat_glow_for(world, entity, heat_glow)) return;
	record[Simulation::PF_WORLD_HEAT_GLOW_VALID] = 1.0f;
	record[Simulation::PF_WORLD_HEAT_GLOW] = static_cast<float>(heat_glow);
}

inline void write_present_vehicle_motion_controls(float *record,
		const opennova::world::World &world,
		const opennova::world::Entity &entity) {
	// This is the modeled ground-vehicle/cveh scope, not a heuristic over every
	// moving item. Only the authority owns the full steer and currentSpeed
	// fields; ClientEntityState carries neither and must leave VALID clear.
	if (entity.handle.pool() != 1 ||
			world.vehicle_traits.get(entity.item_id) == nullptr)
		return;
	const opennova::world::VehicleCtrlRegisters controls =
			opennova::world::vehicle_ctrl_registers(entity.veh);
	record[Simulation::PF_VEHICLE_MOTION_VALID] = 1.0f;
	record[Simulation::PF_VEHICLE_STEERING] =
			static_cast<float>(controls.steering);
	record[Simulation::PF_VEHICLE_SPEED] =
			static_cast<float>(controls.speed);
	// The part-animation words ride the same valid bit: the rotor angle
	// accumulator's high word (HELO_ROTOR / HELO_TAILROTOR) and the wheel
	// phase's (VEHICLE_WHEELS), published by the same cveh callback
	// [orig: Entity_CacheVehicleHUDStats @0x4929B0 — the +0x466 read
	// @0x492ACA..0x492ADE, the +0x2BA read @0x4929B4; the accumulators are
	// world/vehicle_part_anim.h's].
	record[Simulation::PF_VEHICLE_ROTOR] =
			static_cast<float>(controls.rotor);
	record[Simulation::PF_VEHICLE_TAIL_ROTOR] =
			static_cast<float>(controls.tail_rotor);
	record[Simulation::PF_VEHICLE_WHEELS] =
			static_cast<float>(controls.wheels);
}

using opennova::world::EmplacedWeaponControls;
using opennova::world::emplaced_weapon_controls_for;

// The client-replica present composition helpers moved to the engine
// (ADR 0031): engine/net/npruntime client_replica_present.h. The using
// declarations keep this family's call sites unchanged.
using opennova::np::emplaced_weapon_controls_for_client;
using opennova::np::write_present_emplaced_controls;

inline double bam_to_radians(int32_t value) {
	return static_cast<double>(value) *
			(6.28318530717958647692 / 4294967296.0);
}

inline Basis godot_model_basis_from_overlay(
		const opennova::anim::AimOverlayAngles &angles) {
	// C++ twin of MissionObjectPlacer.bms_to_godot_basis. The first yaw
	// simplifies to the BAM heading itself; the final +90 degree term is the
	// .3di model-forward correction.
	return Basis(Vector3(0.0, 1.0, 0.0), bam_to_radians(angles.yaw)) *
			Basis(Vector3(0.0, 0.0, 1.0), bam_to_radians(angles.pitch)) *
			Basis(Vector3(1.0, 0.0, 0.0), bam_to_radians(angles.roll)) *
			Basis(Vector3(0.0, 1.0, 0.0), 1.57079632679489661923);
}

// Godot-type packer over the engine euler composition (the math lives in
// npruntime client_replica_present.h).
inline Vector3 mission_euler_from_overlay(
		const opennova::anim::AimOverlayAngles &angles) {
	const opennova::np::MissionEulerDeg e =
			opennova::np::mission_euler_from_overlay(angles);
	return Vector3(e.pitch, e.yaw, e.roll);
}

using opennova::np::write_present_held_weapon;
using opennova::np::write_present_overlay;

inline std::string dictionary_string(const Dictionary &d, const char *key, const std::string &fallback) {
	if (!d.has(key)) return fallback;
	const String value = d.get(key, String());
	return std::string(value.utf8().get_data());
}

inline void apply_dictionary_string(const Dictionary &d, const char *key, std::string &out) {
	if (d.has(key)) {
		out = dictionary_string(d, key, out);
	}
}

inline uint16_t dictionary_u16(const Dictionary &d, const char *key, uint16_t fallback) {
	if (!d.has(key)) return fallback;
	const int value = static_cast<int>(d.get(key, static_cast<int>(fallback)));
	return static_cast<uint16_t>(std::clamp(value, 0, 0xFFFF));
}

inline uint32_t dictionary_u32(const Dictionary &d, const char *key, uint32_t fallback) {
	if (!d.has(key)) return fallback;
	const int64_t value = static_cast<int64_t>(d.get(key, static_cast<int64_t>(fallback)));
	if (value < 0) return 0;
	if (value > 0xFFFFFFFFll) return 0xFFFFFFFFu;
	return static_cast<uint32_t>(value);
}

inline int32_t dictionary_i32(const Dictionary &d, const char *key, int32_t fallback) {
	if (!d.has(key)) return fallback;
	const int64_t value = static_cast<int64_t>(d.get(key, static_cast<int64_t>(fallback)));
	return static_cast<int32_t>(std::clamp(
			value,
			static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
			static_cast<int64_t>(std::numeric_limits<int32_t>::max())));
}

// The collision/occlusion/bound-radius builders and the model predicates
// moved to the engine (ADR 0028): engine/runtime/simassets. The using
// declarations keep this header's call sites unchanged.
using opennova::simassets::model_bound_radius_from_3di;


// S4b (ADR 0028): the joiner's addeweap reconstruction rides the SAME engine
// resolver the host authority runs (simassets::resolve_model_mounted_pose) —
// one mounted matrix path; the adapter's Dictionary evaluate_panm twin is
// gone. The model resolves through the sim cache by the installed spec's
// graphic key, exactly like the host-side resolver.
inline bool resolve_client_eweap_attachment_pose(
		const opennova::netsim::ClientEntityState &child,
		const opennova::netsim::ClientState &state,
		const std::vector<opennova::mission::ItemSeatSpec> &specs,
		const std::unordered_map<int32_t, std::string> &graphics_by_type,
		opennova::simassets::SimModelCache &models,
		uint32_t time_ms, opennova::world::MountedPose &out) {
	if (child.parent_handle == opennova::world::EntityHandle::kInvalid) return false;
	const opennova::netsim::ClientEntityState *parent =
			client_entity_for_handle(state, child.parent_handle);
	if (parent == nullptr) return false;
	const opennova::mission::ItemSeatSpec *parent_spec =
			item_seat_spec_for_type(specs, parent->type_id);
	if (parent_spec == nullptr) return false;

	// The 0x0D relation names only the parent, not the authored attachment slot.
	// Reconstruct only when the child type selects exactly one authored row and
	// that row resolves a userpoint; duplicate same-type rows are intentionally
	// left on the rigid fallback.
	const opennova::mission::ItemEmplacementAttachmentSpec *attachment = nullptr;
	for (const opennova::mission::ItemEmplacementAttachmentSpec &candidate :
			parent_spec->emplacement_attachments) {
		if (candidate.child_type_id != static_cast<int32_t>(child.type_id))
			continue;
		if (attachment != nullptr) return false;
		attachment = &candidate;
	}
	if (attachment == nullptr || !attachment->anchor_found ||
			attachment->anchor.bone_index == 0)
		return false;
	const auto graphic_found = graphics_by_type.find(parent->type_id);
	if (graphic_found == graphics_by_type.end() || !models.has_index())
		return false;
	const Threedi3di3 *model_ptr = models.model_for(graphic_found->second);
	if (model_ptr == nullptr) return false;

	// Remote generic PLAYPARTANIM phases are not in ClientEntityState. Do not
	// synthesize them from timing or repurpose a wire field. EWEAP is the one safe
	// articulated family: the decoded mounted gunner already determines both
	// semantic controls through the witnessed parent-minus-occupant relationship.
	EmplacedWeaponControls emplaced;
	if (!emplaced_weapon_controls_for_client(
				*parent, state, specs, emplaced))
		return false;
	const Threedi3di3 &model = *model_ptr;
	if (model.ctrl.count > 0 && model.ctrl.registers == nullptr)
		return false;
	bool has_eweap_control = false;
	for (uint32_t slot = 0; slot < model.ctrl.count; ++slot) {
		const String name = String::utf8(model.ctrl.registers[slot].name);
		if (name.nocasecmp_to(kEmplacedGunYawRegister) == 0 ||
				name.nocasecmp_to(kEmplacedGunPitchRegister) == 0) {
			has_eweap_control = true;
			break;
		}
	}
	if (!has_eweap_control) return false;
	int32_t ctrl_values[THREEDI_CTRL_REGISTER_COUNT] = {};
	ctrl_values[THREEDI_CTRL_EWEAP_GUNYAW] =
			static_cast<int32_t>(emplaced.gun_yaw);
	ctrl_values[THREEDI_CTRL_EWEAP_GUNPITCH] =
			static_cast<int32_t>(emplaced.gun_pitch);

	opennova::world::Entity carrier;
	carrier.item_id = static_cast<int32_t>(parent->type_id);
	carrier.position = {
			static_cast<float>(parent->x / kFixed16),
			static_cast<float>(parent->y / kFixed16),
			static_cast<float>(parent->z / kFixed16)};
	carrier.yaw = static_cast<int16_t>(std::lround(
			opennova::world::mission_yaw_deg_from_bam_heading(
					parent->heading_bam)));
	carrier.pitch = static_cast<int16_t>(std::lround(
			static_cast<double>(parent->pitch_bam) *
				opennova::world::kDegreesPerBam));
	carrier.roll = static_cast<int16_t>(std::lround(
			static_cast<double>(parent->roll_bam) *
				opennova::world::kDegreesPerBam));
	return opennova::simassets::resolve_model_mounted_pose(
			model, carrier, attachment->anchor, ctrl_values, time_ms, out);
}

// Coordinate converters shared by the debug views and present getters.
// Mission-space 16.16 triple -> Godot world space: (x, y, z) -> (x, z, -y) units.
inline Vector3 godot_from_fixed3(const int32_t p[3]) {
	return Vector3(static_cast<float>(p[0] / 65536.0), static_cast<float>(p[2] / 65536.0),
	               static_cast<float>(-p[1] / 65536.0));
}
// Mission float Vec3 -> Godot world space: (x, y, z) -> (x, z, -y).
// The ONE mission <-> presentation map (world/presentation_frame.h).
inline Vector3 godot_from_mission_vec3(const opennova::world::Vec3 &p) {
	return Vector3(p.x, p.z, -p.y);
}
// Render float world -> Godot world: the render frame is Godot with X/Z
// swapped ((-my, mz, mx)/65536 == (gz, gy, gx)), so the inverse is the same swap.
inline Vector3 godot_from_render_float3(const float p[3]) {
	return Vector3(p[2], p[1], p[0]);
}

} // namespace sim_internal
