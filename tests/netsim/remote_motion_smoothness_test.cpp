// Remote-entity between-update motion — the joiner-side smoothness contract.
//
// A retail host replicates each entity at a SUBRATE of the 62.5 Hz frame
// stream (priority + aging under the 600-byte g_entity_send_budget —
// connection_fan.cpp [orig: @0x50f070]; D-NET-154), so a given boat/peer
// record arrives every N frames. The retail CLIENT hides that cadence with
// per-class between-update movers (net-re §5.38e, D-NET-196): every compact
// read STAGES the smooth-target and the class mover chases the live pose one
// step per 62.5 Hz tick. This test drives the ported mover — the production
// fold (ClientReplicaPipeline::apply) in remote-motion mode plus tick_remote_motion —
// and asserts:
//   1. smoothness — bounded per-tick step + no majority-stall for motion the
//      witnessed chase sustains (players at run speed; vehicles at moderate
//      speed with the speed register staged);
//   2. the witnessed edges — the >2 m one-tick snap, the position deadband,
//      and the death->respawn snap (D-NET-66).
// SUSTAIN LIMIT (deliberate, witnessed): the vehicle chase alone can follow at
// most snap_threshold/30 per tick (~12.5 m/s with the speed register >= 293,
// ~5 m/s without) — beyond that retail's PHYSICS-PREDICTION leg carries the
// motion and the chase only trims. The prediction legs are LANDED for all four
// families as world-side movers (world/vehicle_motor, exercised by
// watercraft_client_motor_test); the row-side chase pinned here remains the
// shipped path for traitless vehicles and lib-only embedders, so
// run_vehicle_fast_speed_snaps pins its honest chase-only behavior for a
// faster mover (periodic one-tick snaps, never a multi-tick hold).
//
// The control leg (gap=1) pins the full-rate regime; the chase must remain
// smooth when records arrive every tick.

#include <runtime/replication/client_replica_pipeline.h>

#include <runtime/terrain_query/height_field.h>
#include <runtime/world/infantry.h> // IRootMotionSource stub for the root-motion legs

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>
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
// speed rides weapon_aim_y compressed (retail vehicleData[177], decompressed
// at the store); decompressed >= 293 (~0.28 m/s) selects the 0x60000 snap.
nw::FrameUpdate vehicle_frame(uint16_t handle, int32_t ax, int32_t ay, int32_t az,
                              int32_t x, int32_t speed_fx = 8192) {
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
	r.vehicle.weapon_aim_y = nw::network_compress_fixedpoint(speed_fx);
	fu.records.push_back(r);
	return fu;
}

// Drive kWarmupTicks + kMeasureTicks client ticks against the PORTED mover:
// fold (staging), then tick_remote_motion (the chase), then sample — the
// production per-tick order (recv fold first, movers after; §5.38e §6).
bool run_leg(const char *label, uint16_t handle, int32_t step_fx,
             bool vehicle, int gap_ticks, ns::ClientReplicaPipeline &view) {
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
	ns::ClientReplicaPipeline view(class_of);
	view.set_remote_motion_mode(true);
	return run_leg("control", 0x0006, 4096, /*vehicle=*/false, /*gap=*/1, view);
}

bool run_remote_player_glides_between_subrate_records() {
	ns::ClientReplicaPipeline view(class_of);
	view.set_remote_motion_mode(true);
	// 0.0625 m/tick = 3.9 m/s — infantry run speed.
	return run_leg("player", 0x0005, 4096, /*vehicle=*/false, kGapTicks, view);
}

