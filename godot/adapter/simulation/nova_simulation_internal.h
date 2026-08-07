// Internal header for the NovaSimulation translation-unit family
// (nova_simulation*.cpp) ONLY — one class, several TUs, split along the
// audit's seams (core / assets / occlusion / player / net / present / bind).
// Carries the family's common includes plus every helper more than one TU
// uses, in namespace novasim (each TU opens it with `using`). Not part of
// the engine's public include surface.
#pragma once

#include "simulation/nova_simulation.h"

#include <wac/compiler.h>
#include <world/turret_window.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "netsim/connection.h"
#include "netsim/entity_wire_bridge.h" // build_player_uplink (joiner-side C2S 0x0C body)

#include <npwire/entity_class.h> // class_from_tag (§5.10b *_function -> wire class)
#include <npwire/ingame_encode.h> // encode_organic_spawn_batch (+ OrganicSpawnBatch)

#include <npruntime/server_message_dispatch.h> // dispatch_session_replies (local loopback gameplay C2S)
#include <npruntime/server_session.h> // set_connection_mode / set_transport_mode / create_session / mark_host_client_in_match
#include <npruntime/server_spawn.h>   // Server_ProcessPendingPlayerSpawns (faithful host-player auto-spawn)
#include <npruntime/server_tick.h>    // Server_TickUpdate (the single C2S drain + logic tick + 0x0A fan)
#include <npruntime/ammo_table_build.h>   // build_ammo_table + round_type resolve (§5.60)
#include <npruntime/weapon_table_build.h> // build_weapon_table (weapon.def -> world armory, D-NET-141)

#include <def/def.h> // def_parse_weapons_memory / def_free_weapons

#include <mission/bms.h>
#include <mission/mission.h>          // kItemIdOffset (wire type id -> items.def id)
#include <mission/mission_systems.h>
#include <anim/aim_overlay.h> // the torso-bend overlay blends [orig: @0x4b1290]
#include <io/bam.h>           // bam_add/bam_sar: the FP roll term composition
#include <io/strutil.h>       // iequals: the loadout sub-variant ammo-class compare
#include <world/angle.h>
#include <simassets/model_builders.h> // the sim-side .3di derivations (ADR 0028)
#include <simassets/pose_inputs.h>    // seat/mount pose predicates + aim inputs (ADR 0028)
#include <world/mount_controls.h>     // heat-glow + emplaced turret CTRL sources (ADR 0028)
#include <world/player_spawn.h>
#include <world/spawn_select.h>
#include <world/vehicle_attach.h> // player_toggle_vehicle_mount (the USE-ITEM toggle)
#include <world/vehicle_mount.h>  // resolve_mounted_ammo_slot (phase-8 route)

#include "env/nova_weather_core.h" // kIrisSample* classification codes
#include "object/nova_item_database.h"
#include "object/nova_object_data.h" // resolve_collision_instances: the .3di collision IR source
#include "object/nova_skeletal_anim.h"
#include "resource_index/nova_resource_root.h"
#include "terrain/nova_terrain_data.h"

using namespace godot;

using opennova::world::AiBrain;
using opennova::world::AiEntity;
using opennova::world::AiSystem;
using opennova::world::TickContext;
using opennova::world::World;

