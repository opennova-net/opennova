// The trail anchor: the point a round appends to its tracer channel.
// [orig: Projectile_GetTrailAnchorPos @ 0x4E64E0 — the round position plus its
//  euler matrix applied to {0, d, d}; d = the channel's amplitude word (style
//  +0xC) x the Q22 sine at (g_EntityUpdateCounter x the rate word (style +8)
//  + 0x200000) >> 22, shifted to 16.16; CEffectChannel_Init copies the two
//  words @ 0x5DB243..0x5DB249]
#include <runtime/world/geom.h>
#include <runtime/world/tracer_trails.h>

#include <cstdio>

namespace w = opennova::world;

namespace {

int failures = 0;

void expect(bool ok, const char *what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++failures;
	}
}

float moved(float v, int32_t q16) {
	return static_cast<float>(w::from_fixed(w::to_fixed(v) + q16));
}

} // namespace

int main() {
	// The channel takes its style's words at alloc.
	w::TracerTrailPool pool;
	const int rocket = pool.alloc(3);
	const int grenade = pool.alloc(5);
	const int laser = pool.alloc(8);
	const int unknown = pool.alloc(40);
	expect(rocket >= 0 && pool.channels[size_t(rocket)].wobble_rate == 0x8000000 &&
	               pool.channels[size_t(rocket)].wobble_amplitude == 0x4000,
	       "rocket: 0x8000000 / 0x4000");
	expect(grenade >= 0 && pool.channels[size_t(grenade)].wobble_amplitude == 0x1000,
	       "grenade: amplitude 0x1000");
	expect(laser >= 0 && pool.channels[size_t(laser)].wobble_amplitude == 0,
	       "the NVG laser: amplitude 0");
	expect(unknown >= 0 && pool.channels[size_t(unknown)].wobble_rate == 0x10000000 &&
	               pool.channels[size_t(unknown)].wobble_amplitude == 0x400,
	       "an unknown id takes the stdred block's words");
	expect(w::tracer_style_wobble_amplitude(11) == 0x100 &&
	               w::tracer_style_wobble_rate(9) == 0x10000000,
	       "df1 0x100, sniper 0x10000000");

	const w::TracerTrailChannel &ch = pool.channels[size_t(rocket)];
	const w::Vec3 at{12.5f, -3.25f, 40.0f};
	// 8 x 0x8000000 + 0x200000 >> 22 = table entry 256, the sine's crest:
	// d = (0x4000 x 0x10000 + 0x8000) >> 16 = 0x4000, a quarter unit. Level,
	// unrotated: local Y and Z are world Y and Z.
	const w::Vec3 crest = w::tracer_trail_anchor(ch, at, 0, 0, 0, 8);
	expect(crest.x == moved(at.x, 0) && crest.y == moved(at.y, 0x4000) &&
	               crest.z == moved(at.z, 0x4000),
	       "the crest: a quarter unit along local Y and Z");
	// Heading 90 degrees: local Y swings to -X, Z stays up.
	const w::Vec3 turned = w::tracer_trail_anchor(ch, at, 0x40000000, 0, 0, 8);
	expect(turned.x == moved(at.x, -0x4000) && turned.y == moved(at.y, 0) &&
	               turned.z == moved(at.z, 0x4000),
	       "heading 90: local Y is -X");
	// 24 x 0x8000000 wraps to entry 768, the trough.
	const w::Vec3 trough = w::tracer_trail_anchor(ch, at, 0, 0, 0, 24);
	expect(trough.y == moved(at.y, -0x4000) && trough.z == moved(at.z, -0x4000),
	       "the trough: the other side");
	// Entry 0 and a 32-update period.
	const w::Vec3 still = w::tracer_trail_anchor(ch, at, 0, 0, 0, 0);
	const w::Vec3 again = w::tracer_trail_anchor(ch, at, 0, 0, 0, 32);
	expect(still.x == moved(at.x, 0) && still.y == moved(at.y, 0) &&
	               still.z == moved(at.z, 0) && again.y == still.y && again.z == still.z,
	       "entry 0 on the path, every 32 updates");

	// The bullet tracers: a sixty-fourth every 16 updates.
	w::TracerTrailChannel stdred;
	stdred.wobble_rate = w::tracer_style_wobble_rate(1);
	stdred.wobble_amplitude = w::tracer_style_wobble_amplitude(1);
	const w::Vec3 bullet = w::tracer_trail_anchor(stdred, w::Vec3{0.0f, 0.0f, 0.0f}, 0, 0, 0, 4);
	expect(bullet.x == 0.0f && bullet.y == 0.015625f && bullet.z == 0.015625f,
	       "stdred: entry 256 at update 4, d = 0x400");

	if (failures != 0) return 1;
	std::printf("tracer_trail_anchor_test: all green\n");
	return 0;
}
