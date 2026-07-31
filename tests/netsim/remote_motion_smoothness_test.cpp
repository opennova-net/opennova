// Remote-entity between-update motion — the joiner-side smoothness contract.
//
// A retail host replicates each entity at a SUBRATE of the 62.5 Hz frame
// stream (priority + aging under the 600-byte g_entity_send_budget —
// connection_fan.cpp [orig: @0x50f070]; D-NET-154), so a given boat/peer
// record arrives every N frames. The retail CLIENT hides that cadence with
// per-class between-update movers (net-re §5.38e, D-NET-196): every compact
// read STAGES the smooth-target and the class mover chases the live pose one
// step per 62.5 Hz tick. This test drives the ported mover — the production
// fold (NetClientView::apply) in remote-motion mode plus tick_remote_motion —
// and asserts:
//   1. smoothness — bounded per-tick step + no majority-stall for motion the
//      witnessed chase sustains (players at run speed; vehicles at moderate
//      speed with the speed register staged);
//   2. the witnessed edges — the >2 m one-tick snap, the position deadband,
//      and the death->respawn snap (D-NET-66).
// SUSTAIN LIMIT (deliberate, witnessed): the vehicle chase alone can follow at
// most snap_threshold/30 per tick (~12.5 m/s with the speed register >= 293,
// ~5 m/s without) — beyond that retail's PHYSICS-PREDICTION leg carries the
// motion and the chase only trims. That leg is the open D-NET-196 B-facet
// (family physics, D-NET-161); run_vehicle_fast_speed_snaps pins the honest
// chase-only behavior for a faster mover (periodic one-tick snaps, never a
// multi-tick hold).
//
// The control leg (gap=1) pins the full-rate regime; the chase must remain
// smooth when records arrive every tick.

#include "netsim/net_client_view.h"

#include <npwire/ingame_decode.h>
#include <npwire/ingame_encode.h>
#include <npwire/ingame_message_id.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

namespace nw = opennova;
namespace ns = opennova::netsim;

constexpr uint16_t kPlayerType = 0x14B9;
constexpr uint16_t kVehicleType = 0x054F;
constexpr int kGapTicks = 8;    // per-entity record cadence (budget rotation)
constexpr int kWarmupTicks = 32;
constexpr int kMeasureTicks = 64;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

nw::EntityClass class_of(uint16_t type_id) {
	if (type_id == kPlayerType) return nw::EntityClass::Player;
	if (type_id == kVehicleType) return nw::EntityClass::Vehicle;
	return nw::EntityClass::Unknown;
}

nw::FrameUpdate header_only_frame() {
	nw::FrameUpdate fu;
	fu.flags2 = 0;
	fu.mount_handle = 0xFFFF;
	fu.health = 100;
	return fu;
}

// One tag-1 player compact at world x (16.16), unmounted.
nw::FrameUpdate player_frame(uint16_t handle, int32_t ax, int32_t ay, int32_t az,
                             int32_t x, uint8_t state_flags = 0) {
	nw::FrameUpdate fu = header_only_frame();
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
	r.player.state_flags = state_flags;
	r.player.move_input_byte = 0x08; // moving (bit 3)
	r.player.anim_state_id = 62;
	r.player.anim_def_index = 0xFF; // null sentinel
	r.player.health_class_byte = 0x28;
	fu.records.push_back(r);
	return fu;
}

// One tag-1 live vehicle compact at world x (16.16), unparented, healthy. The
// speed register rides weapon_aim_y (retail vehicleData[177]); >= 293 selects
// the fast-vehicle 0x60000 snap threshold.
nw::FrameUpdate vehicle_frame(uint16_t handle, int32_t ax, int32_t ay, int32_t az,
                              int32_t x, uint16_t speed_reg = 300) {
	nw::FrameUpdate fu = header_only_frame();
	fu.anchor_x = ax;
	fu.anchor_y = ay;
	fu.anchor_z = az;
	nw::FrameUpdateRecord r;
	r.handle = handle;
	r.type_id = kVehicleType;
	r.cls = nw::EntityClass::Vehicle;
	r.vehicle.parent_slot_handle = 0xFFFF;
	r.vehicle.pos_x_compressed = nw::network_compress_fixedpoint(x - ax);
	r.vehicle.pos_y_compressed = nw::network_compress_fixedpoint(0);
	r.vehicle.pos_z_compressed = nw::network_compress_fixedpoint(0);
	r.vehicle.euler_z = 0x2000;
	r.vehicle.flags_byte = 0;      // live form
	r.vehicle.is_dead_pose = false;
	r.vehicle.health_word = 100;   // 0 would kill the vehicle every fold
	r.vehicle.weapon_aim_y = speed_reg;
	fu.records.push_back(r);
	return fu;
}

