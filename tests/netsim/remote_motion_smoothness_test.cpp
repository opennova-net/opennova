// Remote-entity between-update motion — the joiner-side smoothness contract.
//
// Repro for the live-join choppiness: a retail host replicates each entity at a
// SUBRATE of the 62.5 Hz frame stream (priority + aging under the 600-byte
// g_entity_send_budget — connection_fan.cpp [orig: @0x50f070]; a populated
// mission fills the budget, D-NET-154), so a given boat/peer record arrives
// every N frames. The retail CLIENT hides that cadence by stepping the live
// pose toward the received pose each tick (the witnessed motor fall-through
// [orig: Entity_UpdateInfantryAI @ 0x4b9a8c], §5.38a — "client-only,
// intentionally NOT ported ... ready to port when a non-authority client path
// exists"). Our joiner renders decoded ClientEntityState rows directly
// (D-NET-188): each 0x0A fold OVERWRITES the row and nothing moves it between
// folds, so the presented pose holds still for N-1 ticks and teleports on the
// fold tick — the reported skipping.
//
// This test drives the production fold (NetClientView::apply on encoded 0x0A
// frames — the exact store present_snapshot_from_client_view copies into
// PF_POS_*) with a constant-velocity entity delivered at a 1-in-8 tick subrate,
// and asserts the SYMPTOM contract on the presented pose:
//   1. bounded step — max per-tick displacement <= 2x the true per-tick motion
//      (+ codec slack), not an N-tick accumulation;
//   2. continuity — the pose may not stand still for the majority of ticks
//      while the entity is in continuous motion.
// The witnessed retail chase satisfies both in this regime (equilibrium lag
// ~16x step < the 0x20000 snap threshold; per-tick chase step ~= true step).
// The contract is symptom-level, not a mechanism spec: the fix must be the
// faithful port, never an invented smoother.
//
// EXPECTED RED until the client-side smoothing port lands. Vehicle leg note:
// the vehicle-class between-update mover is NOT yet witnessed (the @0x4b9a8c
// chase is the infantry motor; §5.13 only witnesses driver-side prediction) —
// witness it via IDA before porting; this leg pins the symptom either way.

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
constexpr int kWarmupTicks = 16;
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

// One tag-1 player compact at world x (16.16), alive, unmounted.
nw::FrameUpdate player_frame(uint16_t handle, int32_t ax, int32_t ay, int32_t az,
                             int32_t x) {
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
	r.player.state_flags = 0;      // alive/deployed
	r.player.anim_state_id = 62;
	r.player.anim_def_index = 0xFF; // null sentinel
	r.player.health_class_byte = 0x28;
	fu.records.push_back(r);
	return fu;
}

// One tag-1 live vehicle compact at world x (16.16), unparented, healthy.
nw::FrameUpdate vehicle_frame(uint16_t handle, int32_t ax, int32_t ay, int32_t az,
                              int32_t x) {
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
	fu.records.push_back(r);
	return fu;
}

// Drive kWarmupTicks + kMeasureTicks client ticks. The entity truly moves
// step_fx (16.16) per tick along +X; its record is delivered on every
// kGapTicks-th tick (all other ticks carry a header-only 0x0A, like a busy
// host's frames that rotated this entity out). Sample the presented x after
// every tick and evaluate the contract over the measurement window.
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

// CONTROL: at gap=1 (record every tick, the small-mission regime) zero-order
// hold IS smooth — per-tick steps equal the true step. Must pass even before
// the smoothing port; proves the contract measures the cadence interaction.
bool run_control_full_rate_is_smooth() {
	ns::NetClientView view(class_of);
	return run_leg("control", 0x0006, 4096, /*vehicle=*/false, /*gap=*/1, view);
}

bool run_remote_player_glides_between_subrate_records() {
	ns::NetClientView view(class_of);
	// 0.0625 m/tick = 3.9 m/s — infantry run speed.
	return run_leg("player", 0x0005, 4096, /*vehicle=*/false, kGapTicks, view);
}

bool run_remote_vehicle_glides_between_subrate_records() {
	ns::NetClientView view(class_of);
	// Spawn the boat as a placed pool-1 entity first (the 0x0D world stream),
	// like retail streams placed vehicles; live compacts then update the row.
	nw::PoolSpawnRecord spawn;
	spawn.slot_id = 0x1007;
	spawn.item_type_id = kVehicleType;
	spawn.pos_x = 100 << 16;
	spawn.pos_y = 20 << 16;
	spawn.pos_z = -50 << 16;
	nw::PoolSpawnBatch batch;
	batch.records.push_back(spawn);
	view.apply(0x0D, nw::encode_pool_spawn_batch(batch));
	// 0.25 m/tick = 15.6 m/s — a boat under way.
	return run_leg("vehicle", 0x1007, 16384, /*vehicle=*/true, kGapTicks, view);
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_control_full_rate_is_smooth();
	ok &= run_remote_player_glides_between_subrate_records();
	ok &= run_remote_vehicle_glides_between_subrate_records();
	if (!ok) {
		std::fprintf(stderr,
		             "remote_motion_smoothness: RED — the joiner presents "
		             "zero-order-held wire samples (no between-update motion)\n");
		return EXIT_FAILURE;
	}
	std::printf("remote_motion_smoothness: OK\n");
	return EXIT_SUCCESS;
}
