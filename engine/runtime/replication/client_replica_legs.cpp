// The organic replica bodies' leg phases, split out of client_replica_motion.cpp (the size
// ratchet): the player body's view phase (its legs, its body heading, its Yaw and Pitch clamps)
// and the org1 body's leg chase. Each class's body pass calls its own (row_root_motion_tick and
// the visit's seated branch; visit_remote_row's free-standing org1 case).
#include "client_replica_legs.h"

#include <runtime/world/entity.h>   // kEntityFlag*, kItemAttribLandable
#include <runtime/world/infantry.h> // infantry_anim_flags
#include <base/io/bam.h>

#include <cstdint>

namespace opennova::replication {

namespace {

constexpr int32_t kRowLegChaseClamp = 0x3000000;
constexpr int32_t kRowLegTwistLimit = 0x30000000;
constexpr int32_t kRowLegReplantMin = 59652320;
constexpr int32_t kRowLegReplantSnap = 357913920;
constexpr int32_t kRowViewYawLimit = 0x55555500;
constexpr int32_t kRowPitchLimit = 0x38E38E00;
constexpr int32_t kRowPronePitchLimit = 0x1C71C700;

inline int32_t row_abs_bam(int32_t v) { return io::bam_abs(v); }

constexpr int32_t kRowOrg1LegChaseClamp = 0x5000000;
constexpr int32_t kRowOrg1LegTwistLimit = 0x20000000;

} // namespace

// The org2 view phase on a decoded row, with no locality test: a free-standing row's after its
// chase apply and ahead of its dead tail, ride and conform (row_root_motion_tick), a seated one's
// after its chase count and ahead of its ride link and mounted site (visit_remote_row's seated
// branch, whose record lands Flags 0x40). Retail's also precedes its death edge, which the visit
// runs first here: the edge writes no word the phase reads but Flags 0x40, which a
// free-standing row's record never sets.
// - under a canopy (Flags 0x20) the body heading (+0x8C) chases a sixteenth of the way to the
//   Yaw and both re-plant targets (+0x2E4 / +0x2E8) take it;
// - on a ladder, carried or under a canopy (Flags 0x100060) the leg model is skipped: the Yaw is
//   held within 0x55555500 (120 degrees) of the body heading, unless its mount's config
//   (+0x86C) is 3 or its ground entity is a vehicle in its crash state (+0x2EC;
//   `yaw_clamp_skipped`), and both legs (+0x2D4 / +0x2D8) take the body heading, their targets
//   left as they stand;
// - otherwise the LEGS chase the Yaw and the body heading is their midpoint, the root delta
//   rotating by the BODY, so a turning peer's feet lead its torso: a movement state (+0x2BC's
//   flag bit 0) re-plants both targets on the Yaw, an idle one each leg past 0x38E38E0 off it and
//   either past 0x15555540 or on its 64-tick window (the left's 32 ticks behind); each leg
//   steps a quarter, clamped to 0x3000000, and is held within 0x30000000 of the Yaw;
// - then the Pitch is held within 0x38E38E00 (80 degrees) of savedLivePitch (+0x90), 0x1C71C700
//   (40) while prone (MoveOrder 0x100, the record's stance bit) with none of Flags 0x10A000,
//   unless its mount is a vehicle in its crash state (`pitch_clamp_skipped`).
// world::infantry_org2_view_tick is the authority's twin.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the prone gate @0x4B416C..0x4B4183, the
//  canopy chase @0x4B4945..0x4B496C, the skip test @0x4B4972..0x4B4978, the mount config test
//  @0x4B4AC6..0x4B4ADE, the ground test @0x4B4AE0..0x4B4AF9 (Entity_HasActiveParent @0x402BB0
//  called @0x4B4AE9), the clamp @0x4B4AFB..0x4B4B42, the legs @0x4B4B5F..0x4B4B6B, the selector
//  @0x4B497E..0x4B4984, the idle windows @0x4B4991..0x4B49E3 (the key @0x4B467A), the chases
//  @0x4B49E9..0x4B4AA3, the midpoint @0x4B4AA9..0x4B4ABB, the mount's crash test
//  @0x4B4B82..0x4B4BA0, the pitch clamp @0x4B4BA2..0x4B4BC6; the record's stance bits
//  NetPacket_SerializePlayerState @0x4C09C0 - @0x4C11FD..0x4C1242]
void row_org2_view_phase(ClientEntityState &es, uint32_t key, bool yaw_clamp_skipped,
		bool pitch_clamp_skipped) {
	// A row no spawn's class init or respawn reset stamped (rm_leg_seeded) takes the stamp here.
	if (!es.rm_leg_seeded) {
		es.rm_leg_yaw[0] = es.rm_leg_yaw[1] = es.heading_bam;
		es.rm_leg_target[0] = es.rm_leg_target[1] = es.heading_bam;
		es.rm_body_heading = es.heading_bam;
		es.rm_leg_seeded = true;
	}
	const uint32_t flags = es.rm_entity_flags;
	if ((flags & world::kEntityFlagParachute) != 0u) {
		es.rm_body_heading = io::bam_add(es.rm_body_heading,
				io::bam_sar(io::bam_add(io::bam_sub(es.heading_bam, es.rm_body_heading), 8), 4));
		es.rm_leg_target[0] = es.rm_leg_target[1] = es.rm_body_heading;
	}
	if ((flags & 0x100060u) != 0u) {
		if (!yaw_clamp_skipped) {
			const int32_t d = io::bam_sub(es.heading_bam, es.rm_body_heading);
			if (d > kRowViewYawLimit)
				es.heading_bam = io::bam_add(es.rm_body_heading, kRowViewYawLimit);
			if (d < -kRowViewYawLimit)
				es.heading_bam = io::bam_sub(es.rm_body_heading, kRowViewYawLimit);
		}
		es.rm_leg_yaw[0] = es.rm_leg_yaw[1] = es.rm_body_heading;
	} else {
		const int32_t yaw = es.heading_bam;
		const int current = es.net_anim_current >= 0 ? static_cast<int>(es.net_anim_current)
		                                             : static_cast<int>(es.anim_state_id);
		if ((world::infantry_anim_flags(current) & 0x1u) != 0) {
			es.rm_leg_target[1] = yaw;
			es.rm_leg_target[0] = yaw;
		} else {
			const int32_t dl = io::bam_sub(yaw, es.rm_leg_yaw[1]);
			if (row_abs_bam(dl) > kRowLegReplantMin &&
			    (row_abs_bam(dl) > kRowLegReplantSnap || ((key - 32) & 63u) == 0))
				es.rm_leg_target[1] = yaw;
			const int32_t dr = io::bam_sub(yaw, es.rm_leg_yaw[0]);
			if (row_abs_bam(dr) > kRowLegReplantMin &&
			    (row_abs_bam(dr) > kRowLegReplantSnap || (key & 63u) == 0))
				es.rm_leg_target[0] = yaw;
		}
		for (int leg = 0; leg < 2; ++leg) {
			const int32_t ldiff = io::bam_sub(es.rm_leg_target[leg], es.rm_leg_yaw[leg]);
			int32_t lstep = io::bam_sar(io::bam_add(ldiff, 2), 2);
			if (lstep > kRowLegChaseClamp) lstep = kRowLegChaseClamp;
			if (lstep < -kRowLegChaseClamp) lstep = -kRowLegChaseClamp;
			es.rm_leg_yaw[leg] = io::bam_add(es.rm_leg_yaw[leg], lstep);
			const int32_t twist = io::bam_sub(es.rm_leg_yaw[leg], yaw);
			if (twist > kRowLegTwistLimit)
				es.rm_leg_yaw[leg] = io::bam_add(yaw, kRowLegTwistLimit);
			else if (twist < -kRowLegTwistLimit)
				es.rm_leg_yaw[leg] = io::bam_sub(yaw, kRowLegTwistLimit);
		}
		es.rm_body_heading = io::bam_add(
				es.rm_leg_yaw[1],
				io::bam_sar(io::bam_sub(es.rm_leg_yaw[0], es.rm_leg_yaw[1]), 1));
	}
	if (pitch_clamp_skipped) return;
	const bool prone = (es.net_stance_bits & 0x1u) != 0u &&
			(flags & (world::kEntityFlagInAir | world::kEntityFlagDrowning |
			          world::kEntityFlagLadderContact)) == 0u;
	const int32_t limit = prone ? kRowPronePitchLimit : kRowPitchLimit;
	if (io::bam_sub(es.pitch_bam, es.rm_body_pitch) > limit)
		es.pitch_bam = io::bam_add(es.rm_body_pitch, limit);
	if (io::bam_sub(es.pitch_bam, es.rm_body_pitch) < -limit)
		es.pitch_bam = io::bam_sub(es.rm_body_pitch, limit);
}

// The org1 leg chase on a free-standing row, after its body chase: rm_leg_yaw[0] / [1] are the
// right / left leg yaws (+0x2D4 / +0x2D8), rm_leg_target[0] / [1] their re-plant targets
// (+0x2E4 / +0x2E8). A walking body (a movement state, or the def's attrib 0x200) half-snaps the
// right leg to the target heading (+0x1A8), pulls the left a quarter of what the right leaves,
// and re-plants both on the target; an idle one re-plants a leg on the midpoint of the target and
// the body heading (+0x8C) when it drifts past 0x38E38E0 and either past 0x15555540 or on the
// leg's 64-tick window (the left's 32 ticks behind the right's). Each leg then chases its target
// a quarter (a sixteenth with attrib 0x200), the step clamped to +-0x5000000, and is held within
// 0x20000000 of the body heading. The carried test ahead of them (Flags 0x40 snaps both legs to
// +0x8C) never holds here: a free-standing record's carried bit is clear (the host's detach
// clears it), and a dead seated body's death edge clears it every tick. `key` is the update's
// stagger key, the tick plus 36 times the DcbId (+0x7C), which a client's replica holds at 0:
// the spawns clear it (0x0C memset 0x388, 0x18 memset 0x2B4) and no client path stores it.
// world::infantry_org1_heading_chase is the authority's twin.
// [orig: Entity_UpdateInfantryAI @0x4B9910 - the key @0x4B9948..0x4B9959, the carried test
//  @0x4BE934..0x4BE93E -> @0x4BEB0C, the selector @0x4BE944..0x4BE967, the idle re-plant
//  @0x4BE969..0x4BE9CC (L @0x4BE977..0x4BE9A7, R @0x4BE9AD..0x4BE9CC), the walk path
//  @0x4BE9D4..0x4BEA0B, R's chase @0x4BEA11..0x4BEA87, L's @0x4BEA8D..0x4BEB04; the death edge's
//  Flags &= ~0xC0 @0x4B9D35, the dead legs' Flags &= ~0x40 @0x4B9D63; Entity_DetachFromVehicle
//  @0x4355F0 - @0x435910; NapiNPClientMsg_0x00C @0x42E730 - the memset @0x42E7E9..0x42E7F1;
//  NapiNPClientMsg_FullEntitySpawn @0x433780 - the memset @0x433821; Entity_InitOrganicAI
//  @0x4BFCC0, Entity_InitFromItemDef @0x49E550, Entity_InitFromModel @0x40DC30,
//  Entity_AllocateAISlot @0x40D2C0, NetPacket_SerializeInfantryEntityState @0x4C0320 and
//  Entity_ResetToSpawnState @0x4B9610 store no +0x7C; Entity_WarmUpOrganicAnimation @0x4B8B20
//  reads it @0x4B8B3F / @0x4B8B81]
void row_org1_legs(ClientEntityState &es, uint32_t key, uint32_t def_attrib, int current_state) {
	const bool landable = (def_attrib & world::kItemAttribLandable) != 0u;
	const int32_t target = es.net_target_heading_bam;
	const int32_t body = es.rm_body_heading;
	if (landable || (world::infantry_anim_flags(current_state) & 0x1u) != 0u) {
		es.rm_leg_yaw[0] = io::bam_add(es.rm_leg_yaw[0],
				io::bam_sar(io::bam_sub(target, es.rm_leg_yaw[0]), 1));
		es.rm_leg_yaw[1] = io::bam_add(es.rm_leg_yaw[1],
				io::bam_sar(io::bam_sub(target, es.rm_leg_yaw[0]), 2));
		es.rm_leg_target[0] = target;
		es.rm_leg_target[1] = target;
	} else {
		const int32_t mid = io::bam_add(target, io::bam_sar(io::bam_sub(body, target), 1));
		const int32_t dl = row_abs_bam(io::bam_sub(mid, es.rm_leg_target[1]));
		if ((dl > kRowLegReplantSnap || ((key - 32u) & 0x3Fu) == 0u) && dl > kRowLegReplantMin)
			es.rm_leg_target[1] = mid;
		const int32_t dr = row_abs_bam(io::bam_sub(mid, es.rm_leg_target[0]));
		if ((dr > kRowLegReplantSnap || (key & 0x3Fu) == 0u) && dr > kRowLegReplantMin)
			es.rm_leg_target[0] = mid;
	}
	for (int leg = 0; leg < 2; ++leg) {
		const int32_t d = io::bam_sub(es.rm_leg_target[leg], es.rm_leg_yaw[leg]);
		int32_t step = landable ? io::bam_sar(io::bam_add(d, 8), 4)
		                        : io::bam_sar(io::bam_add(d, 2), 2);
		if (step > kRowOrg1LegChaseClamp) step = kRowOrg1LegChaseClamp;
		if (step < -kRowOrg1LegChaseClamp) step = -kRowOrg1LegChaseClamp;
		es.rm_leg_yaw[leg] = io::bam_add(es.rm_leg_yaw[leg], step);
		const int32_t twist = io::bam_sub(es.rm_leg_yaw[leg], body);
		if (twist > kRowOrg1LegTwistLimit)
			es.rm_leg_yaw[leg] = io::bam_add(body, kRowOrg1LegTwistLimit);
		else if (twist < -kRowOrg1LegTwistLimit)
			es.rm_leg_yaw[leg] = io::bam_sub(body, kRowOrg1LegTwistLimit);
	}
}

} // namespace opennova::replication