bool run_remote_vehicle_glides_between_subrate_records() {
	ns::ClientReplicaPipeline view(class_of);
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
// chase-only behavior on rows without a world-side prediction mover (the
// prediction legs live in world/vehicle_motor and are pinned separately).
bool run_vehicle_fast_speed_snaps_not_stalls() {
	ns::ClientReplicaPipeline view(class_of);
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
	ns::ClientReplicaPipeline view(class_of);
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
	ns::ClientReplicaPipeline view(class_of);
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
	ns::ClientReplicaPipeline view(class_of);
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

// Pin the org2 bucket ladder's exact per-step math at three distances,
// INCLUDING the verbatim non-monotonic [0x6000,0x7000) -> 7 rung — a silent
// "monotonicizing fix" must fail here [orig: the ladder @0x4B44C4..0x4B4581;
// step = (d + N/2) / N re-store @0x4B459D]. The staged target is read back
// from the row so codec quantization cancels out of the expectation.
bool run_org2_bucket_ladder_is_verbatim() {
	struct Probe { int32_t dist; int32_t bucket; };
	// Distances FROM the settled pose, chosen mid-rung.
	const Probe probes[] = {
			{0x3800, 7},  // [0x3000,0x4000) -> 7
			{0x5800, 9},  // [0x5000,0x6000) -> 9
			{0x6800, 7},  // [0x6000,0x7000) -> 7 (the non-monotonic rung)
	};
	bool ok = true;
	uint16_t next_handle = 0x0020;
	for (const Probe &p : probes) {
		ns::ClientReplicaPipeline view(class_of);
		view.set_remote_motion_mode(true);
		const uint16_t handle = next_handle++;
		const int32_t ax = 100 << 16, ay = 20 << 16, az = -50 << 16;
		view.apply(nw::s2c::PER_FRAME_UPDATE,
		           nw::encode_frame_update(player_frame(handle, ax, ay, az, ax)));
		view.tick_remote_motion(0xFFFF); // snap-arm at ax
		const int32_t x0 = view.state().find(handle)->x;
		view.apply(nw::s2c::PER_FRAME_UPDATE,
		           nw::encode_frame_update(
		                   player_frame(handle, ax, ay, az, x0 + p.dist)));
		// The staged ABSOLUTE target (post-codec) before the first chase tick.
		const int32_t target = view.state().find(handle)->net_smooth_target[0];
		const int32_t d = target - x0;
		const int32_t expected = (d + (p.bucket >> 1)) / p.bucket;
		view.tick_remote_motion(0xFFFF);
		const int32_t stepped = view.state().find(handle)->x - x0;
		std::fprintf(stderr,
		             "[org2-ladder] dist=0x%X bucket=%d staged d=%d step=%d expected=%d\n",
		             unsigned(p.dist), p.bucket, d, stepped, expected);
		ok &= expect(stepped == expected,
		             "org2 ladder step is the verbatim (d + N/2) / N of its rung");
	}
	return ok;
}

// The vehicle-family ladder pinned the same way [orig: @0x48D480 —
// {6,8,10,15,20,25,30}]: 0x9000 sits in [0x8000,0x10000) -> 20.
bool run_vehicle_bucket_ladder_is_verbatim() {
	ns::ClientReplicaPipeline view(class_of);
	view.set_remote_motion_mode(true);
	const uint16_t handle = 0x1030;
	const int32_t ax = 100 << 16, ay = 20 << 16, az = -50 << 16;
	view.apply(nw::s2c::PER_FRAME_UPDATE,
	           nw::encode_frame_update(vehicle_frame(handle, ax, ay, az, ax)));
	view.tick_remote_motion(0xFFFF); // snap-arm
	const int32_t x0 = view.state().find(handle)->x;
	view.apply(nw::s2c::PER_FRAME_UPDATE,
	           nw::encode_frame_update(
	                   vehicle_frame(handle, ax, ay, az, x0 + 0x9000)));
	const int32_t target = view.state().find(handle)->net_smooth_target[0];
	const int32_t d = target - x0;
	const int32_t expected = (d + 10) / 20;
	view.tick_remote_motion(0xFFFF);
	const int32_t stepped = view.state().find(handle)->x - x0;
	std::fprintf(stderr, "[vehicle-ladder] staged d=%d step=%d expected=%d\n",
	             d, stepped, expected);
	return expect(stepped == expected,
	              "vehicle ladder 0x9000 takes the 20-bucket verbatim step");
}

// The starvation decay drains the FULL int32 decompressed register — signed,
// untruncated [orig: (v+64)>>7 on vehicleData[177], a signed 32-bit register;
// the < 293 compare is signed]. A reversing boat must decay toward zero from
// below (never wrap positive and flip the 0x60000 snap gate); a fast aircraft
// register above 0xFFFF must keep its high bits.
bool run_vehicle_starvation_decay_is_signed_untruncated() {
	struct Probe { int32_t speed_fx; const char *label; };
	const Probe probes[] = {
			{-8192, "reverse"},
			{131072, "fast (2.0 u/tick)"},
	};
	bool ok = true;
	uint16_t next_handle = 0x1040;
	for (const Probe &p : probes) {
		ns::ClientReplicaPipeline view(class_of);
		view.set_remote_motion_mode(true);
		const uint16_t handle = next_handle++;
		const int32_t ax = 100 << 16, ay = 20 << 16, az = -50 << 16;
		view.apply(nw::s2c::PER_FRAME_UPDATE,
		           nw::encode_frame_update(
		                   vehicle_frame(handle, ax, ay, az, ax, p.speed_fx)));
		const int32_t seeded = view.state().find(handle)->vehicle_speed_reg;
		// Starve: header-only frames until well past the >=128 decay threshold.
		int32_t prev = seeded;
		bool monotone_toward_zero = true;
		for (int t = 0; t < 200; ++t) {
			nw::FrameUpdate fu = header_only_frame();
			fu.anchor_x = ax; fu.anchor_y = ay; fu.anchor_z = az;
			view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
			view.tick_remote_motion(0xFFFF);
			const int32_t reg = view.state().find(handle)->vehicle_speed_reg;
			if (std::abs(reg) > std::abs(prev) ||
					(seeded < 0 && reg > 0) || (seeded > 0 && reg < 0))
				monotone_toward_zero = false;
			prev = reg;
		}
		std::fprintf(stderr, "[starvation %s] seeded=%d final=%d\n",
		             p.label, seeded, prev);
		ok &= expect(monotone_toward_zero,
		             "the starved speed register decays toward zero without "
		             "sign flips or 16-bit wraps");
	}
	return ok;
}

// The org2 pitch chase (this PR's port of the +0x244 leg): a fresh aim pitch
// steps the live pitch_bam with the same divisor-12 chase as heading — no
// zero-order hold at the wire cadence [orig: smoothTargetPitch staged in every
// branch of @0x4B4470..0x4B46C0; Pitch += step while progress < 12].
bool run_player_pitch_chases_wire_byte() {
	ns::ClientReplicaPipeline view(class_of);
	view.set_remote_motion_mode(true);
	const uint16_t handle = 0x0031;
	const int32_t ax = 100 << 16, ay = 20 << 16, az = -50 << 16;
	nw::FrameUpdate fu = player_frame(handle, ax, ay, az, ax);
	fu.records[0].player.pitch_byte = 0;
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
	view.tick_remote_motion(0xFFFF); // arm level
	// Aim up: pitch byte 0x20 (45 deg). Records keep coming at the same spot.
	const int32_t target = 0x20 << 24;
	int32_t last = view.state().find(handle)->pitch_bam;
	bool stepped_smoothly = true;
	for (int t = 0; t < 40; ++t) {
		nw::FrameUpdate up = player_frame(handle, ax, ay, az, ax);
		up.records[0].player.pitch_byte = 0x20;
		view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(up));
		view.tick_remote_motion(0xFFFF);
		const int32_t now = view.state().find(handle)->pitch_bam;
		// Never a whole-target jump in one tick, always monotone toward it.
		if (std::abs(now - last) > target / 4 || now < last)
			stepped_smoothly = false;
		last = now;
	}
	std::fprintf(stderr, "[pitch-chase] final pitch_bam=%d target=%d\n",
	             last, target);
	bool ok = expect(stepped_smoothly,
	                 "pitch steps smoothly (no wire-cadence hold, no jump)");
	ok &= expect(std::abs(last - target) <= target / 16,
	             "pitch converges onto the wire aim");
	return ok;
}

// The org1 first-compact heading seed: the promote publishes the FIRST
// record's own heading, not the zero-initialized stage — an AI soldier must
// not pirouette toward BAM 0 on its first record (the fold-side seed in
// land_compact_pose; retail rows are born with spawn orientation in every
// heading slot).
bool run_infantry_first_record_does_not_swing_to_zero() {
	ns::ClientReplicaPipeline view([](uint16_t type_id) {
		return type_id == 0x0777 ? nw::EntityClass::Infantry
		                         : nw::EntityClass::Unknown;
	});
	view.set_remote_motion_mode(true);
	const uint16_t handle = 0x0032;
	const int32_t ax = 100 << 16, ay = 20 << 16, az = -50 << 16;
	// Born from the organic spawn stream facing 180 deg, like every AI row.
	{
		nw::OrganicSpawnRecord spawn;
		spawn.slot_id = handle;
		spawn.has_body = true;
		spawn.item_type_id = 0x0777;
		spawn.pos_x = ax;
		spawn.pos_y = ay;
		spawn.pos_z = az;
		spawn.orientation =
				static_cast<int32_t>(static_cast<uint32_t>(0x80u) << 24);
		nw::OrganicSpawnBatch batch;
		batch.entity_count = 1;
		batch.records.push_back(spawn);
		view.apply(0x0C, nw::encode_organic_spawn_batch(batch));
		if (!expect(view.state().find(handle) != nullptr,
		            "the organic spawn created the row")) return false;
	}
	nw::FrameUpdate fu = header_only_frame();
	fu.anchor_x = ax; fu.anchor_y = ay; fu.anchor_z = az;
	nw::FrameUpdateRecord r;
	r.handle = handle;
	r.type_id = 0x0777;
	r.cls = nw::EntityClass::Infantry;
	r.infantry.vehicle_slot_handle = 0xFFFF;
	r.infantry.pos_x_compressed = nw::network_compress_fixedpoint(0);
	r.infantry.pos_y_compressed = nw::network_compress_fixedpoint(0);
	r.infantry.pos_z_compressed = nw::network_compress_fixedpoint(0);
	r.infantry.yaw_byte = 0x80; // facing 180 deg — max distance from BAM 0
	r.infantry.flags_byte = 0;
	fu.records.push_back(r);
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
	const int32_t wire_heading =
			static_cast<int32_t>(static_cast<uint32_t>(0x80u) << 24);
	// A record gap's worth of ticks with no further records: the promoted
	// target must be the record's own heading, so the chase holds it.
	for (int t = 0; t < 24; ++t) view.tick_remote_motion(0xFFFF);
	const int32_t heading = view.state().find(handle)->heading_bam;
	const int32_t err = std::abs(static_cast<int32_t>(
			static_cast<uint32_t>(heading) - static_cast<uint32_t>(wire_heading)));
	std::fprintf(stderr, "[org1-first-seed] heading=%d wire=%d err=%d\n",
	             heading, wire_heading, err);
	// With the pre-fix zero promote the chase swings ~5.8 deg/tick toward 0;
	// 24 ticks is > 90 deg of drift. Hold within ~2 deg.
	return expect(err <= 24000000, "first-record heading holds (no pirouette)");
}


// A deterministic walking clip: forward root delta per tick, looping. The
// IDA-shaped double convention matches infantry_test's TestSource.
struct WalkSource final : public opennova::world::IRootMotionSource {
	int32_t step = 4096; // forward u/tick (16.16)
	bool has_clip(int, int state) const override {
		return state == opennova::world::anim_state::kWalkForward ||
		       state == opennova::world::anim_state::kIdle;
	}
	bool advance(int, int state, int32_t &phase, opennova::world::RootMotionFrame &out) override {
		phase = (phase + 1) % 62;
		out = {};
		if (state == opennova::world::anim_state::kWalkForward) out.dx = step;
		return true;
	}
	int32_t clip_length_ticks(int, int, int /*variant*/) const override { return 62; }
};

struct StanceSource final : public opennova::world::IRootMotionSource {
	bool has_clip(int, int state) const override {
		return state == opennova::world::anim_state::kIdle ||
		       state == opennova::world::anim_state::kIdleCrouch;
	}
	bool advance(int, int state, int32_t &phase,
	             opennova::world::RootMotionFrame &out) override {
		phase = (phase + 1) % 62;
		out = {};
		out.capsule_bottom = state == opennova::world::anim_state::kIdle
				? 2 << 16
				: 1 << 16;
		out.capsule_top = out.capsule_bottom + (1 << 16);
		return true;
	}
	int32_t clip_length_ticks(int, int, int /*variant*/) const override { return 62; }
};

struct ResetBottomSource final : public opennova::world::IRootMotionSource {
	bool has_clip(int, int state) const override {
		return state == opennova::world::anim_state::kClimbIdle;
	}
	bool advance(int, int, int32_t &phase,
	             opennova::world::RootMotionFrame &out) override {
		phase = (phase + 1) % 62;
		out = {};
		out.dz = 1024;
		out.capsule_bottom = 1 << 16;
		out.capsule_top = 2 << 16;
		return true;
	}
	int32_t clip_length_ticks(int, int, int /*variant*/) const override { return 62; }
};

struct FlatTerrain {
	static constexpr int kDim = 512;
	std::vector<uint16_t> heightmap;
	std::vector<int> sector_grid;
	opennova::terrain::TerrainHeightField field;

	explicit FlatTerrain(int ground_units)
			: heightmap(kDim * kDim,
			            static_cast<uint16_t>(ground_units * 256)),
			  sector_grid(16 * 16, 1) {
		field.heightmap = heightmap.data();
		field.dim = kDim;
		field.layout.sector_grid = sector_grid.data();
	}
};

// A stance transition changes both the authoritative origin Z and the local
// AnimMap capsule bottom. The compact chase and root-bottom delta therefore
// overlap for a few ticks; retail runs its ground resolver after both and
// removes only penetration. [orig: Entity_UpdateInfantryPlayerBody root/Z
// integrate @0x4B7CB4..0x4B7CEF, resolver call @0x4B7CF4, <=0 lift
// @0x4B7CFE..0x4B7D0A]
bool run_crouch_transition_stays_on_terrain() {
	ns::ClientReplicaPipeline view(class_of);
	view.set_remote_motion_mode(true);
	StanceSource src;
	FlatTerrain terrain(50);
	view.set_root_motion_source(&src);
	view.set_remote_motion_terrain(&terrain.field);

	const uint16_t handle = 0x0040;
	const int32_t ax = 100 << 16, ay = 20 << 16;
	const int32_t ground = 50 << 16;
	const int32_t stand_bottom = 2 << 16;
	const int32_t crouch_bottom = 1 << 16;

	nw::FrameUpdate stand = player_frame(handle, ax, ay, ground, ax);
	stand.records[0].player.pos_z_compressed =
			nw::network_compress_fixedpoint(stand_bottom);
	stand.records[0].player.move_input_byte = 0;
	stand.records[0].player.anim_state_id =
			opennova::world::anim_state::kIdle;
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(stand));
	view.state().find(handle)->rm_adm_id = 0;
	view.tick_remote_motion(0xFFFF);

	bool ok = true;
	const ns::ClientEntityState *es = view.state().find(handle);
	ok &= expect(es != nullptr && es->rm_prev_bottom_live,
	             "the standing row armed its capsule-bottom channel");
	if (!es) return false;
	const int32_t stand_clearance = es->z - es->rm_prev_bottom - ground;
	ok &= expect(stand_clearance >= 0 && stand_clearance <= 0x17FF,
	             "the standing row starts within the resolver grid above terrain");

	for (int tick = 0; tick < 12; ++tick) {
		if (tick == 0) {
			nw::FrameUpdate crouch =
					player_frame(handle, ax, ay, ground, ax);
			crouch.records[0].player.pos_z_compressed =
					nw::network_compress_fixedpoint(crouch_bottom);
			crouch.records[0].player.move_input_byte = 0;
			crouch.records[0].player.anim_state_id =
					opennova::world::anim_state::kIdleCrouch;
			view.apply(nw::s2c::PER_FRAME_UPDATE,
			           nw::encode_frame_update(crouch));
		}
		view.tick_remote_motion(0xFFFF);
		es = view.state().find(handle);
		const int32_t clearance =
				es->z - es->rm_prev_bottom - ground;
		if (tick == 0)
			std::fprintf(stderr,
			             "[root-crouch-ground] first clearance=%d\n",
			             clearance);
		ok &= expect(clearance >= 0 && clearance <= 0x17FF,
		             "the crouch transition never sinks below terrain");
	}
	return ok;
}

