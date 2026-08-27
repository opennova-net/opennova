// Vehicle collision damage (roadkill): the target gate, attribution walking,
// and the kill-on-transition rule.
// [orig: Entity_ApplyVehicleCollisionDamage @0x4E6620]

#include <runtime/world/vehicle_collision_damage.h>

#include <cstdio>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

const int32_t PERSON = kCollisionDamageTargetType;
const int32_t VEHICLE = 3;

// Only a PERSON takes collision damage — vehicle-on-vehicle goes through the
// physics contact path instead.
void test_only_people() {
	CHECK(collision_damage_applies(PERSON, 0u, false), "a person is run over");
	CHECK(!collision_damage_applies(VEHICLE, 0u, false),
			"a vehicle is not — that is the contact path");
}

// Both flags are REFUSALS: a corpse is not run over twice, and an immune
// entity is skipped entirely.
void test_flag_refusals() {
	CHECK(!collision_damage_applies(PERSON, kEntityFlagDeadOrDying, false),
			"a dead or dying body takes no further collision damage");
	CHECK(!collision_damage_applies(PERSON, kEntityFlagNoCollisionDamage, false),
			"an immune entity is skipped");
	// Unrelated flags do not block it.
	CHECK(collision_damage_applies(PERSON, 0x8000u, false),
			"an unrelated flag does not block the path");
}

// THE SELF-HIT GUARD: without it a driver whose own hull resolves back to them
// takes their own roadkill.
void test_self_hit_guard() {
	CHECK(!collision_damage_applies(PERSON, 0u, true),
			"an entity never runs itself over");
	CHECK(collision_damage_applies(PERSON, 0u, false),
			"but does run someone else over");
}

// ATTRIBUTION WALKS UP THROUGH A DEAD PARENT, so a dead driver's vehicle still
// credits whoever killed the driver rather than a corpse.
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
// "was alive" half, every later collision with the body re-awards the kill.
void test_kill_fires_once_on_transition() {
	CHECK(collision_kill_fires(100, 0), "alive -> dead fires the kill");
	CHECK(collision_kill_fires(1, -20), "including overkill");
	CHECK(!collision_kill_fires(0, -20),
			"dead -> deader does NOT re-award the kill");
	CHECK(!collision_kill_fires(100, 50), "surviving does not fire it");
	CHECK(!collision_kill_fires(-5, -10),
			"a body already at negative health awards nothing");
}

// The collision damage flag and death cause are distinct, witnessed values.
void test_constants() {
	CHECK(kDamageFlagCollision == 0x400u, "the collision damage flag");
	CHECK(kDeathCauseCollision == 3, "the death cause code");
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
	test_constants();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("vehicle_collision_damage_test OK\n");
	return 0;
}
