// The suspension spring leg through the live seams: the def keys land on the
// traits, the role-picked parked latch with its Flags-0x10 replication, the
// spring dt by the latch, and the per-wheel compress/oscillate step over pad
// depths (the oscillator kernel's two decays are pinned in ground_conform_test;
// this file pins what WIRING adds).
// [orig: ItemDef_ParsePhysicsProperty @0x49db5c/@0x49dbd4/@0x49dc10/@0x49db98;
//  Entity_ProcessWheeledVehicleSuspension @0x46B1A6..0x46B213;
//  Entity_ProcessTrackedVehiclePhysics @0x47C218..0x47C22B + the spring loop]

#include "world/ai.h"
#include "world/entity.h"
#include "world/ground_conform.h"
#include "world/vehicle_motor.h"
#include "world/vehicle_suspension.h"
#include "world/world.h"

#include "def/def.h"

#include <cstdio>
#include <cstring>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

// The four keys parse raw beside the rest of the physics block, and
// spring_comp is matched before its prefix.
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
		CHECK(d.top_heavy == 3, "top_heavy raw");
		CHECK(d.physics == 1, "the rest of the block still parses");
	}
	def_free_items(&file);
}

World make_world(bool authority) {
	World w;
	w.registry.configure_pool(0, 4);
	w.registry.configure_pool(1, 4);
	w.vehicle_authority = authority;
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

// The leg is wired but armed OFF until the park machine's remaining producers
// are witnessed (vehicle_suspension.h). While it is off the live solves must
// see exactly their zero-state behaviour: no latch, no pad offsets.
void test_leg_is_armed_off_until_witnessed() {
	if (kSuspensionLegArmed) return;
	World w = make_world(true);
	EntityHandle h;
	Entity &veh = spawn_veh(w, h);
	CHECK(!vehicle_suspension_latch(w, veh), "armed off: the latch never sets");
	CHECK(veh.veh.susp_latched == 0 && (veh.flags & 0x10u) == 0,
			"armed off: no latch byte, no Flags 0x10");
	VehicleTraits t;
	t.spring = 8;
	t.spring_comp = 20;
	t.shock = 4;
	const int32_t depths[4] = {4096, 4096, 4096, 4096};
	for (int i = 0; i < 10; ++i) vehicle_suspension_step(w, veh, t, depths);
	CHECK(veh.veh.wheel_comp[0] == 0 && veh.veh.wheel_comp[3] == 0,
			"armed off: the pad offsets stay at the zero state");
}


// The latch edge: picked ONCE by role, the authority raises Flags 0x10, a
// client without the bit does not latch, a client WITH it does.
void test_latch_pick_by_role_and_replication() {
	if (!kSuspensionLegArmed) return; // armed-state pins; see test_leg_is_armed_off_until_witnessed
	{
		World w = make_world(true);
		EntityHandle h;
		Entity &veh = spawn_veh(w, h);
		CHECK(vehicle_suspension_latch(w, veh), "the authority latches");
		CHECK(veh.veh.susp_latched == 1, "+0x2EC set");
		CHECK(veh.veh.susp_rate_pick == kSuspensionDisableRateAuthority,
				"the authority picks 1.25");
		CHECK((veh.flags & 0x10u) != 0, "the authority raises Flags 0x10");
		CHECK(!vehicle_suspension_latch(w, veh), "a latched row does not re-pick");
		CHECK(vehicle_suspension_dt(veh.veh) == kSuspensionDtParked,
				"parked: the 3.0 spring dt");
	}
	{
		World w = make_world(false);
		EntityHandle h;
		Entity &veh = spawn_veh(w, h);
		CHECK(vehicle_suspension_dt(veh.veh) == kSuspensionDtUnparked,
				"unparked: the 0.75 spring dt");
		CHECK(!vehicle_suspension_latch(w, veh),
				"a client without the replicated bit does not latch");
		CHECK(veh.veh.susp_latched == 0 && (veh.flags & 0x10u) == 0,
				"and writes neither the latch nor the flag");
		veh.flags |= 0x10u; // the authority's bit arrives on the wire
		CHECK(vehicle_suspension_latch(w, veh), "a client with the bit latches");
		CHECK(veh.veh.susp_rate_pick == kSuspensionDisableRateNonAuthority,
				"the client picks 1.75");
	}
	{
		World w = make_world(true);
		EntityHandle h;
		Entity &veh = spawn_veh(w, h);
		veh.veh.susp_disable_req = 1;
		CHECK(!vehicle_suspension_latch(w, veh),
				"a pending mover disable request blocks the latch");
	}
}

// The latch edge clears the per-wheel state.
void test_latch_clears_state() {
	if (!kSuspensionLegArmed) return; // armed-state pins; see test_leg_is_armed_off_until_witnessed
	World w = make_world(true);
	EntityHandle h;
	Entity &veh = spawn_veh(w, h);
	veh.veh.wheel_comp[2] = 1234;
	veh.veh.wheel_osc[1].amplitude = 99;
	veh.veh.spring_energy = 7;
	CHECK(vehicle_suspension_latch(w, veh), "latch");
	CHECK(veh.veh.wheel_comp[2] == 0 && veh.veh.wheel_osc[1].amplitude == 0 &&
					veh.veh.spring_energy == 0,
			"the latch edge resets the spring state");
}

// The per-wheel step: a penetrating pad compresses by one dt step; a wheel in
// the air oscillates and its amplitude decays by 0.99 every tick; the
// compression is per WHEEL, so one pad's penetration never moves another's.
void test_step_compresses_and_releases_per_wheel() {
	if (!kSuspensionLegArmed) return; // armed-state pins; see test_leg_is_armed_off_until_witnessed
	World w = make_world(false);
	EntityHandle h;
	Entity &veh = spawn_veh(w, h);
	VehicleTraits t;
	t.spring = 8;
	t.spring_comp = 20;
	t.shock = 4;
	const int32_t depths[4] = {4096, 0, 0, 0}; // only pad 0 penetrates
	vehicle_suspension_step(w, veh, t, depths);
	const int32_t step = static_cast<int32_t>(
			static_cast<float>(kSuspStep) * kSuspensionDtUnparked);
	CHECK(veh.veh.wheel_comp[0] == step,
			"a penetrating pad compresses by exactly one unparked dt step");
	CHECK(veh.veh.wheel_osc[0].phase == kOscPhasePreset,
			"a compressing wheel is forced to the phase preset");
	CHECK(veh.veh.wheel_comp[1] == 0 && veh.veh.wheel_comp[2] == 0 &&
					veh.veh.wheel_comp[3] == 0,
			"the other wheels are untouched");
	// Now lift pad 0: it releases through the oscillator from its amplitude.
	const int32_t amp0 = veh.veh.wheel_osc[0].amplitude;
	CHECK(amp0 == step, "the amplitude tracks the compression");
	const int32_t none[4] = {0, 0, 0, 0};
	vehicle_suspension_step(w, veh, t, none);
	CHECK(veh.veh.wheel_osc[0].phase > kOscPhasePreset,
			"a released wheel advances its phase");
	CHECK(veh.veh.wheel_osc[0].amplitude <= amp0,
			"the amplitude decays (x0.99) on release");
	CHECK(veh.veh.wheel_comp[0] >= 0 && veh.veh.wheel_comp[0] <= amp0,
			"the released compression rides the raised-sine envelope");
	// Parked, the step is four times larger [orig: the 3.0 dt].
	World wp = make_world(true);
	EntityHandle hp;
	Entity &parked = spawn_veh(wp, hp);
	CHECK(vehicle_suspension_latch(wp, parked), "park");
	vehicle_suspension_step(wp, parked, t, depths);
	CHECK(parked.veh.wheel_comp[0] == static_cast<int32_t>(
					static_cast<float>(kSuspStep) * kSuspensionDtParked),
			"a parked vehicle steps its springs with the 3.0 dt");
	// 0xFFF * 3.0 = 12285 vs 0xFFF * 0.75 = 3071.25 -> 3071: the parked step
	// is four times the unparked one up to the ftol truncation of the latter.
	CHECK(parked.veh.wheel_comp[0] >= 4 * step &&
					parked.veh.wheel_comp[0] <= 4 * step + 4,
			"which is four times the unparked step (to the ftol truncation)");
}

// The travel bound from spring_comp caps the compression a pad can take.
void test_travel_bound_from_spring_comp() {
	if (!kSuspensionLegArmed) return; // armed-state pins; see test_leg_is_armed_off_until_witnessed
	World w = make_world(false);
	EntityHandle h;
	Entity &veh = spawn_veh(w, h);
	VehicleTraits t;
	t.spring = 8;
	t.spring_comp = 1; // almost no travel: (100-1)% of 0xFFFF is locked
	t.shock = 4;
	const int32_t travel = conform_travel_from_def(t.spring_comp);
	const int32_t depths[4] = {4096, 4096, 4096, 4096};
	for (int i = 0; i < 40; ++i) vehicle_suspension_step(w, veh, t, depths);
	// Once past the travel bound the compress step absorbs nothing more: the
	// compression sits one step beyond travel at most.
	const int32_t step = static_cast<int32_t>(
			static_cast<float>(kSuspStep) * kSuspensionDtUnparked);
	CHECK(veh.veh.wheel_comp[0] <= travel + step,
			"a bottomed wheel stops absorbing at its travel bound");
	CHECK(veh.veh.wheel_osc[0].amplitude == travel,
			"a bottomed wheel's amplitude pins at travel");
}

} // namespace

int main() {
	test_def_keys_parse_raw();
	test_leg_is_armed_off_until_witnessed();
	test_latch_pick_by_role_and_replication();
	test_latch_clears_state();
	test_step_compresses_and_releases_per_wheel();
	test_travel_bound_from_spring_comp();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("vehicle_suspension_test OK\n");
	return 0;
}
