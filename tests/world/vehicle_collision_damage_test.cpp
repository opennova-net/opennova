// The contact-damage predicates: the kind-1 (knife) kill-zone callback's
// target gate, attribution walking and kill-on-transition rule, and the
// movement resolver's run-over kill and bump sound gates.
// [orig: Entity_ApplyVehicleCollisionDamage @0x4E6620;
//  Entity_MovementCollisionResolver @0x4B37C2..0x4B3A5C]

#include <runtime/world/vehicle_collision_damage.h>

#include <cstdio>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

// The retail ItemDef type values entity item_type carries: 3 person, 1 vehicle
// [orig: the `cmp dword ptr [ecx+5Ch], 3` person test @0x4E6633].
const int32_t PERSON = kCollisionDamageTargetType;
const int32_t VEHICLE = 1;

// Only a PERSON takes the kind-1 damage.
void test_only_people() {
	CHECK(PERSON == 3, "the person ItemDef type is 3");
	CHECK(collision_damage_applies(PERSON, 0u, false), "a person is hit");
	CHECK(!collision_damage_applies(VEHICLE, 0u, false), "a vehicle is not");
}

// Both flags are REFUSALS: a corpse is not hit twice, and an immune
// entity is skipped entirely.
void test_flag_refusals() {
	CHECK(!collision_damage_applies(PERSON, kEntityFlagDeadOrDying, false),
			"a dead or dying body takes no further damage");
	CHECK(!collision_damage_applies(PERSON, kEntityFlagNoCollisionDamage, false),
			"an immune entity is skipped");
	// Unrelated flags do not block it.
	CHECK(collision_damage_applies(PERSON, 0x8000u, false),
			"an unrelated flag does not block the path");
}

// THE SELF-HIT GUARD: the source standing inside its own kill zone.
void test_self_hit_guard() {
	CHECK(!collision_damage_applies(PERSON, 0u, true),
			"an entity never hits itself");
	CHECK(collision_damage_applies(PERSON, 0u, false),
			"but does hit someone else");
}

// ATTRIBUTION WALKS UP THROUGH A DEAD PARENT, so a dead source still
// credits whoever killed it rather than a corpse.
void test_attribution_walks_past_a_dead_parent() {
	CHECK(attribution_walks_past(0, 0u), "a dead parent is walked past");
	CHECK(attribution_walks_past(-50, 0u), "including one at negative health");
	CHECK(!attribution_walks_past(100, 0u),
			"a live parent is the attacker — the walk stops");
	// Flag 0x100 makes an entity terminal even when dead.
	CHECK(!attribution_walks_past(0, kEntityFlagTerminalAttribution),
			"the terminal flag stops the walk even at zero health");
}

// THE KILL FIRES ON THE TRANSITION. Both halves are needed: without the
// "was alive" half, every later hit on the body re-awards the kill.
void test_kill_fires_once_on_transition() {
	CHECK(collision_kill_fires(100, 0), "alive -> dead fires the kill");
	CHECK(collision_kill_fires(1, -20), "including overkill");
	CHECK(!collision_kill_fires(0, -20),
			"dead -> deader does NOT re-award the kill");
	CHECK(!collision_kill_fires(100, 50), "surviving does not fire it");
	CHECK(!collision_kill_fires(-5, -10),
			"a body already at negative health awards nothing");
}

// The shared approach quadrant: (Yaw - atan2BAM(dy, dx) + bias) >> 30, with
// (dx, dy) pointing from the victim toward the source. yaw 0 faces +x.
void test_approach_quadrant() {
	CHECK(approach_quadrant(0, 0x10000, 0, 0x1FFFFFFFu) == 0, "a source dead ahead is 0");
	CHECK(approach_quadrant(0, -0x10000, 0, 0x1FFFFFFFu) == 2, "one behind is 2");
	// atan2(+y) = +90 deg: (0 - 0x40000000 + 0x1FFFFFFF) >> 30 = 3.
	CHECK(approach_quadrant(0, 0, 0x10000, 0x1FFFFFFFu) == 3, "one on +y is 3");
	CHECK(approach_quadrant(0, 0, -0x10000, 0x1FFFFFFFu) == 1, "one on -y is 1");
	// The kind-1 bias 0x20000000 differs from 0x1FFFFFFF only on the exact
	// 45-degree seam [orig: @0x4E66B1 vs @0x4B39A2].
	CHECK(approach_quadrant(0x20000000, 0x10000, 0, 0x1FFFFFFFu) == 0,
			"yaw +45 deg with the 0x1FFFFFFF bias stays in 0");
	CHECK(approach_quadrant(0x20000000, 0x10000, 0, kCollisionQuadrantBias) == 1,
			"the 0x20000000 bias tips the seam into 1");
	CHECK(kCollisionDeathBone == 2 && kCollisionDeathCause == 1,
			"the kind-1 death anim is cause 1 at bone 2");
}