// Drive kWarmupTicks + kMeasureTicks client ticks against the PORTED mover:
// fold (staging), then tick_remote_motion (the chase), then sample — the
// production per-tick order (recv fold first, movers after; §5.38e §6).
bool run_leg(const char *label, uint16_t handle, int32_t step_fx,
             bool vehicle, int gap_ticks, ns::NetClientView &view) {
	const int32_t ax = 100 << 16, ay = 20 << 16, az = -50 << 16;
	std::vector<int32_t> presented;
	presented.reserve(kWarmupTicks + kMeasureTicks);

	for (int t = 0; t < kWarmupTicks + kMeasureTicks; ++t) {
		const int32_t true_x = ax + step_fx * t;
		nw::FrameUpdate fu = (t % gap_ticks == 0)
				? (vehicle ? vehicle_frame(handle, ax, ay, az, true_x)
				           : player_frame(handle, ax, ay, az, true_x))
				: header_only_frame();
		if (t % gap_ticks != 0) {
			fu.anchor_x = ax;
			fu.anchor_y = ay;
			fu.anchor_z = az;
		}
		view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
		view.tick_remote_motion(0xFFFF);
		const ns::ClientEntityState *es = view.state().find(handle);
		if (!expect(es != nullptr, "entity row exists after first fold")) return false;
		presented.push_back(es->x);
	}

	// Codec slack: compression error is bounded by |delta-from-anchor| >> 11 + 1
	// per sample; the path stays within a few metres of the anchor here.
	const int32_t slack = 512;
	const int32_t step_bound = 2 * step_fx + slack;
	const int32_t stall_thresh = step_fx / 4;

	int32_t max_step = 0;
	int stalled = 0;
	for (int t = kWarmupTicks; t < kWarmupTicks + kMeasureTicks; ++t) {
		const int32_t d = std::abs(presented[t] - presented[t - 1]);
		max_step = std::max(max_step, d);
		if (d < stall_thresh) ++stalled;
	}
	const double stall_frac = double(stalled) / double(kMeasureTicks);

	std::fprintf(stderr,
	             "[%s] gap=%d  true step/tick=%d (16.16)  measured max step=%d  "
	             "stalled ticks=%d/%d (%.1f%%)  last-gap steps:",
	             label, gap_ticks, step_fx, max_step, stalled, kMeasureTicks,
	             stall_frac * 100.0);
	for (int t = kWarmupTicks + kMeasureTicks - gap_ticks;
	     t < kWarmupTicks + kMeasureTicks; ++t) {
		std::fprintf(stderr, " %d", presented[t] - presented[t - 1]);
	}
	std::fprintf(stderr, "\n");

	bool ok = true;
	ok &= expect(max_step <= step_bound,
	             "presented per-tick step stays bounded (no N-tick teleport)");
	ok &= expect(stall_frac <= 0.25,
	             "presented pose keeps moving while the entity is in motion");
	return ok;
}

// CONTROL: at gap=1 (record every tick) the chase must remain smooth — the
// walk-speed equilibrium sits exactly on the org2 ladder's tuned 7-bucket.
bool run_control_full_rate_is_smooth() {
	ns::NetClientView view(class_of);
	view.set_remote_motion_mode(true);
	return run_leg("control", 0x0006, 4096, /*vehicle=*/false, /*gap=*/1, view);
}

bool run_remote_player_glides_between_subrate_records() {
	ns::NetClientView view(class_of);
	view.set_remote_motion_mode(true);
	// 0.0625 m/tick = 3.9 m/s — infantry run speed.
	return run_leg("player", 0x0005, 4096, /*vehicle=*/false, kGapTicks, view);
}

