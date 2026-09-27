// The packed present-row collectors (see present_rows.h).
#include <runtime/inmatch/present_rows.h>

#include <runtime/inmatch/client_replica_present.h> // the emplaced/overlay/held-weapon writers
#include <runtime/inmatch/client_replica_emplaced.h> // the joiner's carrier gun words
#include <runtime/inmatch/client_replica_present_projection.h> // the canonical decoded-client projection (ADR 0031)
#include <runtime/inmatch/replica_query.h> // client_entity_for_handle
#include <runtime/inmatch/napi_np_server_ctx.h> // the listen host's player slots (EquippedSlot)
#include <runtime/world/mounted_pose.h> // the ONE mounted matrix path (S4b)
#include <runtime/world/pose_inputs.h> // seat/mount pose predicates + aim inputs (ADR 0028)
#include <runtime/mission/seat_spec_extract.h> // item_seat_spec_for_type
#include <runtime/world/mount_controls.h> // heat-glow + emplaced turret CTRL sources (ADR 0028)
#include <runtime/world/present_rows.h>
#include <runtime/world/vehicle_motor.h> // vehicle_ctrl_registers
#include <runtime/world/infantry.h> // infantry_weapon_channel_visible
#include <runtime/world/player_weapon.h> // weapon_flag::kNoClipsNoDraw
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/entity.h> // kEntityFlag* (the wire state_flags byte IS entity+36 low)
#include <runtime/world/zone_chain.h> // zone_chain_zone_info_byte
#include <runtime/world/collision.h> // entity_live_euler_bam, ResolvedCollisionShape
#include <runtime/renderer/render_slot_shadow.h> // slot_march_start_offset
#include <runtime/replication/client_state.h>
#include <runtime/replication/entity_wire_bridge.h> // entity_class_of / player_wire_net_id (the host's own rows)
#include <runtime/anim/aim_overlay.h> // the torso-bend overlay blends [orig: @0x4b1290]
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <base/io/fixed.h>
#include <base/io/strutil.h> // iequals

#include <cmath>
#include <cstring>

using namespace opennova::threedi;

