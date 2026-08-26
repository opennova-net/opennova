// The suspension spring leg through the live seams: the def keys land on the
// traits, the crash request / crashed latch polarity with its Flags-0x10
// replication, Entity_RespawnVehicle's field set, the crash tests over the
// inputs the port carries, the sink growth and its gates, the grounded and
// airborne spring loops, the post-contact sink reset and the tick tail, and
// the oscillator's in-place shock clamp on the traits table entry. The
// oscillator kernel's own decays are pinned in ground_conform_test; this file
// pins what the WIRING adds.
// [orig: ItemDef_ParsePhysicsProperty @0x49db5c/@0x49dbd4/@0x49dc10/@0x49db98;
//  Entity_RespawnVehicle @0x45FF40; Entity_ProcessWheeledVehicleSuspension
//  @0x46B1A6..0x46B213; Entity_ProcessTrackedVehiclePhysics — the crash tests
//  @0x47D745..0x47D7A8 + @0x47E793..0x47E7EE, the extend loop @0x47DB70..
//  0x47DBD1, the airborne loop @0x47E283..0x47E344, the grounded loop
//  @0x47E960..0x47EC1F, the tail @0x47EEEE; the bike test @0x47B32D..0x47B375]

#include "world/ai.h"
#include "world/entity.h"
#include "world/ground_conform.h"
#include "world/vehicle_attach.h"
#include "world/vehicle_motor.h"
#include "world/vehicle_suspension.h"
#include "world/world.h"

#include "def/def.h"

#include <cstdio>
#include <cstring>
#include <memory>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

// The four keys parse raw beside the rest of the physics block, and
// spring_comp is matched before its prefix. top_heavy parses for parity
// only — retail never reads it past the parser.
void test_def_keys_parse_raw() {
	const char *src =
			"begin \"Sprung\"\n"
			"  id 101291\n"
			"  type vehicle\n"
			"  physics 1\n"
			"  spring 8\n"
			"  spring_comp 20\n"
			"  shock 4\n"
			"  top_heavy 3\n"
			"end\n";
	DefItemsFile file;
	std::memset(&file, 0, sizeof(file));
	CHECK(def_parse_items_memory(reinterpret_cast<const uint8_t *>(src),
	                             std::strlen(src), &file) == 0,
			"parse");
	CHECK(file.count == 1, "one entry");
	if (file.count == 1) {
		const DefItemDef &d = file.entries[0];
		CHECK(d.spring == 8, "spring raw");
		CHECK(d.spring_comp == 20, "spring_comp raw, not eaten by `spring`");
		CHECK(d.shock == 4, "shock raw");
		CHECK(d.top_heavy == 3, "top_heavy raw (parity only)");
		CHECK(d.physics == 1, "the rest of the block still parses");
	}
	def_free_items(&file);
}

std::unique_ptr<World> make_world(bool authority) {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 4);
    w->registry.configure_pool(1, 4);
    w->logic_authority = authority;
    return w;
}

Entity &spawn_veh(World &w, EntityHandle &out) {
	Entity veh;
	veh.kind = EntityKind::Item;
	veh.item_id = 1291;
	veh.position = {100.0f, 200.0f, 10.0f};
	veh.health = 3000;
	veh.health_max = 3000;
	veh.alive = true;
	out = w.registry.spawn(1, veh);
	return *w.registry.get(out);
}

VehicleTraits sprung_traits() {
	VehicleTraits t;
	t.spring = 8;
	t.spring_comp = 20;
	t.shock = 4;
	t.mass = 20;
	t.flip = 45;
	return t;
}

// POLARITY: a fresh row (no request pending) never arms — the spawn-park the
// inverted gate produced is gone; the request arms, crashed blocks a re-arm.
void test_fresh_row_never_arms() {
	auto w_heap = make_world(true);
	World &w = *w_heap;
	EntityHandle h;
	Entity &veh = spawn_veh(w, h);
	for (int i = 0; i < 5; ++i)
		CHECK(!vehicle_suspension_arm(w, veh, false), "a fresh row never arms");
	CHECK(veh.veh.crashed == 0 && (veh.flags & kEntityFlagSuspensionCrashed) == 0,
			"no crashed byte, no Flags 0x10");
}