bool run_remote_vehicle_glides_between_subrate_records() {
	ns::NetClientView view(class_of);
	view.set_remote_motion_mode(true);
	// Spawn the boat as a placed pool-1 entity first (the 0x0D world stream).
	nw::PoolSpawnRecord spawn;
	spawn.slot_id = 0x1007;
	spawn.item_type_id = kVehicleType;
	spawn.pos_x = 100 << 16;
	spawn.pos_y = 20 << 16;
	spawn.pos_z = -50 << 16;
	nw::PoolSpawnBatch batch;
	batch.records.push_back(spawn);
	view.apply(0x0D, nw::encode_pool_spawn_batch(batch));
	// 0.125 m/tick = 7.8 m/s — a boat at moderate throttle, within the chase's
	// witnessed sustain limit (snap_threshold/30 with the speed register set).
	return run_leg("vehicle", 0x1007, 8192, /*vehicle=*/true, kGapTicks, view);
}

// A sustained mover FASTER than the chase can follow (the witnessed limit):
// the pose must track via periodic ONE-TICK snaps — never a multi-tick hold
// while records keep arriving, and never unbounded lag. This is the honest
// chase-only behavior for fast vehicles until the prediction leg (D-NET-196
// B-facet) lands.
bool run_vehicle_fast_speed_snaps_not_stalls() {
	ns::NetClientView view(class_of);
	view.set_remote_motion_mode(true);
	const uint16_t handle = 0x1008;
	const int32_t ax = 100 << 16, ay = 20 << 16, az = -50 << 16;
	const int32_t step_fx = 16384; // 0.25 m/tick = 15.6 m/s
	int32_t max_lag = 0;
	int hold_run = 0, worst_hold_run = 0;
	int32_t prev_x = 0;
	for (int t = 0; t < 256; ++t) {
		const int32_t true_x = ax + step_fx * t;
		nw::FrameUpdate fu = (t % 4 == 0)
				? vehicle_frame(handle, ax, ay, az, true_x)
				: header_only_frame();
		if (t % 4 != 0) { fu.anchor_x = ax; fu.anchor_y = ay; fu.anchor_z = az; }
		view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
		view.tick_remote_motion(0xFFFF);
		const ns::ClientEntityState *es = view.state().find(handle);
		if (!expect(es != nullptr, "fast vehicle row exists")) return false;
		if (t > 32) {
			max_lag = std::max(max_lag, true_x - es->x);
			if (es->x == prev_x) { ++hold_run; worst_hold_run = std::max(worst_hold_run, hold_run); }
			else hold_run = 0;
		}
		prev_x = es->x;
	}
	std::fprintf(stderr, "[fast-vehicle] max lag=%d (16.16)  worst hold run=%d ticks\n",
	             max_lag, worst_hold_run);
	bool ok = true;
	// Lag stays bounded by the fast snap threshold + one gap of motion.
	ok &= expect(max_lag <= 0x60000 + 4 * step_fx + 512,
	             "fast vehicle lag bounded by the snap threshold");
	// Post-snap the target is consumed; the pose may hold until the NEXT record
	// (<= one gap) — bounded, unlike the ZOH's cadence-long holds.
	ok &= expect(worst_hold_run <= 4,
	             "fast vehicle holds are bounded by one record gap");
	return ok;
}

// The witnessed >2 m edge: a teleport-sized jump lands in ONE tick (the snap
// branch), not a long glide [orig: @0x4b9a8c dist>0x20000 / org2 @0x4B4470+].
bool run_player_large_jump_snaps_in_one_tick() {
	ns::NetClientView view(class_of);
	view.set_remote_motion_mode(true);
	const uint16_t handle = 0x0009;
	const int32_t ax = 100 << 16, ay = 20 << 16, az = -50 << 16;
	const int32_t x0 = ax;
	view.apply(nw::s2c::PER_FRAME_UPDATE,
	           nw::encode_frame_update(player_frame(handle, ax, ay, az, x0)));
	view.tick_remote_motion(0xFFFF); // arms + snaps to the first sample
	// A 5 m jump (>0x20000 = 2 m):
	const int32_t x1 = x0 + (5 << 16);
	view.apply(nw::s2c::PER_FRAME_UPDATE,
	           nw::encode_frame_update(player_frame(handle, ax, ay, az, x1)));
	view.tick_remote_motion(0xFFFF);
	const ns::ClientEntityState *es = view.state().find(handle);
	if (!expect(es != nullptr, "row exists")) return false;
	const int32_t err = std::abs(es->x - x1);
	return expect(err <= 512, "a >2m jump snaps to the target in one tick");
}