// The run-over kill's refusals [orig: @0x4b37c2..0x4b3946]: only a moving
// vehicle that is not the victim's ride, on the authority, past 0x27B0 on both
// its own and its relative displacement, across teams (or BERSERK), against a
// live, destructible, unprotected victim.
void test_run_over_gates() {
	const int32_t fast = kRunOverSpeedThreshold + 1;
	const int32_t slow = kRunOverSpeedThreshold;
	CHECK(run_over_kill_applies(true, false, false, 100, false, fast, fast, false, false, true, false, false),
			"a moving enemy vehicle kills");
	CHECK(!run_over_kill_applies(false, false, false, 100, false, fast, fast, false, false, true, false, false),
			"a non-vehicle pusher never kills");
	CHECK(!run_over_kill_applies(true, true, false, 100, false, fast, fast, false, false, true, false, false),
			"the victim's own ride never crushes it");
	CHECK(!run_over_kill_applies(true, false, true, 100, false, fast, fast, false, false, true, false, false),
			"a dead hull never kills");
	CHECK(!run_over_kill_applies(true, false, false, 0, false, fast, fast, false, false, true, false, false),
			"a victim without health is skipped");
	CHECK(!run_over_kill_applies(true, false, false, 100, true, fast, fast, false, false, true, false, false),
			"a dead victim is skipped");
	CHECK(!run_over_kill_applies(true, false, false, 100, false, fast, slow, false, false, true, false, false),
			"relative displacement at the threshold does not kill");
	CHECK(!run_over_kill_applies(true, false, false, 100, false, slow, fast, false, false, true, false, false),
			"pusher displacement at the threshold does not kill");
	CHECK(!run_over_kill_applies(true, false, false, 100, false, fast, fast, true, false, true, false, false),
			"same team, no berserk: no kill");
	CHECK(run_over_kill_applies(true, false, false, 100, false, fast, fast, true, true, true, false, false),
			"same team with a berserk side kills");
	CHECK(!run_over_kill_applies(true, false, false, 100, false, fast, fast, false, false, false, false, false),
			"a joiner never kills");
	CHECK(!run_over_kill_applies(true, false, false, 100, false, fast, fast, false, false, true, true, false),
			"an indestructible victim survives");
	// [orig: `cmp [esi+124h], 0` @0x4b391f; the slot byte @0x4b393f]
	CHECK(!run_over_kill_applies(true, false, false, 100, false, fast, fast, false, false, true, false, true),
			"a spawn-protected or spectating player survives");
	CHECK(run_over_contact_applies(true, false, false, 100, false),
			"the shared head admits a live victim of a live vehicle");
	CHECK(!run_over_contact_applies(true, true, false, 100, false),
			"but never the victim's own ride");
	// The run-over quadrant's vector is the VICTIM's displacement minus the
	// pusher's [orig: @0x4b395d / @0x4b3963]: a pusher driving -x into a
	// victim facing +x yields +x, the front quadrant 0.
	CHECK(run_over_quadrant(0, 0x10000, 0) == 0, "hit head-on: quadrant 0");
	CHECK(run_over_quadrant(0, -0x10000, 0) == 2, "hit from behind: quadrant 2");
	CHECK(run_over_quadrant(0, 0, 0x10000) == 3, "quadrant 3");
	CHECK(run_over_quadrant(0, 0, -0x10000) == 1, "quadrant 1");
	CHECK(kRunOverDeathCause == 2 && kRunOverDeathBone == 2, "cause 2 at bone 2");
}

// The bump sound needs BOTH planar displacements strictly above 0x3F8
// [orig: the `jle` pair @0x4b3a06 / @0x4b3a0c].
void test_bump_sound_gate() {
	CHECK(run_over_sound_applies(kRunOverSoundThreshold + 1, kRunOverSoundThreshold + 1),
			"a moving push plays the bump");
	CHECK(!run_over_sound_applies(kRunOverSoundThreshold, kRunOverSoundThreshold + 1),
			"a pusher at the threshold is silent");
	CHECK(!run_over_sound_applies(kRunOverSoundThreshold + 1, kRunOverSoundThreshold),
			"a relative displacement at the threshold is silent");
	CHECK(kRunOverSoundSuppressTicks == 0x1F, "the 31-tick suppression window");
}

void test_constants() {
	CHECK(kDamageFlagCollision == 0x400u, "the knife cause bit");
	CHECK(kDeathCauseCollision == 3, "the class callback event");
	CHECK(kEntityFlagDeadOrDying != kEntityFlagNoCollisionDamage,
			"the two refusal flags are distinct");
}

} // namespace

int main() {
	test_only_people();
	test_flag_refusals();
	test_self_hit_guard();
	test_attribution_walks_past_a_dead_parent();
	test_kill_fires_once_on_transition();
	test_approach_quadrant();
	test_run_over_gates();
	test_bump_sound_gate();
	test_constants();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("vehicle_collision_damage_test OK\n");
	return 0;
}