// The arming seed: picked ONCE by role, the authority raises Flags 0x10, a
// client without the bit does not arm, a client WITH it does.
void test_request_arms_by_role_and_replication() {
	{
		auto w_heap = make_world(true);
		World &w = *w_heap;
		EntityHandle h;
		Entity &veh = spawn_veh(w, h);
		veh.veh.crash_request = 1;
		veh.veh.landing_2ee = 1;
		veh.veh.byte_2ef = 1;
		CHECK(vehicle_suspension_arm(w, veh, false), "the authority arms on a request");
		CHECK(veh.veh.crashed == 1, "+0x2EC set");
		CHECK(veh.veh.susp_rate_pick == kSuspensionDisableRateAuthority,
				"the authority picks 1.25");
		CHECK((veh.flags & kEntityFlagSuspensionCrashed) != 0,
				"the authority raises Flags 0x10");
		CHECK(veh.veh.landing_2ee == 0 && veh.veh.byte_2ef == 0,
				"+0x2EE / +0x2EF zeroed at arming");
		CHECK(!vehicle_suspension_arm(w, veh, false), "a crashed row does not re-arm");
	}
	{
		auto w_heap = make_world(false);
		World &w = *w_heap;
		EntityHandle h;
		Entity &veh = spawn_veh(w, h);
		veh.veh.crash_request = 1;
		CHECK(!vehicle_suspension_arm(w, veh, false),
				"a client without the replicated bit does not arm");
		CHECK(veh.veh.crashed == 0 && (veh.flags & kEntityFlagSuspensionCrashed) == 0,
				"and writes neither the latch nor the flag");
		veh.flags |= kEntityFlagSuspensionCrashed; // the authority's bit arrives
		CHECK(vehicle_suspension_arm(w, veh, false), "a client with the bit arms");
		CHECK(veh.veh.susp_rate_pick == kSuspensionDisableRateNonAuthority,
				"the client picks 1.75");
	}
}

// Arming never touches the sinks, compressions or oscillators
// (Entity_ClearSuspensionState is the chassis matrix reset).
void test_arming_keeps_the_spring_state() {
	auto w_heap = make_world(true);
	World &w = *w_heap;
	EntityHandle h;
	Entity &veh = spawn_veh(w, h);
	veh.veh.plat_acc[1] = 400;
	veh.veh.wheel_comp[2] = 1234;
	veh.veh.wheel_osc[1].amplitude = 99;
	veh.veh.spring_energy = 7;
	veh.veh.crash_request = 1;
	CHECK(vehicle_suspension_arm(w, veh, false), "arm");
	CHECK(veh.veh.plat_acc[1] == 400 && veh.veh.wheel_comp[2] == 1234 &&
					veh.veh.wheel_osc[1].amplitude == 99 && veh.veh.spring_energy == 7,
			"arming leaves the spring state alone");
}

// Entity_RespawnVehicle's write set.
void test_respawn_field_set() {
	Entity::VehicleMotorState m;
	m.settle_2f0 = m.crashed = m.landing_2ee = m.settled_2f2 = m.wreck_2fc = 1;
	m.crash_request = 1;
	m.fresh_2f1 = 0;
	m.airborne_stamp_2f8 = 77;
	m.plat_acc[0] = 5;
	m.wheel_comp[0] = 9;
	vehicle_suspension_respawn(m);
	CHECK(m.settle_2f0 == 0 && m.crashed == 0 && m.landing_2ee == 0 &&
					m.settled_2f2 == 0 && m.wreck_2fc == 0 && m.crash_request == 0 &&
					m.airborne_stamp_2f8 == 0,
			"the respawn zeroes the latch set");
	CHECK(m.fresh_2f1 == 1, "and raises +0x2F1");
	CHECK(m.plat_acc[0] == 5 && m.wheel_comp[0] == 9,
			"sinks and compressions are not in the set");
	Entity::VehicleMotorState bms;
	CHECK(bms.fresh_2f1 == 0, "a BMS-spawned row has +0x2F1 = 0 (the memset)");
}

