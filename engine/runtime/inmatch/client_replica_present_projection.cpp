#include <runtime/inmatch/client_replica_present_projection.h>

#include <base/io/fixed.h>
#include <runtime/inmatch/client_replica_present.h>
#include <runtime/world/entity.h>
#include <runtime/world/person_overlays.h>
#include <runtime/world/script_voice.h>

#include <algorithm>

namespace opennova::inmatch {

namespace {
constexpr double kFixed16 = io::kFp16OneD;

// Whether this tick's primary pose samples the armed park: the unblended
// channel holding a deferred state sits on the boundary its arm parked it on,
// where the clip reads its last frame, not the wrapped start (the row's twin of
// InfantryState::body_phase_parked).
// [orig: AnimChannel_AdvancePlayback @0x40B140 (the armed wrap @0x40B19E..0x40B1B1)]
bool row_primary_parked(const replication::ClientEntityState &entity) {
	return entity.rm_blend_weight >= 1.0f && entity.net_anim_pending != 0 &&
			entity.net_anim_pending_boundary >= 0 &&
			entity.net_anim_pending_boundary != world::kEndNotifyNeverLatches &&
			entity.rm_phase == entity.net_anim_pending_boundary;
}
} // namespace

void initialize_client_replica_present_row(float *row) {
	std::fill(row, row + world::PF_STRIDE, 0.0f);
	row[world::PF_KIND] = -1.0f;
	row[world::PF_INDEX] = -1.0f;
	row[world::PF_BODY_ANIM_SLOT] = -1.0f;
	row[world::PF_ANIM_STATE] = -1.0f;
	row[world::PF_ANIM_PHASE_TICKS] = -1.0f;
	row[world::PF_ANIM_SOURCE_STATE] = -1.0f;
	row[world::PF_ANIM_SOURCE_PHASE_TICKS] = -1.0f;
	row[world::PF_ANIM_BLEND_WEIGHT] = 1.0f;
	row[world::PF_ANIM_STATE_PULSE] = -1.0f;
	row[world::PF_ANIM_PULSE_TICKS] = -1.0f;
	row[world::PF_WPN_ANIM_STATE] = -1.0f;
	row[world::PF_WPN_PHASE_TICKS] = -1.0f;
	row[world::PF_WPN_SOURCE_STATE] = -1.0f;
	row[world::PF_WPN_SOURCE_PHASE_TICKS] = -1.0f;
	row[world::PF_WPN_BLEND_WEIGHT] = 1.0f;
	row[world::PF_WPN_VARIANT] = 0.0f;
	row[world::PF_WPN_SOURCE_VARIANT] = 0.0f;
	row[world::PF_CARRIER_HANDLE] = -1.0f;
	row[world::PF_SLOT_GROUND_HANDLE] = -1.0f;
	row[world::PF_SLOT_SEAT_WALK_HANDLE] = -1.0f;
	row[world::PF_ALIVE] = 1.0f;
}

void project_client_replica_present_row(
		float *row,
		const replication::ClientEntityState &entity,
		const replication::ClientState &state,
		const ClientReplicaPresentContext &context) {
	namespace ns = replication;

	row[world::PF_TYPE_ID] = static_cast<float>(entity.type_id);
	row[world::PF_WIRE_HANDLE] = static_cast<float>(entity.handle);
	// The groundEntity/mount link the on-entity footstep slot reads (the seat
	// wins over the ground link, as the authority rows publish it); 0xFFFF
	// (no link) publishes as -1 so presentation reads one sentinel.
	const uint16_t ground_link = entity.carrier_handle != 0xFFFFu
			? entity.carrier_handle : entity.resolved_ground;
	row[world::PF_CARRIER_HANDLE] = ground_link != 0xFFFFu
			? static_cast<float>(ground_link)
			: -1.0f;
	if (entity.cls == EntityClass::Player) {
		// A player's wire net_id IS its packed character id (entity+0x15C).
		row[world::PF_CHARACTER_ID] = static_cast<float>(entity.net_id);
		const uint32_t anim_flags =
				world::infantry_anim_flags(entity.anim_state_id);
		const uint32_t stance_bits =
				((anim_flags & world::kAnimStanceFlagProne) != 0 ? 1u : 0u) |
				((anim_flags & world::kAnimStanceFlagCrouched) != 0 ? 2u : 0u);
		row[world::PF_STANCE_BITS] = static_cast<float>(stance_bits);
	}
	if (entity.cls == EntityClass::Player || entity.cls == EntityClass::Infantry) {
		row[world::PF_PARACHUTE_DEPLOYED] =
				(entity.rm_entity_flags & world::kEntityFlagParachute) != 0 ? 1.0f : 0.0f;
	}
	// Decoded wire position is mission (x,y,z) 16.16 -> present (x, z, -y)
	// world units, the SAME remap the AI-pool present uses. On a joiner the
	// row's live pose is chased between records by tick_remote_motion (net-re
	// §5.38e, D-NET-196); snap-mode folds (host/SP loopback, spectate before
	// the movers arm) re-seed it per record — either way exactly what retail
	// renders for decoded peers (post-compression, lossy).
	row[world::PF_POS_X] = static_cast<float>(entity.x / kFixed16);
	row[world::PF_POS_Y] = static_cast<float>(entity.z / kFixed16);
	row[world::PF_POS_Z] = static_cast<float>(-entity.y / kFixed16);
	// Euler X/Y live in the same BAM32 entity fields as the authoritative
	// pose. Spawn/dead-pose records seed them; the joiner prediction mirror
	// advances air/water attitude between records. Role bindings override
	// afterwards where a better source exists (the host's authoritative
	// registry attitude, attachment poses). Every non-person row draws from its
	// full euler, a pitched or rolled building or object included; a person's
	// pitch is its look, which the body's channels take, not the root.
	// [orig: BoneCallback_bldg_World @0x4E21A0 - the matrix rebuilt from
	//  Position and the eulers @0x4E21BA..0x4E220C: Math_BuildFixedPointMatrixFromEulerAngles
	//  @0x613F40 (the call @0x4E220C), the scaled builder
	//  Math_BuildFixedPointRotationMatrixFromEulerAnglesAndScale @0x614210]
	if (entity.cls != EntityClass::Player && entity.cls != EntityClass::Infantry) {
		row[world::PF_PITCH_DEG] = static_cast<float>(
				double(entity.pitch_bam) * world::kDegreesPerBam);
		row[world::PF_ROLL_DEG] = static_cast<float>(
				double(entity.roll_bam) * world::kDegreesPerBam);
	}
	// The row's live engine-BAM heading (chased on a joiner; seeded from the
	// wire byte/euler in snap folds, with sub-byte body effects such as recoil
	// applied on top) -> mission yaw (90 - heading), matching the AI-pool
	// present. Vehicles keep the wire's full 16-bit euler precision this way
	// (yaw_byte would re-truncate it).
	row[world::PF_YAW_DEG] = static_cast<float>(
			world::mission_yaw_deg_from_bam_heading(entity.heading_bam));
	row[world::PF_RESPAWN_REVISION] =
			static_cast<float>(entity.respawn_revision);

	if (entity.state_flags_known) {
		// bit0 = "not independently collected/moved" — the retail
		// visible-entity collector skips such rows [orig: @0x5C8CF4]. Seat
		// mounts never stream it (they carry 0x40), and retail draws
		// carrier-ATTACHED children regardless of Flags [orig: the
		// no-flag-test child draws @0x5D795A/@0x5D79C8] — so a row riding a
		// live carrier attach stays visible even with bit0.
		const bool attached = entity.net_seat_valid &&
				entity.carrier_handle != 0xFFFFu;
		row[world::PF_HIDDEN] =
				!attached && (entity.state_flags & 0x01u) != 0u
				? 1.0f : 0.0f;
		// Vehicles mark the wreck with the dead-pose bit, not the organic
		// bit 1 [orig: the §5.13 short-form select; decoder is_dead_pose =
		// (flags_byte & 4)].
		const uint8_t dead_bit = entity.cls == EntityClass::Vehicle
				? ns::kVehicleFlagDeadPose
				: static_cast<uint8_t>(world::kEntityFlagDead);
		row[world::PF_ALIVE] =
				(entity.state_flags & dead_bit) == 0u ? 1.0f : 0.0f;
	}

	const uint8_t pool = static_cast<uint8_t>(entity.handle >> 12);
	const bool zone = entity.zone_number_rank != 0;
	if ((pool >= 1 && pool <= 3) || zone) {
		row[world::PF_TEX_TEAM_VALID] = 1.0f;
		row[world::PF_TEX_TEAM] = entity.team < 0x80u
				? static_cast<float>(entity.team)
				: static_cast<float>(static_cast<int>(entity.team) - 0x100);
	}
	if (zone) {
		row[world::PF_ZONE_CTRL_VALID] = 1.0f;
		row[world::PF_TEAMSWING] = entity.team == 1u
				? 0.0f : (entity.team == 2u ? 65536.0f : 32768.0f);
	}

	if (!context.project_remote_appearance) return;
	static const std::vector<mission::ItemSeatSpec> kNoSeatSpecs;
	const auto &seat_specs = context.item_seat_specs != nullptr
			? *context.item_seat_specs : kNoSeatSpecs;

	world::EmplacedWeaponControls emplaced;
	if (emplaced_weapon_controls_for_client(entity, emplaced))
		write_present_emplaced_controls(row, emplaced);

	anim::AimOverlayInputs inputs;
	bool blocking_seat = false;
	if (!aim_overlay_inputs_for_client(
				entity, state, seat_specs, inputs, &blocking_seat))
		return;
	// The speaking body's head kick and TALK register off the mixer meter: a joiner's
	// decoded speaker is named by its row's handle (ScriptVoiceChannel::entity_set).
	if (context.world != nullptr) {
		const world::EntityHandle body{entity.handle};
		inputs.speaking_kick = world::speaking_head_kick(*context.world, body);
		row[world::PF_TALK_CTRL] =
				static_cast<float>(world::talk_ctrl_register_value(*context.world, body));
	}
	// The collapsed right-hand row: a blocking seat without the Player bit.
	// [orig: Entity_BuildBoneTransformMatrices @0x4B1290 - the special row @0x4B1F84..0x4B1F92]
	const bool collapse_right_hand = entity.cls == EntityClass::Infantry && blocking_seat;

	// An armed row presents the same simulation-owned primary channel whose
	// root delta moves the replica, a corpse's death clip included (the mover's
	// death edge commits it). Frozen (bit0) or unresolved rows, and every carried
	// row but a seated body's, are disarmed by the fold and fall back to the
	// latest wire state. A seated row, a player's or an org1 body's, presents the
	// channel its pass runs at its head (advance_seated_channel, D-NET-330), as an
	// authoritative body presents its own (present_rows.cpp write_present_body): the
	// bone build samples the body's primary channel, seated or not.
	// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the dual update @0x4B41DF
	//  ahead of the mounted site @0x4B637C..0x4B638F; Entity_UpdateInfantryAI @0x4B9910 -
	//  the dual update @0x4B9A48 ahead of the seat branch @0x4BE8F7;
	//  Entity_BuildBoneTransformMatrices @0x4B1290 - the primary channel @0x4B1355,
	//  AnimChannel_ComputeBoneMatrices called @0x4B1388]
	const bool root_motion_armed = entity.rm_adm_id >= 0 && entity.rm_state >= 0;
	if (root_motion_armed) {
		// The channel already ran the per-record receive arbitration + the
		// deferred promotion in the fold/tick (D-NET-209), so it presents
		// DIRECTLY in the host-loopback tuple shape: current + playhead +
		// the blending source pair. The model-side remote FSM is bypassed.
		row[world::PF_ANIM_STATE] = static_cast<float>(entity.rm_state);
		row[world::PF_ANIM_REMOTE_REQUEST] = 0.0f;
		row[world::PF_ANIM_PHASE_TICKS] = static_cast<float>(entity.rm_phase);
		// The served ring entry each half plays (D-NET-196), and the armed park,
		// as an authoritative body presents them (present_rows.cpp
		// write_present_body).
		row[world::PF_ANIM_VARIANT] = static_cast<float>(entity.rm_variant);
		row[world::PF_ANIM_PHASE_PARKED] = row_primary_parked(entity) ? 1.0f : 0.0f;
		if (entity.rm_blend_weight < 1.0f && entity.rm_prev_state >= 0) {
			row[world::PF_ANIM_SOURCE_STATE] =
					static_cast<float>(entity.rm_prev_state);
			row[world::PF_ANIM_SOURCE_PHASE_TICKS] =
					static_cast<float>(entity.rm_prev_phase);
			row[world::PF_ANIM_BLEND_WEIGHT] = entity.rm_blend_weight;
			row[world::PF_ANIM_SOURCE_VARIANT] =
					static_cast<float>(entity.rm_prev_variant);
		}
	} else {
		row[world::PF_ANIM_STATE] = static_cast<float>(
				root_motion_armed ? entity.rm_state : entity.anim_state_id);
		row[world::PF_ANIM_REMOTE_REQUEST] = 1.0f;
		if (root_motion_armed) {
			row[world::PF_ANIM_PHASE_TICKS] =
					static_cast<float>(entity.rm_phase);
		}
		// The armed simulation channel owns transition arbitration.
		// Dispatching the free-running wire pulse as well would race two
		// paths on the same model.
		if (!root_motion_armed && entity.anim_state_pulse >= 0) {
			row[world::PF_ANIM_STATE_PULSE] =
					static_cast<float>(entity.anim_state_pulse);
			if (entity.cls == EntityClass::Player) {
				row[world::PF_ANIM_PULSE_TICKS] =
						static_cast<float>(entity.anim_pulse_ratio);
			}
		}
		if (!root_motion_armed && entity.cls == EntityClass::Player) {
			row[world::PF_ANIM_PHASE_TICKS] =
					static_cast<float>(entity.anim_channel_ratio);
		}
	}
	row[world::PF_RIGHT_HAND_COLLAPSED] =
			collapse_right_hand ? 1.0f : 0.0f;

	int weapon_hold_state = -1;
	if (entity.cls == EntityClass::Player) {
		int hold_kind = 0;
		if (context.weapons != nullptr) {
			if (const world::WeaponTableEntry *held =
						context.weapons->by_index(entity.equipped_adm_index))
				hold_kind = held->special_hold;
		}
		weapon_hold_state = world::infantry_weapon_hold_state(
				hold_kind, entity.anim_state_id,
				(entity.state_flags & world::kEntityFlagScopeRaised) != 0,
				(entity.state_flags & world::kEntityFlagBinoculars) != 0,
				/*reloading=*/false);
		// An armed row's channel holds the +0x2C8 target the hand-frame
		// selector reads (write_present_held_weapon).
		if (entity.wpn_playing >= 0) weapon_hold_state = entity.wpn_state;
		// The splice: the Player bit, no blocking seat, flag 0x40 on the requested state
		// [orig: Entity_BuildBoneTransformMatrices @0x4B1290 - @0x4B14A7 / @0x4B14B9 /
		//  @0x4B14C9].
		if (!blocking_seat &&
				(world::infantry_anim_flags(entity.anim_state_id) &
				 0x40u) != 0) {
			if (entity.wpn_playing >= 0) {
				// The row's own secondary channel (armed with a root-motion
				// source): its playing state, playhead and blend, as the
				// authority presents a body's (present_rows.cpp).
				row[world::PF_WPN_ANIM_STATE] = static_cast<float>(entity.wpn_playing);
				row[world::PF_WPN_PHASE_TICKS] = static_cast<float>(entity.wpn_phase);
				row[world::PF_WPN_VARIANT] = static_cast<float>(entity.wpn_variant);
				row[world::PF_WPN_PHASE_PARKED] =
						entity.wpn_blend_weight >= 1.0f && entity.wpn_deferred != 0 &&
								entity.wpn_deferred_boundary >= 0 &&
								entity.wpn_deferred_boundary != world::kEndNotifyNeverLatches &&
								entity.wpn_phase == entity.wpn_deferred_boundary
						? 1.0f : 0.0f;
				if (entity.wpn_blend_weight < 1.0f && entity.wpn_prev >= 0) {
					row[world::PF_WPN_SOURCE_STATE] = static_cast<float>(entity.wpn_prev);
					row[world::PF_WPN_SOURCE_PHASE_TICKS] =
							static_cast<float>(entity.wpn_prev_phase);
					row[world::PF_WPN_BLEND_WEIGHT] = entity.wpn_blend_weight;
					row[world::PF_WPN_SOURCE_VARIANT] =
							static_cast<float>(entity.wpn_prev_variant);
				}
			} else {
				// Unarmed (no root-motion source): the ladder's pose at frame 0.
				row[world::PF_WPN_ANIM_STATE] =
						static_cast<float>(weapon_hold_state);
				row[world::PF_WPN_PHASE_TICKS] = -1.0f;
			}
		}
	}

	anim::AimOverlayAngles angles[anim::kOverlayClassCount];
	anim::compute_aim_overlay_angles(inputs, angles);
	write_present_overlay(row, angles);
	if (entity.cls == EntityClass::Player) {
		write_present_held_weapon(
				row, entity.equipped_adm_index,
				(entity.state_flags & world::kEntityFlagDead) != 0,
				inputs, weapon_hold_state);
	}
	// The body's item overlays from the decoded state: the wire flags byte
	// (entity+0x24's low byte; bits 2-4 are the sender's own view flags), the
	// replica's canopy words and the carry relation.
	// [orig: BoneCallback_org0_World @0x4e3940]
	world::PersonOverlayInputs overlay_inputs;
	overlay_inputs.flags = entity.state_flags;
	overlay_inputs.pose = inputs;
	overlay_inputs.chute = entity.rm_parachute;
	if (context.carried_types != nullptr) {
		const auto it = context.carried_types->find(entity.handle);
		if (it != context.carried_types->end()) overlay_inputs.carried_type_id = it->second;
	}
	world::write_present_person_overlays(row, world::person_overlays(overlay_inputs));
}

bool skeletal_pose_inputs_for_client(
		const replication::ClientEntityState &entity,
		const replication::ClientState &state,
		const ClientReplicaPresentContext &context,
		world::SkeletalPoseInputs &out) {
	out = world::SkeletalPoseInputs{};
	static const std::vector<mission::ItemSeatSpec> kNoSeatSpecs;
	const auto &seat_specs = context.item_seat_specs != nullptr
			? *context.item_seat_specs : kNoSeatSpecs;
	bool blocking_seat = false;
	if (!aim_overlay_inputs_for_client(
				entity, state, seat_specs, out.overlay, &blocking_seat))
		return false;
	if (context.world != nullptr)
		out.overlay.speaking_kick =
				world::speaking_head_kick(*context.world, world::EntityHandle{entity.handle});
	out.collapse_right_hand = entity.cls == EntityClass::Infantry && blocking_seat;
	// The projection's armed branch: the row's own primary channel, current,
	// playhead, served ring entry, armed park and the blending source pair; a
	// seated player row's included (D-NET-330).
	if (entity.rm_adm_id < 0 || entity.rm_state < 0)
		return false;
	out.primary_state = entity.rm_state;
	out.primary_phase = entity.rm_phase;
	out.primary_variant = entity.rm_variant;
	out.primary_parked = row_primary_parked(entity);
	if (entity.rm_blend_weight < 1.0f && entity.rm_prev_state >= 0) {
		out.blend = true;
		out.source_state = entity.rm_prev_state;
		out.source_phase = entity.rm_prev_phase;
		out.source_variant = entity.rm_prev_variant;
		out.blend_weight = entity.rm_blend_weight;
	}
	// A player row's secondary channel, where the projection presents one: the Player
	// bit, no blocking seat, flag 0x40 on the requested state [orig:
	// Entity_BuildBoneTransformMatrices @0x4B1290 - @0x4B14A7 / @0x4B14B9 / @0x4B14C9].
	if (entity.cls != EntityClass::Player || blocking_seat ||
			(world::infantry_anim_flags(entity.anim_state_id) & 0x40u) == 0)
		return true;
	out.weapon = true;
	if (entity.wpn_playing >= 0) {
		out.weapon_state = entity.wpn_playing;
		out.weapon_phase = entity.wpn_phase;
		out.weapon_variant = entity.wpn_variant;
		out.weapon_parked = entity.wpn_blend_weight >= 1.0f && entity.wpn_deferred != 0 &&
				entity.wpn_deferred_boundary >= 0 &&
				entity.wpn_deferred_boundary != world::kEndNotifyNeverLatches &&
				entity.wpn_phase == entity.wpn_deferred_boundary;
		if (entity.wpn_blend_weight < 1.0f && entity.wpn_prev >= 0) {
			out.weapon_blend = true;
			out.weapon_prev_state = entity.wpn_prev;
			out.weapon_prev_phase = entity.wpn_prev_phase;
			out.weapon_prev_variant = entity.wpn_prev_variant;
			out.weapon_blend_weight = entity.wpn_blend_weight;
		}
		return true;
	}
	// Unarmed: the hold ladder's pose at frame 0.
	int hold_kind = 0;
	if (context.weapons != nullptr) {
		if (const world::WeaponTableEntry *held =
					context.weapons->by_index(entity.equipped_adm_index))
			hold_kind = held->special_hold;
	}
	out.weapon_state = world::infantry_weapon_hold_state(
			hold_kind, entity.anim_state_id,
			(entity.state_flags & world::kEntityFlagScopeRaised) != 0,
			(entity.state_flags & world::kEntityFlagBinoculars) != 0,
			/*reloading=*/false);
	out.weapon_phase = 0;
	return true;
}

std::unordered_map<uint16_t, uint16_t> carried_object_types_by_carrier(
		const replication::ClientState &state) {
	std::unordered_map<uint16_t, uint16_t> out;
	for (const replication::ClientEntityState &row : state.entities) {
		if (row.type_id != 4091 && row.type_id != 4093 && row.type_id != 4095) continue;
		if (row.parent_handle == wire_handle::kInvalid) continue;
		out[row.parent_handle] = row.type_id;
	}
	return out;
}

} // namespace opennova::inmatch
