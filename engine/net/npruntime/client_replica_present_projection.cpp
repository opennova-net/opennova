#include "npruntime/client_replica_present_projection.h"

#include "npruntime/client_replica_present.h"

#include <algorithm>

namespace opennova::np {

namespace {
constexpr double kFixed16 = 65536.0;
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
	row[world::PF_ALIVE] = 1.0f;
}

void project_client_replica_present_row(
		float *row,
		const netsim::ClientEntityState &entity,
		const netsim::ClientState &state,
		const ClientReplicaPresentContext &context) {
	namespace ns = netsim;

	row[world::PF_TYPE_ID] = static_cast<float>(entity.type_id);
	row[world::PF_WIRE_HANDLE] = static_cast<float>(entity.handle);
	// The decoded groundEntity link, for the on-entity footstep slot. 0xFFFF
	// (no link) publishes as -1 so presentation reads one sentinel.
	row[world::PF_CARRIER_HANDLE] = entity.carrier_handle != 0xFFFFu
			? static_cast<float>(entity.carrier_handle)
			: -1.0f;
	if (entity.cls == EntityClass::Player) {
		// A player's wire net_id IS its packed character id (entity+0x15C).
		row[world::PF_CHARACTER_ID] = static_cast<float>(entity.net_id);
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
	// Vehicle euler X/Y live in the same BAM32 entity fields as the
	// authoritative pose. Spawn/dead-pose records seed them; the joiner
	// prediction mirror advances air/water attitude between records. Role
	// adapters override afterwards where a better source exists (the host's
	// authoritative registry attitude, attachment poses).
	if (entity.cls == EntityClass::Vehicle) {
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
	if (emplaced_weapon_controls_for_client(entity, state, seat_specs, emplaced))
		write_present_emplaced_controls(row, emplaced);

	anim::AimOverlayInputs inputs;
	bool collapse_right_hand = false;
	if (!aim_overlay_inputs_for_client(
				entity, state, seat_specs, inputs, &collapse_right_hand))
		return;

	// An armed row presents the same simulation-owned primary channel whose
	// root delta moves the replica. Frozen/unresolved rows are disarmed by the
	// fold and fall back to the latest wire state, including death/seat clips
	// (the retail +0x2C0 park's visible outcome).
	const bool root_motion_armed =
			entity.rm_adm_id >= 0 && entity.rm_state >= 0;
	if (root_motion_armed) {
		// The channel already ran the per-record receive arbitration + the
		// deferred promotion in the fold/tick (D-NET-209), so it presents
		// DIRECTLY in the host-loopback tuple shape: current + playhead +
		// the blending source pair. The model-side remote FSM is bypassed.
		row[world::PF_ANIM_STATE] = static_cast<float>(entity.rm_state);
		row[world::PF_ANIM_REMOTE_REQUEST] = 0.0f;
		row[world::PF_ANIM_PHASE_TICKS] = static_cast<float>(entity.rm_phase);
		if (entity.rm_blend_weight < 1.0f && entity.rm_prev_state >= 0) {
			row[world::PF_ANIM_SOURCE_STATE] =
					static_cast<float>(entity.rm_prev_state);
			row[world::PF_ANIM_SOURCE_PHASE_TICKS] =
					static_cast<float>(entity.rm_prev_phase);
			row[world::PF_ANIM_BLEND_WEIGHT] = entity.rm_blend_weight;
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
		if (!collapse_right_hand &&
				(world::infantry_anim_flags(entity.anim_state_id) &
				 0x40u) != 0) {
			row[world::PF_WPN_ANIM_STATE] =
					static_cast<float>(weapon_hold_state);
			row[world::PF_WPN_PHASE_TICKS] = -1.0f;
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
}

} // namespace opennova::np