// The shared settle must also be reached by the org1 compact branch.
// [orig: Entity_UpdateInfantryAI root integration @0x4BF684..0x4BF6A2;
// resolver caller @0x4BF7FA; Entity_MovementCollisionResolver @0x4B2BD0]
bool run_infantry_crouch_transition_stays_on_terrain() {
	ns::ClientReplicaPipeline view([](uint16_t type_id) {
		return type_id == 0x0777 ? nw::EntityClass::Infantry
		                         : nw::EntityClass::Unknown;
	});
	view.set_remote_motion_mode(true);
	StanceSource src;
	FlatTerrain terrain(50);
	view.set_root_motion_source(&src);
	view.set_remote_motion_terrain(&terrain.field);

	const uint16_t handle = 0x004C;
	const int32_t ax = 100 << 16, ay = 20 << 16;
	const int32_t ground = 50 << 16;
	auto stance_frame = [&](int32_t capsule_bottom, int state) {
		nw::FrameUpdate fu = header_only_frame();
		fu.anchor_x = ax;
		fu.anchor_y = ay;
		fu.anchor_z = ground;
		nw::FrameUpdateRecord r;
		r.handle = handle;
		r.type_id = 0x0777;
		r.cls = nw::EntityClass::Infantry;
		r.infantry.vehicle_slot_handle = 0xFFFF;
		r.infantry.pos_x_compressed = nw::network_compress_fixedpoint(0);
		r.infantry.pos_y_compressed = nw::network_compress_fixedpoint(0);
		r.infantry.pos_z_compressed =
				nw::network_compress_fixedpoint(capsule_bottom);
		r.infantry.anim_byte = static_cast<uint8_t>(state);
		r.infantry.flags_byte = 0;
		fu.records.push_back(r);
		return fu;
	};

	nw::FrameUpdate stand = stance_frame(
			2 << 16, opennova::world::anim_state::kIdle);
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(stand));
	ns::ClientEntityState *es = view.state().find(handle);
	if (!expect(es != nullptr, "the org1 standing row was decoded")) return false;
	es->rm_adm_id = 0;
	view.tick_remote_motion(0xFFFF);

	nw::FrameUpdate crouch = stance_frame(
			1 << 16, opennova::world::anim_state::kIdleCrouch);
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(crouch));
	bool ok = true;
	for (int tick = 0; tick < 12; ++tick) {
		view.tick_remote_motion(0xFFFF);
		es = view.state().find(handle);
		const int32_t clearance = es->z - es->rm_prev_bottom - ground;
		ok &= expect(clearance >= 0 && clearance <= 0x17FF,
		             "the org1 crouch transition never sinks below terrain");
	}
	return ok;
}

