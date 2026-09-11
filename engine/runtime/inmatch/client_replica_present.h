// Client-replica present composition: the engine-typed helpers that turn one
// decoded ClientEntityState (plus seat/weapon definitions) into packed
// world::PF_* present-row fields — the mounted/emplaced turret phases, the
// remote aim-overlay inputs, and the third-person held-weapon frame. Shared
// by the client-replica projection (this lib) and the shell binding's role
// enrichers. Moved from the binding's simulation family (ADR 0031); the
// witness citations travel with each body.
#pragma once

#include <runtime/inmatch/replica_query.h> // client_entity_for_handle

#include <runtime/replication/client_state.h>
#include <net/npwire/entity_class.h>

#include <runtime/anim/aim_overlay.h>
#include <base/io/bam.h>
#include <runtime/mission/promote.h>
#include <runtime/simassets/pose_inputs.h>
#include <runtime/simassets/seat_spec_extract.h>
#include <runtime/world/angle.h>
#include <runtime/world/entity.h>
#include <runtime/world/infantry.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/present_rows.h>
#include <runtime/world/turret_window.h>

#include <cstdint>
#include <vector>

namespace opennova::inmatch {

// Mission-space euler degrees composed from one overlay angle triple — the
// (pitch, yaw, roll) form every PF_*_DEG field carries. Engine heading is
// (90 - mission yaw); kDegreesPerBam scales the raw BAM terms.
struct MissionEulerDeg {
	float pitch = 0.0f;
	float yaw = 0.0f;
	float roll = 0.0f;
};

inline MissionEulerDeg mission_euler_from_overlay(
		const anim::AimOverlayAngles &angles) {
	MissionEulerDeg out;
	out.pitch = static_cast<float>(
			static_cast<double>(angles.pitch) * world::kDegreesPerBam);
	out.yaw = static_cast<float>(
			world::mission_yaw_deg_from_bam_heading(angles.yaw));
	out.roll = static_cast<float>(
			static_cast<double>(angles.roll) * world::kDegreesPerBam);
	return out;
}

inline bool emplaced_weapon_controls_for_client(
		const replication::ClientEntityState &mount,
		const replication::ClientState &state,
		const std::vector<mission::ItemSeatSpec> &specs,
		world::EmplacedWeaponControls &out) {
	out = world::EmplacedWeaponControls{};
	const mission::ItemSeatSpec *spec =
			simassets::item_seat_spec_for_type(specs, mount.type_id);
	if (spec == nullptr) return false;

	for (const replication::ClientEntityState &occupant : state.entities) {
		if (occupant.carrier_handle != mount.handle ||
				occupant.mount_bone == 0 ||
				(occupant.cls != EntityClass::Player &&
				 occupant.cls != EntityClass::Infantry))
			continue;
		const world::Seat *seat = nullptr;
		for (const world::Seat &candidate : spec->seats) {
			if (candidate.bone_index == occupant.mount_bone) {
				seat = &candidate;
				break;
			}
		}
		if (seat == nullptr || seat->type != world::SeatType::Gunner)
			continue;

		// ClientReplicaPipeline has already composed mounted yaw into world heading and
		// reconstructed an infantry gunner's live entity pitch from the compact
		// aim target using retail's one-eighth chase. Root headings read the
		// row's live BAM (chased on a joiner — §5.38e; full euler precision for
		// vehicles).
		const int32_t parent_heading = mount.heading_bam;
		const int32_t occupant_heading = occupant.heading_bam;
		const int32_t occupant_pitch =
				occupant.cls == EntityClass::Player
				? static_cast<int32_t>(
						static_cast<uint32_t>(occupant.pitch_byte) << 24)
				: occupant.pitch_bam;
		// The immediate producer of the gun words, over the replica: yaw =
		// gun - occupant; pitch = gun - occupant.recoilPitch - occupant, the
		// recoil being the row's reconstructed entity+0x380 (stamped from the
		// received round event, decayed by the local body pass). The replica
		// carries neither the stored +0x322/+0x324 words nor the mount's
		// ItemDefAttrib2, so the IsTurret slew/tether leg and the window
		// clamp's occupant write-back (both authority-side in
		// world/mount_controls.h) are not mirrored here: a joiner presents a
		// remote IsTurret turret at the occupant's aim, not the slewed word.
		// [orig: Entity_UpdateChildAttachment @0x4409A0 immediate path
		//  @0x440b39..0x440b58 — `sub ecx,[edx+380h]` @0x440b4c]
		int32_t yaw_delta = io::bam_sub(parent_heading, occupant_heading);
		int32_t pitch_delta = io::bam_sub(
				io::bam_sub(mount.pitch_bam, occupant.recoil_pitch), occupant_pitch);
		// The per-seat authored window wins first — the CARRIER's addeweap arc
		// for this attach point, matched by the mount row's streamed carrier +
		// bone (the same key the seat match above uses). Any nonzero value
		// selects the quartet verbatim; all-zero falls to the weapon-def
		// window below. [orig: Entity_GetWeaponTurretLimits @0x540d70 per-seat
		// leg @0x540db5..0x540e27 — carrierDef+540/556/572/588[seat]]
		const mission::ItemEmplacementAttachmentSpec *arc = nullptr;
		if (mount.carrier_handle != world::EntityHandle::kInvalid) {
			for (const replication::ClientEntityState &carrier : state.entities) {
				if (carrier.handle != mount.carrier_handle) continue;
				if (const mission::ItemSeatSpec *carrier_spec =
						simassets::item_seat_spec_for_type(specs, carrier.type_id)) {
					for (const mission::ItemEmplacementAttachmentSpec
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
		// [orig: the per-update clamp @0x441228..0x44128c via Math_ClampAngleToBounds]
		const world::TurretWindow window =
				world::select_turret_window_bam(
						arc != nullptr ? arc->down_limit_bam : 0,
						arc != nullptr ? arc->up_limit_bam : 0,
						arc != nullptr ? arc->right_limit_bam : 0,
						arc != nullptr ? arc->left_limit_bam : 0,
						spec->turret_yaw_range_bam,
						spec->turret_pitch_max_bam,
						spec->turret_pitch_min_bam);
		if (window.per_seat) {
			world::emplaced_clamp_turret_bam(yaw_delta, window.yaw_upper,
					window.yaw_lower);
			world::emplaced_clamp_turret_bam(pitch_delta, window.pitch_upper,
					window.pitch_lower);
		} else {
			// weapon.def fallback (stamped onto the spec from the primary
			// weapon's rows).
			if (window.yaw_upper != 0)
				world::emplaced_clamp_turret_bam(yaw_delta, window.yaw_upper,
						window.yaw_lower);
			if (window.pitch_upper != 0 || window.pitch_lower != 0)
				world::emplaced_clamp_turret_bam(pitch_delta,
						window.pitch_upper, window.pitch_lower);
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
		float *record, const world::EmplacedWeaponControls &controls) {
	if (!controls.valid) return;
	record[world::PF_EMPLACED_CONTROLS_VALID] = 1.0f;
	record[world::PF_EWEAP_GUNYAW] = static_cast<float>(controls.gun_yaw);
	record[world::PF_EWEAP_GUNPITCH] = static_cast<float>(controls.gun_pitch);
}

inline bool aim_overlay_inputs_for_client(
		const replication::ClientEntityState &entity,
		const replication::ClientState &state,
		const std::vector<mission::ItemSeatSpec> &specs,
		anim::AimOverlayInputs &out,
		bool *r_collapse_right_hand = nullptr) {
	if (r_collapse_right_hand != nullptr) *r_collapse_right_hand = false;
	if (entity.cls != EntityClass::Player &&
			entity.cls != EntityClass::Infantry)
		return false;

	out = anim::AimOverlayInputs{};
	out.aim_yaw = entity.heading_bam;
	// Free-standing players read the row's live pitch (the org2 body chase
	// steps it toward the wire byte with divisor 12 on a joiner; snap folds
	// mirror the byte — §5.38e §3). Mounted players and infantry keep the raw
	// wire aim byte: their own mover is bit0-skipped and the seat pose owns the
	// body [orig: the mounted read of entity+0x2D0-desired aim].
	const bool free_standing_player =
			entity.cls == EntityClass::Player &&
			entity.carrier_handle == world::EntityHandle::kInvalid;
	out.aim_pitch = free_standing_player
			? entity.pitch_bam
			: static_cast<int32_t>(
					static_cast<uint32_t>(
							entity.cls == EntityClass::Player
									? entity.pitch_byte
									: entity.aim_yaw_byte)
					<< 24);
	out.body_yaw = out.aim_yaw;
	out.leg_yaw_r = out.body_yaw;
	out.leg_yaw_l = out.body_yaw;
	out.aim_state =
			(world::infantry_anim_flags(entity.anim_state_id) & 0x40u) != 0;
	out.rolling =
			entity.anim_state_id == world::anim_state::kRollLeft ||
			entity.anim_state_id == world::anim_state::kRollRight;
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
	if (entity.carrier_handle == world::EntityHandle::kInvalid ||
			entity.mount_bone == 0) return true;
	const replication::ClientEntityState *carrier =
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
	const mission::ItemSeatSpec *spec =
			simassets::item_seat_spec_for_type(specs, carrier->type_id);
	const world::Seat *seat = nullptr;
	if (spec != nullptr) {
		for (const world::Seat &candidate : spec->seats) {
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
				entity.cls == EntityClass::Infantry &&
				simassets::seat_type_blocks_weapon_channel(seat->type);
	}

	out.mount_mode = simassets::mount_mode_for_seat_type(seat->type);
	if (out.mount_mode == anim::MountMode::OnFoot) return true;
	out.mount_config_valid = spec->mount_config_valid;
	out.mount_config = spec->mount_config_valid ? spec->mount_config : 0;

	// pose_mounted_occupant faces seated slots at carrier+yaw_offset and a
	// Gunner at carrier-yaw_offset in mission yaw. Engine heading is
	// (90-mission yaw), so those signs invert here. The carrier root heading is
	// the row's live BAM (chased on a joiner — §5.38e).
	const int32_t carrier_heading = carrier->heading_bam;
	const int32_t offset = world::bam_from_degrees_wrapped(
			static_cast<double>(seat->yaw_offset));
	out.body_yaw = out.mount_mode == anim::MountMode::Gunner
			? io::bam_add(carrier_heading, offset)
			: io::bam_sub(carrier_heading, offset);
	out.body_pitch = carrier->pitch_bam;
	out.roll = carrier->roll_bam;
	out.leg_yaw_r = out.body_yaw;
	out.leg_yaw_l = out.body_yaw;
	return true;
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
		const anim::AimOverlayInputs &p_in,
		int p_weapon_hold_state) {
	if (p_dead || p_in.mount_mode != anim::MountMode::OnFoot) return;
	if (p_equipped_adm_index == 0 ||
			p_equipped_adm_index == world::kAdmSlotNone) return;
	r[world::PF_HELD_WEAPON_ADM] = static_cast<float>(p_equipped_adm_index);
	const MissionEulerDeg attach = mission_euler_from_overlay(
			anim::compute_held_weapon_attach_angles(p_in));
	r[world::PF_HELD_WEAPON_PITCH_DEG] = attach.pitch;
	r[world::PF_HELD_WEAPON_YAW_DEG] = attach.yaw;
	r[world::PF_HELD_WEAPON_ROLL_DEG] = attach.roll;
	// The frame selector. Retail reads it off the SAME hold state the upper-body weapon
	// channel already uses, so nothing new has to be derived here — flag 0x80 on that
	// state means the weapon is posed at the hand instead of at the entity triple.
	// Unlike the channel's own snapshot field this must not be gated on channel
	// visibility: the frame applies whenever the weapon is DRAWN.
	// [orig: gate @ 0x4b21b6 / branch @ 0x4b220f; the state's writer
	//  Entity_UpdateInfantryPlayerBody @ 0x4b5dad..0x4b5ea9]
	if (p_weapon_hold_state >= 0 &&
			(world::infantry_anim_flags(p_weapon_hold_state) & 0x80u) != 0)
		r[world::PF_HELD_WEAPON_HAND_FRAME] = 1.0f;
}

inline void write_present_overlay(float *record,
		const anim::AimOverlayAngles angles[anim::kOverlayClassCount]) {
	record[world::PF_AIM_OVERLAY_VALID] = 1.0f;
	const MissionEulerDeg body =
			mission_euler_from_overlay(angles[anim::kOverlayBody]);
	record[world::PF_AIM_BODY_PITCH_DEG] = body.pitch;
	record[world::PF_AIM_BODY_YAW_DEG] = body.yaw;
	record[world::PF_AIM_BODY_ROLL_DEG] = body.roll;
	for (int cls = 0; cls < anim::kOverlayClassCount; ++cls) {
		const MissionEulerDeg value = mission_euler_from_overlay(angles[cls]);
		const int base = world::PF_AIM_ANGLES +
				cls * world::PF_AIM_CLASS_STRIDE;
		record[base] = value.pitch;
		record[base + 1] = value.yaw;
		record[base + 2] = value.roll;
	}
}

} // namespace opennova::inmatch
