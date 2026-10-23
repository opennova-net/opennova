// The joiner's between-update movers: every chased replica row's per-tick
// chase toward its staged target (org2 / org1 / the vehicle template), the
// deck ride, the AnimMap channel and root integrate, the settle and the
// water channel, the starved leg and the death edge. Split from
// client_replica_pipeline.cpp, which keeps the record fold (size ratchet;
// the mover is its own responsibility, net-re §5.38e).
#include <runtime/replication/client_replica_pipeline.h>

#include "client_replica_body_arbitration.h"

#include <runtime/audio/sound_profile.h>            // kSlot* (the replica mover legs' profile slots)
#include <runtime/terrain_query/height_field.h>        // remote-person terrain settle
#include <runtime/world/collision.h>           // ground_probe_origin_z (the settle tail's probe origin), terrain_clip_segment
#include <runtime/world/dir_table.h>           // quantized_dir (the slope conform's probe facing)
#include <runtime/world/entity.h>              // kEntityFlag* (the wire state_flags byte IS entity+36 low)
#include <runtime/world/infantry.h>            // IRootMotionSource + the anim flag/state tables
#include <runtime/world/vehicle_motor.h>       // vehicle_chase_bucket (the vehicle-family interp bucket)
#include <net/npwire/wire_handle.h>
#include <base/io/bam.h>                      // wrapped retail pitch chase
#include <base/io/fixed.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace opennova::replication {

namespace {

// The org1 position-chase bucket [orig: Entity_UpdateInfantryAI ladder
// @0x4b9b0b-region — {3,4,5,8,16} at 0x2AAA/0x4000/0x5555/0x8000].
inline int16_t org1_bucket(int32_t dist) {
	if (dist < 0x2AAA) return 3;
	if (dist < 0x4000) return 4;
	if (dist < 0x5555) return 5;
	if (dist < 0x8000) return 8;
	return 16;
}

// The org2 position-chase bucket from the 2D horizontal distance — the ladder
// is verbatim from the binary INCLUDING the non-monotonic [0x6000,0x7000)->7
// step [orig: Entity_UpdateInfantryPlayerBody @0x4B44C4..0x4B4581].
inline int16_t org2_bucket(int32_t dist2d) {
	if (dist2d < 0x3000) return 6;
	if (dist2d < 0x4000) return 7;
	if (dist2d < 0x5000) return 8;
	if (dist2d < 0x6000) return 9;
	if (dist2d < 0x7000) return 7;
	if (dist2d < 0x8000) return 8;
	if (dist2d < 0xA000) return 10;
	if (dist2d < 0xC000) return 12;
	if (dist2d < 0xE000) return 14;
	if (dist2d < 0x10000) return 16;
	return 18;
}

// The deck ride's trig multiplies a BAM by retail's radians-per-BAM double, loaded
// at each motor's ride head [orig: Entity_UpdateInfantryPlayerBody `fld dbl_7C3608`
// @0x4B528B; Entity_UpdateInfantryAI @0x4BA460].
constexpr double kRadPerBam = opennova::io::kRadiansPerBamX87;

// The caller-owned org gravity channel (the infantry.cpp world-motor twins)
// [orig: org2 vel_z -= 208 @0x4b7acf, clamp @0x4b7c77; org1 -= 416
//  @0x4bf7bf; terminal -32768].
constexpr int32_t kGravityStepPlayer = 208;
constexpr int32_t kGravityStep = 416;
constexpr int32_t kTerminalVelZ = -32768;

// The deck-ride (D-NET-196 replica tails): an org row follows its
// groundEntity's per-tick pose delta at mover top, before root motion and
// the settle — translation, the rotate-about-carrier, and the heading/roll
// adoption. The deltas read the CARRIER's live pose against its mover-entry
// savedLivePose stamp (Entity::saved_live_* — retail +0x80..+0x94), served
// together by the provider: the witnessed source pair, no rider-side copy.
// The rotate un-rotates the rider's carrier
// offset by the SAVED attitude with 2^-22-scaled NEGATED sines
// (dbl_7C57B0 = -4194304.0 — the inverse rotation) and re-rotates by the
// CURRENT attitude with positive sines, exactly the witnessed product
// order. [orig: org2 @0x4b52a0..0x4b5726 (z biased by capsule_bottom/2,
// restored @0x4b5649); org1 @0x4ba45d..0x4ba891; the standalone twin
// Entity_InterpolateFromParentDelta @0x4a8dc0]
// The org2 body pass's ride link, the replica twin of world::infantry_ride_link:
// the occupant of the entity the row stands on, else of that entity's own
// ground entity, when it is another body of the row's team, becomes the row's
// link and climbs the hold by 4 while under 0xF0; a tick without one spends a
// hold tick and clears the link once the hold is spent (the link keeps its
// last occupant while held). The occupant's team is its decoded row's.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the ground's occupant
//  @0x4B5EA9..0x4B5ECA (same team @0x4B5EBE..0x4B5ECA), the ground's ground's
//  @0x4B5ECC..0x4B5EF1, the link @0x4B5EF3, the hold @0x4B5EF9..0x4B5F09, the
//  spend @0x4B5F11..0x4B5F1E, the clear @0x4B5F26]
void row_ride_link(ClientEntityState &es, const ClientState &state,
                   const ClientReplicaPipeline::GroundOccupantProvider &ground_words) {
	const auto team_of = [](const ClientEntityState &row) {
		return row.team_known ? row.team : uint8_t{0};
	};
	const auto rider_of = [&](uint16_t ground, uint16_t &next_ground) -> uint16_t {
		uint16_t occupant = 0xFFFF;
		next_ground = 0xFFFF;
		if (ground == 0xFFFF || !ground_words(ground, occupant, next_ground)) return 0xFFFF;
		if (occupant == 0xFFFF || occupant == es.handle) return 0xFFFF;
		const ClientEntityState *row = state.find(occupant);
		if (row == nullptr || team_of(*row) != team_of(es)) return 0xFFFF;
		return occupant;
	};
	uint16_t next_ground = 0xFFFF;
	uint16_t link = rider_of(es.resolved_ground, next_ground);
	if (link == 0xFFFF && es.resolved_ground != 0xFFFF) {
		uint16_t unused = 0xFFFF;
		link = rider_of(next_ground, unused);
	}
	if (link != 0xFFFF) {
		es.ride_occupant = link;
		if (es.ride_link_hold < 0xF0) es.ride_link_hold += 4;
	} else if (es.ride_link_hold > 0) {
		--es.ride_link_hold;
	} else {
		es.ride_occupant = 0xFFFF;
	}
}

void row_deck_ride(ClientEntityState &es,
                   const ClientReplicaPipeline::CarrierPoseProvider &carrier) {
	if (es.resolved_ground == 0xFFFF) return;
	ClientReplicaPipeline::CarrierPose cp;
	if (!carrier(es.resolved_ground, cp)) return;
	// The unmounted out-of-radius drop: 3D distance vs the carrier's bound
	// radius, saturated at 0x7FFF0000 before the int compare; beyond it the
	// ride AND the ground link drop [orig: @0x4b52a7..0x4b52ff — the
	// flt_7C19E0 = 2147418112.0 clamp, groundEntity = 0].
	{
		const double ddx = static_cast<double>(es.x - cp.pos[0]);
		const double ddy = static_cast<double>(es.y - cp.pos[1]);
		const double ddz = static_cast<double>(es.z - cp.pos[2]);
		double len = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
		if (len > 2147418112.0) len = 2147418112.0;
		if (static_cast<int32_t>(len) > cp.bound_radius) {
			es.resolved_ground = 0xFFFF;
			return;
		}
	}
	const int32_t dpx = cp.pos[0] - cp.saved_pos[0];
	const int32_t dpy = cp.pos[1] - cp.saved_pos[1];
	const int32_t dpz = cp.pos[2] - cp.saved_pos[2];
	const int32_t dyaw = io::bam_sub(cp.yaw, cp.saved_yaw);
	const int32_t dpitch = io::bam_sub(cp.pitch, cp.saved_pitch);
	const int32_t droll = io::bam_sub(cp.roll, cp.saved_roll);
	es.x += dpx;
	es.y += dpy;
	es.z += dpz;
	const int32_t rider_yaw_before = es.heading_bam;
	if (dyaw != 0 || dpitch != 0 || droll != 0) {
		auto q22c = [](int32_t bam) {
			return static_cast<int32_t>(
					std::cos(static_cast<double>(bam) * kRadPerBam) * io::kQ22One);
		};
		auto q22s = [](int32_t bam) {
			return static_cast<int32_t>(
					std::sin(static_cast<double>(bam) * kRadPerBam) * io::kQ22One);
		};
		auto q22s_neg = [](int32_t bam) {
			return static_cast<int32_t>(
					std::sin(static_cast<double>(bam) * kRadPerBam) * -io::kQ22One);
		};
		auto m22 = [](int32_t a, int32_t b) {
			return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> 22);
		};
		// The org2 z bias: rel_z is taken from the capsule mid, the halved
		// anim-frame bottom restored on writeback [orig: @0x4b53a1/0x4b5649];
		// the last advanced frame's bottom serves a transport row.
		const int32_t cb_half =
				es.rm_prev_bottom_live ? (es.rm_prev_bottom >> 1) : 0;
		// Retail's SHLs wrap in 32-bit registers; shift through unsigned so a
		// negative offset is not C++ UB.
		auto shl8 = [](int32_t v) {
			return static_cast<int32_t>(static_cast<uint32_t>(v) << 8);
		};
		const int32_t rel_x = shl8(es.x - cp.pos[0]) + 127;
		const int32_t rel_y = shl8(es.y - cp.pos[1]) + 127;
		const int32_t rel_z = shl8(es.z - cb_half - cp.pos[2]) + 127;
		// Un-rotate by the saved attitude (negated sines = the inverse)
		// [orig: @0x4b537b..0x4b5567].
		const int32_t cys = q22c(cp.saved_yaw), sys = q22s_neg(cp.saved_yaw);
		const int32_t cps = q22c(cp.saved_pitch), sps = q22s_neg(cp.saved_pitch);
		const int32_t crs = q22c(cp.saved_roll), srs = q22s_neg(cp.saved_roll);
		const int32_t rot_yaw_x = m22(rel_x, cys) - m22(rel_y, sys);
		const int32_t rot_yaw_y = m22(rel_x, sys) + m22(rel_y, cys);
		const int32_t rot_pitch_x = m22(rot_yaw_x, cps);
		const int32_t rot_pitch_cross = m22(rel_z, sps);
		const int32_t rot_pitch_z = m22(rot_yaw_x, sps) + m22(rel_z, cps);
		const int32_t rot_roll_x = m22(rot_yaw_y, crs) - m22(rot_pitch_z, srs);
		const int32_t rot_roll_z = m22(rot_yaw_y, srs) + m22(rot_pitch_z, crs);
		const int32_t unrot_xz = rot_pitch_x - rot_pitch_cross;
		// Re-rotate by the current attitude (positive sines), roll ->
		// pitch -> yaw [orig: @0x4b54ff..0x4b5653].
		const int32_t cyc = q22c(cp.yaw), syc = q22s(cp.yaw);
		const int32_t cpc = q22c(cp.pitch), spc = q22s(cp.pitch);
		const int32_t crc = q22c(cp.roll), src = q22s(cp.roll);
		const int32_t a = m22(rot_roll_x, src) + m22(rot_roll_z, crc);
		const int32_t b = m22(rot_roll_x, crc) - m22(rot_roll_z, src);
		const int32_t p = m22(unrot_xz, cpc) - m22(a, spc);
		const int32_t zp = m22(unrot_xz, spc) + m22(a, cpc);
		es.x = cp.pos[0] + ((m22(p, cyc) - m22(b, syc)) >> 8);
		es.y = cp.pos[1] + ((m22(p, syc) + m22(b, cyc)) >> 8);
		es.z = cp.pos[2] + cb_half + (zp >> 8);
	}
	// Heading/attitude adoption. Both motors add the carrier yaw delta to the
	// render heading and the body heading (org2 @0x4b5656..0x4b56c1, org1
	// @0x4ba842..0x4ba861; the mounted 0x1000-seat skip is seat-machinery a
	// transport row never reaches). org1 additionally drags its chase TARGET
	// (+0x1A8 @0x4ba867) and look Pitch (+0x14 @0x4ba88e); org2's savedLivePitch
	// and torso/aim target adds have no row channels — named deferrals. Roll
	// adopts on both (+0x18 @0x4b5723/@0x4ba88b). The pitch/roll deltas are
	// rotated by the carrier-vs-rider relative yaw taken BEFORE the yaw add.
	if (dyaw != 0 || dpitch != 0 || droll != 0) {
		const int32_t rel_yaw = io::bam_sub(cp.yaw, rider_yaw_before);
		const double rr = static_cast<double>(rel_yaw) * kRadPerBam;
		const int32_t rc = static_cast<int32_t>(std::cos(rr) * io::kQ22One);
		const int32_t rs = static_cast<int32_t>(std::sin(rr) * io::kQ22One);
		auto m22 = [](int32_t a2, int32_t b2) {
			return static_cast<int32_t>((static_cast<int64_t>(a2) * b2) >> 22);
		};
		const int32_t pitch_d = m22(dpitch, rc) - m22(droll, rs);
		const int32_t roll_d = m22(dpitch, rs) + m22(droll, rc);
		es.heading_bam = io::bam_add(es.heading_bam, dyaw);
		es.rm_body_heading = io::bam_add(es.rm_body_heading, dyaw);
		es.roll_bam = io::bam_add(es.roll_bam, roll_d);
		if (es.cls == EntityClass::Infantry) {
			es.net_target_heading_bam =
					io::bam_add(es.net_target_heading_bam, dyaw);
			es.pitch_bam = io::bam_add(es.pitch_bam, pitch_d);
		}
	}
}