// The resolver's terrain leg is a quantized two-unit DOWNWARD probe, not an
// unconditional height clamp. A positive foot gap stays untouched, and a row
// whose whole probe starts below the outdoor surface must not teleport upward
// to it (the miss may only receive the resolver's sub-grid end-point settle).
// [orig: Entity_MovementCollisionResolver probe setup @0x4B3D6E..0x4B3D95;
// terrain hit-window in raycast_entity_collision @0x413785..0x4137CB,
// reached through Entity_RaycastGroundHeightAndObject @0x414320;
// Entity_UpdateInfantryPlayerBody <=0 caller lift @0x4B7CFE..0x4B7D0A]
bool run_terrain_settle_respects_retail_probe_window() {
	StanceSource src;
	FlatTerrain terrain(50);
	const int32_t ax = 100 << 16, ay = 20 << 16;
	const int32_t ground = 50 << 16;
	bool ok = true;

	auto tick_at_origin = [&](uint16_t handle, int32_t origin_z, int state) {
		ns::ClientReplicaPipeline view(class_of);
		view.set_remote_motion_mode(true);
		view.set_root_motion_source(&src);
		view.set_remote_motion_terrain(&terrain.field);
		nw::FrameUpdate fu = player_frame(handle, ax, ay, ground, ax);
		const uint16_t compressed =
				nw::network_compress_fixedpoint(origin_z - ground);
		fu.records[0].player.pos_z_compressed = compressed;
		fu.records[0].player.move_input_byte = 0;
		fu.records[0].player.anim_state_id = static_cast<uint8_t>(state);
		view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
		view.state().find(handle)->rm_adm_id = 0;
		const int32_t expected = ground +
				nw::network_decompress_fixedpoint(compressed);
		view.tick_remote_motion(0xFFFF);
		return std::pair<int32_t, int32_t>{expected,
		                                      view.state().find(handle)->z};
	};

	const auto positive = tick_at_origin(
			0x0049, ground + (1 << 16) + 0x8000,
			opennova::world::anim_state::kIdleCrouch);
	ok &= expect(positive.second == positive.first,
	             "positive foot clearance is not pulled down to terrain");

	const auto buried = tick_at_origin(
			0x004A, ground - (4 << 16),
			opennova::world::anim_state::kIdle);
	const int32_t buried_lift = buried.second - buried.first;
	ok &= expect(buried.second < ground && buried_lift >= 0 &&
	                     buried_lift <= 0x17FF,
	             "a buried row cannot teleport through a missed down-probe");
	return ok;
}

