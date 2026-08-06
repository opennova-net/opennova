// The replica contact-resolver seam (net-re §5.38e, D-NET-196): with an
// embedder-provided resolver, each armed Player/Infantry row's settle runs the
// FULL movement collision resolver at the witnessed caller order — the
// caller-owned vertical velocity integrates first (org2 `vel -= 208; pos +=
// vel`), the resolver runs with the row pose and this tick's peer spheres, a
// non-positive clearance lifts the row and zeroes the vertical velocity, and
// the ground-probe hit lands in resolved_ground (retail's groundEntity).
// [orig: vertical add @0x4B7CE0..0x4B7CEF then the resolver call @0x4B7CF4
//  and lift @0x4B7CFE..0x4B7D0A; Entity_MovementCollisionResolver @0x4B2BD0]

#include "netsim/client_replica_pipeline.h"

#include <world/infantry.h>

#include <npwire/ingame_encode.h>
#include <npwire/ingame_message_id.h>

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
	bool has_clip(int, int) const override { return true; }
	int32_t clip_length_ticks(int, int) const override { return 1024; }
	bool advance(int, int, int32_t &phase,
	             opennova::world::RootMotionFrame &out) override {
		phase += 1024;
		out = {};
		out.capsule_bottom = 0x8000; // 0.5 u feet
		out.capsule_top = 0x1C000;
		return true;
	}
	bool advance_blended(int, int, int32_t &pphase, int, int32_t &tphase, float,
	                     opennova::world::RootMotionFrame &out) override {
		pphase += 1024;
		tphase += 1024;
		out = {};
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
	bool is_player = false;
	std::vector<uint16_t> peer_handles;
	int32_t return_clearance = 1000; // small positive: hovering, no lift
	uint16_t ground = 0xFFFF;
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

	ResolverCapture cap;
	view.set_replica_contact_resolver(
			[&cap](ns::ClientReplicaPipeline::ReplicaContactQuery &q) -> int32_t {
				++cap.calls;
				cap.last_vel_z = q.vel_z;
				cap.last_pos_z = q.pos[2];
				cap.last_capsule_bottom = q.capsule_bottom;
				cap.is_player = q.is_player_class;
				cap.peer_handles.clear();
				for (int32_t i = 0; i < q.peer_count; ++i)
					cap.peer_handles.push_back(q.peers[i].handle);
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
	ok &= expect(cap.last_vel_z == -208,
	             "first tick integrates exactly one org2 gravity step");
	ok &= expect(row_a->rm_vel_z == -208 && row_b->rm_vel_z == -208,
	             "the row keeps its vertical velocity (hover return, no zero)");
	ok &= expect(cap.peer_handles.size() == 2,
	             "the peer span carries every live organic row");
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

	std::printf("[contact-resolver] calls=%d vel_z=%d peers=%zu z=%d\n",
	            cap.calls, cap.last_vel_z, cap.peer_handles.size(), row_a->z);
	return ok ? 0 : 1;
}
