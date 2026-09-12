// The packed present-row collectors (see present_rows.h).
#include <runtime/inmatch/present_rows.h>

#include <runtime/inmatch/client_replica_present.h> // the emplaced/overlay/held-weapon writers
#include <runtime/inmatch/client_replica_present_projection.h> // the canonical decoded-client projection (ADR 0031)
#include <runtime/inmatch/replica_query.h> // client_entity_for_handle
#include <runtime/simassets/mounted_pose.h> // the ONE mounted matrix path (S4b)
#include <runtime/simassets/pose_inputs.h> // seat/mount pose predicates + aim inputs (ADR 0028)
#include <runtime/simassets/seat_spec_extract.h> // item_seat_spec_for_type
#include <runtime/world/mount_controls.h> // heat-glow + emplaced turret CTRL sources (ADR 0028)
#include <runtime/world/present_rows.h>
#include <runtime/world/vehicle_motor.h> // vehicle_ctrl_registers
#include <runtime/world/infantry.h> // infantry_weapon_channel_visible
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/entity.h> // kEntityFlag* (the wire state_flags byte IS entity+36 low)
#include <runtime/world/zone_chain.h> // zone_chain_zone_info_byte
#include <runtime/replication/client_state.h>
#include <runtime/replication/entity_wire_bridge.h> // entity_class_of / player_wire_net_id (the host's own rows)
#include <runtime/anim/aim_overlay.h> // the torso-bend overlay blends [orig: @0x4b1290]
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <base/io/strutil.h> // iequals

#include <cmath>
#include <cstring>

using namespace opennova::threedi;