// The disable-rate pick is a role difference, not a spring scale: the two
// roles get different values, neither is unity. The tracked dt is the crashed
// latch's: a crashed hull grows its sinks four times faster.
void test_rate_pick_and_dt_are_the_witnessed_pair() {
	CHECK(kSuspensionDisableRateNonAuthority == 1.75f, "1.75 off the authority");
	CHECK(kSuspensionDisableRateAuthority == 1.25f, "1.25 on it");
	CHECK(kSuspensionDisableRateNonAuthority != kSuspensionDisableRateAuthority,
			"the two roles pick different rates");
	CHECK(kSuspensionDtCrashed == 4.0f * kSuspensionDtNormal,
			"crashed dt 3.0 against 0.75 normal");
}

// The tracked/tank crash tests over the inputs the port carries.
void test_crash_tests() {
	const VehicleTraits t = sprung_traits();
	const int32_t flip_q16 = vehicle_flip_threshold_q16(t);
	CHECK(flip_q16 == 29491, "flip 45 -> ftol(45 * 0.01 * 65536) = 29491");
	{
		// (a) tipped past the flip angle while airborne.
		auto w_heap = make_world(true);
		World &w = *w_heap;
		EntityHandle h;
		Entity &veh = spawn_veh(w, h);
		veh.veh.fresh_2f1 = 1;
		vehicle_suspension_crash_tests(w, veh, t, 20000, SuspensionFamily::Tracked);
		CHECK(veh.veh.crash_request == 0, "tipped but grounded: no request");
		veh.flags |= kEntityFlagInAir;
		vehicle_suspension_crash_tests(w, veh, t, 20000, SuspensionFamily::Tracked);
		CHECK(veh.veh.crash_request == 1 && veh.veh.fresh_2f1 == 0,
				"tipped and airborne: request, +0x2F1 cleared");
		veh.veh.crash_request = 0;
		vehicle_suspension_crash_tests(w, veh, t, 60000, SuspensionFamily::Tracked);
		CHECK(veh.veh.crash_request == 0, "upright and airborne: no request");
		// The bound is on |up.z|: an INVERTED hull is not "tipped".
		vehicle_suspension_crash_tests(w, veh, t, -60000, SuspensionFamily::Tracked);
		CHECK(veh.veh.crash_request == 0, "inverted and airborne: |up.z| is over the bound");
		vehicle_suspension_crash_tests(w, veh, t, -20000, SuspensionFamily::Tank);
		CHECK(veh.veh.crash_request == 1, "|up.z| under the bound tips either way up");
		veh.veh.crash_request = 0;
		veh.flags |= kEntityFlagSuspensionCrashed;
		vehicle_suspension_crash_tests(w, veh, t, 60000, SuspensionFamily::Tank);
		CHECK(veh.veh.crash_request == 0, "the tank's tip test has no bit alternative");
		vehicle_suspension_crash_tests(w, veh, t, 60000, SuspensionFamily::Tracked);
		CHECK(veh.veh.crash_request == 1, "the replicated bit while airborne requests too");
	}
	{
		// (b) the authority's hard fall vs the client's replicated bit.
		auto w_heap = make_world(true);
		World &w = *w_heap;
		EntityHandle h;
		Entity &veh = spawn_veh(w, h);
		veh.veh.slide_z = -0x7001;
		vehicle_suspension_crash_tests(w, veh, t, 60000, SuspensionFamily::Tracked);
		CHECK(veh.veh.crash_request == 1, "the authority: |vz| > 0x7000 requests");
		auto wc_heap = make_world(false);
		World &wc = *wc_heap;
		EntityHandle hc;
		Entity &cv = spawn_veh(wc, hc);
		cv.veh.fresh_2f1 = 1;
		cv.veh.slide_z = -0x7001;
		vehicle_suspension_crash_tests(wc, cv, t, 60000, SuspensionFamily::Tracked);
		CHECK(cv.veh.crash_request == 0, "a client never reads the fall test");
		cv.flags |= kEntityFlagInAir | kEntityFlagSuspensionCrashed;
		vehicle_suspension_crash_tests(wc, cv, t, 60000, SuspensionFamily::Tracked);
		CHECK(cv.veh.crash_request == 1, "a client: airborne with the bit requests");
	}
	{
		// (c) the client crash window: ten ticks from the airborne stamp.
		auto w_heap = make_world(false);
		World &w = *w_heap;
		EntityHandle h;
		Entity &veh = spawn_veh(w, h);
		w.logic_tick = 100;
		vehicle_suspension_crash_tests(w, veh, t, 60000, SuspensionFamily::Tracked);
		CHECK(veh.veh.crash_request == 0 && veh.veh.airborne_stamp_2f8 == 0,
				"grounded: the window does not open (tick - 0 >= 10)");
		CHECK(veh.veh.fresh_2f1 == 1, "and a never-airborne fresh row is marked respawned");
		veh.veh.fresh_2f1 = 0;
		veh.flags |= kEntityFlagInAir;
		vehicle_suspension_crash_tests(w, veh, t, 60000, SuspensionFamily::Tracked);
		CHECK(veh.veh.airborne_stamp_2f8 == 100 && veh.veh.crash_request == 1,
				"airborne: the stamp takes the tick and the window requests");
		vehicle_suspension_tick_tail(veh, t);
		w.logic_tick = 109;
		vehicle_suspension_crash_tests(w, veh, t, 60000, SuspensionFamily::Tracked);
		CHECK(veh.veh.crash_request == 1, "nine ticks in: still requesting");
		vehicle_suspension_tick_tail(veh, t);
		w.logic_tick = 110;
		vehicle_suspension_crash_tests(w, veh, t, 60000, SuspensionFamily::Tracked);
		CHECK(veh.veh.crash_request == 0 && veh.veh.airborne_stamp_2f8 == 0 &&
						veh.veh.fresh_2f1 == 1,
				"ten ticks: the window closes, the stamp clears, +0x2F1 raises");
		w.logic_tick = 111;
		vehicle_suspension_crash_tests(w, veh, t, 60000, SuspensionFamily::Tracked);
		CHECK(veh.veh.crash_request == 0 && veh.veh.airborne_stamp_2f8 == 0,
				"a respawned row never re-opens the window");
	}
	{
		// (c) the TANK window also waits on the settle latch; the tracked one
		// does not read it.
		auto w_heap = make_world(false);
		World &w = *w_heap;
		EntityHandle h;
		Entity &veh = spawn_veh(w, h);
		w.logic_tick = 100;
		veh.flags |= kEntityFlagInAir;
		veh.veh.settle_2f0 = 1;
		vehicle_suspension_crash_tests(w, veh, t, 60000, SuspensionFamily::Tank);
		CHECK(veh.veh.crash_request == 0 && veh.veh.airborne_stamp_2f8 == 0,
				"a settling tank does not open the window");
		vehicle_suspension_crash_tests(w, veh, t, 60000, SuspensionFamily::Tracked);
		CHECK(veh.veh.crash_request == 1 && veh.veh.airborne_stamp_2f8 == 100,
				"a settling tracked row does");
	}
}

