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
#include <runtime/world/pose_inputs.h>
#include <runtime/mission/seat_spec_extract.h>
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
  const replication::ClientEntityState &mount, world::EmplacedWeaponControls &out) {
 out = world::EmplacedWeaponControls{};
 if (!mount.emplaced_controls_valid) return false;
 out.valid = true;
 out.gun_yaw = static_cast<uint16_t>(mount.emplaced_gun_yaw_word);
 out.gun_pitch = static_cast<uint16_t>(mount.emplaced_gun_pitch_word);
 out.spin = mount.emplaced_spin_phase;
 return true;
}

inline void write_present_emplaced_controls(
		float *record, const world::EmplacedWeaponControls &controls) {
	if (!controls.valid) return;
	record[world::PF_EMPLACED_CONTROLS_VALID] = 1.0f;
	record[world::PF_EWEAP_GUNYAW] = static_cast<float>(controls.gun_yaw);
	record[world::PF_EWEAP_GUNPITCH] = static_cast<float>(controls.gun_pitch);
	record[world::PF_WEAP_SPIN] = static_cast<float>(controls.spin);
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
			mission::item_seat_spec_for_type(specs, carrier->type_id);
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
				world::seat_type_blocks_weapon_channel(seat->type);
	}

	out.mount_mode = world::mount_mode_for_seat_type(seat->type);
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
