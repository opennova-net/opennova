#include "simulation/nova_simulation.h"

#include <wac/compiler.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "netsim/connection.h"
#include "netsim/entity_wire_bridge.h" // build_player_uplink (joiner-side C2S 0x0C body)

#include <npwire/ingame_decode.h> // class_from_tag (§5.10b *_function -> wire class)
#include <npwire/ingame_encode.h> // encode_organic_spawn_batch (+ OrganicSpawnBatch)

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
#include <world/player_spawn.h>
#include <world/spawn_select.h>
#include <world/vehicle_attach.h> // player_toggle_vehicle_mount (the USE-ITEM toggle)

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

namespace {

constexpr double kFixed16 = 65536.0;
constexpr int kPlayerVisualItemId = 105310; // items.def "Player #1, Single player" -> US01/US01.adm
// Canonical definition lives in libs/world/player_spawn.h (shared with the npruntime host).
constexpr uint16_t kRetailPlayerMinEntitySlot = opennova::world::kRetailPlayerMinEntitySlot;
constexpr float kBinocularAimOffsetDeg = 2.8125f; // 0x02000000 BAM
constexpr double kTau = 6.28318530717958647692;

uint64_t present_effect_origin_key(int kind, int index) {
	return (static_cast<uint64_t>(static_cast<uint32_t>(kind)) << 32) |
	       static_cast<uint32_t>(index);
}

uint64_t perf_now_us() {
	using Clock = std::chrono::steady_clock;
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

opennova::world::SeatType seat_type_from_variant(int value) {
	switch (value) {
		case static_cast<int>(opennova::world::SeatType::Passenger): return opennova::world::SeatType::Passenger;
		case static_cast<int>(opennova::world::SeatType::Controller): return opennova::world::SeatType::Controller;
		case static_cast<int>(opennova::world::SeatType::Gunner): return opennova::world::SeatType::Gunner;
		case static_cast<int>(opennova::world::SeatType::Driver): return opennova::world::SeatType::Driver;
		default: return opennova::world::SeatType::None;
	}
}

bool seat_type_blocks_weapon_channel(opennova::world::SeatType type) {
	switch (type) {
		case opennova::world::SeatType::Controller:
		case opennova::world::SeatType::Gunner:
		case opennova::world::SeatType::Driver:
			return true;
		default:
			return false; // passenger seats retain the on-foot upper-body channel
	}
}

bool mount_blocks_weapon_channel(const opennova::world::Entity &entity) {
	return entity.mounted && seat_type_blocks_weapon_channel(entity.mount_type);
}

bool mount_collapses_right_hand_row(const opennova::world::Entity &entity) {
	// This terminal skeletal row is stricter than the secondary-channel gate:
	// retail requires a controller/gunner/driver parent slot AND no Flags 0x100.
	// In the port, engine_flags is the authoritative entity+0x24 Flags mirror.
	return entity.mounted &&
			seat_type_blocks_weapon_channel(entity.mount_type) &&
			(entity.engine_flags & 0x100u) == 0;
}

int visual_item_id_for_runtime_type(int item_id, const Ref<NovaItemDatabase> &item_db) {
	if (item_id == opennova::world::kPlayerInfantryTypeId && item_db.is_valid() &&
	    item_db->has_item(kPlayerVisualItemId)) {
		return kPlayerVisualItemId;
	}
	return item_id + opennova::mission::kItemIdOffset;
}

opennova::anim::MountMode mount_mode_for_seat_type(
		opennova::world::SeatType seat_type) {
	switch (seat_type) {
		case opennova::world::SeatType::Controller:
		case opennova::world::SeatType::Driver:
			return opennova::anim::MountMode::Seated;
		case opennova::world::SeatType::Gunner:
			return opennova::anim::MountMode::Gunner;
		default:
			return opennova::anim::MountMode::OnFoot;
	}
}

opennova::anim::MountMode mount_mode_for(
		const opennova::world::Entity &entity) {
	return entity.mounted
			? mount_mode_for_seat_type(entity.mount_type)
			: opennova::anim::MountMode::OnFoot;
}

opennova::anim::AimOverlayInputs aim_overlay_inputs_for(
		const AiEntity &entity, const opennova::world::Entity &world_entity) {
	opennova::anim::AimOverlayInputs in;
	in.aim_yaw = entity.heading;
	in.aim_pitch = entity.pitch;
	in.body_yaw = entity.inf.body_heading;
	in.leg_yaw_r = entity.inf.leg_yaw[0];
	in.leg_yaw_l = entity.inf.leg_yaw[1];
	in.head_look_decay = entity.inf.head_look_decay;
	in.lean = entity.inf.lean_angle;
	in.roll = entity.roll;
	in.body_pitch = entity.body_pitch;
	in.torso_roll = entity.inf.torso_roll;
	in.aim_state =
			(opennova::world::infantry_anim_flags(entity.inf.anim_state) & 0x40u) != 0;
	in.rolling =
			entity.inf.anim_state == opennova::world::anim_state::kRollLeft ||
			entity.inf.anim_state == opennova::world::anim_state::kRollRight;
	in.mount_mode = mount_mode_for(world_entity);
	if (in.mount_mode != opennova::anim::MountMode::OnFoot) {
		in.mount_config_valid = world_entity.mounted_config_valid;
		in.mount_config = world_entity.mounted_config_valid
				? world_entity.mounted_config
				: 0;
	}
	return in;
}

const opennova::netsim::ClientEntityState *client_entity_for_handle(
		const opennova::netsim::ClientState &state, uint16_t handle) {
	for (const opennova::netsim::ClientEntityState &entity : state.entities) {
		if (entity.handle == handle) return &entity;
	}
	return nullptr;
}

const opennova::mission::ItemSeatSpec *item_seat_spec_for_type(
		const std::vector<opennova::mission::ItemSeatSpec> &specs,
		uint16_t type_id) {
	for (const opennova::mission::ItemSeatSpec &spec : specs) {
		if (spec.type_id == static_cast<int32_t>(type_id)) return &spec;
	}
	return nullptr;
}

constexpr char kEmplacedGunYawRegister[] = "EWEAP_GUNYAW";
constexpr char kEmplacedGunPitchRegister[] = "EWEAP_GUNPITCH";
constexpr char kHeatGlowRegister[] = "HEAT_GLOW";

// CTRL registers that a dedicated engine system owns, so a generic PLAYPARTANIM
// phase must never be placed on one.
//
// In retail these are two unrelated arrays on the AI comp: PLAYPARTANIM integrates
// its two channels into comp[113]/comp[114] (direction comp[109/110] + rate
// comp[111/112], clamped to [0, 0x10000]), while the named CTRL registers live at
// comp+0x1D4 + 4*index and are written by their own systems — the emplaced turret
// writes the yaw/pitch pair directly, and HEAT_GLOW is only ever driven by an ACTION
// row carrying a `ctrlreg` key. Our PANM evaluation binds by NAME, so without this
// rule the model-order walk below drops a part phase onto whichever register happens
// to come first. B50Cal is exactly that case: its CTRL list is
// [HEAT_GLOW, EWEAP_GUNYAW, EWEAP_GUNPITCH], so all three are owned and it takes no
// generic phase at all.
//
// Deliberately only the three names witnessed as non-PLAYPARTANIM. Other entries in
// the global 32-byte table (LOD_*, HELO_*, VEHICLE_*, PARTICLE_*) are very likely
// engine-owned too, but their writers are unwalked and guessing would be inventing.
// [orig: PLAYPARTANIM case 34 @ 0x43b192 (slot = ANIMNUM-1, direction @ comp+0x1B4,
//  rate @ comp+0x1BC); the integrator @ 0x456710; the CTRL name table @ 0x83dce8
//  (HEAT_GLOW ordinal 54); the turret writer @ 0x441007/@ 0x44101a; the only CTRL
//  animator ActionSlot_ExecuteAction @ 0x4020cc -> CtrlRegAnimSlot_Allocate
//  @ 0x401ca0 -> CtrlRegAnimSlot_UpdateAll @ 0x401bf0]
bool ctrl_register_is_engine_owned(const String &p_name) {
	return p_name.nocasecmp_to(kEmplacedGunYawRegister) == 0 ||
			p_name.nocasecmp_to(kEmplacedGunPitchRegister) == 0 ||
			p_name.nocasecmp_to(kHeatGlowRegister) == 0;
}

// Place the two generic PLAYPARTANIM phases onto the model's CTRL registers in model
// order, skipping engine-owned names. `p_phase_for` yields the phase for channel i.
template <typename PhaseFn>
void assign_part_anim_phases(const ThreediModelIR &p_ir, Dictionary &r_controls,
		PhaseFn p_phase_for) {
	int channel = 0;
	for (size_t slot = 0; slot < p_ir.control_register_count && channel < 2; ++slot) {
		const String name = String::utf8(p_ir.control_registers[slot].name);
		if (name.is_empty() || ctrl_register_is_engine_owned(name)) continue;
		r_controls[name] = p_phase_for(channel);
		++channel;
	}
}

struct EmplacedWeaponControls {
	bool valid = false;
	uint16_t gun_yaw = 0;
	uint16_t gun_pitch = 0;
};

uint16_t emplaced_control_phase(int32_t parent_bam, int32_t occupant_bam) {
	// Retail stores the high word of the wrapped parent-minus-occupant angle:
	// occupant Yaw/Pitch = parent Yaw/Pitch - turret control.
	// [orig: Entity_UpdateTransformAndTurret @0x441251..0x441263,
	//  @0x441298..0x4412b3]
	const int32_t delta = opennova::io::bam_sub(parent_bam, occupant_bam);
	return static_cast<uint16_t>(static_cast<uint32_t>(delta) >> 16);
}

bool emplaced_weapon_controls_for(
		const opennova::world::World &world,
		opennova::world::AiSystem *ai,
		const opennova::world::Entity &mount,
		EmplacedWeaponControls &out) {
	out = EmplacedWeaponControls{};
	if (ai == nullptr || !mount.primary_weapon_owner.valid()) return false;
	const opennova::world::Entity *occupant =
			world.registry.get(mount.primary_weapon_owner);
	if (occupant == nullptr || !occupant->alive || occupant->health <= 0 ||
			!occupant->mounted ||
			occupant->mount_type != opennova::world::SeatType::Gunner ||
			occupant->mount_target != mount.handle)
		return false;
	const AiEntity *gunner = ai->for_handle(occupant->handle);
	if (gunner == nullptr) return false;

	// The parent owns the embedded weapon/model while the organic owns live look.
	// A vehicle motor preserves sub-degree parent yaw in BAM; a static EWEAP uses
	// its mission-yaw field. Pitch has no separate motor accumulator.
	const int32_t parent_heading = mount.veh.yaw_seeded
			? mount.veh.yaw_bam
			: opennova::world::bam_heading_from_mission_yaw_deg(
					static_cast<double>(mount.yaw));
	const int32_t parent_pitch =
			opennova::world::bam_from_degrees_wrapped(
					static_cast<double>(mount.pitch));
	out.valid = true;
	out.gun_yaw = emplaced_control_phase(parent_heading, gunner->heading);
	out.gun_pitch = emplaced_control_phase(parent_pitch, gunner->pitch);
	return true;
}

bool emplaced_weapon_controls_for_client(
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

		// NetClientView has already composed mounted yaw into world heading and
		// reconstructed an infantry gunner's live entity pitch from the compact
		// aim target using retail's one-eighth chase.
		const int32_t parent_heading = static_cast<int32_t>(
				static_cast<uint32_t>(mount.yaw_byte) << 24);
		const int32_t occupant_heading = static_cast<int32_t>(
				static_cast<uint32_t>(occupant.yaw_byte) << 24);
		const int32_t occupant_pitch =
				occupant.cls == opennova::EntityClass::Player
				? static_cast<int32_t>(
						static_cast<uint32_t>(occupant.pitch_byte) << 24)
				: occupant.pitch_bam;
		out.valid = true;
		out.gun_yaw =
				emplaced_control_phase(parent_heading, occupant_heading);
		out.gun_pitch =
				emplaced_control_phase(mount.pitch_bam, occupant_pitch);
		return true;
	}
	return false;
}

void write_present_emplaced_controls(
		float *record, const EmplacedWeaponControls &controls) {
	if (!controls.valid) return;
	record[NovaSimulation::PF_EMPLACED_CONTROLS_VALID] = 1.0f;
	record[NovaSimulation::PF_EWEAP_GUNYAW] =
			static_cast<float>(controls.gun_yaw);
	record[NovaSimulation::PF_EWEAP_GUNPITCH] =
			static_cast<float>(controls.gun_pitch);
}

bool aim_overlay_inputs_for_client(
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
	out.aim_yaw = static_cast<int32_t>(
			static_cast<uint32_t>(entity.yaw_byte) << 24);
	out.aim_pitch = static_cast<int32_t>(
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

	// Both witnessed compact organic records already carry the carrier and raw
	// seat bone. Bone zero is the standing-on/deck form, not a mount. Resolve
	// the carrier's wire type into the host-fed production seat table; never
	// synthesize a config byte or alias a missing definition to config zero.
	if (entity.carrier_handle == 0xFFFFu || entity.mount_bone == 0) return true;
	const opennova::netsim::ClientEntityState *carrier =
			client_entity_for_handle(state, entity.carrier_handle);
	if (carrier == nullptr) return true;
	const opennova::mission::ItemSeatSpec *spec =
			item_seat_spec_for_type(specs, carrier->type_id);
	if (spec == nullptr) return true;
	const opennova::world::Seat *seat = nullptr;
	for (const opennova::world::Seat &candidate : spec->seats) {
		if (candidate.bone_index == entity.mount_bone) {
			seat = &candidate;
			break;
		}
	}
	if (seat == nullptr) return true;
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
	// (90-mission yaw), so those signs invert here.
	const int32_t carrier_heading = static_cast<int32_t>(
			static_cast<uint32_t>(carrier->yaw_byte) << 24);
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

double bam_to_radians(int32_t value) {
	return static_cast<double>(value) *
			(6.28318530717958647692 / 4294967296.0);
}

Basis godot_model_basis_from_overlay(
		const opennova::anim::AimOverlayAngles &angles) {
	// C++ twin of MissionObjectPlacer.bms_to_godot_basis. The first yaw
	// simplifies to the BAM heading itself; the final +90 degree term is the
	// .3di model-forward correction.
	return Basis(Vector3(0.0, 1.0, 0.0), bam_to_radians(angles.yaw)) *
			Basis(Vector3(0.0, 0.0, 1.0), bam_to_radians(angles.pitch)) *
			Basis(Vector3(1.0, 0.0, 0.0), bam_to_radians(angles.roll)) *
			Basis(Vector3(0.0, 1.0, 0.0), 1.57079632679489661923);
}

Vector3 mission_euler_from_overlay(
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

void write_present_overlay(float *record,
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

Array aim_overlay_deltas_for(
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

std::string dictionary_string(const Dictionary &d, const char *key, const std::string &fallback) {
	if (!d.has(key)) return fallback;
	const String value = d.get(key, String());
	return std::string(value.utf8().get_data());
}

void apply_dictionary_string(const Dictionary &d, const char *key, std::string &out) {
	if (d.has(key)) {
		out = dictionary_string(d, key, out);
	}
}

uint16_t dictionary_u16(const Dictionary &d, const char *key, uint16_t fallback) {
	if (!d.has(key)) return fallback;
	const int value = static_cast<int>(d.get(key, static_cast<int>(fallback)));
	return static_cast<uint16_t>(std::clamp(value, 0, 0xFFFF));
}

uint32_t dictionary_u32(const Dictionary &d, const char *key, uint32_t fallback) {
	if (!d.has(key)) return fallback;
	const int64_t value = static_cast<int64_t>(d.get(key, static_cast<int64_t>(fallback)));
	if (value < 0) return 0;
	if (value > 0xFFFFFFFFll) return 0xFFFFFFFFu;
	return static_cast<uint32_t>(value);
}

// Build the same synthetic patrol mission the C++ promote_test uses: 3 markers forming a
// path, one looping waypoint record (channel 1), 2 organics on that route, 1 building.
opennova::bms::File make_demo_mission() {
	opennova::bms::File m{};

	auto marker = [](int32_t x, int32_t y, int32_t z) {
		opennova::bms::Entity e{};
		e.type = opennova::bms::ItemType::Marker;
		e.x = x; e.y = y; e.z = z;
		return e;
	};
	auto organic = [](int32_t x, int32_t y, int32_t z, uint8_t team, uint8_t wp_id) {
		opennova::bms::Entity e{};
		e.type = opennova::bms::ItemType::Organic;
		e.x = x; e.y = y; e.z = z;
		e.yaw = 90;
		e.team = team;
		e.waypoint_id = wp_id;
		e.wp_number = 0;
		e.min_engagement_distance = 50 << 16;
		e.max_engagement_distance = 500 << 16;
		return e;
	};

	m.markers.push_back(marker(100 << 16, 0, 0));
	m.markers.push_back(marker(200 << 16, 0, 0));
	m.markers.push_back(marker(300 << 16, 0, 0));

	opennova::bms::WaypointRecord wr{};
	wr.flags = opennova::bms::WaypointFlags::None; // loops
	wr.marker_count = 3;
	wr.waypoint_numbers = {0, 1, 2};
	m.waypoint_records.push_back(wr);

	// A BLUE-flagged player route over the same markers: promotion builds the
	// HUD waypoint track from the first such record (marker 0 authors a wide
	// radius + a name id so the view surfaces meaningful fields).
	m.markers[0].wp_distance = 25;
	m.markers[0].ttool_index = 1;
	opennova::bms::WaypointRecord player_route{};
	player_route.flags = opennova::bms::WaypointFlags::BlueTeam;
	player_route.marker_count = 3;
	player_route.waypoint_numbers = {0, 1, 2};
	m.waypoint_records.push_back(player_route);

	m.organics.push_back(organic(0, 0, 0, /*team=*/1, /*wp_id=*/1));
	m.organics.push_back(organic(50 << 16, 0, 0, /*team=*/2, /*wp_id=*/1));

	opennova::bms::Entity bldg{};
	bldg.type = opennova::bms::ItemType::Building;
	bldg.x = 999 << 16;
	m.buildings.push_back(bldg);

	// Authored SSNs (promotion copies record ids verbatim, like the original).
	m.organics[0].id = 1;
	m.organics[1].id = 2;
	m.buildings[0].id = 3;
	m.markers[0].id = 10;
	m.markers[1].id = 11;
	m.markers[2].id = 12;
	return m;
}

} // namespace

NovaSimulation::NovaSimulation() {
	reset_world();
	set_process(true);
}

void NovaSimulation::reset_local_player_view_effects() {
	player_view_.binoculars_requested = false;
	player_view_.binoculars_raised = false;
	player_view_.binoculars_view_active = false;
	binocular_yaw_offset_deg_ = 0.0f;
	binocular_pitch_offset_deg_ = 0.0f;
	player_view_.nvg_gain = opennova::world::kNvgGainMin;
	player_view_.nvg_active = world_ != nullptr &&
			(world_->mission_attrib_flags &
					static_cast<uint32_t>(
							opennova::bms::AttribFlags::StartWithNVGOn)) != 0;
	nvg_scope_restore_ = false;
	refresh_local_player_view_effects();
}

void NovaSimulation::refresh_local_player_view_effects() {
	const opennova::world::Entity *local = world_ != nullptr
			? world_->registry.get(world_->cached.local_player)
			: nullptr;
	const bool alive = local != nullptr && local->alive && local->health > 0;
	const bool round_ended = world_ != nullptr && world_->round_end.ended;
	opennova::world::player_view_update_effective_modes(
			player_view_, alive, round_ended);
}

void NovaSimulation::reset_world() {
	invalidate_present_effect_pose_cache();
	pending_weapon_events_.clear();
	weapon_anim_tick_ = 0;
	local_round_sequence_ = 0;
	weapon_active_ = false;
	weapon_fire_held_ = false;
	weapon_fire_pressed_ = false;
	weapon_reload_pressed_ = false;
	// Mission-scoped loadout state [orig: Game_StartMission rebuilds restrictionData +
	// g_armoryWeaponAvailability per mission @ 0x5246c3/@ 0x5246e8].
	local_inventory_valid_ = false;
	spawn_kit_.clear();
	spawn_kit_set_ = false;
	weapon_availability_.reset();
	weapon_switch_in_flight_ = false;
	weapon_switch_deferred_action_ = -1;
	weapon_start_in_switchto_ = false;
	player_view_ = opennova::world::PlayerViewState{};
	nvg_scope_restore_ = false;
	binocular_yaw_offset_deg_ = 0.0f;
	binocular_pitch_offset_deg_ = 0.0f;
	local_eye_valid_ = false;
	local_usegun_switch_ = LocalUseGunSwitch::kNone;
	local_usegun_slot_active_ = false;
	local_usegun_mount_ = opennova::world::EntityHandle{};
	local_usegun_weapon_adm_ = 0xFF;
	local_usegun_pending_mount_ = opennova::world::EntityHandle{};
	local_usegun_pending_weapon_adm_ = 0xFF;
	local_usegun_saved_adm_ = 0xFF;
	local_usegun_switch_action_ = -1;
	local_first_person_model_adm_ = 0xFF;
	world_ = std::make_unique<World>();
	world_->external_local_mounted_weapon_pump = true;
	world_->projectile_authority = !joiner_;
	world_->mp_session = host_listen_ || joiner_;
	ai_ = std::make_unique<AiSystem>();
	bms_ = std::make_unique<opennova::mission::BmsEventSystem>();
	wac_ = std::make_unique<opennova::wac::WacSystem>();
	promo_ = opennova::mission::PromoteResult{};
	loaded_ = false;
	playing_ = false;
	have_baseline_ = false;
	last_sim_tick_us_ = 0;
	last_net_tick_us_ = 0;
	last_present_snapshot_us_ = 0;
	last_present_entity_count_ = 0;
	// Collision models/instances are mission-scoped: drop them with the world (the
	// sweep re-registers on the next load) and re-point the fresh ai_ at the container.
	collision_item_db_.unref();
	collision_placer_.unref();
	item_traits_db_.unref();
	collision_model_by_graphic_.clear();
	collision_occlusion_by_graphic_.clear();
	collision_radius_by_graphic_.clear();
	collision_husk_kz_points_by_graphic_.clear();
	collision_husk_pieces_by_graphic_.clear();
	collision_resolution_attempted_.clear();
	collision_pose_data_.clear();
	collision_skeletal_sources_.clear();
	infantry_adm_resource_root_.unref();
	infantry_adm_item_db_.unref();
	infantry_adm_resolved_ai_count_ = 0;
	panm_time_override_ms_ = -1;
	collision_world_ = opennova::world::CollisionWorld{};
	// Occlusion models too — retail reloads the model cache per mission, so the
	// weld pass's shared-record type-5 rewrites never leak across loads.
	occlusion_world_ = opennova::world::OcclusionWorld{};
	occlusion_culled_bms_.clear();
	apply_terrain_to_ai(); // re-point the fresh ai_ at the persisted terrain field (if any)
	apply_root_motion_to_ai(); // ...and at the persisted infantry clip set (if any)
	apply_collision_to_ai();
}

// Re-point the (possibly just-rebuilt) AI system at our owned terrain field. The field's raw
// pointers reference terrain_heightmap_/terrain_sector_grid_, which persist across reset_world.
void NovaSimulation::apply_terrain_to_ai() {
	// The round sim's ground stop shares the same field (world.terrain; §5.60).
	if (world_) world_->terrain = terrain_field_.valid() ? &terrain_field_ : nullptr;
	if (world_) {
		// The footstep surface pick reads the charmap through this view; the
		// zero-initialized map is the sampler's "no charmap -> surface 1" leg.
		world_->surface_map =
			surface_indices_.empty() ? opennova::terrain::SurfaceTypeMap{} : surface_map_;
	}
	apply_sound_state_to_world();
	if (!ai_) return;
	ai_->terrain = terrain_field_.valid() ? &terrain_field_ : nullptr;
	ai_->ground_clearance = opennova::world::GroundClearance{};
	// The collision ground probe shares the same field.
	collision_world_.terrain = terrain_field_.valid() ? &terrain_field_ : nullptr;
}

// (Re)apply the persisted sound-profile chain state to the current world: the parsed
// SndProf.def table and the water plane. Runs from apply_terrain_to_ai
// (reset_world / load) and from the setters when live. (The mission attrib
// dword the scream's night gate reads is stamped by finish_load from the BMS
// header — not re-applied here.)
void NovaSimulation::apply_sound_state_to_world() {
	if (!world_) return;
	world_->sound_profiles.clear();
	if (!sndprof_text_.empty())
		world_->sound_profiles.parse(reinterpret_cast<const char *>(sndprof_text_.data()),
		                             sndprof_text_.size());
	world_->env.water_z = env_water_z_q16_;
}

// Re-point the (possibly just-rebuilt) AI system at the owned infantry root-motion source.
// A source with no clips counts as none: the selector then resolves every state to "no
// clip" and soldiers stand, exactly the original's relationship between motion and clips.
void NovaSimulation::apply_root_motion_to_ai() {
	if (!ai_) return;
	ai_->root_motion = !infantry_anim_.empty() ? &infantry_anim_ : nullptr;
}

void NovaSimulation::reset_infantry_adm_ids() {
	infantry_adm_resolved_ai_count_ = 0;
	if (!world_ || !world_->ai) return;
	AiSystem &ai = *world_->ai;
	for (int i = 0; i < ai.count(); ++i) {
		if (AiEntity *e = ai.at(i)) e->inf.adm_id = 0;
	}
}

int NovaSimulation::set_infantry_anim_map(const Ref<NovaResourceRoot> &p_resource_root, const String &p_adm_name) {
	// The default clip set (adm_id 0): every infantry entity grounds off this until its own
	// model's .adm is registered (register_infantry_adm + set_infantry_adm_id). Clearing here
	// resets the whole registry on each (re)load.
	infantry_anim_.clear();
	reset_infantry_adm_ids();
	const int default_adm_id = infantry_anim_.register_adm(p_resource_root, p_adm_name);
	const int default_clip_count = default_adm_id == 0 ? infantry_anim_.clip_count(0) : 0;
	// Every stored per-entity id indexes this registry; rebuilding it invalidates
	// all prior assignments. Only repopulate once slot 0 is the successfully loaded
	// default map; otherwise a model-specific ADM could usurp the default slot and
	// turn a failed load into false success.
	if (default_adm_id == 0) resolve_new_infantry_adm_ids();
	apply_root_motion_to_ai();
	return default_clip_count;
}

// Assign only newly attached AI entries. AiSystem::attach is append-only, including when
// an entity handle is reused, so the count is a generation-safe high-water mark. This is
// the spawn-time half of AnimMap_RegisterEntity: late joiner-local/remote players must not
// retain the default E_STAND map or configured emplacements fall back to anim_emplaced.
// [orig: AnimMap_RegisterEntity @0x40bb60; AnimMap_UpdateEntity @0x40b5f0.]
void NovaSimulation::resolve_new_infantry_adm_ids() {
	if (!world_ || !world_->ai || infantry_adm_resource_root_.is_null() ||
			infantry_adm_item_db_.is_null() || infantry_anim_.empty())
		return;
	AiSystem &ai = *world_->ai;
	const int count = ai.count();
	if (infantry_adm_resolved_ai_count_ < 0 ||
			infantry_adm_resolved_ai_count_ > count)
		infantry_adm_resolved_ai_count_ = 0;
	for (int i = infantry_adm_resolved_ai_count_; i < count; ++i) {
		AiEntity *e = ai.at(i);
		if (!e) continue;
		e->inf.adm_id = 0;
		if (!e->inf.active) continue;
		const opennova::world::Entity *ent = world_->registry.get(e->handle);
		if (!ent) continue;
		const int visual_item_id =
				visual_item_id_for_runtime_type(ent->item_id, infantry_adm_item_db_);
		String adm = infantry_adm_item_db_->get_anim_def(visual_item_id);
		if (adm.is_empty()) continue;
		if (!adm.to_lower().ends_with(".adm")) adm += ".adm";
		const int adm_id =
				infantry_anim_.register_adm(infantry_adm_resource_root_, adm);
		if (adm_id >= 0) e->inf.adm_id = adm_id;
	}
	infantry_adm_resolved_ai_count_ = count;
}

// Per-entity .adm resolution: ground each soldier off its OWN model's clip, not the shared
// default set (adm_id 0). Retain the host inputs because multiplayer players are spawned
// after this mission-load sweep; the step/spawn hooks above the world layer resolve each
// later AiSystem entry exactly once.
void NovaSimulation::resolve_infantry_adm_ids(const Ref<NovaResourceRoot> &p_resource_root,
		const Ref<NovaItemDatabase> &p_item_db) {
	if (p_resource_root.is_null() || p_item_db.is_null()) return;
	infantry_adm_resource_root_ = p_resource_root;
	infantry_adm_item_db_ = p_item_db;
	reset_infantry_adm_ids();
	resolve_new_infantry_adm_ids();
}

// Stamp every live entity's items.def-derived wire traits via the item database:
// - Entity::is_ai_capable from ItemDefAttrib & 0x100000 (AIData): the host's pool-1 0x0D stream
//   emits its AI-trailer iff AI-capable, matching the stock 0x0D decoder's own gate exactly
//   (itemDef.attrib & 0x100000 @0x433327) — byte-faithful AND crash-safe (D-NET-97).
// - Entity::net_class_code from the items.def class tag (ai_function, else move_function — the
//   directive that drives the ItemDef+356 serialize-callback lookup [orig: ingame_decode.h §5.10b])
//   via opennova::class_from_tag. Load-bearing: only witnessed callback classes may be serialized
//   into the 0x0A event loop — classifying a pool-1 ewep emplacement as a vehicle desyncs the
//   retail client mid-frame (retail-join v13, 2026-07-02).
// - Entity::health_max (+ health lift) from items.def hp (itemDef+0x17C healthMax): the original
//   spawns Health = healthMax [orig: Entity_InitFromItemDef @0x49e550]; entities still at the
//   promotion default (100) are lifted to full health. Feeds the §5.13 vehicle health word (a
//   too-small value renders every vehicle burning) and the §5.10 field-17 tier denominator.
//
// The registry's for_each is const-only, so collect the live handles first, then re-fetch each as
// a mutable Entity* — the same mutate-by-handle shape resolve_infantry_adm_ids uses.
//
// ID SPACE (load-bearing): Entity::item_id is the WIRE type id — the small on-disk .bms type that
// build_pool*_batch puts on the wire verbatim (e.g. 0x050E). NovaItemDatabase is keyed by the
// items.def id, which is wire + kItemIdOffset (mission_bms_test: bms_type_id 1291 -> item_id
// 101291; nova_net_client.cpp wire = def_id - 100000). The offset here is mandatory: without it
// every pool-1 lookup misses.
// [orig: NapiNPClientMsg_0x00D @0x432c40; docs/net/novaworld-net-re.md D-NET-97]
void NovaSimulation::resolve_item_traits(const Ref<NovaItemDatabase> &p_item_db) {
	if (!world_ || p_item_db.is_null()) return;
	item_traits_db_ = p_item_db;
	// Cache the Player template's items.def hp at world level so LATE-JOINER spawns (which happen
	// after this sweep) seed full health without an item-db reach-back from libs/ [orig:
	// Entity_InitFromItemDef @0x49e550 — spawn Health = itemDef->healthMax]. (D-NET-144)
	const int player_def_id =
			static_cast<int>(opennova::world::kPlayerInfantryTypeId) +
			opennova::mission::kItemIdOffset;
	world_->player_has_item_def = p_item_db->has_item(player_def_id);
	world_->player_item_hp = opennova::world::retail_signed_i16(
			p_item_db->get_hp(player_def_id));
	world_->player_item_type = static_cast<uint8_t>(p_item_db->get_item_type(player_def_id));
	world_->player_item_attrib = p_item_db->get_attrib(player_def_id);
	world_->player_armor_impact = opennova::world::retail_signed_i16(
			p_item_db->get_armor_impact(player_def_id));
	world_->player_armor_kz = opennova::world::retail_signed_i16(
			p_item_db->get_armor_kz(player_def_id));
	world_->player_damage_reduc_pp = p_item_db->get_damage_reduc_pp(player_def_id);
	world_->player_damage_reduc_max = p_item_db->get_damage_reduc_max(player_def_id);
	std::vector<opennova::world::EntityHandle> handles;
	world_->registry.for_each([&](const opennova::world::Entity &e) { handles.push_back(e.handle); });
	for (const opennova::world::EntityHandle h : handles) {
		opennova::world::Entity *e = world_->registry.get(h);
		if (!e) continue;
		const int def_id = static_cast<int>(e->item_id) + opennova::mission::kItemIdOffset;
		e->has_item_def = p_item_db->has_item(def_id);
		e->item_type = static_cast<uint8_t>(p_item_db->get_item_type(def_id));
		e->is_ai_capable = p_item_db->is_ai_capable(def_id);
		// §5.10b replication class from the *_function tag (ai_function, else move_function).
		const String ai_fn = p_item_db->get_ai_function(def_id);
		const String tag = ai_fn.is_empty() ? p_item_db->get_move_function(def_id) : ai_fn;
		e->net_class_code =
				static_cast<uint8_t>(opennova::class_from_tag(tag.utf8().get_data()));
		// items.def hp -> healthMax; lift spawn-default health to full [orig: @0x49e550].
		const int hp = opennova::world::retail_signed_i16(p_item_db->get_hp(def_id));
		e->health = opennova::world::retail_signed_i16(e->health);
		e->health_max = opennova::world::retail_signed_i16(e->health_max);
		e->armor_impact = opennova::world::retail_signed_i16(
				p_item_db->get_armor_impact(def_id));
		e->armor_kz = opennova::world::retail_signed_i16(
				p_item_db->get_armor_kz(def_id));
		e->damage_reduc_pp = p_item_db->get_damage_reduc_pp(def_id);
		e->damage_reduc_max = p_item_db->get_damage_reduc_max(def_id);
		if (hp != 0) {
			e->health_max = hp;
			if (e->health == 100) e->health = hp; // still at the promotion default
		}
		// Indestructible item (def hp == 0): entity Flags |= 0x4000000 and subType = 0xFF —
		// the def-sourced half of the 0x10 static record's flag dword / flag-0x80 byte
		// (D-NET-147; every golden ASH_I5A building carries both). Resolved defs only — a
		// missing items.def id stays untouched. [orig: Entity_InitFromModel @0x40dc8e:
		// !itemDef->healthMax -> Flags |= 0x4000000, Health = 1, subType = -1]
		if (hp == 0 && p_item_db->has_item(def_id)) {
			e->engine_flags |= 0x4000000u;
			e->sub_type = 0xFF;
		}
		// AS zone traits from the attrib dword: 0x20000 "ChangeTeam" = capture trigger,
		// 0x40000 "SpawnPoint" = deploy-selectable (the ASH_I5A "Change Team & Spawn
		// Volume" objects carry both). [orig: def+84 gates in ZoneSlotChain_BuildFromMission
		// @0x4a2de0 / Server_ResolveSpawnTargetHandle @0x4fe110; net-re §5.61]
		const uint32_t attrib = p_item_db->get_attrib(def_id);
		e->item_attrib = attrib;
		e->is_capture_trigger = (attrib & 0x20000u) != 0;
		e->is_spawn_point = (attrib & 0x40000u) != 0;
		// Death-presentation traits: LeaveCorpse (attrib 0x400000) keeps the corpse
		// forever; deathtime (def+0x890, parse-scaled ticks) seeds the corpse timer at
		// the death edge. [orig: ItemDef_ParseProperty @0x4a09d3 / @0x49fa6c; consumers
		// Entity_UpdateInfantryAI @0x4b9e54 / @0x4b9c97; world-wac-ai-re §19]
		e->leave_corpse = (attrib & 0x400000u) != 0;
		e->deathtime_ticks = p_item_db->get_deathtime_ticks(def_id);
		// Destruction traits (world/destruction.h; world-wac-ai-re §24): the death
		// chain's def fields, keyed by item id. Fills once per distinct id.
		// [orig: the ItemDef fields Entity_ApplyWeaponDamage / the death dispatch /
		// Entity_InitDeathSounds read — armor +0x190/+0x192, unitType +0x196, kz
		// +0x198, huskSubPart* +0x100.., debrisScale +0x1BC, soundDeath +0x860,
		// the particledeath family +0x412..]
		if (world_->item_death_traits.get(e->item_id) == nullptr &&
		    p_item_db->has_item(def_id)) {
			const Dictionary dt = p_item_db->get_death_traits(def_id);
			if (!dt.is_empty()) {
				opennova::world::ItemDeathTraits t;
				t.unit_type = int(dt.get("unit_type", 0));
				t.kz = float(double(dt.get("kz", 0.0)));
				t.armor_impact = int(dt.get("armor_impact", 0));
				t.armor_blast = int(dt.get("armor_blast", 0));
				t.team_protect = (attrib & 0x8000u) != 0;
				t.no_die = (attrib & 0x40000000u) != 0;
				t.static_death = (p_item_db->get_attrib2(def_id) & 0x100u) != 0;
				t.has_husk = bool(dt.get("has_husk", false));
				t.is_decoration =
						p_item_db->get_item_type(def_id) == NovaItemDatabase::TYPE_DECORATION;
				t.husk_sub_part_count =
						static_cast<uint8_t>(std::clamp(int(dt.get("husk_sub_parts", 0)), 0, 255));
				const PackedInt32Array types = dt.get("husk_sub_part_types", PackedInt32Array());
				for (int s = 0; s < 17 && s < types.size(); ++s)
					t.husk_sub_part_types[s] = static_cast<uint8_t>(types[s]);
				t.debris_scale = float(double(dt.get("debris_scale", 0.0)));
				t.sound_death = String(dt.get("sounddeath", String())).utf8().get_data();
				const Dictionary fx = p_item_db->get_particle_effects(def_id);
				t.particledeath = String(fx.get("particledeath", String())).utf8().get_data();
				t.particleh2odeath =
						String(fx.get("particleh2odeath", String())).utf8().get_data();
				t.particlefire = String(fx.get("particlefire", String())).utf8().get_data();
				t.particleother = String(fx.get("particleother", String())).utf8().get_data();
				// resolve_collision_instances enriches this row with live husk-model
				// state and the active first-stage husk's KZ user points.
				world_->item_death_traits.set(e->item_id, std::move(t));
			}
		}
		// Vehicle motor traits: the pre-scaled items.def physics block + the PlayerControl
		// attrib (0x40) gate, keyed by item id in the world table. Fills once per distinct
		// id; the AI tick's vehicle pass drives pool-1 entities whose traits carry a
		// non-zero `physics` selector. [orig: ItemDef_ParsePhysicsProperty @0x49d870;
		// Entity_UpdateVehiclePhysics @0x48af00 attrib & 0x40 gate @0x48b0e6]
		if (e->handle.pool() == 1 &&
		    world_->vehicle_traits.get(e->item_id) == nullptr) {
			const PackedInt32Array vp = p_item_db->get_vehicle_physics(def_id);
			if (vp.size() == 8 && vp[0] != 0) {
				opennova::world::VehicleTraits vt;
				vt.physics = vp[0];
				vt.player_speed = vp[1];
				vt.acceleration = vp[2];
				vt.deceleration = vp[3];
				vt.turn_rate = vp[4];
				vt.turn_rate2 = vp[5];
				vt.unit_type = vp[6];
				vt.torque = vp[7];
				vt.player_control = (attrib & 0x40u) != 0;
				world_->vehicle_traits.set(e->item_id, vt);
			}
		}
	}
	// Throwable class bindings: every items.def entry whose ai_function /
	// move_function names a throwable class (nade/schl/clym/vmne/lndm) lands a
	// row keyed by type id (id - 100000, the ammo TrcrID space), with the def
	// hp/armor the placed device spawns at. [orig: EntityDef_InitAllCallbacks
	// @ 0x4a5a70 resolves the class tables into every item def at load;
	// world-wac-ai-re §27.]
	world_->throwables.classes.clear();
	const PackedInt32Array all_ids = p_item_db->get_item_ids();
	for (int i = 0; i < all_ids.size(); ++i) {
		const int def_id = all_ids[i];
		const opennova::world::ThrowClass think = opennova::world::throw_class_from_tag(
				p_item_db->get_ai_function(def_id).utf8().get_data());
		const opennova::world::ThrowClass motor = opennova::world::throw_class_from_tag(
				p_item_db->get_move_function(def_id).utf8().get_data());
		if (think == opennova::world::ThrowClass::kNone &&
				motor == opennova::world::ThrowClass::kNone)
			continue;
		opennova::world::ThrowableClassRow row;
		row.item_id = def_id - opennova::mission::kItemIdOffset;
		row.think = think;
		row.motor = motor;
		row.health_max = opennova::world::retail_signed_i16(p_item_db->get_hp(def_id));
		row.armor_impact = opennova::world::retail_signed_i16(
				p_item_db->get_armor_impact(def_id));
		row.armor_kz = opennova::world::retail_signed_i16(
				p_item_db->get_armor_kz(def_id));
		world_->throwables.classes.set(row);
	}
	// The AS zone-slot chain — built AFTER the trait stamp (zone registration keys on
	// is_capture_trigger), then the secure latch seeds each rear zone's control to 1.0.
	// [orig: ZoneSlotChain_BuildFromMission @0x4a2de0 from Game_StartMission @0x526126;
	// the latch is Server_UpdateCaptureZoneEntities' first act @0x519764; net-re §5.61]
	opennova::world::zone_chain_build_from_mission(*world_, world_->zone_chain);
	opennova::world::zone_chain_latch_control(*world_, world_->zone_chain);

	// Decode-side twin of the net_class_code stamp above: the full wire-id ->
	// replication-class table for the LOCAL CLIENT VIEW. The retail client sizes each
	// inbound 0x0A tag-1 record via its OWN items.def serialize callback [orig:
	// itemDef+356 dispatch @0x50f2e2 / ItemList_FindIndexByTypeId]; without this table
	// the view's phase-1 heuristic walks vehicle (15/21 B) and no-callback (0 B)
	// records at the wrong width and desyncs the rest of the frame — every junk record
	// after the desync lands anchor-relative, i.e. scattered around the local player.
	// Same tag rule as the encoder stamp (ai_function, else move_function) so both
	// sides of the in-process wire agree by construction.
	auto table = std::make_shared<std::unordered_map<uint16_t, opennova::EntityClass>>();
	const PackedInt32Array ids = p_item_db->get_item_ids();
	for (int i = 0; i < ids.size(); ++i) {
		const int def_id = ids[i];
		const int wire_id = def_id - opennova::mission::kItemIdOffset;
		if (wire_id < 0 || wire_id > 0xFFFF) continue;
		const String ai_fn = p_item_db->get_ai_function(def_id);
		const String tag = ai_fn.is_empty() ? p_item_db->get_move_function(def_id) : ai_fn;
		const opennova::EntityClass cls =
				opennova::class_from_tag(tag.utf8().get_data());
		if (cls != opennova::EntityClass::Unknown) {
			(*table)[static_cast<uint16_t>(wire_id)] = cls;
		}
	}
	item_class_table_ = std::move(table);
	install_item_class_resolver();
}

void NovaSimulation::install_item_class_resolver() {
	if (!runtime_ || !item_class_table_) return;
	runtime_->view().set_item_class_resolver(
			[table = item_class_table_](uint16_t type_id) {
				const auto it = table->find(type_id);
				return it != table->end() ? it->second : opennova::EntityClass::Unknown;
			});
}

// The D-AI-5 host weapon seed. The original resolves the items.def ammo_closeattack/
// easyrocket/advancedrocket/marker3 names into ammo-def ids on the def and block-copies
// them onto the entity (+0x358..0x35B; the copy site is the open world-wac-ai-re §17.7
// item 1 — no per-field writer exists). Until that copy is witnessed, the port carries
// ONE ammo id + clipsize per NPC (AiProfile — JO riflemen author all four slots to the
// same rifle round), stamped here from the item database against the loaded ammo table.
// Also seeds the spawn magazine: word entity+0x35C = itemDef+0x894 clipsize [orig:
// Entity_ResetToSpawnState @ 0x4b97a9/0x4b97b5]. Consumption stays motor-gated: only
// the infantry fire pass reads ammo_primary (host-side NPCs; never the local player).
// [orig: ItemDef_ParseProperty @ 0x4a1823 (-> def+0x56B) / @ 0x49fa1c (-> def+0x894);
// docs/divergence-ledger.md D-AI-5]
int NovaSimulation::resolve_ai_weapons(const Ref<NovaItemDatabase> &p_item_db) {
	if (!world_ || !world_->ai || p_item_db.is_null()) return 0;
	int armed = 0;
	// Bind every body's sound profile first — persons AND vehicles carry one
	// (e.g. DBuggy01 -> SP_DuneBuggy), and unarmed defs (the player) must not
	// skip it. An unauthored key resolves to "default" via the emit-side
	// fallback (index stays -1). [orig: the def+0x268 parse binding
	// @ 0x49fb0f-0x49fb64; alloc seed @ 0x49e3f5]
	if (!world_->sound_profiles.empty()) {
		for (int i = 0; i < world_->ai->count(); ++i) {
			opennova::world::AiEntity *ae = world_->ai->at(i);
			if (ae == nullptr) continue;
			const opennova::world::Entity *e = world_->registry.get(ae->handle);
			if (e == nullptr) continue;
			const int def_id = static_cast<int>(e->item_id) + opennova::mission::kItemIdOffset;
			const String prof = p_item_db->get_sound_profile(def_id);
			if (prof.is_empty()) continue;
			ae->profile.sound_profile = static_cast<int16_t>(
				world_->sound_profiles.index_of(prof.utf8().get_data()));
		}
	}
	if (world_->ammo.empty()) return 0; // no ammo.def loaded — NPCs stay unarmed
	for (int i = 0; i < world_->ai->count(); ++i) {
		opennova::world::AiEntity *ae = world_->ai->at(i);
		if (ae == nullptr) continue;
		const opennova::world::Entity *e = world_->registry.get(ae->handle);
		if (e == nullptr) continue;
		const int def_id = static_cast<int>(e->item_id) + opennova::mission::kItemIdOffset;
		const String ammo_name = p_item_db->get_ammo_closeattack(def_id);
		if (ammo_name.is_empty()) continue; // def authors no anim-fire round (e.g. the player)
		const int ammo = world_->ammo.index_of(ammo_name.utf8().get_data());
		if (ammo < 0) continue; // name not in this mission's ammo.def — stay unarmed
		ae->profile.ammo_primary = ammo;
		ae->profile.clip_size = p_item_db->get_clipsize(def_id);
		ae->inf.magazine = static_cast<int16_t>(ae->profile.clip_size);
		++armed;
	}
	return armed;
}

namespace {

// Build the runtime collision model from a parsed .3di collision IR block — the exact
// inverse of the parse scaling (BPLN normals int16 Q14 / 16384, distances + AABBs 16.16;
// libs/threedi/src/threedi_3di3.cpp parse_bpln/parse_bvol). Sections mirror the COBJ
// grouping: CVRT/CNRM/CFAC/BVOL arrays are sequential per object. Face-only Poly
// Collision LOD models remain valid without semantic volumes, and organic callers may
// explicitly retain COBJ sphere-only models for posed person collision.
bool collision_model_from_ir(const ThreediIRCollision *col,
	                             opennova::world::CollisionModel &out,
	                             bool allow_sphere_only = false) {
	if (col == nullptr || !threedi_ir_collision_is_runtime_safe(col)) return false;
	const bool has_face_mesh =
			col->face_count > 0 && col->faces != nullptr && col->vertex_count > 0 &&
			col->vertices != nullptr && col->object_count > 0 && col->objects != nullptr;
	bool has_person_spheres = false;
	if (allow_sphere_only && col->objects != nullptr) {
		for (size_t i = 0; i < col->object_count; ++i) {
			if (col->objects[i].radius_fp16 > 0) {
				has_person_spheres = true;
				break;
			}
		}
	}
	if (col->volume_count == 0 && !has_face_mesh && !has_person_spheres)
		return false;
	auto fx = [](float v) { return static_cast<int32_t>(std::lround(v * 65536.0)); };

	out.vertices.reserve(col->vertex_count);
	for (size_t i = 0; i < col->vertex_count; ++i) {
		opennova::world::CollisionVertex v;
		for (int k = 0; k < 3; ++k) v.p[k] = fx(col->vertices[i].position[k]);
		out.vertices.push_back(v);
	}
	out.normals.reserve(col->normal_count);
	for (size_t i = 0; i < col->normal_count; ++i) {
		opennova::world::CollisionNormal n;
		for (int k = 0; k < 3; ++k) n.n[k] = col->normals[i].normal_q14[k];
		n.dominant_axis = col->normals[i].dominant_axis;
		out.normals.push_back(n);
	}
	// The legacy projectile path consumes the same CVRT run requantized to its
	// authored Q8 words. IR positions originated as Q8/256, so this round-trip
	// is exact while the indexed path above retains its Q16 view.
	out.face_vertices.reserve(col->vertex_count);
	for (size_t i = 0; i < col->vertex_count; ++i) {
		opennova::world::CollisionFaceVertex v;
		v.x = static_cast<int16_t>(std::lround(col->vertices[i].position[0] * 256.0f));
		v.y = static_cast<int16_t>(std::lround(col->vertices[i].position[1] * 256.0f));
		v.z = static_cast<int16_t>(std::lround(col->vertices[i].position[2] * 256.0f));
		out.face_vertices.push_back(v);
	}
	// One shared face vector carries both query representations: exact indexed
	// CVRT/CNRM fields and the embedded Q8/Q14 fields used by the older walker.
	out.faces.reserve(col->face_count);
	for (size_t i = 0; i < col->face_count; ++i) {
		const ThreediIRCollisionFace &sf = col->faces[i];
		opennova::world::CollisionFace f;
		for (int k = 0; k < 3; ++k) {
			f.vertex_index[k] = sf.vert_index[k];
			f.v[k] = sf.vert_index[k];
			f.normal[k] = sf.normal[k];
			f.min[k] = sf.min_fp16[k];
			f.max[k] = sf.max_fp16[k];
		}
		f.normal_index = sf.normal_index;
		f.axis = sf.dominate_axis;
		f.plane_dist = sf.plane_dist_fp16;
		f.material_flags = sf.material_flags;
		f.poly_type = sf.poly_type;
		f.flags = sf.material_flags;
		f.material = sf.poly_type;
		out.faces.push_back(f);
	}

	out.planes.reserve(col->plane_count);
	for (size_t i = 0; i < col->plane_count; ++i) {
		const ThreediIRCollisionPlane &sp = col->planes[i];
		opennova::world::CollisionPlane p;
		p.nx = static_cast<int16_t>(std::lround(sp.normal[0] * 16384.0f));
		p.ny = static_cast<int16_t>(std::lround(sp.normal[1] * 16384.0f));
		p.nz = static_cast<int16_t>(std::lround(sp.normal[2] * 16384.0f));
		p.dist = fx(sp.distance);
		out.planes.push_back(p);
	}

	out.volumes.reserve(col->volume_count);
	for (size_t i = 0; i < col->volume_count; ++i) {
		const ThreediIRCollisionVolume &sv = col->volumes[i];
		opennova::world::CollisionVolume v;
		v.type = sv.type;
		v.flags = static_cast<uint32_t>(sv.flags);
		v.min_x = fx(sv.min[0]);
		v.max_x = fx(sv.max[0]);
		v.min_y = fx(sv.min[1]);
		v.max_y = fx(sv.max[1]);
		v.min_z = fx(sv.min[2]);
		v.max_z = fx(sv.max[2]);
		v.plane_start = sv.plane_start;
		v.plane_count = sv.plane_count;
		out.volumes.push_back(v);
	}

	if (col->object_count == 0) {
		// Legacy ungrouped convex-only block.
		out.sections.assign(1, {});
		out.sections[0].volume_count = static_cast<int32_t>(col->volume_count);
		return true;
	}

	out.sections.assign(col->object_count, {});
	int32_t vertex_cursor = 0, normal_cursor = 0, face_cursor = 0, volume_cursor = 0;
	for (size_t s = 0; s < col->object_count; ++s) {
		const ThreediIRCollisionObject &object = col->objects[s];
		opennova::world::CollisionSection &sec = out.sections[s];
		sec.vertex_start = vertex_cursor;
		sec.vertex_count = object.num_vertices;
		sec.normal_start = normal_cursor;
		sec.normal_count = object.num_planes;
		sec.face_start = face_cursor;
		sec.face_count = object.num_faces;
		sec.face_vertex_start = vertex_cursor;
		sec.face_vertex_count = object.num_vertices;
		sec.volume_start = volume_cursor;
		sec.volume_count = object.num_bounding_volumes;
		sec.parent_part_index = object.parent_subobject_index;
		sec.part_index = object.parent_subobject_index;
		for (int k = 0; k < 3; ++k) {
			sec.offset[k] = object.offset[k];
			sec.center[k] = object.mid[k];
		}
		sec.min_x = object.min[0]; sec.max_x = object.max[0];
		sec.min_y = object.min[1]; sec.max_y = object.max[1];
		sec.min_z = object.min[2]; sec.max_z = object.max[2];
		sec.radius = object.radius;
		// Empty COBJ records can carry inverted/sentinel bounds. Preserve valid
		// authored bounds exactly; otherwise let finalize_sections derive them
		// from that object's volume or vertex run.
		sec.authored_bounds =
				object.radius >= 0 &&
				object.min[0] <= object.max[0] &&
				object.min[1] <= object.max[1] &&
				object.min[2] <= object.max[2];
		vertex_cursor += sec.vertex_count;
		normal_cursor += sec.normal_count;
		face_cursor += sec.face_count;
		volume_cursor += sec.volume_count;
	}
	return true;
}

// The model bound-sphere radius from the .3di itself — the union of the LOD-0
// part bounding spheres seen from the model origin, with the primitive boxes as
// the degenerate-sphere fallback. This is the entity+0 boundRadius source: the
// original reads it off the MODEL header (gpm[5]) for every placed item,
// collision block or not, and the proximity/hit tests and blast ranges all
// consume it [orig: Entity_InitFromModel @ 0x40dc30; world-wac-ai-re §24].
float model_bound_radius_from_ir(const ThreediModelIR &ir) {
	if (ir.lod_count == 0 || ir.lods == nullptr) return 0.0f;
	const ThreediIRLod &lod = ir.lods[0];
	float r = 0.0f;
	for (size_t i = 0; lod.parts != nullptr && i < lod.part_count; ++i) {
		const ThreediIRPart &p = lod.parts[i];
		const float cx = p.abs_position[0] + p.bounding_center[0];
		const float cy = p.abs_position[1] + p.bounding_center[1];
		const float cz = p.abs_position[2] + p.bounding_center[2];
		const float c = std::sqrt(cx * cx + cy * cy + cz * cz);
		if (c + p.bounding_radius > r) r = c + p.bounding_radius;
	}
	if (r <= 0.0f) {
		for (size_t i = 0; lod.primitives != nullptr && i < lod.primitive_count; ++i) {
			const ThreediIRPrimitive &pr = lod.primitives[i];
			for (int a = 0; a < 3; ++a) {
				r = std::max(r, std::abs(pr.min[a]));
				r = std::max(r, std::abs(pr.max[a]));
			}
		}
	}
	return r;
}

// Build the runtime occlusion model from the parsed occlusion IR — the 60 B
// portal-face records with their sequential slices (the IR conversion already
// mirrors the arena assignment of [orig: load_occlusion_model_data @ 0x5b4a00]).
// The IR face dwords decode as the 12 B OFAC record: bytes 0-2 = vertex
// indices, byte 3 = plane index, then the 3 edge words (bit 15 = winding).
bool occlusion_model_from_ir(const ThreediIROcclusion *occ,
                             opennova::world::OcclusionModel &out) {
	if (occ == nullptr || occ->object_count == 0) return false;
	out.vertices.reserve(occ->vertex_count);
	for (size_t i = 0; i < occ->vertex_count; ++i) {
		opennova::world::OcclusionVertex v;
		v.p[0] = occ->vertices[i].position[0];
		v.p[1] = occ->vertices[i].position[1];
		v.p[2] = occ->vertices[i].position[2];
		out.vertices.push_back(v);
	}
	out.planes.reserve(occ->plane_count);
	for (size_t i = 0; i < occ->plane_count; ++i) {
		opennova::world::OcclusionPlane p;
		p.normal[0] = occ->planes[i].normal[0];
		p.normal[1] = occ->planes[i].normal[1];
		p.normal[2] = occ->planes[i].normal[2];
		p.d = occ->planes[i].radius;
		out.planes.push_back(p);
	}
	out.faces.reserve(occ->face_count);
	for (size_t i = 0; i < occ->face_count; ++i) {
		const ThreediIROcclusionFace &sf = occ->faces[i];
		opennova::world::OcclusionFaceRec f;
		f.v[0] = static_cast<uint8_t>(sf.raw_indices & 0xFF);
		f.v[1] = static_cast<uint8_t>((sf.raw_indices >> 8) & 0xFF);
		f.v[2] = static_cast<uint8_t>((sf.raw_indices >> 16) & 0xFF);
		f.plane = static_cast<uint8_t>((sf.raw_indices >> 24) & 0xFF);
		f.edge[0] = static_cast<uint16_t>(sf.edge_data & 0xFFFF);
		f.edge[1] = static_cast<uint16_t>(sf.edge_data >> 16);
		f.edge[2] = static_cast<uint16_t>(sf.other_edge_data & 0xFFFF);
		out.faces.push_back(f);
	}
	out.records.reserve(occ->object_count);
	for (size_t i = 0; i < occ->object_count; ++i) {
		const ThreediIROcclusionObject &so = occ->objects[i];
		opennova::world::OcclusionPortalFace rec;
		rec.type = static_cast<uint8_t>(so.type);
		rec.section_a = static_cast<uint8_t>(so.parent_subobject_index);
		rec.section_b = static_cast<uint8_t>(so.connecting_subobject);
		rec.pos[0] = so.position[0];
		rec.pos[1] = so.position[1];
		rec.pos[2] = so.position[2];
		rec.radius = so.radius;
		rec.vert_start = so.vertex_start;
		rec.vert_count = so.num_vertices;
		rec.plane_start = so.plane_start;
		rec.plane_count = so.num_planes;
		rec.face_start = so.face_start;
		rec.face_count = so.face_count;
		rec.glow_scale = so.glow_scale;
		out.records.push_back(rec);
	}
	// Slice sanity: reject models whose records point past their arrays, and
	// whose OFAC bytes index outside their record's slice — the engine's hot
	// loops (traverse/build_occluder_planes) read face vertex/plane/edge
	// indices unchecked, so malformed or modded data is rejected here once.
	for (const opennova::world::OcclusionPortalFace &rec : out.records) {
		if (rec.vert_start < 0 || rec.vert_count < 0 ||
		    rec.vert_start + rec.vert_count > static_cast<int32_t>(out.vertices.size()) ||
		    rec.plane_start < 0 || rec.plane_count < 0 ||
		    rec.plane_start + rec.plane_count > static_cast<int32_t>(out.planes.size()) ||
		    rec.face_start < 0 || rec.face_count < 0 ||
		    rec.face_start + rec.face_count > static_cast<int32_t>(out.faces.size()))
			return false;
		for (int32_t f = 0; f < rec.face_count; ++f) {
			const opennova::world::OcclusionFaceRec &face = out.faces[rec.face_start + f];
			if (face.v[0] >= rec.vert_count || face.v[1] >= rec.vert_count ||
			    face.v[2] >= rec.vert_count || face.plane >= rec.plane_count)
				return false;
			for (int k = 0; k < 3; ++k) {
				if ((face.edge[k] & 0xFF) >= rec.vert_count ||
				    ((face.edge[k] >> 8) & 0x7F) >= rec.vert_count)
					return false;
			}
		}
	}
	return true;
}

void panm_render_matrix_from_godot(const Transform3D &transform, float out[16]) {
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

constexpr double kHalfPi = 1.57079632679489661923;
constexpr double kRadiansPerDegree =
		3.14159265358979323846 / 180.0;

// C++ twin of MissionObjectPlacer.bms_to_godot_basis for ordinary mission
// eulers. Kept here because the mounted provider must produce the same world
// frame without depending on presentation/GDScript.
Basis godot_model_basis_from_mission_euler(
		double pitch_deg, double yaw_deg, double roll_deg) {
	return Basis(Vector3(0.0, 1.0, 0.0),
			(90.0 - yaw_deg) * kRadiansPerDegree) *
			Basis(Vector3(0.0, 0.0, 1.0),
					pitch_deg * kRadiansPerDegree) *
			Basis(Vector3(1.0, 0.0, 0.0),
					roll_deg * kRadiansPerDegree) *
			Basis(Vector3(0.0, 1.0, 0.0), kHalfPi);
}

bool finite_vector3(const Vector3 &value) {
	return std::isfinite(static_cast<double>(value.x)) &&
			std::isfinite(static_cast<double>(value.y)) &&
			std::isfinite(static_cast<double>(value.z));
}

bool finite_basis(const Basis &value) {
	return finite_vector3(value[0]) && finite_vector3(value[1]) &&
			finite_vector3(value[2]);
}

bool resolve_model_mounted_pose(
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

	const double baseline_yaw =
			seat.type == opennova::world::SeatType::Gunner
			? static_cast<double>(carrier.yaw - seat.yaw_offset)
			: static_cast<double>(carrier.yaw + seat.yaw_offset);
	const Basis baseline_basis = godot_model_basis_from_mission_euler(
			carrier.pitch, baseline_yaw, carrier.roll);
	const Basis part_delta = live_part.basis * rest_part.basis.inverse();
	Basis live_basis = carrier_basis * part_delta *
			carrier_basis.inverse() * baseline_basis;
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

bool resolve_client_eweap_attachment_pose(
		const opennova::netsim::ClientEntityState &child,
		const opennova::netsim::ClientState &state,
		const std::vector<opennova::mission::ItemSeatSpec> &specs,
		const std::unordered_map<int32_t, Ref<NovaObjectData>> &model_data_by_type,
		uint32_t time_ms, opennova::world::MountedPose &out) {
	if (child.parent_handle == 0xFFFFu) return false;
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
	const ThreediModelIR &ir = data->native_ir();
	if (ir.control_register_count > 0 && ir.control_registers == nullptr)
		return false;
	bool has_eweap_control = false;
	for (size_t slot = 0; slot < ir.control_register_count; ++slot) {
		const String name = String::utf8(ir.control_registers[slot].name);
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
					static_cast<int32_t>(
							static_cast<uint32_t>(parent->yaw_byte) << 24))));
	carrier.pitch = static_cast<int16_t>(std::lround(
			static_cast<double>(parent->pitch_bam) *
				opennova::world::kDegreesPerBam));
	carrier.roll = static_cast<int16_t>(std::lround(
			static_cast<double>(parent->roll_bam) *
				opennova::world::kDegreesPerBam));
	return resolve_model_mounted_pose(
			data, carrier, attachment->anchor, controls, time_ms, out);
}

} // namespace

void NovaSimulation::apply_collision_to_ai() {
	collision_world_.terrain = terrain_field_.valid() ? &terrain_field_ : nullptr;
	collision_world_.set_section_matrix_provider(this);
	if (world_) {
		world_->collision = &collision_world_;
		world_->mounted_pose_provider = this;
	}
	if (ai_) ai_->collision = &collision_world_;
}

bool NovaSimulation::resolve_mounted_pose(
		opennova::world::World &p_world,
		const opennova::world::Entity &p_carrier,
		const opennova::world::Seat &p_seat,
		opennova::world::MountedPose &r_out) {
	if (!world_ || &p_world != world_.get() ||
			p_seat.type != opennova::world::SeatType::Gunner ||
			p_seat.bone_index == 0)
		return false;
	const auto data_found =
			mounted_pose_data_by_type_.find(p_carrier.item_id);
	if (data_found == mounted_pose_data_by_type_.end() ||
			data_found->second.is_null())
		return false;
	const Ref<NovaObjectData> &data = data_found->second;
	const int userpoint_index = static_cast<int>(p_seat.bone_index) - 1;
	if (userpoint_index < 0 ||
			userpoint_index >= data->get_user_point_count())
		return false;
	const Dictionary userpoint = data->get_user_point_info(userpoint_index);
	const int part_index = static_cast<int>(userpoint.get("subobject", -1));
	const Vector3 authored_model_position =
			userpoint.get("position", Vector3());
	if (part_index < 0 || !finite_vector3(authored_model_position))
		return false;

	constexpr int lod_index = 0;
	const Dictionary rest_parts =
			data->evaluate_panm(lod_index, 0, Dictionary());
	if (!rest_parts.has(part_index)) return false;

	Dictionary controls;
	const ThreediModelIR &ir = data->native_ir();
	if (ir.control_register_count > 0 && ir.control_registers == nullptr)
		return false;
	AiEntity *carrier_ai = ai_ ? ai_->for_handle(p_carrier.handle) : nullptr;
	assign_part_anim_phases(ir, controls, [carrier_ai](int p_channel) {
		return carrier_ai != nullptr
				? std::clamp(carrier_ai->brain.f[
						AiBrain::kPartAnimPhase0 + p_channel], 0, 65535)
				: 0;
	});
	EmplacedWeaponControls emplaced;
	if (emplaced_weapon_controls_for(
			p_world, ai_.get(), p_carrier, emplaced)) {
		controls[String(kEmplacedGunYawRegister)] =
				static_cast<int>(emplaced.gun_yaw);
		controls[String(kEmplacedGunPitchRegister)] =
				static_cast<int>(emplaced.gun_pitch);
	}
	const uint32_t time_ms = panm_time_override_ms_ >= 0
			? static_cast<uint32_t>(panm_time_override_ms_)
			: p_world.logic_tick * 16u;
	const Dictionary live_parts =
			data->evaluate_panm(lod_index, time_ms, controls);
	if (!live_parts.has(part_index)) return false;
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
			p_carrier.pitch, p_carrier.yaw, p_carrier.roll);
	if (!finite_vector3(live_model_position) || !finite_basis(carrier_basis) ||
			std::abs(static_cast<double>(carrier_basis.determinant())) < 1.0e-8)
		return false;
	const Vector3 carrier_origin(
			p_carrier.position.x, p_carrier.position.z,
			-p_carrier.position.y);
	const Vector3 live_world_position =
			Transform3D(carrier_basis, carrier_origin).xform(live_model_position);
	if (!finite_vector3(live_world_position)) return false;
	r_out.position = {
			static_cast<float>(live_world_position.x),
			static_cast<float>(-live_world_position.z),
			static_cast<float>(live_world_position.y)};

	// Apply the articulated part's rest-to-live delta around the carrier to the
	// exact static seat orientation. This identity-round-trips the fallback and
	// lets moving seat parts carry yaw/pitch/roll without coupling to LOOK.
	const double baseline_yaw =
			p_seat.type == opennova::world::SeatType::Gunner
			? static_cast<double>(p_carrier.yaw - p_seat.yaw_offset)
			: static_cast<double>(p_carrier.yaw + p_seat.yaw_offset);
	const Basis baseline_basis = godot_model_basis_from_mission_euler(
			p_carrier.pitch, baseline_yaw, p_carrier.roll);
	const Basis part_delta =
			live_part.basis * rest_part.basis.inverse();
	Basis live_basis = carrier_basis * part_delta *
			carrier_basis.inverse() * baseline_basis;
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
	r_out.yaw = static_cast<int16_t>(std::lround(yaw_deg));
	r_out.pitch = static_cast<int16_t>(std::lround(pitch_deg));
	r_out.roll = static_cast<int16_t>(std::lround(roll_deg));
	return true;
}

bool NovaSimulation::ensure_collision_instance(
		opennova::world::World &p_world,
		opennova::world::EntityHandle p_entity) {
	if (!world_ || &p_world != world_.get() ||
			collision_item_db_.is_null() || collision_placer_.is_null())
		return false;
	const opennova::world::Entity *entity = p_world.registry.get(p_entity);
	if (entity == nullptr) {
		collision_world_.remove_entity_instance(p_entity);
		collision_skeletal_sources_.erase(p_entity.packed);
		collision_resolution_attempted_.erase(p_entity.packed);
		return false;
	}
	const auto attempted =
			collision_resolution_attempted_.find(p_entity.packed);
	if (attempted != collision_resolution_attempted_.end()) {
		if (attempted->second == entity->registry_spawn_id)
			return collision_world_.has_instance(p_world, p_entity);
		collision_world_.remove_entity_instance(p_entity);
		collision_skeletal_sources_.erase(p_entity.packed);
		collision_resolution_attempted_.erase(attempted);
	}

	// Re-run the idempotent attach sweep against the retained mission caches.
	// It resolves every entity that appeared since the previous sweep, including
	// a player deployed after load, without registering another graphic model.
	resolve_collision_instances(collision_item_db_, collision_placer_.ptr());
	return collision_world_.has_instance(p_world, p_entity);
}

bool NovaSimulation::build_section_matrices(opennova::world::World &p_world,
		opennova::world::EntityHandle p_entity, int32_t p_model_id,
		const opennova::world::CollisionMatrix &p_entity_world,
		const opennova::world::CollisionModel &p_model,
		std::vector<opennova::world::CollisionMatrix> &r_out) {
	const auto skeletal_found =
			collision_skeletal_sources_.find(p_entity.packed);
	const opennova::world::Entity *entity =
			p_world.registry.get(p_entity);
	if (skeletal_found != collision_skeletal_sources_.end() &&
			skeletal_found->second.model_id == p_model_id &&
			entity != nullptr && skeletal_found->second.registry_spawn_id ==
					entity->registry_spawn_id) {
		const SkeletalCollisionSource &source = skeletal_found->second;
		AiEntity *ai_entity = ai_ ? ai_->for_handle(p_entity) : nullptr;
		const size_t section_count = p_model.sections.size();
		if (source.anim.is_null() || ai_entity == nullptr || entity == nullptr ||
				source.parents.size() < section_count ||
				source.rest_global.size() < section_count ||
				source.overlay_classes.size() < static_cast<int64_t>(section_count))
			return false;

		const String primary_key = infantry_anim_key(ai_entity->inf.anim_state);
		if (primary_key.is_empty()) return false;
		const float primary_fps = source.anim->get_clip_fps(primary_key, 0);
		const double primary_seconds = primary_fps > 0.0f
				? static_cast<double>(std::max(ai_entity->inf.clip_phase, 0)) /
						(2.0 * primary_fps)
				: 0.0;

		const opennova::anim::AimOverlayInputs inputs =
				aim_overlay_inputs_for(*ai_entity, *entity);
		opennova::anim::AimOverlayAngles
				angles[opennova::anim::kOverlayClassCount];
		opennova::anim::compute_aim_overlay_angles(inputs, angles);
		const Array deltas = aim_overlay_deltas_for(angles);

		String weapon_key;
		double weapon_seconds = 0.0;
		const bool collapse_right_hand =
				mount_collapses_right_hand_row(*entity);
		if (p_world.cached.local_player.valid() &&
				p_entity.packed == p_world.cached.local_player.packed &&
				opennova::world::infantry_weapon_channel_visible(
						ai_entity->inf, weapon_active_,
						mount_blocks_weapon_channel(*entity))) {
			weapon_key = infantry_anim_key(ai_entity->inf.wpn_state);
			const float weapon_fps = source.anim->get_clip_fps(weapon_key, 0);
			if (weapon_fps > 0.0f)
				weapon_seconds =
						static_cast<double>(
								std::max(ai_entity->inf.wpn_clip_phase, 0)) /
						(2.0 * weapon_fps);
		}

		const Array pose = source.anim->eval_pose_overlay(
				primary_key, primary_seconds, source.overlay_classes, deltas,
				weapon_key, weapon_seconds, collapse_right_hand);
		if (pose.size() < static_cast<int64_t>(section_count)) return false;

		// The callback result is FINAL world-space. Build the body placement from
		// the overlay's body class (not the aim heading), then apply the skinned
		// deformation exactly once. At bind pose pose_global*rest_global^-1 is
		// identity, which guards against both double-rest and double-entity
		// translation. COBJ parent/offset/CXLT are deliberately not selectors:
		// COBJ[i] pairs strictly with this output slot i.
		const int32_t position[3] = {
				p_entity_world.m[3], p_entity_world.m[7], p_entity_world.m[11]};
		const opennova::world::CollisionMatrix body_world =
				opennova::world::collision_matrix_from_euler(
						angles[opennova::anim::kOverlayBody].yaw,
						angles[opennova::anim::kOverlayBody].pitch,
						angles[opennova::anim::kOverlayBody].roll, position);
		std::vector<Transform3D> pose_global(section_count);
		r_out.resize(section_count);
		for (size_t i = 0; i < section_count; ++i) {
			const Variant value = pose[static_cast<int64_t>(i)];
			if (value.get_type() != Variant::TRANSFORM3D) return false;
			const Transform3D local = static_cast<Transform3D>(value);
			const int32_t parent = source.parents[i];
			// eval_pose_overlay emits BN17's zero-scale local clip pose.
			// Retail zeroes the FINAL collision row after overlay/re-anchor. Preserve
			// that literal collision result for COBJ 16: composing body_world here
			// would incorrectly reintroduce the entity translation.
			// [orig: special row @0x4b1290]
			const bool collapsed_right_hand =
					collapse_right_hand && i == 16;
			if (collapsed_right_hand) {
				pose_global[i] = local;
				r_out[i] = opennova::world::CollisionMatrix{};
				continue;
			}
			pose_global[i] = parent >= 0
					? pose_global[static_cast<size_t>(parent)] * local
					: local;
			const Transform3D deformation =
					pose_global[i] * source.rest_global[i].affine_inverse();
			float render_pose[16];
			panm_render_matrix_from_godot(deformation, render_pose);
			if (!opennova::world::collision_matrix_apply_render_pose(
						body_world, render_pose, r_out[i]))
				return false;
		}
		return true;
	}

	const auto found = collision_pose_data_.find(p_model_id);
	if (found == collision_pose_data_.end() || found->second.is_null()) return false;
	const Ref<NovaObjectData> &data = found->second;
	// Retail Generic collision always transforms the canonical first RLOD. It
	// never follows the render-selected LOD or scans for another live PANM.
	constexpr int lod_index = 0;
	if (!data->has_live_panm_for_lod(lod_index)) return false;
	const PackedInt32Array targets =
			data->get_effective_panm_targets(lod_index);
	if (targets.is_empty()) return false;
	const ThreediModelIR &ir = data->native_ir();
	if (ir.control_register_count > 0 && ir.control_registers == nullptr)
		return false;

	// PLAYPARTANIM channel 1/2 drives control-register ordinal 0/1. A brainless
	// static still evaluates free-running PANM with the zero control table.
	Dictionary controls;
	AiEntity *ai_entity = ai_ ? ai_->for_handle(p_entity) : nullptr;
	assign_part_anim_phases(ir, controls, [ai_entity](int p_channel) {
		return ai_entity != nullptr
				? std::clamp(ai_entity->brain.f[
						AiBrain::kPartAnimPhase0 + p_channel], 0, 65535)
				: 0;
	});
	// EWEAP yaw/pitch are semantic CTRL names, not PLAYPARTANIM ordinals — two
	// separate arrays in retail, see ctrl_register_is_engine_owned. The generic
	// walk above skips them (and HEAT_GLOW), so B50Cal takes no part phase and
	// this pair is the only thing that drives it.
	EmplacedWeaponControls emplaced;
	if (entity != nullptr &&
			emplaced_weapon_controls_for(p_world, ai_.get(), *entity, emplaced)) {
		controls[String(kEmplacedGunYawRegister)] =
				static_cast<int>(emplaced.gun_yaw);
		controls[String(kEmplacedGunPitchRegister)] =
				static_cast<int>(emplaced.gun_pitch);
	}
	const uint32_t time_ms = panm_time_override_ms_ >= 0
			? static_cast<uint32_t>(panm_time_override_ms_)
			: (world_ != nullptr ? world_->logic_tick * 16u : 0u);
	const Dictionary transforms =
			data->evaluate_panm(lod_index, time_ms, controls);
	if (transforms.is_empty()) return false;

	// Default every COBJ slot to the Simple callback. Override only PANM nodes
	// whose target part ordinal exists as a collision section. This intentionally
	// ignores COBJ parent metadata and CXLT/offset records: CVRT is model-space.
	r_out.assign(p_model.sections.size(), p_entity_world);
	bool matched_section = false;
	for (int i = 0; i < targets.size(); ++i) {
		const int section = targets[i];
		if (static_cast<size_t>(section) >= r_out.size() ||
				!transforms.has(section))
			continue;
		const Variant value = transforms[section];
		if (value.get_type() != Variant::TRANSFORM3D) return false;
		float pose[16];
		panm_render_matrix_from_godot(static_cast<Transform3D>(value), pose);
		if (!opennova::world::collision_matrix_apply_render_pose(
					p_entity_world, pose, r_out[static_cast<size_t>(section)]))
			return false;
		matched_section = true;
	}
	return matched_section;
}

int NovaSimulation::resolve_collision_instances(const Ref<NovaItemDatabase> &p_item_db,
                                                Object *p_placer) {
	if (!world_ || p_item_db.is_null() || p_placer == nullptr) return 0;
	RefCounted *placer_ref = Object::cast_to<RefCounted>(p_placer);
	if (placer_ref == nullptr) return 0;
	collision_item_db_ = p_item_db;
	collision_placer_ = placer_ref;
	apply_collision_to_ai();
	std::vector<opennova::world::EntityHandle> handles;
	world_->registry.for_each(
			[&](const opennova::world::Entity &e) { handles.push_back(e.handle); });
	int attached = 0;
	for (const opennova::world::EntityHandle h : handles) {
		opennova::world::Entity *e = world_->registry.get(h);
		if (!e || e->kind == opennova::world::EntityKind::Marker)
			continue;
		const auto previous_attempt =
				collision_resolution_attempted_.find(h.packed);
		if (previous_attempt != collision_resolution_attempted_.end() &&
				previous_attempt->second != e->registry_spawn_id) {
			collision_world_.remove_entity_instance(h);
			collision_skeletal_sources_.erase(h.packed);
		}
		collision_resolution_attempted_[h.packed] = e->registry_spawn_id;
		const bool is_organic =
				e->kind == opennova::world::EntityKind::Organic;
		const int def_id = is_organic
				? visual_item_id_for_runtime_type(e->item_id, p_item_db)
				: static_cast<int>(e->item_id) +
						opennova::mission::kItemIdOffset;
		const String graphic = p_item_db->get_graphic(def_id);
		if (graphic.is_empty()) continue;
		const std::string key(graphic.utf8().get_data());
		auto it = collision_model_by_graphic_.find(key);
		if (it == collision_model_by_graphic_.end()) {
			int32_t model_id = -1;
			int32_t occlusion_id = -1;
			float bound_radius = 0.0f;
			// Duck-typed MissionObjectPlacer.object_data_for(graphic) — the placer's
			// per-graphic NovaObjectData cache (the render path loads the same object).
			Ref<NovaObjectData> data = p_placer->call("object_data_for", graphic);
			if (data.is_valid()) {
				opennova::world::CollisionModel model;
				if (collision_model_from_ir(
						data->native_ir().collision, model, data->has_collision())) {
					model_id = collision_world_.add_model(std::move(model));
					if (data->has_live_panm_for_lod(0))
						collision_pose_data_[model_id] = data;
				}
				opennova::world::OcclusionModel occ;
				if (occlusion_model_from_ir(data->native_ir().occlusion, occ))
					occlusion_id = occlusion_world_.add_model(std::move(occ));
				bound_radius = model_bound_radius_from_ir(data->native_ir());
			}
			it = collision_model_by_graphic_.emplace(key, model_id).first;
			collision_occlusion_by_graphic_.emplace(key, occlusion_id);
			collision_radius_by_graphic_.emplace(key, bound_radius);
		}
		// The bound-sphere radius (entity+0 boundRadius) comes from the .3di
		// MODEL header bound, not the collision block — every placed item
		// carries one, so collision-less props are still hittable by rounds and
		// reachable by blasts. Raised to the husk model's bound below, then
		// padded +0.0625 [orig: Entity_InitFromModel @ 0x40dc30 — boundRadius =
		// max(gpm[5], husk gpm[5]) + 0x1000; the authored def scale factor is
		// not yet applied (tracked, D-COL-3)].
		float entity_bound = collision_radius_by_graphic_[key];
		if (it->second >= 0) {
			collision_world_.assign_entity(
					h, it->second, e->registry_spawn_id);
			++attached;
			if (is_organic) {
				collision_skeletal_sources_.erase(h.packed);
				Ref<NovaSkeletalAnim> skeletal =
						p_placer->call("skeletal_anim_for", def_id, graphic);
				const opennova::world::CollisionModel *person_model =
						collision_world_.model(it->second);
				if (skeletal.is_valid() && skeletal->is_loaded() &&
						person_model != nullptr) {
					const Array bones = skeletal->get_skeleton_bones();
					const size_t section_count = person_model->sections.size();
					if (bones.size() >= static_cast<int64_t>(section_count)) {
						SkeletalCollisionSource source;
						source.model_id = it->second;
						source.registry_spawn_id = e->registry_spawn_id;
						source.anim = skeletal;
						source.overlay_classes =
								skeletal->get_overlay_classes();
						source.parents.resize(section_count, -1);
						source.rest_global.resize(section_count);
						bool valid_rig =
								source.overlay_classes.size() >=
								static_cast<int64_t>(section_count);
						for (size_t i = 0; valid_rig && i < section_count; ++i) {
							const Variant bone_value =
									bones[static_cast<int64_t>(i)];
							if (bone_value.get_type() != Variant::DICTIONARY) {
								valid_rig = false;
								break;
							}
							const Dictionary bone = bone_value;
							const int32_t parent =
									static_cast<int32_t>(bone.get(
											"parent_index", -1));
							const Variant rest_value =
									bone.get("rest", Transform3D());
							if (parent < -1 ||
									parent >= static_cast<int32_t>(i) ||
									rest_value.get_type() != Variant::TRANSFORM3D) {
								valid_rig = false;
								break;
							}
							const Transform3D rest =
									static_cast<Transform3D>(rest_value);
							source.parents[i] = parent;
							source.rest_global[i] = parent >= 0
									? source.rest_global[
											static_cast<size_t>(parent)] * rest
									: rest;
						}
						if (valid_rig)
							collision_skeletal_sources_[h.packed] =
									std::move(source);
					}
				}
			}
		}
		// The husk-stage collision model: attached beside the graphic instance so
		// every query swaps to the wreck once Flags & 4 sets. The collision pick
		// is the FIRST husk stage (entity+52 huskModel), not huskFinal [orig: the
		// +52 substitution @ 0x538720 / @ 0x413086; D-AI-7 residual closed].
		const String first_husk_name_s = p_item_db->get_husk(def_id);
		const String final_husk_name_s = p_item_db->get_huskfinal(def_id);
		const String husk_name_s = first_husk_name_s.is_empty()
				? final_husk_name_s
				: first_husk_name_s;
		if (!husk_name_s.is_empty()) {
			// Retail keeps live huskModel and huskFinalModel pointers on the
			// entity. A successfully opened model supplies that pointer even when
			// it has no collision block; missing/corrupt assets leave it null.
			// [orig: Entity_ProcessBuildingDeath @ 0x49442c]
			Ref<NovaObjectData> first_husk_data;
			Ref<NovaObjectData> final_husk_data;
			if (!first_husk_name_s.is_empty())
				first_husk_data = p_placer->call("object_data_for", first_husk_name_s);
			if (!final_husk_name_s.is_empty())
				final_husk_data = p_placer->call("object_data_for", final_husk_name_s);
			if (opennova::world::ItemDeathTraits *t =
						world_->item_death_traits.get_mutable(e->item_id))
				t->husk_model_loaded =
						first_husk_data.is_valid() || final_husk_data.is_valid();
			const std::string husk_key(husk_name_s.utf8().get_data());
			Ref<NovaObjectData> husk_data = first_husk_name_s.is_empty()
					? final_husk_data
					: first_husk_data;
			auto hit = collision_model_by_graphic_.find(husk_key);
			if (hit == collision_model_by_graphic_.end()) {
				int32_t husk_model_id = -1;
				if (husk_data.is_valid()) {
					opennova::world::CollisionModel hmodel;
					if (collision_model_from_ir(
							husk_data->native_ir().collision, hmodel,
							husk_data->has_collision())) {
						husk_model_id = collision_world_.add_model(std::move(hmodel));
						if (husk_data->has_live_panm_for_lod(0))
							collision_pose_data_[husk_model_id] = husk_data;
					}
					collision_radius_by_graphic_.emplace(husk_key,
							model_bound_radius_from_ir(husk_data->native_ir()));
				}
				hit = collision_model_by_graphic_.emplace(
						husk_key, husk_model_id).first;
				collision_occlusion_by_graphic_.emplace(husk_key, -1);
			}
			if (hit->second >= 0 && it->second >= 0)
				collision_world_.assign_entity_husk(h, hit->second);
			// Retail's death-sound tail walks exact, case-insensitive "KZ"
			// user points on the active FIRST husk, not the huskFinal piece
			// model, and queues a radius-5 blast at every match. Cache this
			// metadata separately from collision registration: the same graphic
			// may already be resident as another entity's main model.
			// Unlike collision's legacy final-only fallback, the retail KZ walker
			// reads entity+52 huskModel. A def with only huskFinal has no KZ source
			// and therefore takes the entity-origin fallback blast.
			if (!first_husk_name_s.is_empty()) {
				auto kz_it = collision_husk_kz_points_by_graphic_.find(husk_key);
				if (kz_it == collision_husk_kz_points_by_graphic_.end()) {
					std::vector<opennova::world::Vec3> kz_points;
					if (husk_data.is_valid()) {
						const ThreediModelIR &ir = husk_data->native_ir();
						for (size_t up_index = 0;
								ir.userpoints != nullptr && up_index < ir.userpoint_count;
								++up_index) {
							const ThreediIRUserPoint &point = ir.userpoints[up_index];
							if (String::utf8(point.name).nocasecmp_to("KZ") != 0)
								continue;
							// IR is (-source y, source z, source x); destruction's
							// placement math consumes mission-local (x, y, z).
							kz_points.push_back(opennova::world::Vec3{
									point.position[2],
									-point.position[0],
									point.position[1]});
						}
					}
					kz_it = collision_husk_kz_points_by_graphic_.emplace(
							husk_key, std::move(kz_points)).first;
				}
				if (opennova::world::ItemDeathTraits *t =
							world_->item_death_traits.get_mutable(e->item_id);
						t != nullptr && t->kz_points.empty() && !kz_it->second.empty())
					t->kz_points = kz_it->second;
			}
			// The PIECE model is huskFINAL first [orig: @ 0x4934af
			// huskFinalModel ?: huskModel] — the opposite preference from the
			// collision husk pick above. Its LOD-0 part table feeds the
			// death-piece loop bound [orig: renderObj[8]+52 @ 0x49361a], the
			// per-section centers [orig: the section-row center @ 0x4938bf],
			// and section 0's z extents (the wreck ground-rest offset
			// [orig: @ 0x461e23-0x461e4b]). Its own cache, independent of the
			// collision cache: a husk graphic can double as some entity's main
			// graphic, which would leave the joint cache without an entry.
			const String piece_name_s = final_husk_name_s.is_empty()
					? first_husk_name_s
					: final_husk_name_s;
			const std::string piece_key(piece_name_s.utf8().get_data());
			auto hs = collision_husk_pieces_by_graphic_.find(piece_key);
			if (hs == collision_husk_pieces_by_graphic_.end()) {
				CollisionHuskPieceInfo info;
				Ref<NovaObjectData> hdata = final_husk_name_s.is_empty()
						? first_husk_data
						: final_husk_data;
				if (hdata.is_valid() && hdata->native_ir().lod_count > 0 &&
				    hdata->native_ir().lods != nullptr) {
					const ThreediIRLod &lod = hdata->native_ir().lods[0];
					info.sections = static_cast<int32_t>(lod.part_count);
					for (size_t pi = 0; lod.parts != nullptr && pi < lod.part_count;
							++pi) {
						const ThreediIRPart &part = lod.parts[pi];
						info.centers.push_back(opennova::world::Vec3{
								part.abs_position[0] + part.bounding_center[0],
								part.abs_position[1] + part.bounding_center[1],
								part.abs_position[2] + part.bounding_center[2]});
					}
					if (lod.parts != nullptr && lod.part_count > 0 &&
					    lod.primitives != nullptr) {
						const ThreediIRPart &p0 = lod.parts[0];
						bool any = false;
						for (int32_t pr = 0; pr < p0.primitive_count; ++pr) {
							const size_t idx =
									static_cast<size_t>(p0.primitive_start) + pr;
							if (idx >= lod.primitive_count) break;
							const ThreediIRPrimitive &prim = lod.primitives[idx];
							info.rest_min_z =
									any ? std::min(info.rest_min_z, prim.min[2])
									    : prim.min[2];
							info.rest_max_z =
									any ? std::max(info.rest_max_z, prim.max[2])
									    : prim.max[2];
							any = true;
						}
					}
				}
				hs = collision_husk_pieces_by_graphic_.emplace(
						piece_key, std::move(info)).first;
				if (hdata.is_valid())
					collision_radius_by_graphic_.emplace(piece_key,
							model_bound_radius_from_ir(hdata->native_ir()));
			}
			if (opennova::world::ItemDeathTraits *t =
						world_->item_death_traits.get_mutable(e->item_id)) {
				const CollisionHuskPieceInfo &info = hs->second;
				if (t->husk_section_count == 0 && info.sections > 0)
					t->husk_section_count = info.sections;
				if (t->husk_section_centers.empty() && !info.centers.empty())
					t->husk_section_centers = info.centers;
				t->husk_rest_min_z = info.rest_min_z;
				t->husk_rest_max_z = info.rest_max_z;
			}
			// The husk model's bound joins the entity bound max [orig:
			// Entity_InitFromModel @ 0x40dc30, the huskModel[5] compare].
			entity_bound = std::max(
					entity_bound, collision_radius_by_graphic_[husk_key]);
		}
		if (entity_bound > 0.0f && e->bound_radius <= 0.0f)
			e->bound_radius = entity_bound + 0.0625f;  // the +0x1000 16.16 pad
		const int32_t occ_id = collision_occlusion_by_graphic_[key];
		if (occ_id >= 0 && e->kind == opennova::world::EntityKind::Building) {
			// The def bits the occlusion engine reads: attrib2 bit 6 "weldable"
			// [orig: itemDef+88 >> 6 @ 0x5c5cce], attrib bit 27 recurse-windows
			// [orig: itemDef+84 >> 27 @ 0x5c7456]; the destruction bone-map
			// bytes (+2193/+2194) stay 0 until the destruction system lands
			// (D-COL-2 / D-OCC-9).
			opennova::world::OcclusionWorld::EntityDefBits bits;
			bits.weldable = (p_item_db->get_attrib2(def_id) & (1u << 6)) != 0;
			bits.recurse_windows = (p_item_db->get_attrib(def_id) & (1u << 27)) != 0;
			occlusion_world_.assign_entity(h, occ_id, bits);
		}
	}
	return attached;
}

void NovaSimulation::occlusion_init_mission() {
	// [orig: Terrain_InitBuildingPortals @ 0x5c7480 from Game_StartMission
	// @ 0x525e11 — runs over the static prox tables, so make sure they exist
	// before the register pass walks the building prefix.]
	if (!world_) return;
	collision_world_.build_tick_tables(*world_);
	occlusion_world_.init_mission(*world_, collision_world_);
}

void NovaSimulation::run_occlusion_frame(const Transform3D &p_camera, double p_fov_y_deg,
                                         double p_aspect, double p_near,
                                         double p_fog_dist_units, double p_water_z_units,
                                         bool p_force_indoors) {
	if (!world_) return;
	using opennova::world::to_fixed;
	opennova::world::OcclusionFrameCamera cam;

	// Godot world (x, up, z) -> mission fixed (x, -z, up) 16.16.
	const Vector3 gp = p_camera.origin;
	cam.pos_fixed[0] = to_fixed(gp.x);
	cam.pos_fixed[1] = to_fixed(-gp.z);
	cam.pos_fixed[2] = to_fixed(gp.y);
	opennova::world::render_float_from_fixed(cam.pos_fixed, cam.pos_float);

	// Camera axes. Godot camera looks -Z; render float = Godot with X/Z swapped
	// ((-my, mz, mx)/65536 == (gz, gy, gx)); mission dirs = (x, -z, y) of Godot.
	const Vector3 fwd_g = -p_camera.basis.get_column(2).normalized();
	const Vector3 right_g = p_camera.basis.get_column(0).normalized();
	const Vector3 up_g = p_camera.basis.get_column(1).normalized();
	auto render_dir = [](const Vector3 &v) {
		return Vector3(v.z, v.y, v.x);
	};
	const Vector3 f = render_dir(fwd_g);
	const Vector3 r = render_dir(right_g);
	const Vector3 u = render_dir(up_g);
	const Vector3 c(cam.pos_float[0], cam.pos_float[1], cam.pos_float[2]);

	// The 5-plane view frustum (near + 4 sides), inward normals, in render
	// float space — the host stand-in for the retail viewport projector
	// [orig: g_CameraFrustumPlanes5 @ 0xA7849C; D-OCC-12].
	const double half_v = Math::deg_to_rad(p_fov_y_deg) * 0.5;
	const double tan_v = std::tan(half_v);
	const double tan_h = tan_v * (p_aspect > 0.0 ? p_aspect : 1.0);
	Vector3 normals[5];
	normals[0] = f;
	normals[1] = (f * static_cast<real_t>(tan_h) + r).normalized();  // left
	normals[2] = (f * static_cast<real_t>(tan_h) - r).normalized();  // right
	normals[3] = (f * static_cast<real_t>(tan_v) + u).normalized();  // bottom
	normals[4] = (f * static_cast<real_t>(tan_v) - u).normalized();  // top
	cam.frustum_count = 5;
	for (int i = 0; i < 5; ++i) {
		const Vector3 anchor = (i == 0) ? c + f * static_cast<real_t>(p_near) : c;
		cam.frustum[i][0] = normals[i].x;
		cam.frustum[i][1] = normals[i].y;
		cam.frustum[i][2] = normals[i].z;
		cam.frustum[i][3] = -normals[i].dot(anchor);
	}

	// World->view rotation rows (mission axes, Q22): row 0 = forward (the depth
	// cull axis), rows 1/2 = the lateral axes the three-ray probe offsets along.
	// [orig: the fixed view matrix @ 0xA7841C]
	auto mission_dir_q22 = [](const Vector3 &v, int32_t out[3]) {
		out[0] = static_cast<int32_t>(std::lround(v.x * 4194304.0));
		out[1] = static_cast<int32_t>(std::lround(-v.z * 4194304.0));
		out[2] = static_cast<int32_t>(std::lround(v.y * 4194304.0));
	};
	mission_dir_q22(fwd_g, cam.view_rows_q22[0]);
	mission_dir_q22(right_g, cam.view_rows_q22[1]);
	mission_dir_q22(up_g, cam.view_rows_q22[2]);

	cam.fog_dist = to_fixed(p_fog_dist_units);
	cam.water_z = to_fixed(p_water_z_units);
	// The mission-attribute force-indoors override ORs the indoors bit into the
	// frame's accum view. [orig: Bms_AttribFlags & 0x10 @ 0x5ca1c8 -> |= 2]
	cam.local_blink_flags =
			collision_world_.local_player_blink_flags | (p_force_indoors ? 0x2u : 0u);

	occlusion_world_.build_frame(*world_, collision_world_, cam);

	// The entity collectors' render gates over the non-building entities the
	// host draws. [orig: Terrain_CollectVisibleEntities_0 @ 0x5c6f20 /
	// collect_visible_entities_for_terrain @ 0x5c8c60]
	occlusion_culled_bms_.clear();
	std::vector<opennova::world::EntityHandle> handles;
	world_->registry.for_each([&](const opennova::world::Entity &e) {
		if (e.kind == opennova::world::EntityKind::Building ||
		    e.kind == opennova::world::EntityKind::Marker)
			return;
		if (e.bms_id == 0) return; // wire avatars ride their own present path
		handles.push_back(e.handle);
	});
	for (const opennova::world::EntityHandle h : handles) {
		opennova::world::Entity *e = world_->registry.get(h);
		if (e == nullptr) continue;
		if (!occlusion_world_.entity_render_visible(*world_, collision_world_, *e, cam))
			occlusion_culled_bms_.push_back(e->bms_id);
	}
}

PackedInt64Array NovaSimulation::get_building_visibility() const {
	PackedInt64Array out;
	if (!world_) return out;
	// Pairs [bms_id, visible<<32 | mask] for every building with an OCCLUSION
	// instance, plus collision-backed de-batched buildings that still entered
	// the retail building batch. OOBJ instances apply their section mask.
	// Without OOBJ there is no safe host part-to-section map, so those buildings
	// keep all render parts while still receiving batch/frustum/TOC visibility.
	world_->registry.for_each([&](const opennova::world::Entity &e) {
		if (e.kind != opennova::world::EntityKind::Building || e.bms_id == 0) return;
		const bool has_occlusion = occlusion_world_.has_instance(e.handle);
		if (!has_occlusion &&
				collision_world_.model_for(*world_, e.handle) == nullptr)
			return;
		const bool visible = occlusion_world_.building_visible(e.handle);
		const uint32_t mask =
		    has_occlusion ? occlusion_world_.section_mask(e.handle) : 0xFFFFFFFFu;
		out.push_back(e.bms_id);
		out.push_back(static_cast<int64_t>(mask) | (visible ? (int64_t(1) << 32) : 0));
	});
	return out;
}

PackedInt32Array NovaSimulation::get_render_culled_bms_ids() const {
	PackedInt32Array out;
	for (const int32_t id : occlusion_culled_bms_) out.push_back(id);
	return out;
}

bool NovaSimulation::occlusion_water_visible() const {
	return occlusion_world_.water_visible();
}

bool NovaSimulation::occlusion_camera_indoors() const {
	return occlusion_world_.camera_indoors();
}

bool NovaSimulation::local_player_indoors() const {
	if (!world_) return false;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e != nullptr && (e->flags & opennova::world::kEntityFlagIndoors) != 0;
}

int NovaSimulation::local_player_blink_flags() const {
	return static_cast<int>(collision_world_.local_player_blink_flags);
}

int64_t NovaSimulation::sound_occlusion_distance_q16(const Vector3 &listener_pos,
                                                     const Vector3 &source_pos,
                                                     int64_t distance_q16,
                                                     int source_bms_id) {
	// [orig: Sound_ApplyOcclusionDistance @ 0x529970] — the audio layer feeds
	// the AUDIO listener (camera), emitter/one-shot position, and source
	// identity when known. -1 denotes the local player, positive values are
	// authored BMS ids, and zero keeps the no-entity path. Godot world
	// (x, up, z) -> mission fixed (x, -z, up) 16.16.
	if (!world_) return distance_q16;
	const int32_t lp[3] = {opennova::world::to_fixed(listener_pos.x),
	                       opennova::world::to_fixed(-listener_pos.z),
	                       opennova::world::to_fixed(listener_pos.y)};
	const int32_t sp[3] = {opennova::world::to_fixed(source_pos.x),
	                       opennova::world::to_fixed(-source_pos.z),
	                       opennova::world::to_fixed(source_pos.y)};
	opennova::world::EntityHandle source;
	if (source_bms_id < 0) {
		source = world_->cached.local_player;
	} else if (source_bms_id > 0) {
		world_->registry.for_each([&](const opennova::world::Entity &e) {
			if (!source.valid() && e.bms_id == source_bms_id) source = e.handle;
		});
	}
	// Static/env emitters do not ride the moving-entity collision resolver.
	// Refresh their blink/indoors state at the audio query boundary so the
	// both-indoors terrain bypass sees the source state retail registered.
	if (source.valid() && source != world_->cached.local_player) {
		if (opennova::world::Entity *source_entity = world_->registry.get(source))
			collision_world_.refresh_blink(*world_, *source_entity);
	}
	return collision_world_.sound_occlusion_inflate(*world_, world_->cached.local_player,
	                                                source, lp, sp,
	                                                static_cast<int32_t>(distance_q16));
}

PackedInt32Array NovaSimulation::compute_iris_samples(const Vector3 &p_cam_pos,
                                                      const Vector3 &p_cam_forward,
                                                      const Vector3 &p_light_dir) {
	PackedInt32Array out;
	if (!world_) return out;

	// Godot world (x, up, z) -> mission fixed (x, -z, up) 16.16.
	const int32_t cam[3] = {opennova::world::to_fixed(p_cam_pos.x),
	                        opennova::world::to_fixed(-p_cam_pos.z),
	                        opennova::world::to_fixed(p_cam_pos.y)};
	// end = camera + forward * 8.0 [orig: the (0x80000, 0, 0) forward vector
	// rotated through the camera matrix @ 0x5c7a56..0x5c7a6f].
	int32_t end[3] = {cam[0] + opennova::world::to_fixed(p_cam_forward.x * 8.0f),
	                  cam[1] + opennova::world::to_fixed(-p_cam_forward.z * 8.0f),
	                  cam[2] + opennova::world::to_fixed(p_cam_forward.y * 8.0f)};

	// Terrain clip of the camera ray [orig: raycast_entity_collision @ 0x413760
	// -> Terrain_RaycastHeightmapHiRes_0 @ 0x60e710, end clipped in place; the
	// entity nearest-hit clip is a tracked D-RLIT-2 residual].
	if (terrain_field_.valid()) {
		int32_t hit[3];
		if (opennova::world::terrain_clip_segment(terrain_field_, cam, end, hit)) {
			end[0] = hit[0];
			end[1] = hit[1];
			end[2] = hit[2];
		}
	}

	// Sun-ray direction in mission fixed: light_dir * 200 u
	// [orig: end = sample + 200 * light_dir @ 0x5c776c..0x5c7780].
	const int32_t sun[3] = {opennova::world::to_fixed(p_light_dir.x * 200.0f),
	                        opennova::world::to_fixed(-p_light_dir.z * 200.0f),
	                        opennova::world::to_fixed(p_light_dir.y * 200.0f)};
	// The three ray clip radii [orig: the -0x2000/-0x5000/-0x8000 pushes
	// @ 0x5c7767/0x5c7792/0x5c77ac].
	static const int32_t kSunRayRadii[3] = {-0x2000, -0x5000, -0x8000};

	// Samples at end, end + (cam-end)/3, end + 2(cam-end)/3 [orig: the thirds
	// march @ 0x5c7ad8..0x5c7b30].
	const int32_t step[3] = {(cam[0] - end[0]) / 3, (cam[1] - end[1]) / 3,
	                         (cam[2] - end[2]) / 3};
	for (int s = 0; s < 3; ++s) {
		const int32_t p[3] = {end[0] + step[0] * s, end[1] + step[1] * s, end[2] + step[2] * s};
		opennova::world::BlinkAccum blink;
		collision_world_.query_blink_boxes_at_point(*world_, p, blink);
		if (blink.hits[0] != 0) {
			// Indoor sample: the hit's pool-2 entity carries interior data or
			// the curve runs on all-zero inputs (gain 255)
			// [orig: Pool_GetEntryUnchecked(2, hit >> 20) @ 0x5c7646; the
			//  pool_entry[12] == 0 skip @ 0x5c7652].
			const opennova::world::EntityHandle h = opennova::world::EntityHandle::make(
					2, static_cast<int32_t>(blink.hits[0] >> 20));
			out.append(occlusion_world_.has_instance(h)
							? NovaWeatherCore::kIrisSampleIndoor
							: NovaWeatherCore::kIrisSampleIndoorNoData);
			continue;
		}
		// Outdoor sample: level = 8 minus one per blocked sun ray
		// [orig: @ 0x5c7784..0x5c77d7; the player-sector entity-count ray gate
		//  is a tracked D-RLIT-2 residual — with no statics the rays cannot hit].
		int32_t level = 8;
		const int32_t ray_end[3] = {p[0] + sun[0], p[1] + sun[1], p[2] + sun[2]};
		for (int r = 0; r < 3; ++r) {
			if (collision_world_.segment_hits_static(*world_, p, ray_end, kSunRayRadii[r]))
				--level;
		}
		out.append(level);
	}
	return out;
}

namespace {
// Mission-space 16.16 triple -> Godot world space: (x, y, z) -> (x, z, -y) units.
inline Vector3 godot_from_fixed3(const int32_t p[3]) {
	return Vector3(static_cast<float>(p[0] / 65536.0), static_cast<float>(p[2] / 65536.0),
	               static_cast<float>(-p[1] / 65536.0));
}
} // namespace

Dictionary NovaSimulation::get_collision_debug() const {
	Dictionary out;
	Array instances;
	Dictionary player;
	player["valid"] = false;
	out["instances"] = instances;
	out["player"] = player;
	if (!world_) return out;

	// Anchor the sweep on the local player when one is spawned (150u box, the
	// resolver's own neighborhood scale); an editor preview with no player sweeps
	// the whole table up to the instance cap.
	int32_t anchor[3] = {0, 0, 0};
	int32_t range = -1;
	const opennova::world::Entity *lp =
	    world_->cached.local_player.valid() ? world_->registry.get(world_->cached.local_player)
	                                        : nullptr;
	if (lp != nullptr) {
		anchor[0] = opennova::world::to_fixed(lp->position.x);
		anchor[1] = opennova::world::to_fixed(lp->position.y);
		anchor[2] = opennova::world::to_fixed(lp->position.z);
		range = 150 << 16;
	}
	const std::vector<opennova::world::CollisionWorld::DebugInstance> insts =
	    collision_world_.debug_instances(*world_, anchor, range, 128);
	for (const opennova::world::CollisionWorld::DebugInstance &inst : insts) {
		Dictionary d;
		d["entity_handle"] = static_cast<int>(inst.handle.packed);
		d["pos"] = godot_from_fixed3(inst.pos);
		d["heading"] = static_cast<float>(
		    opennova::world::mission_yaw_deg_from_bam_heading(inst.heading_bam));
		Array vols;
		for (const opennova::world::CollisionWorld::DebugVolume &v : inst.volumes) {
			Dictionary vd;
			vd["type"] = v.type;
			vd["min_x"] = static_cast<float>(v.min[0] / kFixed16);
			vd["max_x"] = static_cast<float>(v.max[0] / kFixed16);
			vd["min_y"] = static_cast<float>(v.min[1] / kFixed16);
			vd["max_y"] = static_cast<float>(v.max[1] / kFixed16);
			vd["min_z"] = static_cast<float>(v.min[2] / kFixed16);
			vd["max_z"] = static_cast<float>(v.max[2] / kFixed16);
			PackedVector3Array corners;
			corners.resize(8);
			Vector3 *cw = corners.ptrw();
			for (int c = 0; c < 8; ++c) cw[c] = godot_from_fixed3(v.corners[c]);
			vd["corners"] = corners;
			vols.push_back(vd);
		}
		d["volumes"] = vols;
		instances.push_back(d);
	}

	// The local player's last full resolve: the capsule test points the resolver
	// queried and the returned foot clearance (CollisionWorld::LocalResolveDebug).
	const opennova::world::CollisionWorld::LocalResolveDebug &lrd =
	    collision_world_.local_resolve_debug;
	if (lrd.valid) {
		player["valid"] = true;
		player["position"] = godot_from_fixed3(lrd.pos);
		PackedVector3Array pts;
		pts.resize(3);
		Vector3 *pw = pts.ptrw();
		PackedFloat32Array radii;
		radii.resize(3);
		float *rw = radii.ptrw();
		for (int i = 0; i < 3; ++i) {
			pw[i] = godot_from_fixed3(lrd.points[i]);
			rw[i] = static_cast<float>(lrd.radii[i] / kFixed16);
		}
		player["points"] = pts;
		player["radii"] = radii;
		player["capsule_bottom"] = static_cast<float>(lrd.capsule_bottom / kFixed16);
		player["capsule_top"] = static_cast<float>(lrd.capsule_top / kFixed16);
		player["foot_clearance"] = static_cast<float>(lrd.foot_clearance / kFixed16);
	}
	return out;
}

namespace {
// Mission float Vec3 -> Godot world space: (x, y, z) -> (x, z, -y).
inline Vector3 godot_from_mission_vec3(const opennova::world::Vec3 &p) {
	return Vector3(p.x, p.z, -p.y);
}
} // namespace

Dictionary NovaSimulation::get_hitbox_debug() {
	constexpr int32_t kEntityCap = 96;
	Dictionary out;
	Array entities;
	Array organics;
	out["entities"] = entities;
	out["organics"] = organics;
	if (!world_) return out;

	// Anchor on the local player like the volume view. All hitbox payloads use
	// the same 80-unit debug budget; a preview with no player sweeps to the caps.
	int32_t anchor[3] = {0, 0, 0};
	int32_t debug_range = -1;
	const opennova::world::EntityHandle local_player =
	    world_->cached.local_player;
	const opennova::world::Entity *lp =
	    local_player.valid() ? world_->registry.get(local_player) : nullptr;
	if (lp != nullptr) {
		anchor[0] = opennova::world::to_fixed(lp->position.x);
		anchor[1] = opennova::world::to_fixed(lp->position.y);
		anchor[2] = opennova::world::to_fixed(lp->position.z);
		debug_range = 80 << 16;
	}
	const std::vector<opennova::world::CollisionWorld::DebugHitboxEntity> ents =
	    collision_world_.debug_hitboxes(*world_, anchor, debug_range, kEntityCap, 24000);
	for (const opennova::world::CollisionWorld::DebugHitboxEntity &ent : ents) {
		Dictionary d;
		d["entity_handle"] = static_cast<int>(ent.handle.packed);
		d["pos"] = godot_from_fixed3(ent.pos);
		d["bound_radius"] = static_cast<float>(ent.bound_radius / kFixed16);
		d["husk"] = ent.husk;
		d["has_faces"] = ent.has_faces;
		d["face_total"] = ent.face_total;
		PackedVector3Array tris;
		PackedByteArray materials;
		PackedInt32Array flags;
		tris.resize(static_cast<int64_t>(ent.faces.size()) * 3);
		materials.resize(static_cast<int64_t>(ent.faces.size()));
		flags.resize(static_cast<int64_t>(ent.faces.size()));
		Vector3 *tw = tris.ptrw();
		uint8_t *mw = materials.ptrw();
		int32_t *fw = flags.ptrw();
		for (size_t i = 0; i < ent.faces.size(); ++i) {
			const opennova::world::CollisionWorld::DebugHitboxFace &f = ent.faces[i];
			for (int k = 0; k < 3; ++k) tw[i * 3 + k] = godot_from_fixed3(f.v[k]);
			mw[i] = f.material;
			fw[i] = static_cast<int32_t>(f.flags);
		}
		d["tris"] = tris;
		d["materials"] = materials;
		d["flags"] = flags;
		entities.push_back(d);
	}

	// Posed pool-0 COBJ spheres from the exact person narrow phase. They share
	// the nearby 80-unit/96-actor debug budget. Preserve F3's late-spawn demand
	// bridge even though the local avatar is presentation-hidden; one spare query
	// slot then prevents its authored rows from consuming the target budget.
	// Entities whose graphic cannot supply usable authored sections are appended
	// below with the bounded compatibility fallback used by RoundSim.
	if (local_player.valid()) ensure_collision_instance(*world_, local_player);
	std::unordered_map<uint16_t, bool> posed_handles;
	const std::vector<opennova::world::CollisionWorld::DebugPersonSection> people =
	    collision_world_.debug_person_sections(
	        *world_, anchor, debug_range, kEntityCap + 1);
	for (const opennova::world::CollisionWorld::DebugPersonSection &person : people) {
		if (person.handle == local_player) continue;
		const bool new_handle =
		    posed_handles.find(person.handle.packed) == posed_handles.end();
		if (new_handle && posed_handles.size() >= static_cast<size_t>(kEntityCap)) break;
		Dictionary d;
		d["entity_handle"] = static_cast<int>(person.handle.packed);
		d["section"] = person.section;
		d["pos"] = godot_from_fixed3(person.center);
		d["radius"] = static_cast<float>(person.radius / kFixed16);
		d["authored_radius"] =
		    static_cast<float>(person.authored_radius / kFixed16);
		d["masked"] = person.masked;
		d["fallback"] = false;
		organics.push_back(d);
		posed_handles[person.handle.packed] = true;
	}
	const size_t pool0 = world_->registry.pool_capacity(0);
	int fallback_entity_count = static_cast<int>(posed_handles.size());
	for (size_t s = 0; s < pool0; ++s) {
		if (fallback_entity_count >= kEntityCap) break;
		const opennova::world::Entity *e =
		    world_->registry.get(opennova::world::EntityHandle{static_cast<uint16_t>(s)});
		if (e == nullptr || e->handle == local_player ||
		    (e->engine_flags & 0x02000001u) != 0 ||
		    posed_handles.find(static_cast<uint16_t>(s)) != posed_handles.end())
			continue;
		if (debug_range >= 0) {
			const int32_t ep[3] = {
			    opennova::world::to_fixed(e->position.x),
			    opennova::world::to_fixed(e->position.y),
			    opennova::world::to_fixed(e->position.z)};
			if (std::llabs(static_cast<int64_t>(ep[0]) - anchor[0]) > debug_range ||
			    std::llabs(static_cast<int64_t>(ep[1]) - anchor[1]) > debug_range ||
			    std::llabs(static_cast<int64_t>(ep[2]) - anchor[2]) > debug_range)
				continue;
		}
		Dictionary d;
		d["entity_handle"] = static_cast<int>(s);
		d["section"] = 1;
		d["pos"] = Vector3(e->position.x,
		                   e->position.z + opennova::world::kOrganicStandInCenterZ,
		                   -e->position.y);
		d["radius"] = opennova::world::kOrganicStandInRadius;
		d["authored_radius"] = opennova::world::kOrganicStandInRadius;
		d["masked"] = false;
		d["fallback"] = true;
		organics.push_back(d);
		++fallback_entity_count;
	}
	return out;
}

int NovaSimulation::debug_spawn_round(const Vector3 &p_from_godot, const Vector3 &p_dir_godot,
                                      const String &p_ammo_name) {
	if (!world_) return -1;
	const int ammo_index = world_->ammo.index_of(p_ammo_name.utf8().get_data());
	if (ammo_index < 0) return -1;
	// Godot world (x, up, z) -> mission (x, -z, up); direction -> the spawn's
	// yaw/pitch BAM (the §5.16 mission bearing: vel = (cos yaw, sin yaw, sin
	// pitch) x speed — round_sim.cpp spawn).
	const double dx = p_dir_godot.x;
	const double dy = -static_cast<double>(p_dir_godot.z);
	const double dz = p_dir_godot.y;
	const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
	if (len <= 0.0) return -1;
	double sz = dz / len;
	if (sz > 1.0) sz = 1.0;
	if (sz < -1.0) sz = -1.0;
	constexpr double kBamPerRad = 4294967296.0 / (2.0 * 3.14159265358979323846);
	opennova::world::RoundSpawnParams params;
	params.owner = world_->cached.local_player;
	params.shooter_handle = params.owner.valid() ? params.owner.packed : 0xFFFF;
	params.ammo_index = ammo_index;
	params.origin = opennova::world::Vec3{p_from_godot.x, -p_from_godot.z, p_from_godot.y};
	params.dir_yaw_bam =
	    static_cast<int32_t>(std::llround(std::atan2(dy, dx) * kBamPerRad));
	params.dir_pitch_bam = static_cast<int32_t>(std::llround(std::asin(sz) * kBamPerRad));
	return world_->round_sim.spawn(*world_, params);
}

Array NovaSimulation::get_throwable_visuals() const {
	Array out;
	if (!world_) return out;
	const double kDegPerBam = opennova::world::kDegreesPerBam;
	auto push_entry = [&](int key, int item_id, const opennova::world::Vec3 &pos,
			int32_t yaw_bam, int32_t pitch_bam, int32_t roll_bam) {
		Dictionary d;
		d["key"] = key;
		d["item_id"] = item_id;
		d["pos"] = Vector3(pos.x, pos.z, -pos.y);
		// the placer euler convention: rotation_deg = (pitch, MISSION yaw, roll)
		d["rotation_deg"] = Vector3(
				static_cast<float>(double(pitch_bam) * kDegPerBam),
				static_cast<float>(
						opennova::world::mission_yaw_deg_from_bam_heading(yaw_bam)),
				static_cast<float>(double(roll_bam) * kDegPerBam));
		out.push_back(d);
	};
	for (int i = 0; i < opennova::world::RoundSim::kCapacity; ++i) {
		const opennova::world::LiveRound &r =
				world_->round_sim.rounds[static_cast<size_t>(i)];
		if (!r.active || !r.tracer || r.item_type_id == 0) continue;
		push_entry(i, r.item_type_id, r.pos, r.yaw_bam, r.pitch_bam, r.roll_bam);
	}
	uint8_t viewer_team = 0xFF;
	if (const opennova::world::Entity *lp =
			world_->registry.get(world_->cached.local_player))
		viewer_team = static_cast<uint8_t>(lp->team);
	for (const opennova::world::PlacedDevice &d : world_->throwables.devices) {
		if (!d.active) continue;
		// Viewer-side team variant with the retail base/friendly fallback when
		// no foe TrcrID is authored [orig: @ 0x5469db..0x546a15].
		const int item = opennova::world::throwable_item_for_viewer(
				d.item_friendly, d.item_enemy, d.team, viewer_team);
		if (item == 0) continue;
		push_entry(0x10000 + d.entity.packed, item, d.pos, d.yaw_bam, d.pitch_bam,
				d.roll_bam);
	}
	return out;
}

Dictionary NovaSimulation::get_round_debug() const {
	Dictionary out;
	Array events;
	out["events"] = events;
	if (!world_) return out;
	const opennova::world::RoundSim &rs = world_->round_sim;
	static const char *const kKindNames[] = {"organic", "item face", "item sphere",
	                                         "terrain",  "expired",   "face miss"};
	// Oldest -> newest so the view can draw newest-last (brightest).
	const int count = rs.debug_trail_count;
	int idx = (rs.debug_trail_next - count + opennova::world::RoundSim::kDebugTrailCap *
	          2) % opennova::world::RoundSim::kDebugTrailCap;
	for (int i = 0; i < count; ++i, idx = (idx + 1) % opennova::world::RoundSim::kDebugTrailCap) {
		const opennova::world::RoundDebugEvent &ev =
		    rs.debug_trail[static_cast<size_t>(idx)];
		Dictionary d;
		d["tick"] = static_cast<int64_t>(ev.tick);
		d["kind"] = static_cast<int>(ev.kind);
		d["kind_name"] = String(ev.kind <= 5 ? kKindNames[ev.kind] : "?");
		d["material"] = static_cast<int>(ev.material);
		d["section"] = static_cast<int>(ev.section);
		d["secondary_section"] = static_cast<int>(ev.secondary_section);
		d["fallback"] = ev.organic_fallback;
		d["face"] = static_cast<int>(ev.face);
		d["effect_tag"] = ev.effect_tag;
		d["effect_tag_name"] =
		    (ev.effect_tag >= 0 && ev.effect_tag < opennova::world::kImpactEffectTagCount)
		        ? String(opennova::world::kImpactEffectTagNames[ev.effect_tag])
		        : String("");
		d["entity_handle"] = static_cast<int>(ev.entity);
		d["shooter_handle"] = static_cast<int>(ev.shooter);
		d["ammo_index"] = ev.ammo_index;
		d["husk"] = ev.husk;
		d["t"] = ev.t;
		d["p0"] = godot_from_mission_vec3(ev.p0);
		d["p1"] = godot_from_mission_vec3(ev.p1);
		d["hit"] = godot_from_mission_vec3(ev.hit);
		// The struck entity's item name when it still resolves (wrecks keep
		// their slot until cleanup) — display sugar for the F3 list.
		String label;
		const opennova::world::Entity *te =
		    world_->registry.get(opennova::world::EntityHandle{ev.entity});
		if (te != nullptr && !te->name.empty())
			label = String(te->name.c_str());
		d["entity_name"] = label;
		events.push_back(d);
	}
	out["tick"] = static_cast<int64_t>(world_->logic_tick);
	return out;
}

namespace {
// Render float world -> Godot world: the render frame is Godot with X/Z
// swapped ((-my, mz, mx)/65536 == (gz, gy, gx)), so the inverse is the same swap.
inline Vector3 godot_from_render_float3(const float p[3]) {
	return Vector3(p[2], p[1], p[0]);
}
} // namespace

Dictionary NovaSimulation::get_occlusion_debug() const {
	Dictionary out;
	Array buildings;
	Array welds;
	Dictionary counts;
	out["active"] = false;
	out["camera_indoors"] = occlusion_world_.camera_indoors();
	out["exterior_visible"] = occlusion_world_.exterior_visible();
	out["water_visible"] = occlusion_world_.water_visible();
	out["local_blink_flags"] = static_cast<int>(collision_world_.local_player_blink_flags);
	out["counts"] = counts;
	out["buildings"] = buildings;
	out["welds"] = welds;
	if (!world_) return out;

	int instances = 0, batched = 0, visible = 0;
	world_->registry.for_each([&](const opennova::world::Entity &e) {
		if (e.kind != opennova::world::EntityKind::Building) return;
		if (!occlusion_world_.has_instance(e.handle)) return;
		++instances;
		const bool is_batched = occlusion_world_.building_batched(e.handle);
		const bool is_visible = occlusion_world_.building_visible(e.handle);
		if (is_batched) ++batched;
		if (is_visible) ++visible;
		if (buildings.size() >= 256) return;
		Dictionary b;
		b["bms_id"] = e.bms_id;
		const int32_t pos_fixed[3] = {opennova::world::to_fixed(e.position.x),
		                              opennova::world::to_fixed(e.position.y),
		                              opennova::world::to_fixed(e.position.z)};
		b["pos"] = godot_from_fixed3(pos_fixed);
		b["batched"] = is_batched;
		b["visible"] = is_visible;
		b["open_flagged"] = occlusion_world_.building_open_flagged(e.handle);
		b["mask"] = static_cast<int64_t>(occlusion_world_.section_mask(e.handle));
		const opennova::world::OcclusionWorld::BuildingFlags flags =
		    occlusion_world_.building_flags(e.handle);
		b["has_open"] = flags.has_open;
		b["has_windows"] = flags.has_windows;
		b["has_links"] = flags.has_links;
		// Record-type census off the (possibly weld-retyped) shared model.
		int windows = 0, portals = 0, links = 0, records = 0;
		const opennova::world::OcclusionModel *m =
		    occlusion_world_.model(occlusion_world_.instance_model_id(e.handle));
		if (m != nullptr) {
			records = static_cast<int>(m->records.size());
			for (const opennova::world::OcclusionPortalFace &rec : m->records) {
				if (rec.type == opennova::world::kOccRecWindow)
					++windows;
				else if (rec.type == opennova::world::kOccRecPortal)
					++portals;
				else if (rec.type == opennova::world::kOccRecWeldedLink)
					++links;
			}
		}
		b["records"] = records;
		b["windows"] = windows;
		b["portals"] = portals;
		b["links"] = links;
		buildings.push_back(b);
	});

	for (const opennova::world::OcclusionWorld::WeldRecord &wr : occlusion_world_.weld_records()) {
		if (welds.size() >= 64) break;
		Dictionary w;
		const opennova::world::Entity *own = world_->registry.get(wr.own_entity);
		const opennova::world::Entity *other = world_->registry.get(wr.other_entity);
		w["own_bms"] = own != nullptr ? own->bms_id : 0;
		w["own_section"] = wr.own_section;
		w["other_bms"] = other != nullptr ? other->bms_id : 0;
		w["other_section"] = wr.other_section;
		welds.push_back(w);
	}

	counts["instances"] = instances;
	counts["batched"] = batched;
	counts["visible"] = visible;
	counts["toc_culled"] = batched - visible;
	counts["slots"] = occlusion_world_.slot_count();
	counts["window_groups"] = occlusion_world_.window_frustum_group_count();
	counts["viewthru_groups"] = occlusion_world_.viewthru_group_count();
	counts["welds"] = static_cast<int>(occlusion_world_.weld_records().size());
	counts["culled_entities"] = static_cast<int>(occlusion_culled_bms_.size());
	out["active"] = instances > 0;
	return out;
}

Dictionary NovaSimulation::get_occlusion_portal_debug(const Vector3 &p_anchor,
                                                      double p_range_units) const {
	Dictionary out;
	Array buildings;
	out["buildings"] = buildings;
	if (!world_) return out;
	// Godot world (x, up, z) -> mission fixed (x, -z, up) 16.16.
	const int64_t anchor_x = opennova::world::to_fixed(p_anchor.x);
	const int64_t anchor_y = opennova::world::to_fixed(-p_anchor.z);
	const int64_t range =
	    p_range_units > 0.0 ? static_cast<int64_t>(p_range_units * kFixed16) : -1;
	world_->registry.for_each([&](const opennova::world::Entity &e) {
		if (e.kind != opennova::world::EntityKind::Building) return;
		if (buildings.size() >= 128) return;
		const opennova::world::OcclusionModel *m =
		    occlusion_world_.model(occlusion_world_.instance_model_id(e.handle));
		if (m == nullptr) return;
		const int32_t pos_fixed[3] = {opennova::world::to_fixed(e.position.x),
		                              opennova::world::to_fixed(e.position.y),
		                              opennova::world::to_fixed(e.position.z)};
		if (range >= 0 && (std::abs(pos_fixed[0] - anchor_x) > range ||
		                   std::abs(pos_fixed[1] - anchor_y) > range))
			return;
		// The same full authored building-pose path the engine's frame uses.
		const opennova::world::RenderMatrix mat =
		    opennova::world::render_matrix_from_entity_pose(e);
		Dictionary b;
		b["bms_id"] = e.bms_id;
		b["pos"] = godot_from_fixed3(pos_fixed);
		b["visible"] = occlusion_world_.building_visible(e.handle);
		Array records;
		for (const opennova::world::OcclusionPortalFace &rec : m->records) {
			Dictionary rd;
			rd["type"] = static_cast<int>(rec.type);
			rd["section_a"] = static_cast<int>(rec.section_a);
			rd["section_b"] = static_cast<int>(rec.section_b);
			float wp[3];
			mat.transform_point(rec.pos, wp);
			rd["pos"] = godot_from_render_float3(wp);
			rd["radius"] = rec.radius;
			rd["glow"] = rec.glow_scale;
			// The record's boundary outline: OFAC edge words whose low-15-bit
			// identity appears once (shared interior edges pair up and drop —
			// the same cancellation identity the occluder pass uses).
			std::vector<uint16_t> edges;
			std::vector<int32_t> hits;
			for (int32_t f = 0; f < rec.face_count; ++f) {
				const opennova::world::OcclusionFaceRec &face = m->faces[rec.face_start + f];
				for (int k = 0; k < 3; ++k) {
					const uint16_t w = face.edge[k];
					bool found = false;
					for (size_t x = 0; x < edges.size(); ++x) {
						if ((edges[x] & 0x7FFF) == (w & 0x7FFF)) {
							++hits[x];
							found = true;
							break;
						}
					}
					if (!found) {
						edges.push_back(w);
						hits.push_back(1);
					}
				}
			}
			PackedVector3Array segments;
			for (size_t x = 0; x < edges.size(); ++x) {
				if (hits[x] != 1) continue;
				const int32_t va = edges[x] & 0xFF;
				const int32_t vb = (edges[x] >> 8) & 0x7F;
				if (va >= rec.vert_count || vb >= rec.vert_count) continue;
				float aw[3], bw[3];
				mat.transform_point(m->vertices[rec.vert_start + va].p, aw);
				mat.transform_point(m->vertices[rec.vert_start + vb].p, bw);
				segments.push_back(godot_from_render_float3(aw));
				segments.push_back(godot_from_render_float3(bw));
			}
			rd["segments"] = segments;
			records.push_back(rd);
		}
		b["records"] = records;
		buildings.push_back(b);
	});
	return out;
}

bool NovaSimulation::local_player_in_armory_zone() const {
	if (!world_) return false;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	// The use-item armory leg rejects a seated player before consulting the type-6
	// volume bit [orig: Input_HandleActionBinding_0 @0x4e0b3f, parentSlot == 0].
	return e != nullptr && !e->mounted &&
	       (e->flags & opennova::world::kEntityFlagArmoryZone) != 0;
}

bool NovaSimulation::local_player_in_vehicle_loadout_zone() const {
	if (!world_) return false;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e != nullptr &&
	       (e->flags & opennova::world::kEntityFlagVehicleLoadoutZone) != 0;
}

opennova::world::WeaponSlotState *NovaSimulation::active_local_weapon_slot() {
	if (local_usegun_slot_active_ && world_ && local_usegun_mount_.valid()) {
		opennova::world::Entity *mount =
				world_->registry.get(local_usegun_mount_);
		if (mount != nullptr &&
				mount->primary_weapon_slot_adm == local_usegun_weapon_adm_)
			return &mount->primary_weapon_slot;
	}
	return &weapon_slot_;
}

const opennova::world::WeaponSlotState *
NovaSimulation::active_local_weapon_slot() const {
	if (local_usegun_slot_active_ && world_ && local_usegun_mount_.valid()) {
		const opennova::world::Entity *mount =
				world_->registry.get(local_usegun_mount_);
		if (mount != nullptr &&
				mount->primary_weapon_slot_adm == local_usegun_weapon_adm_)
			return &mount->primary_weapon_slot;
	}
	return &weapon_slot_;
}

bool NovaSimulation::local_usegun_switch_is_instant() const {
	if (!world_ ||
			local_usegun_switch_action_ !=
				opennova::world::weapon_action::kSwitchFrom)
		return false;
	const uint8_t from_adm = local_usegun_slot_active_
			? local_usegun_weapon_adm_
			: local_usegun_saved_adm_;
	const uint8_t to_adm =
			(local_usegun_switch_ == LocalUseGunSwitch::kAttach ||
			 local_usegun_switch_ == LocalUseGunSwitch::kSwap)
			? local_usegun_pending_weapon_adm_
			: local_usegun_saved_adm_;
	const opennova::world::WeaponTableEntry *from =
			world_->weapons.by_index(from_adm);
	const opennova::world::WeaponTableEntry *to =
			world_->weapons.by_index(to_adm);
	const int32_t flags = (from != nullptr ? from->flags : 0) |
			(to != nullptr ? to->flags : 0);
	return (flags & opennova::world::weapon_flag::kEmplaced) != 0;
}

void NovaSimulation::queue_local_usegun_weapon_switch(bool p_same_category) {
	local_usegun_switch_action_ = p_same_category
			? opennova::world::weapon_action::kSwitchRank
			: opennova::world::weapon_action::kSwitchFrom;
	weapon_switch_deferred_action_ = local_usegun_switch_action_;
	if (!weapon_active_ ||
			(local_usegun_slot_active_ &&
			 active_local_weapon_slot() == &weapon_slot_)) {
		commit_local_usegun_weapon_switch();
		return;
	}
	opennova::world::WeaponSlotState *slot = active_local_weapon_slot();
	if (local_usegun_switch_action_ ==
			opennova::world::weapon_action::kSwitchRank)
		opennova::world::weapon_fsm_queue_switch_rank(*slot);
	else
		opennova::world::weapon_fsm_queue_switch_from(*slot);
}

void NovaSimulation::commit_local_usegun_weapon_switch() {
	if (!world_ || local_usegun_switch_ == LocalUseGunSwitch::kNone) return;
	opennova::world::Entity *player =
			world_->registry.get(world_->cached.local_player);
	if (player == nullptr) {
		local_usegun_switch_ = LocalUseGunSwitch::kNone;
		local_usegun_slot_active_ = false;
		local_usegun_mount_ = opennova::world::EntityHandle{};
		local_usegun_weapon_adm_ = 0xFF;
		local_usegun_pending_mount_ = opennova::world::EntityHandle{};
		local_usegun_pending_weapon_adm_ = 0xFF;
		local_usegun_saved_adm_ = 0xFF;
		local_usegun_switch_action_ = -1;
		weapon_switch_deferred_action_ = -1;
		weapon_active_ = false;
		local_first_person_model_adm_ = 0xFF;
		return;
	}

	uint8_t next_adm = 0xFF;
	opennova::world::WeaponSlotState *next_slot = nullptr;
	bool select_parent =
			local_usegun_switch_ == LocalUseGunSwitch::kAttach ||
			local_usegun_switch_ == LocalUseGunSwitch::kSwap;
	if (select_parent) {
		opennova::world::Entity *mount =
				world_->registry.get(local_usegun_pending_mount_);
		if (mount == nullptr ||
				mount->primary_weapon_slot_adm !=
						local_usegun_pending_weapon_adm_) {
			select_parent = false;
		} else {
			local_usegun_slot_active_ = true;
			local_usegun_mount_ = local_usegun_pending_mount_;
			local_usegun_weapon_adm_ =
					local_usegun_pending_weapon_adm_;
			next_adm = local_usegun_weapon_adm_;
			next_slot = &mount->primary_weapon_slot;
		}
	}
	if (!select_parent) {
		local_usegun_slot_active_ = false;
		next_adm = local_usegun_saved_adm_;
		next_slot = &weapon_slot_;
	}

	player->equipped_adm_index = next_adm;
	const opennova::world::WeaponTableEntry *next_def =
			world_->weapons.by_index(next_adm);
	// SWITCHFROM draws the committed target through TryQueueSwitchTo. SWITCHRANK
	// swaps directly and must not disturb a persistent target slot's current
	// action/phase/next fields. [orig: commits @0x543475 / @0x543539]
	if (next_slot != nullptr && next_def != nullptr &&
			local_usegun_switch_action_ ==
				opennova::world::weapon_action::kSwitchFrom)
		opennova::world::weapon_fsm_try_queue_switch_to(*next_slot);
	PendingWeaponEvent event;
	event.tick = world_->logic_tick;
	event.world_position = get_local_player_position();
	event.switch_to_weapon =
			next_def != nullptr ? String::utf8(next_def->name.c_str()) : String();
	event.clear_weapon = next_def == nullptr;
	event.preserve_slot_state = true;
	pending_weapon_events_.push_back(std::move(event));
	if (next_def == nullptr) weapon_active_ = false;

	local_usegun_switch_ = LocalUseGunSwitch::kNone;
	local_usegun_pending_mount_ = opennova::world::EntityHandle{};
	local_usegun_pending_weapon_adm_ = 0xFF;
	local_usegun_switch_action_ = -1;
	weapon_switch_deferred_action_ = -1;
	if (!local_usegun_slot_active_) {
		local_usegun_mount_ = opennova::world::EntityHandle{};
		local_usegun_weapon_adm_ = 0xFF;
		local_usegun_saved_adm_ = 0xFF;
	}
}

void NovaSimulation::sync_local_usegun_weapon_transition() {
	if (!world_ || !world_->cached.local_player.valid()) return;
	opennova::world::Entity *player =
			world_->registry.get(world_->cached.local_player);
	if (player == nullptr) return;
	opennova::world::Entity *mounted_parent =
			player->mounted &&
					player->mount_type == opennova::world::SeatType::Gunner
			? world_->registry.get(player->mount_target)
			: nullptr;
	const bool on_usegun =
			mounted_parent != nullptr && player->use_gun_slot_swapped &&
			mounted_parent->primary_weapon_slot_adm != 0xFF;
	const auto same_category = [&](uint8_t p_from, uint8_t p_to) {
		const opennova::world::WeaponTableEntry *from =
				world_->weapons.by_index(p_from);
		const opennova::world::WeaponTableEntry *to =
				world_->weapons.by_index(p_to);
		return from != nullptr && to != nullptr &&
				from->category == to->category;
	};
	const auto stage_parent = [&](opennova::world::Entity &p_mount) {
		const uint8_t target_adm = p_mount.primary_weapon_slot_adm;
		if (!local_usegun_slot_active_)
			local_usegun_saved_adm_ =
					player->pre_use_gun_equipped_adm_index;
		local_usegun_pending_mount_ = p_mount.handle;
		local_usegun_pending_weapon_adm_ = target_adm;
		local_usegun_switch_ = local_usegun_slot_active_
				? LocalUseGunSwitch::kSwap
				: LocalUseGunSwitch::kAttach;
		const uint8_t from_adm = local_usegun_slot_active_
				? local_usegun_weapon_adm_
				: local_usegun_saved_adm_;
		// The shared helper applied the nonlocal immediate stamp. L retains its
		// outgoing EquippedSlot until the action handler's commit seam.
		player->equipped_adm_index = from_adm;
		queue_local_usegun_weapon_switch(
				same_category(from_adm, target_adm));
	};
	const auto stage_personal = [&]() {
		const uint8_t from_adm = local_usegun_slot_active_
				? local_usegun_weapon_adm_
				: local_usegun_saved_adm_;
		player->equipped_adm_index = from_adm;
		local_usegun_pending_mount_ =
				opennova::world::EntityHandle{};
		local_usegun_pending_weapon_adm_ = 0xFF;
		local_usegun_switch_ = LocalUseGunSwitch::kDetach;
		queue_local_usegun_weapon_switch(
				same_category(from_adm, local_usegun_saved_adm_));
	};

	if (on_usegun) {
		const bool active_matches = local_usegun_slot_active_ &&
				local_usegun_mount_ == mounted_parent->handle &&
				local_usegun_weapon_adm_ ==
						mounted_parent->primary_weapon_slot_adm;
		const bool pending_matches =
				(local_usegun_switch_ == LocalUseGunSwitch::kAttach ||
				 local_usegun_switch_ == LocalUseGunSwitch::kSwap) &&
				local_usegun_pending_mount_ == mounted_parent->handle &&
				local_usegun_pending_weapon_adm_ ==
						mounted_parent->primary_weapon_slot_adm;
		if (local_usegun_switch_ == LocalUseGunSwitch::kNone) {
			if (!active_matches) stage_parent(*mounted_parent);
		} else if (!pending_matches) {
			// A later attach overwrites g_pendingWeaponSlot without changing the
			// outgoing slot. This includes direct old-gun -> new-gun swaps.
			stage_parent(*mounted_parent);
		}
		return;
	}

	if ((local_usegun_slot_active_ ||
			 local_usegun_switch_ == LocalUseGunSwitch::kAttach ||
			 local_usegun_switch_ == LocalUseGunSwitch::kSwap) &&
			local_usegun_switch_ != LocalUseGunSwitch::kDetach)
		stage_personal();
}

bool NovaSimulation::local_player_toggle_mount() {
	// The USE-ITEM mount toggle for the local player — the shell calls this when the
	// armory/vehicle-zone legs of the key don't apply. [orig: Input_ProcessFrame release
	// edge @0x49d6dc -> Entity_ToggleVehicleMount @0x436950]
	if (!world_) return false;
	// Authority-only: the witnessed non-authority path queues C2S 0x26 (attach) /
	// sends 0x27 (detach) and waits for the 0x0A stream to confirm [orig:
	// Entity_RequestVehicleAttach @0x4364a0 / Entity_SendDetachPacket @0x435510].
	// That joiner wire leg is unported (D-AI-11 l) — applying locally on a joiner
	// would silently desync against the host, so the toggle refuses (the armory
	// leg's MP stance).
	if (joiner_) return false;
	// The ordinary local/offline toggle requires a live EquippedSlot/Def. Script
	// and network authority paths may still attach through the world core.
	// [orig: Entity_AttachToUseGunSlot null-slot reject @0x546c07]
	const opennova::world::Entity *toggle_player =
			world_->registry.get(world_->cached.local_player);
	if (!weapon_active_ &&
			(toggle_player == nullptr || !toggle_player->mounted))
		return false;
	sync_local_usegun_weapon_transition();
	// The weapon-busy gate [orig: @0x436958-0x436977 — no EquippedSlot passes;
	// currentAction < 2 (idle/emptyidle) or == 5 (the dry click) passes, as does a
	// pending OVERHEATED (nextAction == 11); an in-flight fire/reload/switch swallows
	// the toggle].
	const opennova::world::WeaponSlotState *active_slot =
			active_local_weapon_slot();
	const int32_t cur = active_slot->current;
	const int32_t next = active_slot->next;
	if (!(cur < 2 || cur == opennova::world::weapon_action::kEmpty ||
	      next == opennova::world::weapon_action::kOverheated))
		return false;
	const bool changed = opennova::world::player_toggle_vehicle_mount(
			*world_, world_->cached.local_player);
	if (changed) {
		// A successful ToSpecial/use-item transition clears the raw binocular
		// request, not merely the effective first-person view.
		player_view_.binoculars_requested = false;
		binocular_yaw_offset_deg_ = 0.0f;
		binocular_pitch_offset_deg_ = 0.0f;
		refresh_local_player_view_effects();
		sync_local_mounted_input_heading();
		sync_local_usegun_weapon_transition();
	}
	return changed;
}

TypedArray<Dictionary> NovaSimulation::get_attach_labels() const {
	TypedArray<Dictionary> out;
	if (!world_) return out;
	const opennova::world::Entity *player = world_->registry.get(world_->cached.local_player);
	if (player == nullptr || !player->alive || player->health <= 0) return out;
	// Armory mode = standing in the type-6 armory volume; the label pass reads the raw
	// flag [orig: is_armory_mode = entity Flags & 0x400000 @0x5a32c4].
	const bool armory_mode =
	    (player->flags & opennova::world::kEntityFlagArmoryZone) != 0;
	// The nearest-only gate [orig: Player_CanFireWeapon @0x5cf780 — EquippedSlot present
	// and parentSlot not 2/5 (ctrl/drvr); the camera-mode/underwater/scope legs live
	// host-side and are unmodeled here: docs/interface/hud-re.md (D-HUD-11)].
	const bool can_fire =
	    player->equipped_adm_index != 0xFF &&
	    !(player->mounted && opennova::world::is_vehicle_control_seat(player->mount_type));
	std::vector<opennova::world::AttachLabel> labels;
	opennova::world::collect_attach_labels(*world_, *player, armory_mode, can_fire, labels);
	for (const opennova::world::AttachLabel &l : labels) {
		Dictionary d;
		d["position"] = Vector3(l.world_pos.x, l.world_pos.y, l.world_pos.z);
		d["seat_type"] = static_cast<int>(l.type);
		d["armory"] = l.armory;
		d["nearest"] = l.nearest;
		String key;
		if (l.type == opennova::world::SeatType::Gunner) {
			// The USEGUN label text: the gun entity's primary weapon -> its weapon.def
			// attachtextid key [orig: Entity_GetWeaponSlots slot0 -> def+0x3A0 @0x5a351d].
			const opennova::world::Entity *cand = world_->registry.get(l.entity);
			if (cand != nullptr && !cand->primary_weapon.empty()) {
				const int wi = world_->weapons.index_of(cand->primary_weapon.c_str());
				if (wi >= 0)
					key = String(world_->weapons.entries[static_cast<size_t>(wi)]
					                     .attach_text_id.c_str());
			}
		}
		d["attach_text_key"] = key;
		out.push_back(d);
	}
	return out;
}

// --- the local player's loadout: slot pool, spawn kit, map rules -----------------------
// (the 2026-07-18 loadout grill; witness map in docs/net/novaworld-net-re.md §5.57)

namespace {

opennova::world::WeaponKitEntry kit_entry_from_dict(const Dictionary &d) {
	opennova::world::WeaponKitEntry e;
	e.name = dictionary_string(d, "name", std::string());
	e.ammo_primary = int(int64_t(d.get("ammo_primary", -1)));
	e.ammo_secondary = int(int64_t(d.get("ammo_secondary", -1)));
	e.flags = int(int64_t(d.get("flags", -1)));
	return e;
}

Dictionary kit_entry_to_dict(const opennova::world::WeaponKitEntry &e) {
	Dictionary d;
	d["name"] = String::utf8(e.name.c_str());
	d["ammo_primary"] = e.ammo_primary;
	d["ammo_secondary"] = e.ammo_secondary;
	d["flags"] = e.flags;
	return d;
}

} // namespace

void NovaSimulation::set_spawn_loadout(const TypedArray<Dictionary> &p_kit,
                                       bool p_filter_by_availability) {
	std::vector<opennova::world::WeaponKitEntry> kit;
	for (int i = 0; i < p_kit.size(); ++i) {
		opennova::world::WeaponKitEntry e = kit_entry_from_dict(p_kit[i]);
		if (!e.name.empty()) kit.push_back(std::move(e));
	}
	if (p_filter_by_availability && world_ != nullptr && !kit.empty()) {
		// The SP .bms promote leg: availability-filter with the knife fallback
		// [orig: Mission_LoadBMSFile @ 0x40f7ae..0x40f95c].
		kit = opennova::world::weapon_kit_filter_by_availability(kit, world_->weapons,
		                                                         weapon_availability_);
	}
	spawn_kit_set_ = !kit.empty();
	spawn_kit_ = std::move(kit);
}

void NovaSimulation::set_weapon_availability(const TypedArray<Dictionary> &p_pairs) {
	weapon_availability_.reset(); // [orig: the all-1 default @ 0x551c86]
	if (!world_ || p_pairs.is_empty()) return;
	std::vector<std::pair<std::string, int32_t>> pairs;
	for (int i = 0; i < p_pairs.size(); ++i) {
		const Dictionary d = p_pairs[i];
		std::string name = dictionary_string(d, "name", std::string());
		if (name.empty()) continue;
		pairs.emplace_back(std::move(name), int32_t(int64_t(d.get("value", 1))));
	}
	opennova::world::weapon_availability_apply_pairs(weapon_availability_, world_->weapons,
	                                                 pairs);
}

int NovaSimulation::get_weapon_availability(const String &p_weapon_name) const {
	if (!world_) return opennova::world::weapon_availability_value::kAllowed;
	const int idx = world_->weapons.index_of(p_weapon_name.utf8().get_data());
	if (idx < 0) return opennova::world::weapon_availability_value::kAllowed;
	return weapon_availability_.value_for(idx);
}

bool NovaSimulation::set_local_player_class(int p_player_class) {
	if (!world_ || p_player_class < 5 || p_player_class > 9) return false;
	opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (e == nullptr) return false;
	e->player_class = static_cast<uint8_t>(p_player_class);
	return true;
}

bool NovaSimulation::apply_local_player_loadout(const TypedArray<Dictionary> &p_kit,
                                                int p_player_class) {
	// The armory ACCEPT apply [orig: WeaponLoadout_ApplyFromBuffer @ 0x565cd0 offline
	// leg: parse the tuples, expand sub-weapons, reset + refill the slot table, apply
	// the requested ammo, re-select the equipped slot].
	if (!world_) return false;
	opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (e == nullptr) return false;
	if (p_player_class >= 5 && p_player_class <= 9)
		e->player_class = static_cast<uint8_t>(p_player_class);
	std::vector<opennova::world::WeaponKitEntry> kit;
	for (int i = 0; i < p_kit.size(); ++i) {
		opennova::world::WeaponKitEntry entry = kit_entry_from_dict(p_kit[i]);
		if (entry.name.empty()) continue;
		const int idx = world_->weapons.index_of(entry.name.c_str());
		if (idx < 0) continue;
		// The per-entry availability validation [orig: the server 0x2F gate
		// @ 0x515a3f — 0 drops the entry; 2 requires the armory zone, which the
		// ACCEPT flow is already gated on host-side].
		if (weapon_availability_.value_for(idx) ==
		    opennova::world::weapon_availability_value::kBanned)
			continue;
		kit.push_back(std::move(entry));
	}
	// The accepted loadout becomes the respawn kit [orig: the S2C 0x5A apply writes
	// restrictionData @ 0x4293e4; SP shares the buffer]. An explicit empty kit stays
	// empty (the armory all-NONE accept leaves the table bare).
	spawn_kit_set_ = true;
	spawn_kit_ = std::move(kit);
	rebuild_local_player_loadout(/*p_select_spawn_default=*/true);
	// The ACCEPT leg applies the REQUESTED ammo over the default seed: pool =
	// min(req, maxclips) * clipsize when requested, else startrounds RAW on the main
	// leg; the first different-class sub-variant takes ammo_secondary with the
	// x clipsize fallback [orig: @ 0x566166 vs @ 0x566209; WeaponSlot_SetAmmoCount
	// @ 0x540b50].
	const opennova::world::WeaponTable &table = world_->weapons;
	for (const opennova::world::WeaponKitEntry &entry : spawn_kit_) {
		const int adm = table.index_of(entry.name.c_str());
		if (adm < 0) continue;
		const opennova::world::WeaponTableEntry *def =
				table.by_index(static_cast<uint8_t>(adm));
		if (def == nullptr) continue;
		if (def->clipsize != -1) {
			int32_t total = entry.ammo_primary >= 0
					? std::min<int32_t>(entry.ammo_primary, def->maxclips) * def->clipsize
					: def->startrounds;
			opennova::world::weapon_pool_set(table, local_inventory_, def->ammo_class_id,
			                                 total);
		}
		for (int k = 1; k <= def->loadout_subclasses; ++k) {
			const opennova::world::WeaponTableEntry *sub =
					(adm + k < 256) ? table.by_index(static_cast<uint8_t>(adm + k))
					                : nullptr;
			if (sub == nullptr) continue;
			if (opennova::strutil::iequals(sub->ammo_class, def->ammo_class)) continue;
			int32_t total = entry.ammo_secondary >= 0
					? std::min<int32_t>(entry.ammo_secondary, sub->maxclips)
					: static_cast<int32_t>(sub->startrounds);
			// A nonnegative sub-weapon count is expressed in clips and expands to
			// rounds; a negative fallback is the no-clip sentinel and stays raw.
			// This is what keeps the implicit satchel detonator switch-eligible.
			// [orig: WeaponLoadout_ApplyFromBuffer @ 0x5661E8..0x566215]
			if (total >= 0) total *= sub->clipsize;
			opennova::world::weapon_pool_set(table, local_inventory_, sub->ammo_class_id,
			                                 total);
			break; // the FIRST different-class sub-variant [orig: @ 0x5027c8 shape]
		}
	}
	opennova::world::weapon_inventory_recalc_clips(table, local_inventory_);
	return true;
}

void NovaSimulation::respawn_local_player_loadout() {
	rebuild_local_player_loadout(/*p_select_spawn_default=*/true);
	// Player_InitPlayer clears both view effects and seeds NVG from the mission
	// StartWithNVGOn bit on every respawn.
	reset_local_player_view_effects();
}

void NovaSimulation::sync_local_player_damage_classes() {
	if (!world_) return;
	opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (e == nullptr) return;
	e->ammo_damage_class.assign(world_->ammo.entries.size(), 0);
	const std::vector<opennova::world::WeaponKitEntry> kit =
			spawn_kit_set_ ? spawn_kit_ : opennova::world::weapon_kit_default();
	opennova::world::weapon_kit_build_damage_classes(
			kit, world_->weapons, world_->ammo.entries.size(), e->ammo_damage_class);
}

void NovaSimulation::rebuild_local_player_loadout(bool p_select_spawn_default) {
	// The Player_InitPlayer weapon leg [orig: @ 0x4e15f0: AvatarDef_BuildDisplayList
	// (restrictionData) -> WeaponSlotPool_ResetAllEntries -> WeaponSlotTable_
	// LoadAllFromDefs -> WeaponSlots_SeedAmmoPoolsFromDefs -> WeaponSlots_
	// RecalculateAmmoFromCapacity -> Player_SelectWeaponSlot(195) ->
	// Player_SwitchToWeaponByHandle(195)].
	if (!world_) return;
	opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (e == nullptr) return;
	const opennova::world::WeaponTable &table = world_->weapons;
	sync_local_player_damage_classes();
	if (table.empty()) return;
	const std::vector<opennova::world::WeaponKitEntry> kit =
			spawn_kit_set_ ? spawn_kit_ : opennova::world::weapon_kit_default();
	local_inventory_.reset(table);
	const std::vector<std::string> display =
			opennova::world::weapon_kit_expand_display_list(kit, table);
	const opennova::world::WeaponFillResult fill =
			opennova::world::weapon_inventory_load_from_display(table, display,
			                                                    local_inventory_);
	for (const std::string &w : fill.warnings)
		print_verbose(String::utf8(w.c_str())); // [orig: ErrorLog_WriteTimestamped]
	opennova::world::weapon_inventory_seed_pools(table, local_inventory_,
	                                             e->player_class);
	opennova::world::weapon_inventory_recalc_clips(table, local_inventory_);
	local_inventory_valid_ = true;
	weapon_switch_in_flight_ = false;
	weapon_switch_deferred_action_ = -1;
	if (!p_select_spawn_default) return;
	const opennova::world::WeaponSwitchGates gates = local_weapon_switch_gates();
	if (!opennova::world::weapon_select_slot(
	            table, local_inventory_,
	            opennova::world::weapon_combo::kDefaultSpawnCombo,
	            !gates.equip_blocked)) {
		// An empty table (the armory all-NONE kit) equips nothing.
		e->equipped_adm_index = 0xFF;
		return;
	}
	// Player_InitPlayer follows the select with SwitchToWeaponByHandle(195)
	// [orig: @ 0x4e1995/@ 0x4e19a1], but the switch's mount walk rides the entity's
	// AI-slot binding gate [orig: entity+0x68 test @ 0x4e023a; writer
	// Entity_AllocateAISlot @ 0x40d2f4] — modeled here as not-yet-bound during the
	// spawn rebuild (the motor/presentation bind after load), so the spawn equips
	// exactly the selected slot and plays no switch actions. The gate's init-time
	// value is an open question (D-WPN-21, docs/divergence-ledger.md).
	local_inventory_.pending_combo = local_inventory_.equipped_combo;
	commit_pending_weapon_switch();
	weapon_start_in_switchto_ = false;
}

opennova::world::WeaponSwitchGates NovaSimulation::local_weapon_switch_gates() const {
	opennova::world::WeaponSwitchGates gates;
	const opennova::world::Entity *e =
			world_ ? world_->registry.get(world_->cached.local_player) : nullptr;
	if (e != nullptr && e->mounted) {
		// [orig: the parentSlot {2,3,5} stance gate @ 0x4e0192 — our SeatType enum
		//  carries the original's raw values: Controller=2, Gunner=3, Driver=5;
		//  passengers (1) keep switching. The equip-commit defer gates {2,3} only
		//  @ 0x4dd6fc.]
		const auto t = e->mount_type;
		gates.seat_blocked = t == opennova::world::SeatType::Controller ||
		                     t == opennova::world::SeatType::Gunner ||
		                     t == opennova::world::SeatType::Driver;
		gates.equip_blocked = t == opennova::world::SeatType::Controller ||
		                      t == opennova::world::SeatType::Gunner;
	}
	gates.equipped_valid =
			local_inventory_.equipped_combo >= 0 &&
			local_inventory_.slot(local_inventory_.equipped_combo) != nullptr &&
			local_inventory_.slot(local_inventory_.equipped_combo)->adm_index >= 0;
	const opennova::world::WeaponSlotState *active_slot =
			active_local_weapon_slot();
	gates.equipped_action = weapon_active_ ? active_slot->current
	                                       : opennova::world::weapon_action::kIdle;
	return gates;
}

void NovaSimulation::commit_pending_weapon_switch() {
	// The pending -> equipped commit [orig: the switchfrom/switchrank completion
	// consumes g_pendingWeaponSlot; EquippedSlot swap + the equippedAdmIndex stamp
	// @ 0x4dd727; the FP model re-resolve runs host-side off the event].
	weapon_switch_in_flight_ = false;
	weapon_switch_deferred_action_ = -1;
	nvg_scope_restore_ = false;
	if (!world_ || !local_inventory_valid_) return;
	opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (e == nullptr) return;
	const int32_t combo = local_inventory_.pending_combo;
	const opennova::world::WeaponInventorySlot *slot = local_inventory_.slot(combo);
	if (slot == nullptr || slot->adm_index < 0) return;
	local_inventory_.equipped_combo = combo;
	e->equipped_adm_index = static_cast<uint8_t>(slot->adm_index);
	const opennova::world::WeaponTableEntry *def =
			world_->weapons.by_index(static_cast<uint8_t>(slot->adm_index));
	weapon_start_in_switchto_ = true;
	PendingWeaponEvent event;
	event.tick = world_->logic_tick;
	event.world_position = get_local_player_position();
	event.switch_to_weapon =
			def != nullptr ? String::utf8(def->name.c_str()) : String();
	pending_weapon_events_.push_back(std::move(event));
}

void NovaSimulation::request_local_player_weapon_category(int p_category) {
	if (player_view_.binoculars_view_active) return;
	// [orig: input cases 200-210 @ 0x4e1144 -> Player_SwitchToWeaponByHandle
	//  ((action-200)*65). The binoculars-view and fire-charge input gates have no
	//  sim mechanics yet — record note.]
	if (local_usegun_switch_ != LocalUseGunSwitch::kNone) return;
	if (!world_ || !local_inventory_valid_) return;
	if (p_category < 0 || p_category >= opennova::world::weapon_combo::kCategories)
		return;
	const opennova::world::WeaponSwitchOutcome out = opennova::world::weapon_switch_to_handle(
			world_->weapons, local_inventory_,
			p_category * opennova::world::weapon_combo::kRanksPerCategory,
			local_weapon_switch_gates());
	handle_weapon_switch_outcome(out);
}

void NovaSimulation::request_local_player_weapon_cycle(int p_direction) {
	if (player_view_.binoculars_view_active) return;
	// [orig: input cases 212/214 -> Player_CycleWeaponSlot @ 0x4dfe70; the mounted-gun
	//  elevation dual-purpose leg belongs to the vehicle channel, not this walk]
	if (local_usegun_switch_ != LocalUseGunSwitch::kNone) return;
	if (!world_ || !local_inventory_valid_) return;
	const opennova::world::WeaponSwitchOutcome out = opennova::world::weapon_cycle_slot(
			world_->weapons, local_inventory_, p_direction, local_weapon_switch_gates());
	handle_weapon_switch_outcome(out);
}

void NovaSimulation::handle_weapon_switch_outcome(
		const opennova::world::WeaponSwitchOutcome &p_out) {
	switch (p_out.kind) {
		case opennova::world::WeaponSwitchOutcome::kDeny: {
			// [orig: PlaySoundOnDedicatedServer(dword_24E08C4) @ 0x4e0354]
			PendingWeaponEvent event;
			event.tick = world_ ? world_->logic_tick : 0;
			event.world_position = get_local_player_position();
			event.switch_denied = true;
			pending_weapon_events_.push_back(std::move(event));
			break;
		}
		case opennova::world::WeaponSwitchOutcome::kMount: {
			// [orig: Player_MountWeaponSlot @ 0x4dfa40 — pending already stamped by
			//  the walk; the OUTGOING slot's FSM plays SWITCHRANK (same category) or
			//  SWITCHFROM (cross category) and its completion commits]
			if (!weapon_active_) {
				commit_pending_weapon_switch();
				break;
			}
			weapon_switch_in_flight_ = true;
			opennova::world::WeaponSlotState *active_slot =
					active_local_weapon_slot();
			const int32_t action = p_out.same_category
					? opennova::world::weapon_action::kSwitchRank
					: opennova::world::weapon_action::kSwitchFrom;
			if (active_slot->current ==
					opennova::world::weapon_action::kSwitchTo) {
				// The witnessed writer refuses during SWITCHTO. Unlike the original
				// dispatcher, this host supplies one press edge, so retain it beside
				// the already-stamped pending combo and keep restoring next after
				// SWITCHTO's delay-start initializer writes its resume action.
				weapon_switch_deferred_action_ = action;
				active_slot->next = action;
			} else if (active_slot->next ==
					opennova::world::weapon_action::kSwitchTo) {
				// The draw is queued but has not entered yet. Preserve it; the
				// post-tick latch below attaches the requested outgoing action.
				weapon_switch_deferred_action_ = action;
			} else if (p_out.same_category) {
				weapon_switch_deferred_action_ = -1;
				opennova::world::weapon_fsm_queue_switch_rank(*active_slot);
			} else {
				weapon_switch_deferred_action_ = -1;
				opennova::world::weapon_fsm_queue_switch_from(*active_slot);
			}
			break;
		}
		default:
			break;
	}
}

Dictionary NovaSimulation::get_local_player_inventory() const {
	Dictionary out;
	out["valid"] = local_inventory_valid_;
	out["equipped_combo"] = local_inventory_.equipped_combo;
	out["carry_flags"] = int64_t(local_inventory_.carry_flags);
	String equipped_name;
	Array slots;
	Dictionary pools;
	if (world_ != nullptr) {
		const opennova::world::WeaponTable &table = world_->weapons;
		for (int32_t combo = 0; combo < opennova::world::weapon_combo::kSlotCount;
		     ++combo) {
			const opennova::world::WeaponInventorySlot *s = local_inventory_.slot(combo);
			if (s == nullptr || s->adm_index < 0) continue;
			const opennova::world::WeaponTableEntry *def =
					table.by_index(static_cast<uint8_t>(s->adm_index));
			if (def == nullptr) continue;
			Dictionary row;
			row["combo"] = combo;
			row["name"] = String::utf8(def->name.c_str());
			row["clip"] = s->clip;
			slots.push_back(row);
			if (combo == local_inventory_.equipped_combo)
				equipped_name = String::utf8(def->name.c_str());
		}
		for (size_t i = 0; i < table.ammo_class_names.size() &&
		                   i < local_inventory_.pools.size();
		     ++i) {
			if (table.ammo_class_names[i].empty()) continue;
			pools[String::utf8(table.ammo_class_names[i].c_str())] =
					local_inventory_.pools[i];
		}
	}
	out["equipped_name"] = equipped_name;
	out["slots"] = slots;
	out["pools"] = pools;
	return out;
}

TypedArray<Dictionary> NovaSimulation::get_local_player_loadout() const {
	TypedArray<Dictionary> out;
	const std::vector<opennova::world::WeaponKitEntry> kit =
			spawn_kit_set_ ? spawn_kit_ : opennova::world::weapon_kit_default();
	for (const opennova::world::WeaponKitEntry &entry : kit)
		out.push_back(kit_entry_to_dict(entry));
	return out;
}

// weapon.def -> the sim world's armory table. Mirrors the retail load site (Game_StartMission
// parses literally "weapon.def" through WeaponDefs_LoadFile right after AnimDef_InitAll wipes
// the AdmDef table [orig: @0x5254b3/@0x5254bd]); build_weapon_table ports the witnessed
// allocation rule (null@0 + by-name-reuse-else-lowest-free = file order; §5.57, D-NET-141).
Error NovaSimulation::load_weapon_table(const Ref<NovaResourceRoot> &p_resource_root,
                                        const String &p_name) {
	if (!world_) return ERR_UNCONFIGURED;
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty())
		return ERR_INVALID_PARAMETER;
	const String file_name = p_name.get_file();
	if (file_name.is_empty()) return ERR_INVALID_PARAMETER;
	const PackedByteArray bytes = p_resource_root->read_file(file_name);
	if (bytes.is_empty()) return ERR_FILE_NOT_FOUND;

	DefWeaponsFile file = {};
	if (def_parse_weapons_memory(bytes.ptr(), static_cast<size_t>(bytes.size()), &file) != 0)
		return ERR_CANT_OPEN;
	world_->weapons = opennova::np::build_weapon_table(file);
	def_free_weapons(&file);

	// The host's own player spawns in finish_load, BEFORE this feed — re-stamp its equipped
	// default now that WPN_M4AUTO resolves by name [orig: PlayerClass_InitEntity @0x4B1116].
	// Joiners spawn after the feed and get the default in Server_BuildPlayerInfoAndAdd.
	// (D-NET-143)
	const int m4 = world_->weapons.index_of("WPN_M4AUTO");
	if (m4 >= 0) {
		std::vector<opennova::world::EntityHandle> handles;
		world_->registry.for_each([&](const opennova::world::Entity &e) {
			if (e.item_id == opennova::world::kPlayerInfantryTypeId &&
			    e.equipped_adm_index == 0xFF)
				handles.push_back(e.handle);
		});
		for (const opennova::world::EntityHandle h : handles) {
			if (opennova::world::Entity *e = world_->registry.get(h))
				e->equipped_adm_index = static_cast<uint8_t>(m4);
		}
	}
	// The LOCAL player's slot pool builds from the spawn kit (the mission/armory
	// loadout when one was promoted, else the WPN_M4AUTO default kit) and selects the
	// spawn default — the Player_InitPlayer weapon leg [orig: @ 0x4e15f0; the default
	// kit literal @ 0x5246be]. This subsumes the bare adm-index stamp above for the
	// local player.
	rebuild_local_player_loadout(/*p_select_spawn_default=*/true);
	return OK;
}

// ammo.def -> the sim world's ballistics table + the weapon round_type resolve. Mirrors the
// retail load site (Game_StartMission parses literally "ammo.def" through AmmoDef_LoadAll
// @0x40b0b0, the sibling of the weapon.def load [orig: @0x52548a]); the resolve binds each
// adm's fired round to its AmmoTable index (the original's adm+84 pair; §5.60). Call AFTER
// load_weapon_table — an empty armory leaves every round_type unresolved and the fire
// pipeline echoes without spawning sim rounds.
Error NovaSimulation::load_ammo_table(const Ref<NovaResourceRoot> &p_resource_root,
                                      const String &p_name) {
	if (!world_) return ERR_UNCONFIGURED;
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty())
		return ERR_INVALID_PARAMETER;
	const String file_name = p_name.get_file();
	if (file_name.is_empty()) return ERR_INVALID_PARAMETER;
	const PackedByteArray bytes = p_resource_root->read_file(file_name);
	if (bytes.is_empty()) return ERR_FILE_NOT_FOUND;

	DefAmmoFile file = {};
	if (def_parse_ammo_memory(bytes.ptr(), static_cast<size_t>(bytes.size()), &file) != 0)
		return ERR_CANT_OPEN;
	world_->ammo = opennova::np::build_ammo_table(file);
	def_free_ammo(&file);
	opennova::np::resolve_weapon_round_types(world_->weapons, world_->ammo);
	sync_local_player_damage_classes();
	return OK;
}

opennova::mission::PromoteOptions NovaSimulation::promote_options() const {
	opennova::mission::PromoteOptions opts;
	opts.item_seat_specs = item_seat_specs_;
	return opts;
}

void NovaSimulation::set_item_seat_specs(const Array &p_specs) {
	item_seat_specs_.clear();
	mounted_pose_data_by_type_.clear();
	for (int64_t i = 0; i < p_specs.size(); ++i) {
		const Variant spec_v = p_specs[i];
		if (spec_v.get_type() != Variant::DICTIONARY) continue;
		const Dictionary spec_d = spec_v;

		opennova::mission::ItemSeatSpec spec;
		spec.type_id = static_cast<int32_t>(spec_d.get("type_id", 0));
		if (spec.type_id == 0) continue;
		const Variant model_data_value =
				spec_d.get("model_data", Variant());
		if (model_data_value.get_type() == Variant::OBJECT) {
			Ref<NovaObjectData> model_data(model_data_value);
			if (model_data.is_valid() && model_data->has_document())
				mounted_pose_data_by_type_[spec.type_id] = model_data;
		}
		if (spec_d.has("mount_config_valid")) {
			spec.mount_config_valid = static_cast<bool>(spec_d.get("mount_config_valid", false));
			spec.mount_config = spec.mount_config_valid
			                        ? static_cast<int32_t>(spec_d.get("mount_config", 0))
			                        : 0;
		}

		const Variant seats_v = spec_d.get("seats", Array());
		if (seats_v.get_type() != Variant::ARRAY) continue;
		const Array seats_a = seats_v;
		bool retail_slots_used[10] = {};
		int inferred_passenger_slot = 0;
		for (int64_t j = 0; j < seats_a.size(); ++j) {
			const Variant seat_v = seats_a[j];
			if (seat_v.get_type() != Variant::DICTIONARY) continue;
			const Dictionary seat_d = seat_v;

			opennova::world::Seat seat;
			seat.type = seat_type_from_variant(static_cast<int>(seat_d.get("type", 0)));
			if (seat.type == opennova::world::SeatType::None) continue;
			int retail_slot = -1;
			if (seat_d.has("retail_slot")) {
				retail_slot = static_cast<int>(seat_d.get("retail_slot", -1));
			} else {
				// Compatibility for tests/tools that construct seat dictionaries
				// directly. Production extraction supplies the explicit slot.
				switch (seat.type) {
					case opennova::world::SeatType::Passenger:
						while (inferred_passenger_slot < 8 &&
						       retail_slots_used[inferred_passenger_slot])
							++inferred_passenger_slot;
						if (inferred_passenger_slot < 8)
							retail_slot = inferred_passenger_slot++;
						break;
					case opennova::world::SeatType::Controller:
					case opennova::world::SeatType::Driver:
						retail_slot = 8;
						break;
					case opennova::world::SeatType::Gunner:
						retail_slot = 9;
						break;
					default:
						break;
				}
			}
			const bool slot_matches_type =
					(retail_slot >= 0 && retail_slot < 8 &&
					 seat.type == opennova::world::SeatType::Passenger) ||
					(retail_slot == 8 &&
					 opennova::world::is_vehicle_control_seat(seat.type)) ||
					(retail_slot == 9 &&
					 seat.type == opennova::world::SeatType::Gunner);
			if (slot_matches_type && !retail_slots_used[retail_slot]) {
				seat.retail_slot = static_cast<uint8_t>(retail_slot);
				retail_slots_used[retail_slot] = true;
			}
			seat.bone_index = static_cast<uint8_t>(
			    std::clamp(static_cast<int>(seat_d.get("bone_index", 0)), 0, 255));
			seat.pose_index = static_cast<uint8_t>(
			    std::clamp(static_cast<int>(seat_d.get("pose_index", 0)), 0, 30));
			seat.source_name = String(seat_d.get("source_name", String())).utf8().get_data();
			const Vector3 pos = seat_d.get("position", Vector3());
			seat.seat_local = {static_cast<float>(pos.x), static_cast<float>(pos.y),
			                   static_cast<float>(pos.z)};
			seat.yaw_offset = static_cast<int16_t>(
			    std::clamp(static_cast<int>(seat_d.get("yaw_offset", 0)), -32768, 32767));
			spec.seats.push_back(seat);
		}
		const Variant attachments_v =
				spec_d.get("emplacement_attachments", Array());
		if (attachments_v.get_type() == Variant::ARRAY) {
			const Array attachments_a = attachments_v;
			for (int64_t j = 0; j < attachments_a.size(); ++j) {
				const Variant attachment_v = attachments_a[j];
				if (attachment_v.get_type() != Variant::DICTIONARY) continue;
				const Dictionary attachment_d = attachment_v;
				const int full_child_id =
						static_cast<int>(attachment_d.get("item_id", 0));
				const int child_type_id = full_child_id - 100000;
				if (child_type_id <= 0) continue;
				opennova::mission::ItemEmplacementAttachmentSpec attachment;
				attachment.child_type_id =
						static_cast<int32_t>(child_type_id);
				attachment.kind =
						static_cast<opennova::mission::EmplacementAttachmentKind>(
								std::clamp(static_cast<int>(
										attachment_d.get("kind", 0)), 0, 2));
				attachment.stored_slot = static_cast<uint8_t>(
						std::clamp(static_cast<int>(
								attachment_d.get("stored_slot", 0)), 0, 4));
				if (static_cast<bool>(
						attachment_d.get("designated_c", false)))
					attachment.attachment_flags |= 1;
				if (static_cast<bool>(
						attachment_d.get("designated_g", false)))
					attachment.attachment_flags |= 2;
				attachment.anchor_found = static_cast<bool>(
						attachment_d.get("anchor_found", false));
				attachment.anchor.type =
						opennova::world::SeatType::Gunner;
				attachment.anchor.bone_index = static_cast<uint8_t>(
						std::clamp(static_cast<int>(
								attachment_d.get("bone_index", 0)), 0, 255));
				attachment.anchor.source_name =
						String(attachment_d.get(
								"source_name", String())).utf8().get_data();
				const Vector3 local =
						attachment_d.get("local", Vector3());
				attachment.anchor.seat_local = {
						static_cast<float>(local.x),
						static_cast<float>(local.y),
						static_cast<float>(local.z)};
				attachment.anchor.yaw_offset = static_cast<int16_t>(
						std::clamp(static_cast<int>(
								attachment_d.get("yaw_offset", 0)),
								-32768, 32767));
				attachment.angle_count = static_cast<uint8_t>(
						static_cast<int>(
								attachment_d.get("angle_count", 0)) == 4
								? 4 : 0);
				attachment.down_limit_bam = static_cast<int32_t>(
						attachment_d.get("down_limit_bam", 0));
				attachment.up_limit_bam = static_cast<int32_t>(
						attachment_d.get("up_limit_bam", 0));
				attachment.right_limit_bam = static_cast<int32_t>(
						attachment_d.get("right_limit_bam", 0));
				attachment.left_limit_bam = static_cast<int32_t>(
						attachment_d.get("left_limit_bam", 0));
				spec.emplacement_attachments.push_back(
						std::move(attachment));
			}
		}
		// The attach-label sources: "armory*" userpoint locals (Armory-attrib items
		// only — the host gates on itemdef attrib 0x80000) + the ewep primary_weapon
		// link [orig: @0x4361ee/@0x5a36f5; ItemDef+0x54B].
		const Variant armory_v = spec_d.get("armory_points", Array());
		if (armory_v.get_type() == Variant::ARRAY) {
			const Array armory_a = armory_v;
			for (int64_t j = 0; j < armory_a.size(); ++j) {
				if (armory_a[j].get_type() != Variant::VECTOR3) continue;
				const Vector3 p = armory_a[j];
				spec.armory_points.push_back({static_cast<float>(p.x),
				                              static_cast<float>(p.y),
				                              static_cast<float>(p.z)});
			}
		}
		spec.primary_weapon =
		    String(spec_d.get("primary_weapon", String())).utf8().get_data();
		if (spec.mount_config_valid || !spec.seats.empty() || !spec.armory_points.empty() ||
		    !spec.primary_weapon.empty() || !spec.emplacement_attachments.empty())
			item_seat_specs_.push_back(std::move(spec));
	}
}

void NovaSimulation::set_terrain_height_field(const Ref<NovaTerrainData> &p_terrain) {
	// Clear first so a null/unloaded terrain disables grounding.
	terrain_heightmap_.clear();
	terrain_sector_grid_.clear();
	terrain_field_ = opennova::terrain::TerrainHeightField{};

	surface_indices_.clear();
	surface_map_ = opennova::terrain::SurfaceTypeMap{};

	if (p_terrain.is_valid() && p_terrain->is_loaded()) {
		const opennova::CptFile &cpt = p_terrain->get_cpt();
		const opennova::TrnConfig &trn = p_terrain->get_trn();
		if (!cpt.depth_buffer.empty()) {
			terrain_heightmap_ = cpt.depth_buffer; // own a copy (outlives the source resource)
			terrain_sector_grid_.resize(256);
			const int *grid = &trn.sector_grid[0][0];
			for (int i = 0; i < 256; ++i) terrain_sector_grid_[i] = grid[i];

			terrain_field_.heightmap = terrain_heightmap_.data();
			terrain_field_.dim = static_cast<int>(std::sqrt(static_cast<double>(terrain_heightmap_.size())));
			terrain_field_.layout.sector_grid = terrain_sector_grid_.data();
			terrain_field_.layout.origin_x = trn.origin_x;
			terrain_field_.layout.origin_y = trn.origin_y;
			// Water clamp deferred: water_height units (vs the 16.16 worldY @0x26C6454 the original
			// compares) are not yet verified, so leave has_water off rather than float entities onto
			// a wrong plane. The ground-following path (the Phase 1 goal) does not need it.
			terrain_field_.has_water = false;

			// The charmap surface raster for the footstep surface pick (own a
			// copy like the depth buffer; shares the sector grid + origins)
			// [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510].
			const std::vector<uint8_t> &charmap = p_terrain->get_charmap_indices();
			if (!charmap.empty() && p_terrain->get_charmap_width() > 0) {
				surface_indices_ = charmap;
				surface_map_.data = surface_indices_.data();
				surface_map_.width = p_terrain->get_charmap_width();
				surface_map_.height = p_terrain->get_charmap_height();
				surface_map_.sector_grid = terrain_sector_grid_.data();
				surface_map_.origin_x = trn.origin_x;
				surface_map_.origin_y = trn.origin_y;
			}
		}
	}
	apply_terrain_to_ai();
}

void NovaSimulation::set_sound_profiles(const PackedByteArray &p_sndprof_text) {
	sndprof_text_.assign(p_sndprof_text.ptr(), p_sndprof_text.ptr() + p_sndprof_text.size());
	apply_sound_state_to_world();
}

void NovaSimulation::set_water_z(double p_water_y) {
	env_water_z_q16_ = static_cast<int32_t>(p_water_y * 65536.0);
	if (world_) world_->env.water_z = env_water_z_q16_;
}

Array NovaSimulation::drain_slot_sounds() {
	Array out;
	if (!loaded_) return out;
	for (const opennova::world::SoundSlotEvent &ev : world_->slot_sounds) {
		Dictionary d;
		d["set"] = String(ev.set_name);
		// Mission-frame 16.16 -> godot (x, z, -y), same mapping as the fire drain.
		d["pos"] = Vector3(static_cast<float>(ev.pos[0]) / 65536.0f,
		                   static_cast<float>(ev.pos[2]) / 65536.0f,
		                   static_cast<float>(-ev.pos[1]) / 65536.0f);
		d["handle"] = ev.source_handle;
		d["slot"] = ev.slot;
		out.push_back(d);
	}
	world_->slot_sounds.clear();
	return out;
}

void NovaSimulation::finish_load(const opennova::bms::File &file) {
	// One world, three systems, the faithful tick order. The AI-change action family reaches
	// brains through World::ai; wire it before registering so the pre-mission pass can dispatch.
	bms_->load(file.events, file.triggers, file.actions);
	// Mission attribute flags -> the world (0x40 = SinglePlayerRespawn gates the SP
	// death auto-lose in check_win_conditions). [orig: Bms_AttribFlags @0xa76258,
	// read by Server_CheckWinConditions @0x51ad6f]
	world_->mission_attrib_flags = static_cast<uint32_t>(file.header.attrib_flags);
	reset_local_player_view_effects();
	world_->ai = ai_.get();
	// P7 listen server (SP + LAN host): stand up the npruntime in-match runtime (mode-3 HostClient
	// over an in-process loopback, the faithful §5.0 path). Server_TickUpdate owns the logic tick +
	// the C2S drain + the 0x0A fan, so there is NO net ISystem here (D-NET-123/125) — the
	// present reads the host's own ClientRuntime view, and a LAN host adds the socket legs in host_pump.
	if (listen_server_) {
		bringup_host_runtime(file);
	}
	// P7 co-op LAN joiner: a pure non-authority client. Build a fresh np::ClientRuntime (Joiner role)
	// per (re)load — start() fully resets the session, so a reload reconnects cleanly. No net ISystem
	// is registered (the joiner never serializes; run_logic_tick(false) leaves World::net the default
	// LocalSink for WAC/BMS sinks). The local player L is spawned in joiner_pump on the name-match.
	if (joiner_) {
		runtime_ = std::make_unique<opennova::np::ClientRuntime>(
				opennova::ClientSession::Config::jointoperations(), joiner_player_name_);
		joiner_started_ = false;
		joiner_local_spawned_ = false;
		joiner_self_wire_handle_ = 0;
	}
	// Re-arm the fresh runtime's decode view with the items.def class table (built by a
	// prior resolve_item_traits; the shell also re-resolves per load, which re-installs).
	install_item_class_resolver();
	opennova::mission::register_mission_systems(*world_, *wac_, *bms_, *ai_);
	// Re-install the held script program onto the fresh WacSystem (reset_world
	// recreated it). The 62-tick execution divider stays inside the system
	// [orig: dword_C6EAD4 / cmp 0x3E]; a missing program leaves the VM unloaded
	// and its tick early-outs, the BMS-only case.
	if (wac_program_.is_valid() && wac_program_->is_ok()) {
		wac_->set_program(wac_program_->native_program());
	}
	// PreMission events settle initial scripted state before the clock starts (AI is skipped on
	// the pre-mission pass). Snapshot AFTER it so Stop restores the true play-start state.
	world_->run_logic_tick(/*is_authority=*/true, /*pre_mission=*/true);
	baseline_ = world_->snapshot();
	ai_->capture_spawn_baseline();
	have_baseline_ = true;
	loaded_ = true;
}

void NovaSimulation::apply_host_session_mission_header(const opennova::bms::File &file) {
	std::vector<uint8_t> header_blob;
	std::string error;
	if (opennova::bms::encode_header_blob(file, header_blob, error)) {
		host_session_config_.mission_header_blob = std::move(header_blob);
	} else {
		host_session_config_.mission_header_blob.clear();
	}

	const std::string mission_name = file.get_mission_name();
	if (!mission_name.empty()) {
		host_session_config_.mission_name = mission_name;
		if (host_session_config_.spawn_names.empty()) {
			host_session_config_.spawn_names.push_back(mission_name);
		}
	}
	// P7: host_session_config_ is consumed at the next load by bringup_host_runtime
	// (configure_session_runtime + the §5.1 reactive-reply config); nothing to refresh live.
}

void NovaSimulation::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_from_mission_data", "mission"), &NovaSimulation::load_from_mission_data);
	ClassDB::bind_method(D_METHOD("load_mission_file", "path"), &NovaSimulation::load_mission_file);
	ClassDB::bind_method(D_METHOD("build_demo_mission"), &NovaSimulation::build_demo_mission);
	ClassDB::bind_method(D_METHOD("is_loaded"), &NovaSimulation::is_loaded);
	ClassDB::bind_method(D_METHOD("set_playing", "playing"), &NovaSimulation::set_playing);
	ClassDB::bind_method(D_METHOD("is_playing"), &NovaSimulation::is_playing);
	ClassDB::bind_method(D_METHOD("step"), &NovaSimulation::step);
	ClassDB::bind_method(D_METHOD("restart"), &NovaSimulation::restart);
	ClassDB::bind_method(D_METHOD("enable_listen_server", "enable"), &NovaSimulation::enable_listen_server);
	ClassDB::bind_method(D_METHOD("set_terrain_til_data", "til_bytes"), &NovaSimulation::set_terrain_til_data);
	ClassDB::bind_method(D_METHOD("is_listen_server"), &NovaSimulation::is_listen_server);
	ClassDB::bind_method(D_METHOD("enable_host_listen", "port"), &NovaSimulation::enable_host_listen);
	ClassDB::bind_method(D_METHOD("is_host_listening"), &NovaSimulation::is_host_listening);
	ClassDB::bind_method(D_METHOD("get_host_listen_port"), &NovaSimulation::get_host_listen_port);
	ClassDB::bind_method(D_METHOD("get_host_peer_count"), &NovaSimulation::get_host_peer_count);
	ClassDB::bind_method(D_METHOD("configure_host_session", "options"), &NovaSimulation::configure_host_session);
	ClassDB::bind_method(D_METHOD("get_host_session_config"), &NovaSimulation::get_host_session_config);
	ClassDB::bind_method(D_METHOD("admit_test_remote_peer", "position", "yaw_deg", "team"), &NovaSimulation::admit_test_remote_peer);
	ClassDB::bind_method(D_METHOD("enable_join", "host_ip", "port", "player_name"), &NovaSimulation::enable_join);
	ClassDB::bind_method(D_METHOD("is_joiner"), &NovaSimulation::is_joiner);
	ClassDB::bind_method(D_METHOD("is_joined_in_match"), &NovaSimulation::is_joined_in_match);
	ClassDB::bind_method(D_METHOD("get_joiner_phase"), &NovaSimulation::get_joiner_phase);
	ClassDB::bind_method(D_METHOD("get_joiner_self_handle"), &NovaSimulation::get_joiner_self_handle);
	ClassDB::bind_method(D_METHOD("spawn_local_player", "position", "yaw_deg", "team"), &NovaSimulation::spawn_local_player);
	ClassDB::bind_method(D_METHOD("spawn_local_player_at_start"), &NovaSimulation::spawn_local_player_at_start);
	ClassDB::bind_method(D_METHOD("has_local_player"), &NovaSimulation::has_local_player);
	ClassDB::bind_method(D_METHOD("get_local_player_wire_handle"), &NovaSimulation::get_local_player_wire_handle);
	ClassDB::bind_method(D_METHOD("set_player_input", "forward", "back", "left", "right", "lean_left", "lean_right", "jump"), &NovaSimulation::set_player_input);
	ClassDB::bind_method(D_METHOD("add_local_player_look", "dx_px", "dy_px"), &NovaSimulation::add_local_player_look);
	ClassDB::bind_method(D_METHOD("set_local_player_mouse", "sensitivity", "invert_y"), &NovaSimulation::set_local_player_mouse);
	ClassDB::bind_method(D_METHOD("request_local_player_stance", "stance"), &NovaSimulation::request_local_player_stance);
	ClassDB::bind_method(D_METHOD("get_local_player_position"), &NovaSimulation::get_local_player_position);
	ClassDB::bind_method(D_METHOD("get_waypoint_hud_view"), &NovaSimulation::get_waypoint_hud_view);
	ClassDB::bind_method(D_METHOD("get_objectives_view"), &NovaSimulation::get_objectives_view);
	ClassDB::bind_method(D_METHOD("get_local_player_yaw_deg"), &NovaSimulation::get_local_player_yaw_deg);
	ClassDB::bind_method(D_METHOD("get_local_player_pitch_deg"), &NovaSimulation::get_local_player_pitch_deg);
	ClassDB::bind_method(D_METHOD("get_local_player_body_anim_slot"), &NovaSimulation::get_local_player_body_anim_slot);
	ClassDB::bind_method(D_METHOD("get_local_player_anim_key"), &NovaSimulation::get_local_player_anim_key);
	ClassDB::bind_method(D_METHOD("get_local_player_anim_phase_ticks"), &NovaSimulation::get_local_player_anim_phase_ticks);
	ClassDB::bind_method(D_METHOD("get_local_player_aim_overlay"), &NovaSimulation::get_local_player_aim_overlay);
	ClassDB::bind_method(
			D_METHOD("set_local_player_weapon", "def", "clip_seconds",
					"preserve_slot_state"),
			&NovaSimulation::set_local_player_weapon, DEFVAL(false));
	ClassDB::bind_method(
			D_METHOD("rebake_local_player_weapon", "def", "clip_seconds",
					"preserve_slot_state"),
			&NovaSimulation::rebake_local_player_weapon, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("clear_local_player_weapon"), &NovaSimulation::clear_local_player_weapon);
	ClassDB::bind_method(D_METHOD("set_local_player_first_person_model_available", "available"),
			&NovaSimulation::set_local_player_first_person_model_available);
	ClassDB::bind_method(D_METHOD("set_local_player_weapon_input", "fire_held", "fire_pressed", "reload_pressed"), &NovaSimulation::set_local_player_weapon_input);
	ClassDB::bind_method(D_METHOD("request_local_player_scope_toggle"), &NovaSimulation::request_local_player_scope_toggle);
	ClassDB::bind_method(D_METHOD("request_local_player_binoculars_toggle"),
			&NovaSimulation::request_local_player_binoculars_toggle);
	ClassDB::bind_method(D_METHOD("request_local_player_nvg_toggle"),
			&NovaSimulation::request_local_player_nvg_toggle);
	ClassDB::bind_method(D_METHOD("request_local_player_nvg_gain", "delta"),
			&NovaSimulation::request_local_player_nvg_gain);
	ClassDB::bind_method(D_METHOD("set_local_player_eye", "eye_godot", "valid"), &NovaSimulation::set_local_player_eye);
	ClassDB::bind_method(D_METHOD("set_local_player_camera_third_person", "third_person"), &NovaSimulation::set_local_player_camera_third_person);
	ClassDB::bind_method(D_METHOD("get_local_player_view"), &NovaSimulation::get_local_player_view);
	ClassDB::bind_static_method("NovaSimulation", D_METHOD("fov_vertical_from_horizontal", "fov_h_deg", "aspect"), &NovaSimulation::fov_vertical_from_horizontal);
	ClassDB::bind_method(D_METHOD("get_local_player_weapon_state"), &NovaSimulation::get_local_player_weapon_state);
	ClassDB::bind_method(D_METHOD("drain_local_player_weapon_events"), &NovaSimulation::drain_local_player_weapon_events);
	ClassDB::bind_method(D_METHOD("drain_round_impacts"), &NovaSimulation::drain_round_impacts);
	ClassDB::bind_method(D_METHOD("get_local_player_health"), &NovaSimulation::get_local_player_health);
	ClassDB::bind_method(D_METHOD("get_local_player_max_health"), &NovaSimulation::get_local_player_max_health);
	ClassDB::bind_method(D_METHOD("get_local_player_team"), &NovaSimulation::get_local_player_team);
	ClassDB::bind_method(D_METHOD("get_local_player_class"), &NovaSimulation::get_local_player_class);
	ClassDB::bind_method(D_METHOD("get_local_player_weapon_name"), &NovaSimulation::get_local_player_weapon_name);
	ClassDB::bind_method(D_METHOD("drain_effects"), &NovaSimulation::drain_effects);
	ClassDB::bind_method(D_METHOD("drain_fire_presentation_events"),
			&NovaSimulation::drain_fire_presentation_events);
	ClassDB::bind_method(D_METHOD("get_tracer_trails"), &NovaSimulation::get_tracer_trails);
	ClassDB::bind_method(D_METHOD("drain_destruction_events"),
			&NovaSimulation::drain_destruction_events);
	ClassDB::bind_method(D_METHOD("get_death_pieces"), &NovaSimulation::get_death_pieces);
	ClassDB::bind_method(D_METHOD("get_destruction_debug", "bms_id"),
			&NovaSimulation::get_destruction_debug);
	ClassDB::bind_method(D_METHOD("set_sound_profiles", "sndprof_text"),
			&NovaSimulation::set_sound_profiles);
	ClassDB::bind_method(D_METHOD("set_water_z", "water_y"), &NovaSimulation::set_water_z);
	ClassDB::bind_method(D_METHOD("drain_slot_sounds"), &NovaSimulation::drain_slot_sounds);
	ClassDB::bind_method(D_METHOD("set_wac_program", "program"), &NovaSimulation::set_wac_program);
	ClassDB::bind_method(D_METHOD("get_wac_program"), &NovaSimulation::get_wac_program);
	ClassDB::bind_method(D_METHOD("compile_and_set_wac", "sources"), &NovaSimulation::compile_and_set_wac);
	ClassDB::bind_method(D_METHOD("get_wac_state"), &NovaSimulation::get_wac_state);
	ClassDB::bind_method(D_METHOD("get_runtime_perf_counters"), &NovaSimulation::get_runtime_perf_counters);
	ClassDB::bind_method(D_METHOD("set_wac_paused", "paused"), &NovaSimulation::set_wac_paused);
	ClassDB::bind_method(D_METHOD("is_wac_paused"), &NovaSimulation::is_wac_paused);
	ClassDB::bind_method(D_METHOD("set_mission_variable", "index", "value"), &NovaSimulation::set_mission_variable);
	ClassDB::bind_method(D_METHOD("get_mission_variable", "index"), &NovaSimulation::get_mission_variable);
	ClassDB::bind_method(D_METHOD("has_event_fired", "index"), &NovaSimulation::has_event_fired);
	ClassDB::bind_method(D_METHOD("get_event_count"), &NovaSimulation::get_event_count);
	ClassDB::bind_method(D_METHOD("get_logic_tick"), &NovaSimulation::get_logic_tick);
	ClassDB::bind_method(D_METHOD("set_panm_time_ms", "time_ms"),
			&NovaSimulation::set_panm_time_ms);
	ClassDB::bind_method(D_METHOD("get_panm_time_ms"),
			&NovaSimulation::get_panm_time_ms);
	ClassDB::bind_method(D_METHOD("debug_set_panm_time_ms", "time_ms"),
			&NovaSimulation::debug_set_panm_time_ms);
	ClassDB::bind_method(D_METHOD("get_mission_variables_snapshot"), &NovaSimulation::get_mission_variables_snapshot);
	ClassDB::bind_method(D_METHOD("get_global_variables_snapshot"), &NovaSimulation::get_global_variables_snapshot);
	ClassDB::bind_method(D_METHOD("get_music_variables_snapshot"), &NovaSimulation::get_music_variables_snapshot);
	ClassDB::bind_method(D_METHOD("set_global_variable", "index", "value"), &NovaSimulation::set_global_variable);
	ClassDB::bind_method(D_METHOD("get_global_variable", "index"), &NovaSimulation::get_global_variable);
	ClassDB::bind_method(D_METHOD("get_fired_events_snapshot"), &NovaSimulation::get_fired_events_snapshot);
	ClassDB::bind_method(D_METHOD("get_entity_debug", "index"), &NovaSimulation::get_entity_debug);
	ClassDB::bind_method(D_METHOD("debug_set_entity_health", "index", "hp"), &NovaSimulation::debug_set_entity_health);
	ClassDB::bind_method(D_METHOD("debug_set_entity_position", "index", "mission_pos"), &NovaSimulation::debug_set_entity_position);
	ClassDB::bind_method(D_METHOD("get_world_entity_debug", "net_id"), &NovaSimulation::get_world_entity_debug);
	ClassDB::bind_method(D_METHOD("debug_set_world_entity_position", "net_id", "mission_pos"), &NovaSimulation::debug_set_world_entity_position);
	ClassDB::bind_method(D_METHOD("set_ai_muzzle_world", "net_id", "godot_pos"), &NovaSimulation::set_ai_muzzle_world);
	ClassDB::bind_method(D_METHOD("get_round_outcome_debug"), &NovaSimulation::get_round_outcome_debug);
	ClassDB::bind_static_method("NovaSimulation", D_METHOD("ai_state_name", "state"), &NovaSimulation::ai_state_name);
	ClassDB::bind_static_method("NovaSimulation", D_METHOD("infantry_anim_key", "state"), &NovaSimulation::infantry_anim_key);
	ClassDB::bind_static_method("NovaSimulation", D_METHOD("infantry_anim_flags", "state"), &NovaSimulation::infantry_anim_flags);
	ClassDB::bind_method(D_METHOD("get_entity_count"), &NovaSimulation::get_entity_count);
	ClassDB::bind_method(D_METHOD("get_entity_kind", "index"), &NovaSimulation::get_entity_kind);
	ClassDB::bind_method(D_METHOD("get_entity_index", "index"), &NovaSimulation::get_entity_index);
	ClassDB::bind_method(D_METHOD("get_entity_position", "index"), &NovaSimulation::get_entity_position);
	ClassDB::bind_method(D_METHOD("get_entity_yaw", "index"), &NovaSimulation::get_entity_yaw);
	ClassDB::bind_method(D_METHOD("get_entity_yaw_deg", "index"), &NovaSimulation::get_entity_yaw_deg);
	ClassDB::bind_method(D_METHOD("get_entity_state", "index"), &NovaSimulation::get_entity_state);
	ClassDB::bind_method(D_METHOD("get_entity_net_id", "index"), &NovaSimulation::get_entity_net_id);
	ClassDB::bind_method(D_METHOD("get_foliage_mask_anchor_positions"),
			&NovaSimulation::get_foliage_mask_anchor_positions);
	ClassDB::bind_method(D_METHOD("get_entity_effect_state_for_ssn", "ssn"),
	                     &NovaSimulation::get_entity_effect_state_for_ssn);
	ClassDB::bind_method(D_METHOD("get_present_effect_state_for_ssn", "ssn"),
	                     &NovaSimulation::get_present_effect_state_for_ssn);
	ClassDB::bind_method(D_METHOD("get_present_effect_state_for_wire_handle", "wire_handle"),
	                     &NovaSimulation::get_present_effect_state_for_wire_handle);
	ClassDB::bind_method(D_METHOD("get_present_effect_state_for_bms_id", "bms_id"),
	                     &NovaSimulation::get_present_effect_state_for_bms_id);
	ClassDB::bind_method(D_METHOD("get_present_effect_state_for_origin", "kind", "index"),
	                     &NovaSimulation::get_present_effect_state_for_origin);
	ClassDB::bind_method(D_METHOD("get_entity_bms_id", "index"), &NovaSimulation::get_entity_bms_id);
	ClassDB::bind_method(D_METHOD("get_entity_owner_connection_id", "index"),
	                     &NovaSimulation::get_entity_owner_connection_id);
	ClassDB::bind_method(D_METHOD("get_entity_wire_handle", "index"),
	                     &NovaSimulation::get_entity_wire_handle);
	ClassDB::bind_method(D_METHOD("get_entity_part_anim_phase", "index", "channel"), &NovaSimulation::get_entity_part_anim_phase);
	ClassDB::bind_method(D_METHOD("get_entity_part_anim_active", "index", "channel"), &NovaSimulation::get_entity_part_anim_active);
	ClassDB::bind_method(D_METHOD("get_entity_body_anim_slot", "index"), &NovaSimulation::get_entity_body_anim_slot);
	ClassDB::bind_method(D_METHOD("get_entity_hidden", "index"), &NovaSimulation::get_entity_hidden);
	ClassDB::bind_method(D_METHOD("get_present_snapshot"), &NovaSimulation::get_present_snapshot);
	ClassDB::bind_method(D_METHOD("get_present_stride"), &NovaSimulation::get_present_stride);
	ClassDB::bind_method(D_METHOD("set_terrain_height_field", "terrain"), &NovaSimulation::set_terrain_height_field);
	ClassDB::bind_method(D_METHOD("set_item_seat_specs", "specs"), &NovaSimulation::set_item_seat_specs);
	ClassDB::bind_method(D_METHOD("set_infantry_anim_map", "resource_root", "adm_name"), &NovaSimulation::set_infantry_anim_map);
	ClassDB::bind_method(D_METHOD("resolve_infantry_adm_ids", "resource_root", "item_db"), &NovaSimulation::resolve_infantry_adm_ids);
	ClassDB::bind_method(D_METHOD("resolve_item_traits", "item_db"), &NovaSimulation::resolve_item_traits);
	ClassDB::bind_method(D_METHOD("resolve_ai_weapons", "item_db"), &NovaSimulation::resolve_ai_weapons);
	ClassDB::bind_method(D_METHOD("resolve_collision_instances", "item_db", "placer"),
	                     &NovaSimulation::resolve_collision_instances);
	ClassDB::bind_method(D_METHOD("occlusion_init_mission"),
	                     &NovaSimulation::occlusion_init_mission);
	ClassDB::bind_method(D_METHOD("run_occlusion_frame", "camera", "fov_y_deg", "aspect",
	                              "near", "fog_dist_units", "water_z_units", "force_indoors"),
	                     &NovaSimulation::run_occlusion_frame);
	ClassDB::bind_method(D_METHOD("get_building_visibility"),
	                     &NovaSimulation::get_building_visibility);
	ClassDB::bind_method(D_METHOD("get_render_culled_bms_ids"),
	                     &NovaSimulation::get_render_culled_bms_ids);
	ClassDB::bind_method(D_METHOD("occlusion_water_visible"),
	                     &NovaSimulation::occlusion_water_visible);
	ClassDB::bind_method(D_METHOD("occlusion_camera_indoors"),
	                     &NovaSimulation::occlusion_camera_indoors);
	ClassDB::bind_method(D_METHOD("get_collision_debug"), &NovaSimulation::get_collision_debug);
	ClassDB::bind_method(D_METHOD("get_occlusion_debug"), &NovaSimulation::get_occlusion_debug);
	ClassDB::bind_method(D_METHOD("get_round_debug"), &NovaSimulation::get_round_debug);
	ClassDB::bind_method(D_METHOD("get_throwable_visuals"), &NovaSimulation::get_throwable_visuals);
	ClassDB::bind_method(D_METHOD("debug_spawn_round", "from_godot", "dir_godot", "ammo_name"),
	                     &NovaSimulation::debug_spawn_round);
	ClassDB::bind_method(D_METHOD("get_hitbox_debug"), &NovaSimulation::get_hitbox_debug);
	ClassDB::bind_method(D_METHOD("get_occlusion_portal_debug", "anchor", "range_units"),
	                     &NovaSimulation::get_occlusion_portal_debug);
	ClassDB::bind_method(D_METHOD("local_player_indoors"), &NovaSimulation::local_player_indoors);
	ClassDB::bind_method(D_METHOD("local_player_blink_flags"), &NovaSimulation::local_player_blink_flags);
	ClassDB::bind_method(D_METHOD("compute_iris_samples", "cam_pos", "cam_forward", "light_dir"),
	                     &NovaSimulation::compute_iris_samples);
	ClassDB::bind_method(D_METHOD("sound_occlusion_distance_q16", "listener_pos", "source_pos",
	                              "distance_q16", "source_bms_id"),
	                     &NovaSimulation::sound_occlusion_distance_q16, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("local_player_in_armory_zone"), &NovaSimulation::local_player_in_armory_zone);
	ClassDB::bind_method(D_METHOD("local_player_in_vehicle_loadout_zone"), &NovaSimulation::local_player_in_vehicle_loadout_zone);
	ClassDB::bind_method(D_METHOD("local_player_toggle_mount"), &NovaSimulation::local_player_toggle_mount);
	ClassDB::bind_method(D_METHOD("get_attach_labels"), &NovaSimulation::get_attach_labels);
	ClassDB::bind_method(D_METHOD("apply_local_player_loadout", "kit", "player_class"),
	                     &NovaSimulation::apply_local_player_loadout);
	ClassDB::bind_method(D_METHOD("set_spawn_loadout", "kit", "filter_by_availability"),
	                     &NovaSimulation::set_spawn_loadout);
	ClassDB::bind_method(D_METHOD("has_explicit_spawn_loadout"),
	                     &NovaSimulation::has_explicit_spawn_loadout);
	ClassDB::bind_method(D_METHOD("set_weapon_availability", "pairs"),
	                     &NovaSimulation::set_weapon_availability);
	ClassDB::bind_method(D_METHOD("get_weapon_availability", "weapon_name"),
	                     &NovaSimulation::get_weapon_availability);
	ClassDB::bind_method(D_METHOD("respawn_local_player_loadout"),
	                     &NovaSimulation::respawn_local_player_loadout);
	ClassDB::bind_method(D_METHOD("set_local_player_class", "player_class"),
	                     &NovaSimulation::set_local_player_class);
	ClassDB::bind_method(D_METHOD("request_local_player_weapon_category", "category"),
	                     &NovaSimulation::request_local_player_weapon_category);
	ClassDB::bind_method(D_METHOD("request_local_player_weapon_cycle", "direction"),
	                     &NovaSimulation::request_local_player_weapon_cycle);
	ClassDB::bind_method(D_METHOD("get_local_player_inventory"),
	                     &NovaSimulation::get_local_player_inventory);
	ClassDB::bind_method(D_METHOD("get_local_player_loadout"),
	                     &NovaSimulation::get_local_player_loadout);
	ClassDB::bind_method(D_METHOD("load_weapon_table", "resource_root", "name"),
	                     &NovaSimulation::load_weapon_table, DEFVAL(String("weapon.def")));
	ClassDB::bind_method(D_METHOD("load_ammo_table", "resource_root", "name"),
	                     &NovaSimulation::load_ammo_table, DEFVAL(String("ammo.def")));
	ClassDB::bind_method(D_METHOD("get_infantry_clip_count"), &NovaSimulation::get_infantry_clip_count);
	ClassDB::bind_method(D_METHOD("set_loco_scale", "scale"), &NovaSimulation::set_loco_scale);
	ClassDB::bind_method(D_METHOD("get_loco_scale"), &NovaSimulation::get_loco_scale);
	ClassDB::bind_method(D_METHOD("get_spawned_count"), &NovaSimulation::get_spawned_count);
	ClassDB::bind_method(D_METHOD("get_brain_count"), &NovaSimulation::get_brain_count);

	// Present-snapshot field layout (single source of truth for the GDScript present pass).
	BIND_ENUM_CONSTANT(PF_KIND);
	BIND_ENUM_CONSTANT(PF_INDEX);
	BIND_ENUM_CONSTANT(PF_BMS_ID);
	BIND_ENUM_CONSTANT(PF_NET_ID);
	BIND_ENUM_CONSTANT(PF_POS_X);
	BIND_ENUM_CONSTANT(PF_POS_Y);
	BIND_ENUM_CONSTANT(PF_POS_Z);
	BIND_ENUM_CONSTANT(PF_PITCH_DEG);
	BIND_ENUM_CONSTANT(PF_YAW_DEG);
	BIND_ENUM_CONSTANT(PF_ROLL_DEG);
	BIND_ENUM_CONSTANT(PF_PHASE1);
	BIND_ENUM_CONSTANT(PF_ACTIVE1);
	BIND_ENUM_CONSTANT(PF_PHASE2);
	BIND_ENUM_CONSTANT(PF_ACTIVE2);
	BIND_ENUM_CONSTANT(PF_BODY_ANIM_SLOT);
	BIND_ENUM_CONSTANT(PF_ANIM_STATE);
	BIND_ENUM_CONSTANT(PF_ANIM_PHASE_TICKS);
	BIND_ENUM_CONSTANT(PF_ANIM_REMOTE_REQUEST);
	BIND_ENUM_CONSTANT(PF_HIDDEN);
	BIND_ENUM_CONSTANT(PF_LOCAL_VIEW_SUPPRESSED);
	BIND_ENUM_CONSTANT(PF_ALIVE);
	BIND_ENUM_CONSTANT(PF_RESPAWN_REVISION);
	BIND_ENUM_CONSTANT(PF_TYPE_ID);
	BIND_ENUM_CONSTANT(PF_WIRE_HANDLE);
	BIND_ENUM_CONSTANT(PF_AIM_OVERLAY_VALID);
	BIND_ENUM_CONSTANT(PF_AIM_BODY_PITCH_DEG);
	BIND_ENUM_CONSTANT(PF_AIM_BODY_YAW_DEG);
	BIND_ENUM_CONSTANT(PF_AIM_BODY_ROLL_DEG);
	BIND_ENUM_CONSTANT(PF_AIM_ANGLES);
	BIND_ENUM_CONSTANT(PF_AIM_CLASS_STRIDE);
	BIND_ENUM_CONSTANT(PF_EMPLACED_CONTROLS_VALID);
	BIND_ENUM_CONSTANT(PF_EWEAP_GUNYAW);
	BIND_ENUM_CONSTANT(PF_EWEAP_GUNPITCH);
	BIND_ENUM_CONSTANT(PF_RIGHT_HAND_COLLAPSED);
	BIND_ENUM_CONSTANT(PF_STRIDE);
	BIND_ENUM_CONSTANT(EFFECT_STATE_POSITION);
	BIND_ENUM_CONSTANT(EFFECT_STATE_ROTATION_DEG);
	BIND_ENUM_CONSTANT(EFFECT_STATE_COUNT);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "playing"), "set_playing", "is_playing");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "loco_scale"), "set_loco_scale", "get_loco_scale");
}

void NovaSimulation::_notification(int p_what) {
	if (p_what == NOTIFICATION_PROCESS && playing_ && loaded_) {
		step();
	}
}

bool NovaSimulation::load_from_mission_data(const Ref<NovaMissionData> &p_mission) {
	if (p_mission.is_null()) return false;
	reset_world();
	// The editor's live, in-memory mission (unsaved edits included).
	const opennova::bms::File &file = p_mission->native_document().bms_file();
	promo_ = opennova::mission::promote_mission(file, *world_, *ai_, promote_options());
	finish_load(file);
	apply_host_session_mission_header(file);
	return true;
}

bool NovaSimulation::load_mission_file(const String &path) {
	reset_world();
	opennova::bms::File file;
	std::string err;
	if (!opennova::bms::parse_file(std::string(path.utf8().get_data()), file, err)) {
		return false;
	}
	host_session_config_.mission_file = std::string(path.get_file().utf8().get_data());
	promo_ = opennova::mission::promote_mission(file, *world_, *ai_, promote_options());
	finish_load(file);
	apply_host_session_mission_header(file);
	return true;
}

void NovaSimulation::build_demo_mission() {
	reset_world();
	opennova::bms::File file = make_demo_mission();
	host_session_config_.mission_file = "demo.bms";
	promo_ = opennova::mission::promote_mission(file, *world_, *ai_, promote_options());
	finish_load(file);
	apply_host_session_mission_header(file);
}

bool NovaSimulation::step() {
	if (!loaded_) return false;
	// ONE logic tick (the original's 62 Hz engine tick). The WAC VM self-gates to every
	// 62nd tick and the BMS evaluator quarter-passes every 16th, inside their systems —
	// exactly where the original keeps those dividers. A host frame runs 0..N of these;
	// the accumulator that decides N lives in MissionRuntime.tick_realtime
	// [orig: Game_MainLoop @ 0x52b630].
	//
	// Listen-server frame order [orig: Game_ProcessMainFrame @ 0x5263f0]:
	//   input -> net(drain C2S) -> run_logic_tick(WAC/BMS/AI) -> net(emit S2C) -> present.
	// Server_TickUpdate owns the C2S drain at the top of the loop and the post-logic S2C fan;
	// host_pump drives it and the local client's decode happens via the host's ClientRuntime.
	const uint64_t sim_start = perf_now_us();
	if (listen_server_) { // P7 listen server (SP + LAN host) -> the npruntime owner loop
		host_pump();
		resolve_new_infantry_adm_ids();
		last_sim_tick_us_ = perf_now_us() - sim_start;
		return true;
	}
	if (joiner_) { // P7 co-op joiner -> the npruntime ClientRuntime (non-authority)
		joiner_pump();
		resolve_new_infantry_adm_ids();
		last_sim_tick_us_ = perf_now_us() - sim_start;
		return true;
	}
	// No-net editor/unit path: one authoritative logic tick, no replication.
	apply_player_input_pre_tick();
	world_->run_logic_tick(/*is_authority=*/true);
	sync_local_mounted_input_heading();
	tick_local_player_view();   // retail promotes the per-frame view before weapon actions
	tick_local_player_weapon(); // the equipped-slot FSM pump, after the view promoter
	resolve_new_infantry_adm_ids();
	last_sim_tick_us_ = perf_now_us() - sim_start;
	return true;
}

// The post-logic half of the listen-server frame: serialize the live world into one
// S2C 0x0A frame, loop it back in-process, and let the local client decode it into the
// ClientState the present pass reads. No-op when the listen server is off.
// P7: per-load host bring-up — the faithful §5.0 mode-3 in-process listen server
// [orig: SinglePlayer_StartMission @0x561af0], mirroring apps/nw_server/main.cpp. The host's own
// player AUTO-spawns through the real pipeline (Server_ProcessPendingPlayerSpawns ->
// select_player_spawn start marker), and its own loopback client renders the per-frame 0x0A.
void NovaSimulation::bringup_host_runtime(const opennova::bms::File &file) {
	namespace np = opennova::np;
	// Persist the mission so ctx_.mission (read by the §5.1 0x0B BMS-header burst for LAN joiners)
	// outlives the match — the load-local bms::File would dangle.
	mission_file_ = file;
	host_loop_.clear();
	// Reload: a fresh host_owner_ drops any stale connections / peers from a prior mission. A reload is a
	// new match (Stop -> load), so configure_session_runtime runs once per match (never mid-match,
	// D-NET-124). serve_and_play: host_session_pump must NOT discard the host's own loopback 0x0A — we
	// fold it into ClientState (runtime_) to render the host's own view.
	// Serve-and-play (default) vs dedicated. SP / editor preview are ALWAYS serve-and-play (they render
	// the host's own player); a LAN host honors the UI server-type (host_serve_and_play_, from
	// configure_host_session). A dedicated host (serve_and_play=false) skips the own-player spawn + the
	// local view below and lets host_session_pump discard the host loopback (step 5) — mirroring
	// start_host_session's gating [orig: SinglePlayer_StartMission @0x561af0].
	const bool serve_and_play = host_listen_ ? host_serve_and_play_ : true;
	host_owner_ = np::HostOwner{};
	host_owner_.host_loopback = &host_loop_;
	host_owner_.serve_and_play = serve_and_play;
	ctx_.world = world_.get();
	ctx_.mission = &mission_file_;
	ctx_.terrain_til_data = terrain_til_data_; // S2C 0x45 terrain-tile load source (empty => skipped, §5.37)
	// Server_TickUpdate owns the per-frame C2S drain + S2C fan over connection_list; there is no
	// separate net ISystem (retired P8).

	// The ONE consolidated GameConfig for create_session (ADR 0013): a LAN host takes its lobby name /
	// gametype / mission + the §5.1 reply slice from the GDScript-configured host_session_config_; SP is
	// the faithful "SINGLEPLAYERGAME" / 1 player. game_type (g_GameType) now feeds BOTH the S2C 0x08
	// block dword[3] AND the 0x7B/0x60 bodies (§6.9; NapiNPMsg_0x7B_BuildPayload @0x507740).
	np::GameConfig host_config;
	if (host_listen_) {
		host_config = host_session_config_; // mission/player/spawn + game_type/mp_attributes from the UI
		if (host_config.server_name.empty()) host_config.server_name = "OpenNova LAN Host";
		host_config.max_players = host_max_players_; // the UI player cap (configure_host_session clamped 1..65)
	} else {
		host_config.server_name = "SINGLEPLAYERGAME";
		host_config.max_players = 1;
	}
	if (world_) {
		world_->fat_bullets = host_config.fat_bullets;
		world_->one_shot_kill = host_config.one_shot_kill;
	}

	// The witnessed §5.0 listen-host bring-up, dedup'd to the ONE shared helper start_host_session
	// (mode 3 -> set_transport_mode -> create_session(&host_loop_) [+ Server_InitNewRoundState] ->
	// configure_session_runtime; then, when serve_and_play, FAITHFUL auto-spawn of the host's own player
	// at the start marker + latch its loopback in-match so Server_TickUpdate fans it the per-frame
	// whole-world 0x0A its local view renders from). host_owner_.host_loopback / .serve_and_play + ctx_.world
	// were set above; this replaces the copy that had drifted out of the helper. [orig: SinglePlayer_StartMission
	// @0x561af0]. The one GameConfig carries the §5.1 reactive-reply config for the joiner replies too.
	np::HostConfig host_cfg;
	host_cfg.config = host_config;
	host_cfg.socket_mode = host_listen_ ? np::SocketMode::Lan : np::SocketMode::Socketless;
	host_cfg.serve_and_play = serve_and_play;
	np::start_host_session(host_owner_, host_cfg);
	if (serve_and_play) {
		// The host's own client view (HostClient role: recv-fold only, 0x0C suppressed). Folds host_loop_
		// each frame into the ClientState the present pass reads.
		runtime_ = std::make_unique<np::ClientRuntime>(host_loop_);
		// Phase-3 0x0A objective width is gated by the same g_GameType
		// carried to remote clients in 0x7B extra; the local loopback has no
		// handshake, so seed its view directly from the consolidated config.
		runtime_->view().set_game_type(host_config.game_type);

		// Seed the look heading from the auto-spawned player's facing so the body starts aligned (the
		// motor drives entity Yaw from player_input_.look_heading each frame, else input snaps it to 0).
		player_input_ = opennova::world::PlayerInput{};
		stance_latch_ = 0;
		look_px_accum_x_ = look_px_accum_y_ = 0.0f;
		if (world_->ai && world_->cached.local_player.valid()) {
			if (const AiEntity *pe = world_->ai->for_handle(world_->cached.local_player)) {
				player_input_.look_heading = pe->heading;
			}
		}
	} else {
		// Dedicated (UI "serve only"). The witnessed original makes this a true host-only session
		// [orig: HG_SERVEONLY -> CGameSession_SetConnectionMode(1), is_host=1/is_client=0; HostDialog
		// read @0x555940, dispatch @0x556d00, mode switch @0x4c49f0]. We instead keep the single mode-3
		// listen-server path (ADR 0011) with serve_and_play=false — wire-equivalent to the joiner (mode 1
		// vs 3 changes only the host's OWN client bookkeeping, never the S2C stream a peer receives),
		// tracked as a divergence (docs/net/novaworld-net-re.md, D-NET-131). No host player spawns (a slot
		// fills lazily via tick_connections if a peer needs it) and host_session_pump discards the host
		// loopback (step 5): there is no local view, so runtime_ stays null — host_pump's fold and the
		// present snapshot both guard on it.
		runtime_.reset();
	}
}

namespace {
// NovaUdpPump-backed netsim::IDatagramSocket — the Godot adapter the shared host owner loop pumps. A
// null/closed pump (pure SP) yields recv 0 / send no-op, so the loop's socket legs go inert exactly as
// the old host_listen_-gated code did. PeerAddr <-> "a.b.c.d" uses the LE octet packing PeerAddr
// documents (octet 0 in the low byte; 127.0.0.1 -> 0x0100007F) — the conversion formerly in
// peer_from_addr / send_datagram.
class NovaUdpPumpDatagramSocket : public opennova::netsim::IDatagramSocket {
public:
	explicit NovaUdpPumpDatagramSocket(NovaUdpPump *pump) : pump_(pump) {}

	int recv_from(uint8_t *buf, std::size_t cap, opennova::PeerAddr &from) override {
		if (pump_ == nullptr || !pump_->is_open()) return 0;
		if (!pump_->has_inbound()) {
			pump_->poll();
			if (!pump_->has_inbound()) return 0;
		}
		const Dictionary d = pump_->take_inbound();
		const String ip = d.get("ip", String());
		const int port = d.get("port", 0);
		const PackedByteArray bytes = d.get("bytes", PackedByteArray());
		uint32_t packed = 0;
		const PackedStringArray parts = ip.split(".");
		if (parts.size() == 4) {
			packed = static_cast<uint32_t>(parts[0].to_int() & 0xFF) |
			         (static_cast<uint32_t>(parts[1].to_int() & 0xFF) << 8) |
			         (static_cast<uint32_t>(parts[2].to_int() & 0xFF) << 16) |
			         (static_cast<uint32_t>(parts[3].to_int() & 0xFF) << 24);
		}
		from = opennova::PeerAddr{packed, static_cast<uint16_t>(port)};
		const std::size_t n = std::min(cap, static_cast<std::size_t>(bytes.size()));
		if (n > 0) std::memcpy(buf, bytes.ptr(), n);
		return static_cast<int>(n);
	}

	void send_to(const opennova::PeerAddr &to, const uint8_t *data, std::size_t len) override {
		if (pump_ == nullptr || !pump_->is_open() || len == 0) return;
		char ipbuf[32];
		std::snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", to.ip & 0xFFu, (to.ip >> 8) & 0xFFu,
		              (to.ip >> 16) & 0xFFu, (to.ip >> 24) & 0xFFu);
		PackedByteArray bytes;
		bytes.resize(static_cast<int64_t>(len));
		std::memcpy(bytes.ptrw(), data, len);
		pump_->send_to(String(ipbuf), to.port, bytes);
	}

private:
	NovaUdpPump *pump_;
};
} // namespace

// P7/A5: the per-frame host owner loop is now a THIN delegation to the shared core host_session_pump
// (libs/npruntime) — the SAME loop apps/nw_server runs, so the headless server and the Godot host can no
// longer drift. NovaSimulation supplies the socket (a NovaUdpPump adapter; SP passes a null pump and the
// loop's socket legs go inert) and folds the host's own loopback 0x0A into ClientState for the present
// pass (serve_and_play: host_session_pump skips the loopback discard so we can read it here).
void NovaSimulation::resolve_infantry_adm_before_server_tick(void *p_context) {
	if (p_context == nullptr) return;
	static_cast<NovaSimulation *>(p_context)->resolve_new_infantry_adm_ids();
}

void NovaSimulation::host_pump() {
	namespace np = opennova::np;
	const uint32_t now = host_owner_.now_tick;
	apply_player_input_pre_tick(); // input -> the host player's body input, before logic (ADR 0009/0012)
	NovaUdpPumpDatagramSocket sock(host_listen_ ? pump_.ptr() : nullptr);
	np::host_session_pump(host_owner_, sock,
			&NovaSimulation::resolve_infantry_adm_before_server_tick, this);
	sync_local_mounted_input_heading();
	tick_local_player_view();   // retail promotes the per-frame view before weapon actions
	tick_local_player_weapon(); // the equipped-slot FSM pump, after the view promoter
	if (runtime_) runtime_->Client_ProcessNetworkFrame(now); // fold host_loop_ -> ClientState (HostClient view)
}

// host_pump's dispatch_event + admit_peer were promoted into libs/npruntime (np::dispatch_event /
// np::admit_peer over host_owner_, driven by host_session_pump) — the SAME code apps/nw_server runs, so
// the Godot host and the headless server can no longer drift.

// P7: the per-frame non-authority client loop — the Godot equivalent of the joiner half of
// Client_ProcessNetworkFrame (§5.44). The recv-fold + the C2S 0x0C uplink are fused inside the
// runtime; run_logic_tick(false) runs first so the uplink reflects L's just-integrated pose
// (matches the legacy "uplink-after-tick"). The recv-fold writes only ClientState (read after the
// frame), so folding it after the motor is harmless.
void NovaSimulation::joiner_pump() {
	namespace np = opennova::np;
	if (!runtime_) return;
	const uint32_t now = now_tick_;
	// ClientHello once (Idle -> Hello) the first armed frame.
	if (!joiner_started_) {
		const std::vector<uint8_t> hello = runtime_->start();
		if (!hello.empty()) ship_to_host(hello);
		joiner_started_ = true;
	}
	// Deposit received framed datagrams for this frame's recv pump.
	if (pump_.is_valid()) {
		pump_->poll();
		while (pump_->has_inbound()) {
			const Dictionary d = pump_->take_inbound();
			const PackedByteArray bytes = d.get("bytes", PackedByteArray());
			runtime_->receive(bytes.ptr(), static_cast<std::size_t>(bytes.size()));
		}
	}
	apply_player_input_pre_tick();                  // input -> L's body input
	world_->run_logic_tick(/*is_authority=*/false); // local World tick: moves L's motor ONLY (never Server_TickUpdate)
	sync_local_mounted_input_heading();
	tick_local_player_view();   // retail promotes the per-frame view before weapon actions
	tick_local_player_weapon(); // the equipped-slot FSM pump, after the view promoter

	// Run the client frame: recv-fold (-> ClientState) + connect-drive + the C2S 0x0C uplink (gated
	// InMatch && deployed inside the runtime). Build the uplink from L once it exists.
	const uint32_t health_updates_before =
			runtime_->state().health_updates_applied;
	const uint32_t objective_updates_before =
			runtime_->state().objective_updates_applied;
	std::vector<std::vector<uint8_t>> outs;
	const bool have_L = joiner_local_spawned_ && world_->ai && world_->cached.local_player.valid();
	const opennova::world::Entity *e = have_L ? world_->registry.get(world_->cached.local_player) : nullptr;
	const opennova::world::AiEntity *ae = have_L ? world_->ai->for_handle(world_->cached.local_player) : nullptr;
	if (e && ae) {
		const opennova::PlayerExtendedUplink up = opennova::netsim::build_player_uplink(*e, *ae);
		outs = runtime_->Client_ProcessNetworkFrame(up, now);
	} else {
		outs = runtime_->Client_ProcessNetworkFrame(now);
	}
	for (const std::vector<uint8_t> &dg : outs) ship_to_host(dg);
	const bool received_authoritative_health =
			runtime_->state().health_updates_applied != health_updates_before;
	const bool received_authoritative_objectives =
			runtime_->state().objective_updates_applied != objective_updates_before;
	if (received_authoritative_objectives) {
		const opennova::netsim::ClientState &client = runtime_->state();
		world_->subgoals.won = client.objective_won;
		world_->subgoals.lost = client.objective_lost;
		world_->subgoals.show_win = client.objective_show_win;
		world_->subgoals.show_lose = client.objective_show_lose;
	}

	// On reaching in-match (detected by the recv-fold above): learn H + spawn L at the host-advertised
	// pose. L is the joiner's OWN motor-driven pool-0 entity (publishes cached.local_player); H is the
	// wire identity the host knows us by — the two stay distinct, reconciled by the name-match (§5.38b).
	if (runtime_->in_match() && !joiner_local_spawned_ && world_->ai) {
		joiner_self_wire_handle_ = runtime_->self_handle();
		const np::JoinerConnection::SelfSpawn &sp = runtime_->spawn_pose();
		const opennova::world::PlayerSpawn spawn = spawn_from_self(sp);
		const opennova::world::EntityHandle h = opennova::world::spawn_player(*world_, spawn);
		joiner_local_spawned_ = h.valid();
		resolve_new_infantry_adm_ids();
		player_input_ = opennova::world::PlayerInput{};
		player_input_.look_heading = opennova::world::bam_heading_from_mission_yaw_deg(spawn.yaw);
		stance_latch_ = 0;
		look_px_accum_x_ = look_px_accum_y_ = 0.0f;
	}

	// The 0x0A tail is the authoritative health source for the recipient's OWN
	// player. H belongs to the host's handle space; apply that recipient-local
	// scalar to the joiner's distinct motor entity L without touching L's
	// predicted pose. A fresh-frame guard prevents ClientState's pre-frame zero
	// default from killing L during the handshake. Once L is dead, a later stale
	// positive tail is ignored: retail requires the separate deploy edge before
	// clearing Flags bit 1 and restoring health.
	// [orig: tail health read @0x430428; store to local Health @0x4305df]
	if (received_authoritative_health && joiner_local_spawned_ &&
			world_->cached.local_player.valid()) {
		const opennova::world::EntityHandle local_h =
				world_->cached.local_player;
		opennova::world::Entity *local =
				world_->registry.get(local_h);
		AiEntity *local_ai =
				world_->ai ? world_->ai->for_handle(local_h) : nullptr;
		if (local != nullptr && local_ai != nullptr) {
			// ClientRuntime latches deployment closed on any decoded zero tail,
			// even if a later packet in this recv pump carries stale positive HP.
			const int16_t health = runtime_->deployed()
					? runtime_->state().local_health : 0;
			if (health <= 0) {
				local->health = health;
				local_ai->health = health;
				local->alive = false;
				local->flags |= 2u;
			} else if (local->alive && (local->flags & 2u) == 0u) {
				local->health = health;
				local_ai->health = health;
			}
		}
	}
	++now_tick_;
}

void NovaSimulation::apply_player_input_pre_tick() {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return;
	AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p) return;
	refresh_local_player_view_effects();
	opennova::world::apply_player_body_input(*p, opennova::world::pack_player_body_input(player_input_));
	// The local-player weapon-channel inputs, refreshed before the body updater runs —
	// the per-tick re-read of the held AdmDefs record kind + the Flags-bit refresh
	// (Flags|0x10 from g_weaponScopeActive; the binoculars bit stays false until a host
	// binoculars input exists). [orig: @ 0x4b5d7f..0x4b5dc0]
	if (p->inf.active) {
		if (weapon_active_) {
			opennova::world::infantry_weapon_switch_stamp(
					p->inf, weapon_anim_map_serial_);
		}
		p->inf.wpn_hold_kind = weapon_active_ ? weapon_hold_kind_ : 0;
		p->inf.scope_raised = weapon_active_ && player_view_.scope_engaged;
		p->inf.binoculars_raised = player_view_.binoculars_raised;
		// The run-gait class + ForceCrouch mirror, same per-tick re-read pattern as the
		// hold kind [orig: the selection reads AdmDefs[+0x2B0]+0xAC each pass @ 0x4b72cf;
		// the ForceCrouch checks read the equipped def flags @ 0x4b7245/@ 0x4e0d8a].
		p->inf.wpn_run_anim = weapon_active_ ? weapon_run_anim_ : 0;
		p->inf.wpn_force_crouch = weapon_active_ && weapon_force_crouch_;
	}
	if (opennova::world::Entity *entity =
				world_->registry.get(world_->cached.local_player)) {
		uint32_t view_flags = 0;
		if (player_view_.nvg_active) view_flags |= 0x4u;
		if (player_view_.binoculars_raised) view_flags |= 0x8u;
		if (weapon_active_ && player_view_.scope_engaged) view_flags |= 0x10u;
		entity->flags = (entity->flags & ~0x1cu) | view_flags;
	}
}

void NovaSimulation::sync_local_mounted_input_heading() {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return;
	const opennova::world::Entity *player =
			world_->registry.get(world_->cached.local_player);
	const AiEntity *body = world_->ai->for_handle(world_->cached.local_player);
	if (player == nullptr || !player->mounted || body == nullptr ||
			!body->inf.is_local_player)
		return;

	// Entity_RequestVehicleAttach writes one retail Yaw before the relationship.
	// The core mirrors that snap into target_heading; carry it through our extra
	// host input record so apply_player_input_pre_tick cannot undo it next frame.
	// Pitch remains untouched because the retail request snap is yaw-only.
	player_input_.look_heading = body->inf.target_heading;
}

bool NovaSimulation::spawn_local_player(Vector3 p_position, float p_yaw_deg, int p_team) {
	if (!loaded_ || !world_ || !world_->ai) return false;
	// P7: the npruntime listen server auto-spawns the host's own player at bring-up (the faithful §5.0
	// mode-3 path), so an explicit spawn is a no-op success there. The legacy LAN host + any non-listen
	// caller (no auto-spawn) still spawn at the requested pose below.
	if (has_local_player()) return true;
	opennova::world::PlayerSpawn spawn;
	// Godot (x,y,z) -> mission (x, -z, y): the inverse of the present (x,y,z) -> (x, z, -y) remap.
	spawn.position = {static_cast<float>(p_position.x), static_cast<float>(-p_position.z),
	                  static_cast<float>(p_position.y)};
	spawn.yaw = static_cast<int16_t>(p_yaw_deg);
	spawn.team = static_cast<uint8_t>(p_team);
	if (listen_server_) spawn.min_entity_slot = kRetailPlayerMinEntitySlot;
	const opennova::world::EntityHandle h = opennova::world::spawn_player(*world_, spawn);
	if (!h.valid()) return false;
	resolve_new_infantry_adm_ids();
	// Seed the look heading to the spawn facing so the body starts aligned. [(90 - yaw) BAM]
	player_input_ = opennova::world::PlayerInput{};
	player_input_.look_heading = opennova::world::bam_heading_from_mission_yaw_deg(p_yaw_deg);
	stance_latch_ = 0;
	look_px_accum_x_ = look_px_accum_y_ = 0.0f;
	reset_local_player_view_effects();
	return true;
}

int NovaSimulation::spawn_local_player_at_start() {
	if (!loaded_ || !world_ || !world_->ai) return -1;
	// P7: the npruntime listen server auto-spawns the host's own player at bring-up via the SAME
	// select_player_spawn start-marker scan (Server_BuildPlayerInfoAndAdd), so when a player already
	// exists this is a no-op success (the player is at its start, input seeded by bringup_host_runtime).
	if (has_local_player()) return 1;
	// Pick the player-start marker the original would — scan the 60xx start-marker family (SP/DM,
	// coop, team), FARTHEST from the enemy set — instead of the first NPC's position. Finds the
	// authored start whatever the mission mode (e.g. a 6001-only SP training mission like 00TRa).
	// [orig: Server_PositionPlayerForSpawn @0x50cf60 -> Entity_FindBestSpawnPoint @0x50ccc0; net-re §5.2c]
	const opennova::world::SpawnPointResult sel = opennova::world::select_player_spawn(*world_);
	opennova::world::PlayerSpawn spawn;
	if (sel.found) {
		spawn.position = sel.position; // mission space, straight from the chosen marker
		spawn.yaw = sel.yaw;
	} else {
		// No player-start marker authored: spawn at the mission origin (the terrain clamp grounds
		// it). NEVER fall back to an NPC's position — that is the bug this replaces.
		spawn.position = {0.0f, 0.0f, 0.0f};
		spawn.yaw = 0;
	}
	// SP keeps the player's own team; the marker's team is not copied [orig: §5.2c]. Team 1 mirrors
	// the prior placeholder until the MP team path lands.
	spawn.team = 1;
	if (listen_server_) spawn.min_entity_slot = kRetailPlayerMinEntitySlot;
	const opennova::world::EntityHandle h = opennova::world::spawn_player(*world_, spawn);
	if (!h.valid()) return -1;
	resolve_new_infantry_adm_ids();
	// Seed the look heading to the spawn facing so the body starts aligned. [(90 - yaw) BAM]
	player_input_ = opennova::world::PlayerInput{};
	player_input_.look_heading = opennova::world::bam_heading_from_mission_yaw_deg(spawn.yaw);
	stance_latch_ = 0;
	look_px_accum_x_ = look_px_accum_y_ = 0.0f;
	reset_local_player_view_effects();
	return sel.found ? 1 : 0;
}

bool NovaSimulation::has_local_player() const {
	return world_ && world_->cached.local_player.valid();
}

int NovaSimulation::get_local_player_wire_handle() const {
	// The handle the wire stream knows the local player by. On a JOINER that is H (the
	// host-assigned wire identity), NOT the local sim handle L — L lives in the joiner's
	// own pool and collides with a host-side slot (e.g. the host player), so excluding L
	// from the wire present would wrongly hide a remote entity. On the host, the local
	// player's own pool-0 handle IS its wire handle.
	if (joiner_) return static_cast<int>(joiner_self_wire_handle_);
	return (world_ && world_->cached.local_player.valid())
			? static_cast<int>(world_->cached.local_player.packed) : 0;
}

void NovaSimulation::set_player_input(bool p_forward, bool p_back, bool p_left, bool p_right,
                                      bool p_lean_left, bool p_lean_right, bool p_jump) {
	player_input_.forward = p_forward;
	player_input_.back = p_back;
	player_input_.left = p_left;
	player_input_.right = p_right;
	// Lean keys -> MoveOrder bits 6/7 [orig: g_inputFlags 0x2000/0x4000 packed
	// @ 0x4df708-0x4df741]; jump is a per-frame edge the motor consumes once grounded.
	player_input_.lean_left = p_lean_left;
	player_input_.lean_right = p_lean_right;
	player_input_.jump = p_jump;
	// Stance comes from the sim-owned SELECT latches (request_local_player_stance —
	// the C2S 0x1D apply semantics [orig: @ 0x501c60]).
	player_input_.crouch = (stance_latch_ == 1);
	player_input_.prone = (stance_latch_ == 2);
	// The movement-held latch and the unscope-on-move [orig:
	// Player_PackInputStateToEntity @ 0x4df450 — any of the four direction keys
	// sets byte_B7653B (blocks scope-UP on Scoped weapons @ 0x4df29c) and, while
	// SETTLED at scope on a Scoped (flags 1) weapon, routes through
	// Player_ToggleWeaponScope @ 0x4df4c9..0x4df4ec = the full unscope. The
	// toggle's ForceScoped pin (@ 0x4df12d) keeps pinned sights raised].
	const bool move_held = p_forward || p_back || p_left || p_right;
	if (opennova::world::player_view_move_input(player_view_, move_held,
			weapon_active_ ? weapon_def_.flags : 0) &&
			(weapon_def_.flags & 0x20000000) == 0) {
		if (opennova::world::player_view_set_engaged(player_view_, false,
				(weapon_def_.flags2 & 0x200) != 0))
			opennova::world::weapon_fsm_queue_scope_down(
					*active_local_weapon_slot());
	}
	refresh_local_player_view_effects();
}

void NovaSimulation::add_local_player_look(float p_dx_px, float p_dy_px) {
	// The scoped sensitivity reduction divides by the CURRENT zoom magnification —
	// the slot zoom seeded from the def's scope_max_mag [orig: sens /
	// Player_GetClampedWeaponElevation() @ 0x499714, applied while scoped and the
	// binocular view is down; no binoculars input exists yet]. Engaged-at-scope is
	// the sim's own bit; the zoom-adjust keys are an unported tail, so the seed
	// (scope_max_mag) IS the current zoom.
	int32_t scoped_zoom = 0;
	if (!player_view_.binoculars_view_active && weapon_active_ &&
			player_view_.scope_engaged && weapon_scope_max_mag_ > 1.0f)
		scoped_zoom = static_cast<int32_t>(weapon_scope_max_mag_);
	const bool prone = (stance_latch_ == 2); // [orig: MoveOrder & 0x100 @ 0x4e0ff7]
	// Godot supplies float relative motion; the original consumes whole center-lock
	// pixels. Accumulate the fraction so slow motion is not truncated away.
	look_px_accum_x_ += p_dx_px;
	look_px_accum_y_ += p_dy_px;
	const int32_t dx = static_cast<int32_t>(look_px_accum_x_);
	const int32_t dy = static_cast<int32_t>(look_px_accum_y_);
	look_px_accum_x_ -= static_cast<float>(dx);
	look_px_accum_y_ -= static_cast<float>(dy);
	if (dx == 0 && dy == 0) return;
	opennova::world::player_look_apply(player_input_.look_heading, player_input_.look_pitch,
	                                   look_settings_, dx, dy, scoped_zoom, prone);
}

void NovaSimulation::set_local_player_mouse(int p_sensitivity, bool p_invert_y) {
	// The mousescale clamp [orig: @ 0x49b19b-0x49b1b9: >= 0x200 -> 0x1FF, <= 0 -> 1].
	int s = p_sensitivity;
	if (s < opennova::world::kMouseSensitivityMin) s = opennova::world::kMouseSensitivityMin;
	if (s > opennova::world::kMouseSensitivityMax) s = opennova::world::kMouseSensitivityMax;
	look_settings_.sensitivity = s;
	look_settings_.invert_y = p_invert_y;
}

bool NovaSimulation::request_local_player_stance(int p_stance) {
	if (p_stance < 0 || p_stance > 2) return false;
	// ForceCrouch weapons refuse stance changes [orig: the case-169/170/172 gate
	// Entity_CheckWeaponSeatFlags(equipped, 0x40000) @ 0x4e0d8a; the seat-kind-3
	// mount refusal rides the unported mounting slice].
	if (weapon_active_ && weapon_force_crouch_) return false;
	if (stance_latch_ == p_stance) return false;
	// SELECT with mutual exclusion — the 0x1D apply writes one stance bit and clears
	// the other [orig: NapiNPServerMsg_HandleStanceChange @ 0x501c60: 169 -> crouch,
	// 170 -> prone, 172 -> clear both].
	stance_latch_ = p_stance;
	player_input_.crouch = (stance_latch_ == 1);
	player_input_.prone = (stance_latch_ == 2);
	return true;
}

Dictionary NovaSimulation::get_waypoint_hud_view() const {
	// The current-waypoint slice of the per-frame HUD info rebuild, plus the
	// mission-scripted show gate. [orig: HUD_BuildEntityInfo @ 0x4b88b7..0x4b8914
	// (hudInfo+373 number, +400/404/408 position) + g_showWaypoints @ 0x27238BC]
	Dictionary out;
	const opennova::world::WaypointTrack *track = world_ ? &world_->waypoints : nullptr;
	out["show"] = track != nullptr && track->show;
	out["count"] = track ? static_cast<int>(track->entries.size()) : 0;
	const opennova::world::WaypointEntry *cur = track ? track->current_entry() : nullptr;
	out["current"] = cur ? track->current : -1;
	out["number"] = cur ? track->current + 1 : 0;
	out["name_id"] = cur ? cur->name_id : 0;
	// Fixed 16.16 mission (x,y,z) -> Godot (x, z, -y), like every entity read.
	out["position"] = cur ? Vector3(cur->x / 65536.0f, cur->z / 65536.0f, -(cur->y / 65536.0f))
						  : Vector3();
	out["done"] = cur != nullptr && cur->done;
	return out;
}

Array NovaSimulation::get_objectives_view() const {
	// The SP objectives panel's row walk: slots 1..8 until a 0/255 win id.
	// [orig: HUD_DrawWinConditions @0x5ba9e0 — byte_A7628B[slot] 0/255 break;
	//  row gate = show-win bit @0x5ba9ff; checkmark = won bit @0x5bab35]
	Array out;
	if (!world_) return out;
	const auto &sg = world_->subgoals;
	for (int slot = 1; slot <= 8; ++slot) {
		const uint8_t id = sg.win_text_ids[slot];
		if (id == 0 || id == 255) break;
		Dictionary row;
		row["slot"] = slot;
		row["text_id"] = static_cast<int>(id);
		row["shown"] = (sg.show_win & (1u << slot)) != 0;
		row["done"] = (sg.won & (1u << slot)) != 0;
		out.push_back(row);
	}
	return out;
}

Vector3 NovaSimulation::get_local_player_position() const {
	if (!world_ || !world_->cached.local_player.valid()) return Vector3();
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (!e) return Vector3();
	// mission (x,y,z) -> Godot (x, z, -y).
	return Vector3(e->position.x, e->position.z, -e->position.y);
}

float NovaSimulation::get_local_player_yaw_deg() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 0.0f;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p) return 0.0f;
	// Engine heading (BAM32) -> mission yaw degrees, the (90 - heading) convention.
	return static_cast<float>(opennova::world::mission_yaw_deg_from_bam_heading(p->heading));
}

float NovaSimulation::get_local_player_pitch_deg() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 0.0f;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p) return 0.0f;
	return static_cast<float>(static_cast<double>(p->pitch) * opennova::world::kDegreesPerBam);
}

int NovaSimulation::get_local_player_body_anim_slot() const {
	if (!world_ || !world_->cached.local_player.valid()) return -1;
	// The same Entity.body_anim_slot the present pass reads for NPC models (written by the
	// infantry motor mirror, infantry.cpp). The avatar is host-managed and not in the present
	// registry, so main_game drives its body clip from this getter.
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e ? e->body_anim_slot : -1;
}

String NovaSimulation::get_local_player_anim_key() const {
	// The local player's full anim-state clip key ("anim_<name>"), straight from the motor's
	// selected state. Unlike the 8-slot BodyAnim enum (get_local_player_body_anim_slot), this carries
	// stance + jump (anim_idle_crouch / anim_walk_prone_forward / anim_jump_loop / ...), so
	// main_game drives the 3rd-person avatar via play_body_clip(key) for full stance fidelity.
	// [orig: off_8135F0 names ARE the .adm keys without the "anim_" prefix]
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return String();
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p) return String();
	return infantry_anim_key(p->inf.anim_state);
}

int NovaSimulation::get_local_player_anim_phase_ticks() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 0;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	return p ? p->inf.clip_phase : 0;
}

Dictionary NovaSimulation::get_local_player_aim_overlay() const {
	// The torso-bend overlay state: the nine per-segment orientations from the exact BAM
	// blends [orig: Entity_BuildBoneTransformMatrices @0x4b1290; world-wac-ai-re.md §14],
	// converted once here to mission-euler degrees — yaw via the canonical (90 - heading),
	// pitch unchanged (the retail placement builder applies authored pitch as Ry(-pitch),
	// and MissionObjectPlacer performs the matching basis conjugation). The host builds
	// Godot bases from these with that single-sourced conversion; delta(body class) is
	// identity by construction.
	Dictionary out;
	out["valid"] = false;
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return out;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	const opennova::world::Entity *entity =
			world_->registry.get(world_->cached.local_player);
	if (!p || !entity) return out;

	opennova::anim::AimOverlayInputs in = aim_overlay_inputs_for(*p, *entity);
	// The head-look decay term carries the arms-dip feed (the +0x371 weapon-switch
	// window drops it 0x2800000/tick; infantry_weapon_channel owns the decay)
	// [orig: @ 0x4b5cab..0x4b5cd5]. The lean term is the sim's lean angle
	// (entity+0xB0; ramp/decay in infantry_lean_tick); roll is the slope-conform
	// visual roll (entity+0x18) and torso_roll its sixteenth-step chaser
	// (entity+0x2DC, infantry_torso_roll_tick); body_pitch is the slope-conform
	// body pitch (entity+0x90; both fed by infantry_slope_pass). pitch_blend
	// stays 0 until its sim source (recoil impulses) is ported — the formulas
	// carry the term so it drops in without touching this seam.
	opennova::anim::AimOverlayAngles angles[opennova::anim::kOverlayClassCount];
	opennova::anim::compute_aim_overlay_angles(in, angles);

	PackedVector3Array packed;
	packed.resize(opennova::anim::kOverlayClassCount);
	for (int i = 0; i < opennova::anim::kOverlayClassCount; ++i) {
		packed[i] = mission_euler_from_overlay(angles[i]);
	}
	out["valid"] = true;
	out["aim_state"] = in.aim_state;
	out["mount_mode"] = static_cast<int>(in.mount_mode);
	out["mount_config_valid"] = in.mount_config_valid;
	out["mount_config"] = in.mount_config_valid ? in.mount_config : 0;
	out["body"] = mission_euler_from_overlay(
			angles[opennova::anim::kOverlayBody]);
	out["angles"] = packed;
	return out;
}

// --- the local player's equipped-weapon FSM (net-re §5.62) --------------------------

NovaSimulation::WeaponClipRing *NovaSimulation::weapon_ring_for(const String &p_key_lower) {
	for (std::pair<String, WeaponClipRing> &kv : weapon_clip_rings_) {
		if (kv.first == p_key_lower) return &kv.second;
	}
	return nullptr;
}

float NovaSimulation::weapon_ring_take_length(const char *p_key) {
	// Serve the ring head's duration, then advance the head — the consuming read
	// [orig: Anim_GetDurationTicks @ 0x53ee10: currentEntry = *slot;
	//  *slot = *(currentEntry + 36); duration from currentEntry's data].
	WeaponClipRing *ring = weapon_ring_for(String::utf8(p_key).to_lower());
	if (ring == nullptr || ring->lengths.is_empty()) return -1.0f;
	const float served = ring->lengths[ring->head];
	ring->head = (ring->head + 1) % static_cast<int>(ring->lengths.size());
	return served;
}

int NovaSimulation::weapon_ring_take_variant(const String &p_key) {
	// Serve the head as the PLAYED variant and advance — the play latch: playback
	// follows the served entry while the ring moves on [orig: AnimMap_PlayAnimBySlot
	// @ 0x40bda0: animEntry = slot[i]; slot[i] = next; animState+68 = animEntry].
	WeaponClipRing *ring = weapon_ring_for(p_key.to_lower());
	if (ring == nullptr || ring->lengths.is_empty()) return 0;
	const int served = ring->head;
	ring->head = (ring->head + 1) % static_cast<int>(ring->lengths.size());
	return served;
}

void NovaSimulation::set_local_player_weapon(const Dictionary &p_def,
		const Dictionary &p_clip_seconds, bool p_preserve_slot_state) {
	install_local_player_weapon(
			p_def, p_clip_seconds, p_preserve_slot_state, false);
}

void NovaSimulation::rebake_local_player_weapon(const Dictionary &p_def,
		const Dictionary &p_clip_seconds, bool p_preserve_slot_state) {
	install_local_player_weapon(
			p_def, p_clip_seconds, p_preserve_slot_state, true);
}

void NovaSimulation::install_local_player_weapon(const Dictionary &p_def,
		const Dictionary &p_clip_seconds, bool p_preserve_slot_state,
		bool p_allow_same_weapon_rebake) {
	using opennova::world::WeaponFsmActionRow;
	// The FP model resolve re-installs the SAME weapon once its viewmodel (and
	// .adm clip lengths) finish loading. That resolve is a render-side consumer
	// in retail with no access to the action slot [orig: the per-frame FP model
	// resolve @ 0x4ded60 vs the mount's slot state in Player_MountWeaponSlot
	// @ 0x4dfa40], so an explicitly requested same-name rebake only refreshes
	// the def and rings.
	// Resetting the slot here instead destroyed a queued/holstering SWITCHFROM
	// whenever the late viewmodel install raced a switch request: the completion
	// never fired, commit_pending_weapon_switch never ran, and the FSM def
	// desynced from equipped_adm_index (the rifle then fired the previous
	// weapon's ammo).
	const String incoming_name = p_def.get("name", String());
	const bool same_weapon_rebake = p_allow_same_weapon_rebake &&
			weapon_active_ && !weapon_start_in_switchto_ &&
			!incoming_name.is_empty() &&
			incoming_name.nocasecmp_to(weapon_def_name_) == 0;
	// A real mount invalidates the one-shot scope restore latch. The late
	// first-person-model rebake is render-side only and must not mutate view state.
	if (!same_weapon_rebake) nvg_scope_restore_ = false;
	// A mount is a new presentation epoch: no payload from the previous weapon may
	// cross this seam, even though its strings were copied into the pending records.
	// The same-weapon rebake is NOT an epoch — undrained records (including a
	// racing switch commit or deny) must survive it.
	if (!same_weapon_rebake) pending_weapon_events_.clear();
	// Mirror the weapon dict's ACTION rows into the def-agnostic bake inputs.
	std::vector<WeaponFsmActionRow> rows;
	const Array actions = p_def.get("actions", Array());
	rows.reserve(static_cast<size_t>(actions.size()));
	for (int i = 0; i < actions.size(); ++i) {
		const Dictionary a = actions[i];
		WeaponFsmActionRow row;
		const CharString name = String(a.get("name", "")).utf8();
		const CharString anim = String(a.get("anim", "")).utf8();
		const CharString function = String(a.get("function", "")).utf8();
		snprintf(row.name, sizeof(row.name), "%s", name.get_data());
		snprintf(row.anim, sizeof(row.anim), "%s", anim.get_data());
		snprintf(row.function, sizeof(row.function), "%s", function.get_data());
		row.delaystart = static_cast<int32_t>(int64_t(a.get("delaystart", -1)));
		row.delayend = static_cast<int32_t>(int64_t(a.get("delayend", -1)));
		// The audio/effect legs ride the bake into the pool entries
		// [orig: ActionDef_ParseScriptLine @ 0x4023c0 rows].
		const CharString soundset = String(a.get("soundset", "")).utf8();
		const CharString soundsetend = String(a.get("soundsetend", "")).utf8();
		const CharString particle = String(a.get("particle", "")).utf8();
		const CharString userpoint = String(a.get("particleuserpoint", "")).utf8();
		snprintf(row.soundset, sizeof(row.soundset), "%s", soundset.get_data());
		snprintf(row.soundsetend, sizeof(row.soundsetend), "%s", soundsetend.get_data());
		snprintf(row.particle, sizeof(row.particle), "%s", particle.get_data());
		snprintf(row.particleuserpoint, sizeof(row.particleuserpoint), "%s", userpoint.get_data());
		rows.push_back(row);
	}
	// Clip lengths come from the loaded viewmodel's .adm (seconds) as per-key VARIANT
	// arrays; they seed the slot rings the bake and the play events consume
	// serve-then-advance [orig: the animState slot heads (+72) built by
	// AnimMap_RegisterBoneNode @ 0x40c2d0; Anim_GetDurationTicks @ 0x53ee10].
	weapon_clip_rings_.clear();
	weapon_anim_variant_ = 0;
	{
		const Array keys = p_clip_seconds.keys();
		for (int i = 0; i < keys.size(); ++i) {
			WeaponClipRing ring;
			const Variant v = p_clip_seconds[keys[i]];
			if (v.get_type() == Variant::PACKED_FLOAT32_ARRAY) {
				ring.lengths = v;
			} else {
				// Single-variant convenience: a plain number is a one-entry ring.
				ring.lengths.push_back(static_cast<float>(double(v)));
			}
			if (ring.lengths.is_empty()) continue;
			weapon_clip_rings_.emplace_back(String(keys[i]).to_lower(), ring);
		}
	}
	// The bake probes existence as a pure lookup and reads durations ring-wise —
	// one consuming read per 'auto' field [orig: Anim_InitActions @ 0x541fa0;
	// the lookup @ 0x5421ae, the reads @ 0x5421c5 / @ 0x5421d8].
	const auto resolve_fn = [](void *p_ctx, const char *key) -> int {
		NovaSimulation *self = static_cast<NovaSimulation *>(p_ctx);
		return self->weapon_ring_for(String::utf8(key).to_lower()) != nullptr ? 1 : 0;
	};
	const auto clip_fn = [](void *p_ctx, const char *key) -> float {
		return static_cast<NovaSimulation *>(p_ctx)->weapon_ring_take_length(key);
	};
	weapon_def_ = opennova::world::WeaponFsmDef{};
	opennova::world::weapon_fsm_bake(rows.data(), rows.size(), resolve_fn, clip_fn, this,
			weapon_def_);
	const int flags = int(p_def.get("flags", 0));
	weapon_def_.auto_fire = (flags & 0x100) != 0; // [orig: WeaponSlot_CanFireInCurrentState @ 0x53f0b0]
	weapon_def_.burst3 = (flags & 0x20) != 0;     // [orig: WeaponAction_Fire @ 0x542c8a]
	weapon_def_.flags = flags;                    // raw mask: the scope gate + fov policy read it
	weapon_def_.flags2 = int(p_def.get("flags2", 0)); // Inset (0x200) picks the 7-step ease
	// The heat model [orig: WeaponDef +0x36C/+0x370/+0x374]. Absent keys leave 0,
	// which disables the model exactly as the original's zero-init does.
	weapon_def_.heat_per_shot = int(int64_t(p_def.get("heat_per_shot", 0)));
	weapon_def_.heat_decay_per_tick = int(int64_t(p_def.get("heat_decay_per_tick", 0)));
	weapon_def_.heat_glow_threshold = int(int64_t(p_def.get("heat_glow_threshold", 0)));
	weapon_scope_max_mag_ = float(double(p_def.get("scope_max_mag", 0.0)));
	// The 3P body-channel kinds [orig: weapon.def special_hold/attack_anim -> the
	// AdmDefs record +0xA4/+0xA8; world-wac-ai-re.md §14.8.4].
	weapon_hold_kind_ = int(int64_t(p_def.get("special_hold", 0)));
	weapon_attack_kind_ = int(int64_t(p_def.get("attack_anim", 0)));
	// The run-gait class [orig: 'run_anim' -> AdmDefs +0xAC; promotion @ 0x4b729d] and
	// ForceCrouch (0x40000): idle_mortar promotion + stance-change refusal.
	weapon_run_anim_ = int(int64_t(p_def.get("run_anim", 0)));
	weapon_force_crouch_ = (flags & 0x40000) != 0;
	// A held-AnimMap CHANGE advances a host serial; the local InfantryState observes
	// that edge pre-tick and stamps its own 20-tick arms-dip window. Compare the
	// resolved map identity, not the weapon name: two weapon records sharing one
	// AnimMap do NOT dip. A fresh mount advances even when the map key is empty.
	// [orig: previous/current AdmDefs record +0 comparison @0x4b46d0..0x4b4701].
	const String anim_map = p_def.get("animadm", String());
	if (!weapon_active_ || anim_map.nocasecmp_to(weapon_anim_map_) != 0) {
		weapon_anim_map_ = anim_map;
		++weapon_anim_map_serial_;
		if (weapon_anim_map_serial_ == 0) ++weapon_anim_map_serial_; // reserve 0 = none
	}
	const int clipsize = int(p_def.get("clipsize", 0));
	weapon_def_.clip_capacity = clipsize > 0 ? clipsize : -1; // no clipsize key = no clip tracking
	if (same_weapon_rebake) {
		// Def + rings rebaked above; the live action slot, serials, input
		// latches, scope state, and charge state all continue untouched.
		weapon_def_name_ = incoming_name;
		return;
	}
	// A queued or in-flight SWITCHTO survives the per-equip reset — the switch flow
	// installs twice (dict-only, then with the rebuilt viewmodel's clip lengths) and
	// the draw-in must reach the second install (D-WPN-6 family artifact).
	const bool carry_switchto = !p_preserve_slot_state && weapon_active_ &&
			(weapon_slot_.next == opennova::world::weapon_action::kSwitchTo ||
			 weapon_slot_.current == opennova::world::weapon_action::kSwitchTo);
	if (!p_preserve_slot_state) {
		weapon_slot_ = opennova::world::WeaponSlotState{};
		// Ammo comes from the slot pool when the installed def IS the equipped
		// inventory slot: clip = the slot's loaded rounds, reserve = the def's
		// ammo-class pool [orig: MountSlot+0x10 +
		// Entity_GetScoreValueBySlotType @0x5406e0].
		bool ammo_from_inventory = false;
		if (local_inventory_valid_ && world_ != nullptr) {
			const opennova::world::WeaponInventorySlot *eq =
					local_inventory_.slot(local_inventory_.equipped_combo);
			const opennova::world::WeaponTableEntry *def =
					(eq != nullptr && eq->adm_index >= 0)
							? world_->weapons.by_index(
									static_cast<uint8_t>(eq->adm_index))
							: nullptr;
			if (def != nullptr &&
					incoming_name.nocasecmp_to(
							String::utf8(def->name.c_str())) == 0) {
				weapon_slot_.clip = eq->clip;
				weapon_slot_.reserve = opennova::world::weapon_pool_get(
						local_inventory_, def->ammo_class_id);
				ammo_from_inventory = true;
			}
		}
		if (!ammo_from_inventory) {
			// Fresh slot: full magazine + the def's carried reserve (the interim
			// ammo default for inventory-less installs — D-WPN-7).
			weapon_slot_.clip = clipsize > 0 ? clipsize : 0;
			weapon_slot_.reserve = int(p_def.get("startrounds", 0));
		}
		// A commit-driven inventory install starts in SWITCHTO. A UseGun commit
		// already queued SWITCHTO on the selected parent/personal slot.
		if (weapon_start_in_switchto_ || carry_switchto) {
			weapon_slot_.next =
					opennova::world::weapon_action::kSwitchTo;
			weapon_start_in_switchto_ = false;
		}
	}
	// A walk outcome that mounted but has not committed yet (its outgoing
	// SWITCHFROM was displaced by this mount) resumes through the deferred latch
	// once the draw completes, so the pending combo still commits.
	if (weapon_switch_in_flight_)
		weapon_switch_deferred_action_ = opennova::world::weapon_action::kSwitchFrom;
	// The mount is the charge epoch [orig: Player_SwitchToWeaponByHandle zeroes
	// g_fireChargeStartTick on the walk, before the mount].
	power_throw_start_tick_ = 0;
	pending_throw_charge_ = 0;
	weapon_play_serial_ = 0;
	weapon_anim_key_ = String();
	weapon_anim_variant_ = 0;
	weapon_anim_tick_ = 0;
	weapon_fired_serial_ = weapon_dry_serial_ = weapon_reload_serial_ = 0;
	weapon_unscope_serial_ = weapon_rescope_serial_ = 0;
	weapon_action_serial_ = 0;
	weapon_action_started_ = -1;
	weapon_action_end_serial_ = 0;
	weapon_action_finished_ = -1;
	weapon_fire_held_ = weapon_fire_pressed_ = weapon_reload_pressed_ = false;
	// A fresh mount starts at the hip with the interp cleared and the hipfire
	// latch reset [orig: Player_MountWeaponSlot zeroes the view biases @ 0x4dfbcf].
	player_view_.scope_engaged = false;
	player_view_.scope_step = 0;
	player_view_.ease_steps = opennova::world::kScopeEaseSteps;
	player_view_.scope_hipfire = true;
	weapon_def_name_ = incoming_name;
	weapon_active_ = true;
}

void NovaSimulation::clear_local_player_weapon() {
	nvg_scope_restore_ = false;
	weapon_active_ = false;
	weapon_def_name_ = String();
	power_throw_start_tick_ = 0;
	pending_throw_charge_ = 0;
	local_first_person_model_adm_ = 0xFF;
	weapon_switch_in_flight_ = false;
	weapon_switch_deferred_action_ = -1;
	pending_weapon_events_.clear();
	weapon_fire_held_ = false;
	weapon_fire_pressed_ = false;
	weapon_reload_pressed_ = false;
	weapon_clip_rings_.clear();
	weapon_anim_variant_ = 0;
	weapon_anim_tick_ = 0;
	player_view_.scope_engaged = false;
	player_view_.scope_step = 0;
	player_view_.ease_steps = opennova::world::kScopeEaseSteps;
	player_view_.scope_hipfire = true;
	weapon_hold_kind_ = 0;
	weapon_attack_kind_ = 0;
	weapon_run_anim_ = 0;
	weapon_force_crouch_ = false;
	weapon_anim_map_ = String();
}

void NovaSimulation::set_local_player_first_person_model_available(bool p_available) {
	local_first_person_model_adm_ = 0xFF;
	if (!p_available || !world_) return;
	const opennova::world::Entity *player =
			world_->registry.get(world_->cached.local_player);
	if (player != nullptr)
		local_first_person_model_adm_ = player->equipped_adm_index;
}

void NovaSimulation::set_local_player_weapon_input(bool p_fire_held, bool p_fire_pressed,
		bool p_reload_pressed) {
	if (player_view_.binoculars_view_active) {
		weapon_fire_held_ = false;
		weapon_fire_pressed_ = false;
		weapon_reload_pressed_ = false;
		return;
	}
	weapon_fire_held_ = p_fire_held;
	weapon_fire_pressed_ = weapon_fire_pressed_ || p_fire_pressed; // latch until consumed
	weapon_reload_pressed_ = weapon_reload_pressed_ || p_reload_pressed;
}

bool NovaSimulation::request_local_player_scope_toggle() {
	// [orig: input case 6 @ 0x4e0420 gates currentAction not in {RELOAD, SWITCHFROM};
	//  Player_ToggleWeaponScope @ 0x4df0c0 gates def Flags & 3, flips g_scopeEngaged
	//  @ 0x82CE94, and queues the scopeup/scopedown FSM state @ 0x53f050/0x53f080]
	if (!weapon_active_) return false;
	opennova::world::WeaponSlotState *active_slot =
			active_local_weapon_slot();
	if (!opennova::world::weapon_fsm_scope_toggle_allowed(
			weapon_def_, *active_slot)) return false;
	// Scope-UP is refused while a movement key is held on a Scoped weapon
	// [orig: byte_B7653B && (flags & 1) -> return @ 0x4df29c].
	if (!player_view_.scope_engaged &&
			opennova::world::player_view_scope_up_blocked(player_view_, weapon_def_.flags))
		return false;
	// Inset optics cannot be raised under NVG. Non-Inset sights retain the
	// original independent behavior.
	if (!player_view_.scope_engaged && player_view_.nvg_active &&
			(weapon_def_.flags2 & 0x200) != 0)
		return false;
	// ForceScoped pins the raised sight: un-scoping is refused once settled
	// [orig: (flags1 & 0x20000000) == 0 || !g_weaponScopeActive @ 0x4df12d].
	if (player_view_.scope_engaged && (weapon_def_.flags & 0x20000000) != 0 &&
			!opennova::world::player_view_scope_ease_active(player_view_))
		return false;
	// The toggle latches this ease's step count (7 for Inset weapons, else 15;
	// 1 on the hipfire-return leg) and REFUSES while the previous ease runs
	// [orig: Player_ToggleWeaponScope @ 0x4df177 !activeFlag; Setup @ 0x4df1b3..0x4df36e].
	if (!opennova::world::player_view_set_engaged(player_view_, !player_view_.scope_engaged,
			(weapon_def_.flags2 & 0x200) != 0))
		return false;
	if (player_view_.scope_engaged)
		opennova::world::weapon_fsm_queue_scope_up(*active_slot);
	else
		opennova::world::weapon_fsm_queue_scope_down(*active_slot);
	return true;
}

bool NovaSimulation::request_local_player_binoculars_toggle() {
	const opennova::world::Entity *local = world_ != nullptr
			? world_->registry.get(world_->cached.local_player)
			: nullptr;
	if (local == nullptr) return false;
	// Retail refuses binoculars while a PowerThrow charge is live. Allowing the
	// view to rise would suppress held weapon input and turn the charge into an
	// unintended release [orig: g_fireChargeStartTick @ 0xB76800; action 26 gate].
	if (power_throw_start_tick_ != 0) return false;
	// An active scope also blocks binoculars in a gunner parent slot.
	if (player_view_.scope_engaged && local->mounted &&
			local->mount_type == opennova::world::SeatType::Gunner)
		return false;

	const bool requested =
			opennova::world::player_view_toggle_binoculars(player_view_);
	if (requested) {
		const double unit =
				(static_cast<double>(std::rand()) + 0.5) /
				(static_cast<double>(RAND_MAX) + 1.0);
		const double angle = unit * kTau;
		binocular_yaw_offset_deg_ =
				static_cast<float>(std::cos(angle) * kBinocularAimOffsetDeg);
		binocular_pitch_offset_deg_ =
				static_cast<float>(std::sin(angle) * kBinocularAimOffsetDeg);
	} else {
		binocular_yaw_offset_deg_ = 0.0f;
		binocular_pitch_offset_deg_ = 0.0f;
	}
	refresh_local_player_view_effects();
	return requested;
}

bool NovaSimulation::request_local_player_nvg_toggle() {
	const opennova::world::Entity *local = world_ != nullptr
			? world_->registry.get(world_->cached.local_player)
			: nullptr;
	if (local == nullptr) return false;

	if (!player_view_.nvg_active) {
		nvg_scope_restore_ = false;
		if (weapon_active_ && player_view_.scope_engaged &&
				(weapon_def_.flags2 & 0x200) != 0 &&
				!opennova::world::player_view_scope_ease_active(player_view_)) {
			nvg_scope_restore_ = request_local_player_scope_toggle();
		}
		return opennova::world::player_view_toggle_nvg(player_view_);
	}

	// Clear NVG before the normal scope-up request so the Inset refusal no
	// longer applies, then consume the one-shot restore latch.
	opennova::world::player_view_toggle_nvg(player_view_);
	const bool restore_scope = nvg_scope_restore_;
	nvg_scope_restore_ = false;
	if (restore_scope && !player_view_.scope_engaged)
		request_local_player_scope_toggle();
	return false;
}

int NovaSimulation::request_local_player_nvg_gain(int p_delta) {
	return opennova::world::player_view_adjust_nvg_gain(player_view_, p_delta);
}

void NovaSimulation::set_local_player_camera_third_person(bool p_third_person) {
	player_view_.third_person = p_third_person; // [orig: g_camera_mode @ 0xA890C8]
	refresh_local_player_view_effects();
}

// One 62.5 Hz tick of the view state, before the weapon pump: the ADS ease and the
// third-person anchor chase run at the WORLD cadence, so camera lag is identical at
// any render rate. Retail's Player_UpdatePerFrame call precedes the later
// WeaponAction_ProcessAllEntities call, so this tick's settle promoter is visible to
// action routing while an action's unscope/rescope begins easing on the next tick
// [orig: call sites @ 0x42c18e / @ 0x526786; promoter @ 0x4de4f7].
void NovaSimulation::tick_local_player_view() {
	if (!world_ || !world_->cached.local_player.valid()) {
		opennova::world::player_view_update_effective_modes(
				player_view_, false, world_ != nullptr && world_->round_end.ended);
		player_view_.tp_anchor_valid = false;
		return;
	}
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (!e) return;
	refresh_local_player_view_effects();
	// The anchor-chase target is Position + CameraOffset — the posed head-bone eye
	// [orig: ThirdPersonCamera_Update @ 0x437b70..76], fed by the host's per-frame
	// skeleton sample (see local_eye_mission_). Without a sample: Position + 1.0,
	// the witnessed NON-person bump [orig: @ 0x437e8f].
	const float eye[3] = {
		local_eye_valid_ ? local_eye_mission_[0] : e->position.x,
		local_eye_valid_ ? local_eye_mission_[1] : e->position.y,
		local_eye_valid_ ? local_eye_mission_[2] : e->position.z + 1.0f,
	};
	opennova::world::player_view_tick(player_view_, eye);
}

void NovaSimulation::set_local_player_eye(const Vector3 &p_eye_godot, bool p_valid) {
	// Godot (x, y, z) -> mission (x, -z, y), the get_local_player_position inverse.
	local_eye_mission_[0] = p_eye_godot.x;
	local_eye_mission_[1] = -p_eye_godot.z;
	local_eye_mission_[2] = p_eye_godot.y;
	local_eye_valid_ = p_valid;
}

Dictionary NovaSimulation::get_local_player_view() const {
	Dictionary out;
	const opennova::world::WeaponSlotState *active_slot =
			active_local_weapon_slot();
	out["scope_engaged"] = player_view_.scope_engaged;
	out["binoculars_requested"] = player_view_.binoculars_requested;
	out["binoculars_raised"] = player_view_.binoculars_raised;
	out["binoculars_view_active"] = player_view_.binoculars_view_active;
	out["binocular_yaw_offset_deg"] = binocular_yaw_offset_deg_;
	out["binocular_pitch_offset_deg"] = binocular_pitch_offset_deg_;
	out["nvg_active"] = player_view_.nvg_active;
	out["nvg_visible"] =
			opennova::world::player_view_nvg_visible(player_view_);
	out["nvg_gain"] = player_view_.nvg_gain;
	const opennova::world::Entity *local = world_ != nullptr
			? world_->registry.get(world_->cached.local_player)
			: nullptr;
	out["mounted"] = local != nullptr && local->mounted;
	// Structural proxy for Player_IsVehicleHasAttackCapability until mounted
	// weapon inventory is modeled: these seat classes replace the on-foot
	// upper-body weapon channel; passenger seats do not.
	out["vehicle_attack_context"] = local != nullptr && mount_blocks_weapon_channel(*local);
	out["scope_fraction"] = opennova::world::player_view_scope_fraction(player_view_);
	// The NoCardSwitch reload rule: while the equipped slot is mid-RELOAD on a
	// weapon WITHOUT NoCardSwitch (flags 0x2000000), the FP camera drops the ADS
	// view bias for the frame — the host reads the eased fraction as 0.
	// [orig: Player_UpdateFirstPersonCamera @ 0x4dd439/@ 0x4dd4cc; the same
	//  predicate is Player_IsReloadingCardSwitchWeapon @ 0x4dcdd0 (ex kong
	//  "Player_IsDriverInVehicle"), whose one caller refuses fire @ 0x5cf7be]
	out["suppress_view_bias"] = weapon_active_ &&
			active_slot->current == opennova::world::weapon_action::kReload &&
			(weapon_def_.flags & opennova::world::weapon_flag::kNoCardSwitch) == 0;
	// On the supported on-foot first-person path, the standard SIGHTS card replaces
	// the FP viewmodel once ADS settles. Scoped and Sighted are asymmetric selectors;
	// NoCardSwitch clears both unless ForceScoped overrides it. The frame draws the
	// card or the FP viewmodel, never both. [orig: Render_ProcessMainSceneFrame
	// @0x5ca299..0x5ca304 / @0x5caaf3..0x5cab15; suppression @0x4dcce0]
	out["scope_card_active"] = weapon_active_ &&
			opennova::world::weapon_sights_card_eligible(
					weapon_def_, *active_slot) &&
			player_view_.scope_engaged && !player_view_.third_person &&
			!player_view_.binoculars_view_active &&
			!opennova::world::player_view_scope_ease_active(player_view_);
	out["fov_h_deg"] = opennova::world::player_view_fov_h_deg(player_view_,
			weapon_active_ ? weapon_def_.flags : 0,
			weapon_active_ ? weapon_scope_max_mag_ : 0.0f);
	// mission (x,y,z) -> Godot (x, z, -y), the get_local_player_position map.
	out["tp_anchor"] = Vector3(player_view_.tp_anchor[0], player_view_.tp_anchor[2],
			-player_view_.tp_anchor[1]);
	out["tp_anchor_valid"] = player_view_.tp_anchor_valid;
	// The FP camera roll in degrees: roll = torsoRoll + lean/4 [orig: the on-foot
	// person leg @ 0x437fe6 — g_view_rot_roll = entity+0x2DC + (entity+0xB0 >> 2)].
	{
		float fp_roll_deg = 0.0f;
		if (world_ && world_->ai && world_->cached.local_player.valid()) {
			if (const AiEntity *p = world_->ai->for_handle(world_->cached.local_player)) {
				const int32_t roll_bam = opennova::io::bam_add(
						p->inf.torso_roll, opennova::io::bam_sar(p->inf.lean_angle, 2));
				fp_roll_deg = static_cast<float>(
						static_cast<double>(roll_bam) * opennova::world::kDegreesPerBam);
			}
		}
		out["fp_roll_deg"] = fp_roll_deg;
	}
	return out;
}

float NovaSimulation::fov_vertical_from_horizontal(float p_fov_h_deg, float p_aspect) {
	return opennova::world::fov_vertical_from_horizontal_deg(p_fov_h_deg, p_aspect);
}

// One 62.5 Hz pump of the local player's slot, after the world logic tick. The world
// tick now owns the parallel NPC UseGun parent-slot pump; this host method remains the
// first-person player's presentation/input seam.
// [orig: WeaponAction_ProcessAllEntities @0x542690 pumps every pooled entity]
void NovaSimulation::tick_local_player_weapon() {
	if (!world_ || !world_->cached.local_player.valid()) return;
	sync_local_usegun_weapon_transition();
	if (!weapon_active_) return;
	opennova::world::WeaponSlotState &active_slot =
			*active_local_weapon_slot();
	const opennova::world::Entity *player =
			world_->registry.get(world_->cached.local_player);
	const bool player_alive = player != nullptr && player->alive &&
			player->health > 0;
	const bool usegun_switch_pending =
			local_usegun_switch_ != LocalUseGunSwitch::kNone;
	if (!player_alive && !usegun_switch_pending) {
		active_slot.refire_queued = false;
		power_throw_start_tick_ = 0;
		pending_throw_charge_ = 0;
		weapon_fire_pressed_ = false;
		weapon_reload_pressed_ = false;
		return;
	}
	// Capture the outgoing slot identity. A switch completion below changes the
	// active selection, but this tick's ammo bridge still belongs to the slot the
	// FSM actually pumped.
	const bool borrowed_usegun_slot = local_usegun_slot_active_;
	opennova::world::WeaponFsmInputs in;
	const bool accept_weapon_input = player_alive && !usegun_switch_pending;
	in.fire_held = accept_weapon_input && weapon_fire_held_;
	in.fire_pressed = accept_weapon_input && weapon_fire_pressed_;
	// PowerThrow: the press never fires — it starts the windup; the release
	// converts the held time into the charge byte and fires. [orig: press gate
	// @ 0x4e08fd (def Flags sign bit 0x80000000, fireable + ammo ->
	// g_fireChargeStartTick = tick), release @ 0x4e07e9 -> WeaponSlot_RequestFire
	// with the computed charge; world-wac-ai-re §27.]
	bool power_throw_release = false;
	if ((weapon_def_.flags & 0x80000000u) != 0) {
		if (!accept_weapon_input) {
			power_throw_start_tick_ = 0;
			pending_throw_charge_ = 0;
		} else if (weapon_fire_held_ || weapon_fire_pressed_) {
			// The windup refuses while a switch action runs OR is queued — the
			// press gate's fireable term, not just the current action [orig: the
			// fireable check @ 0x4e08fd]. Without the queued/deferred legs a
			// press landing inside the draw-in latched a windup whose release
			// the FSM then refused, leaking the charge byte onto a later shot.
			const bool fireable =
					active_slot.current == opennova::world::weapon_action::kIdle &&
					active_slot.next == opennova::world::weapon_action::kIdle &&
					!weapon_switch_in_flight_ && weapon_switch_deferred_action_ < 0;
			const bool has_ammo =
					active_slot.clip > 0 || weapon_def_.clip_capacity < 0;
			if (power_throw_start_tick_ == 0 && fireable && has_ammo)
				power_throw_start_tick_ = world_->logic_tick;
			in.fire_held = false;
			in.fire_pressed = false;
		} else if (power_throw_start_tick_ != 0) {
			const int32_t held = static_cast<int32_t>(
					world_->logic_tick - power_throw_start_tick_);
			pending_throw_charge_ =
					opennova::world::power_throw_charge_from_hold(held);
			power_throw_start_tick_ = 0;
			in.fire_pressed = true;
			in.fire_held = false;
			power_throw_release = true;
		}
	} else {
		power_throw_start_tick_ = 0;
	}
	// The dispatch gate runs here now: the raw reload edge is refused on a full
	// magazine or an empty reserve [orig: input case 0xD3 @ 0x4e0420].
	in.reload_pressed = accept_weapon_input && weapon_reload_pressed_ &&
			opennova::world::weapon_fsm_reload_allowed(
					weapon_def_, active_slot);
	in.is_local = true;
	in.is_authority = !joiner_; // the joiner defers the refill to the §5.58 round-trip
	in.auto_reload = true;      // [orig: g_autoReloadEnabled @ 0x24D2118, default on]
	// The weapon FSM consumes the promoted/settled scope bit, not the raw
	// requested-engagement bit. Player_UpdatePerFrame runs before the weapon
	// pump in retail and only promotes g_weaponScopeActive after the ease has
	// completed [orig: promoter @ 0x4de4f7; weapon pump @ 0x526786].
	in.scope_active = player_view_.scope_engaged &&
			!opennova::world::player_view_scope_ease_active(player_view_);
	in.instant_emplaced_switch = local_usegun_switch_is_instant();
	// The heat window is a deadline against the logic tick, not a stored level.
	// `submerged` stays false: the sim has no per-entity water test at the weapon
	// site yet, and above water is what the runtime actually plays (D-WPN-29).
	// [orig: current_tick @ 0x24C1968; the water gate @ 0x54101c]
	in.current_tick = static_cast<int32_t>(world_->logic_tick);
	if (!accept_weapon_input) active_slot.refire_queued = false;
	opennova::world::WeaponFsmEvents ev;
	opennova::world::weapon_fsm_tick(
			weapon_def_, active_slot, in, ev);
	// A release whose fire request the FSM refused must not leave the charge
	// latched for a later unrelated shot — the charge byte is consumed by the
	// very fire it triggers [orig: descriptor +20 consume @ 0x4ec5bb].
	if (power_throw_release && !ev.fired &&
			active_slot.current != opennova::world::weapon_action::kFire &&
			active_slot.next != opennova::world::weapon_action::kFire)
		pending_throw_charge_ = 0;
	// SWITCHTO seeds next=prev at the end of its delay-start phase. Reapply the
	// retained one-shot after every draw tick so its eventual transition performs
	// the pending inventory handoff without requiring another key press.
	if (weapon_switch_deferred_action_ >= 0) {
		if (active_slot.current == weapon_switch_deferred_action_) {
			weapon_switch_deferred_action_ = -1;
		} else {
			active_slot.next = weapon_switch_deferred_action_;
		}
	}
	weapon_fire_pressed_ = false; // edges consume on the first tick of the frame
	weapon_reload_pressed_ = false;
	PendingWeaponEvent pending;
	pending.tick = world_->logic_tick;
	bool has_presentation_event = false;
	if (ev.play_anim) {
		++weapon_play_serial_;
		weapon_anim_key_ = String::utf8(ev.anim_key);
		weapon_anim_tick_ = world_->logic_tick;
		// The play consumes the slot ring and latches the served variant — the host
		// plays exactly this variant on every viewmodel part
		// [orig: AnimMap_PlayAnimBySlot @ 0x40bda0 advances the head and latches
		//  the served entry at animState+68].
		weapon_anim_variant_ = weapon_ring_take_variant(weapon_anim_key_);
		pending.anim_key = weapon_anim_key_;
		pending.anim_variant = weapon_anim_variant_;
		has_presentation_event = true;
	}
	if (ev.action_started >= 0) {
		// Copy the begin leg while this def is mounted; a later weapon switch cannot
		// change the queued sound/effect payload.
		// [orig: ActionSlot_ExecuteActionWithEffect @ 0x541860].
		++weapon_action_serial_;
		weapon_action_started_ = ev.action_started;
		pending.action_started = ev.action_started;
		if (ev.action_started < opennova::world::weapon_action::kCount) {
			const opennova::world::WeaponFsmAction &act = weapon_def_.actions[ev.action_started];
			pending.action_soundset = String::utf8(act.soundset);
			pending.action_particle = String::utf8(act.particle);
			pending.action_particle_userpoint = String::utf8(act.particle_userpoint);
		}
		has_presentation_event = true;
	}
	if (ev.action_finished >= 0) {
		// The END leg: the finished action's soundsetend — fire rows carry the gunshot
		// here, reload rows the completion sound
		// [orig: ActionSlot_FinishActivePhase @ 0x53f7b0 -> the end shim @ 0x401100].
		++weapon_action_end_serial_;
		weapon_action_finished_ = ev.action_finished;
		pending.action_finished = ev.action_finished;
		if (ev.action_finished < opennova::world::weapon_action::kCount) {
			pending.action_end_soundset =
					String::utf8(weapon_def_.actions[ev.action_finished].soundsetend);
		}
		has_presentation_event = true;
	}
	if (ev.action_effect >= 0 && ev.action_effect < opennova::world::weapon_action::kCount) {
		// The recoil-row DIRECT effect leg — casing eject / bolt smoke at the arbiter
		// tick. Copied like the begin leg so a weapon switch cannot swap the payload.
		// [orig: WeaponAction_Recoil @ 0x542dd0 spawn @ 0x542f64]
		const opennova::world::WeaponFsmAction &act = weapon_def_.actions[ev.action_effect];
		pending.action_effect = ev.action_effect;
		pending.effect_particle = String::utf8(act.particle);
		pending.effect_particle_userpoint = String::utf8(act.particle_userpoint);
		has_presentation_event = true;
	}
	// Preserve the retail call order within one pump: clip start, begin leg, then
	// finish leg. Records themselves stay in logic-tick order until the host drains.
	if (has_presentation_event) {
		pending.world_position = get_local_player_position();
		pending.scope_settled = player_view_.scope_engaged &&
				!opennova::world::player_view_scope_ease_active(player_view_);
		pending.third_person = player_view_.third_person;
		const opennova::world::Entity *local =
				world_->registry.get(world_->cached.local_player);
		pending.vehicle_attack_context =
				local != nullptr && mount_blocks_weapon_channel(*local);
		pending_weapon_events_.push_back(std::move(pending));
	}
	if (ev.fired) {
		++weapon_fired_serial_;
		// The 3P body attack stamp — knife/grenade kinds only; rifle fire stamps NO body
		// state (the FP clip plays on the weapon adm channel, and the fire path's only
		// other anim side effect drives the .3di control registers)
		// [orig: WeaponAction_Fire @ 0x542bbc..0x542bea; ActionSlot_TryAllocCtrlRegAnim
		//  @ 0x401f00 -> dword_83FCE8].
		AiEntity *p = world_->ai ? world_->ai->for_handle(world_->cached.local_player) : nullptr;
		if (p && p->inf.active)
			opennova::world::infantry_weapon_attack_stamp(p->inf, weapon_attack_kind_);
		// Local/SP fire already passed the same FSM/ammo authority that the remote
		// C2S 0x06 handler validates. Append the host's round-ring record and spawn
		// the authoritative projectile here; the loopback server handler correctly
		// ignores this player because retail's local action has already done both.
		// [orig: WeaponAction_Fire @ 0x542c5e ->
		// Entity_FireWeaponAndSendPacket @ 0x42bd80 local re-entry ->
		// RoundData_AddRound @ 0x4fdb40 inline RoundData_SpawnRound @ 0x4ec0d0]
		opennova::world::Entity *shooter =
				world_->registry.get(world_->cached.local_player);
		if (!joiner_ && shooter != nullptr && p != nullptr) {
			const uint8_t adm_index = shooter->equipped_adm_index;
			const opennova::world::WeaponTableEntry *adm =
					world_->weapons.by_index(adm_index);
			if (adm != nullptr && adm->ammo_index >= 0) {
				opennova::world::Vec3 origin = shooter->position;
				if (local_eye_valid_) {
					origin.x = local_eye_mission_[0];
					origin.y = local_eye_mission_[1];
					origin.z = local_eye_mission_[2];
				} else {
					origin.z += 1.0f;
				}
				// The round bearing frame IS the engine heading frame: RoundSim's
				// (cos, sin) mission-axis mapping is wire-validated on the 0x06 yaw
				// BAM (round_sim.cpp spawn, D-NET-153), and the retail spawner runs
				// raw descriptor angles through sin/cos [orig: RoundData_SpawnRound
				// trig @ 0x4ec511..0x4ec5fb]. Applying the (90 - heading)
				// mission-yaw flip here mirrored every local shot across the NE
				// diagonal (impacts landed 90 deg off the aim ray - the
				// fp_impact_probe pin; the same mistake D-NET-153 records for the
				// wire leg).
				const int32_t dir_yaw = p->heading;
				const int32_t dir_pitch = p->pitch;
				local_round_sequence_ =
						static_cast<uint16_t>(local_round_sequence_ + 1u);
				const uint16_t shot_seq = local_round_sequence_;

				opennova::world::RoundEvent round_event;
				round_event.shooter_handle = world_->cached.local_player.packed;
				round_event.origin_x = static_cast<int32_t>(
						std::lround(double(origin.x) * kFixed16));
				round_event.origin_y = static_cast<int32_t>(
						std::lround(double(origin.y) * kFixed16));
				round_event.origin_z = static_cast<int32_t>(
						std::lround(double(origin.z) * kFixed16));
				round_event.dir_yaw = dir_yaw;
				round_event.dir_pitch = dir_pitch;
				round_event.shot_seq = shot_seq;
				const uint32_t clip_before_consume = static_cast<uint32_t>(
						std::max(0, ev.fired_clip_before_consume));
				round_event.mode_flags = static_cast<uint8_t>(
						((clip_before_consume & 0x3u) << 4u) | 0x02u);
				const bool vehicle_attack_context =
						mount_blocks_weapon_channel(*shooter);
				const bool scope_settled = player_view_.scope_engaged &&
						!opennova::world::player_view_scope_ease_active(player_view_);
				// The ordinary on-foot hip-fire leg is exact: retail passes
				// Weapon_GetScopeZoomLevel(false, 12), which returns 12, and the
				// server's bit-6-clearing composite preserves it. The predicate
				// reads the PROMOTED scope bit, so ADS raise and third-person use
				// the same 12. Settled-FP/mounted zoom levels remain D-WPN-8.
				if (player_view_.third_person ||
						(!scope_settled && !vehicle_attack_context &&
								(weapon_def_.flags & 0x20000000) == 0)) {
					round_event.subtype = 12;
				}
				round_event.adm_index = adm_index;
				// The PowerThrow charge rides the ring/wire slot_byte (ring+32,
				// wire flags|0x80 leg) and scales the spawned round's launch
				// speed [orig: WeaponAction_Fire arg 6 <- MountSlot+0x5C ->
				// descriptor +20 @ 0x4ec5bb; deserializer restore @ 0x42f769].
				round_event.slot_byte = pending_throw_charge_;
				world_->rounds.add(round_event);

				opennova::world::RoundSpawnParams round;
				round.owner = world_->cached.local_player;
				round.shooter_handle = world_->cached.local_player.packed;
				round.origin = origin;
				round.dir_yaw_bam = dir_yaw;
				round.dir_pitch_bam = dir_pitch;
				round.ammo_index = adm->ammo_index;
				round.adm_index = adm_index;
				round.shot_seq = shot_seq;
				round.charge = pending_throw_charge_;
				world_->round_sim.spawn(*world_, round);
				pending_throw_charge_ = 0;
			}
		}
	}
	if (ev.dry_fired) ++weapon_dry_serial_;
	if (ev.reload_requested) ++weapon_reload_serial_;
	if (ev.reload_applied) {
		// The refill stamps the 3P body reload-anim window on the entity — 80 ticks; the
		// infantry weapon channel then wants state 65 reload until it expires (and the
		// locked clip plays to its end). In the original the stamp lives inside the
		// refill itself; the SP/listen-host loopback applies it at reload start.
		// [orig: WeaponSlot_ReloadAmmo @ 0x54173c; world-wac-ai-re.md §14.8.5]
		AiEntity *p = world_->ai ? world_->ai->for_handle(world_->cached.local_player) : nullptr;
		if (p && p->inf.active) p->inf.reload_anim_ticks = 80;
	}
	// --- the slot-pool bridge: the pool model is authoritative for ammo -------------
	// [orig: the FSM's clip lives on the MountSlot (+0x10) and the reserve is the
	//  per-ammo-class pool — one storage, two views; this port mirrors between the
	//  single-slot FSM state and the inventory]
	if (local_usegun_switch_ != LocalUseGunSwitch::kNone &&
			ev.switch_completed &&
			active_slot.current == local_usegun_switch_action_)
		commit_local_usegun_weapon_switch();
	if (local_inventory_valid_ && world_ != nullptr &&
			!borrowed_usegun_slot) {
		opennova::world::WeaponInventorySlot *eq =
				local_inventory_.slot(local_inventory_.equipped_combo);
		const opennova::world::WeaponTableEntry *eq_def =
				(eq != nullptr && eq->adm_index >= 0)
						? world_->weapons.by_index(static_cast<uint8_t>(eq->adm_index))
						: nullptr;
		if (eq != nullptr && eq_def != nullptr) {
			if (ev.reload_applied) {
				// The witnessed refill: refund the remaining clip to the pool, draw a
				// full clip clamped by it [orig: WeaponSlot_ReloadAmmo @ 0x541720,
				// §5.58] — overriding the FSM's single-class transfer (D-WPN-2).
				// eq->clip still holds the pre-reload remaining rounds (mirrored on
				// the previous tick); the refund below consumes it.
				opennova::world::weapon_inventory_reload_slot(world_->weapons,
				                                              local_inventory_,
				                                              local_inventory_.equipped_combo);
				active_slot.clip = eq->clip;
			} else {
				eq->clip = active_slot.clip; // fire consume mirrors down
			}
			active_slot.reserve =
					opennova::world::weapon_pool_get(local_inventory_, eq_def->ammo_class_id);
			// The post-recoil auto-switch [orig: WeaponAction_Recoil tail @ 0x543062:
			// def+0x168 -> Player_SwitchToWeaponByHandle(def[+0x164]*65) — the
			// grenade/LAW switchback, unconditional per throw].
			if (ev.action_finished == opennova::world::weapon_action::kRecoil &&
			    eq_def->has_switchcategory) {
				handle_weapon_switch_outcome(opennova::world::weapon_switch_to_handle(
						world_->weapons, local_inventory_,
						eq_def->switchcategory *
								opennova::world::weapon_combo::kRanksPerCategory,
						local_weapon_switch_gates()));
			}
		}
		// A queued manual switch commits at the outgoing SWITCHFROM/SWITCHRANK
		// swap seam [orig: the completion consumes g_pendingWeaponSlot].
		if (weapon_switch_in_flight_ && ev.switch_completed) {
			commit_pending_weapon_switch();
		}
	}
	// The FSM's scope side effects land on the sim-owned engaged bit: forced
	// unscope (one-shot / reload stash) and the pump's rescope-after-reload
	// [orig: g_weaponScopeActive writes; the rescope block @ 0x54139e].
	if (ev.unscope) {
		++weapon_unscope_serial_;
		// The forced paths run the same refusing toggle — a mid-ease unscope keeps
		// the scope (rare: a reload requested inside the raise ease)
		// [orig: @ 0x543136 calls Player_ToggleWeaponScope, activeFlag-gated].
		opennova::world::player_view_set_engaged(player_view_, false,
				(weapon_def_.flags2 & 0x200) != 0);
	}
	if (ev.rescope) {
		++weapon_rescope_serial_;
		opennova::world::player_view_set_engaged(player_view_, true,
				(weapon_def_.flags2 & 0x200) != 0);
	}
}

Dictionary NovaSimulation::get_local_player_weapon_state() const {
	Dictionary out;
	out["active"] = weapon_active_;
	if (!weapon_active_) return out;
	const opennova::world::WeaponSlotState &active_slot =
			*active_local_weapon_slot();
	out["current"] = active_slot.current;
	out["next"] = active_slot.next;
	out["phase"] = static_cast<int>(active_slot.phase);
	out["switch_deferred"] = weapon_switch_deferred_action_;
	out["switch_in_flight"] = weapon_switch_in_flight_;
	out["pending_combo"] = local_inventory_.pending_combo;
	out["anim_key"] = weapon_anim_key_;
	out["anim_variant"] = weapon_anim_variant_;
	const uint32_t anim_age_ticks = world_ && !weapon_anim_key_.is_empty()
			? world_->logic_tick - weapon_anim_tick_ : 0;
	out["anim_age_ticks"] = static_cast<int64_t>(anim_age_ticks);
	out["play_serial"] = static_cast<int64_t>(weapon_play_serial_);
	// The last-started action's audio/effect legs remain useful snapshot diagnostics;
	// ordered delivery uses drain_local_player_weapon_events().
	// [orig: ActionSlot_ExecuteActionWithEffect
	// @ 0x541860 -> ActionSlot_SpawnEffect @ 0x401f20].
	out["action_serial"] = static_cast<int64_t>(weapon_action_serial_);
	if (weapon_action_started_ >= 0 &&
			weapon_action_started_ < opennova::world::weapon_action::kCount) {
		const opennova::world::WeaponFsmAction &act = weapon_def_.actions[weapon_action_started_];
		out["action_started"] = weapon_action_started_;
		out["action_soundset"] = String::utf8(act.soundset);
		out["action_particle"] = String::utf8(act.particle);
		out["action_particle_userpoint"] = String::utf8(act.particle_userpoint);
	} else {
		out["action_started"] = -1;
		out["action_soundset"] = String();
		out["action_particle"] = String();
		out["action_particle_userpoint"] = String();
	}
	// The latest END-leg snapshot diagnostic: fire rows carry the per-shot gunshot
	// here (GS_*), reload rows the completion sound. Ordered delivery uses the batch.
	// [orig: ActionSlot_FinishActivePhase @ 0x53f7b0 -> the end shim @ 0x401100 plays
	//  ActionDef+12 at the owner entity].
	out["action_end_serial"] = static_cast<int64_t>(weapon_action_end_serial_);
	if (weapon_action_finished_ >= 0 &&
			weapon_action_finished_ < opennova::world::weapon_action::kCount) {
		out["action_end_soundset"] =
				String::utf8(weapon_def_.actions[weapon_action_finished_].soundsetend);
	} else {
		out["action_end_soundset"] = String();
	}
	// The PowerThrow windup for the HUD charge bar [orig: HUD_DrawPowerThrowChargeBar
	// @ 0x599830 (ex kong "HUD_DrawWeaponReloadBar" misnomer — it only draws the
	// windup): gates = def+8 sign bit, g_fireChargeStartTick != 0, ammo available;
	// the drawer derives the fill from held ticks].
	const bool windup_active = (weapon_def_.flags & 0x80000000u) != 0 &&
			power_throw_start_tick_ != 0 && world_ != nullptr &&
			(active_slot.clip > 0 || weapon_def_.clip_capacity < 0);
	out["windup_active"] = windup_active;
	out["windup_held_ticks"] = windup_active
			? static_cast<int64_t>(world_->logic_tick - power_throw_start_tick_)
			: static_cast<int64_t>(0);
	out["fired_serial"] = static_cast<int64_t>(weapon_fired_serial_);
	out["dry_serial"] = static_cast<int64_t>(weapon_dry_serial_);
	out["reload_serial"] = static_cast<int64_t>(weapon_reload_serial_);
	out["unscope_serial"] = static_cast<int64_t>(weapon_unscope_serial_);
	out["rescope_serial"] = static_cast<int64_t>(weapon_rescope_serial_);
	out["clip"] = active_slot.clip;
	out["reserve"] = active_slot.reserve;
	out["kick"] = static_cast<int>(active_slot.kick);
	// Weapon heat, clamped where the original's info builder clamps it — the drawer
	// downstream reads a plain 0..0xFFFF level and self-hides at 0.
	// [orig: HUD_BuildEntityInfo @ 0x4b852e -> hudInfo+60, the clamp @ 0x4b854d]
	{
		const int32_t heat = world_ != nullptr
				? opennova::world::weapon_slot_accumulated_heat(
						  weapon_def_, active_slot,
						  static_cast<int32_t>(world_->logic_tick))
				: 0;
		out["heat"] = heat > opennova::world::weapon_heat::kFull
				? opennova::world::weapon_heat::kFull
				: heat;
	}
	out["borrowed_usegun_slot"] = local_usegun_slot_active_;
	out["emplaced_controls_valid"] = false;
	out["emplaced_gun_yaw"] = 0;
	out["emplaced_gun_pitch"] = 0;
	if (world_ && world_->cached.local_player.valid()) {
		const opennova::world::Entity *local =
				world_->registry.get(world_->cached.local_player);
		const opennova::world::Entity *mount =
				local != nullptr && local->mounted &&
						local->mount_type == opennova::world::SeatType::Gunner
				? world_->registry.get(local->mount_target)
				: nullptr;
		EmplacedWeaponControls emplaced;
		if (mount != nullptr &&
				mount->primary_weapon_owner == local->handle &&
				emplaced_weapon_controls_for(
						*world_, ai_.get(), *mount, emplaced)) {
			out["emplaced_controls_valid"] = true;
			out["emplaced_gun_yaw"] =
					static_cast<int>(emplaced.gun_yaw);
			out["emplaced_gun_pitch"] =
					static_cast<int>(emplaced.gun_pitch);
		}
	}
	// Read-only diagnostics for the local FIRE -> RoundData_AddRound seam. The last
	// row lets parity tests pin the observed tag-2 mode byte without exposing mutable
	// ring state. [orig: ((MountSlot.clip & 3) << 4) | 2 sampled before consume
	// @ WeaponAction_Fire 0x542c11 / 0x542c75].
	out["round_ring_count"] = world_ ? world_->rounds.count : 0;
	if (world_ && world_->rounds.count > 0) {
		const int last = world_->rounds.cursor == 0
				? opennova::world::RoundRing::kCapacity - 1
				: world_->rounds.cursor - 1;
		const opennova::world::RoundEvent &round = world_->rounds.records[
				static_cast<std::size_t>(last)];
		out["last_round_flags"] = round.mode_flags;
		out["last_round_subtype"] = round.subtype;
		out["last_round_slot_byte"] = round.slot_byte;
		out["last_round_seq"] = round.shot_seq;
	}
	// The 3P body's weapon channel (the entity's secondary AnimMap channel): the clip key
	// + its own playhead for the host's mask-bone override. The key remains populated
	// when the state id matches the primary because the two playheads are independent.
	// Empty means the override gate is off (weapon in hands + allowed mount class +
	// primary state flag 0x40).
	// [orig: gate @ 0x4b14a7; producer @ 0x4b5dad; world-wac-ai-re.md §14.8].
	out["body_anim_key"] = String();
	out["body_anim_phase"] = 0;
	if (world_ && world_->ai && world_->cached.local_player.valid()) {
		const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
		const opennova::world::Entity *entity =
				world_->registry.get(world_->cached.local_player);
		const bool blocked_mount =
				entity != nullptr && mount_blocks_weapon_channel(*entity);
		if (p && entity && opennova::world::infantry_weapon_channel_visible(
					p->inf, weapon_active_, blocked_mount)) {
			out["body_anim_key"] = infantry_anim_key(p->inf.wpn_state);
			out["body_anim_phase"] = p->inf.wpn_clip_phase;
		}
	}
	return out;
}

Array NovaSimulation::drain_local_player_weapon_events() {
	Array out;
	const uint32_t now = world_ ? world_->logic_tick : 0;
	for (const PendingWeaponEvent &event : pending_weapon_events_) {
		Dictionary row;
		// Unsigned subtraction intentionally preserves age across logic-tick wrap.
		row["age_ticks"] = static_cast<int64_t>(now - event.tick);
		row["world_position"] = event.world_position;
		row["anim_key"] = event.anim_key;
		row["anim_variant"] = event.anim_variant;
		row["action_started"] = event.action_started;
		row["action_soundset"] = event.action_soundset;
		row["action_particle"] = event.action_particle;
		row["action_particle_userpoint"] = event.action_particle_userpoint;
		row["scope_settled"] = event.scope_settled;
		row["third_person"] = event.third_person;
		row["vehicle_attack_context"] = event.vehicle_attack_context;
		row["action_finished"] = event.action_finished;
		row["action_end_soundset"] = event.action_end_soundset;
		row["action_effect"] = event.action_effect;
		row["effect_particle"] = event.effect_particle;
		row["effect_particle_userpoint"] = event.effect_particle_userpoint;
		row["switch_to_weapon"] = event.switch_to_weapon;
		row["clear_weapon"] = event.clear_weapon;
		row["preserve_slot_state"] = event.preserve_slot_state;
		row["switch_denied"] = event.switch_denied;
		out.push_back(row);
	}
	pending_weapon_events_.clear();
	return out;
}

// Drain the round impacts the flight sim resolved since the last call, each row already
// resolved through the ammo effects_table (canonical tag -> {effect, sound}) and its
// per-leg presentation mask; rows with no enabled authored leg are dropped, matching
// the original impact presenter [orig: AmmoDef_ProcessImpactEffect @ 0x40a170;
// ballistic wrapper Projectile_SpawnImpactEffect @ 0x4e9b80; selection witness
// on world/round_sim.h RoundImpact].
Array NovaSimulation::drain_round_impacts() {
	Array out;
	if (!world_) return out;
	const uint32_t now = world_->logic_tick;
	for (const opennova::world::RoundImpact &imp : world_->round_sim.impacts) {
		const opennova::world::AmmoTableEntry *ammo = world_->ammo.by_index(imp.ammo_index);
		if (ammo == nullptr) continue;
		if (imp.effect_tag < 0 || imp.effect_tag >= opennova::world::kImpactEffectTagCount)
			continue;
		const opennova::world::AmmoImpactEffectRow &row = ammo->impact_effects[imp.effect_tag];
		const bool has_effect = imp.present_effect && !row.effect.empty();
		const bool has_sound = imp.present_sound && !row.sound.empty();
		if (!has_effect && !has_sound) continue;
		Dictionary d;
		// mission (x,y,z) -> Godot (x, z, -y), the get_local_player_position convention.
		d["position"] = Vector3(imp.position.x, imp.position.z, -imp.position.y);
		d["direction"] = Vector3(imp.direction.x, imp.direction.z, -imp.direction.y);
		d["effect"] = has_effect ? String::utf8(row.effect.c_str()) : String();
		d["sound"] = has_sound ? String::utf8(row.sound.c_str()) : String();
		// A lifecycle rewind must never turn a future/stale source tick into an
		// unsigned multi-billion-tick particle pre-age request.
		const uint32_t age_ticks = now >= imp.tick ? now - imp.tick : 0u;
		d["age_ticks"] = static_cast<int64_t>(age_ticks);
		d["source_tick"] = static_cast<int64_t>(imp.tick);
		d["source_order"] = static_cast<int64_t>(imp.source_order);
		out.push_back(d);
	}
	world_->round_sim.impacts.clear();
	return out;
}

// HUD health/team. The original rebuilds these into its per-frame HUD info struct every frame
// (health ratio at +92 = currentHealth/maxHealth, team byte at +374). We surface the raw values
// and let the HUD compute the ratio. [orig: HUD_BuildEntityInfo @0x4b8440]
int NovaSimulation::get_local_player_health() const {
	if (!world_ || !world_->cached.local_player.valid()) return 0;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e ? e->health : 0;
}

int NovaSimulation::get_local_player_max_health() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 100;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p || p->inf.max_health <= 0) return 100;
	return p->inf.max_health;
}

int NovaSimulation::get_local_player_team() const {
	if (!world_ || !world_->cached.local_player.valid()) return 0;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e ? static_cast<int>(e->team) : 0;
}

int NovaSimulation::get_local_player_class() const {
	if (!world_ || !world_->cached.local_player.valid()) return 0;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e ? static_cast<int>(e->player_class) : 0;
}

String NovaSimulation::get_local_player_weapon_name() const {
	if (!world_ || !world_->cached.local_player.valid()) return String();
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (!e) return String();
	const opennova::world::WeaponTableEntry *weapon =
			world_->weapons.by_index(e->equipped_adm_index);
	return weapon ? String(weapon->name.c_str()) : String();
}

void NovaSimulation::restart() {
	if (!loaded_ || !have_baseline_) return;
	const bool usegun_was_active = local_usegun_slot_active_;
	const bool usegun_was_pending =
			local_usegun_switch_ != LocalUseGunSwitch::kNone;
	const uint8_t saved_personal_adm = local_usegun_saved_adm_;
	pending_weapon_events_.clear();
	power_throw_start_tick_ = 0;
	pending_throw_charge_ = 0;
	weapon_fire_held_ = false;
	weapon_fire_pressed_ = false;
	weapon_reload_pressed_ = false;
	local_usegun_switch_ = LocalUseGunSwitch::kNone;
	local_usegun_slot_active_ = false;
	local_usegun_mount_ = opennova::world::EntityHandle{};
	local_usegun_weapon_adm_ = 0xFF;
	local_usegun_pending_mount_ = opennova::world::EntityHandle{};
	local_usegun_pending_weapon_adm_ = 0xFF;
	local_usegun_saved_adm_ = 0xFF;
	local_usegun_switch_action_ = -1;
	weapon_switch_deferred_action_ = -1;
	world_->restore(baseline_); // rewinds registry/vars/env/clock + re-inits systems (incl. AI;
	                            // WacSystem::on_load also resets its 62-tick accumulator)
	// The baseline is captured during finish_load, before MissionRuntime supplies
	// items.def. Restore those authoritative callback/health traits first; the
	// encoder and the client classifier must agree on every 0x0A record width.
	if (item_traits_db_.is_valid()) resolve_item_traits(item_traits_db_);
	if (listen_server_ && !joiner_ && runtime_) {
		// Stop restores the authoritative registry, including NoNetworkCallback
		// attachment children, but those children never have a live 0x0A body that
		// could recreate a row erased from the host's decoded ClientState. Start a
		// fresh HostClient view and replay the same production load batches, in the
		// witnessed stream order, so restored runtime identities materialize now
		// instead of inheriting a prior play epoch's rows and handle caches.
		host_loop_.clear();
		runtime_ = std::make_unique<opennova::np::ClientRuntime>(host_loop_);
		install_item_class_resolver();
		opennova::netsim::NetClientView &view = runtime_->view();
		view.apply(0x10, opennova::encode_static_entity_batch(
				opennova::netsim::build_pool2_static_batch(*world_)));
		view.apply(0x0D, opennova::encode_pool_spawn_batch(
				opennova::netsim::build_pool1_spawn_batch(*world_)));
		view.apply(0x0C, opennova::encode_organic_spawn_batch(
				opennova::netsim::build_pool0_organic_batch(
						*world_, world_->cached.local_player)));
		view.apply(0x20, opennova::encode_pool3_sync_batch(
				opennova::netsim::build_pool3_marker_batch(*world_)));
	}
	reset_infantry_adm_ids();
	resolve_new_infantry_adm_ids();
	if (usegun_was_active) {
		// The world snapshot restores the play-start entity set, while the host
		// still presents the borrowed emplacement definition. Reinstall the saved
		// personal selection as a fresh restart epoch; MissionRuntime delivers this
		// event synchronously while stopped.
		if (opennova::world::Entity *player =
					world_->registry.get(world_->cached.local_player))
			player->equipped_adm_index = saved_personal_adm;
		weapon_active_ = false;
		const opennova::world::WeaponTableEntry *saved_def =
				world_->weapons.by_index(saved_personal_adm);
		weapon_start_in_switchto_ = saved_def != nullptr;
		PendingWeaponEvent event;
		event.tick = world_->logic_tick;
		event.world_position = get_local_player_position();
		event.switch_to_weapon = saved_def != nullptr
				? String::utf8(saved_def->name.c_str())
				: String();
		event.clear_weapon = saved_def == nullptr;
		pending_weapon_events_.push_back(std::move(event));
	} else if (usegun_was_pending) {
		// The presenter never left the personal weapon, but its outgoing slot may
		// already be inside SWITCHFROM/RANK. Cancel only that action state while
		// retaining the personal magazine and reserve.
		const int32_t clip = weapon_slot_.clip;
		const int32_t reserve = weapon_slot_.reserve;
		weapon_slot_ = opennova::world::WeaponSlotState{};
		weapon_slot_.clip = clip;
		weapon_slot_.reserve = reserve;
	}
	if (collision_item_db_.is_valid() && collision_placer_.is_valid())
		resolve_collision_instances(collision_item_db_, collision_placer_.ptr());
	weapon_anim_tick_ = world_->logic_tick;
	reset_local_player_view_effects();
	// The restored world can share a tick number with a previously cached view.
	// Force the next FollowOwner query to rebuild against the post-restart epoch.
	invalidate_present_effect_pose_cache();
}

Array NovaSimulation::drain_effects() {
	Array out;
	if (!loaded_) return out;
	for (const opennova::world::Effect &e : world_->effects.entries()) {
		Dictionary d;
		d["kind"] = String(e.kind.c_str());
		d["a"] = e.a;
		d["b"] = e.b;
		d["c"] = e.c;
		d["d"] = e.d;
		if (e.kind == "vehicle_control_started" ||
				e.kind == "vehicle_control_stopped")
			d["wire_handle"] = e.d;
		d["str"] = String(e.str.c_str());
		out.push_back(d);
	}
	world_->effects.clear();
	return out;
}

// The host fire-presentation drain — see the header note. Direction math mirrors
// the round spawn's mission-frame forward (cos yaw * cp, sin yaw * cp, sin pitch)
// [orig: RoundData_SpawnRound @0x4ec5e9], axis-mapped mission -> godot (x, z, -y).
Array NovaSimulation::drain_fire_presentation_events() {
	Array out;
	if (!loaded_) return out;
	constexpr double kRadPerBam = (2.0 * 3.14159265358979323846) / 4294967296.0;
	const bool have_local = world_->cached.local_player.valid();
	const uint16_t local_packed = have_local ? world_->cached.local_player.packed : 0xFFFF;
	for (const opennova::world::FireEvent &fe : world_->round_sim.fired) {
		Dictionary d;
		d["origin"] = Vector3(fe.origin.x, fe.origin.z, -fe.origin.y);
		const double bearing = static_cast<double>(fe.yaw_bam) * kRadPerBam;
		const double pitch = static_cast<double>(fe.pitch_bam) * kRadPerBam;
		const double cp = std::cos(pitch);
		d["forward"] = Vector3(static_cast<real_t>(std::cos(bearing) * cp),
				static_cast<real_t>(std::sin(pitch)),
				static_cast<real_t>(-std::sin(bearing) * cp));
		d["shooter_handle"] = static_cast<int>(fe.shooter_handle);
		opennova::world::EntityHandle shooter_handle;
		shooter_handle.packed = fe.shooter_handle;
		const opennova::world::Entity *shooter = world_->registry.get(shooter_handle);
		d["source_bms_id"] = shooter != nullptr ? shooter->bms_id : 0;
		d["is_local_player"] = have_local && fe.shooter_handle == local_packed;
		d["ammo_index"] = fe.ammo_index;
		const opennova::world::AmmoTableEntry *ammo = world_->ammo.by_index(fe.ammo_index);
		d["sound_set"] = ammo ? String(ammo->ai_launch_set.c_str()) : String();
		d["effect"] = ammo ? String(ammo->ai_launch_effect.c_str()) : String();
		d["mf_light"] = ammo ? ammo->mf_light : 0;
		out.push_back(d);
	}
	world_->round_sim.fired.clear();
	return out;
}

// The destruction presentation drain (world/destruction.h; §24): one call per
// present, converting the sim's events into godot-space dictionaries. Mission
// (x, y, z-up) -> Godot (x, z, -y), the drain_fire_presentation_events rule.
Dictionary NovaSimulation::drain_destruction_events() {
	Dictionary out;
	if (!loaded_) return out;
	opennova::world::DestructionEvents &ev = world_->destruction;
	auto to_godot = [](const opennova::world::Vec3 &v) {
		return Vector3(v.x, v.z, -v.y);
	};
	Array effects;
	for (const opennova::world::DestructionEffectEvent &e : ev.effects) {
		Dictionary d;
		d["effect"] = String(e.effect.c_str());
		d["pos"] = to_godot(e.pos);
		d["dir"] = to_godot(e.dir);
		d["attach_net_id"] = static_cast<int>(e.attach_net_id);
		d["attach_bms_id"] = e.attach_bms_id;
		d["attach_wire_handle"] = static_cast<int>(e.attach_wire_handle);
		d["attach_spawn_origin"] =
				static_cast<int64_t>(e.attach_spawn_origin);
		d["family"] = static_cast<int>(e.family);
		effects.push_back(d);
	}
	Array sounds;
	for (const opennova::world::DestructionSoundEvent &s : ev.sounds) {
		Dictionary d;
		d["sound"] = String(s.sound.c_str());
		d["pos"] = to_godot(s.pos);
		sounds.push_back(d);
	}
	Array husks;
	for (const opennova::world::HuskSwapEvent &h : ev.husk_swaps) {
		Dictionary d;
		d["net_id"] = static_cast<int>(h.net_id);
		d["wire_handle"] = static_cast<int>(h.wire_handle);
		d["bms_id"] = h.bms_id;
		d["spawn_origin"] = static_cast<int64_t>(h.spawn_origin);
		d["item_id"] = h.item_id;
		d["spawned_piece_mask"] = static_cast<int64_t>(h.spawned_piece_mask);
		d["pos"] = to_godot(h.pos);
		husks.push_back(d);
	}
	Array bursts;
	for (const opennova::world::SectionDebrisEvent &b : ev.debris_bursts) {
		Dictionary d;
		d["net_id"] = static_cast<int>(b.net_id);
		d["bms_id"] = b.bms_id;
		d["spawn_origin"] = static_cast<int64_t>(b.spawn_origin);
		d["item_id"] = b.item_id;
		d["pos"] = to_godot(b.pos);
		d["blast_center"] = to_godot(b.blast_center);
		d["has_blast_center"] = b.blast_center.x != 0.0f || b.blast_center.y != 0.0f ||
				b.blast_center.z != 0.0f;
		bursts.push_back(d);
	}
	Array glass;
	for (const opennova::world::GlassBreakEvent &g : ev.glass_breaks) {
		Dictionary d;
		d["net_id"] = static_cast<int>(g.net_id);
		d["bms_id"] = g.bms_id;
		d["spawn_origin"] = static_cast<int64_t>(g.spawn_origin);
		d["item_id"] = g.item_id;
		d["blast_pos"] = to_godot(g.blast_pos);
		d["radius"] = g.radius;
		glass.push_back(d);
	}
	out["effects"] = effects;
	out["sounds"] = sounds;
	out["husk_swaps"] = husks;
	out["debris_bursts"] = bursts;
	out["glass_breaks"] = glass;
	out["explosions_processed"] = ev.explosions_processed;
	out["items_destroyed"] = ev.items_destroyed;
	ev.clear();
	return out;
}

// The live death-piece pool snapshot — the present pass renders each piece as
// its single husk-model section [orig: the piece render mask piece[31]; §24].
Array NovaSimulation::get_death_pieces() const {
	Array out;
	if (!loaded_) return out;
	for (size_t slot = 0; slot < world_->death_pieces.pieces.size(); ++slot) {
		const opennova::world::DeathPiece &p = world_->death_pieces.pieces[slot];
		if (!p.active) continue;
		Dictionary d;
		d["slot"] = static_cast<int>(slot);
		d["generation"] = static_cast<int64_t>(p.generation);
		d["item_id"] = p.item_id;
		d["section"] = static_cast<int>(p.section);
		d["type_index"] = static_cast<int>(p.type_index);
		d["scale"] = p.render_scale;
		d["pos"] = Vector3(p.pos.x, p.pos.z, -p.pos.y);
		d["heading"] = p.heading;
		d["pitch"] = p.pitch;
		d["settled"] = p.settled;
		out.push_back(d);
	}
	return out;
}


// Per-entity destruction diagnostics (probe/F3 seam): the gate inputs the
// damage chain reads, resolved by bms_id. {} = no such entity.
Dictionary NovaSimulation::get_destruction_debug(int p_bms_id) const {
	Dictionary out;
	if (!world_) return out;
	const opennova::world::Entity *found = nullptr;
	world_->registry.for_each([&](const opennova::world::Entity &e) {
		if (found == nullptr && e.bms_id == p_bms_id) found = &e;
	});
	if (found == nullptr) return out;
	out["bms_id"] = found->bms_id;
	out["net_id"] = static_cast<int>(found->net_id);
	out["kind"] = static_cast<int>(found->kind);
	out["pool"] = found->handle.pool();
	out["item_id"] = found->item_id;
	out["health"] = found->health;
	out["health_max"] = found->health_max;
	out["alive"] = found->alive;
	out["bound_radius"] = found->bound_radius;
	out["engine_flags"] = static_cast<int64_t>(found->engine_flags);
	out["is_ai_capable"] = found->is_ai_capable;
	out["has_collision_instance"] =
			collision_world_.has_instance(*world_, found->handle);
	const opennova::world::ItemDeathTraits *t =
			world_->item_death_traits.get(found->item_id);
	out["has_death_traits"] = t != nullptr;
	if (t != nullptr) {
		out["armor_impact"] = t->armor_impact;
		out["armor_blast"] = t->armor_blast;
		out["unit_type"] = t->unit_type;
		out["kz"] = t->kz;
		out["has_husk"] = t->has_husk;
		out["husk_model_loaded"] = t->husk_model_loaded;
		PackedVector3Array kz_points;
		for (const opennova::world::Vec3 &point : t->kz_points)
			kz_points.push_back(Vector3(point.x, point.y, point.z));
		out["kz_point_count"] = static_cast<int64_t>(t->kz_points.size());
		out["kz_points"] = kz_points;
	}
	out["pos"] = Vector3(found->position.x, found->position.z, -found->position.y);
	return out;
}

// The live tracer trail channels for the ribbon layer — see the header note.
// Mission -> godot axis map (x, z, -y), matching the other presentation drains.
PackedFloat32Array NovaSimulation::get_tracer_trails() const {
	PackedFloat32Array out;
	if (!loaded_) return out;
	for (const opennova::world::TracerTrailChannel &c : world_->round_sim.trails.channels) {
		if (!c.active || c.count <= 0) continue;
		const int64_t base = out.size();
		out.resize(base + 3 + static_cast<int64_t>(c.count) * 4);
		float *w = out.ptrw() + base;
		w[0] = static_cast<float>(c.style_id);
		w[1] = static_cast<float>(c.age);
		w[2] = static_cast<float>(c.count);
		float *pw = w + 3;
		for (int i = 0; i < c.count; ++i, pw += 4) {
			const opennova::world::TracerTrailPoint &p = c.pts[static_cast<size_t>(i)];
			pw[0] = p.pos.x;
			pw[1] = p.pos.z;
			pw[2] = -p.pos.y;
			pw[3] = p.w;
		}
	}
	return out;
}

void NovaSimulation::set_wac_program(const Ref<NovaWacProgram> &p_program) {
	wac_program_ = p_program;
	if (!loaded_ || !wac_) {
		return; // finish_load applies it on the next load
	}
	if (wac_program_.is_valid() && wac_program_->is_ok()) {
		wac_->set_program(wac_program_->native_program());
	} else {
		wac_->set_program(opennova::wac::Program());
	}
}

bool NovaSimulation::compile_and_set_wac(const PackedStringArray &p_sources) {
	ERR_FAIL_COND_V_MSG(!loaded_, false, "compile_and_set_wac needs a loaded world (the registry resolves symbolic names).");
	std::vector<std::string> sources;
	sources.reserve(static_cast<size_t>(p_sources.size()));
	for (int64_t i = 0; i < p_sources.size(); ++i) {
		const CharString utf8 = p_sources[i].utf8();
		sources.emplace_back(utf8.get_data(), static_cast<size_t>(utf8.length()));
	}
	opennova::wac::CompileEnv env;
	env.registry = &world_->registry;
	opennova::wac::Program program = opennova::wac::compile_program(sources, env);
	Ref<NovaWacProgram> holder;
	holder.instantiate();
	// Adopt the registry-compiled program into the holder so get_wac_program()
	// exposes its diagnostics either way.
	holder->adopt(std::move(program));
	wac_program_ = holder;
	if (!wac_program_->is_ok()) {
		return false;
	}
	wac_->set_program(wac_program_->native_program());
	return true;
}

Dictionary NovaSimulation::get_wac_state() const {
	Dictionary out;
	out["loaded"] = wac_ != nullptr && wac_->vm().loaded();
	out["paused"] = wac_ != nullptr && wac_->paused;
	out["runs"] = wac_ != nullptr ? static_cast<int64_t>(wac_->runs()) : 0;
	out["event_count"] = wac_ != nullptr ? wac_->program().event_count : 0;
	out["code_size"] = wac_ != nullptr ? static_cast<int>(wac_->program().code.size()) : 0;
	return out;
}

Dictionary NovaSimulation::get_runtime_perf_counters() const {
	Dictionary out;
	out["loaded"] = loaded_;
	out["listen_server"] = listen_server_;
	out["ai_count"] = ai_ ? ai_->count() : 0;
	out["present_entity_count"] = last_present_entity_count_;
	out["sim_tick_us"] = static_cast<int64_t>(last_sim_tick_us_);
	out["net_tick_us"] = static_cast<int64_t>(last_net_tick_us_);
	out["present_snapshot_us"] = static_cast<int64_t>(last_present_snapshot_us_);
	return out;
}

void NovaSimulation::set_wac_paused(bool p_paused) {
	if (wac_) {
		wac_->paused = p_paused; // [orig: dword_C6EB28]
	}
}

bool NovaSimulation::is_wac_paused() const {
	return wac_ != nullptr && wac_->paused;
}

void NovaSimulation::set_mission_variable(int index, int value) {
	if (world_) world_->vars.set_mission(index, value);
}

// Probe/diagnostic seam beside get_entity_debug: write an AI entity's health through
// the same stores the scripted SETHP path touches (registry + the motor copy)
// [orig: the WAC SETHP op writes entity+286]. Lets in-game probes shorten a fight
// without bypassing the damage/death chain under test.
void NovaSimulation::debug_set_entity_health(int p_index, int p_hp) {
	if (!ai_ || !world_) return;
	AiEntity *e = ai_->at(p_index);
	if (!e) return;
	e->health = static_cast<int16_t>(p_hp);
	if (opennova::world::Entity *ent = world_->registry.get(e->handle)) {
		ent->health = p_hp;
		ent->alive = p_hp > 0;
	}
}

// The D-AI-6 muzzle seam: the present layer pushes each posed model's gun-flash
// userpoint world position back to the sim once per frame; the AI fire pass spawns
// rounds from it while fresh. Godot (x, up, z) -> mission (x, -gz, gy) in 16.16
// fixed — the inverse of the present mapping godot = (mx, mz, -my).
// [orig: Entity_GetAttachmentWorldPosition @0x4b2670 from the anim-event fire block
// @0x4bf326 — computed inline against the engine-side skeleton; ours is host-fed.]
void NovaSimulation::set_ai_muzzle_world(int p_net_id, const Vector3 &p_godot_pos) {
	if (!ai_ || !world_) return;
	// Keyed by the row's PF_NET_ID (the authored SSN) — the wire handle is
	// 0-ambiguous for pool-0 slot 0, and the row order is not the AI index.
	// for_handle inside set_entity_muzzle drops non-AI entities.
	if (p_net_id <= 0 || p_net_id > 0xFFFF) return;
	const opennova::world::EntityHandle h =
			world_->registry.find_by_net_id(static_cast<uint16_t>(p_net_id));
	if (!h.valid()) return;
	const int32_t pos[3] = {
		static_cast<int32_t>(p_godot_pos.x * 65536.0f),
		static_cast<int32_t>(-p_godot_pos.z * 65536.0f),
		static_cast<int32_t>(p_godot_pos.y * 65536.0f),
	};
	if (opennova::world::Entity *entity = world_->registry.get(h)) {
		entity->posed_muzzle_world[0] = pos[0];
		entity->posed_muzzle_world[1] = pos[1];
		entity->posed_muzzle_world[2] = pos[2];
		entity->posed_muzzle_tick = world_->logic_tick;
		entity->posed_muzzle_valid = true;
	}
	ai_->set_entity_muzzle(h, pos, world_->logic_tick);
}

// Probe seam beside debug_set_entity_health: teleport an AI entity through both
// position stores (registry + motor copy) — mission-space coordinates. Lets
// in-game probes bring a reachable victim to the player when the mission
// geography (interiors, fences) defeats straight-line navigation.
void NovaSimulation::debug_set_entity_position(int p_index, const Vector3 &p_mission_pos) {
	if (!ai_ || !world_) return;
	AiEntity *e = ai_->at(p_index);
	if (!e) return;
	e->pos[0] = static_cast<int32_t>(p_mission_pos.x * 65536.0f);
	e->pos[1] = static_cast<int32_t>(p_mission_pos.y * 65536.0f);
	e->pos[2] = static_cast<int32_t>(p_mission_pos.z * 65536.0f);
	if (opennova::world::Entity *ent = world_->registry.get(e->handle)) {
		ent->position.x = p_mission_pos.x;
		ent->position.y = p_mission_pos.y;
		ent->position.z = p_mission_pos.z;
	}
}

// World-registry probe seams keyed by SSN — pool-1 vehicles (and anything else
// without an AI brain) are invisible to the AI-index seams above; vehicle probes
// need to find and place them. Mission-space coordinates, same convention as
// debug_set_entity_position.
Dictionary NovaSimulation::get_world_entity_debug(int p_net_id) const {
	Dictionary out;
	if (!world_ || p_net_id <= 0 || p_net_id > 0xFFFF) return out;
	const opennova::world::EntityHandle h =
			world_->registry.find_by_net_id(static_cast<uint16_t>(p_net_id));
	const opennova::world::Entity *ent = world_->registry.get(h);
	if (!ent) return out;
	out["net_id"] = static_cast<int>(ent->net_id);
	out["bms_id"] = ent->bms_id;
	out["pool"] = h.pool();
	out["alive"] = ent->alive;
	out["health"] = ent->health;
	out["mission_position"] = Vector3(ent->position.x, ent->position.y, ent->position.z);
	out["position"] = Vector3(ent->position.x, ent->position.z, -ent->position.y);
	out["yaw"] = static_cast<int>(ent->yaw);
	out["seat_count"] = static_cast<int>(ent->seats.size());
	Array seats;
	for (const opennova::world::Seat &s : ent->seats) {
		Dictionary sd;
		sd["type"] = static_cast<int>(s.type);
		sd["occupied"] = s.occupant.valid();
		sd["local"] = Vector3(s.seat_local.x, s.seat_local.y, s.seat_local.z);
		sd["name"] = String(s.source_name.c_str());
		seats.push_back(sd);
	}
	out["seats"] = seats;
	return out;
}

void NovaSimulation::debug_set_world_entity_position(int p_net_id,
                                                     const Vector3 &p_mission_pos) {
	if (!world_ || p_net_id <= 0 || p_net_id > 0xFFFF) return;
	const opennova::world::EntityHandle h =
			world_->registry.find_by_net_id(static_cast<uint16_t>(p_net_id));
	opennova::world::Entity *ent = world_->registry.get(h);
	if (!ent) return;
	ent->position.x = p_mission_pos.x;
	ent->position.y = p_mission_pos.y;
	ent->position.z = p_mission_pos.z;
	// Keep the AI mirror in step when the entity carries a brain (harmless otherwise).
	if (ai_) {
		if (AiEntity *ae = ai_->for_handle(h)) {
			ae->pos[0] = static_cast<int32_t>(p_mission_pos.x * 65536.0f);
			ae->pos[1] = static_cast<int32_t>(p_mission_pos.y * 65536.0f);
			ae->pos[2] = static_cast<int32_t>(p_mission_pos.z * 65536.0f);
		}
	}
}

int NovaSimulation::get_mission_variable(int index) const {
	return world_ ? world_->vars.get_mission(index) : 0;
}

Dictionary NovaSimulation::get_round_outcome_debug() const {
	Dictionary out;
	if (!world_) return out;
	out["ended"] = world_->round_end.ended;
	out["winner_team"] = world_->round_end.winner_team;
	out["bluekills"] = world_->kill_stats.bluekills_by_player;
	out["greenkills"] = world_->kill_stats.greenkills_by_player;
	out["enemy_kills"] = world_->kill_stats.enemy_kills_by_player;
	out["team_kills_by_others"] = world_->kill_stats.team_kills_by_others;
	out["friendly_kills_by_others"] = world_->kill_stats.friendly_kills_by_others;
	out["enemy_kills_by_others"] = world_->kill_stats.enemy_kills_by_others;
	out["humans"] = world_->cached.humans;
	out["mp_session"] = world_->mp_session;
	return out;
}

bool NovaSimulation::has_event_fired(int index) const {
	if (!bms_ || index < 0) return false;
	// The active latch + delay-elapsed window — the same read the original's Event
	// trigger category makes [orig: EventTrigger_EvaluateCondition @0x453620 case 3].
	return bms_->event_fired(static_cast<size_t>(index));
}

int NovaSimulation::get_event_count() const {
	return bms_ ? static_cast<int>(bms_->events().size()) : 0;
}

int64_t NovaSimulation::get_logic_tick() const {
	// uint32 -> int64 keeps long sessions sign-safe on the GDScript side.
	return world_ ? static_cast<int64_t>(world_->logic_tick) : 0;
}

void NovaSimulation::set_panm_time_ms(int64_t p_time_ms) {
	panm_time_override_ms_ = p_time_ms < 0
			? -1
			: static_cast<int64_t>(static_cast<uint32_t>(p_time_ms));
}

int64_t NovaSimulation::get_panm_time_ms() const {
	return panm_time_override_ms_;
}

void NovaSimulation::debug_set_panm_time_ms(int64_t p_time_ms) {
	set_panm_time_ms(p_time_ms);
}

namespace {
PackedInt32Array snapshot_bank(const opennova::world::World *world, int count,
                               int32_t (opennova::world::ScriptVarStore::*getter)(int) const) {
	PackedInt32Array out;
	out.resize(count);
	int32_t *w = out.ptrw();
	for (int i = 0; i < count; ++i) {
		w[i] = world ? (world->vars.*getter)(i) : 0;
	}
	return out;
}
} // namespace

PackedInt32Array NovaSimulation::get_mission_variables_snapshot() const {
	return snapshot_bank(world_.get(), opennova::world::ScriptVarStore::kMissionVars,
	                     &opennova::world::ScriptVarStore::get_mission);
}

PackedInt32Array NovaSimulation::get_global_variables_snapshot() const {
	return snapshot_bank(world_.get(), opennova::world::ScriptVarStore::kGlobalVars,
	                     &opennova::world::ScriptVarStore::get_global);
}

PackedInt32Array NovaSimulation::get_music_variables_snapshot() const {
	return snapshot_bank(world_.get(), opennova::world::ScriptVarStore::kMusicVars,
	                     &opennova::world::ScriptVarStore::get_music);
}

void NovaSimulation::set_global_variable(int index, int value) {
	if (world_) world_->vars.set_global(index, value);
}

int NovaSimulation::get_global_variable(int index) const {
	return world_ ? world_->vars.get_global(index) : 0;
}

PackedByteArray NovaSimulation::get_fired_events_snapshot() const {
	PackedByteArray out;
	if (!bms_) return out;
	const size_t count = bms_->events().size();
	out.resize(static_cast<int64_t>(count));
	uint8_t *w = out.ptrw();
	for (size_t i = 0; i < count; ++i) {
		w[i] = bms_->event_fired(i) ? 1 : 0;
	}
	return out;
}

Dictionary NovaSimulation::get_entity_debug(int p_index) const {
	Dictionary out;
	if (!ai_ || !world_) return out;
	AiEntity *e = ai_->at(p_index);
	if (!e) return out;
	// A scripted remove (VaporizeSingle / removeSSN) despawns the registry slot
	// while the AiEntity stays in the AI pool, so the registry block emits TYPED
	// DEFAULTS rather than dropping keys - the card's shape is stable whether
	// the entity is whole or registry-despawned.
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	out["kind"] = ent ? static_cast<int>(ent->spawn_origin >> 24) : -1;
	out["index"] = ent ? static_cast<int>(ent->spawn_origin & 0xFFFFFF) : -1;
	out["bms_id"] = ent ? ent->bms_id : 0;
	out["item_id"] = ent ? ent->item_id : 0;
	// NOTE: retail BMS names are Windows-1252; non-ASCII bytes will read as
	// invalid UTF-8 here. Names are ASCII in practice; revisit if mojibake shows.
	out["name"] = ent ? String(ent->name.c_str()) : String();
	out["group_id"] = ent ? static_cast<int>(ent->group_id) : 0;
	out["team"] = ent ? static_cast<int>(ent->team) : -1;
	out["pool"] = ent ? ent->handle.pool() : -1;
	out["engine_flags"] = ent ? static_cast<int64_t>(ent->engine_flags) : 0;
	out["waypoint_id"] = ent ? static_cast<int>(ent->waypoint_id) : 0;
	out["wp_number"] = ent ? ent->wp_number : 0;
	out["health"] = ent ? ent->health : 0;
	out["alive"] = ent ? ent->alive : false;
	out["hidden"] = ent ? ent->hidden : false;
	out["held"] = ent ? ent->held : false;
	out["disabled"] = ent ? ent->disabled : false;
	out["body_anim_slot"] = ent ? ent->body_anim_slot : -1;
	out["mounted"] = ent ? ent->mounted : false;
	out["mount_target_net_id"] = 0;
	out["mount_seat"] = ent ? static_cast<int>(ent->mount_seat) : -1;
	out["mount_type"] = ent ? static_cast<int>(ent->mount_type) : 0;
	out["mount_config_valid"] = ent ? ent->mounted_config_valid : false;
	out["mount_config"] =
			(ent && ent->mounted_config_valid) ? static_cast<int>(ent->mounted_config) : 0;
	out["mount_seat_bone"] = 0;
	out["mount_seat_pose_index"] = 0;
	out["mount_seat_source_name"] = String();
	out["mount_seat_local"] = Vector3();
	out["mount_seat_yaw_offset"] = 0;
	out["mount_target_config_valid"] = false;
	out["mount_target_config"] = 0;
	out["mount_target_seat_count"] = 0;
	out["mount_target_seats"] = Array();
	if (ent && ent->mounted) {
		const opennova::world::Entity *target = world_->registry.get(ent->mount_target);
		if (target) {
			out["mount_target_net_id"] = static_cast<int>(target->net_id);
			out["mount_target_config_valid"] = target->emplaced_config_valid;
			out["mount_target_config"] =
					target->emplaced_config_valid ? static_cast<int>(target->emplaced_config) : 0;
			out["mount_target_seat_count"] = static_cast<int>(target->seats.size());
			Array target_seats;
			for (int i = 0; i < static_cast<int>(target->seats.size()); ++i) {
				const opennova::world::Seat &seat = target->seats[i];
				Dictionary d;
				d["index"] = i;
				d["type"] = static_cast<int>(seat.type);
				d["retail_slot"] = static_cast<int>(seat.retail_slot);
				d["bone_index"] = static_cast<int>(seat.bone_index);
				d["pose_index"] = static_cast<int>(seat.pose_index);
				d["source_name"] = String(seat.source_name.c_str());
				d["local"] = Vector3(seat.seat_local.x, seat.seat_local.y, seat.seat_local.z);
				d["yaw_offset"] = static_cast<int>(seat.yaw_offset);
				d["occupied"] = seat.occupant.valid();
				target_seats.push_back(d);
			}
			out["mount_target_seats"] = target_seats;
			if (ent->mount_seat >= 0 && ent->mount_seat < static_cast<int>(target->seats.size())) {
				const opennova::world::Seat &seat = target->seats[ent->mount_seat];
				out["mount_type"] = static_cast<int>(seat.type);
				out["mount_seat_bone"] = static_cast<int>(seat.bone_index);
				out["mount_seat_pose_index"] = static_cast<int>(seat.pose_index);
				out["mount_seat_source_name"] = String(seat.source_name.c_str());
				out["mount_seat_local"] = Vector3(seat.seat_local.x, seat.seat_local.y, seat.seat_local.z);
				out["mount_seat_yaw_offset"] = static_cast<int>(seat.yaw_offset);
			}
		}
	}
	out["net_id"] = e->net_id;
	out["wire_handle"] = static_cast<int>(e->handle.packed);
	out["team"] = static_cast<int>(e->team);
	// The AI-side entity+286 mirror; diverges from the registry health under
	// some damage paths, so the card shows both.
	out["ai_health"] = static_cast<int>(e->health);
	out["position"] = get_entity_position(p_index);
	out["yaw_deg"] = get_entity_yaw_deg(p_index);
	const int state = e->brain.f[AiBrain::kCurState];
	out["state"] = state;
	out["state_name"] = ai_state_name(state);
	out["pending_state"] = e->brain.f[AiBrain::kPendState];
	out["alert"] = e->brain.f[AiBrain::kAlert];
	out["wp_channel"] = e->brain.f[AiBrain::kWpChannel];
	out["wp_node"] = e->brain.f[AiBrain::kWpNode];
	out["wp_distance"] = e->brain.f[AiBrain::kWpDistance];
	out["out_speed"] = e->brain.f[AiBrain::kOutSpeed];
	out["infantry"] = e->inf.active;
	out["adm_id"] = e->inf.active ? e->inf.adm_id : -1;
	out["adm_name"] = e->inf.active ? infantry_anim_.adm_name(e->inf.adm_id) : String();
	out["infantry_move_mode"] = e->inf.move_mode;
	out["anim_state"] = e->inf.active ? e->inf.anim_state : -1;
	out["anim_key"] = e->inf.active ? infantry_anim_key(e->inf.anim_state) : String();
	// Infantry combat diagnostics (the P1 threat-loop bring-up surface): the
	// perception/attack ranges the scan reads (AiSlot +68/+60, world units), the
	// D-AI-5 weapon seed (AiProfile ammo index + clip, the live magazine word),
	// and the current combat target.
	out["sight_range_u"] = e->slot.f[17] / 65536.0;
	out["attack_range_u"] = e->slot.f[15] / 65536.0;
	out["ammo_primary"] = e->profile.ammo_primary;
	out["clip_size"] = e->profile.clip_size;
	out["magazine"] = static_cast<int>(e->inf.magazine);
	out["combat_target_valid"] = e->inf.combat_target.valid();
	// The D-AI-6 muzzle seam readback (probe surface): the host-fed posed muzzle.
	out["muzzle_valid"] = e->muzzle_valid;
	out["muzzle"] = godot_from_fixed3(e->muzzle_world);
	// Death presentation (P1c): the damage-time selection still pending consume,
	// the live corpse countdown, and the def traits behind them (world-wac-ai-re §19).
	out["death_anim_state"] = ent ? ent->death_anim_state : 0;
	out["corpse_timer"] = ent ? ent->corpse_timer : 0;
	out["deathtime_ticks"] = ent ? ent->deathtime_ticks : 0;
	out["leave_corpse"] = ent ? ent->leave_corpse : false;
	return out;
}

String NovaSimulation::ai_state_name(int p_state) {
	return String(opennova::world::ai_state_name(p_state));
}

String NovaSimulation::infantry_anim_key(int p_state) {
	if (p_state < 0 || p_state >= opennova::world::kInfantryAnimStateCount) return String();
	const char *name = opennova::world::kInfantryAnimNames[p_state];
	if (!name || !name[0]) return String();
	return String("anim_") + String(name);
}

int64_t NovaSimulation::infantry_anim_flags(int p_state) {
	if (p_state < 0 || p_state >= opennova::world::kInfantryAnimStateCount) return 0;
	return static_cast<int64_t>(opennova::world::kInfantryAnimFlags[p_state]);
}

int NovaSimulation::get_entity_count() const {
	return ai_ ? ai_->count() : 0;
}

int NovaSimulation::get_entity_kind(int p_index) const {
	if (!ai_ || !world_) return -1;
	AiEntity *e = ai_->at(p_index);
	if (!e) return -1;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	if (!ent) return -1;
	return static_cast<int>(ent->spawn_origin >> 24); // [orig promote: (kind<<24)|index]
}

int NovaSimulation::get_entity_index(int p_index) const {
	if (!ai_ || !world_) return -1;
	AiEntity *e = ai_->at(p_index);
	if (!e) return -1;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	if (!ent) return -1;
	return static_cast<int>(ent->spawn_origin & 0xFFFFFF);
}

Vector3 NovaSimulation::get_entity_position(int p_index) const {
	if (!ai_) return Vector3();
	AiEntity *e = ai_->at(p_index);
	if (!e) return Vector3();
	// mission (x, y, z) 16.16 -> Godot (x, z, -y) world units. [orig render remap: (x, z, -y).]
	return Vector3(static_cast<float>(e->pos[0] / kFixed16),
	               static_cast<float>(e->pos[2] / kFixed16),
	               static_cast<float>(-e->pos[1] / kFixed16));
}

// The AI brain stores heading in the ENGINE frame (90 - mission yaw): the spawn seed and the
// waypoint mover (atan2(dY,dX) bearing) both use it, so a unit faces consistently whether parked or
// moving. The host basis (MissionObjectPlacer.bms_to_godot_basis) takes the MISSION yaw and internally
// applies the faithful (90 - yaw) engine heading, so the present converts engine -> mission here:
// mission_yaw = 90 - engine_heading. (Stationary units still report their authored yaw.)
float NovaSimulation::get_entity_yaw(int p_index) const {
	if (!ai_) return 0.0f;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0.0f;
	const double mission_yaw_deg = opennova::world::mission_yaw_deg_from_bam_heading(e->heading);
	return static_cast<float>(mission_yaw_deg * 0.017453292519943295);
}

float NovaSimulation::get_entity_yaw_deg(int p_index) const {
	if (!ai_) return 0.0f;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0.0f;
	return static_cast<float>(opennova::world::mission_yaw_deg_from_bam_heading(e->heading));
}

int NovaSimulation::get_entity_state(int p_index) const {
	if (!ai_) return 0;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0;
	return e->brain.f[AiBrain::kCurState];
}

int NovaSimulation::get_entity_net_id(int p_index) const {
	if (!ai_) return 0;
	AiEntity *e = ai_->at(p_index);
	return e ? e->net_id : 0;
}

// The distant MODEL/depth-mask foliage tier is the hide-in-grass mechanic: the
// sector-entity walk only calls Foliage_UpdateModelTiles around entities whose
// MoveOrder carries a stance bit (0x100 prone / 0x200 crouch) and whose
// groundEntity is empty — never around placed objects, which leave MoveOrder 0.
// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dc2/0x5c7ded (flags & 0x300),
// groundEntity gate @ 0x5c7dd5..0x5c7df7; stance writers
// Player_PackInputStateToEntity @ 0x4df6a7..0x4df6cd,
// NapiNPServerMsg_HandleStanceChange @ 0x501c60]
PackedVector3Array NovaSimulation::get_foliage_mask_anchor_positions() const {
	PackedVector3Array out;
	if (!ai_ || !world_) return out;
	for (int i = 0; i < ai_->count(); ++i) {
		AiEntity *e = ai_->at(i);
		if (!e) continue;
		const opennova::world::Entity *ent = world_->registry.get(e->handle);
		if (!ent) continue;
		if ((ent->net_stance_bits & 0x3u) == 0) continue;
		if (ent->ground_target.valid()) continue;
		out.push_back(Vector3(static_cast<float>(e->pos[0] / kFixed16),
		                      static_cast<float>(e->pos[2] / kFixed16),
		                      static_cast<float>(-e->pos[1] / kFixed16)));
	}
	return out;
}

PackedVector3Array NovaSimulation::get_entity_effect_state_for_ssn(int p_ssn) const {
	PackedVector3Array out;
	if (world_ == nullptr || p_ssn <= 0 ||
			p_ssn > static_cast<int>(std::numeric_limits<std::uint16_t>::max())) {
		return out;
	}
	const opennova::world::Entity *entity = world_->registry.get(
			world_->registry.find_by_net_id(static_cast<std::uint16_t>(p_ssn)));
	if (entity == nullptr) return out;

	out.resize(EFFECT_STATE_COUNT);
	out.set(EFFECT_STATE_POSITION, Vector3(
			entity->position.x, entity->position.z, -entity->position.y));
	out.set(EFFECT_STATE_ROTATION_DEG, Vector3(
			static_cast<float>(entity->pitch),
			static_cast<float>(entity->yaw),
			static_cast<float>(entity->roll)));
	return out;
}

void NovaSimulation::invalidate_present_effect_pose_cache() const {
	present_effect_pose_cache_valid_ = false;
	present_effect_pose_cache_runtime_ = nullptr;
	present_effect_poses_by_handle_.clear();
	present_effect_handles_by_bms_id_.clear();
	present_effect_handles_by_ssn_.clear();
	present_effect_handles_by_origin_.clear();
}

void NovaSimulation::ensure_present_effect_pose_cache() const {
	if (!world_ || !runtime_) {
		if (present_effect_pose_cache_valid_) invalidate_present_effect_pose_cache();
		return;
	}

	const opennova::netsim::ClientState &client = runtime_->state();
	const uint32_t logic_tick = world_->logic_tick;
	if (present_effect_pose_cache_valid_ &&
			present_effect_pose_cache_runtime_ == runtime_.get() &&
			present_effect_pose_cache_logic_tick_ == logic_tick &&
			present_effect_pose_cache_client_frame_ == client.frames_applied) {
		return;
	}

	present_effect_poses_by_handle_.clear();
	present_effect_handles_by_bms_id_.clear();
	present_effect_handles_by_ssn_.clear();
	present_effect_handles_by_origin_.clear();
	present_effect_poses_by_handle_.reserve(client.entities.size());
	present_effect_handles_by_bms_id_.reserve(client.entities.size());
	present_effect_handles_by_ssn_.reserve(client.entities.size());
	present_effect_handles_by_origin_.reserve(client.entities.size());

	for (const opennova::netsim::ClientEntityState &entity_state : client.entities) {
		// Match present_snapshot_from_client_view's joiner self-filter: the host's
		// wire echo H is not drawn and therefore cannot own a presented effect.
		if (joiner_ && joiner_self_wire_handle_ != 0 &&
				entity_state.handle == joiner_self_wire_handle_) {
			continue;
		}

		const int32_t heading_bam = static_cast<int32_t>(
				static_cast<uint32_t>(entity_state.yaw_byte) << 24);
		// Host/listen presentation can recover the authored pitch and roll from
		// the authoritative registry. The compact peer row only carries yaw;
		// joiners therefore retain the wire-only zeroes here.
		const opennova::world::Entity *entity = joiner_ ? nullptr : world_->registry.get(
				opennova::world::EntityHandle{entity_state.handle});
		PresentEffectPose pose;
		pose.position = Vector3(
				static_cast<float>(entity_state.x / kFixed16),
				static_cast<float>(entity_state.z / kFixed16),
				static_cast<float>(-entity_state.y / kFixed16));
		pose.rotation_deg = Vector3(
				entity ? static_cast<float>(entity->pitch) : 0.0f,
				static_cast<float>(opennova::world::mission_yaw_deg_from_bam_heading(
						heading_bam)),
				entity ? static_cast<float>(entity->roll) : 0.0f);
		present_effect_poses_by_handle_[entity_state.handle] = pose;

		// A joiner's decoded handles belong to the host, so only wire identity is
		// meaningful there. Host/listen views can resolve the same registry entity
		// used by get_present_snapshot() for BMS origin and SSN identity.
		if (joiner_) continue;
		if (!entity) continue;
		if (entity->bms_id > 0) {
			present_effect_handles_by_bms_id_[static_cast<int>(entity->bms_id)] =
					entity_state.handle;
		}
		if (entity->net_id > 0) {
			present_effect_handles_by_ssn_[static_cast<int>(entity->net_id)] =
					entity_state.handle;
		}
		const int kind = static_cast<int>(entity->spawn_origin >> 24);
		const int index = static_cast<int>(entity->spawn_origin & 0xFFFFFFu);
		present_effect_handles_by_origin_[present_effect_origin_key(kind, index)] =
				entity_state.handle;
	}

	present_effect_pose_cache_logic_tick_ = logic_tick;
	present_effect_pose_cache_client_frame_ = client.frames_applied;
	present_effect_pose_cache_runtime_ = runtime_.get();
	present_effect_pose_cache_valid_ = true;
}

PackedVector3Array NovaSimulation::cached_present_effect_state_for_handle(
		uint16_t p_handle) const {
	PackedVector3Array out;
	const auto found = present_effect_poses_by_handle_.find(p_handle);
	if (found == present_effect_poses_by_handle_.end()) return out;
	out.resize(EFFECT_STATE_COUNT);
	out.set(EFFECT_STATE_POSITION, found->second.position);
	out.set(EFFECT_STATE_ROTATION_DEG, found->second.rotation_deg);
	return out;
}

PackedVector3Array NovaSimulation::present_effect_state_for_handle(uint16_t p_handle) const {
	ensure_present_effect_pose_cache();
	return cached_present_effect_state_for_handle(p_handle);
}

PackedVector3Array NovaSimulation::get_present_effect_state_for_ssn(int p_ssn) const {
	if (p_ssn <= 0) return PackedVector3Array();
	ensure_present_effect_pose_cache();
	const auto found = present_effect_handles_by_ssn_.find(p_ssn);
	if (found == present_effect_handles_by_ssn_.end()) return PackedVector3Array();
	return cached_present_effect_state_for_handle(found->second);
}

PackedVector3Array NovaSimulation::get_present_effect_state_for_wire_handle(
		int p_wire_handle) const {
	if (p_wire_handle <= 0 ||
			p_wire_handle > static_cast<int>(std::numeric_limits<uint16_t>::max())) {
		return PackedVector3Array();
	}
	return present_effect_state_for_handle(static_cast<uint16_t>(p_wire_handle));
}

PackedVector3Array NovaSimulation::get_present_effect_state_for_bms_id(int p_bms_id) const {
	if (p_bms_id <= 0) return PackedVector3Array();
	ensure_present_effect_pose_cache();
	const auto found = present_effect_handles_by_bms_id_.find(p_bms_id);
	if (found == present_effect_handles_by_bms_id_.end()) return PackedVector3Array();
	return cached_present_effect_state_for_handle(found->second);
}

PackedVector3Array NovaSimulation::get_present_effect_state_for_origin(
		int p_kind, int p_index) const {
	if (p_kind < 0 || p_index < 0) return PackedVector3Array();
	ensure_present_effect_pose_cache();
	const auto found = present_effect_handles_by_origin_.find(
			present_effect_origin_key(p_kind, p_index));
	if (found == present_effect_handles_by_origin_.end()) return PackedVector3Array();
	return cached_present_effect_state_for_handle(found->second);
}

int NovaSimulation::get_entity_bms_id(int p_index) const {
	if (!ai_ || !world_) return 0;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	return ent ? ent->bms_id : 0;
}

// [D-NET-112] entity+0x78 ownerConnectionId (the connection/dcb that owns this entity). A networked
// PLAYER is identified by this + its handle, NOT by an SSN (players carry net_id 0). 0 = unowned (AI /
// mission entity / the host's dedicated reservation).
int NovaSimulation::get_entity_owner_connection_id(int p_index) const {
	if (!ai_ || !world_) return 0;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	return ent ? static_cast<int>(ent->owner_connection_id) : 0;
}

// The entity's wire handle (pool<<12|slot) — the per-entity identity carried on the 0x0A/0x0C wire and
// the decoded present's PF_WIRE_HANDLE. Unique per entity (unlike a player's net_id, which is now 0).
int NovaSimulation::get_entity_wire_handle(int p_index) const {
	if (!ai_) return 0;
	AiEntity *e = ai_->at(p_index);
	return e ? static_cast<int>(e->handle.packed) : 0;
}

int NovaSimulation::get_entity_part_anim_phase(int p_index, int channel) const {
	if (!ai_ || channel < 1 || channel > 2) return 0;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0;
	return e->brain.f[AiBrain::kPartAnimPhase0 + (channel - 1)];
}

bool NovaSimulation::get_entity_part_anim_active(int p_index, int channel) const {
	if (!ai_ || channel < 1 || channel > 2) return false;
	AiEntity *e = ai_->at(p_index);
	if (!e) return false;
	const int slot = channel - 1;
	return e->brain.f[AiBrain::kPartAnimRate0 + slot] != 0 ||
	       e->brain.f[AiBrain::kPartAnimPhase0 + slot] != 0;
}

int NovaSimulation::get_entity_body_anim_slot(int p_index) const {
	if (!ai_ || !world_) return -1;
	AiEntity *e = ai_->at(p_index);
	if (!e) return -1;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	return ent ? ent->body_anim_slot : -1;
}

bool NovaSimulation::get_entity_hidden(int p_index) const {
	if (!ai_ || !world_) return false;
	AiEntity *e = ai_->at(p_index);
	if (!e) return false;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	return ent ? ent->hidden : false;
}

PackedFloat32Array NovaSimulation::get_present_snapshot() const {
	const uint64_t start_us = perf_now_us();
	// P7 (ADR 0011 Decision 1): every play path is the in-process listen server — the present pass
	// reads the state the LOCAL CLIENT decoded off the wire (ClientState), not the authoritative sim
	// directly. SP, a LAN host, and the editor preview all render exactly what a networked peer would;
	// a joiner renders remote entities wire-direct. The old no-net AI-pool present is retired. Empty
	// when no runtime is active (a bare sim) — scalar getters (get_entity_*) read the AI pool for tooling.
	PackedFloat32Array out;
	if (runtime_) {
		out = present_snapshot_from_client_view();
	}
	last_present_entity_count_ = static_cast<int>(out.size() / PF_STRIDE);
	last_present_snapshot_us_ = perf_now_us() - start_us;
	return out;
}

void NovaSimulation::enable_listen_server(bool p_enable) {
	listen_server_ = p_enable;
	// P7: the SP listen server now rides the npruntime in-match runtime (ctx_ / host_loop_ /
	// runtime_), stood up per-load in bringup_host_runtime — there is no net ISystem and
	// no legacy loopback seam here. (The LAN host still builds the legacy seam in enable_host_listen
	// until A3; a sim is SP listen XOR LAN host XOR joiner.)
}

void NovaSimulation::set_terrain_til_data(const PackedByteArray &p_til_bytes) {
	terrain_til_data_.assign(p_til_bytes.ptr(), p_til_bytes.ptr() + p_til_bytes.size());
}

bool NovaSimulation::enable_host_listen(int p_port) {
	listen_server_ = true;
	if (pump_.is_null()) pump_.instantiate();
	if (pump_->bind_listen(p_port) != 0) {
		host_listen_ = false;
		return false;
	}
	host_listen_ = true;
	if (world_) {
		world_->projectile_authority = true;
		world_->mp_session = true;
	}
	// P7: the LAN host rides the npruntime runtime (ctx_ over a real UDP socket), stood up per-load in
	// bringup_host_runtime with SocketMode::Lan. NovaUdpPump owns the socket; all protocol/crypto/
	// framing stays in libs (ADR 0010). host_session_config_ keeps the GDScript-facing session options
	// (the Dictionary getter + the §5.1 reactive-reply config fed to configure_session_runtime).
	host_bind_port_ = static_cast<uint16_t>(std::clamp(p_port, 0, 0xFFFF));
	return true;
}

int NovaSimulation::get_host_listen_port() const {
	return (host_listen_ && pump_.is_valid()) ? pump_->local_port() : 0;
}

int NovaSimulation::get_host_peer_count() const {
	// Count the type-1 (remote-joiner) connections in the npruntime table. The host's own type-2
	// loopback is excluded; a pre-Hello garbage datagram registers no node (handle_server_datagram
	// drops bad envelopes), so it stays 0 until a real JointOperations peer handshakes.
	int n = 0;
	for (const opennova::np::NapiNPConnection &c : ctx_.np_protocol.connection_list) {
		if (c.type == 1) ++n;
	}
	return n;
}

void NovaSimulation::configure_host_session(Dictionary p_options) {
	opennova::np::GameConfig config = host_session_config_;
	host_bind_port_ = dictionary_u16(p_options, "bind_port", host_bind_port_);
	apply_dictionary_string(p_options, "server_name", config.server_name);
	apply_dictionary_string(p_options, "mission_name", config.mission_name);
	apply_dictionary_string(p_options, "mission_file", config.mission_file);
	apply_dictionary_string(p_options, "player_name", config.player_name);
	apply_dictionary_string(p_options, "expansion", config.expansion);
	if (p_options.has("gametype")) {
		config.game_type = dictionary_u32(p_options, "gametype", config.game_type);
	} else if (p_options.has("game_type")) {
		config.game_type = dictionary_u32(p_options, "game_type", config.game_type);
	}
	config.mp_attributes = dictionary_u32(p_options, "mpattrib", config.mp_attributes);
	if (p_options.has("fat_bullets"))
		config.fat_bullets = static_cast<bool>(p_options["fat_bullets"]);
	if (p_options.has("one_shot_kill"))
		config.one_shot_kill = static_cast<bool>(p_options["one_shot_kill"]);
	if (p_options.has("spawn_x") || p_options.has("spawn_y") || p_options.has("spawn_z")) {
		config.spawn_x = dictionary_u32(p_options, "spawn_x", config.spawn_x);
		config.spawn_y = dictionary_u32(p_options, "spawn_y", config.spawn_y);
		config.spawn_z = dictionary_u32(p_options, "spawn_z", config.spawn_z);
	}
	if (p_options.has("spawn_names")) {
		config.spawn_names.clear();
		const Variant names_v = p_options.get("spawn_names", Array());
		if (names_v.get_type() == Variant::ARRAY) {
			const Array names = names_v;
			for (int64_t i = 0; i < names.size(); ++i) {
				const String name = names[i];
				if (!name.is_empty()) {
					config.spawn_names.emplace_back(name.utf8().get_data());
				}
			}
		}
	}
	// Server type + player cap (UI host config): serve_and_play gates the host's own-player spawn +
	// loopback fold at bring-up; max_players is the lobby-advertised cap, clamped to the witnessed 1..65.
	if (p_options.has("serve_and_play")) {
		host_serve_and_play_ = static_cast<bool>(p_options["serve_and_play"]);
	}
	if (p_options.has("max_players")) {
		uint32_t mp = dictionary_u32(p_options, "max_players", host_max_players_);
		if (mp < 1u) {
			mp = 1u;
		} else if (mp > 65u) {
			mp = 65u;
		}
		host_max_players_ = mp;
	}
	host_session_config_ = std::move(config);
	if (world_ && host_listen_) {
		world_->fat_bullets = host_session_config_.fat_bullets;
		world_->one_shot_kill = host_session_config_.one_shot_kill;
	}
}

Dictionary NovaSimulation::get_host_session_config() const {
	const opennova::np::GameConfig &session = host_session_config_;
	Dictionary out;
	out["bind_port"] = static_cast<int>(host_bind_port_);
	out["server_name"] = String(session.server_name.c_str());
	out["mission_name"] = String(session.mission_name.c_str());
	out["mission_file"] = String(session.mission_file.c_str());
	out["player_name"] = String(session.player_name.c_str());
	out["expansion"] = String(session.expansion.c_str());
	out["gametype"] = static_cast<int64_t>(session.game_type);
	out["mpattrib"] = static_cast<int64_t>(session.mp_attributes);
	out["fat_bullets"] = session.fat_bullets;
	out["one_shot_kill"] = session.one_shot_kill;
	out["spawn_x"] = static_cast<int64_t>(session.spawn_x);
	out["spawn_y"] = static_cast<int64_t>(session.spawn_y);
	out["spawn_z"] = static_cast<int64_t>(session.spawn_z);
	out["mission_header_size"] = static_cast<int64_t>(session.mission_header_blob.size());
	Array spawn_names;
	for (const std::string &name : session.spawn_names) {
		spawn_names.push_back(String(name.c_str()));
	}
	out["spawn_names"] = spawn_names;
	return out;
}

// ---- co-op LAN joiner (D.2) -------------------------------------------------

bool NovaSimulation::enable_join(const String &p_host_ip, int p_port, const String &p_player_name) {
	// P7: the joiner is a non-authority np::ClientRuntime (Joiner role) built per-load in finish_load;
	// it owns the connect-leg state machine + the S2C->ClientState fold internally. Here we only dial
	// the socket + store the player name (the ClientHello.co the host echoes for the name-match). Leave
	// listen_server_ false (the present gate adds || joiner_); a sim is host XOR joiner.
	if (pump_.is_null()) pump_.instantiate();
	if (pump_->dial(p_host_ip, p_port) != 0) {
		joiner_ = false;
		return false;
	}
	joiner_player_name_ = std::string(p_player_name.utf8().get_data());
	// Build the Joiner runtime now so get_joiner_phase reads Idle before the first load (the contract
	// the legacy joiner_session_ held); finish_load rebuilds it fresh on each (re)load.
	runtime_ = std::make_unique<opennova::np::ClientRuntime>(
			opennova::ClientSession::Config::jointoperations(), joiner_player_name_);
	install_item_class_resolver();
	joiner_ = true;
	if (world_) {
		world_->projectile_authority = false;
		world_->mp_session = true;
	}
	joiner_started_ = false;
	joiner_local_spawned_ = false;
	joiner_self_wire_handle_ = 0;
	return true;
}

bool NovaSimulation::is_joined_in_match() const {
	return joiner_ && runtime_ && runtime_->in_match();
}

int NovaSimulation::get_joiner_phase() const {
	return (joiner_ && runtime_) ? static_cast<int>(runtime_->phase()) : -1;
}

int NovaSimulation::get_joiner_self_handle() const {
	return joiner_ ? static_cast<int>(joiner_self_wire_handle_) : 0;
}

void NovaSimulation::ship_to_host(const std::vector<uint8_t> &dg) {
	if (pump_.is_null() || dg.empty()) return;
	PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(dg.size()));
	std::memcpy(bytes.ptrw(), dg.data(), dg.size());
	pump_->send_to_host(bytes);
}

opennova::world::PlayerSpawn NovaSimulation::spawn_from_self(
		const opennova::np::JoinerConnection::SelfSpawn &s) const {
	opennova::world::PlayerSpawn spawn;
	// SelfSpawn position is mission i32 16.16; PlayerSpawn.position is float mission units.
	spawn.position = {static_cast<float>(s.pos_x / kFixed16),
	                  static_cast<float>(s.pos_y / kFixed16),
	                  static_cast<float>(s.pos_z / kFixed16)};
	// orientation is ALREADY a full 32-bit BAM (unlike HostJoinerPose.heading, an i16
	// the host shifts << 16) -> pass it straight to the (90 - heading) mission-degree map.
	spawn.yaw = static_cast<int16_t>(
			std::lround(opennova::world::mission_yaw_deg_from_bam_heading(s.orientation)));
	spawn.team = s.team;
	spawn.net_id = s.net_id; // the host-assigned joiner SSN (NOT the host's own 0xFFF0)
	return spawn;
}

// (P7 A4: joiner_net_poll / joiner_net_flush deleted — the joiner now runs through joiner_pump
//  over an np::ClientRuntime; the legacy JoinerSession path is retired here.)

bool NovaSimulation::admit_test_remote_peer(Vector3 p_position, float p_yaw_deg, int p_team) {
	if (!host_listen_ || !world_ || !world_->ai) return false;
	opennova::world::PlayerSpawn spawn;
	// Godot (x,y,z) -> mission (x,-z,y), the inverse of the present remap (same as spawn_local_player).
	spawn.position = {static_cast<float>(p_position.x), static_cast<float>(-p_position.z),
	                  static_cast<float>(p_position.y)};
	spawn.yaw = static_cast<int16_t>(p_yaw_deg);
	spawn.team = static_cast<uint8_t>(p_team);
	// A synthetic loopback peer; distinct port per call so repeated admits don't alias. Own a transport
	// so the connection is well-formed. The synthetic admit (no handshake) mirrors the post-PeerSpawned
	// state; with the host already at net_id 0xFFF0 the joiner allocates 0xFFF1.
	const opennova::PeerAddr peer{0x0100007Fu, static_cast<uint16_t>(40000 + host_owner_.peers.size())};
	opennova::np::PeerLink &link = host_owner_.peers[peer];
	if (!link.transport) {
		link.transport = std::make_unique<opennova::netsim::UdpSessionTransport>(
				opennova::netsim::UdpSessionTransport::Role::Host);
	}
	const opennova::world::EntityHandle h =
			opennova::np::admit_synthetic_peer(
					ctx_, *world_, peer, spawn, link.transport.get());
	resolve_new_infantry_adm_ids();
	return h.valid();
}

PackedFloat32Array NovaSimulation::present_snapshot_from_client_view() const {
	PackedFloat32Array out;
	if (!world_ || !runtime_) return out;
	// P7: every path (SP / LAN host / joiner) reads its own npruntime ClientRuntime view's ClientState.
	const opennova::netsim::ClientState &cs = runtime_->state();
	const opennova::world::Entity *local_player =
			world_->registry.get(world_->cached.local_player);
	// Entity_RenderVehicleModel's local UseGun predicate is a render verdict,
	// not Entity.hidden: parent equality + first-person camera + raw UseGun seat.
	// The per-row tail below adds the live EquippedSlot/Def tests.
	// [orig: @0x4407f6..0x44084c; sole submit @0x440918]
	const bool local_first_person_usegun =
			!player_view_.third_person && local_player != nullptr &&
			local_player->mounted &&
			local_player->mount_type == opennova::world::SeatType::Gunner;
	const int count = static_cast<int>(cs.entities.size());
	out.resize(static_cast<int64_t>(count) * PF_STRIDE);
	float *w = out.ptrw();
	for (int i = 0; i < count; ++i) {
		float *r = w + static_cast<int64_t>(i) * PF_STRIDE;
		const opennova::netsim::ClientEntityState &es = cs.entities[i];
		r[PF_KIND] = -1.0f; r[PF_INDEX] = -1.0f; r[PF_BMS_ID] = 0.0f; r[PF_NET_ID] = 0.0f;
		r[PF_POS_X] = 0.0f; r[PF_POS_Y] = 0.0f; r[PF_POS_Z] = 0.0f;
		r[PF_PITCH_DEG] = 0.0f; r[PF_YAW_DEG] = 0.0f; r[PF_ROLL_DEG] = 0.0f;
		r[PF_PHASE1] = 0.0f; r[PF_ACTIVE1] = 0.0f; r[PF_PHASE2] = 0.0f; r[PF_ACTIVE2] = 0.0f;
		r[PF_BODY_ANIM_SLOT] = -1.0f; r[PF_ANIM_STATE] = -1.0f; r[PF_ANIM_PHASE_TICKS] = -1.0f;
		r[PF_ANIM_REMOTE_REQUEST] = 0.0f;
		r[PF_HIDDEN] = 0.0f; r[PF_LOCAL_VIEW_SUPPRESSED] = 0.0f;
		r[PF_ALIVE] = 1.0f; r[PF_RESPAWN_REVISION] = 0.0f;
		r[PF_TYPE_ID] = 0.0f; r[PF_WIRE_HANDLE] = 0.0f;
		for (int field = PF_AIM_OVERLAY_VALID; field < PF_STRIDE; ++field)
			r[field] = 0.0f;

		// Self-filter (joiner): the host SNAPs our own entity (wire handle H) and streams
		// it back in 0x0A; we draw our local player L via LocalPlayerHost, so drop the wire
		// echo here. The row stays at its zero/unresolved defaults (PF_TYPE_ID 0), which the
		// wire render pass skips. [net-re §5.38b two-handle L-vs-H reconciliation]
		if (joiner_ && joiner_self_wire_handle_ != 0 && es.handle == joiner_self_wire_handle_) {
			continue;
		}
		// Wire identity for the render pass. The joiner has no authoritative registry for
		// the host's entities, so it renders from the wire type id + handle, not a node.
		r[PF_TYPE_ID] = static_cast<float>(es.type_id);
		r[PF_WIRE_HANDLE] = static_cast<float>(es.handle);

		// On the HOST listen server, kind/index/bms_id/net_id resolve from the registry
		// entity behind the decoded handle (host == authoritative client, so the placed-node
		// mapping still resolves through MissionEntityRegistry exactly as the AI-pool path
		// does). A joiner's decoded handles live in the HOST's handle space and would resolve
		// to the wrong local entity, so the joiner skips this and renders wire-direct (b2).
		const opennova::world::EntityHandle h{es.handle};
		const opennova::world::Entity *ent = (!joiner_) ? world_->registry.get(h) : nullptr;
		if (ent) {
			r[PF_KIND] = static_cast<float>(ent->spawn_origin >> 24);
			r[PF_INDEX] = static_cast<float>(ent->spawn_origin & 0xFFFFFF);
			r[PF_BMS_ID] = static_cast<float>(ent->bms_id);
			r[PF_NET_ID] = static_cast<float>(ent->net_id);
			r[PF_BODY_ANIM_SLOT] = static_cast<float>(ent->body_anim_slot);
			r[PF_PITCH_DEG] = static_cast<float>(ent->pitch);
			r[PF_ROLL_DEG] = static_cast<float>(ent->roll);
			r[PF_HIDDEN] = ent->hidden ? 1.0f : 0.0f;
			r[PF_ALIVE] = ent->alive ? 1.0f : 0.0f;
			r[PF_RIGHT_HAND_COLLAPSED] =
					mount_collapses_right_hand_row(*ent) ? 1.0f : 0.0f;
			if (local_first_person_usegun &&
					local_player->mount_target == h) {
				const opennova::world::WeaponTableEntry *mount_def =
						world_->weapons.by_index(ent->primary_weapon_slot_adm);
				// Primary retail leg: FP model exists and this exact embedded
				// MountSlot is the live EquippedSlot. flags2 Invisible is the
				// witnessed alternate forced-cull leg and does not require the
				// EquippedSlot comparison.
				// [orig: Def+0x16c @0x440824; EquippedSlot @0x440833;
				//  Def+0x0c & 0x800 @0x44083f]
				const bool equipped_parent_slot =
						local_usegun_slot_active_ && local_usegun_mount_ == h &&
						local_usegun_weapon_adm_ == ent->primary_weapon_slot_adm;
				if (mount_def != nullptr &&
						((mount_def->has_first_person_model_reference &&
						  local_first_person_model_adm_ ==
								  ent->primary_weapon_slot_adm &&
						  equipped_parent_slot) ||
						 (mount_def->flags2 &
						  opennova::world::weapon_flag2::kInvisible) != 0))
					r[PF_LOCAL_VIEW_SUPPRESSED] = 1.0f;
			}
		}
		// A joiner cannot resolve host wire handles through its local registry.
		// Organic lifecycle therefore comes straight from the raw compact byte:
		// bit 0 hides, bit 1 is dead/undeployed. The revision survives multiple
		// decoded frames between render passes and gives presentation a stable
		// signal to reset one-shot/body-channel state on respawn.
		// Both roles fold compact lifecycle records. Preserve the epoch on the
		// host too: WirePresentPass renders admitted remote players that have no
		// placed mission node.
		r[PF_RESPAWN_REVISION] = static_cast<float>(es.respawn_revision);
		if (joiner_) {
			if (es.state_flags_known) {
				r[PF_HIDDEN] = (es.state_flags & 0x01u) != 0u ? 1.0f : 0.0f;
				r[PF_ALIVE] = (es.state_flags & 0x02u) == 0u ? 1.0f : 0.0f;
			}
		}
		EmplacedWeaponControls emplaced;
		if (ent != nullptr) {
			if (emplaced_weapon_controls_for(
						*world_, ai_.get(), *ent, emplaced))
				write_present_emplaced_controls(r, emplaced);
		} else if (joiner_ &&
				emplaced_weapon_controls_for_client(
						es, cs, item_seat_specs_, emplaced)) {
			write_present_emplaced_controls(r, emplaced);
		}
		const bool authoritative_attachment_pose =
				ent != nullptr && ent->emplacement_parent.valid() &&
				ent->emplacement_parent.packed == es.parent_handle;
		opennova::world::MountedPose client_attachment_pose;
		const uint32_t attachment_time_ms = panm_time_override_ms_ >= 0
				? static_cast<uint32_t>(panm_time_override_ms_)
				: world_->logic_tick * 16u;
		const bool reconstructed_client_attachment_pose = joiner_ &&
				resolve_client_eweap_attachment_pose(
						es, cs, item_seat_specs_, mounted_pose_data_by_type_,
						attachment_time_ms, client_attachment_pose);
		if (authoritative_attachment_pose) {
			// NoNetworkCallback addeweap children have only their 0x0D spawn pose in
			// ClientState. The host has already advanced their authoritative userpoint
			// pose through World::update_emplacement_attachments; use that exact result
			// rather than flattening live PANM back to the client's rigid spawn offset.
			r[PF_POS_X] = ent->position.x;
			r[PF_POS_Y] = ent->position.z;
			r[PF_POS_Z] = -ent->position.y;
			r[PF_PITCH_DEG] = static_cast<float>(ent->pitch);
			r[PF_YAW_DEG] = static_cast<float>(ent->yaw);
			r[PF_ROLL_DEG] = static_cast<float>(ent->roll);
		} else if (reconstructed_client_attachment_pose) {
			r[PF_POS_X] = client_attachment_pose.position.x;
			r[PF_POS_Y] = client_attachment_pose.position.z;
			r[PF_POS_Z] = -client_attachment_pose.position.y;
			r[PF_PITCH_DEG] = static_cast<float>(client_attachment_pose.pitch);
			r[PF_YAW_DEG] = static_cast<float>(client_attachment_pose.yaw);
			r[PF_ROLL_DEG] = static_cast<float>(client_attachment_pose.roll);
		} else {
			// Decoded wire position is mission (x,y,z) 16.16 -> Godot (x, z, -y)
			// world units, the SAME remap the AI-pool path uses. Position is
			// post-compression (lossy), exactly what retail renders for decoded peers.
			r[PF_POS_X] = static_cast<float>(es.x / kFixed16);
			r[PF_POS_Y] = static_cast<float>(es.z / kFixed16);
			r[PF_POS_Z] = static_cast<float>(-es.y / kFixed16);
			// Rebuild the 32-bit engine BAM from the compact high byte, then convert
			// engine -> mission yaw (90 - heading), matching the AI-pool present.
			const int32_t heading_bam = static_cast<int32_t>(
					static_cast<uint32_t>(es.yaw_byte) << 24);
			r[PF_YAW_DEG] = static_cast<float>(
					opennova::world::mission_yaw_deg_from_bam_heading(heading_bam));
		}
		// Infantry anim from the local AI pool (host only — same registry caveat as above).
		if (world_->ai && !joiner_) {
			const AiEntity *ae = world_->ai->for_handle(h);
			if (ae != nullptr) {
				for (int slot = 0; slot < 2; ++slot) {
					const int phase = std::clamp(
							ae->brain.f[AiBrain::kPartAnimPhase0 + slot],
							0, 65535);
					const bool active =
							ae->brain.f[AiBrain::kPartAnimRate0 + slot] != 0 ||
							phase != 0;
					r[PF_PHASE1 + slot * 2] = static_cast<float>(phase);
					r[PF_ACTIVE1 + slot * 2] = active ? 1.0f : 0.0f;
				}
			}
			if (ae && ae->inf.active) {
				r[PF_ANIM_STATE] = static_cast<float>(ae->inf.anim_state);
				r[PF_ANIM_PHASE_TICKS] = static_cast<float>(ae->inf.clip_phase);
				if (ent != nullptr) {
					const opennova::anim::AimOverlayInputs inputs =
							aim_overlay_inputs_for(*ae, *ent);
					opennova::anim::AimOverlayAngles
							angles[opennova::anim::kOverlayClassCount];
					opennova::anim::compute_aim_overlay_angles(inputs, angles);
					write_present_overlay(r, angles);
				}
			}
		}
		if (joiner_) {
			opennova::anim::AimOverlayInputs inputs;
			bool collapse_right_hand = false;
			if (aim_overlay_inputs_for_client(
						es, cs, item_seat_specs_, inputs,
						&collapse_right_hand)) {
				r[PF_ANIM_STATE] =
						static_cast<float>(es.anim_state_id);
				r[PF_ANIM_REMOTE_REQUEST] = 1.0f;
				// The player compact's byte 15 is the authority's elapsed
				// half-frame ticks in the current body loop. Retail applies it
				// to remote players as the anim-channel phase seed. Infantry
				// compacts carry only the state byte, so their -1 sentinel tells
				// presentation to advance the selected clip locally.
				// [orig: player write @0x4c0cf2; remote apply @0x4c11a6;
				//  AnimMap_UpdateEntity consumes entity+0x377 @0x40b74b]
				if (es.cls == opennova::EntityClass::Player) {
					r[PF_ANIM_PHASE_TICKS] =
							static_cast<float>(es.anim_channel_ratio);
				}
				r[PF_RIGHT_HAND_COLLAPSED] =
						collapse_right_hand ? 1.0f : 0.0f;
				opennova::anim::AimOverlayAngles
						angles[opennova::anim::kOverlayClassCount];
				opennova::anim::compute_aim_overlay_angles(inputs, angles);
				write_present_overlay(r, angles);
			}
		}
	}
	return out;
}

void NovaSimulation::set_loco_scale(int p_scale) {
	if (ai_) ai_->loco_scale = p_scale;
}

int NovaSimulation::get_loco_scale() const {
	return ai_ ? ai_->loco_scale : 0;
}