namespace novasim {

inline constexpr double kFixed16 = 65536.0;
inline constexpr int kPlayerVisualItemId = 105310; // items.def "Player #1, Single player" -> US01/US01.adm
// Canonical definition lives in engine/runtime/world/player_spawn.h (shared with the npruntime host).
inline constexpr uint16_t kRetailPlayerMinEntitySlot = opennova::world::kRetailPlayerMinEntitySlot;
inline constexpr float kBinocularAimOffsetDeg = 2.8125f; // 0x02000000 BAM
inline constexpr double kTau = 6.28318530717958647692;

inline uint64_t present_effect_origin_key(int kind, int index) {
	return (static_cast<uint64_t>(static_cast<uint32_t>(kind)) << 32) |
	       static_cast<uint32_t>(index);
}

inline uint64_t perf_now_us() {
	using Clock = std::chrono::steady_clock;
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

inline int32_t trace_profile_lane(int64_t value) {
	return static_cast<int32_t>(std::clamp<int64_t>(
			value, 0, std::numeric_limits<int32_t>::max()));
}

inline opennova::world::SeatType seat_type_from_variant(int value) {
	switch (value) {
		case static_cast<int>(opennova::world::SeatType::Passenger): return opennova::world::SeatType::Passenger;
		case static_cast<int>(opennova::world::SeatType::Controller): return opennova::world::SeatType::Controller;
		case static_cast<int>(opennova::world::SeatType::Gunner): return opennova::world::SeatType::Gunner;
		case static_cast<int>(opennova::world::SeatType::Driver): return opennova::world::SeatType::Driver;
		default: return opennova::world::SeatType::None;
	}
}

// The seat/mount pose predicates moved to the engine (ADR 0028):
// engine/runtime/simassets pose_inputs.h. The using declarations keep this
// header's call sites unchanged.
using opennova::simassets::mount_blocks_weapon_channel;
using opennova::simassets::mount_collapses_right_hand_row;
using opennova::simassets::seat_type_blocks_weapon_channel;

inline int visual_item_id_for_runtime_type(int item_id, const Ref<NovaItemDatabase> &item_db) {
	if (item_id == opennova::world::kPlayerInfantryTypeId && item_db.is_valid() &&
	    item_db->has_item(kPlayerVisualItemId)) {
		return kPlayerVisualItemId;
	}
	return item_id + opennova::mission::kItemIdOffset;
}

using opennova::simassets::aim_overlay_inputs_for;
using opennova::simassets::mount_mode_for;
using opennova::simassets::mount_mode_for_seat_type;

inline const opennova::netsim::ClientEntityState *client_entity_for_handle(
		const opennova::netsim::ClientState &state, uint16_t handle) {
	for (const opennova::netsim::ClientEntityState &entity : state.entities) {
		if (entity.handle == handle) return &entity;
	}
	return nullptr;
}

inline const opennova::mission::ItemSeatSpec *item_seat_spec_for_type(
		const std::vector<opennova::mission::ItemSeatSpec> &specs,
		uint16_t type_id) {
	// specs are sorted by type_id at install (resolve_item_traits); a joiner
	// probes this per present row per frame, so the scan is a binary search.
	const auto it = std::lower_bound(
			specs.begin(), specs.end(), static_cast<int32_t>(type_id),
			[](const opennova::mission::ItemSeatSpec &spec, int32_t t) {
				return spec.type_id < t;
			});
	if (it != specs.end() && it->type_id == static_cast<int32_t>(type_id))
		return &*it;
	return nullptr;
}

// The mounted shooter's own vehicle joins the projectile trace exclusion exactly like
// retail's mount rule (Controller/Gunner/Driver seats only — passengers keep clipping
// their ride) — resolved from the wire shooter row's carrier + the binding-fed seat table
// instead of live mount pointers. Returns 0xFFFF when unmounted, passenger-seated, or
// the rows aren't streamed yet. Used for BOTH decoded remote rounds and the joiner's
// own predicted rounds: on a joiner the local ignored-mount leg is dead (wire_projected
// skips the local dynamics table), so this carrier gate is the only surviving exclusion.
// [orig: the ignored-mount select feeding Physics_RaycastAgainstBoneCollision @ 0x4e4cb0
//  via ray[18]]
inline uint16_t wire_carrier_exclusion_for(
		const opennova::netsim::ClientState &state, uint16_t shooter_handle,
		const std::vector<opennova::mission::ItemSeatSpec> &seat_specs) {
	const opennova::netsim::ClientEntityState *row =
			client_entity_for_handle(state, shooter_handle);
	if (row == nullptr || row->carrier_handle == opennova::world::EntityHandle::kInvalid ||
			row->mount_bone == 0)
		return 0xFFFFu;
	const opennova::netsim::ClientEntityState *carrier =
			client_entity_for_handle(state, row->carrier_handle);
	const opennova::mission::ItemSeatSpec *spec = carrier != nullptr
			? item_seat_spec_for_type(seat_specs, carrier->type_id)
			: nullptr;
	if (spec == nullptr) return 0xFFFFu;
	for (const opennova::world::Seat &seat : spec->seats) {
		if (seat.bone_index != row->mount_bone) continue;
		if (seat.type == opennova::world::SeatType::Controller ||
				seat.type == opennova::world::SeatType::Gunner ||
				seat.type == opennova::world::SeatType::Driver)
			return row->carrier_handle;
		break;
	}
	return 0xFFFFu;
}

inline constexpr char kEmplacedGunYawRegister[] = "EWEAP_GUNYAW";
inline constexpr char kEmplacedGunPitchRegister[] = "EWEAP_GUNPITCH";
inline constexpr char kVehicleSpecial1Register[] = "VEHICLE_SPECIAL1";
inline constexpr char kVehicleSpecial2Register[] = "VEHICLE_SPECIAL2";
inline constexpr char kHeatGlowRegister[] = "HEAT_GLOW";

// The HEAT_GLOW derivation and the emplaced turret CTRL sources moved to the
// engine (ADR 0028): engine/runtime/world mount_controls.h. The using
// declarations keep this header's call sites unchanged.
using opennova::world::world_model_heat_glow_for;

inline void assign_world_model_heat_glow(Dictionary &r_controls,
		const opennova::world::World &world,
		const opennova::world::Entity &entity) {
	int32_t heat_glow = 0;
	if (world_model_heat_glow_for(world, entity, heat_glow))
		r_controls[String(kHeatGlowRegister)] = heat_glow;
}

inline void write_present_world_model_heat_glow(float *record,
		const opennova::world::World &world,
		const opennova::world::Entity &entity) {
	int32_t heat_glow = 0;
	if (!world_model_heat_glow_for(world, entity, heat_glow)) return;
	record[NovaSimulation::PF_WORLD_HEAT_GLOW_VALID] = 1.0f;
	record[NovaSimulation::PF_WORLD_HEAT_GLOW] = static_cast<float>(heat_glow);
}

// Publish the two PLAYPARTANIM accumulators onto retail's semantic CTRL bus.
// The action writes direction/rate at comp[109..112] and the integrator advances
// comp[113]/comp[114]. HUD_CacheEntityDisplayInfo then publishes those exact
// phase fields as VEHICLE_SPECIAL1/2; it never walks the model's CTRL list.
// SPECIAL1 alone is suppressed by ItemDefAttrib 0x1000 (FastRope), while
// SPECIAL2 is unconditional.
// [orig: Entity_ApplyCommand case 0x22 @ 0x43B192; integrator @ 0x456710;
//  HUD_CacheEntityDisplayInfo @ 0x4A3E18..0x4A3E38; global CTRL ordinals
//  VEHICLE_SPECIAL1=71 / VEHICLE_SPECIAL2=72]
template <typename PhaseFn>
inline void assign_part_anim_phases(Dictionary &r_controls,
		bool p_publish_special1, PhaseFn p_phase_for) {
	if (p_publish_special1) {
		r_controls[String(kVehicleSpecial1Register)] = p_phase_for(0);
	}
	r_controls[String(kVehicleSpecial2Register)] = p_phase_for(1);
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
	record[NovaSimulation::PF_VEHICLE_MOTION_VALID] = 1.0f;
	record[NovaSimulation::PF_VEHICLE_STEERING] =
			static_cast<float>(controls.steering);
	record[NovaSimulation::PF_VEHICLE_SPEED] =
			static_cast<float>(controls.speed);
}

using opennova::world::EmplacedWeaponControls;
using opennova::world::emplaced_clamp_turret_bam;
using opennova::world::emplaced_control_phase;
using opennova::world::emplaced_weapon_controls_for;
using opennova::world::turret_limit_bam;

inline bool emplaced_weapon_controls_for_client(
		const opennova::netsim::ClientEntityState &mount,
		const opennova::netsim::ClientState &state,
		const std::vector<opennova::mission::ItemSeatSpec> &specs,
		EmplacedWeaponControls &out) {
	out = EmplacedWeaponControls{};
	const opennova::mission::ItemSeatSpec *spec =
			item_seat_spec_for_type(specs, mount.type_id);
	if (spec == nullptr) return false;

	for (const opennova::netsim::ClientEntityState &occupant : state.entities) {
		if (occupant.carrier_handle != mount.handle ||
				occupant.mount_bone == 0 ||
				(occupant.cls != opennova::EntityClass::Player &&
				 occupant.cls != opennova::EntityClass::Infantry))
			continue;
		const opennova::world::Seat *seat = nullptr;
		for (const opennova::world::Seat &candidate : spec->seats) {
			if (candidate.bone_index == occupant.mount_bone) {
				seat = &candidate;
				break;
			}
		}
		if (seat == nullptr ||
				seat->type != opennova::world::SeatType::Gunner)
			continue;

		// ClientReplicaPipeline has already composed mounted yaw into world heading and
		// reconstructed an infantry gunner's live entity pitch from the compact
		// aim target using retail's one-eighth chase. Root headings read the
		// row's live BAM (chased on a joiner — §5.38e; full euler precision for
		// vehicles).
		const int32_t parent_heading = mount.heading_bam;
		const int32_t occupant_heading = occupant.heading_bam;
		const int32_t occupant_pitch =
				occupant.cls == opennova::EntityClass::Player
				? static_cast<int32_t>(
						static_cast<uint32_t>(occupant.pitch_byte) << 24)
				: occupant.pitch_bam;
		int32_t yaw_delta =
				opennova::io::bam_sub(parent_heading, occupant_heading);
		int32_t pitch_delta =
				opennova::io::bam_sub(mount.pitch_bam, occupant_pitch);
		// The per-seat authored window wins first — the CARRIER's addeweap arc
		// for this attach point, matched by the mount row's streamed carrier +
		// bone (the same key the seat match above uses). Any nonzero value
		// selects the quartet verbatim; all-zero falls to the weapon-def
		// window below. [orig: Entity_GetWeaponTurretLimits @0x540d70 per-seat
		// leg @0x540db5..0x540e27 — carrierDef+540/556/572/588[seat]]
		const opennova::mission::ItemEmplacementAttachmentSpec *arc = nullptr;
		if (mount.carrier_handle != opennova::world::EntityHandle::kInvalid) {
			for (const opennova::netsim::ClientEntityState &carrier :
					state.entities) {
				if (carrier.handle != mount.carrier_handle) continue;
				if (const opennova::mission::ItemSeatSpec *carrier_spec =
						item_seat_spec_for_type(specs, carrier.type_id)) {
					for (const opennova::mission::ItemEmplacementAttachmentSpec
							&candidate :
							carrier_spec->emplacement_attachments) {
						if (candidate.anchor_found &&
								candidate.anchor.bone_index ==
										mount.mount_bone &&
								(candidate.down_limit_bam |
								 candidate.up_limit_bam |
								 candidate.right_limit_bam |
								 candidate.left_limit_bam) != 0) {
							arc = &candidate;
							break;
						}
					}
				}
				break;
			}
		}
		// A retail joiner never presents a barrel outside the arc even when
		// the mount's streamed base heading is stale relative to its gunner:
		// the phase pins at the arc edge (live 00TRg witness 2026-08-04 — gun
		// 0x100c streamed its spawn heading 140 deg while its gunner aimed
		// 314 deg; unclamped, the "180 tripod" model wrapped the barrel
		// visibly wrong).
		// [orig: the per-update clamp @0x441228..0x44128c via sub_540CC0]
		const opennova::world::TurretWindow window =
				opennova::world::select_turret_window_bam(
						arc != nullptr ? arc->down_limit_bam : 0,
						arc != nullptr ? arc->up_limit_bam : 0,
						arc != nullptr ? arc->right_limit_bam : 0,
						arc != nullptr ? arc->left_limit_bam : 0,
						spec->turret_yaw_range_bam,
						spec->turret_pitch_max_bam,
						spec->turret_pitch_min_bam);
		if (window.per_seat) {
			emplaced_clamp_turret_bam(yaw_delta, window.yaw_upper,
					window.yaw_lower);
			emplaced_clamp_turret_bam(pitch_delta, window.pitch_upper,
					window.pitch_lower);
		} else {
			// weapon.def fallback (stamped onto the spec from the primary
			// weapon's rows).
			if (window.yaw_upper != 0)
				emplaced_clamp_turret_bam(yaw_delta, window.yaw_upper,
						window.yaw_lower);
			if (window.pitch_upper != 0 || window.pitch_lower != 0)
				emplaced_clamp_turret_bam(pitch_delta, window.pitch_upper,
						window.pitch_lower);
		}
		out.valid = true;
		out.gun_yaw = static_cast<uint16_t>(
				static_cast<uint32_t>(yaw_delta) >> 16);
		out.gun_pitch = static_cast<uint16_t>(
				static_cast<uint32_t>(pitch_delta) >> 16);
		return true;
	}
	return false;
}

inline void write_present_emplaced_controls(
		float *record, const EmplacedWeaponControls &controls) {
	if (!controls.valid) return;
	record[NovaSimulation::PF_EMPLACED_CONTROLS_VALID] = 1.0f;
	record[NovaSimulation::PF_EWEAP_GUNYAW] =
			static_cast<float>(controls.gun_yaw);
	record[NovaSimulation::PF_EWEAP_GUNPITCH] =
			static_cast<float>(controls.gun_pitch);
}

inline bool aim_overlay_inputs_for_client(
		const opennova::netsim::ClientEntityState &entity,
		const opennova::netsim::ClientState &state,
		const std::vector<opennova::mission::ItemSeatSpec> &specs,
		opennova::anim::AimOverlayInputs &out,
		bool *r_collapse_right_hand = nullptr) {
	if (r_collapse_right_hand != nullptr) *r_collapse_right_hand = false;
	if (entity.cls != opennova::EntityClass::Player &&
			entity.cls != opennova::EntityClass::Infantry)
		return false;

	out = opennova::anim::AimOverlayInputs{};
	out.aim_yaw = entity.heading_bam;
	// Free-standing players read the row's live pitch (the org2 body chase
	// steps it toward the wire byte with divisor 12 on a joiner; snap folds
	// mirror the byte — §5.38e §3). Mounted players and infantry keep the raw
	// wire aim byte: their own mover is bit0-skipped and the seat pose owns the
	// body [orig: the mounted read of entity+0x2D0-desired aim].
	const bool free_standing_player =
			entity.cls == opennova::EntityClass::Player &&
			entity.carrier_handle == opennova::world::EntityHandle::kInvalid;
	out.aim_pitch = free_standing_player
			? entity.pitch_bam
			: static_cast<int32_t>(
					static_cast<uint32_t>(
							entity.cls == opennova::EntityClass::Player
									? entity.pitch_byte
									: entity.aim_yaw_byte)
					<< 24);
	out.body_yaw = out.aim_yaw;
	out.leg_yaw_r = out.body_yaw;
	out.leg_yaw_l = out.body_yaw;
	out.aim_state =
			(opennova::world::infantry_anim_flags(entity.anim_state_id) &
					0x40u) != 0;
	out.rolling =
			entity.anim_state_id == opennova::world::anim_state::kRollLeft ||
			entity.anim_state_id == opennova::world::anim_state::kRollRight;
	// The remote lean render is the aim-overlay lean term (Roll = torsoRoll + lean/2),
	// fed by the wire-bit integrator in ClientEntityState — the same source the host
	// path reads from its own entities. [orig: the §14 overlay lean consumer; the
	// integrator @0x4b7dbf/@0x4b7dd6/@0x4b5c97]
	out.lean = entity.lean_angle;
	// The remote arms dip. Retail feeds the same accumulator into the overlay for
	// every body it draws; without this a peer's reload is invisible to an observer,
	// which is the ONLY feedback a pure client gets [orig: consumer @0x4b1bd4/@0x4b1c1b,
	//  producer @0x4b5cab..0x4b5ce7].
	out.pitch_kick_accum = entity.pitch_kick_accum;
	// Recoil is not a wire field: the decoded client stamps it from the same
	// received round event and decays it in its local body pass. Presentation
	// consumes that reconstructed entity+0x380 exactly like an authority body.
	// [orig: overlay consumer @0x4b1bce; round impulse @0x4ec378/@0x4ec8a3]
	out.pitch_blend = entity.recoil_pitch;

	// Both witnessed compact organic records already carry the carrier and raw
	// seat bone. Bone zero is the standing-on/deck form, not a mount. Resolve
	// the carrier's wire type into the binding-fed production seat table; never
	// synthesize a config byte or alias a missing definition to config zero.
	if (entity.carrier_handle == opennova::world::EntityHandle::kInvalid ||
			entity.mount_bone == 0) return true;
	const opennova::netsim::ClientEntityState *carrier =
			client_entity_for_handle(state, entity.carrier_handle);
	if (carrier == nullptr) return true;
	// A carrier row with no resolved seat table (or none for this bone) is OUR
	// resolution-timing state, with no retail counterpart: retail composes every
	// mounted body from the always-loaded itemDef+model, while our model-derived
	// spec table can lag the stream (the admission prewarm is one-shot; a type
	// first streamed later never resolves until the next rebuild). Retail never
	// draws a mounted body free-standing, so degrade TOWARD the seat: hold the
	// carrier's live root frame (heading/pitch/roll, offset unknown => zero)
	// instead of letting body_yaw ride the aim — an aim-riding body orbits the
	// mount with the gunner's scan, which is the reported joiner spin.
	const opennova::mission::ItemSeatSpec *spec =
			item_seat_spec_for_type(specs, carrier->type_id);
	const opennova::world::Seat *seat = nullptr;
	if (spec != nullptr) {
		for (const opennova::world::Seat &candidate : spec->seats) {
			if (candidate.bone_index == entity.mount_bone) {
				seat = &candidate;
				break;
			}
		}
	}
	if (seat == nullptr) {
		out.body_yaw = carrier->heading_bam;
		out.body_pitch = carrier->pitch_bam;
		out.roll = carrier->roll_bam;
		out.leg_yaw_r = out.body_yaw;
		out.leg_yaw_l = out.body_yaw;
		return true;
	}
	if (r_collapse_right_hand != nullptr) {
		// The compact organic class is the decoded form of the relevant Flags
		// distinction: Player rows carry 0x100; Infantry rows do not. Derive the
		// presentation verdict from existing wire fields rather than adding a
		// transport-only boolean.
		*r_collapse_right_hand =
				entity.cls == opennova::EntityClass::Infantry &&
				seat_type_blocks_weapon_channel(seat->type);
	}

	out.mount_mode = mount_mode_for_seat_type(seat->type);
	if (out.mount_mode == opennova::anim::MountMode::OnFoot) return true;
	out.mount_config_valid = spec->mount_config_valid;
	out.mount_config = spec->mount_config_valid ? spec->mount_config : 0;

	// pose_mounted_occupant faces seated slots at carrier+yaw_offset and a
	// Gunner at carrier-yaw_offset in mission yaw. Engine heading is
	// (90-mission yaw), so those signs invert here. The carrier root heading is
	// the row's live BAM (chased on a joiner — §5.38e).
	const int32_t carrier_heading = carrier->heading_bam;
	const int32_t offset = opennova::world::bam_from_degrees_wrapped(
			static_cast<double>(seat->yaw_offset));
	out.body_yaw = out.mount_mode == opennova::anim::MountMode::Gunner
			? opennova::io::bam_add(carrier_heading, offset)
			: opennova::io::bam_sub(carrier_heading, offset);
	out.body_pitch = carrier->pitch_bam;
	out.roll = carrier->roll_bam;
	out.leg_yaw_r = out.body_yaw;
	out.leg_yaw_l = out.body_yaw;
	return true;
}

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

inline Vector3 mission_euler_from_overlay(
		const opennova::anim::AimOverlayAngles &angles) {
	return Vector3(
			static_cast<float>(static_cast<double>(angles.pitch) *
					opennova::world::kDegreesPerBam),
			static_cast<float>(
					opennova::world::mission_yaw_deg_from_bam_heading(
							angles.yaw)),
			static_cast<float>(static_cast<double>(angles.roll) *
					opennova::world::kDegreesPerBam));
}

// The third-person held weapon for ONE presented row: which ADM model it is holding and
// the weapon's own attach orientation. Retail applies one predicate to the model's
// visibility — it is drawn iff the soldier may FIRE it — and for a NON-local body that
// predicate reduces to "alive, and not in a control/gunner/driver seat": the remote branch
// never consults an EquippedSlot, so a peer whose slot we do not model still passes.
// MountMode::OnFoot is exactly the complement of retail's {2,3,5} hide set (a PASSENGER
// keeps its weapon and maps to OnFoot here), so the seat half needs no extra state.
// Reports adm 0 when hidden, which is both our table's null row and the original's own
// `if (entity->equippedAdmIndex)` precondition.
// [orig: Entity_CanFireWeapon @ 0x4dcb10 remote branch @0x4dcb3c..0x4dcb57;
//  draw precondition @ 0x4e3c97; attach basis @ 0x4b1bdc..0x4b1bf8]
inline void write_present_held_weapon(
		float *r, uint8_t p_equipped_adm_index, bool p_dead,
		const opennova::anim::AimOverlayInputs &p_in,
		int p_weapon_hold_state) {
	if (p_dead || p_in.mount_mode != opennova::anim::MountMode::OnFoot) return;
	if (p_equipped_adm_index == 0 ||
			p_equipped_adm_index == opennova::world::kAdmSlotNone) return;
	r[NovaSimulation::PF_HELD_WEAPON_ADM] = static_cast<float>(p_equipped_adm_index);
	const Vector3 attach = mission_euler_from_overlay(
			opennova::anim::compute_held_weapon_attach_angles(p_in));
	r[NovaSimulation::PF_HELD_WEAPON_PITCH_DEG] = attach.x;
	r[NovaSimulation::PF_HELD_WEAPON_YAW_DEG] = attach.y;
	r[NovaSimulation::PF_HELD_WEAPON_ROLL_DEG] = attach.z;
	// The frame selector. Retail reads it off the SAME hold state the upper-body weapon
	// channel already uses, so nothing new has to be derived here — flag 0x80 on that
	// state means the weapon is posed at the hand instead of at the entity triple.
	// Unlike the channel's own snapshot field this must not be gated on channel
	// visibility: the frame applies whenever the weapon is DRAWN.
	// [orig: gate @ 0x4b21b6 / branch @ 0x4b220f; the state's writer
	//  Entity_UpdateInfantryPlayerBody @ 0x4b5dad..0x4b5ea9]
	if (p_weapon_hold_state >= 0 &&
			(opennova::world::infantry_anim_flags(p_weapon_hold_state) & 0x80u) != 0)
		r[NovaSimulation::PF_HELD_WEAPON_HAND_FRAME] = 1.0f;
}

inline void write_present_overlay(float *record,
		const opennova::anim::AimOverlayAngles
				angles[opennova::anim::kOverlayClassCount]) {
	record[NovaSimulation::PF_AIM_OVERLAY_VALID] = 1.0f;
	const Vector3 body = mission_euler_from_overlay(
			angles[opennova::anim::kOverlayBody]);
	record[NovaSimulation::PF_AIM_BODY_PITCH_DEG] = body.x;
	record[NovaSimulation::PF_AIM_BODY_YAW_DEG] = body.y;
	record[NovaSimulation::PF_AIM_BODY_ROLL_DEG] = body.z;
	for (int cls = 0; cls < opennova::anim::kOverlayClassCount; ++cls) {
		const Vector3 value = mission_euler_from_overlay(angles[cls]);
		const int base = NovaSimulation::PF_AIM_ANGLES +
				cls * NovaSimulation::PF_AIM_CLASS_STRIDE;
		record[base] = value.x;
		record[base + 1] = value.y;
		record[base + 2] = value.z;
	}
}

inline Array aim_overlay_deltas_for(
		const opennova::anim::AimOverlayAngles
				angles[opennova::anim::kOverlayClassCount]) {
	Array deltas;
	deltas.resize(opennova::anim::kOverlayClassCount);
	const Basis inverse_body =
			godot_model_basis_from_overlay(
					angles[opennova::anim::kOverlayBody]).inverse();
	for (int i = 0; i < opennova::anim::kOverlayClassCount; ++i)
		deltas[i] = inverse_body * godot_model_basis_from_overlay(angles[i]);
	return deltas;
}

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

// The collision/occlusion/bound-radius builders and the model predicates
// moved to the engine (ADR 0028): engine/runtime/simassets. The using
// declarations keep this header's call sites unchanged.
using opennova::simassets::collision_model_from_3di;
using opennova::simassets::model_bound_radius_from_3di;
using opennova::simassets::occlusion_model_from_3di;


inline void panm_render_matrix_from_godot(const Transform3D &transform, float out[16]) {
	// Exact inverse of NovaObjectData::panm_matrix_to_transform: recover the
	// row-major, row-vector render matrix emitted by the native PANM evaluator.
	std::memset(out, 0, sizeof(float) * 16);
	const Basis &basis = transform.basis;
	out[0] = basis[0].x;
	out[4] = -basis[0].y;
	out[8] = -basis[0].z;
	out[1] = -basis[1].x;
	out[5] = basis[1].y;
	out[9] = basis[1].z;
	out[2] = -basis[2].x;
	out[6] = basis[2].y;
	out[10] = basis[2].z;
	out[12] = -transform.origin.x;
	out[13] = transform.origin.y;
	out[14] = transform.origin.z;
	out[15] = 1.0f;
}

inline constexpr double kHalfPi = 1.57079632679489661923;
inline constexpr double kRadiansPerDegree =
		3.14159265358979323846 / 180.0;

// C++ twin of MissionObjectPlacer.bms_to_godot_basis for ordinary mission
// eulers. Kept here because the mounted provider must produce the same world
// frame without depending on presentation/GDScript.
inline Basis godot_model_basis_from_mission_euler(
		double pitch_deg, double yaw_deg, double roll_deg) {
	return Basis(Vector3(0.0, 1.0, 0.0),
			(90.0 - yaw_deg) * kRadiansPerDegree) *
			Basis(Vector3(0.0, 0.0, 1.0),
					pitch_deg * kRadiansPerDegree) *
			Basis(Vector3(1.0, 0.0, 0.0),
					roll_deg * kRadiansPerDegree) *
			Basis(Vector3(0.0, 1.0, 0.0), kHalfPi);
}

inline bool finite_vector3(const Vector3 &value) {
	return std::isfinite(static_cast<double>(value.x)) &&
			std::isfinite(static_cast<double>(value.y)) &&
			std::isfinite(static_cast<double>(value.z));
}

inline bool finite_basis(const Basis &value) {
	return finite_vector3(value[0]) && finite_vector3(value[1]) &&
			finite_vector3(value[2]);
}

inline bool resolve_model_mounted_pose(
		const Ref<NovaObjectData> &data,
		const opennova::world::Entity &carrier,
		const opennova::world::Seat &seat,
		const Dictionary &controls, uint32_t time_ms,
		opennova::world::MountedPose &out) {
	if (data.is_null() || seat.type != opennova::world::SeatType::Gunner ||
			seat.bone_index == 0)
		return false;
	const int userpoint_index = static_cast<int>(seat.bone_index) - 1;
	if (userpoint_index < 0 || userpoint_index >= data->get_user_point_count())
		return false;
	const Dictionary userpoint = data->get_user_point_info(userpoint_index);
	const int part_index = static_cast<int>(userpoint.get("subobject", -1));
	const Vector3 authored_model_position = userpoint.get("position", Vector3());
	const Vector3 authored_model_direction =
			userpoint.get("rotation", Vector3());
	if (part_index < 0 || !finite_vector3(authored_model_position)) return false;

	constexpr int lod_index = 0;
	const Dictionary rest_parts = data->evaluate_panm(lod_index, 0, Dictionary());
	const Dictionary live_parts = data->evaluate_panm(lod_index, time_ms, controls);
	if (!rest_parts.has(part_index) || !live_parts.has(part_index)) return false;
	const Variant rest_value = rest_parts[part_index];
	const Variant live_value = live_parts[part_index];
	if (rest_value.get_type() != Variant::TRANSFORM3D ||
			live_value.get_type() != Variant::TRANSFORM3D)
		return false;
	const Transform3D rest_part = static_cast<Transform3D>(rest_value);
	const Transform3D live_part = static_cast<Transform3D>(live_value);
	if (!finite_vector3(rest_part.origin) || !finite_basis(rest_part.basis) ||
			!finite_vector3(live_part.origin) || !finite_basis(live_part.basis) ||
			std::abs(static_cast<double>(rest_part.basis.determinant())) < 1.0e-8)
		return false;

	const Vector3 point_in_part =
			rest_part.affine_inverse().xform(authored_model_position);
	const Vector3 live_model_position = live_part.xform(point_in_part);
	const Basis carrier_basis = godot_model_basis_from_mission_euler(
			carrier.pitch, carrier.yaw, carrier.roll);
	if (!finite_vector3(live_model_position) || !finite_basis(carrier_basis) ||
			std::abs(static_cast<double>(carrier_basis.determinant())) < 1.0e-8)
		return false;
	const Vector3 carrier_origin(
			carrier.position.x, carrier.position.z, -carrier.position.y);
	const Vector3 live_world_position =
			Transform3D(carrier_basis, carrier_origin).xform(live_model_position);
	if (!finite_vector3(live_world_position)) return false;
	out.position = {
			static_cast<float>(live_world_position.x),
			static_cast<float>(-live_world_position.z),
			static_cast<float>(live_world_position.y)};

	Basis live_basis;
	if (seat.attachment_frame &&
			finite_vector3(authored_model_direction) &&
			authored_model_direction.length_squared() > 1.0e-8) {
		// An addeweap child owns the complete EWeap userpoint frame. Build the
		// same direction look-at frame retail multiplies through the live bone:
		// forward = direction; right = (forward.z, 0, -forward.x);
		// up = forward x right. Retail's result is a row-vector render matrix,
		// so transpose and conjugate by the loader's X mirror exactly as
		// panm_matrix_to_transform does before composing it in Godot. The
		// rest-bone inverse then makes that authored model-space frame
		// part-local; the live bone carries both position and orientation
		// through PANM.
		// [orig: build_bone_attachment_matrix @0x56C630;
		// build_direction_look_at_matrix @0x612C90]
		const Vector3 forward = authored_model_direction.normalized();
		Vector3 right(forward.z, 0.0, -forward.x);
		if (right.length_squared() <= 1.0e-8)
			right = Vector3(1.0, 0.0, 0.0);
		else
			right.normalize();
		Vector3 up = forward.cross(right);
		if (up.length_squared() <= 1.0e-8) return false;
		up.normalize();
		const Basis retail_frame(right, up, forward);
		const Basis render_x_flip(
				Vector3(-1.0, 0.0, 0.0),
				Vector3(0.0, 1.0, 0.0),
				Vector3(0.0, 0.0, 1.0));
		const Basis authored_basis =
				render_x_flip * retail_frame.transposed() * render_x_flip;
		const Basis attachment_in_part =
				rest_part.basis.inverse() * authored_basis;
		live_basis =
				carrier_basis * live_part.basis * attachment_in_part;
	} else {
		const double baseline_yaw =
				seat.attachment_frame
				? static_cast<double>(carrier.yaw + seat.yaw_offset)
				: seat.type == opennova::world::SeatType::Gunner
				? static_cast<double>(carrier.yaw - seat.yaw_offset)
				: static_cast<double>(carrier.yaw + seat.yaw_offset);
		const Basis baseline_basis = godot_model_basis_from_mission_euler(
				carrier.pitch, baseline_yaw, carrier.roll);
		const Basis part_delta =
				live_part.basis * rest_part.basis.inverse();
		live_basis = carrier_basis * part_delta *
				carrier_basis.inverse() * baseline_basis;
	}
	if (!finite_basis(live_basis) ||
			std::abs(static_cast<double>(live_basis.determinant())) < 1.0e-8)
		return false;
	live_basis = live_basis.orthonormalized();
	const Basis euler_basis = live_basis *
			Basis(Vector3(0.0, 1.0, 0.0), -kHalfPi);
	const double pitch_rad = std::asin(std::clamp(
			static_cast<double>(euler_basis[1].x), -1.0, 1.0));
	if (std::abs(std::cos(pitch_rad)) < 1.0e-6) return false;
	const double heading_rad = std::atan2(
			-static_cast<double>(euler_basis[2].x),
			static_cast<double>(euler_basis[0].x));
	const double roll_rad = std::atan2(
			-static_cast<double>(euler_basis[1].z),
			static_cast<double>(euler_basis[1].y));
	const double yaw_deg = opennova::world::normalize_mission_yaw_deg(
			90.0 - heading_rad / kRadiansPerDegree);
	const double pitch_deg = pitch_rad / kRadiansPerDegree;
	const double roll_deg = roll_rad / kRadiansPerDegree;
	if (!std::isfinite(yaw_deg) || !std::isfinite(pitch_deg) ||
			!std::isfinite(roll_deg))
		return false;
	out.yaw = static_cast<int16_t>(std::lround(yaw_deg));
	out.pitch = static_cast<int16_t>(std::lround(pitch_deg));
	out.roll = static_cast<int16_t>(std::lround(roll_deg));
	return true;
}

inline bool resolve_client_eweap_attachment_pose(
		const opennova::netsim::ClientEntityState &child,
		const opennova::netsim::ClientState &state,
		const std::vector<opennova::mission::ItemSeatSpec> &specs,
		const std::unordered_map<int32_t, Ref<NovaObjectData>> &model_data_by_type,
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
	const auto data_found = model_data_by_type.find(parent->type_id);
	if (data_found == model_data_by_type.end() || data_found->second.is_null())
		return false;
	const Ref<NovaObjectData> &data = data_found->second;

	// Remote generic PLAYPARTANIM phases are not in ClientEntityState. Do not
	// synthesize them from timing or repurpose a wire field. EWEAP is the one safe
	// articulated family: the decoded mounted gunner already determines both
	// semantic controls through the witnessed parent-minus-occupant relationship.
	EmplacedWeaponControls emplaced;
	if (!emplaced_weapon_controls_for_client(
				*parent, state, specs, emplaced))
		return false;
	const Threedi3di3 &model = data->native_model();
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
	Dictionary controls;
	controls[String(kEmplacedGunYawRegister)] =
			static_cast<int>(emplaced.gun_yaw);
	controls[String(kEmplacedGunPitchRegister)] =
			static_cast<int>(emplaced.gun_pitch);

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
	return resolve_model_mounted_pose(
			data, carrier, attachment->anchor, controls, time_ms, out);
}

// Coordinate converters shared by the debug views and present getters.
// Mission-space 16.16 triple -> Godot world space: (x, y, z) -> (x, z, -y) units.
inline Vector3 godot_from_fixed3(const int32_t p[3]) {
	return Vector3(static_cast<float>(p[0] / 65536.0), static_cast<float>(p[2] / 65536.0),
	               static_cast<float>(-p[1] / 65536.0));
}
// Mission float Vec3 -> Godot world space: (x, y, z) -> (x, z, -y).
inline Vector3 godot_from_mission_vec3(const opennova::world::Vec3 &p) {
	return Vector3(p.x, p.z, -p.y);
}
// Render float world -> Godot world: the render frame is Godot with X/Z
// swapped ((-my, mz, mx)/65536 == (gz, gy, gx)), so the inverse is the same swap.
inline Vector3 godot_from_render_float3(const float p[3]) {
	return Vector3(p[2], p[1], p[0]);
}

} // namespace novasim