namespace opennova::inmatch {

using namespace opennova::world;

namespace {

inline constexpr double kFixed16 = io::kFp16OneD;

// An authoritative pool row's collector verdict: retail's terrain collector
// never draws an entity whose Flags carry bit 0 — the carried object (the
// carry attach sets it), an undeployed or spectating player, a blocked spawn
// marker, an SSN hide — so the host row is hidden exactly as the decoded
// joiner row is.
// [orig: Terrain_CollectVisibleEntitiesForTerrain @0x5c8cef..0x5c8cf4;
//  Entity_AttachCarriedObject @0x43c14a]
bool pool_row_hidden(const Entity &e) {
	return e.hidden || ((e.flags | e.engine_flags) & kEntityFlagCarried) != 0;
}

// One authoritative person's item-overlay inputs: its Flags, the canopy
// words, the pose triple and body heading the bone build reads, and the
// carried child (entity+0x268) when it has an item def.
// [orig: BoneCallback_org0_World @0x4e3940 — the child gate @0x4e3da6..0x4e3db7]
PersonOverlayInputs pool_person_overlay_inputs(const World &w, const Entity &e,
		const AiEntity &ae, const anim::AimOverlayInputs &pose) {
	PersonOverlayInputs in;
	in.flags = e.flags | e.engine_flags;
	in.pose = pose;
	in.chute = ae.inf.parachute;
	if (const Entity *child = w.registry.get(e.mounted_child);
			child != nullptr && child->has_item_def)
		in.carried_type_id = child->item_id;
	return in;
}

// The ammo leg of the held-weapon draw gate's REMOTE branch, on the authority:
// when the player entity's EquippedSlot (the host's record of it, which the
// weapon echo never moves) holds a NoClipsNoDraw def, the model hides once that
// def's ammo class pool plus — for a one-round clip — its loaded rounds (the
// shared bucket when the def names one, else the slot's own clip) reads zero.
// The debug no-ammo-cost bit answers every class with its carry cap.
// [orig: Entity_CanFireWeapon @0x4dcb5f..0x4dcbc6 — EquippedSlot @0x4dcb30;
//  Entity_GetScoreValueBySlotType @0x5406E0 (the cap @0x5406fc, the validated
//  player's pools @0x5407b3); sub_5405F0 @0x5405F0 (its loaded buckets @0x540657)]
bool remote_held_weapon_out_of_ammo(const World &w, const NapiNPServerCtx &server,
		const Entity &e) {
	for (const NapiNPConnection &conn : server.np_protocol.connection_list) {
		if (conn.link.owned_entity.packed != e.handle.packed) continue;
		const WeaponTableEntry *def = nullptr;
		int16_t slot_clip = 0;
		switch (conn.equipped_slot.kind) {
			case NapiNPConnection::EquippedSlotRef::kNone:
				return false;
			case NapiNPConnection::EquippedSlotRef::kPersonal: {
				const auto slot = conn.weapon_slots.find(conn.equipped_slot.combo);
				if (slot == conn.weapon_slots.end()) return false;
				def = w.tables.weapons.by_index(slot->second.adm_index);
				slot_clip = slot->second.clip;
				break;
			}
			case NapiNPConnection::EquippedSlotRef::kMount: {
				const Entity *mount = w.registry.get(conn.equipped_slot.mount);
				if (mount == nullptr) return false;
				def = w.tables.weapons.by_index(mount->primary_weapon_slot_adm);
				slot_clip = static_cast<int16_t>(mount->primary_weapon_slot.clip);
				break;
			}
		}
		if (def == nullptr || (def->flags & weapon_flag::kNoClipsNoDraw) == 0) return false;
		const int cls = def->ammo_class_id;
		const auto pool = [&](int id) -> int64_t {
			if (id < 0 || id >= 128) return 0;
			if (w.rules.ignore_weapon_ammo_cost)
				return id < static_cast<int>(w.tables.weapons.ammo_class_caps.size())
						? w.tables.weapons.ammo_class_caps[static_cast<size_t>(id)]
						: 0;
			return conn.reply.ammo_pools[static_cast<size_t>(id)];
		};
		int64_t ammo = pool(cls);
		if (def->clipsize == 1) {
			const int32_t bucket = def->ammo_bucket;
			ammo += bucket != 0
					? (bucket > 0 && bucket < 128 ? conn.reply.shared_clips[static_cast<size_t>(bucket)] : 0)
					: slot_clip;
		}
		return ammo == 0;
	}
	return false;
}

// One authoritative body's third-person gun: draw 5 draws the ADM model its
// entity+0x2B0 names once Entity_CanFireWeapon passes [orig:
// BoneCallback_org0_World precondition @0x4e3c97, gate call @0x4e3ca5]. Both
// authority collectors publish it through here. A pure client never holds an
// NPC's byte: the rider copy below needs the combat target only the
// authority's think writes [orig: Entity_UpdateInfantryAI @0x4ba97e..0x4ba985],
// and the org serializer never streams +0x2B0 [orig:
// NetPacket_SerializeInfantryEntityState @0x4c0320], so the decoded projection
// publishes player rows only.
void write_pool_held_weapon(float *r, const PresentRowsContext &context,
		const Entity *local_player, const Entity &e, const AiEntity &ae,
		const anim::AimOverlayInputs &inputs) {
	if ((e.engine_flags & kEntityFlagPlayer) != 0) {
		// A remote player's gun also answers the gate's ammo leg on the host.
		const bool out_of_ammo = context.server != nullptr && &e != local_player &&
				remote_held_weapon_out_of_ammo(context.kernel.world, *context.server, e);
		write_present_held_weapon(
				r, out_of_ammo ? 0 : e.equipped_adm_index, (e.flags & kEntityFlagDead) != 0,
				inputs, ae.inf.wpn_state);
		return;
	}
	// An org1 body's byte is 0 or 0xFF on foot: the org init never writes it
	// and the fire stamps clear it in the same pass [orig: Entity_InitOrganicAI
	// @0x4bfcc0; stamp and clear @0x4bf347..0x4bf369]. A mounted rider's
	// survives the frame: the mounted fire-request window copies the parent's
	// byte [orig: Entity_UpdateInfantryAI @0x4bf4f4..0x4bf4fa] and only a
	// detach clears it [orig: Entity_DetachFromVehicle @0x43569c]. A seat-1
	// rider's EquippedSlot is null, so only the gate's dead and seat legs apply
	// [orig: Entity_CanFireWeapon @0x4dcb1e, @0x4dcb3c..0x4dcb57,
	//  @0x4dcb5d..0x4dcb5f].
	write_present_held_weapon(r, e.equipped_adm_index,
			((e.flags | e.engine_flags) & kEntityFlagDead) != 0, inputs, ae.inf.wpn_state);
}

// The EWEAP articulation registers a mounted gun's .3di CTRL table names.
inline constexpr char kEmplacedGunYawRegister[] = "EWEAP_GUNYAW";
inline constexpr char kEmplacedGunPitchRegister[] = "EWEAP_GUNPITCH";

// The row's door count plus, for a row that publishes any, one
// (row index, count, phases...) entry on the side table.
inline void write_present_doors(float *row, int row_index, const World &world,
		const Entity &entity, DoorPhaseTable &door_phases) {
	int32_t phases[DoorSystem::kMaxDoors] = {};
	const int count = world.doors.write_phases(entity, phases, DoorSystem::kMaxDoors);
	row[PF_DOOR_COUNT] = static_cast<float>(count);
	if (count <= 0) return;
	door_phases.push_back(row_index);
	door_phases.push_back(count);
	door_phases.insert(door_phases.end(), phases, phases + count);
}

inline void write_present_section_mask(float *row, uint32_t hidden_mask) {
	row[PF_SECTION_MASK_VALID] = 1.0f;
	row[PF_SECTION_MASK_LO] = static_cast<float>(hidden_mask & 0xFFFFu);
	row[PF_SECTION_MASK_HI] = static_cast<float>((hidden_mask >> 16) & 0xFFFFu);
}

inline void write_present_world_model_heat_glow(float *record, const World &world,
		const Entity &entity) {
	int32_t heat_glow = 0;
	if (!world_model_heat_glow_for(world, entity, heat_glow)) return;
	record[PF_WORLD_HEAT_GLOW_VALID] = 1.0f;
	record[PF_WORLD_HEAT_GLOW] = static_cast<float>(heat_glow);
}

inline void write_present_vehicle_motion_controls(float *record, const World &world,
		const Entity &entity) {
	// The row's motor owns these controls — the authority's, or on a joiner the
	// client mover that predicts its twin. Its render callback selects which
	// channels may be published.
	const FocalSwayPose sway = world.rotor_wash.sway_pose(entity);
	record[PF_FOCAL_SWAY_VALID] = sway.active ? 1.0f : 0.0f;
	if (sway.active) {
		for (int k = 0; k < 9; ++k)
			record[PF_FOCAL_SWAY_BASIS_0 + k] = sway.basis[k];
		record[PF_FOCAL_SWAY_X] = sway.offset.x;
		record[PF_FOCAL_SWAY_Y] = sway.offset.z;
		record[PF_FOCAL_SWAY_Z] = -sway.offset.y;
	}
	const VehicleTraits *traits = world.vehicles.traits.get(entity.item_id);
	if (entity.handle.pool() != 1 || traits == nullptr)
		return;
	const VehicleCtrlRegisters controls = vehicle_ctrl_registers(
			entity.veh, traits->render_family, world.ai.for_handle(entity.handle));
	record[PF_VEHICLE_MOTION_VALID] = 1.0f;
	record[PF_VEHICLE_CTRL_MASK] = static_cast<float>(controls.mask);
	record[PF_VEHICLE_TRACK_LEFT] = static_cast<float>(controls.tracks[0]);
	record[PF_VEHICLE_TRACK_RIGHT] = static_cast<float>(controls.tracks[1]);
	record[PF_VEHICLE_GUN_YAW] = static_cast<float>(controls.gun_yaw);
	record[PF_VEHICLE_GUN_PITCH] = static_cast<float>(controls.gun_pitch);
	record[PF_VEHICLE_STEERING] = static_cast<float>(controls.steering);
	record[PF_VEHICLE_SPEED] = static_cast<float>(controls.speed);
	// The part-animation words ride the same valid bit: the rotor angle
	// accumulator's high word (HELO_ROTOR / HELO_TAILROTOR) and the wheel
	// phase's (VEHICLE_WHEELS), published by the same cveh callback
	// [orig: Entity_CacheVehicleHUDStats @0x4929B0 — the +0x466 read
	// @0x492ACA..0x492ADE, the +0x2BA read @0x4929B4; the accumulators are
	// world/vehicle_part_anim.h's].
	record[PF_VEHICLE_ROTOR] = static_cast<float>(controls.rotor);
	record[PF_VEHICLE_TAIL_ROTOR] = static_cast<float>(controls.tail_rotor);
	record[PF_VEHICLE_GEAR] = static_cast<float>(controls.gear);
	record[PF_VEHICLE_WHEELS] = static_cast<float>(controls.wheels);
	for (size_t i = 0; i < controls.tires.size(); ++i)
		record[PF_VEHICLE_TIRE00 + i] = static_cast<float>(controls.tires[i]);
}

// S4b (ADR 0028): the joiner's addeweap reconstruction rides the SAME engine
// resolver the host authority runs (world::resolve_model_mounted_pose) —
// one mounted matrix path. The model resolves through the shared asset store by the
// installed spec's graphic key, exactly like the host-side resolver.
bool resolve_client_eweap_attachment_pose(
		const replication::ClientEntityState &child,
		const replication::ClientState &state,
		const std::vector<mission::ItemSeatSpec> &specs,
		const std::unordered_map<int32_t, std::string> &graphics_by_type,
		const assets::AssetStore &models,
		uint32_t time_ms, MountedPose &out) {
	// The carrier is the child's followed one (its 0x0D target), never the
	// occupant back-reference the parent field carries.
	const uint16_t carrier_handle = replication::persistent_carrier_handle(child);
	if (carrier_handle == EntityHandle::kInvalid) return false;
	const replication::ClientEntityState *parent =
			client_entity_for_handle(state, carrier_handle);
	if (parent == nullptr) return false;
	const mission::ItemSeatSpec *parent_spec =
			mission::item_seat_spec_for_type(specs, parent->type_id);
	if (parent_spec == nullptr) return false;

	// The 0x0D relation names only the parent, not the authored attachment slot.
	// Reconstruct only when the child type selects exactly one authored row and
	// that row resolves a userpoint; duplicate same-type rows are intentionally
	// left on the rigid fallback.
	const mission::ItemEmplacementAttachmentSpec *attachment = nullptr;
	for (const mission::ItemEmplacementAttachmentSpec &candidate :
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
	if (graphic_found == graphics_by_type.end() || !models.has_source())
		return false;
	const Threedi3di3 *model_ptr = models.model(graphic_found->second).get();
	if (model_ptr == nullptr) return false;

	// Remote generic PLAYPARTANIM phases are not in ClientEntityState. Do not
	// synthesize them from timing or repurpose a wire field. EWEAP is the one safe
	// articulated family: the joiner tick retains both semantic controls after
	// the shared slew, gunner tether and authored window clamp.
	EmplacedWeaponControls emplaced;
	if (!emplaced_weapon_controls_for_client(*parent, emplaced))
		return false;
	const Threedi3di3 &model = *model_ptr;
	if (model.ctrl.count > 0 && model.ctrl.registers == nullptr)
		return false;
	bool has_eweap_control = false;
	for (uint32_t slot = 0; slot < model.ctrl.count; ++slot) {
		const char *name = model.ctrl.registers[slot].name;
		if (strutil::iequals(name, kEmplacedGunYawRegister) ||
				strutil::iequals(name, kEmplacedGunPitchRegister)) {
			has_eweap_control = true;
			break;
		}
	}
	if (!has_eweap_control) return false;
	int32_t ctrl_values[THREEDI_CTRL_REGISTER_COUNT] = {};
	ctrl_values[THREEDI_CTRL_EWEAP_GUNYAW] = static_cast<int32_t>(emplaced.gun_yaw);
	ctrl_values[THREEDI_CTRL_EWEAP_GUNPITCH] = static_cast<int32_t>(emplaced.gun_pitch);
	ctrl_values[THREEDI_CTRL_WEAP_SPIN] = emplaced.spin;

	Entity carrier;
	carrier.item_id = static_cast<int32_t>(parent->type_id);
	carrier.position = {
			static_cast<float>(parent->x / kFixed16),
			static_cast<float>(parent->y / kFixed16),
			static_cast<float>(parent->z / kFixed16)};
	carrier.veh.yaw_seeded = true;
	carrier.veh.yaw_bam = parent->heading_bam;
	carrier.veh.air_pitch_bam = parent->pitch_bam;
	carrier.veh.air_roll_bam = parent->roll_bam;
	return world::resolve_model_mounted_pose(
			model, carrier, attachment->anchor, ctrl_values, time_ms, out);
}

// Entity_RenderVehicleModel's local UseGun predicate is a render verdict,
// not Entity.hidden: parent equality + first-person camera + raw UseGun seat.
// The per-row tail adds the live EquippedSlot/Def tests.
// [orig: Entity_RenderVehicleModel @0x4407f6..0x44084c, sole submit @0x440918;
//  see docs/world/world-wac-ai-re.md]
bool local_first_person_usegun(const mission::MissionKernel &kernel, const Entity *local_player) {
	return !kernel.local.view.third_person && local_player != nullptr &&
			local_player->mounted && local_player->mount_type == SeatType::Gunner;
}

// The local first-person UseGun parent cull for the mount row `h` (the
// carrier's embedded MountSlot `mount_slot_adm`): a render verdict of THIS
// machine's own mount state that retail's render walk applies identically on
// every role. [orig: Entity_RenderVehicleModel @0x4407d0 predicate
//  @0x4407f6..0x44084c; sole submit @0x440918]
bool local_view_suppresses_mount(const mission::MissionKernel &kernel,
		EntityHandle h, int32_t mount_slot_adm) {
	const WeaponTableEntry *mount_def = kernel.world.tables.weapons.by_index(mount_slot_adm);
	// Primary retail leg: FP model exists and this exact embedded
	// MountSlot is the live EquippedSlot. flags2 Invisible is the
	// witnessed alternate forced-cull leg and does not require the
	// EquippedSlot comparison.
	// [orig: Def+0x16c @0x440824; EquippedSlot @0x440833;
	//  Def+0x0c & 0x800 @0x44083f]
	const bool equipped_parent_slot =
			kernel.local.weapon.usegun_slot_active && kernel.local.weapon.usegun_mount == h &&
			kernel.local.weapon.usegun_weapon_adm == mount_slot_adm;
	return mount_def != nullptr &&
			((mount_def->has_first_person_model_reference &&
			  kernel.local.weapon.first_person_model_adm == mount_slot_adm &&
			  equipped_parent_slot) ||
			 (mount_def->flags2 & weapon_flag2::kInvisible) != 0);
}

} // namespace

double pool_present_yaw_deg(const Entity &e, const AiEntity *ae, EntityClass cls) {
	if (e.emplacement_parent.valid() && e.emplacement_pose_metadata_resolved)
		return e.veh.yaw_seeded ? mission_yaw_deg_from_bam_heading(e.veh.yaw_bam)
				: static_cast<double>(e.yaw);
	if (cls == EntityClass::Vehicle && e.veh.yaw_seeded)
		return mission_yaw_deg_from_bam_heading(e.veh.yaw_bam);
	if (cls == EntityClass::Infantry && ae != nullptr)
		return mission_yaw_deg_from_bam_heading(ae->heading);
	return static_cast<double>(e.yaw);
}

// [orig: the model matrix is built over the entity's 32-bit euler triple --
//  Math_BuildFixedPointMatrixFromEulerAngles @0x613F40 over entity+0x10..0x18,
//  e.g. HUD_BuildEntityInfo @0x4B84C9]
double pool_present_pitch_deg(const Entity &e) {
	return e.veh.yaw_seeded ? static_cast<double>(e.veh.air_pitch_bam) * kDegreesPerBam
							: static_cast<double>(e.pitch);
}

double pool_present_roll_deg(const Entity &e) {
	return e.veh.yaw_seeded ? static_cast<double>(e.veh.air_roll_bam) * kDegreesPerBam
							: static_cast<double>(e.roll);
}

// One pool row from the authoritative record: the body every host/SP world
// row and the joiner's appended fragment rows share (defined below the two
// collectors).
static void write_world_present_row(const PresentRowsContext &context,
		const Entity *local_player, bool first_person_usegun, const Entity &e,
		int row_index, PoolPresentLifecycleMap &lifecycle, float *r,
		DoorPhaseTable &door_phases);

// The render-slot march start one row publishes (world::PF_SLOT_MARCH_OFFSET_X)
// [orig: RenderSlot_UpdateEntityLight @0x5d6ce7..0x5d6d31].
static void write_present_slot_march(float *r, uint32_t flags_dword, int item_type,
		const int32_t euler_bam[3], const std::array<int32_t, 3> &bbox_center_q16) {
	const std::array<float, 3> offset = renderer::slot_march_start_offset(
			renderer::slot_entity_flags_zero(flags_dword, 0u, item_type),
			euler_bam[0], euler_bam[1], euler_bam[2], bbox_center_q16);
	r[PF_SLOT_MARCH_OFFSET_X] = offset[0];
	r[PF_SLOT_MARCH_OFFSET_Y] = offset[1];
	r[PF_SLOT_MARCH_OFFSET_Z] = offset[2];
}

// From the authoritative entity: its split Flags words, live Euler triple and
// stamped collision-bbox centre (entity+0x1FC).
static void write_present_slot_march(float *r, const Entity &e) {
	int32_t euler[3];
	entity_live_euler_bam(e, euler);
	write_present_slot_march(r, e.flags | e.engine_flags, e.item_type, euler,
			{static_cast<int32_t>(std::lround(e.bbox_center.x * 65536.0)),
					static_cast<int32_t>(std::lround(e.bbox_center.y * 65536.0)),
					static_cast<int32_t>(std::lround(e.bbox_center.z * 65536.0))});
}

// From a joiner's decoded row, the facts its own retail entity holds: its
// Flags dword (replica_entity_flags_dword), the decoded Euler triple, and the
// type's resolved collision-bbox centre (Entity_InitFromModel
// @0x40df1e..0x40df4a; the shape the joiner's sun and LOS legs read).
static void write_present_replica_slot_march(float *r, mission::MissionKernel &kernel,
		const replication::ClientEntityState &es) {
	const ResolvedCollisionShape shape = kernel.wire_collision_shape_for_type(es.type_id);
	const int32_t euler[3] = {es.heading_bam, es.pitch_bam, es.roll_bam};
	write_present_slot_march(r, replica_entity_flags_dword(es), shape.item_type, euler,
			{shape.bbox_center_q16.x, shape.bbox_center_q16.y, shape.bbox_center_q16.z});
}

void build_client_replica_present_rows(const PresentRowsContext &context,
		PoolPresentLifecycleMap &lifecycle, std::vector<float> &out, DoorPhaseTable &door_phases) {
	out.clear();
	if (context.runtime == nullptr) return;
	mission::MissionKernel &kernel = context.kernel;
	ClientRuntime &runtime = *context.runtime;
	const bool joiner = context.joiner;
	// P7: every path (SP / LAN host / joiner) reads its own inmatch ClientRuntime view's ClientState.
	const replication::ClientState &cs = runtime.state();
	const Entity *local_player = kernel.world.registry.get(kernel.world.cached.local_player);
	const bool first_person_usegun = local_first_person_usegun(kernel, local_player);
	const int count = static_cast<int>(cs.entities.size());
	const std::unordered_map<uint16_t, uint16_t> carried_types =
			carried_object_types_by_carrier(cs);
	const ClientReplicaPresentContext replica_present_context{
			&kernel.seat_specs, &kernel.world.tables.weapons, joiner, &carried_types};
	out.assign(static_cast<size_t>(count) * PF_STRIDE, 0.0f);
	door_phases.clear();
	float *w = out.data();
	for (int i = 0; i < count; ++i) {
		float *r = w + static_cast<size_t>(i) * PF_STRIDE;
		const replication::ClientEntityState &es = cs.entities[i];
		initialize_client_replica_present_row(r);
		// A wire-only row has no corpse timer: it reads the org0 skin callback's
		// live DEATH value (world::death_ctrl_register_value's 0xFFFF).
		r[PF_DEATH_CTRL] = 65535.0f;

		// Self-filter (joiner): the host SNAPs our own entity (wire handle H) and streams
		// it back in 0x0A; we draw our local player L via LocalPlayerPresenter, so drop the wire
		// echo here. The row stays at its zero/unresolved defaults (PF_TYPE_ID 0), which the
		// wire render pass skips. [net-re §5.38b two-handle L-vs-H reconciliation]
		if (joiner && runtime.has_self_handle() && es.handle == runtime.self_handle()) {
			continue;
		}
		// The canonical decoded-client projection owns wire identity, pose,
		// lifecycle, and remote Person appearance for every role. The remainder
		// of this function is role/world enrichment only.
		project_client_replica_present_row(r, es, cs, replica_present_context);

		// On the HOST listen server, kind/index/bms_id/net_id resolve from the authored
		// registry entity behind the decoded handle, so EntityIndex can defer
		// that row to MissionPresentPass. A production header-only joiner instead presents
		// every streamed row wire-direct: pools 1-3 have exact-handle native gameplay rows,
		// but those rows carry the spawn-origin sentinel and therefore no authored-node
		// identity. The guarded fill below exists only for an explicit complete-BMS/debug
		// join, where authored promotion supplied a matching type at the same handle.
		// Hidden/alive/animation state still comes from the wire. Remote pool-0 organics
		// remain wire-rendered through their remote-request-shaped body path.
		const EntityHandle h{es.handle};
		const Entity *ent = (!joiner) ? kernel.world.registry.get(h) : nullptr;
		// Retail's terrain collector sends pool-1 model rows through
		// Render_SectorEntity; pool-2 statics and pool-3 marker models join the
		// same sector list through their dedicated collectors. Pool-0 skeletal
		// organics take the general/body list and do not execute this writer.
		// Keep validity independent of whether this client resolves the row to a
		// placed node: a wire-fallback model still executes the same callback,
		// while a failed model build has no CTRL surface on which to apply it.
		const bool sector_model_row = h.pool() >= 1 && h.pool() <= 3;
		if (joiner && h.pool() >= 1 && h.pool() <= 3) {
			const Entity *local = kernel.world.registry.get(h);
			if (local != nullptr && local->spawn_origin != kSpawnOriginNone &&
					static_cast<uint16_t>(local->item_id) == es.type_id) {
				r[PF_KIND] = static_cast<float>(local->spawn_origin >> 24);
				r[PF_INDEX] = static_cast<float>(spawn_origin_index(local->spawn_origin));
				r[PF_BMS_ID] = static_cast<float>(local->bms_id);
				r[PF_NET_ID] = static_cast<float>(local->net_id);
			}
		}
		if (ent) {
			r[PF_KIND] = static_cast<float>(ent->spawn_origin >> 24);
			r[PF_INDEX] = static_cast<float>(spawn_origin_index(ent->spawn_origin));
			r[PF_BMS_ID] = static_cast<float>(ent->bms_id);
			r[PF_NET_ID] = static_cast<float>(ent->net_id);
			r[PF_BODY_ANIM_SLOT] = static_cast<float>(ent->body_anim_slot);
			r[PF_PITCH_DEG] = static_cast<float>(pool_present_pitch_deg(*ent));
			r[PF_ROLL_DEG] = static_cast<float>(pool_present_roll_deg(*ent));
			r[PF_HIDDEN] = pool_row_hidden(*ent) ? 1.0f : 0.0f;
			r[PF_ALIVE] = ent->alive ? 1.0f : 0.0f;
			r[PF_HUSK] = (ent->engine_flags & kEntityFlagHusk) ? 1.0f : 0.0f;
			for (int phase = 0; phase < 6; ++phase)
				r[PF_OBJECT_DESTROY + phase] = float(ent->destroy_phases_q16[phase]);
			// The org0 skin callback's DEATH register off the authoritative
			// organic row's dead flag + corpse timer [orig: BoneCallback_org0_Skin
			// @0x4e3669..0x4e368e].
			if (ent->kind == EntityKind::Organic)
				r[PF_DEATH_CTRL] = static_cast<float>(death_ctrl_register_value(
						((ent->flags | ent->engine_flags) & kEntityFlagDead) != 0, ent->corpse_timer));
			// The authority owns the exact MoveOrder stance latch (bits 8/9 of
			// Player_PackInputStateToEntity @0x4df450; the MATCHTERRAIN tier reads them at
			// Terrain_RenderSectorEntitiesBySide @0x5c7dc2..0x5c7ded - docs/foliage/foliage-re.md). The compact
			// projection above reconstructs this from animation flags for joiners;
			// host/SP must prefer the source byte used by retail's gate.
			r[PF_STANCE_BITS] = static_cast<float>(ent->net_stance_bits & 0x03u);
			r[PF_PARACHUTE_DEPLOYED] =
					((ent->flags | ent->engine_flags) & kEntityFlagParachute) != 0 ? 1.0f : 0.0f;
			write_present_slot_march(r, *ent);
			write_present_section_mask(r, item_hidden_sections(*ent));
			write_present_doors(r, i, kernel.world, *ent, door_phases);
			r[PF_RIGHT_HAND_COLLAPSED] =
					world::mount_collapses_right_hand_row(*ent) ? 1.0f : 0.0f;
			// The cveh render callback publishes directly from the live entity
			// motor fields; a joiner publishes its own twin's below.
			// [orig: Entity_CacheVehicleHUDStats @ 0x4929B0;
			//  stores @0x4929D7 / @0x4929F1]
			write_present_vehicle_motion_controls(r, kernel.world, *ent);
			// Only a carrier in the witnessed live UseGun attachment relation
			// publishes its inline MountSlot's HEAT_GLOW, including owned cold
			// zero. A joiner has no heat-window/ownership state in its compact
			// row and must not synthesize one.
			// [orig: attachment call @ 0x546518;
			//  HUD_CacheWeaponSlotInfo stores @ 0x440969 / @ 0x440991]
			write_present_world_model_heat_glow(r, kernel.world, *ent);
		}
		// The local first-person UseGun parent cull is a render verdict of THIS
		// machine's own mount state, and retail's render walk applies it
		// identically on every role. Resolve the mount row directly: a joiner's
		// exact-handle materialized entity carries the resolved embedded
		// MountSlot (primary_weapon_slot_adm), while `ent` above deliberately
		// stays host-only authored-identity enrichment — keeping this inside it
		// skipped the cull on joiners, drawing the mounted gun TWICE (its wire
		// world model plus the FP model).
		// [orig: Entity_RenderVehicleModel @0x4407d0 predicate
		//  @0x4407f6..0x44084c; sole submit @0x440918]
		if (first_person_usegun && local_player->mount_target == h) {
			const Entity *mount_row = kernel.world.registry.get(h);
			if (mount_row != nullptr &&
					local_view_suppresses_mount(kernel, h, mount_row->primary_weapon_slot_adm))
				r[PF_LOCAL_VIEW_SUPPRESSED] = 1.0f;
		}
		// The `tank` render class swaps the hull for its virtual display while
		// this machine's player drives it in first person.
		// [orig: 0x449EF0 gate @0x449F12..0x449F27]
		if (local_player != nullptr && local_player->mount_target == h) {
			const Entity *vehicle_row = kernel.world.registry.get(h);
			if (vehicle_row != nullptr &&
					local_view_draws_virtual_display(kernel.world, kernel.local.view, *vehicle_row))
				r[PF_LOCAL_VIEW_SUPPRESSED] = 1.0f;
		}
		// Two retail callbacks write this three-register family. The sector
		// renderer publishes TEX_TEAM for every placed pool-1/2/3 model that
		// reaches its model callback. The generic-world callback publishes the
		// same TEX_TEAM plus TEAMSWING for a nonzero packed zone byte, and writes
		// LFP only when the client-side shared timer-list entry exists.
		// Values come from the decoded client row for BOTH authority and joiner:
		// this preserves the exact 0x0D/0x10/0x20 bytes and later S2C 0x50 team
		// mutations instead of reaching around the replica pipeline.
		// [orig: Render_SectorEntity @0x5C424F..0x5C425F;
		//  BoneCallback_gnrc_World @0x4E288B..0x4E28FB]
		const bool zone_ctrl = es.zone_number_rank != 0;
		const int32_t signed_team = es.team < 0x80u
				? static_cast<int32_t>(es.team)
				: static_cast<int32_t>(es.team) - 0x100;
		if (sector_model_row || zone_ctrl) {
			r[PF_TEX_TEAM_VALID] = 1.0f;
			r[PF_TEX_TEAM] = static_cast<float>(signed_team);
		}
		if (zone_ctrl) {
			r[PF_ZONE_CTRL_VALID] = 1.0f;
			const int32_t team_swing = es.team == 1u ? 0 : (es.team == 2u ? 0x10000 : 0x8000);
			r[PF_TEAMSWING] = static_cast<float>(team_swing);
			int32_t camp_percent = 0;
			if (runtime.lfp_cam_percent(es.handle, camp_percent)) {
				r[PF_LFP_CAMPPERCENT_VALID] = 1.0f;
				r[PF_LFP_CAMPPERCENT] = static_cast<float>(camp_percent);
			}
		}
		// Wire lifecycle — PF_RESPAWN_REVISION, the bit0 hide with its
		// carrier-attach exemption, and the class-dependent dead bit (vehicle
		// wrecks are flags&4, not the organic bit 1) — is written by
		// project_client_replica_present_row for every role; the authoritative
		// registry block above overrides hidden/alive on the host. Do not
		// re-derive it here: a pre-ADR-0026 copy of this logic once drifted by
		// testing vehicles against the organic dead bit.
		EmplacedWeaponControls emplaced;
		if (ent != nullptr) {
			if (emplaced_weapon_controls_for(kernel.world, *ent, emplaced))
				write_present_emplaced_controls(r, emplaced);
		}
		const bool authoritative_attachment_pose =
				ent != nullptr && ent->emplacement_parent.valid() &&
				ent->emplacement_pose_metadata_resolved &&
				ent->emplacement_parent.packed == replication::persistent_carrier_handle(es);
		MountedPose client_attachment_pose;
		const uint32_t attachment_time_ms = kernel.panm_time_override_ms >= 0
				? static_cast<uint32_t>(kernel.panm_time_override_ms)
				: kernel.world.logic_tick * 16u;
		const bool reconstructed_client_attachment_pose = joiner &&
				resolve_client_eweap_attachment_pose(
						es, cs, kernel.seat_specs, kernel.mounted_graphics,
						kernel.assets(), attachment_time_ms, client_attachment_pose);
		if (authoritative_attachment_pose) {
			// NoNetworkCallback addeweap children have only their 0x0D spawn pose in
			// ClientState. The host has already advanced their authoritative userpoint
			// pose through World::update_emplacement_attachments; use that exact result
			// rather than flattening live PANM back to the client's rigid spawn offset.
			r[PF_POS_X] = ent->position.x;
			r[PF_POS_Y] = ent->position.z;
			r[PF_POS_Z] = -ent->position.y;
			r[PF_PITCH_DEG] = static_cast<float>(pool_present_pitch_deg(*ent));
			r[PF_YAW_DEG] = static_cast<float>(pool_present_yaw_deg(*ent, nullptr, EntityClass::Unknown));
			r[PF_ROLL_DEG] = static_cast<float>(pool_present_roll_deg(*ent));
		} else if (reconstructed_client_attachment_pose) {
			r[PF_POS_X] = client_attachment_pose.position.x;
			r[PF_POS_Y] = client_attachment_pose.position.z;
			r[PF_POS_Z] = -client_attachment_pose.position.y;
			r[PF_PITCH_DEG] = static_cast<float>(client_attachment_pose.pitch * kDegreesPerBam);
			r[PF_YAW_DEG] = static_cast<float>(mission_yaw_deg_from_bam_heading(client_attachment_pose.heading));
			r[PF_ROLL_DEG] = static_cast<float>(client_attachment_pose.roll * kDegreesPerBam);
		}
		// No unattached else: the projection already wrote the decoded wire
		// pose — the chased/snapped position, the vehicle BAM32 euler X/Y, and
		// the full-16-bit-precision heading (net-re §5.38e, D-NET-196). The
		// two attachment branches above and the host's authoritative registry
		// pitch/roll (the `ent` block) override it where a better source
		// exists; re-deriving the wire pose here would re-clobber the host's
		// live vehicle attitude with the stale spawn/dead-pose eulers.
		// Infantry anim from the local AI pool (host only — same registry caveat as above).
		if (!joiner) {
			const AiEntity *ae = kernel.world.ai.for_handle(h);
			if (ae != nullptr) {
				for (int slot = 0; slot < 2; ++slot) {
					// HUD_CacheEntityDisplayInfo copies comp[113/114] as raw
					// signed dwords. Do not normalize wrapping zero-time states.
					// [orig: stores @0x4A3E2D/@0x4A3E38]
					const int32_t phase = ae->brain.f[AiBrain::kPartAnimPhase0 + slot];
					const bool publish =
							slot != 0 || ent == nullptr || (ent->item_attrib & 0x1000u) == 0;
					uint32_t phase_bits;
					std::memcpy(&phase_bits, &phase, sizeof(phase_bits));
					// The float snapshot transports both 16-bit words as exact
					// integers. ACTIVE zero means unpublished; otherwise it is
					// high16+1. A numeric float32 could lose signed-dword low
					// bits for fast/malformed PLAYPARTANIM rates.
					r[PF_PHASE1 + slot * 2] = static_cast<float>(phase_bits & 0xFFFFu);
					r[PF_ACTIVE1 + slot * 2] = publish
							? static_cast<float>((phase_bits >> 16) + 1u)
							: 0.0f;
				}
			}
			if (ae && ae->inf.active) {
				r[PF_ANIM_STATE] = static_cast<float>(ae->inf.body_clip_state());
				r[PF_ANIM_PHASE_TICKS] = static_cast<float>(ae->inf.clip_phase);
				r[PF_ANIM_VARIANT] = static_cast<float>(ae->inf.anim_variant);
				if (ae->inf.body_blend_active()) {
					r[PF_ANIM_SOURCE_STATE] = static_cast<float>(ae->inf.anim_prev);
					r[PF_ANIM_SOURCE_PHASE_TICKS] =
							static_cast<float>(ae->inf.anim_prev_clip_phase);
					r[PF_ANIM_BLEND_WEIGHT] = ae->inf.anim_blend_weight;
					r[PF_ANIM_SOURCE_VARIANT] = static_cast<float>(ae->inf.anim_prev_variant);
				}
				// The upper-body weapon channel this body derived for itself —
				// for the host's OWN player and for every wire peer alike, since
				// remote_player_body_anim now runs the same selection. The gate is
				// the §14.8.6 consumer test; engine_flags bit 0x100 is this file's
				// established "is a player" mirror of entity+0x24, which is what
				// keeps NPCs (who carry no hold ladder in the original either) out.
				if (ent != nullptr &&
						infantry_weapon_channel_visible(
								ae->inf, (ent->engine_flags & kEntityFlagPlayer) != 0,
								world::mount_blocks_weapon_channel(*ent))) {
					r[PF_WPN_ANIM_STATE] = static_cast<float>(ae->inf.weapon_clip_state());
					r[PF_WPN_PHASE_TICKS] = static_cast<float>(ae->inf.wpn_clip_phase);
					r[PF_WPN_VARIANT] = static_cast<float>(ae->inf.wpn_variant);
					if (ae->inf.weapon_blend_active()) {
						r[PF_WPN_SOURCE_STATE] = static_cast<float>(ae->inf.wpn_prev);
						r[PF_WPN_SOURCE_PHASE_TICKS] =
								static_cast<float>(ae->inf.wpn_prev_clip_phase);
						r[PF_WPN_BLEND_WEIGHT] = ae->inf.wpn_blend_weight;
						r[PF_WPN_SOURCE_VARIANT] =
								static_cast<float>(ae->inf.wpn_prev_variant);
					}
				}
				if (ent != nullptr) {
					const anim::AimOverlayInputs inputs =
							world::aim_overlay_inputs_for(*ae, *ent);
					anim::AimOverlayAngles angles[anim::kOverlayClassCount];
					anim::compute_aim_overlay_angles(inputs, angles);
					write_present_overlay(r, angles);
					// This body's third-person gun, from the authoritative
					// record (this block runs on the authority only).
					write_pool_held_weapon(r, context, local_player, *ent, *ae, inputs);
					write_present_person_overlays(r, person_overlays(
							pool_person_overlay_inputs(kernel.world, *ent, *ae, inputs)));
				}
			}
		}
		// These class callbacks mutate the peer's own model sections/pose.
		// Their 0x26 payload is not a compact-transform update.
		// [orig: palm @ 0x53C4C0; cran @ 0x43FC70]
		if (joiner) {
			write_present_replica_slot_march(r, kernel, es);
			const Entity *local = kernel.world.registry.get(h);
			if (local && static_cast<uint16_t>(local->item_id) == es.type_id) {
				r[PF_HUSK] = (local->engine_flags & kEntityFlagHusk) ? 1.0f : 0.0f;
				for (int phase = 0; phase < 6; ++phase)
					r[PF_OBJECT_DESTROY + phase] = float(local->destroy_phases_q16[phase]);
				// The door records advance on every peer (the per-frame entity
				// update calls the door tick at the pool-2 loop exit with no
				// authority test) and the door render/bone callbacks copy each
				// row's Q16 phase onto the CTRL bus from DOOR_00 for every drawn
				// door entity, so a joiner publishes its own DoorSystem rows
				// (ticked + contact-driven locally) exactly as the authority
				// collector does; write_phases self-gates on door_motion.
				// [orig: Entity_UpdateAllEntities @0x4c2100 (the site @0x4C2307,
				//  loop exit @0x4c2278); BoneCallback_BuildBoneTransforms @0x4E3070 (the
				//  loop @0x4e312a..0x4e3145); BoneCallback_AnimatedBones_World
				//  @0x4E3180 (@0x4e3201..0x4e3218)]
				write_present_doors(r, i, kernel.world, *local, door_phases);
				// The vehicle render callbacks run on every peer against the
				// client's own mover state (the joiner's world-side prediction
				// advances the same tracks, wheel phase and springs), so a
				// joiner publishes its local twin's controls exactly as the
				// authority collector does.
				// [orig: HUD_CacheEntityDebugStats @0x449C10 (tank track words
				//  @0x449C3C..0x449C69); Entity_CacheVehicleHUDStats @0x4929B0;
				//  the client mover's track phase Entity_UpdateTankVehiclePhysics
				//  @0x489F98 / @0x489FA0]
				write_present_vehicle_motion_controls(r, kernel.world, *local);
				// A joiner runs no brains: the gun words this carrier's ewep
				// children published on its replica row stand for the brain
				// words its tank/helo render callback reads.
				// [orig: Entity_UpdateTransformAndTurret @0x440F70..0x441020;
				//  HUD_CacheEntityDebugStats @0x449ECF..0x449EE2]
				write_present_replica_vehicle_gun(r, kernel.world, *local, es);
			}
			const auto *item = local ? kernel.world.tables.item_death_traits.get(local->item_id) : nullptr;
			if (local && static_cast<uint16_t>(local->item_id) == es.type_id &&
					(local->palm_sections || local->item_section_piece ||
					 (item && item->death_class == ItemDeathClass::kTower) ||
					 local->death_motion == DeathMotionMode::CraneFalling ||
					 local->death_motion == DeathMotionMode::BuildingEffects)) {
				const Vec3 pos = item_section_render_position(kernel.world, *local);
				r[PF_POS_X] = pos.x; r[PF_POS_Y] = pos.z; r[PF_POS_Z] = -pos.y;
				r[PF_YAW_DEG] = local->yaw;
				r[PF_PITCH_DEG] = local->pitch; r[PF_ROLL_DEG] = local->roll;
				r[PF_ALIVE] = local->alive ? 1.0f : 0.0f;
				write_present_section_mask(r, item_hidden_sections(*local));
			}
		}
	}
	// A callback allocates its fragment directly into the local pool. It has
	// no independent spawn message to wait for. Append only these locally
	// created rows through the ordinary pool-row writer (a fragment publishes
	// no door entry); keep decoded organics on their receive-side animation
	// path. Only the fragment rows touch the shared lifecycle map.
	// [orig: Entity_CloneFromTemplateByType @ 0x4398A0;
	// Terrain_CollectVisibleEntitiesForTerrain @ 0x5C8C60]
	if (!joiner) return;
	DoorPhaseTable unused_doors;
	kernel.world.registry.for_each([&](const Entity &entity) {
		if (!entity.item_section_piece || cs.find(entity.handle.packed) != nullptr) return;
		const int row_index = static_cast<int>(out.size() / PF_STRIDE);
		out.resize(out.size() + PF_STRIDE, 0.0f);
		write_world_present_row(context, local_player, first_person_usegun, entity, row_index,
				lifecycle, out.data() + static_cast<size_t>(row_index) * PF_STRIDE, unused_doors);
	});
}

uint32_t replica_entity_flags_dword(const replication::ClientEntityState &es) {
	// The compact record's flags byte IS entity+36's low byte (the 0x0A fold);
	// a row without one keeps its load-stream byte.
	const uint32_t low = es.state_flags_known ? es.state_flags : (es.spawn_entity_flags & 0xFFu);
	uint32_t flags = (es.spawn_entity_flags & ~0xFFu) | low | es.rm_entity_flags;
	if (es.cls == EntityClass::Player) flags |= kEntityFlagPlayer;
	return flags;
}

void build_world_present_rows(const PresentRowsContext &context,
		PoolPresentLifecycleMap &lifecycle, std::vector<float> &out,
		DoorPhaseTable &door_phases) {
	out.clear();
	mission::MissionKernel &kernel = context.kernel;
	const World &w = kernel.world;
	const Entity *local_player = w.registry.get(w.cached.local_player);
	const bool first_person_usegun = local_first_person_usegun(kernel, local_player);
	// One row per live pool slot, in registry order — the set the host's own
	// ClientState held before D-NET-140 closed (every slot the 0x0C/0x0D/0x10/
	// 0x20 spawn batches stream plus every 0x0A record), now read straight
	// from the pools [orig: Terrain_CollectVisibleEntitiesForTerrain @0x5c8c60
	// walks the pools; see docs/net/novaworld-net-re.md D-NET-140]. A row
	// without a def keeps PF_TYPE_ID 0, which the wire pass skips.
	int count = 0;
	w.registry.for_each([&](const Entity &) { ++count; });
	out.assign(static_cast<size_t>(count) * PF_STRIDE, 0.0f);
	door_phases.clear();
	float *rows = out.data();
	int i = 0;
	w.registry.for_each([&](const Entity &e) {
		const int row_index = i++;
		write_world_present_row(context, local_player, first_person_usegun, e, row_index,
				lifecycle, rows + static_cast<size_t>(row_index) * PF_STRIDE, door_phases);
	});
}

static void write_world_present_row(const PresentRowsContext &context,
		const Entity *local_player, bool first_person_usegun, const Entity &e,
		int row_index, PoolPresentLifecycleMap &lifecycle, float *r,
		DoorPhaseTable &door_phases) {
	mission::MissionKernel &kernel = context.kernel;
	const World &w = kernel.world;
	const EntityHandle h = e.handle;
	const EntityClass cls = replication::entity_class_of(e);
	const AiEntity *ae = w.ai.for_handle(h);
	initialize_client_replica_present_row(r);

	// Wire identity + lifecycle, exactly what project_client_replica_present_row
	// derives for a decoded row, sourced from the authoritative record.
	r[PF_TYPE_ID] = static_cast<float>(static_cast<uint16_t>(e.item_id));
	r[PF_WIRE_HANDLE] = static_cast<float>(h.packed);
	// The groundEntity/mount link the footstep slot reads (mount wins over
	// ground — retail: NetPacket_SerializePlayerState op1 @0x4c0a08, see
	// docs/net/novaworld-net-re.md); -1 = free-standing.
	const uint16_t carrier = e.mounted && e.mount_target.valid()
			? e.mount_target.packed
			: (e.ground_target.valid() ? e.ground_target.packed : EntityHandle::kInvalid);
	r[PF_CARRIER_HANDLE] = carrier != EntityHandle::kInvalid
			? static_cast<float>(carrier) : -1.0f;
	if (cls == EntityClass::Player) {
		// A player's wire net_id IS its packed character id (entity+0x15C).
		r[PF_CHARACTER_ID] = static_cast<float>(replication::player_wire_net_id(e));
	}
	const Vec3 render_position = item_section_render_position(w, e);
	r[PF_POS_X] = render_position.x;
	r[PF_POS_Y] = render_position.z;
	r[PF_POS_Z] = -render_position.y;
	r[PF_PITCH_DEG] = static_cast<float>(pool_present_pitch_deg(e));
	r[PF_YAW_DEG] = static_cast<float>(pool_present_yaw_deg(e, ae, cls));
	r[PF_ROLL_DEG] = static_cast<float>(pool_present_roll_deg(e));
	// The decoded fold bumps a row's respawn revision on every dead->alive
	// edge of its wire state byte (organic bit 1; vehicle wrecks flag 4).
	// Mirror that edge from the authoritative flags so the EntityPresenter wire
	// walk re-seeds the same way on the host.
	{
		const uint8_t dead_bit = cls == EntityClass::Vehicle
				? replication::kVehicleFlagDeadPose
				: static_cast<uint8_t>(kEntityFlagDead);
		const bool dead = (e.flags & dead_bit) != 0u;
		PoolPresentLifecycle &life = lifecycle[h.packed];
		if (life.registry_spawn_id != e.registry_spawn_id) {
			life.registry_spawn_id = e.registry_spawn_id;
			life.dead_known = false;
		}
		if (life.dead_known && life.dead && !dead) ++life.respawn_revision;
		life.dead_known = true;
		life.dead = dead;
		r[PF_RESPAWN_REVISION] = static_cast<float>(life.respawn_revision);
	}
	r[PF_KIND] = static_cast<float>(e.spawn_origin >> 24);
	r[PF_INDEX] = static_cast<float>(spawn_origin_index(e.spawn_origin));
	r[PF_BMS_ID] = static_cast<float>(e.bms_id);
	r[PF_NET_ID] = static_cast<float>(e.net_id);
	r[PF_BODY_ANIM_SLOT] = static_cast<float>(e.body_anim_slot);
	r[PF_HIDDEN] = pool_row_hidden(e) ? 1.0f : 0.0f;
	r[PF_ALIVE] = e.alive ? 1.0f : 0.0f;
	r[PF_HUSK] = (e.engine_flags & kEntityFlagHusk) ? 1.0f : 0.0f;
	for (int phase = 0; phase < 6; ++phase)
		r[PF_OBJECT_DESTROY + phase] = float(e.destroy_phases_q16[phase]);
	// The org0 skin bone-callback's DEATH register (CTRL ordinal 6): the corpse
	// fade off the authoritative organic row's dead flag + corpse timer; every
	// other row reads retail's live 0xFFFF [orig: BoneCallback_org0_Skin
	// @0x4e3669..0x4e368e; see docs/world/world-wac-ai-re.md].
	r[PF_DEATH_CTRL] = static_cast<float>(e.kind == EntityKind::Organic
			? death_ctrl_register_value(((e.flags | e.engine_flags) & kEntityFlagDead) != 0, e.corpse_timer)
			: 0xFFFF);
	// The authority owns the exact MoveOrder stance latch (bits 8/9 of
	// Player_PackInputStateToEntity @0x4df450; the MATCHTERRAIN tier reads them at
	// Terrain_RenderSectorEntitiesBySide @0x5c7dc2..0x5c7ded - docs/foliage/foliage-re.md).
	r[PF_STANCE_BITS] = static_cast<float>(e.net_stance_bits & 0x03u);
	r[PF_PARACHUTE_DEPLOYED] =
			((e.flags | e.engine_flags) & kEntityFlagParachute) != 0 ? 1.0f : 0.0f;
	write_present_slot_march(r, e);
	write_present_section_mask(r, item_hidden_sections(e));
	write_present_doors(r, row_index, w, e, door_phases);
	r[PF_RIGHT_HAND_COLLAPSED] =
			world::mount_collapses_right_hand_row(e) ? 1.0f : 0.0f;
	// The cveh render callback publishes directly from the live entity
	// motor fields [orig: Entity_CacheVehicleHUDStats @0x4929B0, stores
	// @0x4929D7 / @0x4929F1; see docs/world/vehicle-client-movers-re.md].
	write_present_vehicle_motion_controls(r, w, e);
	// Only a carrier in the witnessed live UseGun attachment relation
	// publishes its inline MountSlot's HEAT_GLOW, including owned cold zero.
	// [orig: attachment call @0x546518; HUD_CacheWeaponSlotInfo stores
	//  @0x440969 / @0x440991; see docs/world/world-wac-ai-re.md]
	write_present_world_model_heat_glow(r, w, e);
	// The local first-person UseGun parent cull is a render verdict of THIS
	// machine's own mount state [orig: Entity_RenderVehicleModel @0x4407d0
	// predicate @0x4407f6..0x44084c, sole submit @0x440918; see
	// docs/world/world-wac-ai-re.md].
	if (first_person_usegun && local_player->mount_target == h &&
			local_view_suppresses_mount(kernel, h, e.primary_weapon_slot_adm))
		r[PF_LOCAL_VIEW_SUPPRESSED] = 1.0f;
	// The `tank` render class swaps the hull for its virtual display while
	// this machine's player drives it in first person.
	// [orig: 0x449EF0 gate @0x449F12..0x449F27]
	if (local_player != nullptr && local_player->mount_target == h &&
			local_view_draws_virtual_display(w, kernel.local.view, e))
		r[PF_LOCAL_VIEW_SUPPRESSED] = 1.0f;
	// Two retail callbacks write this three-register family. The sector
	// renderer publishes TEX_TEAM for every placed pool-1/2/3 model that
	// reaches its model callback. The generic-world callback publishes the
	// same TEX_TEAM plus TEAMSWING for a nonzero packed zone byte, and writes
	// LFP only when the client-side shared timer-list entry exists.
	// [orig: Render_SectorEntity @0x5C424F..0x5C425F;
	//  BoneCallback_gnrc_World @0x4E288B..0x4E28FB; see docs/world/world-wac-ai-re.md]
	const bool sector_model_row = h.pool() >= 1 && h.pool() <= 3;
	const bool zone_ctrl = e.zone_number != 0 &&
			zone_chain_zone_info_byte(w.zones.chain, e) != 0;
	const int32_t signed_team = e.team < 0x80u
			? static_cast<int32_t>(e.team)
			: static_cast<int32_t>(e.team) - 0x100;
	if (sector_model_row || zone_ctrl) {
		r[PF_TEX_TEAM_VALID] = 1.0f;
		r[PF_TEX_TEAM] = static_cast<float>(signed_team);
	}
	if (zone_ctrl) {
		r[PF_ZONE_CTRL_VALID] = 1.0f;
		const int32_t team_swing = e.team == 1u ? 0 : (e.team == 2u ? 0x10000 : 0x8000);
		r[PF_TEAMSWING] = static_cast<float>(team_swing);
		int32_t camp_percent = 0;
		if (context.runtime != nullptr &&
				context.runtime->lfp_cam_percent(h.packed, camp_percent)) {
			r[PF_LFP_CAMPPERCENT_VALID] = 1.0f;
			r[PF_LFP_CAMPPERCENT] = static_cast<float>(camp_percent);
		}
	}
	EmplacedWeaponControls emplaced;
	if (emplaced_weapon_controls_for(w, e, emplaced))
		write_present_emplaced_controls(r, emplaced);
	if (ae == nullptr) return;
	for (int slot = 0; slot < 2; ++slot) {
		// HUD_CacheEntityDisplayInfo copies comp[113/114] as raw
		// signed dwords. Do not normalize wrapping zero-time states.
		// [orig: HUD_CacheEntityDisplayInfo stores @0x4A3E2D/@0x4A3E38;
		//  see docs/world/world-wac-ai-re.md]
		const int32_t phase = ae->brain.f[AiBrain::kPartAnimPhase0 + slot];
		const bool publish = slot != 0 || (e.item_attrib & 0x1000u) == 0;
		uint32_t phase_bits;
		std::memcpy(&phase_bits, &phase, sizeof(phase_bits));
		// The float snapshot transports both 16-bit words as exact
		// integers. ACTIVE zero means unpublished; otherwise it is
		// high16+1.
		r[PF_PHASE1 + slot * 2] = static_cast<float>(phase_bits & 0xFFFFu);
		r[PF_ACTIVE1 + slot * 2] = publish
				? static_cast<float>((phase_bits >> 16) + 1u)
				: 0.0f;
	}
	if (!ae->inf.active) return;
	r[PF_ANIM_STATE] = static_cast<float>(ae->inf.body_clip_state());
	r[PF_ANIM_PHASE_TICKS] = static_cast<float>(ae->inf.clip_phase);
	r[PF_ANIM_VARIANT] = static_cast<float>(ae->inf.anim_variant);
	if (ae->inf.body_blend_active()) {
		r[PF_ANIM_SOURCE_STATE] = static_cast<float>(ae->inf.anim_prev);
		r[PF_ANIM_SOURCE_PHASE_TICKS] =
				static_cast<float>(ae->inf.anim_prev_clip_phase);
		r[PF_ANIM_BLEND_WEIGHT] = ae->inf.anim_blend_weight;
		r[PF_ANIM_SOURCE_VARIANT] = static_cast<float>(ae->inf.anim_prev_variant);
	}
	// The upper-body weapon channel this body derived for itself; the gate
	// is the §14.8.6 consumer test and engine_flags bit 0x100 is the "is a
	// player" mirror of entity+0x24 (NPCs carry no hold ladder).
	if (infantry_weapon_channel_visible(
				ae->inf, (e.engine_flags & kEntityFlagPlayer) != 0,
				world::mount_blocks_weapon_channel(e))) {
		r[PF_WPN_ANIM_STATE] = static_cast<float>(ae->inf.weapon_clip_state());
		r[PF_WPN_PHASE_TICKS] = static_cast<float>(ae->inf.wpn_clip_phase);
		r[PF_WPN_VARIANT] = static_cast<float>(ae->inf.wpn_variant);
		if (ae->inf.weapon_blend_active()) {
			r[PF_WPN_SOURCE_STATE] = static_cast<float>(ae->inf.wpn_prev);
			r[PF_WPN_SOURCE_PHASE_TICKS] =
					static_cast<float>(ae->inf.wpn_prev_clip_phase);
			r[PF_WPN_BLEND_WEIGHT] = ae->inf.wpn_blend_weight;
			r[PF_WPN_SOURCE_VARIANT] =
					static_cast<float>(ae->inf.wpn_prev_variant);
		}
	}
	const anim::AimOverlayInputs inputs = world::aim_overlay_inputs_for(*ae, e);
	anim::AimOverlayAngles angles[anim::kOverlayClassCount];
	anim::compute_aim_overlay_angles(inputs, angles);
	write_present_overlay(r, angles);
	// This body's third-person gun: a player's echo, or the copy a mounted org1
	// rider holds of its parent's byte.
	write_pool_held_weapon(r, context, local_player, e, *ae, inputs);
	// The body's item overlays: the canopy, the goggles, the binoculars and the
	// carried object. [orig: BoneCallback_org0_World @0x4e3940]
	write_present_person_overlays(r, person_overlays(pool_person_overlay_inputs(w, e, *ae, inputs)));
}

bool local_player_person_overlays(const PresentRowsContext &context, PersonOverlays &out) {
	out = PersonOverlays{};
	const World &w = context.kernel.world;
	const Entity *e = w.registry.get(w.cached.local_player);
	const AiEntity *ae = e != nullptr ? w.ai.for_handle(e->handle) : nullptr;
	if (ae == nullptr || !ae->inf.active) return false;
	PersonOverlayInputs in =
			pool_person_overlay_inputs(w, *e, *ae, world::aim_overlay_inputs_for(*ae, *e));
	// A joiner's own body carries what the authority attached to its entity
	// there: the S2C 0x2F relation names that entity's handle (H), and the
	// retail client's 0x0A local block attaches the carried object to the
	// local player itself [orig: NapiNPClientMsg_0x00A @0x43066c..0x430695;
	// NapiNPClientMsg_0x02F @0x430E10].
	if (context.joiner && context.runtime != nullptr && context.runtime->has_self_handle()) {
		const std::unordered_map<uint16_t, uint16_t> carried =
				carried_object_types_by_carrier(context.runtime->state());
		const auto it = carried.find(context.runtime->self_handle());
		if (it != carried.end()) in.carried_type_id = it->second;
	}
	out = person_overlays(in);
	return true;
}

} // namespace opennova::inmatch