// The witnessed position deadband: sub-0x2AAA (org2) jitter is IGNORED — the
// live pose does not creep on tiny deltas.
bool run_player_deadband_ignores_jitter() {
	ns::NetClientView view(class_of);
	view.set_remote_motion_mode(true);
	const uint16_t handle = 0x000A;
	const int32_t ax = 100 << 16, ay = 20 << 16, az = -50 << 16;
	view.apply(nw::s2c::PER_FRAME_UPDATE,
	           nw::encode_frame_update(player_frame(handle, ax, ay, az, ax)));
	view.tick_remote_motion(0xFFFF); // snap-arm at ax
	const int32_t settled = view.state().find(handle)->x;
	// 0.1 m jitter (< 0x2AAA):
	const int32_t jitter = settled + 6554;
	for (int t = 0; t < 16; ++t) {
		view.apply(nw::s2c::PER_FRAME_UPDATE,
		           nw::encode_frame_update(player_frame(handle, ax, ay, az, jitter)));
		view.tick_remote_motion(0xFFFF);
	}
	const ns::ClientEntityState *es = view.state().find(handle);
	return expect(std::abs(es->x - settled) <= 512,
	              "sub-deadband jitter does not move the live pose");
}

// Death -> respawn stays a SNAP (D-NET-66): no corpse-to-spawn glide.
bool run_respawn_snaps_without_glide() {
	ns::NetClientView view(class_of);
	view.set_remote_motion_mode(true);
	const uint16_t handle = 0x000B;
	const int32_t ax = 100 << 16, ay = 20 << 16, az = -50 << 16;
	view.apply(nw::s2c::PER_FRAME_UPDATE,
	           nw::encode_frame_update(player_frame(handle, ax, ay, az, ax)));
	view.tick_remote_motion(0xFFFF);
	// Dead record (wire bit1). Position must not move (dead path skips it).
	view.apply(nw::s2c::PER_FRAME_UPDATE,
	           nw::encode_frame_update(
	                   player_frame(handle, ax, ay, az, ax + (50 << 16), 0x02)));
	view.tick_remote_motion(0xFFFF);
	const int32_t death_x = view.state().find(handle)->x;
	if (!expect(std::abs(death_x - ax) <= 512,
	            "a wire-dead record does not move the pose")) return false;
	// Respawn 80 m away: the dead->alive edge snaps in one fold.
	const int32_t spawn_x = ax + (80 << 16);
	view.apply(nw::s2c::PER_FRAME_UPDATE,
	           nw::encode_frame_update(player_frame(handle, ax, ay, az, spawn_x)));
	view.tick_remote_motion(0xFFFF);
	const ns::ClientEntityState *es = view.state().find(handle);
	// 80 m from the anchor: compression error is ~ |delta| >> 11 ≈ 2560.
	if (!expect(std::abs(es->x - spawn_x) <= 4096,
	            "the respawn edge snaps to the spawn point")) return false;
	return expect(es->respawn_revision == 1, "the respawn edge was counted once");
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_control_full_rate_is_smooth();
	ok &= run_remote_player_glides_between_subrate_records();
	ok &= run_remote_vehicle_glides_between_subrate_records();
	ok &= run_vehicle_fast_speed_snaps_not_stalls();
	ok &= run_player_large_jump_snaps_in_one_tick();
	ok &= run_player_deadband_ignores_jitter();
	ok &= run_respawn_snaps_without_glide();
	if (!ok) {
		std::fprintf(stderr, "remote_motion_smoothness: FAILED\n");
		return EXIT_FAILURE;
	}
	std::printf("remote_motion_smoothness: OK\n");
	return EXIT_SUCCESS;
}
