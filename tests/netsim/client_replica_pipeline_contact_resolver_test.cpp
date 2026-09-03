// The replica contact-resolver seam (net-re §5.38e, D-NET-196): with an
// embedder-provided resolver, each armed Player/Infantry row's settle runs the
// FULL movement collision resolver at the witnessed caller order — the
// caller-owned vertical velocity integrates first (org2 `vel -= 208; pos +=
// vel`), the resolver runs with the row pose and this tick's peer spheres, a
// non-positive clearance lifts the row and zeroes the vertical velocity, and
// the ground-probe hit lands in resolved_ground (retail's groundEntity).
// [orig: vertical add @0x4B7CE0..0x4B7CEF then the resolver call @0x4B7CF4
//  and lift @0x4B7CFE..0x4B7D0A; Entity_MovementCollisionResolver @0x4B2BD0]

#include <runtime/replication/client_replica_pipeline.h>

#include <runtime/world/infantry.h>

#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

namespace nw = opennova;
namespace ns = opennova::netsim;

constexpr uint16_t kPlayerType = 0x14B9;
constexpr uint16_t kRowA = 0x0005;
constexpr uint16_t kRowB = 0x0007;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// A minimal root source: zero planar root, live capsule extents — the settle
// leg is the subject, not the walk.
struct StillSource final : public opennova::world::IRootMotionSource {
	int32_t dx = 0; // settable forward root step (the ledge-carry legs)
	bool has_clip(int, int) const override { return true; }
	int32_t clip_length_ticks(int, int, int /*variant*/) const override { return 1024; }
	bool advance(int, int, int32_t &phase,
	             opennova::world::RootMotionFrame &out) override {
		phase += 1024;
		out = {};
		out.dx = dx;
		out.capsule_bottom = 0x8000; // 0.5 u feet
		out.capsule_top = 0x1C000;
		return true;
	}
	bool advance_blended(int, int, int32_t &pphase, int, int32_t &tphase, float,
	                     opennova::world::RootMotionFrame &out) override {
		pphase += 1024;
		tphase += 1024;
		out = {};
		out.dx = dx;
		out.capsule_bottom = 0x8000;
		out.capsule_top = 0x1C000;
		return true;
	}
};

nw::FrameUpdate player_frame(uint16_t handle, int32_t ax, int32_t ay, int32_t az,
                             int32_t x) {
	nw::FrameUpdate fu;
	fu.flags2 = 0;
	fu.mount_handle = 0xFFFF;
	fu.health = 100;
	fu.anchor_x = ax;
	fu.anchor_y = ay;
	fu.anchor_z = az;
	nw::FrameUpdateRecord r;
	r.handle = handle;
	r.type_id = kPlayerType;
	r.cls = nw::EntityClass::Player;
	r.player.carrier_handle = 0xFFFF;
	r.player.pos_x_compressed = nw::network_compress_fixedpoint(x - ax);
	r.player.pos_y_compressed = nw::network_compress_fixedpoint(0);
	r.player.pos_z_compressed = nw::network_compress_fixedpoint(0);
	r.player.yaw_byte = 0x40;
	r.player.state_flags = 0;
	r.player.move_input_byte = 0x08;
	r.player.anim_state_id = 62;
	r.player.anim_def_index = 0xFF;
	r.player.health_class_byte = 0x28;
	fu.records.push_back(r);
	return fu;
}

struct ResolverCapture {
	int calls = 0;
	int32_t last_vel_z = 0;
	int32_t last_pos_z = 0;
	int32_t last_capsule_bottom = 0;
	uint16_t last_type_id = 0;
	int32_t last_source_bound = 0;
	bool is_player = false;
	std::vector<uint16_t> peer_handles;
	std::vector<int32_t> peer_radii;
	int32_t return_clearance = 1000; // small positive: hovering, no lift
	uint16_t ground = 0xFFFF;
	uint32_t last_flags_in = 0; // the row's flags mirror as the resolve sees it
	uint32_t or_flags = 0;      // latched into the echo (the fake latch site)
};

} // namespace