// The bike's live crash test, and its seed ejecting the rider.
void test_bike_crash_test_and_eject() {
	auto w_heap = make_world(true);
	World &w = *w_heap;
	Entity bike;
	bike.kind = EntityKind::Item;
	bike.item_id = 1300;
	bike.position = {100.0f, 200.0f, 10.0f};
	bike.health = 500;
	bike.health_max = 500;
	bike.alive = true;
	Seat ctrl;
	ctrl.type = SeatType::Controller;
	ctrl.bone_index = 1;
	ctrl.source_name = "ctrlx00";
	bike.seats.push_back(ctrl);
	const EntityHandle bh = w.registry.spawn(1, bike);
	Entity drv;
	drv.kind = EntityKind::Organic;
	drv.item_id = 5305;
	drv.player_class = 8;
	drv.position = {100.0f, 199.0f, 10.0f};
	drv.health = 150;
	drv.health_max = 150;
	drv.alive = true;
	const EntityHandle dh = w.registry.spawn(0, drv);
	CHECK(entity_process_vehicle_attach(w, dh, bh, 1), "mount");
	Entity &veh = *w.registry.get(bh);
	CHECK(veh.primary_occupant == dh, "the rider claims the bike");

	vehicle_suspension_bike_crash_test(veh, false, false, true);
	CHECK(veh.veh.crash_request == 0, "a never-driven bike does not request");
	veh.veh.has_been_driven = 1;
	vehicle_suspension_bike_crash_test(veh, true, false, true);
	CHECK(veh.veh.crash_request == 0, "a wheel on the ground: no request");
	vehicle_suspension_bike_crash_test(veh, false, false, false);
	CHECK(veh.veh.crash_request == 0, "no spine contact: no request");
	vehicle_suspension_bike_crash_test(veh, false, false, true);
	CHECK(veh.veh.crash_request == 1,
			"both wheels off, driven, a spine probe touching: request");
	CHECK(vehicle_suspension_arm(w, veh, /*eject_occupants=*/true), "the bike seed arms");
	CHECK(!veh.seats[0].occupant.valid() && !veh.primary_occupant.valid(),
			"the bike seed ejects its rider");
	CHECK(!w.registry.get(dh)->mounted, "the rider is dismounted");
	CHECK(veh.veh.has_been_driven == 0, "and the driven byte clears");
}

