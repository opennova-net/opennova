// The organic replica bodies' leg phases, split out of client_replica_motion.cpp (the size
// ratchet): the player body's view phase (its legs, its body heading, its Yaw and Pitch clamps)
// and the org1 body's leg chase. Each class's body pass calls its own (row_root_motion_tick and
// the visit's seated branch; visit_remote_row's free-standing org1 case).
#include "client_replica_legs.h"

#include <runtime/world/entity.h>   // kEntityFlag*, kItemAttribLandable
#include <runtime/world/infantry.h> // infantry_anim_flags
#include <runtime/world/infantry_view.h>
#include <base/io/bam.h>

#include <cstdint>

namespace opennova::replication {

namespace {

constexpr int32_t kRowLegReplantMin = 59652320;
constexpr int32_t kRowLegReplantSnap = 357913920;

inline int32_t row_abs_bam(int32_t v) { return io::bam_abs(v); }

constexpr int32_t kRowOrg1LegChaseClamp = 0x5000000;
constexpr int32_t kRowOrg1LegTwistLimit = 0x20000000;

} // namespace

// The org2 view phase on a decoded row (world::infantry_org2_view_phase, the authority's body tick's
// kernel too): a free-standing row's after its chase apply and ahead of its dead tail, ride and
// conform (row_root_motion_tick), a seated one's after its chase count and ahead of its ride link
// and mounted site (visit_remote_row's seated branch, whose record lands Flags 0x40). Retail's also
// precedes its death edge, which the visit runs first here: the edge writes no word the phase
// reads but Flags 0x40, which a free-standing row's record never sets. The row's words are the
// decoded Yaw and Pitch, the body heading, legs and targets its passes keep, and savedLivePitch;
// its prone local the record's stance bit (MoveOrder 0x100) under the prologue's gates
// (world::infantry_org2_prone_local: a seated row's record lands Flags 0x40 for a vehicle seat,
// and `living_mount` is a live seated row's carrier row, which a client spawns from its def);
// its window key the update's.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the prone gate @0x4B416C..0x4B4183, the
//  phase @0x4B4945..0x4B4BC6; the record's stance bits NetPacket_SerializePlayerState @0x4C09C0 -
//  @0x4C11FD..0x4C1242]
void row_org2_view_phase(ClientEntityState &es, uint32_t key, bool yaw_clamp_skipped,
		bool pitch_clamp_skipped, bool living_mount) {
	// A row no spawn's class init or respawn reset stamped (rm_leg_seeded) takes the stamp here.
	if (!es.rm_leg_seeded) {
		es.rm_leg_yaw[0] = es.rm_leg_yaw[1] = es.heading_bam;
		es.rm_leg_target[0] = es.rm_leg_target[1] = es.heading_bam;
		es.rm_body_heading = es.heading_bam;
		es.rm_leg_seeded = true;
	}
	const uint32_t flags = es.rm_entity_flags;
	world::Org2ViewGates gates;
	gates.flags = flags;
	gates.anim_state = es.net_anim_current >= 0 ? static_cast<int>(es.net_anim_current)
	                                            : static_cast<int>(es.anim_state_id);
	gates.tick = key;
	gates.prone = world::infantry_org2_prone_local((es.net_stance_bits & 0x1u) != 0u, flags,
			living_mount);
	gates.yaw_clamp_skipped = yaw_clamp_skipped;
	gates.pitch_clamp_skipped = pitch_clamp_skipped;
	world::infantry_org2_view_phase(
			world::Org2ViewWords{es.heading_bam, es.pitch_bam, es.rm_body_heading, es.rm_leg_yaw,
					es.rm_leg_target, es.rm_body_pitch},
			gates);
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
