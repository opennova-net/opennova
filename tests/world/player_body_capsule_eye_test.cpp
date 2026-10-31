// D-NET-330: the player body's capsule eye (CameraOffset +0x6C / +0x70 / +0x74) for a body
// the local player does not drive: the eye height on the lean's arc (x 0, y sin, z cos of
// -lean), turned about z by the twist (Yaw - the body heading), about x by Roll, about y by
// the body pitch and about z by the body heading; each angle's pair truncates
// sin / cos(word * radians-per-BAM) * 2^22, each product is a Q22 multiply truncated to a
// dword, and the height floors at 0x2000. The oracles are the witnessed arithmetic worked
// in that order (each x87 pair clear of its truncation edge at 53 or 64 bits: the nearest is
// 0.018 of a unit, the lean-only case's 0.11).
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the extent cap @0x4B698C..0x4B6997,
//  -lean @0x4B699F..0x4B69A7, the lean's pair @0x4B69AB..0x4B69D4, the twist
//  @0x4B69D9..0x4B6A08, Roll @0x4B6A68, +0x90 @0x4B6A8F, +0x8C @0x4B6AB3, the turns
//  @0x4B6A0C..0x4B6B96, the floor @0x4B6B98..0x4B6BA0, the stores @0x4B6BA5..0x4B6BAB]

#include <runtime/world/ai.h>
#include <runtime/world/infantry.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include "../common/title_fixture.h"

namespace {

namespace w = opennova::world;

int g_failures = 0;

void check(bool cond, const char *msg) {
	if (cond) return;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++g_failures;
}

void eye(int32_t extent, uint32_t lean, uint32_t yaw, uint32_t body_heading, uint32_t roll,
		uint32_t body_pitch, int32_t out[3]) {
	w::player_body_capsule_eye(extent, static_cast<int32_t>(lean), static_cast<int32_t>(yaw),
			static_cast<int32_t>(body_heading), static_cast<int32_t>(roll),
			static_cast<int32_t>(body_pitch), out);
}

} // namespace

int main() {
	int32_t out[3] = {};
	// Level: the capped height straight up, no lateral words.
	eye(0xD000, 0, 0, 0, 0, 0, out);
	check(out[0] == 0 && out[1] == 0 && out[2] == 0xD000, "a level body's eye is its height");
	// A 0x20000000 lean: sin(-lean) * 2^22 = -2965891.89 -> -2965891, cos -> 2965749;
	// y = -2965891 * 0x8000 >> 22 = -23172 (the floor of -23171.02), z = 23169.
	eye(0x8000, 0x20000000u, 0, 0, 0, 0, out);
	check(out[0] == 0 && out[1] == -23172 && out[2] == 23169,
			"the lean's arc: y the sine, z the cosine of -lean");
	// Every turn at once: lean -0x18000000, Yaw 0x30000000 over a body heading of
	// 0x38000000 (twist -0x08000000), Roll -0x04000000, body pitch 0x02000000.
	eye(0xD000, 0xE8000000u, 0x30000000u, 0x38000000u, 0xFC000000u, 0x02000000u, out);
	check(out[0] == -31846 && out[1] == 10149 && out[2] == 41448,
			"the twist, Roll, body pitch and body heading turns, in retail's order");
	// A short capsule floors the height; the lateral words do not floor.
	eye(0x1000, 0, 0, 0, 0, 0, out);
	check(out[2] == 0x2000, "the height floors at 0x2000");

	// The lean's halves around the eye: the decay (lean+8)>>4 at the pass's head, the
	// on-foot ramp 0x3000000 after the eye, a seated passenger's 0x1400000 before it
	// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the decay @0x4B5C97, the on-foot
	//  ramp @0x4B7DBF / @0x4B7DD6, the passenger test @0x4B6684 and its ramp
	//  @0x4B66B5 / @0x4B66C3].
	auto world = std::make_unique<w::World>();
	test_titles::jo_world(*world);
	w::AiEntity body;
	body.health = 100;
	body.inf.lean_angle = 0x20000000;
	body.inf.lean_left = true;
	world->ai.infantry_lean_decay(body);
	check(body.inf.lean_angle == 0x1E000000, "the decay takes (lean + 8) >> 4");
	world->ai.infantry_lean_ramp(body, 0);
	check(body.inf.lean_angle == 0x1B000000, "the on-foot ramp steps 0x3000000");
	world->ai.infantry_lean_seated_ramp(body);
	check(body.inf.lean_angle == 0x19C00000, "a seated passenger's ramp steps 0x1400000");
	body.inf.lean_left = false;
	body.inf.lean_right = true;
	world->ai.infantry_lean_seated_ramp(body);
	check(body.inf.lean_angle == 0x1B000000, "its right key steps back by 0x1400000");
	if (g_failures == 0) std::printf("player_body_capsule_eye: OK\n");
	return g_failures == 0 ? 0 : 1;
}