// Sink growth: off-ground pads only, gated on the latch bytes for the
// tracked/tank families, ungated for the bike, skipped by the pre-gate.
void test_sink_growth_and_gates() {
	auto w_heap = make_world(true);
	World &w = *w_heap;
	EntityHandle h;
	Entity &veh = spawn_veh(w, h);
	const bool contact[4] = {true, false, false, true};
	vehicle_suspension_grow_sinks(veh, contact, 4, 187, true, false);
	CHECK(veh.veh.plat_acc[0] == 0 && veh.veh.plat_acc[1] == 187 &&
					veh.veh.plat_acc[2] == 187 && veh.veh.plat_acc[3] == 0,
			"only the pads off the ground sink, by the growth step");
	veh.veh.crash_request = 1;
	vehicle_suspension_grow_sinks(veh, contact, 4, 187, true, false);
	CHECK(veh.veh.plat_acc[1] == 187, "a pending request blocks the gated growth");
	veh.veh.crash_request = 0;
	veh.veh.crashed = 1;
	vehicle_suspension_grow_sinks(veh, contact, 4, 187, true, false);
	CHECK(veh.veh.plat_acc[1] == 187, "crashed blocks the gated growth");
	veh.veh.crashed = 0;
	vehicle_suspension_grow_sinks(veh, contact, 4, 187, true, true);
	CHECK(veh.veh.plat_acc[1] == 187, "the pre-gate skips the loop");
	veh.veh.crash_request = 1;
	vehicle_suspension_grow_sinks(veh, contact, 2, kSinkGrowthBike, false, false);
	CHECK(veh.veh.plat_acc[0] == 0 && veh.veh.plat_acc[1] == 187 + kSinkGrowthBike &&
					veh.veh.plat_acc[2] == 187,
			"the bike's growth has no latch terms and covers its two wheels");
	// The tracked step is ftol(0.75 * 250) normal, ftol(3.0 * 250) crashed.
	CHECK(static_cast<int32_t>(kSuspensionDtNormal * kSinkGrowthPerTick) == 187,
			"normal dt: 187");
	CHECK(static_cast<int32_t>(kSuspensionDtCrashed * kSinkGrowthPerTick) == 750,
			"crashed dt: 750");
}