// The resolver tail uses ordinary x86 ADD/SUB registers, so its fixed-point
// clearance and correction wrap at INT32_MIN instead of invoking signed C++
// overflow. [orig: Entity_MovementCollisionResolver @0x4B3D83..0x4B3DA3;
// Entity_UpdateInfantryPlayerBody correction @0x4B7CFE..0x4B7D0A]
bool run_terrain_settle_wraps_the_fixedpoint_seam() {
	ns::ClientReplicaPipeline view(class_of);
	view.set_remote_motion_mode(true);
	StanceSource src;
	FlatTerrain terrain(0);
	view.set_root_motion_source(&src);
	view.set_remote_motion_terrain(&terrain.field);

	const uint16_t handle = 0x004D;
	nw::FrameUpdate fu = player_frame(handle, 0, 0, 0, 0);
	fu.records[0].player.move_input_byte = 0;
	fu.records[0].player.anim_state_id =
			opennova::world::anim_state::kIdle;
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
	ns::ClientEntityState *es = view.state().find(handle);
	if (!expect(es != nullptr, "the seam row was decoded")) return false;
	es->rm_adm_id = 0;
	es->z = std::numeric_limits<int32_t>::min() + 0x1000;
	es->net_interp_progress = 1; // keep the chase from replacing the seam pose
	view.tick_remote_motion(0xFFFF);

	return expect(es->z == std::numeric_limits<int32_t>::min() + 0x2000,
	              "the resolver correction wraps like retail x86 SUB");
}

// Climb and grenade-death clips clear the capsule-bottom history BEFORE every
// AnimMap update, including ticks where the state does not change. Therefore
// their authored dz remains authoritative instead of being replaced after the
// first tick by a constant capsule-bottom delta.
// [orig: AnimMap_UpdateEntity state tests @0x40B607..0x40B635 and unconditional
// per-update history clear at @0x40B637, before same-state test @0x40B643]
bool run_reset_bottom_state_keeps_raw_vertical_root() {
	ns::ClientReplicaPipeline view(class_of);
	view.set_remote_motion_mode(true);
	ResetBottomSource src;
	view.set_root_motion_source(&src);

	const uint16_t handle = 0x0048;
	const int32_t ax = 100 << 16, ay = 20 << 16;
	const int32_t z0 = -(50 << 16);
	nw::FrameUpdate fu = player_frame(handle, ax, ay, z0, ax);
	fu.records[0].player.move_input_byte = 0;
	fu.records[0].player.anim_state_id =
			opennova::world::anim_state::kClimbIdle;
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
	view.state().find(handle)->rm_adm_id = 0;

	for (int tick = 0; tick < 3; ++tick)
		view.tick_remote_motion(0xFFFF);
	return expect(view.state().find(handle)->z == z0 + 3 * 1024,
	              "reset-bottom clips preserve raw vertical root every tick");
}