int main() {
	ns::ClientReplicaPipeline view;
	view.set_remote_motion_mode(true);
	view.set_item_class_resolver([](uint16_t type)
			-> ns::ClientReplicaPipeline::ItemClassResolution {
		if (type == kPlayerType) return nw::EntityClass::Player;
		return std::nullopt;
	});
	StillSource still;
	view.set_root_motion_source(&still);
	view.set_replica_bound_radius_resolver([](uint16_t type_id) {
		return type_id == kPlayerType ? 0x23456 : 0;
	});

	ResolverCapture cap;
	view.set_replica_contact_resolver(
			[&cap](ns::ClientReplicaPipeline::ReplicaContactQuery &q) -> int32_t {
				++cap.calls;
				cap.last_vel_z = q.vel_z;
				cap.last_pos_z = q.pos[2];
				cap.last_capsule_bottom = q.capsule_bottom;
				cap.last_type_id = q.type_id;
				cap.last_source_bound = q.source_bound_radius_q16;
				cap.is_player = q.is_player_class;
				cap.peer_handles.clear();
				cap.peer_radii.clear();
				for (int32_t i = 0; i < q.peer_count; ++i) {
					cap.peer_handles.push_back(q.peers[i].handle);
					cap.peer_radii.push_back(q.peers[i].radius);
				}
				cap.last_flags_in = q.entity_flags;
				q.entity_flags |= cap.or_flags;
				q.out_ground = cap.ground;
				return cap.return_clearance;
			});

	const int32_t ax = 100 << 16, ay = 20 << 16, az = 50 << 16;
	view.apply(nw::s2c::PER_FRAME_UPDATE,
	           nw::encode_frame_update(player_frame(kRowA, ax, ay, az, ax)));
	view.apply(nw::s2c::PER_FRAME_UPDATE,
	           nw::encode_frame_update(
	               player_frame(kRowB, ax, ay, az, ax + (2 << 16))));
	ns::ClientEntityState *row_a = view.state().find(kRowA);
	ns::ClientEntityState *row_b = view.state().find(kRowB);
	bool ok = true;
	ok &= expect(row_a != nullptr && row_b != nullptr, "both rows decode");
	if (!ok) return 1;
	row_a->rm_adm_id = 0;
	row_b->rm_adm_id = 0;

	// --- Tick 1: the fresh row SNAPS to its staged wire target (the first
	// compact stages; the chase's over-distance arm adopts it), then the
	// caller-owned org2 gravity integrates (vel -208, pos += vel) and the
	// resolver is consulted for each armed row with both rows in the peer
	// span.
	view.tick_remote_motion(/*self_handle=*/0xFFFF);
	ok &= expect(cap.calls == 2, "the resolver ran once per armed row");
	ok &= expect(cap.is_player, "a Player row resolves with the org2 shape");
	ok &= expect(cap.last_type_id == kPlayerType,
	             "the resolver receives the decoded runtime type identity");
	ok &= expect(cap.last_source_bound == 0x23456,
	             "the resolver receives the exact authored source bound");
	ok &= expect(cap.last_vel_z == -208,
	             "first tick integrates exactly one org2 gravity step");
	ok &= expect(row_a->rm_vel_z == -208 && row_b->rm_vel_z == -208,
	             "the row keeps its vertical velocity (hover return, no zero)");
	ok &= expect(cap.peer_handles.size() == 2,
	             "the peer span carries every live organic row");
	ok &= expect(cap.peer_radii.size() == 2 && cap.peer_radii[0] == 0x23456 &&
	             cap.peer_radii[1] == 0x23456,
	             "every replica peer sphere carries its authored bound");
	ok &= expect(row_a->z == az - 208,
	             "a positive clearance leaves the integrated fall in place");

	// --- Tick 2: a landing return (clearance -0x1200) lifts the row by the
	// clearance, zeroes the vertical velocity, and stores the ground hit.
	cap.return_clearance = -0x1200;
	cap.ground = 0x1006;
	const int32_t z_pre_land = row_a->z;
	view.tick_remote_motion(0xFFFF);
	ok &= expect(row_a->rm_vel_z == 0, "landing zeroes the vertical velocity");
	// This tick integrated one more step (-416 cumulative -> pos -416), then
	// lifted by +0x1200.
	ok &= expect(row_a->z == z_pre_land - 416 + 0x1200,
	             "landing lifts the row by the negative clearance");
	ok &= expect(row_a->resolved_ground == 0x1006,
	             "the ground-probe hit lands in resolved_ground");

	// --- The flag channel round-trip (D-NET-196 replica tails): the resolve's
	// latched bits persist on the row and gate the NEXT tick's gravity and
	// root channels. A CL/ladder-platform latch (0x100000) skips the gravity
	// step and zeroes the horizontal root channels — both motors store true
	// zeros [orig: gate @0x4b7ac8; xor edi,edi @0x4b797e/@0x4b79b7 (org2),
	// xor ebx,ebx @0x4bf600 (org1)].
	cap.return_clearance = 1000;
	cap.ground = 0xFFFF;
	cap.or_flags = 0x100000u;
	view.tick_remote_motion(0xFFFF); // this tick latches; gravity already ran
	ok &= expect((row_a->rm_entity_flags & 0x100000u) != 0,
	             "the resolver echo persists into rm_entity_flags");
	cap.or_flags = 0;
	const int32_t vel_before_gate = row_a->rm_vel_z;
	const int32_t x_before_gate = row_a->x;
	view.tick_remote_motion(0xFFFF);
	ok &= expect((cap.last_flags_in & 0x100000u) != 0,
	             "the next resolve sees the persisted flags word");
	ok &= expect(row_a->rm_vel_z == vel_before_gate,
	             "the 0x108000 gate skips the gravity step");
	ok &= expect(row_a->x == x_before_gate,
	             "CL contact zeroes the horizontal root channels (no drift)");

	// --- The airborne edge pair: clearance > 0xF000 latches 0x2000 (gated on
	// !(Flags & 0x10A002)); a grounded return clears it with the landing.
	// [orig: org2 edge @0x4b7e17..0x4b7e73; landing clear @0x4b7f7c..0x4b7fa1]
	row_a->rm_entity_flags = 0; // the real resolver's start-clear analog
	row_b->rm_entity_flags = 0;
	cap.return_clearance = 0x10000;
	view.tick_remote_motion(0xFFFF);
	ok &= expect((row_a->rm_entity_flags & 0x2000u) != 0,
	             "clearance > 0xF000 latches the airborne bit");
	cap.return_clearance = -0x100;
	view.tick_remote_motion(0xFFFF);
	ok &= expect((row_a->rm_entity_flags & 0x2000u) == 0,
	             "a grounded clearance clears the airborne bit");
	ok &= expect(row_a->rm_vel_z == 0, "the landing zeroes the velocity");
	// The suppressed edge: a pre-set CL contact keeps the ledge edge closed.
	row_a->rm_entity_flags = 0x100000u;
	cap.return_clearance = 0x10000;
	view.tick_remote_motion(0xFFFF);
	ok &= expect((row_a->rm_entity_flags & 0x2000u) == 0,
	             "the 0x10A002 gate suppresses the edge under CL contact");
	row_a->rm_entity_flags = 0;
	row_b->rm_entity_flags = 0;

	// --- The water/float channel, org2 buoyant-rise form (remote arm): a
	// submerged Player row rises ~0xBD/tick, latches (Flags & ~0x2000)|0x8000,
	// sets the dive bit while deep, and clamps at the surface line
	// water - 0x265 - 0xD000/2. [orig: @0x4b8020..0x4b8373]
	cap.return_clearance = 1000;
	{
		const int32_t z0 = row_a->z;
		const int32_t water = z0 + 0x18000;
		view.set_water_z(water, true);
		view.tick_remote_motion(0xFFFF);
		ok &= expect((row_a->rm_entity_flags & 0x8000u) != 0,
		             "a submerged row latches the float bit");
		ok &= expect((row_a->rm_entity_flags & 0x200000u) != 0,
		             "a deep row latches the dive bit");
		// The entering fall keeps sinking until the 1/32 drag decays the
		// carried vertical velocity below the rise rate (the unconditional
		// org2 vel add) — monotone only after the settle.
		int32_t last_z = row_a->z;
		bool rising = true;
		for (int i = 0; i < 480; ++i) {
			view.tick_remote_motion(0xFFFF);
			if (i >= 150 && row_a->z < last_z) rising = false;
			last_z = row_a->z;
		}
		const int32_t surf = water + (-0x4C9 >> 1) - (0xD000 >> 1);
		ok &= expect(rising, "the buoyant rise is monotone after the settle");
		ok &= expect(row_a->z == surf,
		             "the riser clamps exactly at the surface line");
		ok &= expect((row_a->rm_entity_flags & 0x200000u) == 0,
		             "surfacing clears the dive bit");
		// Leaving the water (row above the plane) clears float + dive.
		view.set_water_z(row_a->z - 0x20000, true);
		view.tick_remote_motion(0xFFFF);
		ok &= expect((row_a->rm_entity_flags & 0x208000u) == 0,
		             "the not-submerged exit clears the float and dive bits");
	}

	// --- The org1 snap form: an Infantry row quarter-chases the float target
	// water + bob - eye/2 - 0x4C9 (bob amplitude 1224). [orig: target
	// @0x4bfb2a..0x4bfb84; the quarter-step tail @0x4bfc65..0x4bfc86]
	{
		row_b->cls = nw::EntityClass::Infantry;
		const int32_t water = row_b->z + 0x18000;
		view.set_water_z(water, true);
		for (int i = 0; i < 80; ++i) view.tick_remote_motion(0xFFFF);
		const int32_t line = water - (0xD000 >> 1) - 0x4C9;
		const int32_t err = row_b->z - line;
		ok &= expect((row_b->rm_entity_flags & 0x8000u) != 0,
		             "the org1 row latches the float bit");
		ok &= expect(err > -(1224 + 620) && err < (1224 + 620),
		             "the org1 quarter-chase holds the bob band");
		view.set_water_z(0, false);
		view.tick_remote_motion(0xFFFF);
		ok &= expect((row_b->rm_entity_flags & 0x208000u) == 0,
		             "no water plane clears the float and dive bits");
		row_b->rm_entity_flags = 0;
		row_b->cls = nw::EntityClass::Player;
	}

	// --- The org2 ledge momentum carry + anim stamp and the airborne velocity
	// model: walking off a ledge banks 3/4 of the root step into the velocity
	// pair and stamps anim 31 straight [orig: @0x4b7e43..0x4b7e6d /
	// @0x4b7e3f..0x4b7e61]; in the air the root pair is zeroed, momentum owns
	// motion, and the MoveOrder-bit3 air-steer nudge plus the 63/64 damp run
	// [orig: @0x4b78a8..0x4b79dc].
	{
		view.set_water_z(0, false);
		cap.return_clearance = -0x100; // ground the row first
		view.tick_remote_motion(0xFFFF);
		row_a->rm_entity_flags = 0;
		row_b->rm_entity_flags = 0;
		row_a->rm_vel_xy[0] = 0;
		row_a->rm_vel_xy[1] = 0;
		still.dx = 1024; // heading 0x40000000 (90 deg): root lands on +y
		cap.return_clearance = 0x10000;
		const int32_t y_before_edge = row_a->y;
		view.tick_remote_motion(0xFFFF);
		ok &= expect(row_a->anim_state_id == 31,
		             "the ledge edge stamps anim 31 straight");
		ok &= expect(row_a->rm_vel_xy[1] == 768,
		             "the edge banks 3/4 of the root step into vel_y");
		ok &= expect(row_a->y == y_before_edge + 1024,
		             "the edge tick still integrates the full root step");
		// In-air tick: root zeroed, the +y nudge (dirpad 0, look +y;
		// ftol(sin*-64.0) truncates to -63 exactly as retail's _ftol2), then
		// the 63/64 damp: (768 + 63) * 63 >> 6 = 818.
		const int32_t y_before_air = row_a->y;
		view.tick_remote_motion(0xFFFF);
		ok &= expect(row_a->rm_vel_xy[1] == 818,
		             "in-air: nudge then the 63/64 damp");
		ok &= expect(row_a->y == y_before_air + 818,
		             "in-air motion is momentum-owned (root zeroed)");
		// Landing tick: the maintenance still ran airborne (the 0x2000 clear
		// lands post-resolve): nudge 63 then damp -> (818+63)*63>>6 = 867.
		still.dx = 0;
		cap.return_clearance = -0x100;
		view.tick_remote_motion(0xFFFF);
		ok &= expect((row_a->rm_entity_flags & 0x2000u) == 0,
		             "landing clears the airborne bit after the carry flight");
		ok &= expect(row_a->rm_vel_xy[1] == 867,
		             "the landing tick's maintenance was still airborne");
		// First grounded tick: the witnessed (7v+4)>>3 decay resumes.
		cap.return_clearance = 1000;
		view.tick_remote_motion(0xFFFF);
		ok &= expect(row_a->rm_vel_xy[1] == ((867 * 7 + 4) >> 3),
		             "grounded decay is the witnessed (7v+4)>>3");
		cap.return_clearance = 1000;
		for (int i = 0; i < 40; ++i) view.tick_remote_motion(0xFFFF);
		ok &= expect(row_a->rm_vel_xy[1] == 0,
		             "the |v| <= 8 snap parks the grounded decay at zero");
		row_a->anim_state_id = 62;
	}

	// --- The deck-ride: a row grounded on a carrier follows its live-vs-
	// savedLivePose delta, rotates about it on a yaw delta, and adopts the
	// yaw into its heading; straying beyond the carrier bound radius drops
	// the ride AND the ground link. The provider serves the live pose and
	// the mover-entry stamp; the test plays the carrier's mover by
	// restamping saved = live after each tick. [orig: org2 @0x4b52a0..0x4b5726]
	{
		view.set_water_z(0, false);
		cap.return_clearance = 1000;
		row_a->rm_entity_flags = 0;
		row_a->rm_vel_xy[0] = 0;
		row_a->rm_vel_xy[1] = 0;
		ns::ClientReplicaPipeline::CarrierPose carrier;
		carrier.pos[0] = row_a->x - (1 << 16);
		carrier.pos[1] = row_a->y;
		carrier.pos[2] = row_a->z - 0x8000;
		carrier.yaw = 0;
		carrier.pitch = 0;
		carrier.roll = 0;
		carrier.bound_radius = 8 << 16;
		auto restamp = [&carrier]() {
			carrier.saved_pos[0] = carrier.pos[0];
			carrier.saved_pos[1] = carrier.pos[1];
			carrier.saved_pos[2] = carrier.pos[2];
			carrier.saved_yaw = carrier.yaw;
			carrier.saved_pitch = carrier.pitch;
			carrier.saved_roll = carrier.roll;
		};
		restamp();
		view.set_carrier_pose_provider(
				[&carrier](uint16_t handle,
						ns::ClientReplicaPipeline::CarrierPose &out) -> bool {
					if (handle != 0x2009) return false;
					out = carrier;
					return true;
				});
		row_a->resolved_ground = 0x2009;
		cap.ground = 0x2009; // keep the probe echoing the carrier
		// A parked carrier (saved == live) contributes zero delta.
		const int32_t x_static = row_a->x;
		view.tick_remote_motion(0xFFFF);
		ok &= expect(row_a->x == x_static,
		             "a parked carrier contributes zero rider delta");
		// Translation follow: the carrier mover moved it +2u since its stamp.
		carrier.pos[0] += 2 << 16;
		const int32_t x_before = row_a->x;
		view.tick_remote_motion(0xFFFF);
		ok &= expect(row_a->x == x_before + (2 << 16),
		             "the rider follows the carrier translation delta");
		restamp();
		// Rotation about the carrier: +90 deg yaw turns the rider's +x offset
		// into +y and adopts the delta into the heading.
		const int32_t rel_x0 = row_a->x - carrier.pos[0];
		const int32_t heading0 = row_a->heading_bam;
		carrier.yaw = 0x40000000;
		view.tick_remote_motion(0xFFFF);
		const int32_t rel_x1 = row_a->x - carrier.pos[0];
		const int32_t rel_y1 = row_a->y - carrier.pos[1];
		ok &= expect(rel_x1 > -0x400 && rel_x1 < 0x400,
		             "a +90deg carrier yaw moves the +x offset off axis x");
		ok &= expect(rel_y1 > rel_x0 - 0x400 && rel_y1 < rel_x0 + 0x400,
		             "the +x offset lands on +y (rotate about the carrier)");
		ok &= expect(row_a->heading_bam == static_cast<int32_t>(
		                     static_cast<uint32_t>(heading0) + 0x40000000u),
		             "the rider heading adopts the carrier yaw delta");
		restamp();
		// The out-of-radius drop: the ride zeroes resolved_ground before the
		// probe echo re-lands it, and the rider holds still that tick.
		carrier.bound_radius = 1 << 14;
		const int32_t x_before_drop = row_a->x;
		carrier.pos[0] += 2 << 16; // a delta the dropped ride must NOT apply
		view.tick_remote_motion(0xFFFF);
		ok &= expect(row_a->x == x_before_drop,
		             "straying beyond the bound radius drops the ride");
	}

	std::printf("[contact-resolver] calls=%d vel_z=%d peers=%zu z=%d flags=%08x\n",
	            cap.calls, cap.last_vel_z, cap.peer_handles.size(), row_a->z,
	            row_a->rm_entity_flags);
	return ok ? 0 : 1;
}