// The grounded loop: the settle term compresses a pad penetrating deeper than
// the shallowest by one capped step and RESOLVES its depth; the landing
// impulse charges the wheel's energy and the impact sink; the other wheels
// are untouched; spring 0 skips the loop.
void test_grounded_loop_settle_and_impulse() {
	const VehicleTraits t = sprung_traits();
	{
		auto w_heap = make_world(true);
		World &w = *w_heap;
		EntityHandle h;
		Entity &veh = spawn_veh(w, h);
		int32_t depth[4] = {4096, 0, 0, 0};
		const bool contact[4] = {true, false, false, false};
		int32_t adj[4] = {0, 0, 0, 0};
		vehicle_suspension_grounded_loop(w, veh, t, 4, depth, contact, 187, adj);
		CHECK(veh.veh.wheel_comp[0] == kSpringStepCap,
				"settle: the deep pad compresses by the capped 4095 step");
		CHECK(depth[0] == 4096 - kSpringStepCap,
				"and its depth resolves by the compression delta");
		CHECK(veh.veh.wheel_osc[0].phase == kOscPhasePreset,
				"a compressing wheel is forced to the phase preset");
		CHECK(veh.veh.wheel_comp[1] == 0 && veh.veh.wheel_comp[2] == 0 &&
						veh.veh.wheel_comp[3] == 0 && depth[1] == 0 && depth[3] == 0,
				"the other wheels are untouched");
		CHECK(adj[0] == 0 && adj[1] == 0, "no sink: no catch-up term");
	}
	{
		auto w_heap = make_world(true);
		World &w = *w_heap;
		EntityHandle h;
		Entity &veh = spawn_veh(w, h);
		veh.veh.plat_acc[0] = 6000; // past the 5000 threshold (spring_comp 20 > 10)
		int32_t depth[4] = {4096, 0, 0, 0};
		const bool contact[4] = {true, false, false, false};
		int32_t adj[4] = {0, 0, 0, 0};
		vehicle_suspension_grounded_loop(w, veh, t, 4, depth, contact, 187, adj);
		CHECK(veh.veh.wheel_comp[0] == kSpringStepCap,
				"impulse: the landing pad compresses by the capped step");
		CHECK(veh.veh.wheel_osc[0].energy > 0,
				"and keeps the energy the one step did not drain");
		CHECK(veh.veh.spring_energy > 0, "the impact sink charged by 1.25 x the energy");
		CHECK(depth[0] == 4096 - kSpringStepCap, "the depth resolves");
	}
	{
		auto w_heap = make_world(true);
		World &w = *w_heap;
		EntityHandle h;
		Entity &veh = spawn_veh(w, h);
		VehicleTraits unsprung = t;
		unsprung.spring = 0;
		int32_t depth[4] = {4096, 4096, 4096, 4096};
		const bool contact[4] = {true, true, true, true};
		int32_t adj[4] = {0, 0, 0, 0};
		vehicle_suspension_grounded_loop(w, veh, unsprung, 4, depth, contact, 187, adj);
		CHECK(veh.veh.wheel_comp[0] == 0 && depth[0] == 4096,
				"spring 0: no suspension def'd, the loop is skipped");
	}
	{
		// A crashed row skips every wheel.
		auto w_heap = make_world(true);
		World &w = *w_heap;
		EntityHandle h;
		Entity &veh = spawn_veh(w, h);
		veh.veh.crashed = 1;
		int32_t depth[4] = {4096, 0, 0, 0};
		const bool contact[4] = {true, false, false, false};
		int32_t adj[4] = {0, 0, 0, 0};
		vehicle_suspension_grounded_loop(w, veh, t, 4, depth, contact, 187, adj);
		CHECK(veh.veh.wheel_comp[0] == 0 && depth[0] == 4096, "crashed: no spring work");
	}
}