// Retail's branchless x86 magnitude leaves INT32_MIN negative. At an exact
// half-turn idle-yaw seam that means the 5/30-degree replant tests do not fire;
// the later twist limiter still clamps both legs to yaw-0x30000000. Route the
// row helper through the defined BAM primitive so C++ overflow cannot change it.
// [orig: Entity_UpdateInfantryPlayerBody magnitude sequences
// @0x4B49A1..0x4B49A6/@0x4B49C4..0x4B49C9; twist clamp @0x4B4A21..0x4B4A43]
bool run_leg_chase_exact_half_turn_matches_x86_abs() {
	ns::ClientReplicaPipeline view(class_of);
	view.set_remote_motion_mode(true);
	StanceSource src;
	view.set_root_motion_source(&src);
	const uint16_t handle = 0x004B;
	const int32_t ax = 100 << 16, ay = 20 << 16, az = -(50 << 16);
	nw::FrameUpdate fu = player_frame(handle, ax, ay, az, ax);
	fu.records[0].player.move_input_byte = 0;
	fu.records[0].player.anim_state_id = opennova::world::anim_state::kIdle;
	fu.records[0].player.yaw_byte = 0x80;
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
	view.state().find(handle)->rm_adm_id = 0;
	view.tick_remote_motion(0xFFFF);

	ns::ClientEntityState *es = view.state().find(handle);
	es->rm_leg_yaw[0] = es->rm_leg_yaw[1] = 0;
	es->rm_leg_target[0] = es->rm_leg_target[1] = 0;
	es->rm_body_heading = 0;
	es->rm_leg_seeded = true;
	view.tick_remote_motion(0xFFFF);

	const int32_t clamped = 0x50000000;
	return expect(es->rm_leg_target[0] == 0 && es->rm_leg_target[1] == 0 &&
	                     es->rm_leg_yaw[0] == clamped &&
	                     es->rm_leg_yaw[1] == clamped &&
	                     es->rm_body_heading == clamped,
	              "exact half-turn keeps x86 magnitude and twist-clamp semantics");
}

// The root-motion dead-reckoning leg (net-re §5.38e; the D-NET-196 B-facet
// landed): with a walking clip whose root speed EQUALS the true speed, a
// remote player's row is carried by root motion every tick and the chase only
// trims — uniform per-tick steps at true speed, equilibrium lag well under
// the chase-only value, and the presented playhead is the sim channel.
bool run_player_root_motion_dead_reckons() {
	ns::ClientReplicaPipeline view(class_of);
	view.set_remote_motion_mode(true);
	WalkSource src;
	view.set_root_motion_source(&src);
	const uint16_t handle = 0x0041;
	const int32_t ax = 100 << 16, ay = 20 << 16, az = -50 << 16;
	const int32_t step_fx = 4096; // 3.9 m/s — walk speed, clip-matched
	std::vector<int32_t> presented;
	for (int t = 0; t < 32 + 64; ++t) {
		const int32_t true_x = ax + step_fx * t;
		nw::FrameUpdate fu = (t % 8 == 0)
				? player_frame(handle, ax, ay, az, true_x)
				: header_only_frame();
		if (t % 8 != 0) { fu.anchor_x = ax; fu.anchor_y = ay; fu.anchor_z = az; }
		if (t % 8 == 0) {
			fu.records[0].player.anim_state_id =
					opennova::world::anim_state::kWalkForward;
			fu.records[0].player.yaw_byte = 0; // engine BAM 0 = +X, the
			                                   // clip's forward axis
		}
		view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
		if (t == 0) {
			// The embedder's adm stamp (resolve_client_row_adm_ids twin).
			view.state().find(handle)->rm_adm_id = 0;
		}
		view.tick_remote_motion(0xFFFF);
		presented.push_back(view.state().find(handle)->x);
	}
	int32_t max_step = 0, min_step = INT32_MAX;
	for (int t = 32; t < 96; ++t) {
		const int32_t d = presented[t] - presented[t - 1];
		max_step = std::max(max_step, d);
		min_step = std::min(min_step, d);
	}
	const ns::ClientEntityState *es = view.state().find(handle);
	const int32_t lag = (ax + step_fx * 95) - presented.back();
	std::fprintf(stderr,
	             "[root-motion] steps [%d..%d] true=%d lag=%d state=%d phase=%d\n",
	             min_step, max_step, step_fx, lag, int(es->rm_state),
	             es->rm_phase);
	bool ok = true;
	// Root carries every tick: no stalls, steps within trim slack of true.
	ok &= expect(min_step >= step_fx - 1024 && max_step <= 2 * step_fx + 512,
	             "root motion carries the row between records (chase trims)");
	// Equilibrium: the lag parks inside the witnessed position deadband
	// (0x2AAA — the chase deliberately ignores sub-deadband error; root
	// motion carries the bulk exactly as §5.38e describes) plus codec slack.
	ok &= expect(std::abs(lag) <= 0x2AAA + 1024,
	             "root-carried lag parks inside the chase deadband");
	ok &= expect(es->rm_state == opennova::world::anim_state::kWalkForward &&
	                     es->rm_adm_id == 0,
	             "the sim channel is armed on the wire state");
	return ok;
}

// The starved idle force [orig: @0x4B465D — g_animStateFlagsTable bit0]: a
// movement state parked past the 512-progress cap must fall to idle 43, or
// the root motion walks the starved row forever.
bool run_starved_row_forces_idle() {
	ns::ClientReplicaPipeline view(class_of);
	view.set_remote_motion_mode(true);
	WalkSource src;
	view.set_root_motion_source(&src);
	const uint16_t handle = 0x0042;
	const int32_t ax = 100 << 16, ay = 20 << 16, az = -50 << 16;
	nw::FrameUpdate fu = player_frame(handle, ax, ay, az, ax);
	fu.records[0].player.anim_state_id =
			opennova::world::anim_state::kWalkForward;
	fu.records[0].player.yaw_byte = 0;
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
	view.state().find(handle)->rm_adm_id = 0;
	view.tick_remote_motion(0xFFFF); // snap-arm at the wire pose first
	const int32_t x0 = view.state().find(handle)->x;
	for (int t = 0; t < 560; ++t) {
		nw::FrameUpdate idlefu = header_only_frame();
		idlefu.anchor_x = ax; idlefu.anchor_y = ay; idlefu.anchor_z = az;
		view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(idlefu));
		view.tick_remote_motion(0xFFFF);
	}
	const ns::ClientEntityState *es = view.state().find(handle);
	const int32_t walked = std::abs(es->x - x0);
	std::fprintf(stderr, "[root-starve] state=%d walked=%d (16.16)\n",
	             int(es->anim_state_id), walked);
	bool ok = expect(es->anim_state_id == opennova::world::anim_state::kIdle,
	                 "the starved movement state falls to idle 43");
	// 512 capped ticks of walking at most — never unbounded treadmill. Allow
	// the pre-cap walk (progress ramps to 512) plus slack.
	ok &= expect(walked <= 512 * 4096 + (2 << 16),
	             "the starved row stops walking after the idle force");
	return ok;
}