// The AIR-family bucket [orig: Entity_UpdateAircraftPhysics @0x490310
// interp — deadband 0x2AAA, {8,10,15,20,25,32}].
inline int16_t vehicle_air_bucket(int32_t dist) {
	if (dist < 0x4000) return 8;
	if (dist < 0x5555) return 10;
	if (dist < 0x8000) return 15;
	if (dist < 0x10000) return 20;
	if (dist < 0x20000) return 25;
	return 32;
}

// 3D / 2D distance of a 16.16 delta, clamped like retail's float->int path
// (flt_7C19E0 is the overflow clamp, not tuning).
inline int32_t dist_16_16(int64_t dx, int64_t dy, int64_t dz) {
	const double d = std::sqrt(double(dx) * double(dx) +
	                           double(dy) * double(dy) +
	                           double(dz) * double(dz));
	return d >= 2147418112.0 ? INT32_MAX : static_cast<int32_t>(d);
}

// Per-step delta with retail's signed half-add rounding: (d + N/2) / N via
// idiv truncation [orig: @0x4b9b2e / @0x4B459D / the family movers].
inline int32_t chase_step(int32_t d, int32_t n) {
	return io::bam_add(d, n >> 1) / n;
}

} // namespace

namespace {

// The org2 leg-chain chase applied to a decoded row: the LEGS chase the wire
// yaw and the body heading is their midpoint — the root delta rotates by the
// BODY, so a turning peer's feet lead its torso exactly as on the authority
// [orig: Entity_UpdateInfantryPlayerBody @0x4b4945..0x4b4ac1 — movement
// re-plant @0x4b49dd/@0x4b49e3; idle windows ((tick-32)&0x3F / tick&0x3F)
// @0x4b49ad..0x4b49e3; quarter-step clamp ±0x3000000 @0x4b49fb; twist
// ±0x30000000 vs the yaw @0x4b4a23; midpoint @0x4b4ab5. The authoritative
// sibling is infantry.cpp's player leg block — same constants, same shape.]
constexpr int32_t kRowLegChaseClamp = 0x3000000;
constexpr int32_t kRowLegTwistLimit = 0x30000000;
constexpr int32_t kRowLegReplantMin = 59652320;
constexpr int32_t kRowLegReplantSnap = 357913920;

inline int32_t row_abs_bam(int32_t v) { return io::bam_abs(v); }


void row_leg_chase(ClientEntityState &es, uint32_t key) {
	const int32_t yaw = es.heading_bam;
	if (!es.rm_leg_seeded) {
		es.rm_leg_yaw[0] = es.rm_leg_yaw[1] = yaw;
		es.rm_leg_target[0] = es.rm_leg_target[1] = yaw;
		es.rm_body_heading = yaw;
		es.rm_leg_seeded = true;
	}
	if ((world::infantry_anim_flags(es.rm_state) & 0x1u) != 0) {
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

// The row's AnimMap primary channel + root integration — retail's remote body
// runs the SAME machinery as the authority [orig: Entity_UpdateInfantryPlayerBody
// calls AnimMap_UpdateDualChannels @0x4B41DF; AnimChannel_InitFromParams blend
// init 0.1 / (1/15 on flag 0x400) @0x410640; Q22 rotation
// @0x4B41F0..0x4B4255; additive integration LAST @0x4B7CB4..0x4B7CEF;
// Entity_UpdateInfantryAI twin @0x4BF684..0x4BF6A2]. The kJumpLoop forward override is
// a retail ADM dump witness (root row 4756), not an IDA code claim. The wire
// state byte drives the channel; the player compact's phase byte seeds a fresh
// transition [orig: NetPacket_SerializePlayerState entity+0x377 store
// @0x4C11A6; AnimMap_UpdateEntity one-shot clear @0x40B7E4]. The
// airborne/drowning/ladder overrides ride the row's rm_entity_flags mirror of
// entity+0x24 — latched locally by the resolve and the edge/water channels
// below, never wire-carried, exactly retail's remote rows (world-wac-ai-re.md
// §29). Caller-owned gravity/vertical velocity is rm_vel_z
// [orig: Entity_UpdateInfantryPlayerBody vertical add @0x4B7CE0..0x4B7CEF,
// then resolver call @0x4B7CF4].
// The per-motor water/float channel, at the mover tail after the settle
// [orig: org2 @0x4b8020..0x4b8373; org1 @0x4bfae2..0x4bfca2]. Entry has the
// asymmetric hysteresis (submerge at head-under z + 0xA000 < water, leave at
// z >= water) and the CL/platform 0x100000 exemption; the splash/overlay
// edges are FX deferrals. The float latch is `(Flags & ~0x2000) | 0x8000` —
// swimming overrides airborne — and the not-submerged exit clears the float
// AND dive bits (0x208000).
void row_water_channel(ClientEntityState &es, int32_t z_post_integrate,
                       int32_t capsule_bottom, int32_t capsule_top,
                       int32_t water_z, uint32_t tick) {
	const bool afloat = (es.rm_entity_flags & 0x8000u) != 0;
	const int32_t probe = io::bam_add(es.z, afloat ? 0 : 0xA000);
	if (probe >= water_z || (es.rm_entity_flags & 0x100000u) != 0) {
		es.rm_entity_flags &= ~0x208000u; // [orig: @0x4b8373 / @0x4bfc5c]
		return;
	}
	const int32_t cb_neg = capsule_bottom > 0 ? 0 : capsule_bottom;
	// The eye vertical (+0x74). The player body's surface line reads the stored
	// word, which this tick's eye leg restamped (row_root_motion_tick)
	// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - `mov edx, [esi+74h]`
	// @0x4B8146]; the NPC line derives it from the anim capsule, min(top - bottom,
	// 0xD000) floored at 0x2000 [orig: the non-local arm @0x4b6984..0x4b6991; the
	// floor @0x4b68e7].
	int32_t eye_h = capsule_top - capsule_bottom;
	if (eye_h > 0xD000) eye_h = 0xD000;
	if (eye_h < 0x2000) eye_h = 0x2000;
	if (es.cls == EntityClass::Player) {
		eye_h = es.eye_offset[2];
		// The org2 buoyant-rise form, REMOTE arm: flat base -0x4C9 (the
		// surface bob AND the capsule-bottom term are local-player-only in
		// org2 [orig: @0x4b8063..0x4b8095 `-1225 - bob + v345` vs the else-arm
		// @0x4b80a5 `v347 = -1225`]) and no look-pitch dive term (gated
		// local-or-authority [orig: @0x4b80aa..0x4b8102]). cb_neg stays the
		// org1 target line's term below.
		const int32_t base = -0x4C9;
		const int32_t base_q = base >> 4;
		es.z = io::bam_add(es.z, (base_q < 0 ? -base_q : base_q) + 0x70);
		// The velocity-triplet drag [orig: @0x4b8124..0x4b8163].
		es.rm_vel_xy[0] -= (es.rm_vel_xy[0] + 16) >> 5;
		es.rm_vel_xy[1] -= (es.rm_vel_xy[1] + 16) >> 5;
		es.rm_vel_z -= (es.rm_vel_z + 16) >> 5;
		// Surface line: water + base/2 - eyeHeight/2, with the stored eye above
		// [orig: line @0x4b8146..0x4b8169].
		const int32_t surf = water_z + (base >> 1) - (eye_h >> 1);
		if (es.z >= surf) {
			es.rm_entity_flags &= ~0x200000u; // surfaced [orig: @0x4b8176]
			es.z = surf;
		} else if (es.z < surf - 0x2000 &&
		           (es.rm_entity_flags & 0x200000u) == 0) {
			es.rm_entity_flags |= 0x200000u; // the dive bit [orig: @0x4b81ef]
		}
	} else {
		// The org1 snap form: the float target quarter-chased from the
		// post-integrate z — gravity's and the resolver's z contributions
		// are DISCARDED while afloat (the tail rewrites z from the saved
		// pre-gravity value). The bob wave applies to every row.
		// [orig: target @0x4bfb2a..0x4bfb84 (sin(((x+y)>>12 + 4*tick)/256 *
		//  3.1) * 1224); the quarter-step tail @0x4bfc65..0x4bfc86]
		const int32_t wave_arg =
				((es.x + es.y) >> 12) + static_cast<int32_t>(tick) * 4;
		const int32_t bob = static_cast<int32_t>(
				std::sin(static_cast<double>(wave_arg) * 0.00390625 * 3.1) *
				1224.0);
		const int32_t target =
				io::bam_add(water_z, bob - (eye_h >> 1) - 0x4C9 + cb_neg);
		es.z = io::bam_add(z_post_integrate,
		                   (io::bam_sub(target, z_post_integrate) + 2) >> 2);
	}
	// The float latch (the splash/overlay edge is an FX deferral)
	// [orig: @0x4b8363 / @0x4bfc48].
	es.rm_entity_flags = (es.rm_entity_flags & ~0x2000u) | 0x8000u;
}

// The player body's slope conform on a remote row, the world pass's org2 leg
// on the row's words (world::AiSystem::infantry_slope_pass keeps the
// authority's): a non-conforming body decays savedLivePitch and Roll a sixteenth
// toward level every tick; a conforming one (the def's attrib 0x200, an anim
// state with flag bit 2, or a corpse neither in the air, afloat nor on a
// ladder) probes four ground columns every second tick, slides on steep ground
// (dead 48 deg, alive 60), tips a corpse's look pitch an eighth and chases
// savedLivePitch and Roll a quarter of the way to the true slope angles, the roll
// held while a combat roll plays. The selector and the roll hold read the
// arbitrated current (+0x2BC), the dead and support bits the Flags word.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the selector
//  @0x4B6D95..0x4B6DBB, the decay @0x4B6DBD..0x4B6DDC, the tick & 1 gate
//  @0x4B6DE4, the probes @0x4B6DEF..0x4B6EB3, the slopes @0x4B6E68 / @0x4B6EC8
//  (fpatan over dbl_7C9BE8 / dbl_7C9BE0, times dbl_7C19D8), the thresholds
//  @0x4B6EE5..0x4B6EF7, the slide @0x4B6F01..0x4B6FA3, the corpse tip
//  @0x4B6FA9..0x4B6FBE, the chase @0x4B6FC1..0x4B6FF4]
struct RowConformIo {
	const ClientReplicaPipeline::ReplicaGroundProbe *probe = nullptr;
	const ClientReplicaPipeline::ReplicaDefAttribResolver *def_attrib = nullptr;
	const ClientReplicaPipeline::ReplicaBoundRadiusResolver *bound_radius = nullptr;
	const terrain::TerrainHeightField *terrain = nullptr;
	uint32_t tick = 0;
};

void row_body_conform(ClientEntityState &es, int arbitrated_state, const RowConformIo &io_in) {
	const uint32_t flags = es.rm_entity_flags;
	const bool dead = (flags & world::kEntityFlagDead) != 0u;
	// A dead body in the air tumbles instead of the decay or the conform: on every
	// fourth tick two tick-phase ramps a and b pull the body pitch and Roll an
	// eighth of the way to a and b, turn the heading by b >> 3 and aim the +0x2D0
	// word at a + b; every tick the pitch chases that word a quarter step, floored
	// at -0x2800000 (the upper clamp compares against 0x28000000, which no step
	// reaches). world::infantry_slope's org2 leg is the authority's twin.
	// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the gate @0x4B6CCB..0x4B6CDE,
	//  tick & 3 @0x4B6CE8, the ramps @0x4B6CEC..0x4B6D12, the chases @0x4B6D18..0x4B6D41,
	//  the heading @0x4B6D3B..0x4B6D3E, the aim store @0x4B6D44, the pitch chase
	//  @0x4B6D58..0x4B6D90]
	if (dead && (flags & world::kEntityFlagInAir) != 0u) {
		const uint32_t tick = io_in.tick;
		if ((tick & 3u) == 0u) {
			const int32_t a = (32 - static_cast<int32_t>(tick & 63u)) * 0xFFFFFF;
			const int32_t b = (32 - static_cast<int32_t>(((tick >> 6) - tick) & 63u)) * 0xFFFFFF;
			es.rm_body_pitch = io::bam_add(es.rm_body_pitch,
					io::bam_sar(io::bam_add(io::bam_sub(a, es.rm_body_pitch), 4), 3));
			es.roll_bam = io::bam_add(es.roll_bam,
					io::bam_sar(io::bam_add(io::bam_sub(b, es.roll_bam), 4), 3));
			es.heading_bam = io::bam_add(es.heading_bam, io::bam_sar(b, 3));
			es.rm_aim_word = io::bam_add(a, b);
		}
		int32_t step = io::bam_sar(io::bam_add(io::bam_sub(es.rm_aim_word, es.pitch_bam), 2), 2);
		if (step > 0x28000000)
			step = 0x2800000;
		else if (step < -0x2800000)
			step = -0x2800000;
		es.pitch_bam = io::bam_add(es.pitch_bam, step);
		return;
	}
	const uint32_t attrib = io_in.def_attrib != nullptr && *io_in.def_attrib
			? (*io_in.def_attrib)(es.type_id) : 0u;
	const bool conform = (attrib & world::kItemAttribLandable) != 0u ||
			(world::infantry_anim_flags(arbitrated_state) & 2u) != 0u ||
			(dead && (flags & 0x10A000u) == 0u);
	if (!conform) {
		es.rm_body_pitch -= (es.rm_body_pitch + 8) >> 4;
		es.roll_bam -= (es.roll_bam + 8) >> 4;
		return;
	}
	if ((io_in.tick & 1u) != 0u) return;
	const int32_t pos[3] = {es.x, es.y, es.z};
	auto probe = [&](int32_t dx, int32_t dy) -> int32_t {
		if (io_in.probe != nullptr && *io_in.probe) {
			ClientReplicaPipeline::ReplicaGroundColumn q;
			q.row_handle = es.handle;
			q.type_id = es.type_id;
			q.pos[0] = pos[0];
			q.pos[1] = pos[1];
			q.pos[2] = pos[2];
			q.dx = dx;
			q.dy = dy;
			q.entity_flags = flags;
			q.source_bound_radius_q16 = io_in.bound_radius != nullptr && *io_in.bound_radius
					? (*io_in.bound_radius)(es.type_id) : 0;
			return (*io_in.probe)(q);
		}
		// The terrain column alone, clipped by the terrain (an indoors body skips
		// it); with no terrain the ray's end comes back [orig: Entity_RaycastGroundHeight
		// @0x4142C0 -> Entity_RaycastCollision's terrain leg @0x413760].
		const int32_t start[3] = {io::bam_add(pos[0], dx), io::bam_add(pos[1], dy),
		                          io::bam_add(pos[2], 0x4000)};
		int32_t end[3] = {start[0], start[1], io::bam_sub(start[2], 0x20000)};
		if (io_in.terrain != nullptr && io_in.terrain->valid() &&
				(flags & world::kEntityFlagIndoors) == 0u)
			(void)world::terrain_clip_segment(*io_in.terrain, start, end, end);
		return end[2];
	};
	int32_t c, s;
	world::quantized_dir(es.heading_bam, c, s);
	const int32_t fx = static_cast<int32_t>((22528LL * c) >> 22);
	const int32_t fy = static_cast<int32_t>((22528LL * s) >> 22);
	const int32_t h_ahead = probe(fx, fy);
	const int32_t h_behind = probe(-fx, -fy);
	const int32_t lx = -(fy >> 2), ly = fx >> 2;
	const int32_t h_left = probe(lx, ly);
	const int32_t h_right = probe(-lx, -ly);
	const int32_t pitch_slope = static_cast<int32_t>(
			std::atan2(static_cast<double>(h_ahead - h_behind), 45056.0) * io::kBamPerRadian);
	const int32_t roll_slope = static_cast<int32_t>(
			std::atan2(static_cast<double>(h_left - h_right), 11264.0) * io::kBamPerRadian);
	const int32_t threshold = dead ? 572662272 : 715827840; // 0x22222200 / 0x2AAAAA80
	const int32_t slide_x = static_cast<int32_t>((static_cast<int64_t>(c) << 9) >> 22);
	const int32_t slide_y = static_cast<int32_t>((static_cast<int64_t>(s) << 9) >> 22);
	if (pitch_slope > threshold) {
		es.rm_vel_xy[0] -= slide_x;
		es.rm_vel_xy[1] -= slide_y;
	} else if (pitch_slope < -threshold) {
		es.rm_vel_xy[0] += slide_x;
		es.rm_vel_xy[1] += slide_y;
	}
	if (roll_slope > threshold) {
		es.rm_vel_xy[0] += slide_y;
		es.rm_vel_xy[1] -= slide_x;
	} else if (roll_slope < -threshold) {
		es.rm_vel_xy[0] -= slide_y;
		es.rm_vel_xy[1] += slide_x;
	}
	if (dead) es.pitch_bam += (pitch_slope - es.pitch_bam + 4) >> 3;
	es.rm_body_pitch += (pitch_slope - es.rm_body_pitch + 2) >> 2;
	if (arbitrated_state != world::anim_state::kRollLeft &&
			arbitrated_state != world::anim_state::kRollRight)
		es.roll_bam += (roll_slope - es.roll_bam + 2) >> 2;
}

// One served ring entry for a decoded row's channel (world::AnimVariantRings,
// D-NET-196): entry 0 without the shared table.
int32_t row_serve_ring(world::AnimVariantRings *rings, const world::IRootMotionSource &src,
                       const ClientEntityState &es, int state) {
	return rings != nullptr ? rings->serve(&src, es.rm_adm_id, state) : 0;
}

// A decoded row's AnimMap primary channel, its state half: the gait insert, the
// re-init onto `target_state` (the insert's, when one replaces the request) and
// the deferred promotion. Returns whether the promotion released a loop parked on
// its boundary this tick (row_channel_advance treats that as the wrap).
// [orig: AnimMap_UpdateEntity @0x40B5F0]
bool row_channel_prepare(ClientEntityState &es, world::IRootMotionSource &src,
                         world::AnimVariantRings *rings, int &target_state) {
	const int blend_key_state = target_state;
	if (es.net_anim_pending == 0 && es.rm_state >= 0 &&
			es.net_anim_current >= 0 && target_state != es.rm_state) {
		const int trans =
				world::gait_stance_transition_clip(es.rm_state, target_state);
		if (trans >= 0 && src.clip_length_ticks(es.rm_adm_id, trans, 0) >= 0) {
			es.net_anim_pending = static_cast<int16_t>(target_state);
			es.net_anim_pending_boundary = -1;
			es.net_anim_current = static_cast<int16_t>(trans);
			target_state = trans;
		}
	}
	// A channel re-init plays the played state's ring head and advances it, from
	// the heads every body of the .adm shares (the client's own World body too)
	// [orig: AnimMap_UpdateEntity @0x40B737..0x40B778: the head read @0x40B740,
	// table[S] = node+0x24 @0x40B746..0x40B749, slot+0x44 = node @0x40B76F].
	const auto serve = [&](int state) { return row_serve_ring(rings, src, es, state); };
	if (es.rm_state < 0) {
		es.rm_state = static_cast<int16_t>(target_state);
		es.rm_prev_state = static_cast<int16_t>(target_state);
		es.rm_variant = serve(target_state);
		es.rm_prev_variant = es.rm_variant;
		es.rm_phase = (es.cls == EntityClass::Player && es.net_anim_ratio_live)
				? es.net_anim_ratio
				: 0;
		es.net_anim_ratio_live = false;
		es.rm_prev_phase = es.rm_phase;
		es.rm_blend_weight = 1.0f;
		es.rm_blend_step = 0.0f;
		es.rm_prev_bottom_live = false;
	} else if (target_state != es.rm_state) {
		if (es.rm_blend_weight >= 1.0f) {
			es.rm_prev_state = es.rm_state;
			es.rm_prev_phase = es.rm_phase;
			es.rm_prev_variant = es.rm_variant;
		}
		es.rm_state = static_cast<int16_t>(target_state);
		es.rm_variant = serve(target_state);
		es.rm_phase = (es.cls == EntityClass::Player && es.net_anim_ratio_live)
				? es.net_anim_ratio
				: 0;
		es.net_anim_ratio_live = false;
		// A deferred state armed against the previous clip re-arms on the new
		// clip: the park is only ever the playing clip's own wrap.
		es.net_anim_pending_boundary = -1;
		es.rm_blend_weight = 0.0f;
		es.rm_blend_step =
				(world::infantry_anim_flags(blend_key_state) & 0x400u) != 0
						? (1.0f / 15.0f)
						: 0.1f;
	}
	// The clip-end deferred promotion [orig: the deferral arms the channel's
	// end-notify each tick (@0x40b7db/@0x40b7ad), AnimChannel_AdvancePlayback
	// latches it at the next loop wrap / one-shot end (@0x40b1ae/@0x40b18f),
	// and AnimMap promotes on the latched flag (@0x40b795/@0x40b7c3) — the
	// promoted retarget lands on the NEXT tick, as here]. The boundary is
	// armed lazily in the growing-phase convention; a queue behind an
	// already-finished one-shot (or a track-less state) promotes immediately —
	// the shipped hold-wedge safety, recorded inside D-NET-209.
	// The promotion that releases a channel parked on its boundary leaves the
	// loop on its end, so the old channel wraps on that tick (as
	// world::advance_primary_channel's kReleased).
	bool released = false;
	if (es.net_anim_pending != 0) {
		if (es.net_anim_pending_boundary < 0) {
			const int32_t len =
					src.clip_length_ticks(es.rm_adm_id, es.rm_state, es.rm_variant);
			if (len <= 0) {
				es.net_anim_pending_boundary = es.rm_phase;
			} else if (src.clip_loops(es.rm_adm_id, es.rm_state, es.rm_variant)) {
				es.net_anim_pending_boundary = src.clip_boundary_after(
						es.rm_adm_id, es.rm_state, es.rm_phase, es.rm_variant);
			} else {
				es.net_anim_pending_boundary = len;
			}
		}
		if (es.rm_phase >= es.net_anim_pending_boundary) {
			released = es.rm_phase == es.net_anim_pending_boundary;
			es.net_anim_current = es.net_anim_pending;
			es.net_anim_pending = 0;
			es.net_anim_pending_boundary = -1;
			// The promoted request starts at frame zero on its retarget —
			// retail zeroes the +0x377 seed every tick [orig: @0x40b7e4].
			es.net_anim_ratio_live = false;
		}
	}
	return released;
}

// The channel's playhead half: the advance (parked while armed) and an unarmed
// loop wrap's ring serve, or the blend's two halves. False = no track.
// [orig: AnimMap_UpdateEntity @0x40B5F0 (the advance @0x40B7FE)]
bool row_channel_advance(ClientEntityState &es, world::IRootMotionSource &src,
                         world::AnimVariantRings *rings, bool released,
                         world::RootMotionFrame &frame) {
	bool have = false;
	const auto serve = [&](int state) { return row_serve_ring(rings, src, es, state); };
	if (es.rm_blend_weight >= 1.0f) {
		int32_t phase = es.rm_phase;
		const int state = es.rm_state;
		const int32_t variant = es.rm_variant;
		const bool armed = es.net_anim_pending != 0;
		// With a deferral armed, the boundary tick samples the parked clip end
		// (the promote above read the latch first) [orig: AnimChannel_AdvancePlayback
		// @0x40B193..0x40B1B1; AnimMap_UpdateEntity advance @0x40B7FE].
		have = src.advance_armed(es.rm_adm_id, state, variant, phase,
		                         armed ? es.net_anim_pending_boundary : -1, frame);
		es.rm_phase = phase;
		// An unarmed loop that wraps serves its playing state's ring: another entry
		// fades in from its first frame over eight ticks, the same entry plays on;
		// an armed channel parks instead and serves nothing. The wrap tick reads
		// both halves at weight 0: the wrapped entry's motion and extents, the
		// incoming entry's trigger word at t = 0.
		// [orig: AnimChannel_AdvancePlayback @0x40B140 (the park @0x40B19E, the
		//  callback @0x40B1C8) -> AnimMap_AdvanceToNextAnim @0x40BDF0: the serve
		//  @0x40BE02..0x40BE07, the same-entry return @0x40BE09..0x40BE0C, the
		//  eight-tick init @0x40BE13..0x40BE24; world::advance_primary_channel]
		const bool wrapped = released
				? src.clip_loops(es.rm_adm_id, state, variant)
				: !armed && src.clip_wraps_at(es.rm_adm_id, state, variant, es.rm_phase);
		if (have && wrapped) {
			const int32_t served = serve(state);
			if (served != variant) {
				es.rm_prev_state = es.rm_state;
				es.rm_prev_variant = variant;
				es.rm_prev_phase = es.rm_phase;
				es.rm_variant = served;
				es.rm_phase = 0;
				es.rm_blend_weight = 0.0f;
				es.rm_blend_step = anim::kWrapFadeStep;
				int32_t outgoing_phase = es.rm_prev_phase - 1;
				int32_t incoming_phase = -1;
				src.advance_blended(es.rm_adm_id, state, variant, outgoing_phase, state, served,
				                    incoming_phase, es.rm_blend_weight, frame);
			}
		}
	} else {
		es.rm_blend_weight += es.rm_blend_step;
		if (es.rm_blend_weight >= 1.0f) {
			es.rm_blend_weight = 1.0f;
			es.rm_blend_step = 0.0f;
		}
		int32_t pphase = es.rm_prev_phase, tphase = es.rm_phase;
		have = src.advance_blended(es.rm_adm_id, es.rm_prev_state, es.rm_prev_variant, pphase,
		                           es.rm_state, es.rm_variant, tphase, es.rm_blend_weight,
		                           frame);
		es.rm_prev_phase = pphase;
		es.rm_phase = tphase;
	}
	return have;
}

void row_root_motion_tick(ClientEntityState &es, world::IRootMotionSource &src,
                          world::AnimVariantRings *rings,
                          const terrain::TerrainHeightField *terrain,
                          const RowConformIo &conform_io,
                          uint32_t key, bool is_self, bool starved,
                          const ClientReplicaPipeline::ReplicaContactResolver *resolver,
                          const ClientReplicaPipeline::ReplicaBoundRadiusResolver *bound_resolver,
                          const ClientReplicaPipeline::ReplicaPeerSphere *peers,
                          int32_t peer_count, uint32_t tick, int32_t water_z,
                          std::vector<ClientReplicaPipeline::ReplicaSlotSound> &sounds) {
	if (es.rm_adm_id < 0) {
		// The body pass's slope conform does not wait on the animation table.
		if (es.cls == EntityClass::Player && !is_self && !starved)
			row_body_conform(es, es.net_anim_current >= 0
					? static_cast<int>(es.net_anim_current)
					: static_cast<int>(es.anim_state_id), conform_io);
		return;
	}
	const auto play_slot_at = [&](uint8_t slot, int32_t x, int32_t y, int32_t z) {
		ClientReplicaPipeline::ReplicaSlotSound sound;
		sound.handle = es.handle;
		sound.type_id = es.type_id;
		sound.character_id = es.cls == EntityClass::Player
				? static_cast<uint16_t>(es.net_id) : 0;
		sound.slot = slot;
		sound.pos[0] = x;
		sound.pos[1] = y;
		sound.pos[2] = z;
		sounds.push_back(sound);
	};
	const auto play_slot = [&](uint8_t slot) { play_slot_at(slot, es.x, es.y, es.z); };
	// Channel state machine (the begin_body_transition mirror). The channel
	// chases the ARBITRATED current (+0x2BC, written per record by the fold's
	// @0x4c1153 apply — D-NET-209), never the raw coalesced wire byte; the
	// phase seed is ONE-SHOT per direct commit [orig: AnimMap_UpdateEntity
	// zeroes entity+0x377 after use @0x40B7E4]; the bottom-history slot
	// resets on EVERY update of climbs 32..35 and grenade deaths 176..179,
	// before the same-state fast path [orig: @0x40B607..0x40B645].
	int target_state = es.net_anim_current >= 0
			? static_cast<int>(es.net_anim_current)
			: static_cast<int>(es.anim_state_id);
	const bool bottom_reset_state =
			(target_state >= world::anim_state::kClimbIdle &&
			 target_state <= world::anim_state::kClimbIdle + 3) ||
			(target_state >= world::anim_state::kDeathGrenadeBase &&
			 target_state <= world::anim_state::kDeathGrenadeBase + 3);
	// The gait->stance transition insert [orig: AnimMap_UpdateEntity
	// @0x40b662..0x40b737]: with no deferred armed, a forward gait
	// committing to the crouch/prone walk first plays the run2crouch-family
	// clip and defers the real target — gated on the adm actually carrying
	// the transition clip (retail: table entry != entry 0; here: the state
	// resolves a track).
	// The blend duration is picked from the REQUESTED state's flags before
	// the insert replaces the played clip [orig: the +0x2BC flags test
	// @0x40b64b..0x40b65d precedes the insert @0x40b662..0x40b737].
	const bool released = row_channel_prepare(es, src, rings, target_state);
	if (bottom_reset_state) es.rm_prev_bottom_live = false;
	// The legs keep chasing whatever the clip coverage is — the body heading
	// is presentation state, not clip state (a clipless wire state must not
	// freeze the torso mid-twist).
	// A starved row (the chase capped at 512) advances its channel only: the
	// body pass returns right after the chase, and the AnimMap update ran at
	// its top [orig: org2 @0x4b4669..0x4b4670 -> @0x4b83a8; org1 @0x4b9c39;
	// AnimMap_UpdateDualChannels @0x4B41C9 / @0x4b9a26].
	if (es.cls == EntityClass::Player && !starved) row_leg_chase(es, key);
	// The org2 dead tail: a latched-dead body's view yaw follows the leg-chased
	// body heading every tick (the local player's look is its own motor's)
	// [orig: Entity_UpdateInfantryPlayerBody @0x4b4d30..0x4b4d4c].
	if (es.cls == EntityClass::Player && !is_self && !starved &&
			(es.rm_entity_flags & world::kEntityFlagDead) != 0)
		es.heading_bam = es.rm_body_heading;
	world::RootMotionFrame frame;
	const bool have = row_channel_advance(es, src, rings, released, frame);
	// The player body's slope conform follows the eye in the body pass, ahead of
	// its velocity and integrate; a starved body returns before both [orig:
	// Entity_UpdateInfantryPlayerBody @0x4B40E0 - the eye @0x4B699F..0x4B6BAB,
	// the conform @0x4B6D95..0x4B6FF4, the starved return @0x4B4669..0x4B4670].
	const bool runs_conform = es.cls == EntityClass::Player && !is_self && !starved;
	if (!have) {
		if (runs_conform) row_body_conform(es, target_state, conform_io);
		return;
	}
	int32_t fwd = frame.dx, lat = frame.dy;
	if (es.rm_state == world::anim_state::kJumpLoop)
		fwd = 1024; // [data: retail ADM dump root row 4756]
	// The witnessed vertical: the capsule-bottom history delta replaces the
	// raw track dz while the slot is live [orig: AnimMap_UpdateEntity reads,
	// subtracts, and rewrites anim_slot[19] @0x40B88E..0x40B8A0].
	int32_t dz_eff = frame.dz;
	if (es.rm_prev_bottom_live)
		dz_eff = frame.capsule_bottom - es.rm_prev_bottom;
	es.rm_prev_bottom = frame.capsule_bottom;
	es.rm_prev_bottom_live = true;
	// A remote player's eye: the body update's non-local arm restamps CameraOffset
	// from this tick's capsule, the extent capped at 0xD000 on the lean's arc, then
	// turned by the twist, Roll, the body pitch and the body heading
	// (world::player_body_capsule_eye, the authority's body leg's too), from the
	// decayed, unramped lean (tick_lean_ramp runs after this pass), and the body
	// pitch and Roll the previous tick's slope conform left (row_body_conform runs
	// next). A starved body returns before the eye. The local player's own eye is
	// its local arm's (LocalPlayer).
	// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the on-foot non-local arm,
	//  the extent cap @0x4B698C..0x4B6997, the turns @0x4B699F..0x4B6B96, the floor
	//  @0x4B6B98..0x4B6BA0, the stores @0x4B6BA5..0x4B6BAB; the starved return
	//  @0x4B4669..0x4B4670]
	if (runs_conform) {
		int32_t extent = frame.capsule_top - frame.capsule_bottom;
		if (extent > 0xD000) extent = 0xD000;
		world::player_body_capsule_eye(extent, es.lean_angle, es.heading_bam, es.rm_body_heading,
		                               es.roll_bam, es.rm_body_pitch, es.eye_offset);
		row_body_conform(es, target_state, conform_io);
	}
	// Rotation heading: org2 = the leg-chased body heading loaded from
	// entity+0x8C [orig: Entity_UpdateInfantryPlayerBody @0x4B41E4, Q22 rotate
	// @0x4B41F0..0x4B4255]; org1 = the row's chased heading (retail pins org1
	// body == render heading). This tick's
	// freshly-chased body heading is used (retail consumes the same-tick
	// value — the leg chase runs earlier in the same body pass).
	int32_t move_heading = es.heading_bam;
	if (es.cls == EntityClass::Player) move_heading = es.rm_body_heading;
	else es.rm_body_heading = move_heading;
	// The own player's row is locally predicted world-side; its chase is the
	// 48/512 soft reconciliation only — no root add (risk-listed; retail's
	// local player integrates in its OWN motor, not the remote path). The
	// velocity term retail adds alongside the root (Position += root + vel)
	// is a named, caller-side deferral because rows carry no velocity state
	// [orig: Entity_UpdateInfantryPlayerBody root+velocity stores
	// @0x4B7CBF..0x4B7CEF, before resolver call @0x4B7CF4].
	if (is_self || starved) return;
	// The world motor's rotation pair: org2's sine table, org1's trig at
	// retail's scale [orig: Entity_UpdateInfantryPlayerBody @0x4B41F0..0x4B4206;
	// Entity_UpdateInfantryAI @0x4BF001..0x4BF02A].
	const world::RootRotationQ22 rot =
			world::root_rotation_q22(move_heading, es.cls == EntityClass::Player);
	const int32_t c = rot.c;
	const int32_t s = rot.s;
	int32_t wx =
			static_cast<int32_t>((static_cast<int64_t>(fwd) * c) >> 22) -
			static_cast<int32_t>((static_cast<int64_t>(lat) * s) >> 22);
	int32_t wy =
			static_cast<int32_t>((static_cast<int64_t>(fwd) * s) >> 22) +
			static_cast<int32_t>((static_cast<int64_t>(lat) * c) >> 22);
	// The planar velocity maintenance [orig: org2 @0x4b78a8..0x4b79dc; org1
	// @0x4bf5cb..0x4bf61f]. org2 splits on the airborne bit: in air, the
	// optional MoveOrder-bit3 air-steer nudge (angle = look-yaw hi16 ·
	// 2π/65536 + dirpad · π/4, force ftol(cos/sin · −64.0), the parachute
	// straight-fall double-apply @0x4b7915..0x4b793d), the 63/64 damp, and
	// the ROOT PAIR ZEROED — airborne movement is momentum-owned
	// [orig: @0x4b7971..0x4b7975]; grounded (and org1 on every path): the
	// (7·v + 4) >> 3 decay with the |v| <= 8 snap to zero.
	if (es.cls == EntityClass::Player &&
	    (es.rm_entity_flags & 0x2000u) != 0) {
		if ((es.move_input & 0x08u) != 0) {
			const int32_t dirpad = static_cast<int32_t>(es.move_input & 0x07u);
			const double ang =
					static_cast<double>(
							static_cast<int16_t>(es.heading_bam >> 16)) *
							9.587371826171875e-05 + // [orig: dbl_7C9BC0]
					static_cast<double>(dirpad) * 0.7853975; // [orig: dbl_7C9BB0]
			const int32_t nx =
					static_cast<int32_t>(std::cos(ang) * -64.0); // flt_7C9BD8
			const int32_t ny = static_cast<int32_t>(std::sin(ang) * -64.0);
			es.rm_vel_xy[0] -= nx;
			es.rm_vel_xy[1] -= ny;
			if ((es.rm_entity_flags & 0x20u) != 0 && es.rm_vel_z <= -14336 &&
			    dirpad == 0) {
				es.rm_vel_xy[0] -= nx;
				es.rm_vel_xy[1] -= ny;
			}
		}
		es.rm_vel_xy[0] = static_cast<int32_t>(
				(static_cast<int64_t>(es.rm_vel_xy[0]) * 63) >> 6);
		es.rm_vel_xy[1] = static_cast<int32_t>(
				(static_cast<int64_t>(es.rm_vel_xy[1]) * 63) >> 6);
		wx = 0;
		wy = 0;
	} else {
		auto ground_decay = [](int32_t v) {
			v = static_cast<int32_t>((static_cast<int64_t>(v) * 7 + 4) >> 3);
			return (v >= -8 && v <= 8) ? 0 : v;
		};
		es.rm_vel_xy[0] = ground_decay(es.rm_vel_xy[0]);
		es.rm_vel_xy[1] = ground_decay(es.rm_vel_xy[1]);
	}

	// Root suppression flag channels: the float bit zeroes the vertical
	// channel, CL/ladder contact the horizontal pair. BOTH motors store true
	// zeros — the earlier "org2 writes the literal 1" reading mistook the
	// 0x8000 TEST-MASK load (`mov ebp, 8000h @0x4b7979`) for the stored
	// operand; the stores use the xor-zeroed scratch registers.
	// [orig: org2 stores edi, `xor edi, edi` @0x4b797e/@0x4b79b7,
	//  stores @0x4b7ab5/@0x4b7ac0-0x4b7ac4; org1 stores ebx,
	//  `xor ebx, ebx` @0x4bf600, stores @0x4bf671/@0x4bf67c-0x4bf680]
	if ((es.rm_entity_flags & 0x8000u) != 0) dz_eff = 0;
	if ((es.rm_entity_flags & 0x100000u) != 0) {
		wx = 0;
		wy = 0;
	}
	// The chute family plays at the pose ahead of this integrate (retail runs
	// it before the position add) [orig: @0x4b7b18..0x4b7c8d ahead of
	// @0x4b7cbf].
	const int32_t pre_integrate[3] = {es.x, es.y, es.z};
	// Integrate: position takes momentum + root together [orig: org2
	// @0x4b7cbf..0x4b7cd2; org1 @0x4bf684..0x4bf6a2].
	es.x += es.rm_vel_xy[0] + wx;
	es.y += es.rm_vel_xy[1] + wy;
	es.z += dz_eff;
	// The org1 water tail re-bases from this value (gravity + resolver z are
	// discarded while afloat) [orig: the pre-gravity save @0x4bf6ba].
	const int32_t z_post_integrate = es.z;

	// The settle. With an embedder-provided contact resolver, this is the FULL
	// movement collision resolver at the witnessed caller order — the
	// caller-owned vertical velocity integrates first (org2 `vel -= 208;
	// pos += vel`, org1 `vel -= 416; pos += 2*vel`, terminal -32768), then the
	// resolver runs candidate-model contacts, push-out, person + replica-peer
	// repulsion, and the ground probe THROUGH candidate models; a non-positive
	// clearance lifts the row and zeroes the vertical velocity (the landing),
	// and the probe's hit lands in resolved_ground (retail's groundEntity).
	// [orig: vertical add @0x4B7CE0..0x4B7CEF then the resolver call
	// @0x4B7CF4 and lift @0x4B7CFE..0x4B7D0A (org2); @0x4BF7B8..0x4BF7FA
	// (org1); Entity_MovementCollisionResolver @0x4B2BD0; landing vel zero in
	// the shared tail]. The row's rm_entity_flags word rides the query both
	// ways — the resolver's latch sites and the caller's edge/water channels
	// share it (world-wac-ai-re.md §29). Without a resolver, the bounded
	// terrain-column subset below stands.
	if (resolver != nullptr && *resolver) {
		// Gravity skips while on a ladder/platform or afloat (the 0x108000
		// gate); the position add itself is unconditional — the witnessed
		// one-store folds vel into the root dz [orig: org2 gate @0x4b7ac8,
		// store @0x4b7cef; org1 gate @0x4bf7b8].
		if ((es.rm_entity_flags & 0x108000u) == 0) {
			es.rm_vel_z -= es.cls == EntityClass::Player ? kGravityStepPlayer
			                                             : kGravityStep;
			if (es.cls != EntityClass::Player && es.rm_vel_z < kTerminalVelZ) es.rm_vel_z = kTerminalVelZ;
		}
        if (es.cls == EntityClass::Player) {
            // The chute family runs every tick on every machine, the
            // deploy/close admission alone being the authority's: the open
            // and close edges against the +0x2C mirror play ChuteOpen 41 /
            // ChuteClose 42, an open canopy ChuteFlap 43 and a closed fall
            // under -0x3000 FreeFall 44 on each 64th tick.
            // [orig: @0x4b7b18..0x4b7b70 (the edges), @0x4b7bcd..0x4b7be4
            //  (the flap), @0x4b7c4c..0x4b7c6f (the free fall)]
            const world::ParachuteEvents chute = world::parachute_tick(es.rm_parachute,
                    es.rm_entity_flags, es.rm_chute_carry_flags, es.rm_vel_z, false, tick);
            const auto chute_slot = [&](uint8_t slot) {
                play_slot_at(slot, pre_integrate[0], pre_integrate[1], pre_integrate[2]);
            };
            if (chute.opened) chute_slot(audio::kSlotChuteOpen);
            if (chute.closed) chute_slot(audio::kSlotChuteClose);
            if (chute.flap) chute_slot(audio::kSlotChuteFlap);
            if (chute.free_fall) chute_slot(audio::kSlotFreeFall);
        }
		es.z = io::bam_add(es.z, es.cls == EntityClass::Player
				? es.rm_vel_z : 2 * es.rm_vel_z);
		ClientReplicaPipeline::ReplicaContactQuery q;
		q.row_handle = es.handle;
		q.type_id = es.type_id;
		q.is_player_class = es.cls == EntityClass::Player;
		q.pos[0] = es.x;
		q.pos[1] = es.y;
		q.pos[2] = es.z;
		q.vel_xy[0] = es.rm_vel_xy[0] + wx; // the actual planar step — the
		q.vel_xy[1] = es.rm_vel_xy[1] + wy; // resolver's moving discriminant
		q.vel_z = es.rm_vel_z;
		q.capsule_bottom = frame.capsule_bottom;
		q.capsule_top = frame.capsule_top;
		q.source_bound_radius_q16 = bound_resolver != nullptr && *bound_resolver
				? (*bound_resolver)(es.type_id)
				: 0;
		q.anim_state_id = es.anim_state_id;
		q.anim_state_flags = world::infantry_anim_flags(es.anim_state_id);
		q.tick = tick;
		q.peers = peers;
		q.peer_count = peer_count;
		q.entity_flags = es.rm_entity_flags;
		// The stamp this visit's recapture took at the mover's entry, and a player
		// row's eye its body update restamped above; the resolver reads both off the
		// row as retail's reads the remote entity's own words.
		// [orig: Entity_UpdateInfantryPlayerBody @0x4B4190..0x4B419C (the stamp), the
		//  call @0x4B7CF4; Entity_UpdateInfantryAI @0x4B9A53..0x4B9A6E;
		//  Entity_MovementCollisionResolver @0x4B2EE2..0x4B2EFB (CameraOffset)]
		for (int axis = 0; axis < 3; ++axis) {
			q.saved_live_pos[axis] = es.net_saved_live_pose[axis];
			q.eye_offset[axis] = es.eye_offset[axis];
		}
		q.has_saved_live = true;
		// The own row's eye is the World body's (LocalPlayer), never restamped here.
		q.has_eye = es.cls == EntityClass::Player && !is_self;
		const int32_t clearance = (*resolver)(q);
		es.x = q.pos[0];
		es.y = q.pos[1];
		es.z = q.pos[2];
		es.rm_vel_z = q.vel_z; // the idle skip band reverts + zeroes it
		es.rm_entity_flags = q.entity_flags;
		es.resolved_ground = q.out_ground;
		if (clearance <= 0) {
			es.z = io::bam_sub(es.z, clearance);
			es.rm_vel_z = 0;
			// Landing from the air thumps on every machine, dead bodies
			// included: profile slot 16 SSFallAlive, 15 SSFallDead when the
			// dead bit is set, at the lifted origin; then the airborne bit
			// clears. Only the authority's fall damage ahead of it is gated.
			// [orig: org2 @0x4b7f71..0x4b7f99, the clear @0x4b7fa1; org1
			// @0x4bf812 (the 0x2000 test), the authority-only damage
			// @0x4bf81e..0x4bf879, the thump @0x4bf87f..0x4bf897, the clear
			// @0x4bf89f]
			if ((es.rm_entity_flags & 0x2000u) != 0)
				play_slot((es.rm_entity_flags & world::kEntityFlagDead) != 0
								? audio::kSlotFallDead
								: audio::kSlotFallAlive);
			es.rm_entity_flags &= ~0x2000u;
		} else if (clearance > 0xF000) {
			// The ledge/airborne edge: gate on !(Flags & 0x10A002) for the
			// player body (dead suppresses the whole edge) and 0x10A000 for
			// org1; carried force-clears and the airborne bit sets. The org2
			// 3/4 momentum carry and the local anim-31/47 stamps are named
			// deferrals — rows carry no slide velocity and the wire state
			// byte owns the channel. [orig: org2 @0x4b7e17..0x4b7e73; org1
			// @0x4bf8ae..0x4bf901]
			const uint32_t edge_mask =
					es.cls == EntityClass::Player ? 0x10A002u : 0x10A000u;
			if ((es.rm_entity_flags & edge_mask) == 0) {
				es.rm_entity_flags =
						(es.rm_entity_flags & ~0x40u) | 0x2000u;
				if (es.cls == EntityClass::Player) {
					// The org2 3/4 momentum carry into the velocity pair and
					// the STRAIGHT anim stamp (31, 47 while parachuting) —
					// the wire overwrites at the next record exactly as
					// retail's pending does [orig: carry @0x4b7e43..0x4b7e6d;
					// stamp @0x4b7e3f..0x4b7e61]. org1 keeps its clip on a
					// plain fall (the 47->31 ladder is parachute-gated
					// @0x4bf8d8).
					es.rm_vel_xy[0] += static_cast<int32_t>(
							(static_cast<int64_t>(wx) * 3) >> 2);
					es.rm_vel_xy[1] += static_cast<int32_t>(
							(static_cast<int64_t>(wy) * 3) >> 2);
					es.anim_state_id =
							(es.rm_entity_flags & 0x20u) != 0 ? 47 : 31;
				}
			}
		}
		row_water_channel(es, z_post_integrate, frame.capsule_bottom,
		                  frame.capsule_top, water_z, tick);
		return;
	}

	// Retail quantizes the final origin upward to the 0x1800 grid and probes
	// exactly 0x20000 downward. Terrain is accepted only inside that segment;
	// otherwise the segment end is the resolver's bounded fallback. Then only
	// non-positive signed foot clearance lifts the row. This ports the outdoor
	// terrain-column subset until decoded rows have candidate slices and the
	// high indoors flag required by the full model/contact resolver.
	// [orig: Entity_UpdateInfantryPlayerBody call @0x4B7CF4 and lift
	// @0x4B7CFE..0x4B7D0A; Entity_UpdateInfantryAI caller @0x4BF7FA;
	// Entity_MovementCollisionResolver probe/return @0x4B3D6E..0x4B3DA9;
	// Entity_RaycastCollision terrain window @0x413785..0x4137CB, reached by
	// Entity_RaycastGroundHeightAndObject @0x414320]
	if (terrain != nullptr && terrain->valid()) {
		const int32_t terrain_ground =
				terrain::height_field_height_bilinear_fixed(*terrain, es.x, es.y); // the words (D-NET-507)
		const int32_t probe_start = world::ground_probe_origin_z(es.z);
		int32_t resolved_ground = static_cast<int32_t>(
				static_cast<uint32_t>(probe_start) - 0x20000u);
		if (terrain_ground > resolved_ground &&
		    terrain_ground <= probe_start)
			resolved_ground = terrain_ground;
		// Retail's SUBs wrap in 32-bit registers. Route both differences through
		// the defined modular helper so an extreme fixed-point seam is not C++ UB.
		const int32_t foot_clearance = io::bam_sub(
				io::bam_sub(es.z, frame.capsule_bottom), resolved_ground);
		if (foot_clearance <= 0) es.z = io::bam_sub(es.z, foot_clearance);
	}
	// The bounded subset still runs the water channel — the float latch and
	// the per-motor surface hold are mover-tail behavior, not resolver
	// behavior (a resolver-less embedder with env water keeps swimmers at
	// the surface between records).
	row_water_channel(es, z_post_integrate, frame.capsule_bottom,
	                  frame.capsule_top, water_z, tick);
}

} // namespace

// The chase tail the org2 and org1 legs share: the position step while the
// bucket runs, then the 512-progress cap with the starved idle force — a
// movement state parked past the progress cap walks its root motion forever,
// so retail reads AND writes the arbitration current (+0x2BC)
// [orig: @0x4b464f/@0x4b465f, g_AnimStateFlagsTable bit0 gate; the org1
//  twin is §5.38a cap 512 -> idle 43, the same shape].
static void row_chase_step_and_cap(ClientEntityState &es, int16_t progress) {
	if (progress < es.net_interp_steps) {
		es.x += es.net_smooth_target[0];
		es.y += es.net_smooth_target[1];
		es.z += es.net_smooth_target[2];
	}
	if (progress < 512) {
		es.net_interp_progress = progress + 1;
	} else if ((world::infantry_anim_flags(es.net_anim_current >= 0
						   ? es.net_anim_current
						   : es.anim_state_id) &
				   0x1u) != 0u) {
		es.net_anim_current = world::anim_state::kIdle;
		es.anim_state_id = world::anim_state::kIdle;
	}
}

// The organic death edge's mover-side halves (client_replica_body_arbitration
// .cpp replica_death_edge): the gate and latch run at the witnessed point after
// the chase, ahead of this tick's integrate and resolve, which read the latch
// (the ledge edge's 0x10A002 mask); the edge's new current state lands after
// the tail, so the channel takes it on the next tick, as retail's top-of-pass
// AnimMap update does [orig: AnimMap_UpdateDualChannels @0x4B41C9 precedes
// the edge @0x4b4bf1].
static int16_t row_death_edge(ClientEntityState &es) {
	if (!es.net_health_zero || (es.rm_entity_flags & world::kEntityFlagDead) != 0)
		return -1;
	return replica_death_edge(es);
}

static void commit_death_state(ClientEntityState &es, int16_t state) {
	if (state < 0) return;
	es.net_anim_current = state;
	es.net_anim_pending = 0;
	es.net_anim_pending_boundary = -1;
}

// The corpse leg both organic passes run behind their death edge, every tick
// the dead bit is latched: the edge seeds moveTimer from the def's deathtime,
// and a def without LeaveCorpse counts it down (org1 holds at 0; org2 steps
// by -1 and is reset to 0 once it is not positive) and spawns its decay
// effect at 186. The LeaveCorpse keep, the 186 spawn and org2's destroy at 0
// are also gated on section bit 0, which a replica row has no source for
// (clear: no org2 destroy). An org1 corpse whose timer is not positive is
// destroyed on a session client: its respawn quota word is always 0 there
// (the 0x0C slot memset and zeroed AI slot feed the init callback's
// quota/62 store; a client compiles no WAC, and SSNSpawn/GroupSpawn are not
// replicated rows), and the session flag skips the watched-corpse hold.
// Returns true when the row is destroyed.
// [orig: seeds Entity_UpdateInfantryPlayerBody @0x4b4c3e,
//  Entity_UpdateInfantryAI @0x4b9c97; org2 tail @0x4b4d63..0x4b4e5f (the -1
//  step @0x4b4d79, the reset @0x4b4e56, the bit-0 destroy @0x4b4e4f); org1
//  tail @0x4b9e54..0x4b9f93 (LeaveCorpse @0x4b9e54, the guarded decrement
//  @0x4b9e6a..0x4b9e77, the 186 spawn @0x4b9e7d..0x4b9f3e, the quota test
//  @0x4b9f50, the session test @0x4b9f59, Entity_Destroy @0x4b9f93); the
//  client quota: NapiNPClientMsg_0x00C memsets @0x42E7E9/@0x42E7F6,
//  Entity_AllocateAISlot @0x40D2E9, the init callback's store @0x4BFDFD;
//  WacScript_InitAndLoad @0x4F9437; the replicate test @0x4F5C7A]
static bool row_corpse_tail(ClientEntityState &es, bool edge_this_tick, bool org1,
		const ClientReplicaPipeline::ReplicaDeathTraits &traits,
		std::vector<ClientReplicaPipeline::ReplicaCorpseDecay> &decays) {
	if (edge_this_tick) es.net_corpse_timer = traits.deathtime_ticks;
	if ((es.rm_entity_flags & world::kEntityFlagDead) == 0 || traits.leave_corpse)
		return false;
	if (org1) {
		if (es.net_corpse_timer != 0) --es.net_corpse_timer;
	} else {
		--es.net_corpse_timer;
	}
	if (es.net_corpse_timer == 186 && traits.decay_effect) {
		ClientReplicaPipeline::ReplicaCorpseDecay decay;
		decay.handle = es.handle;
		decay.type_id = es.type_id;
		decay.pos[0] = es.x;
		decay.pos[1] = es.y;
		decay.pos[2] = es.z;
		decays.push_back(decay);
	}
	if (es.net_corpse_timer > 0) return false;
	if (org1) return true;
	es.net_corpse_timer = 0;
	return false;
}

// A seated body's pass, which ends at the mounted site: the primary channel at its
// head (its capsule is the eye's extent), the site's scrub, the seat carry, then the
// site's four leg words, a passenger's lean keys and the eye, in that order. The
// seated row presents this channel, as every armed row does (D-NET-330).
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the dual update @0x4B41DF; the
//  seat carry Entity_GetBoneTransformAndOrientation @0x4B6510 (+0x8C @0x4B654E, the legs
//  @0x4B6554 / @0x4B655A, Roll @0x4B6572, +0x90 @0x4B6575); the site's leg words
//  @0x4B668B..0x4B66A3; the passenger test `cmp [esi+168h], 1` @0x4B6684, left
//  @0x4B66B5, right @0x4B66C3; the local test @0x4B66CD; the eye @0x4B66D9..0x4B68FC (the
//  extent cap @0x4B66D9..0x4B66EC)]
void ClientReplicaPipeline::tick_seated_body(ClientEntityState &es) {
	world::RootMotionFrame frame;
	bool have = false;
	if (root_motion_ != nullptr && es.rm_adm_id >= 0) {
		int target_state = es.net_anim_current >= 0
				? static_cast<int>(es.net_anim_current)
				: static_cast<int>(es.anim_state_id);
		const bool bottom_reset_state =
				(target_state >= world::anim_state::kClimbIdle &&
				 target_state <= world::anim_state::kClimbIdle + 3) ||
				(target_state >= world::anim_state::kDeathGrenadeBase &&
				 target_state <= world::anim_state::kDeathGrenadeBase + 3);
		const bool released = row_channel_prepare(es, *root_motion_, anim_rings_, target_state);
		if (bottom_reset_state) es.rm_prev_bottom_live = false;
		have = row_channel_advance(es, *root_motion_, anim_rings_, released, frame);
		// The capsule-bottom history the AnimMap update rewrites every update
		// [orig: AnimMap_UpdateEntity @0x40B88E..0x40B8A0].
		if (have) {
			es.rm_prev_bottom = frame.capsule_bottom;
			es.rm_prev_bottom_live = true;
		}
	}
	const ClientEntityState *carrier = state_.find(es.carrier_handle);
	// The mounted site's scrub on every seated pass, ahead of the seat carry: the
	// chute, in-air, afloat, ladder, dive and armory bits, the vertical velocity and
	// the two canopy words, so a row that mounted airborne, afloat or on a ladder
	// carries none of them through the ride. The site's +0x2C bit 4 clear is the
	// local player's ladder pitch-restore latch, which a replica row never arms.
	// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the carrier's def test
	//  @0x4B638C, `and dword ptr [esi+24h],0FF8F57DFh` @0x4B6395, +0x2C @0x4B639C,
	//  slideDecay @0x4B63AB, +0x378 / +0x37A @0x4B63B1 / @0x4B63B8]
	if (carrier != nullptr) {
		es.rm_entity_flags &= 0xFF8F57DFu;
		es.rm_vel_z = 0;
		es.rm_parachute = world::ParachuteState{};
	}
	// The seat carry: the seat bone's yaw into the body heading, its roll into
	// Roll, its pitch into savedLivePitch.
	ReplicaSeatFrame seat;
	if (carrier != nullptr && replica_seat_frame_resolver_ &&
			replica_seat_frame_resolver_(carrier->handle, carrier->type_id, es.mount_bone, seat)) {
		es.rm_body_heading = seat.heading;
		es.roll_bam = seat.roll;
		es.rm_body_pitch = seat.pitch;
	}
	// The mounted site: the four leg words take the body heading, so the legs
	// leave the seat from it.
	es.rm_leg_yaw[0] = es.rm_leg_yaw[1] = es.rm_body_heading;
	es.rm_leg_target[0] = es.rm_leg_target[1] = es.rm_body_heading;
	es.rm_leg_seeded = true;
	// A passenger's keys move its lean 0x1400000 a tick with no other gate, ahead
	// of its eye; a remote body's +0x168 is the seat type the record's attach
	// stamps off the carrier's userpoint (ReplicaSeatTypeResolver).
	const uint8_t seat_type = carrier != nullptr && replica_seat_type_resolver_
			? replica_seat_type_resolver_(carrier->type_id, es.mount_bone)
			: 0u;
	if (seat_type == static_cast<uint8_t>(world::SeatType::Passenger)) {
		if ((es.move_input & world::Entity::kMoveOrderLeanLeft) != 0)
			es.lean_angle = io::bam_sub(es.lean_angle, 0x1400000);
		if ((es.move_input & world::Entity::kMoveOrderLeanRight) != 0)
			es.lean_angle = io::bam_add(es.lean_angle, 0x1400000);
	}
	// The eye: the channel's capsule extent capped at 0xD000, the four turns over
	// the seat's words (world::player_body_capsule_eye, the authority's seated leg
	// too).
	if (!have) return;
	int32_t extent = frame.capsule_top - frame.capsule_bottom;
	if (extent > 0xD000) extent = 0xD000;
	world::player_body_capsule_eye(extent, es.lean_angle, es.heading_bam, es.rm_body_heading,
	                               es.roll_bam, es.rm_body_pitch, es.eye_offset);
}

std::optional<world::PhysicsClass> ClientReplicaPipeline::person_move_row(uint16_t type_id) const {
	const ItemReplicationProfile *profile = item_def(type_id);
	if (profile == nullptr) return std::nullopt;
	return world::physics_class_from_move_function(profile->callbacks.move_function);
}

bool ClientReplicaPipeline::person_takes_item_row(const ClientEntityState &es,
		uint16_t self_handle) const {
	if (es.handle == self_handle || world::EntityHandle{es.handle}.pool() != 0 ||
			(es.cls != EntityClass::Player && es.cls != EntityClass::Infantry))
		return false;
	const std::optional<world::PhysicsClass> row = person_move_row(es.type_id);
	return row && *row != world::PhysicsClass::Org1 && *row != world::PhysicsClass::Org2;
}

// One row's whole visit in the body walk: its secondary channel, then its family's
// chase, death edge, deck ride and root motion (tick_remote_motion's walk order).
void ClientReplicaPipeline::visit_remote_row(ClientEntityState &es, uint16_t self_handle,
		uint32_t rm_key) {
	// The deck-ride + root-motion pair every organic chase leg ends with: the
	// deck-ride runs at the witnessed mover position — after the chase, before
	// root motion — for every armed org row with a grounded carrier, clip or no
	// clip [orig: org2 ride @0x4b52a0 between the chase @0x4b4470 and the
	// integrate @0x4b7cbf].
	RowConformIo conform_io;
	conform_io.probe = &replica_ground_probe_;
	conform_io.def_attrib = &replica_def_attrib_resolver_;
	conform_io.bound_radius = &replica_bound_radius_resolver_;
	conform_io.terrain = remote_motion_terrain_;
	conform_io.tick = rm_key;
	auto organic_chase_tail = [&](ClientEntityState &row, bool is_self,
	                              bool starved = false) {
		if (!is_self && !starved && carrier_pose_provider_)
			row_deck_ride(row, carrier_pose_provider_);
		// The body pass's ride link, after the ride and ahead of the integrate
		// [orig: @0x4B5EA9..0x4B5F2C]; the client's own body runs the World's.
		if (!is_self && row.cls == EntityClass::Player && ground_occupant_provider_)
			row_ride_link(row, state_, ground_occupant_provider_);
		// The own row's channel shadows the World body's, which serves the rings
		// for that body: a second serve would double its plays (D-NET-196).
		if (root_motion_ != nullptr)
			row_root_motion_tick(row, *root_motion_, is_self ? nullptr : anim_rings_,
			                     remote_motion_terrain_, conform_io,
			                     rm_key, is_self, starved, &replica_contact_resolver_,
			                     &replica_bound_radius_resolver_,
			                     contact_peer_scratch_.data(),
			                     static_cast<int32_t>(contact_peer_scratch_.size()),
			                     rm_key, water_z_, slot_sounds_);
	};
	// The corpse leg's def source (row_corpse_tail), read only for a row
	// whose dead bit is latched or latches this tick, and the org1 corpses it
	// destroys, which leave the row set after the walk.
	auto corpse_leg = [&](ClientEntityState &row, bool edge, bool org1) {
		if (!edge && (row.rm_entity_flags & world::kEntityFlagDead) == 0) return;
		ReplicaDeathTraits traits;
		if (replica_death_traits_resolver_ &&
				replica_death_traits_resolver_(row.type_id, traits) &&
				row_corpse_tail(row, edge, org1, traits, corpse_decays_))
			remote_motion_expired_.emplace_back(row.handle, traits.decay_effect);
	};
	// The pool-0 walk calls a person's bound move function (+0x1C4): org1's and org2's are the
	// chases below; the ewep row's update runs through the embedder's seam over the row's words
	// with no compact needed, the row leaving after the walk when the update destroyed it; any
	// other row runs nothing here (ClientReplicaPipeline::set_person_ewep_mover). The joiner's
	// own row is its World body's (L runs the World's update).
	// [orig: Entity_UpdateAllEntities @0x4C2100 - the +0x1C4 call @0x4C2472;
	//  Entity_UpdateTransformAndTurret @0x440CA0 - Entity_Destroy @0x440E2F]
	if (person_takes_item_row(es, self_handle)) {
		if (person_move_row(es.type_id) == world::PhysicsClass::Ewep && person_ewep_mover_ &&
				person_ewep_mover_(es)) {
			// The stale-ground leg: the ground's C2S 0x0F, then the body's, and the body's
			// destroy, the row leaving after the walk.
			// [orig: @0x440DAB, @0x440E29, Entity_Destroy @0x440E2F]
			queue_carrier_repair(es.resolved_ground);
			queue_carrier_repair(es.handle);
			remote_motion_expired_.emplace_back(es.handle, false);
		}
		return;
	}
	advance_row_weapon_channel(es, self_handle);
	if (!es.net_has_compact) return;
	const bool chased_class = es.cls == EntityClass::Player ||
	                          es.cls == EntityClass::Infantry ||
	                          es.cls == EntityClass::Vehicle;
	if (!chased_class) return;

	// Carried rows skip their own chase; the post-mover phase below follows
	// the carrier attach after all carrier rows have advanced. Retail renders
	// a seat mount through the carrier attach each frame
	// [orig: the seat attach sets Flags 0x40, not bit0 —
	// @0x4946D0/@0x494752; bit0 belongs to carried OBJECTS and not-ready
	// rows, and is what the visible-entity collector skips @0x5C8CF4].
	// A carrier-owned row never falls back to its standalone chase. When a
	// newer compact sample switches to a carrier that is not present yet,
	// net_seat_valid is deliberately cleared so no stale local offset can be
	// reused; carrier_handle still records that the row is blocked on an
	// attachment. Hold its last world pose until a resolvable carried sample
	// (or an explicit free-standing sample) arrives.
	if (es.carrier_handle != wire_handle::kInvalid) {
		// A seated body's pass still refreshes its ride link off the seat's
		// ground link (the record's store) [orig: @0x4B5EA9..0x4B5F2C;
		// the ground store @0x4C1353].
		if (es.cls == EntityClass::Player && es.handle != self_handle &&
				ground_occupant_provider_ &&
				!(es.state_flags_known && (es.state_flags & 0x01u) != 0u))
			row_ride_link(es, state_, ground_occupant_provider_);
		// A seated body's pass runs its channel and ends at the mounted site
		// (bone 0 is a deck, not a seat) [orig: Entity_UpdateInfantryPlayerBody
		// @0x4B40E0 - the bit0 test @0x4B411E, the dual update call @0x4B41DF, the
		// site selector on the mount word +0x16C @0x4B637C..0x4B638F].
		if (es.cls == EntityClass::Player && es.handle != self_handle &&
				es.mount_bone != 0 &&
				!(es.state_flags_known && (es.state_flags & 0x01u) != 0u))
			tick_seated_body(es);
		return;
	}
	// The organic mover-skip: wire bit0 (carried-object/killed/not-ready
	// — NOT seat mounts, which stream 0x40) freezes the row at its staged
	// pose [orig: the Flags&1 early return @0x4b9a03 / the body-pass twin;
	// the bit rides the wire raw, §5.38e §5]. No vehicle mover tests it
	// [orig: Entity_UpdatePool1Slot calls the mover @0x4B8E53 ungated].
	if (es.cls != EntityClass::Vehicle && es.state_flags_known &&
			(es.state_flags & 0x01u) != 0u) return;
	// A vehicle wreck holds its dead-pose snap. A dead ORGANIC keeps its
	// mover: neither body pass tests Flags & 2 ahead of the chase, so a
	// corpse follows its own wire records (the host's dying body falls and
	// slides) and its death clip's root [orig: org2 bit0-only top gate
	// @0x4b411e; org1 @0x4b9a03].
	if (es.cls == EntityClass::Vehicle && es.state_flags_known &&
			(es.state_flags & kVehicleFlagDeadPose) != 0u) return;
	// A world-side family mover owns this row's motion (§5.38e B-facet: the
	// embedding sim stages, predicts, and mirrors back). The freezes above
	// run first so carried/not-ready/dead rows hold even when flagged.
	if (es.net_world_mover) return;

	// Saved-live recapture, every tick [orig: @0x4b9a5f / each family head].
	es.net_saved_live_pose[0] = es.x;
	es.net_saved_live_pose[1] = es.y;
	es.net_saved_live_pose[2] = es.z;

	// The local player's own respawn clears the being-revived latch in the record
	// decode, not here (ClientRuntime's self record respawn arm).
	const bool is_self = es.handle == self_handle;
	const int64_t dx = int64_t(es.net_smooth_target[0]) - es.x;
	const int64_t dy = int64_t(es.net_smooth_target[1]) - es.y;
	const int64_t dz = int64_t(es.net_smooth_target[2]) - es.z;

	switch (es.cls) {
	case EntityClass::Player: {
		// The org2 body-pass chase [orig: Entity_UpdateInfantryPlayerBody
		// @0x4B4470..0x4B46C0]. Client heading/pitch divisor = 12.
		constexpr int32_t kOrg2HeadingDiv = 12;
		if (es.net_interp_progress == 0) {
			const int32_t dist = dist_16_16(dx, dy, dz);
			if (dist > 0x20000) {
				// Snap: position always; heading/pitch only for a non-self
				// row.
				es.x = es.net_smooth_target[0];
				es.y = es.net_smooth_target[1];
				es.z = es.net_smooth_target[2];
				if (!is_self) {
					es.heading_bam = es.net_smooth_heading;
					es.pitch_bam = es.net_smooth_pitch;
				}
				es.net_smooth_target[0] = 0;
				es.net_smooth_target[1] = 0;
				es.net_smooth_target[2] = 0;
				es.net_interp_steps = 0;
				es.net_smooth_heading = 0;
				es.net_smooth_pitch = 0;
			} else if (dist < 0x2AAA) {
				// Position deadband — heading/pitch still chase.
				es.net_smooth_target[0] = 0;
				es.net_smooth_target[1] = 0;
				es.net_smooth_target[2] = 0;
				es.net_interp_steps = 0;
				es.net_smooth_heading = chase_step(
						io::bam_sub(es.net_smooth_heading, es.heading_bam),
						kOrg2HeadingDiv);
				es.net_smooth_pitch = chase_step(
						io::bam_sub(es.net_smooth_pitch, es.pitch_bam),
						kOrg2HeadingDiv);
			} else {
				if (is_self) {
					// The own-player soft reconciliation: 48 moving / 512
					// still, position only [orig: @0x4B4490/@0x4B449E].
					es.net_interp_steps =
							(es.move_input & 0x08u) != 0u ? 48 : 512;
				} else {
					es.net_interp_steps =
							org2_bucket(dist_16_16(dx, dy, 0));
				}
				const int32_t n = es.net_interp_steps;
				es.net_smooth_target[0] = chase_step(int32_t(dx), n);
				es.net_smooth_target[1] = chase_step(int32_t(dy), n);
				int32_t step_z = chase_step(int32_t(dz), n);
				// Client vertical damping [orig: @0x4B4626/@0x4B4635].
				const int32_t adz =
						int32_t(dz < 0 ? -dz : dz);
				if (adz < 0x5555) step_z >>= 1;
				if (adz < 0x2AAA) step_z = 0;
				es.net_smooth_target[2] = step_z;
				es.net_smooth_heading = chase_step(
						io::bam_sub(es.net_smooth_heading, es.heading_bam),
						kOrg2HeadingDiv);
				es.net_smooth_pitch = chase_step(
						io::bam_sub(es.net_smooth_pitch, es.pitch_bam),
						kOrg2HeadingDiv);
			}
		}
		const int16_t progress = es.net_interp_progress;
		if (progress < kOrg2HeadingDiv && !is_self) {
			es.heading_bam = io::bam_add(es.heading_bam, es.net_smooth_heading);
			es.pitch_bam = io::bam_add(es.pitch_bam, es.net_smooth_pitch);
		}
		// The position step + progress cap + starved idle force
		// [orig: @0x4b464f/@0x4b465f] (row_chase_step_and_cap), then the
		// deck-ride/root-motion tail (organic_chase_tail).
		row_chase_step_and_cap(es, progress);
		// A starved row returns here on a client: no radio aging, no leg
		// chase, death edge, deck ride, integrate or resolve; only the
		// channel the pass's top advanced [orig: the non-authority branch
		// @0x4b4669..0x4b4670 to the function end @0x4b83a8].
		if (progress >= 512) {
			organic_chase_tail(es, is_self, /*starved=*/true);
			break;
		}
		// The radio-request latch ages on the 64-tick window
		// [orig: @0x4b4431..0x4b4445 -> @0x4b467a..0x4b469d].
		if ((rm_key & 0x3Fu) == 0u) {
			if (es.radio_request_seconds != 0) --es.radio_request_seconds;
			else es.radio_request = 0;
		}
		const int16_t death_state = row_death_edge(es);
		if (death_state >= 0 && !is_self) death_edges_.push_back(es.handle);
		// The org2 death edge drops the carried flag on every machine,
		// ungated [orig: Entity_UpdateInfantryPlayerBody @0x4b4d0d ->
		// Entity_DropCarriedObject].
		if (death_state >= 0) drop_carried_objective(es.handle);
		corpse_leg(es, death_state >= 0, /*org1=*/false);
		organic_chase_tail(es, is_self);
		commit_death_state(es, death_state);
		break;
	}
	case EntityClass::Infantry: {
		// The org1 motor fall-through [orig: @0x4b9a8c].
		if (es.net_interp_progress == 0) {
			const int32_t dist = dist_16_16(dx, dy, dz);
			if (dist > 0x20000) {
				// Snap is position-only for org1.
				es.x = es.net_smooth_target[0];
				es.y = es.net_smooth_target[1];
				es.z = es.net_smooth_target[2];
				es.net_smooth_target[0] = 0;
				es.net_smooth_target[1] = 0;
				es.net_smooth_target[2] = 0;
				es.net_interp_steps = 0;
			} else if (dist < 0x2000) {
				es.net_smooth_target[0] = 0;
				es.net_smooth_target[1] = 0;
				es.net_smooth_target[2] = 0;
				es.net_interp_steps = 0;
			} else {
				const int32_t n = org1_bucket(dist);
				es.net_interp_steps = static_cast<int16_t>(n);
				es.net_smooth_target[0] = chase_step(int32_t(dx), n);
				es.net_smooth_target[1] = chase_step(int32_t(dy), n);
				es.net_smooth_target[2] = chase_step(int32_t(dz), n);
			}
		}
		const int16_t progress = es.net_interp_progress;
		// The org1 position step + cap + starved idle force — the same
		// +0x2BC read/write [orig: §5.38a cap 512 -> idle 43;
		// @0x4b464f/@0x4b465f shape] (row_chase_step_and_cap).
		row_chase_step_and_cap(es, progress);
		// A starved org1 row returns right after its idle force on a
		// client: no body-yaw chase, death edge, deck ride or integrate
		// [orig: Entity_UpdateInfantryAI @0x4b9c09..0x4b9c39].
		if (progress >= 512) {
			organic_chase_tail(es, is_self, /*starved=*/true);
			break;
		}
		// Heading: the promoted target chased with the org1 body
		// quarter-step — the witnessed (d + 2) >> 2 rounding, clamped
		// [orig: the body chase @0x4be8fd — (target - body + 2) >> 2 then
		// ±69273360/tick; the target is one record behind the wire
		// (§5.38e §1); the sibling world-side port is infantry.cpp's
		// body-yaw chase].
		{
			int32_t step = io::bam_sar(
					io::bam_add(io::bam_sub(es.net_target_heading_bam,
					                        es.heading_bam),
					            2),
					2);
			if (step > 69273360) step = 69273360;
			if (step < -69273360) step = -69273360;
			es.heading_bam = io::bam_add(es.heading_bam, step);
		}
		const int16_t death_state = row_death_edge(es);
		if (death_state >= 0 && !is_self) death_edges_.push_back(es.handle);
		corpse_leg(es, death_state >= 0, /*org1=*/true);
		organic_chase_tail(es, is_self);
		commit_death_state(es, death_state);
		break;
	}
	case EntityClass::Vehicle: {
		// The vehicle-family chase template [orig: Entity_UpdateWatercraftPhysics
		// @0x48D480 et al.] — runs alone for rows without a world-side
		// prediction mover (traitless vehicles, lib-only embedders); flagged
		// rows are predicted world-side and skipped above (§5.38e B-facet).
		// ONE shape, TWO witnessed constant sets: ground/water snap
		// 0x60000/0x20000 at reg>=293, deadband 0x2000, buckets
		// {6,8,10,15,20,25,30}; AIR snap 0xA0000 (0x20000 only when BOTH
		// received commands < 293), deadband 0x2AAA, buckets
		// {8,10,15,20,25,32} [orig: @0x48D480 / @0x490310] — selected by
		// the sim-stamped family so an air row without traits still
		// chases with its own family's constants.
		const bool air = es.net_air_family;
		if (es.net_interp_progress == 0) {
			const int32_t snap_threshold = air
					? ((es.vehicle_speed_reg < 293 &&
					    es.vehicle_lat_reg < 293) ? 0x20000 : 0xA0000)
					: (es.vehicle_speed_reg >= 293 ? 0x60000 : 0x20000);
			const int32_t dist = dist_16_16(dx, dy, dz);
			if (dist > snap_threshold) {
				es.x = es.net_smooth_target[0];
				es.y = es.net_smooth_target[1];
				es.z = es.net_smooth_target[2];
				es.heading_bam = es.net_smooth_heading;
				es.net_smooth_target[0] = 0;
				es.net_smooth_target[1] = 0;
				es.net_smooth_target[2] = 0;
				es.net_interp_steps = 0;
				es.net_smooth_heading = 0;
			} else if (dist < (air ? 0x2AAA : 0x2000)) {
				// Position deadband — heading still steps toward the wire
				// euler [orig: the v46 < 0x2000 else-arm @0x48D480 zeroes
				// the target and still computes (d + 10) / 20; the air
				// deadband is 0x2AAA @0x490310].
				es.net_smooth_target[0] = 0;
				es.net_smooth_target[1] = 0;
				es.net_smooth_target[2] = 0;
				es.net_interp_steps = 0;
				es.net_smooth_heading =
						io::bam_add(
								io::bam_sub(es.net_smooth_heading,
								            es.heading_bam),
								10) /
						20;
			} else {
				const int32_t n = air ? vehicle_air_bucket(dist)
				                      : world::vehicle_chase_bucket(dist);
				es.net_interp_steps = static_cast<int16_t>(n);
				es.net_smooth_target[0] = chase_step(int32_t(dx), n);
				es.net_smooth_target[1] = chase_step(int32_t(dy), n);
				es.net_smooth_target[2] = chase_step(int32_t(dz), n);
				es.net_smooth_heading =
						io::bam_add(
								io::bam_sub(es.net_smooth_heading,
								            es.heading_bam),
								10) /
						20;
			}
		}
		const int16_t progress = es.net_interp_progress;
		// Heading steps for exactly 20 ticks (the /20 divisor).
		if (progress < 20)
			es.heading_bam = io::bam_add(es.heading_bam, es.net_smooth_heading);
		if (progress < es.net_interp_steps) {
			es.x += es.net_smooth_target[0];
			es.y += es.net_smooth_target[1];
			es.z += es.net_smooth_target[2];
		}
		if (progress >= 128) {
			// Starvation: the speed register decays; progress freezes. The
			// register is the full int32 decompressed 16.16 value — retail
			// drains it signed and untruncated [orig: (v+64)>>7 drain
			// @0x48D480 interp tail; the signed < 293 compare on [177]].
			es.vehicle_speed_reg -= (es.vehicle_speed_reg + 64) >> 7;
		} else {
			es.net_interp_progress = progress + 1;
		}
		break;
	}
	default:
		break;
	}
}

// The walk's expired corpses go through the shared destroy, whose emitter leg
// releases the decay group the corpse holds in +0x1CC (a release with no group
// is a no-op, as the destroy's nonzero test is).
// [orig: Entity_Destroy @0x43E810, the +0x1CC test @0x43E8EC ->
//  Entity_ReleaseEffectEmitter @0x43E8F5]
void ClientReplicaPipeline::erase_expired_remote_rows() {
	for (const auto &[handle, decay_effect] : remote_motion_expired_) {
		if (decay_effect) {
			ReplicaCorpseDecay release;
			release.handle = handle;
			release.release = true;
			corpse_decays_.push_back(release);
		}
		erase_entity_tree(handle);
	}
	remote_motion_expired_.clear();
}

// The walk's order: pool 1 ahead of pool 0, then the higher pools, each by slot.
// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk @0x4C2158..0x4C21E9,
//  the pool-0 walk @0x4C2426..0x4C2472]
uint32_t ClientReplicaPipeline::remote_walk_key(uint16_t handle) {
	const uint32_t pool = handle >> 12u;
	const uint32_t rank = pool == 1u ? 0u : pool == 0u ? 1u : 2u + pool;
	return (rank << 12u) | (handle & 0x0FFFu);
}

// The own-body split's line (the own row's key), read by the walk and by the
// decay and dip ahead of it, so all three part the rows alike.
uint32_t ClientReplicaPipeline::remote_walk_split_key(uint16_t self_handle) const {
	if (!remote_motion_mode_ || !self_body_split_ || (self_handle >> 12u) != 0u ||
			state_.find(self_handle) == nullptr)
		return UINT32_MAX;
	return remote_walk_key(self_handle);
}

// Under the split the walk parks from pool 0's first slot: every pool-0 body updates
// after the pool-1 walk, the projectiles and the proximity build.
// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk @0x4C2158..0x4C21E9, the
//  Weapon_UpdateAllProjectiles call @0x4C223A, the proximity build @0x4C240A, the pool-0
//  walk @0x4C2426..0x4C2472]
// Under the pool-1 interleave it parks from pool 1's first slot: every pool-1 row updates
// at its own slot of the World's pool-1 walk (D-NET-196).
// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk @0x4C2158..0x4C21E9]
uint32_t ClientReplicaPipeline::remote_walk_park_key(uint16_t self_handle) const {
	if (pool1_interleave_ && remote_motion_mode_) return remote_walk_key(0x1000u);
	return remote_walk_split_key(self_handle) == UINT32_MAX ? UINT32_MAX
	                                                        : remote_walk_key(0x0000u);
}

void ClientReplicaPipeline::tick_remote_motion(uint16_t self_handle) {
	if (!remote_motion_mode_) return;
	// A half the last tick parked finishes before this tick opens.
	tick_remote_motion_after_self();
	// The entity update steps the replica clock (frame_tick()).
	const uint32_t rm_key = ++frame_tick_;
	// The replica-peer sphere table for this tick's contact resolves — every
	// live organic replica row, one snapshot per tick (replica rows are
	// ordinary persons to the resolver's repulsion loop; the world person
	// tables cannot see ClientState rows). The dead-peer skip is the
	// witnessed +36&2 gate [orig: @0x4b3b8d]; bit-0 rows are frozen
	// carried-object/not-ready placeholders and sit out as our analogue.
	// Pipeline-owned scratch: this table is rebuilt every 62.5 Hz tick, so a
	// fresh heap vector per tick was pure allocator churn.
	std::vector<ReplicaPeerSphere> &contact_peers = contact_peer_scratch_;
	contact_peers.clear();
	if (replica_contact_resolver_) {
		contact_peers.reserve(state_.entities.size());
		for (const ClientEntityState &pe : state_.entities) {
			if (pe.cls != EntityClass::Player && pe.cls != EntityClass::Infantry)
				continue;
			if (pe.state_flags_known && (pe.state_flags & 0x03u) != 0u) continue;
			ReplicaPeerSphere p;
			p.handle = pe.handle;
			p.x = pe.x;
			p.y = pe.y;
			p.z = pe.z;
			p.radius = replica_bound_radius_resolver_
					? replica_bound_radius_resolver_(pe.type_id)
					: 0;
			contact_peers.push_back(p);
		}
	}
	// The walk visits the rows as the entity pass does: pool 1 ahead of pool 0,
	// each pool in slot order, one row's whole visit before the next row's. A
	// player row's visit opens with its secondary channel, ahead of its primary
	// (the dual update serves the secondary's ring entry first), so the shared
	// ring heads serve body by body in slot order, not arrival order (D-NET-196).
	// The pool-1 walk's early visits up a hull's ground chain belong to the World
	// movers, which own those rows (net_world_mover).
	// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk @0x4C2158..0x4C21E9
	//  ahead of the pool-0 walk @0x4C2437..0x4C2472 (from the pool base, += stride);
	//  AnimMap_UpdateDualChannels @0x40B8C0, the secondary @0x40B908 before the
	//  primary @0x40B94E]
	std::vector<size_t> &walk = walk_order_scratch_;
	walk.clear();
	for (size_t i = 0; i < state_.entities.size(); ++i) walk.push_back(i);
	std::stable_sort(walk.begin(), walk.end(), [this](size_t a, size_t b) {
		return remote_walk_key(state_.entities[a].handle) <
				remote_walk_key(state_.entities[b].handle);
	});
	// The own body splits the walk: retail's pool-0 walk calls every used slot's
	// class update in slot order with no local-player case, after the pool-1 walk,
	// the projectiles and the proximity build, so the local body serves the shared
	// ring heads at its own slot. Ours serves it in the World's entity pass, so a
	// joiner's walk (self_body_split_) visits the pool-1 rows here and parks every
	// pool-0 row, with its lean decay, arms dip, selection and lean ramp: the rows
	// through the own row's slot for tick_remote_motion_through_self, ahead of the
	// World's pool-0 walk, the rest for tick_remote_motion_after_self (D-NET-196).
	// [orig: Entity_UpdateAllEntities @0x4C2100 - the proximity build @0x4C240A, the
	//  pool-0 walk @0x4C2426..0x4C2472, the +0x1C4 call @0x4C2472]
	const uint32_t split = remote_walk_split_key(self_handle);
	const uint32_t park = remote_walk_park_key(self_handle);
	size_t through = walk.size();
	for (size_t k = 0; k < walk.size(); ++k) {
		if (remote_walk_key(state_.entities[walk[k]].handle) >= park) {
			through = k;
			break;
		}
	}
	parked_self_ = self_handle;
	parked_tick_ = rm_key;
	// Under the pool-1 interleave the pool-1 rows wait for their slots in the World's
	// pool-1 walk (visit_pool1_row), the rest for the pool-0 halves (D-NET-196).
	for (size_t k = through; k < walk.size(); ++k) {
		ClientEntityState &parked = state_.entities[walk[k]];
		parked.rm_parked = true;
		if (pool1_interleave_ && (parked.handle >> 12u) == 1u) {
			pool1_rows_.push_back(parked.handle);
			pool1_parked_[parked.handle] = walk[k];
			continue;
		}
		(remote_walk_key(parked.handle) <= split ? middle_rows_ : parked_rows_)
				.push_back(parked.handle);
	}
	remote_motion_expired_.clear();
	for (size_t k = 0; k < through; ++k)
		visit_remote_row(state_.entities[walk[k]], self_handle, rm_key);
	// Seat mounts and persistent no-callback children are a post-mover phase:
	// all carrier rows above have reached this tick's live pose first. The
	// per-tick call also advances the pure-client stale-carrier sweep.
	erase_expired_remote_rows();
	refresh_carried_entities(/*tick_sweep=*/true);
	// Every visited player row's secondary selection (client_replica_weapon_channel.cpp).
	select_row_weapon_channels(self_handle, rm_key);
}

void ClientReplicaPipeline::tick_remote_motion_through_self() {
	finish_pool1_rows();
	if (middle_rows_.empty()) return;
	std::vector<uint16_t> rows;
	rows.swap(middle_rows_);
	visit_parked_rows(rows);
}

void ClientReplicaPipeline::tick_remote_motion_after_self() {
	// A middle half the embedder never ran keeps the walk's order.
	tick_remote_motion_through_self();
	if (parked_rows_.empty()) return;
	std::vector<uint16_t> rows;
	rows.swap(parked_rows_);
	visit_parked_rows(rows);
}

// A pool-1 row's visit at its own slot of the World's pool-1 walk: its carrier follow
// opens it, then its body visit, as the parked pool-0 rows' (visit_parked_rows); a hull
// on a deck reads its carrier as it stands at this slot, a carrier the walk visited
// first (a lower slot, or the World's ground chain ahead of the row) moved this tick
// (D-NET-196). [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk
// @0x4C2158..0x4C21E9, the Entity_UpdatePool1Slot calls @0x4C21B9 / @0x4C21D0 /
// @0x4C21E0 / @0x4C21E9]
ClientEntityState *ClientReplicaPipeline::visit_pool1_row(uint16_t handle) {
	const auto parked = pool1_parked_.find(handle);
	if (parked == pool1_parked_.end()) return nullptr;
	const size_t index = parked->second;
	pool1_parked_.erase(parked);
	ClientEntityState *es = index < state_.entities.size() &&
					state_.entities[index].handle == handle
			? &state_.entities[index]
			: state_.find(handle);
	if (es == nullptr || !es->rm_parked) return nullptr;
	es->rm_parked = false;
	if (follows_carrier_seat(*es)) follow_carrier_seat(*es, state_.find(es->carrier_handle));
	// A pool-1 row is no organic: its visit takes no lean, dip or ramp and expires no
	// corpse, so the World's walk keeps every row it may read.
	visit_remote_row(*es, parked_self_, parked_tick_);
	select_row_weapon_channel(*es, parked_self_, parked_tick_);
	return es;
}

std::vector<uint16_t> ClientReplicaPipeline::unvisited_pool1_rows() const {
	std::vector<uint16_t> rows;
	for (const uint16_t handle : pool1_rows_)
		if (pool1_parked_.count(handle) != 0) rows.push_back(handle);
	return rows;
}

void ClientReplicaPipeline::finish_pool1_rows() {
	if (pool1_rows_.empty()) return;
	std::vector<uint16_t> rows;
	rows.swap(pool1_rows_);
	for (const uint16_t handle : rows) visit_pool1_row(handle);
}

void ClientReplicaPipeline::visit_parked_rows(std::vector<uint16_t> &rows) {
	// The rows may have moved in the vector since the walk parked them (the first
	// half's erase): find each by handle, in the walk's order.
	std::unordered_map<uint16_t, size_t> row_by_handle;
	row_by_handle.reserve(state_.entities.size());
	for (size_t i = 0; i < state_.entities.size(); ++i)
		row_by_handle.emplace(state_.entities[i].handle, i);
	remote_motion_expired_.clear();
	// Each parked row's lean decay and arms dip (with the torso roll) open its own
	// visit, as retail's body update runs them in its own pass; the embedder's
	// tick_lean_decay / tick_arms_dip skipped it (D-NET-196).
	// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the decay @0x4B5C97..0x4B5CA5,
	//  the dip @0x4B5CAB..0x4B5CE7, the torso roll @0x4B5CFF..0x4B5D67]
	for (const uint16_t handle : rows) {
		const auto it = row_by_handle.find(handle);
		if (it == row_by_handle.end()) continue;
		ClientEntityState &es = state_.entities[it->second];
		es.rm_parked = false;
		lean_decay_row(es);
		arms_dip_row(es);
		// A compact-carried row's own update poses it on its carrier as the carrier
		// stands now: the pool-1 walk ahead of the pool-0 walk moved it this tick (at its
		// own slot of the World's walk under the pool-1 interleave, a chased hull's chase
		// or a World mover's pose mirrored onto its row there; in ClientRuntime's leg
		// without it). Nothing ahead of the mounted site reads the
		// body's pose (the channel, the ride link), and the site's eye reads it after the
		// attach, so the follow opens the visit. ClientRuntime's leg passed over the
		// parked row (refresh_carried_entities), so until here it held the pose its last
		// tick left, as retail's table copies it (D-NET-196).
		// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk @0x4C2158..0x4C21E9
		//  ahead of the pool-0 walk @0x4C2426..0x4C2472; Entity_UpdateInfantryPlayerBody
		//  @0x4B40E0 - the site selector on the mount word +0x16C @0x4B637C..0x4B638F, the
		//  kind-3 Entity_AttachToBoneAndUpdateTransform call @0x4B63C7, every other kind's
		//  bone pose through the branch @0x4B64EF to the Entity_GetBoneTransformAndOrientation
		//  call @0x4B6510, ahead of the eye restamp]
		if (follows_carrier_seat(es)) {
			const auto carrier = row_by_handle.find(es.carrier_handle);
			follow_carrier_seat(es, carrier == row_by_handle.end()
					? nullptr : &state_.entities[carrier->second]);
		}
		visit_remote_row(es, parked_self_, parked_tick_);
	}
	// Each parked row's selection and lean ramp close its own visit, as the
	// first half's rows' did (select_row_weapon_channels, tick_lean_ramp).
	for (const uint16_t handle : rows) {
		const auto it = row_by_handle.find(handle);
		if (it == row_by_handle.end()) continue;
		ClientEntityState &es = state_.entities[it->second];
		select_row_weapon_channel(es, parked_self_, parked_tick_);
		lean_ramp_row(es);
	}
	erase_expired_remote_rows();
}

} // namespace opennova::replication