// The airborne loop: stored energy compresses by the FULL step, the catch-up
// drops the corner by the sink beyond one growth step, a drop past -5000
// while falling marks the hard landing, and +0x2F0 gates the whole loop.
void test_airborne_loop_full_step_and_catch_up() {
	const VehicleTraits t = sprung_traits();
	{
		auto w_heap = make_world(true);
		World &w = *w_heap;
		EntityHandle h;
		Entity &veh = spawn_veh(w, h);
		veh.veh.wheel_osc[0].energy = 1000;
		veh.veh.plat_acc[0] = 1000;
		int32_t adj[4] = {0, 0, 0, 0};
		vehicle_suspension_airborne_loop(w, veh, t, 4, 187, adj);
		CHECK(veh.veh.wheel_comp[0] == kSpringStepCap,
				"airborne energy compresses by the full 4095 step");
		CHECK(adj[0] == -(1000 - 187), "the corner drops by the sink beyond one step");
		CHECK(adj[1] == 0, "a zero sink adds nothing");
		CHECK(veh.veh.landing_2ee == 0, "a shallow drop is no hard landing");
	}
	{
		auto w_heap = make_world(true);
		World &w = *w_heap;
		EntityHandle h;
		Entity &veh = spawn_veh(w, h);
		veh.veh.plat_acc[2] = 6000;
		veh.veh.slide_z = -1;
		int32_t adj[4] = {0, 0, 0, 0};
		vehicle_suspension_airborne_loop(w, veh, t, 4, 187, adj);
		CHECK(adj[2] == -(6000 - 187) && veh.veh.landing_2ee == 1,
				"a drop past -5000 while falling marks the hard landing");
	}
	{
		auto w_heap = make_world(true);
		World &w = *w_heap;
		EntityHandle h;
		Entity &veh = spawn_veh(w, h);
		veh.veh.settle_2f0 = 1;
		veh.veh.wheel_osc[0].energy = 1000;
		veh.veh.plat_acc[0] = 1000;
		int32_t adj[4] = {0, 0, 0, 0};
		vehicle_suspension_airborne_loop(w, veh, t, 4, 187, adj);
		CHECK(veh.veh.wheel_comp[0] == 0 && adj[0] == 0, "+0x2F0 gates the airborne loop");
	}
}