// The own-player row gets NO root add (its motion is world-side prediction;
// the row chase is the 48/512 soft reconciliation only).
bool run_self_row_gets_no_root_add() {
	ns::ClientReplicaPipeline view(class_of);
	view.set_remote_motion_mode(true);
	WalkSource src;
	view.set_root_motion_source(&src);
	const uint16_t handle = 0x0043;
	const int32_t ax = 100 << 16, ay = 20 << 16, az = -50 << 16;
	nw::FrameUpdate fu = player_frame(handle, ax, ay, az, ax);
	fu.records[0].player.anim_state_id =
			opennova::world::anim_state::kWalkForward;
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
	view.state().find(handle)->rm_adm_id = 0;
	view.tick_remote_motion(handle); // arm at the wire pose (self)
	const int32_t x0 = view.state().find(handle)->x;
	for (int t = 0; t < 24; ++t) {
		nw::FrameUpdate same = player_frame(handle, ax, ay, az, ax);
		same.records[0].player.anim_state_id =
				opennova::world::anim_state::kWalkForward;
		view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(same));
		view.tick_remote_motion(handle);
	}
	const int32_t moved = std::abs(view.state().find(handle)->x - x0);
	std::fprintf(stderr, "[root-self] moved=%d\n", moved);
	return expect(moved <= 512,
	              "the own-player row never integrates root motion");
}


// Rotation pin at a non-trivial heading: facing +Y (engine BAM 0x40000000,
// yaw byte 0x40), the clip's forward delta must move the row in +Y with X
// static - a sign-flipped rotation cannot pass this and the +X leg at once.
bool run_root_rotation_follows_heading() {
	ns::ClientReplicaPipeline view(class_of);
	view.set_remote_motion_mode(true);
	WalkSource src;
	view.set_root_motion_source(&src);
	const uint16_t handle = 0x0044;
	const int32_t ax = 100 << 16, ay = 20 << 16, az = -50 << 16;
	nw::FrameUpdate fu = player_frame(handle, ax, ay, az, ax);
	fu.records[0].player.anim_state_id =
			opennova::world::anim_state::kWalkForward;
	fu.records[0].player.yaw_byte = 0x40; // engine 90 deg = +Y forward
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
	view.state().find(handle)->rm_adm_id = 0;
	view.tick_remote_motion(0xFFFF); // snap-arm
	const int32_t x0 = view.state().find(handle)->x;
	const int32_t y0 = view.state().find(handle)->y;
	for (int t = 0; t < 16; ++t) view.tick_remote_motion(0xFFFF);
	const ns::ClientEntityState *es = view.state().find(handle);
	std::fprintf(stderr, "[root-rotate] dX=%d dY=%d\n", es->x - x0, es->y - y0);
	bool ok = expect(es->y - y0 > 12 * 4096,
	                 "the +Y-facing clip walks the row in +Y");
	ok &= expect(std::abs(es->x - x0) <= 4096,
	             "the +Y-facing clip leaves X static");
	return ok;
}

// The org1 (Infantry) root path: the AI row dead-reckons by its clip too.
bool run_infantry_root_motion_dead_reckons() {
	ns::ClientReplicaPipeline view([](uint16_t type_id) {
		return type_id == 0x0777 ? nw::EntityClass::Infantry
		                         : nw::EntityClass::Unknown;
	});
	view.set_remote_motion_mode(true);
	WalkSource src;
	view.set_root_motion_source(&src);
	const uint16_t handle = 0x0045;
	const int32_t ax = 100 << 16, ay = 20 << 16, az = -50 << 16;
	nw::FrameUpdate fu = header_only_frame();
	fu.anchor_x = ax; fu.anchor_y = ay; fu.anchor_z = az;
	nw::FrameUpdateRecord r;
	r.handle = handle;
	r.type_id = 0x0777;
	r.cls = nw::EntityClass::Infantry;
	r.infantry.vehicle_slot_handle = 0xFFFF;
	r.infantry.pos_x_compressed = nw::network_compress_fixedpoint(0);
	r.infantry.pos_y_compressed = nw::network_compress_fixedpoint(0);
	r.infantry.pos_z_compressed = nw::network_compress_fixedpoint(0);
	r.infantry.yaw_byte = 0; // +X
	r.infantry.anim_byte = opennova::world::anim_state::kWalkForward;
	r.infantry.flags_byte = 0;
	fu.records.push_back(r);
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
	view.state().find(handle)->rm_adm_id = 0;
	view.tick_remote_motion(0xFFFF);
	const int32_t x0 = view.state().find(handle)->x;
	for (int t = 0; t < 16; ++t) view.tick_remote_motion(0xFFFF);
	const int32_t walked = view.state().find(handle)->x - x0;
	std::fprintf(stderr, "[root-org1] walked=%d\n", walked);
	return expect(walked > 12 * 4096,
	              "an org1 row dead-reckons by its clip too");
}