namespace opennova::inmatch {

using namespace opennova::world;

namespace {

inline constexpr double kFixed16 = 65536.0;
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
	// The authoritative motor owns these controls. Its render callback selects
	// which channels may be published; joiner compact rows leave VALID clear
	// because they do not carry the full animation and turret state.
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
// resolver the host authority runs (simassets::resolve_model_mounted_pose) —
// one mounted matrix path. The model resolves through the sim cache by the
// installed spec's graphic key, exactly like the host-side resolver.
bool resolve_client_eweap_attachment_pose(
		const replication::ClientEntityState &child,
		const replication::ClientState &state,
		const std::vector<mission::ItemSeatSpec> &specs,
		const std::unordered_map<int32_t, std::string> &graphics_by_type,
		simassets::SimModelCache &models,
		uint32_t time_ms, MountedPose &out) {
	if (child.parent_handle == EntityHandle::kInvalid) return false;
	const replication::ClientEntityState *parent =
			client_entity_for_handle(state, child.parent_handle);
	if (parent == nullptr) return false;
	const mission::ItemSeatSpec *parent_spec =
			simassets::item_seat_spec_for_type(specs, parent->type_id);
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
	if (graphic_found == graphics_by_type.end() || !models.has_index())
		return false;
	const Threedi3di3 *model_ptr = models.model_for(graphic_found->second);
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

	Entity carrier;
	carrier.item_id = static_cast<int32_t>(parent->type_id);
	carrier.position = {
			static_cast<float>(parent->x / kFixed16),
			static_cast<float>(parent->y / kFixed16),
			static_cast<float>(parent->z / kFixed16)};
	carrier.yaw = static_cast<int16_t>(std::lround(
			mission_yaw_deg_from_bam_heading(parent->heading_bam)));
	carrier.pitch = static_cast<int16_t>(std::lround(
			static_cast<double>(parent->pitch_bam) * kDegreesPerBam));
	carrier.roll = static_cast<int16_t>(std::lround(
			static_cast<double>(parent->roll_bam) * kDegreesPerBam));
	return simassets::resolve_model_mounted_pose(
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
		return static_cast<double>(e.yaw); // World::update_emplacement_attachments' exact result
	if (cls == EntityClass::Vehicle && e.veh.yaw_seeded)
		return mission_yaw_deg_from_bam_heading(e.veh.yaw_bam);
	if (cls == EntityClass::Infantry && ae != nullptr)
		return mission_yaw_deg_from_bam_heading(ae->heading);
	return static_cast<double>(e.yaw);
}

void build_client_replica_present_rows(const PresentRowsContext &context,
        PoolPresentLifecycleMap &lifecycle, std::vector<float> &out, DoorPhaseTable &door_phases) {
	out.clear();
	if (context.runtime == nullptr) return;
	mission::MissionKernel &kernel = context.kernel;
	ClientRuntime &runtime = *context.runtime;
	const bool joiner = context.joiner;
	// P7: every path (SP / LAN host / joiner) reads its own npruntime ClientRuntime view's ClientState.
	const replication::ClientState &cs = runtime.state();
	const Entity *local_player = kernel.world.registry.get(kernel.world.cached.local_player);
	const bool first_person_usegun = local_first_person_usegun(kernel, local_player);
	const int count = static_cast<int>(cs.entities.size());
	const ClientReplicaPresentContext replica_present_context{
			&kernel.seat_specs, &kernel.world.tables.weapons, joiner};
	out.assign(static_cast<size_t>(count) * PF_STRIDE, 0.0f);
	door_phases.clear();
	float *w = out.data();
	for (int i = 0; i < count; ++i) {
		float *r = w + static_cast<size_t>(i) * PF_STRIDE;
		const replication::ClientEntityState &es = cs.entities[i];
		initialize_client_replica_present_row(r);

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
		// render_sector_entity; pool-2 statics and pool-3 marker models join the
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
			r[PF_PITCH_DEG] = static_cast<float>(ent->pitch);
			r[PF_ROLL_DEG] = static_cast<float>(ent->roll);
			r[PF_HIDDEN] = ent->hidden ? 1.0f : 0.0f;
			r[PF_ALIVE] = ent->alive ? 1.0f : 0.0f;
            r[PF_HUSK] = (ent->engine_flags & kEntityFlagHusk) ? 1.0f : 0.0f;
            for (int phase = 0; phase < 6; ++phase)
                r[PF_OBJECT_DESTROY + phase] = float(ent->destroy_phases_q16[phase]);
			// The authority owns the exact MoveOrder stance latch (bits 8/9 of
			// Player_PackInputStateToEntity @0x4df450; the MATCHTERRAIN tier reads them at
			// Terrain_RenderSectorEntitiesBySide @0x5c7dc2..0x5c7ded - docs/foliage/foliage-re.md). The compact
			// projection above reconstructs this from animation flags for joiners;
			// host/SP must prefer the source byte used by retail's gate.
			r[PF_STANCE_BITS] = static_cast<float>(ent->net_stance_bits & 0x03u);
			write_present_section_mask(r, item_hidden_sections(*ent));
			write_present_doors(r, i, kernel.world, *ent, door_phases);
			r[PF_RIGHT_HAND_COLLAPSED] =
					simassets::mount_collapses_right_hand_row(*ent) ? 1.0f : 0.0f;
			// The cveh render callback publishes directly from the live entity
			// motor fields. Do this only for the authoritative registry row:
			// the compact view has no steer/currentSpeed source to reconstruct.
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
		// Two retail callbacks write this three-register family. The sector
		// renderer publishes TEX_TEAM for every placed pool-1/2/3 model that
		// reaches its model callback. The generic-world callback publishes the
		// same TEX_TEAM plus TEAMSWING for a nonzero packed zone byte, and writes
		// LFP only when the client-side shared timer-list entry exists.
		// Values come from the decoded client row for BOTH authority and joiner:
		// this preserves the exact 0x0D/0x10/0x20 bytes and later S2C 0x50 team
		// mutations instead of reaching around the replica pipeline.
		// [orig: render_sector_entity @0x5C424F..0x5C425F;
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
				ent->emplacement_parent.packed == es.parent_handle;
		MountedPose client_attachment_pose;
		const uint32_t attachment_time_ms = kernel.panm_time_override_ms >= 0
				? static_cast<uint32_t>(kernel.panm_time_override_ms)
				: kernel.world.logic_tick * 16u;
		const bool reconstructed_client_attachment_pose = joiner &&
				resolve_client_eweap_attachment_pose(
						es, cs, kernel.seat_specs, kernel.mounted_graphics,
						kernel.models, attachment_time_ms, client_attachment_pose);
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
				r[PF_ANIM_STATE] = static_cast<float>(ae->inf.anim_state);
				r[PF_ANIM_PHASE_TICKS] = static_cast<float>(ae->inf.clip_phase);
				if (ae->inf.body_blend_active()) {
					r[PF_ANIM_SOURCE_STATE] = static_cast<float>(ae->inf.anim_prev);
					r[PF_ANIM_SOURCE_PHASE_TICKS] =
							static_cast<float>(ae->inf.anim_prev_clip_phase);
					r[PF_ANIM_BLEND_WEIGHT] = ae->inf.anim_blend_weight;
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
								simassets::mount_blocks_weapon_channel(*ent))) {
					r[PF_WPN_ANIM_STATE] = static_cast<float>(ae->inf.wpn_state);
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
							simassets::aim_overlay_inputs_for(*ae, *ent);
					anim::AimOverlayAngles angles[anim::kOverlayClassCount];
					anim::compute_aim_overlay_angles(inputs, angles);
					write_present_overlay(r, angles);
					// This body's third-person gun. Player rows only: retail's
					// composition gate is the Flags 0x100 player classifier, and
					// placed NPCs carry no equipped index anyway.
					if ((ent->engine_flags & kEntityFlagPlayer) != 0) {
						write_present_held_weapon(
								r, ent->equipped_adm_index,
								(ent->flags & kEntityFlagDead) != 0, inputs,
								ae->inf.wpn_state);
					}
				}
			}
		}
        // These class callbacks mutate the peer's own model sections/pose.
        // Their 0x26 payload is not a compact-transform update.
        // [orig: palm @ 0x53C4C0; cran @ 0x43FC70]
        if (joiner) {
            const Entity *local = kernel.world.registry.get(h);
            if (local && static_cast<uint16_t>(local->item_id) == es.type_id) {
                r[PF_HUSK] = (local->engine_flags & kEntityFlagHusk) ? 1.0f : 0.0f;
                for (int phase = 0; phase < 6; ++phase)
                    r[PF_OBJECT_DESTROY + phase] = float(local->destroy_phases_q16[phase]);
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
    // no independent spawn message to wait for. Use the ordinary pool writer
    // and append only these locally created rows; keep decoded organics on
    // their receive-side animation path.
    // [orig: Entity_CloneFromTemplateByType @ 0x4398A0;
    // collect_visible_entities_for_terrain @ 0x5C8C60]
    bool local_pieces = false;
    kernel.world.registry.for_each([&](const Entity &entity) {
        if (entity.item_section_piece && cs.find(entity.handle.packed) == nullptr)
            local_pieces = true;
    });
    if (joiner && local_pieces) {
        std::vector<float> native;
        DoorPhaseTable unused_doors;
        build_world_present_rows(context, lifecycle, native, unused_doors);
        for (size_t offset = 0; offset < native.size(); offset += PF_STRIDE) {
            const auto handle = EntityHandle{uint16_t(native[offset + PF_WIRE_HANDLE])};
            const Entity *entity = kernel.world.registry.get(handle);
            if (entity && entity->item_section_piece && cs.find(handle.packed) == nullptr)
                out.insert(out.end(), native.begin() + offset, native.begin() + offset + PF_STRIDE);
        }
    }
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
	// from the pools [orig: collect_visible_entities_for_terrain @0x5c8c60
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
		float *r = rows + static_cast<size_t>(row_index) * PF_STRIDE;
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
		r[PF_PITCH_DEG] = static_cast<float>(e.pitch);
		r[PF_YAW_DEG] = static_cast<float>(pool_present_yaw_deg(e, ae, cls));
		r[PF_ROLL_DEG] = static_cast<float>(e.roll);
		// The decoded fold bumps a row's respawn revision on every dead->alive
		// edge of its wire state byte (organic bit 1; vehicle wrecks flag 4).
		// Mirror that edge from the authoritative flags so WirePresentPass
		// re-seeds the same way on the host.
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
		r[PF_HIDDEN] = e.hidden ? 1.0f : 0.0f;
		r[PF_ALIVE] = e.alive ? 1.0f : 0.0f;
        r[PF_HUSK] = (e.engine_flags & kEntityFlagHusk) ? 1.0f : 0.0f;
        for (int phase = 0; phase < 6; ++phase)
            r[PF_OBJECT_DESTROY + phase] = float(e.destroy_phases_q16[phase]);
		// The authority owns the exact MoveOrder stance latch (bits 8/9 of
		// Player_PackInputStateToEntity @0x4df450; the MATCHTERRAIN tier reads them at
		// Terrain_RenderSectorEntitiesBySide @0x5c7dc2..0x5c7ded - docs/foliage/foliage-re.md).
		r[PF_STANCE_BITS] = static_cast<float>(e.net_stance_bits & 0x03u);
		write_present_section_mask(r, item_hidden_sections(e));
		write_present_doors(r, row_index, w, e, door_phases);
		r[PF_RIGHT_HAND_COLLAPSED] =
				simassets::mount_collapses_right_hand_row(e) ? 1.0f : 0.0f;
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
		// Two retail callbacks write this three-register family. The sector
		// renderer publishes TEX_TEAM for every placed pool-1/2/3 model that
		// reaches its model callback. The generic-world callback publishes the
		// same TEX_TEAM plus TEAMSWING for a nonzero packed zone byte, and writes
		// LFP only when the client-side shared timer-list entry exists.
		// [orig: render_sector_entity @0x5C424F..0x5C425F;
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
		r[PF_ANIM_STATE] = static_cast<float>(ae->inf.anim_state);
		r[PF_ANIM_PHASE_TICKS] = static_cast<float>(ae->inf.clip_phase);
		if (ae->inf.body_blend_active()) {
			r[PF_ANIM_SOURCE_STATE] = static_cast<float>(ae->inf.anim_prev);
			r[PF_ANIM_SOURCE_PHASE_TICKS] =
					static_cast<float>(ae->inf.anim_prev_clip_phase);
			r[PF_ANIM_BLEND_WEIGHT] = ae->inf.anim_blend_weight;
		}
		// The upper-body weapon channel this body derived for itself; the gate
		// is the §14.8.6 consumer test and engine_flags bit 0x100 is the "is a
		// player" mirror of entity+0x24 (NPCs carry no hold ladder).
		if (infantry_weapon_channel_visible(
					ae->inf, (e.engine_flags & kEntityFlagPlayer) != 0,
					simassets::mount_blocks_weapon_channel(e))) {
			r[PF_WPN_ANIM_STATE] = static_cast<float>(ae->inf.wpn_state);
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
		const anim::AimOverlayInputs inputs = simassets::aim_overlay_inputs_for(*ae, e);
		anim::AimOverlayAngles angles[anim::kOverlayClassCount];
		anim::compute_aim_overlay_angles(inputs, angles);
		write_present_overlay(r, angles);
		// This body's third-person gun. Player rows only: retail's composition
		// gate is the Flags 0x100 player classifier.
		if ((e.engine_flags & kEntityFlagPlayer) != 0) {
			write_present_held_weapon(
					r, e.equipped_adm_index, (e.flags & kEntityFlagDead) != 0, inputs,
					ae->inf.wpn_state);
		}
	});
}

} // namespace opennova::inmatch