// The post-contact sink reset and the tick tail.
void test_post_contact_and_tail() {
	const VehicleTraits t = sprung_traits();
	auto w_heap = make_world(true);
	World &w = *w_heap;
	EntityHandle h;
	Entity &veh = spawn_veh(w, h);
	const auto reseed = [&veh]() {
		for (int k = 0; k < 4; ++k) veh.veh.plat_acc[k] = 5;
		veh.veh.landing_2ee = 1;
		veh.veh.byte_2ef = 1;
	};
	reseed();
	// A pad in contact zeroes ITS sink; nothing else moves.
	const bool one[4] = {true, false, false, false};
	vehicle_suspension_post_contact(veh, one, 4, 60000);
	CHECK(veh.veh.plat_acc[0] == 0 && veh.veh.plat_acc[1] == 5 && veh.veh.plat_acc[3] == 5,
			"a pad in contact zeroes its own sink only");
	CHECK(veh.veh.landing_2ee == 1 && veh.veh.byte_2ef == 1, "and clears no latch");
	// An ADJACENT pair is not the gate.
	reseed();
	const bool adjacent[4] = {true, true, false, false};
	vehicle_suspension_post_contact(veh, adjacent, 4, 60000);
	CHECK(veh.veh.plat_acc[2] == 5 && veh.veh.plat_acc[3] == 5 && veh.veh.landing_2ee == 1,
			"an adjacent pair is not a diagonal: no clear");
	// A DIAGONAL pair clears — on an upright, uncrashed hull.
	reseed();
	const bool diagonal[4] = {false, true, false, true};
	vehicle_suspension_post_contact(veh, diagonal, 4, -60000);
	CHECK(veh.veh.plat_acc[0] == 5 && veh.veh.landing_2ee == 1, "inverted: no clear");
	veh.veh.crashed = 1;
	vehicle_suspension_post_contact(veh, diagonal, 4, 60000);
	CHECK(veh.veh.plat_acc[0] == 5 && veh.veh.landing_2ee == 1, "crashed: no clear");
	veh.veh.crashed = 0;
	vehicle_suspension_post_contact(veh, diagonal, 4, 60000);
	CHECK(veh.veh.plat_acc[0] == 0 && veh.veh.plat_acc[2] == 0 &&
					veh.veh.landing_2ee == 0 && veh.veh.byte_2ef == 0,
			"a diagonal pair on an upright hull clears every sink, the landing marker and +0x2EF");
	// A two-wheel row pairs its two wheels.
	reseed();
	vehicle_suspension_post_contact(veh, one, 2, 60000);
	CHECK(veh.veh.plat_acc[1] == 5, "one wheel of two: no clear");
	vehicle_suspension_post_contact(veh, adjacent, 2, 60000);
	CHECK(veh.veh.plat_acc[1] == 0 && veh.veh.plat_acc[3] == 0 && veh.veh.landing_2ee == 0,
			"both wheels down: the clear");

	veh.veh.crash_request = 1;
	veh.veh.spring_energy = 7;
	vehicle_suspension_tick_tail(veh, t);
	CHECK(veh.veh.crash_request == 0, "the tail clears the per-tick request");
	CHECK(veh.veh.spring_energy == 0,
			"quiet amplitudes drop a leftover impact sink");
	veh.veh.spring_energy = 7;
	veh.veh.wheel_osc[2].amplitude = 600; // > ftol(0.01 * 52428) = 524
	vehicle_suspension_tick_tail(veh, t);
	CHECK(veh.veh.spring_energy == 7, "a live amplitude keeps the impact sink");
}

// The oscillator clamps the def's shock IN PLACE: a row the traits table
// knows sees its shared entry clamped, as retail clamps the shared def.
void test_shock_clamps_the_table_entry_in_place() {
	auto w_heap = make_world(true);
	World &w = *w_heap;
	EntityHandle h;
	Entity &veh = spawn_veh(w, h);
	VehicleTraits t = sprung_traits();
	t.shock = 25;
	w.vehicle_traits.set(veh.item_id, t);
	const VehicleTraits &live = *w.vehicle_traits.get(veh.item_id);
	veh.veh.wheel_osc[0].amplitude = 1000; // a releasing wheel -> the oscillator runs
	int32_t depth[4] = {0, 0, 0, 0};
	const bool contact[4] = {false, false, false, false};
	int32_t adj[4] = {0, 0, 0, 0};
	vehicle_suspension_grounded_loop(w, veh, live, 4, depth, contact, 187, adj);
	CHECK(w.vehicle_traits.get(veh.item_id)->shock == 10,
			"the table entry's shock is clamped to 10 in place");
	CHECK(veh.veh.wheel_osc[0].phase > 0.0f, "the oscillator ran");
}

} // namespace

int main() {
	test_def_keys_parse_raw();
	test_fresh_row_never_arms();
	test_request_arms_by_role_and_replication();
	test_arming_keeps_the_spring_state();
	test_respawn_field_set();
	test_rate_pick_and_dt_are_the_witnessed_pair();
	test_crash_tests();
	test_bike_crash_test_and_eject();
	test_sink_growth_and_gates();
	test_grounded_loop_settle_and_impulse();
	test_airborne_loop_full_step_and_catch_up();
	test_post_contact_and_tail();
	test_shock_clamps_the_table_entry_in_place();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("vehicle_suspension_test OK\n");
	return 0;
}