// The transition blend: walk -> idle ramps the root delta out over the
// 10-frame window instead of stopping dead.
bool run_root_transition_blends() {
	ns::ClientReplicaPipeline view(class_of);
	view.set_remote_motion_mode(true);
	WalkSource src;
	view.set_root_motion_source(&src);
	const uint16_t handle = 0x0046;
	const int32_t ax = 100 << 16, ay = 20 << 16, az = -50 << 16;
	nw::FrameUpdate fu = player_frame(handle, ax, ay, az, ax);
	fu.records[0].player.anim_state_id =
			opennova::world::anim_state::kWalkForward;
	fu.records[0].player.yaw_byte = 0;
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
	view.state().find(handle)->rm_adm_id = 0;
	view.tick_remote_motion(0xFFFF);
	for (int t = 0; t < 8; ++t) view.tick_remote_motion(0xFFFF); // steady walk
	// Stage the idle record AT the row's current spot so the chase sits in
	// its deadband and the measured motion is the pure blend ramp.
	nw::FrameUpdate idlefu = player_frame(handle, ax, ay, az,
	                                      view.state().find(handle)->x);
	idlefu.records[0].player.anim_state_id = opennova::world::anim_state::kIdle;
	idlefu.records[0].player.yaw_byte = 0;
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(idlefu));
	int32_t prev = view.state().find(handle)->x;
	int32_t first_step = -1, tenth_step = -1;
	for (int t = 0; t < 12; ++t) {
		view.tick_remote_motion(0xFFFF);
		const int32_t now = view.state().find(handle)->x;
		if (t == 0) first_step = now - prev;
		if (t == 10) tenth_step = now - prev;
		prev = now;
	}
	std::fprintf(stderr, "[root-blend] first=%d tenth=%d\n", first_step,
	             tenth_step);
	bool ok = expect(first_step > 1024,
	                 "the first blend tick still carries most of walk's delta");
	ok &= expect(tenth_step <= 512,
	             "the walk delta has ramped out by the window's end");
	return ok;
}

// The freeze/respawn lifecycle (the review-confirmed critical): a dead
// record DISARMS the channel - presentation falls back to the wire byte,
// the corpse stops walking, and the respawn re-arms fresh.
bool run_dead_row_disarms_and_respawn_rearms() {
	ns::ClientReplicaPipeline view(class_of);
	view.set_remote_motion_mode(true);
	WalkSource src;
	view.set_root_motion_source(&src);
	const uint16_t handle = 0x0047;
	const int32_t ax = 100 << 16, ay = 20 << 16, az = -50 << 16;
	nw::FrameUpdate fu = player_frame(handle, ax, ay, az, ax);
	fu.records[0].player.anim_state_id =
			opennova::world::anim_state::kWalkForward;
	fu.records[0].player.yaw_byte = 0;
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(fu));
	view.state().find(handle)->rm_adm_id = 0;
	view.tick_remote_motion(0xFFFF);
	for (int t = 0; t < 4; ++t) view.tick_remote_motion(0xFFFF);
	if (!expect(view.state().find(handle)->rm_state ==
	                    opennova::world::anim_state::kWalkForward,
	            "the walking row armed its channel")) return false;
	nw::FrameUpdate dead = player_frame(handle, ax, ay, az, ax, 0x02);
	dead.records[0].player.anim_state_id =
			opennova::world::anim_state::kDeathFire;
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(dead));
	const ns::ClientEntityState *es = view.state().find(handle);
	bool ok = expect(es->rm_state == -1,
	                 "the dead record disarms the channel (presentation falls "
	                 "back to the wire death byte)");
	const int32_t corpse_x = es->x;
	for (int t = 0; t < 24; ++t) view.tick_remote_motion(0xFFFF);
	ok &= expect(std::abs(view.state().find(handle)->x - corpse_x) <= 512,
	             "the corpse never walks by root motion");
	nw::FrameUpdate alive = player_frame(handle, ax, ay, az, ax + (60 << 16));
	alive.records[0].player.anim_state_id =
			opennova::world::anim_state::kWalkForward;
	alive.records[0].player.yaw_byte = 0;
	view.apply(nw::s2c::PER_FRAME_UPDATE, nw::encode_frame_update(alive));
	view.tick_remote_motion(0xFFFF);
	const ns::ClientEntityState *re = view.state().find(handle);
	ok &= expect(re->rm_state == opennova::world::anim_state::kWalkForward &&
	                     re->rm_blend_weight >= 1.0f,
	             "the respawned row re-arms fresh (no blend out of the "
	             "pre-death primary)");
	return ok;
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
	ok &= run_org2_bucket_ladder_is_verbatim();
	ok &= run_vehicle_bucket_ladder_is_verbatim();
	ok &= run_vehicle_starvation_decay_is_signed_untruncated();
	ok &= run_player_pitch_chases_wire_byte();
	ok &= run_infantry_first_record_does_not_swing_to_zero();
	ok &= run_crouch_transition_stays_on_terrain();
	ok &= run_infantry_crouch_transition_stays_on_terrain();
	ok &= run_terrain_settle_respects_retail_probe_window();
	ok &= run_terrain_settle_wraps_the_fixedpoint_seam();
	ok &= run_reset_bottom_state_keeps_raw_vertical_root();
	ok &= run_leg_chase_exact_half_turn_matches_x86_abs();
	ok &= run_player_root_motion_dead_reckons();
	ok &= run_starved_row_forces_idle();
	ok &= run_self_row_gets_no_root_add();
	ok &= run_root_rotation_follows_heading();
	ok &= run_infantry_root_motion_dead_reckons();
	ok &= run_root_transition_blends();
	ok &= run_dead_row_disarms_and_respawn_rearms();
	if (!ok) {
		std::fprintf(stderr, "remote_motion_smoothness: FAILED\n");
		return EXIT_FAILURE;
	}
	std::printf("remote_motion_smoothness: OK\n");
	return EXIT_SUCCESS;
}
