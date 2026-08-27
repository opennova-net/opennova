// Pins the witnessed tracer style tables and the ribbon compile
// [orig: CEffectChannel_RenderRibbon @ 0x5DB8A0; style build
// CEffectEmitterPool_ResetAndBuildStyles @ 0x5DB3A0].

#include <runtime/renderer/tracer_frame.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
namespace r = renderer;

bool check(bool ok, const char *message) {
	if (!ok) {
		std::fputs(message, stderr);
		std::fputc(10, stderr);
	}
	return ok;
}

bool near(float actual, float expected, float epsilon = 0.0001f) {
	return std::fabs(actual - expected) <= epsilon;
}

bool style_table_contract() {
	// stdred is the id-1 block AND the fallback for id 0 / out-of-range ids
	// [orig: the CEffectChannel_Init default case @ 0x5db1c8].
	const auto &stdred = r::tracer_style(1);
	if (!check(r::tracer_style(0).colors == stdred.colors &&
			r::tracer_style(13).colors == stdred.colors &&
			r::tracer_style(99).colors == stdred.colors &&
			r::tracer_style(-1).colors == stdred.colors,
			"unknown tracer_type ids take the stdred block")) return false;
	if (!check(stdred.color_count == 12 && stdred.colors[1] == 0xE08080 &&
			stdred.colors[6] == 0x200000 && stdred.additive &&
			stdred.base_argb == 0 && stdred.size_count == 1 &&
			near(stdred.sizes[0], 0.02f),
			"stdred: 12-entry ramp, width 0.02, additive")) return false;
	if (!check(r::tracer_style(2).colors[1] == 0x80A080,
			"stdgreen swaps the ramp to the green channel")) return false;

	// Rocket/AT4 smoke: quadratic alpha fade a = ((255-2i)^2) >> 8 over 112
	// grays; linear width growth [orig: @ 0x5db4f8-0x5db5f6].
	const auto &rocket = r::tracer_style(3);
	const auto &at4 = r::tracer_style(4);
	if (!check(rocket.color_count == 112 && !rocket.additive &&
			rocket.base_argb == 0xC0C0C0 &&
			(rocket.colors[0] >> 24) == 254 && (rocket.colors[0] & 0xFFFFFF) == 0xC0C0C0 &&
			(rocket.colors[111] >> 24) == 4,
			"rocket smoke: 112-entry quadratic fade over gray")) return false;
	if (!check(rocket.size_count == 32 && near(rocket.sizes[16], 1.0f) &&
			at4.size_count == 32 && near(at4.sizes[16], 0.5f) &&
			at4.colors == rocket.colors,
			"rocket widths grow at 0.0625/entry, at4 at 0.03125, shared ramp")) return false;

	// Grenade: cubic alpha fade a = (192 * (255-3i)^3) >> 24 over 64 grays
	// [orig: @ 0x5db648].
	const auto &grenade = r::tracer_style(5);
	if (!check(grenade.color_count == 64 && !grenade.additive &&
			(grenade.colors[0] >> 24) == 189 && (grenade.colors[63] >> 24) == 3 &&
			grenade.size_count == 32 && near(grenade.sizes[8], 0.0625f),
			"grenade smoke: 64-entry cubic fade, width 0.0078125/entry")) return false;

	// Rapid: the short 6-entry ramps; NVG (id 8): 0xFF2020, alpha UP 6/entry
	// [orig: @ 0x5db6f7-0x5db711].
	const auto &nvg = r::tracer_style(8);
	if (!check(r::tracer_style(6).color_count == 6 &&
			r::tracer_style(7).colors[1] == 0x80A080 &&
			nvg.color_count == 32 && (nvg.colors[31] >> 24) == 186 &&
			(nvg.colors[31] & 0xFFFFFF) == 0xFF2020 && nvg.base_argb == 0xC04040,
			"rapid ramps are 6 entries; the NVG laser ramps up over red")) return false;

	// Sniper: width 0.04 -> 0.1 over 10 [orig: .data @ 0x8458C8]; DF1: 32-entry
	// fades at constant width 0.006 [orig: @ 0x5db3d9-0x5db4d6].
	const auto &sniper = r::tracer_style(9);
	if (!check(sniper.color_count == 20 && sniper.colors[0] == 0xFF180000 &&
			sniper.size_count == 10 && near(sniper.sizes[0], 0.04f) &&
			near(sniper.sizes[9], 0.1f) &&
			r::tracer_style(10).colors[4] == 0xF0000700,
			"sniper ramps and the 0.04->0.1 width curve")) return false;
	const auto &df1_red = r::tracer_style(11);
	return check(df1_red.color_count == 32 && df1_red.colors[0] == 0 &&
			df1_red.colors[1] == 0x7C3E3E &&
			r::tracer_style(12).colors[1] == 0x3E7C3E &&
			near(df1_red.sizes[0], 0.006f),
			"df1 fades and the thin constant width");
}

bool ribbon_geometry_contract() {
	// Three points along +X, camera on +Z: right = normalize(cross(dir, cam-p)).
	const float points[12] = {
		0.0f, 0.0f, 0.0f, 1.0f,
		1.0f, 0.0f, 0.0f, 1.0f,
		2.0f, 0.0f, 0.0f, 1.0f,
	};
	r::TracerChannelInput channel;
	channel.style_id = 1;
	channel.age = 0;
	channel.count = 3;
	channel.points = points;
	r::TracerRibbonFrame frame;
	r::compile_tracer_ribbons(&channel, 1, {0.0f, 0.0f, 10.0f}, frame);
	if (!check(frame.channels == 1 && frame.alpha.empty() &&
			frame.additive.size() == 4 * 7,
			"points [0..count-2] emit one +/- pair each into the additive run"))
		return false;
	const std::vector<float> &v = frame.additive;
	// Pair 0 at the oldest point: right = (0,-1,0), half_w = max(0.02, 10*0.0012)
	// = 0.02; the oldest pair takes the style base color (stdred base 0, a=1).
	if (!check(near(v[0], 0.0f) && near(v[1], -0.02f) && near(v[2], 0.0f) &&
			near(v[7 + 1], 0.02f),
			"the +/- right pair spans the witnessed half-width")) return false;
	if (!check(near(v[3], 0.0f) && near(v[4], 0.0f) && near(v[5], 0.0f) &&
			near(v[6], 1.0f),
			"the oldest pair takes the style base color; additive alpha rides 1"))
		return false;
	// Pair 1: ramp index (count - i) + age - 1 = 1 -> stdred 0xE08080.
	if (!check(near(v[14 + 3], 224.0f / 255.0f) &&
			near(v[14 + 4], 128.0f / 255.0f) && near(v[14 + 5], 128.0f / 255.0f),
			"the ramp indexes (count - i) + age - 1")) return false;

	// Age shifts the same ramp (the post-death fade): age 5 -> index 6 -> 0x200000.
	r::TracerChannelInput aged = channel;
	aged.age = 5;
	r::compile_tracer_ribbons(&aged, 1, {0.0f, 0.0f, 10.0f}, frame);
	if (!check(near(frame.additive[14 + 3], 32.0f / 255.0f) &&
			near(frame.additive[14 + 4], 0.0f),
			"age advances the ramp index (the fade half)")) return false;

	// Distance clamp: at 100 u the min screen width (100 * 0.0012 = 0.12) beats
	// the 0.02 style width.
	r::compile_tracer_ribbons(&channel, 1, {0.0f, 0.0f, 100.0f}, frame);
	return check(near(frame.additive[1], -0.12f, 0.001f),
			"far points take the distance-proportional min width");
}

bool family_split_and_join_contract() {
	const float run_a[8] = {
		0.0f, 0.0f, 0.0f, 1.0f,
		1.0f, 0.0f, 0.0f, 1.0f,
	};
	const float run_b[8] = {
		5.0f, 0.0f, 0.0f, 1.0f,
		6.0f, 0.0f, 0.0f, 1.0f,
	};
	const float run_c[8] = {
		9.0f, 0.0f, 0.0f, 1.0f,
		9.5f, 0.0f, 0.0f, 1.0f,
	};
	r::TracerChannelInput channels[3];
	channels[0] = {1, 0, 2, run_a};   // stdred -> additive
	channels[1] = {11, 0, 2, run_b};  // df1red -> additive (joins)
	channels[2] = {3, 0, 2, run_c};   // rocket smoke -> alpha
	r::TracerRibbonFrame frame;
	r::compile_tracer_ribbons(channels, 3, {0.0f, 0.0f, 10.0f}, frame);
	// Additive: 2 verts + degenerate join (prev-last repeat + new-first) + 2.
	if (!check(frame.channels == 3 && frame.additive.size() == 6 * 7,
			"same-family channels join through one degenerate pair")) return false;
	if (!check(near(frame.additive[2 * 7 + 0], frame.additive[1 * 7 + 0]) &&
			near(frame.additive[2 * 7 + 1], frame.additive[1 * 7 + 1]) &&
			near(frame.additive[3 * 7 + 0], frame.additive[4 * 7 + 0]),
			"the join repeats the previous last vertex and the new first vertex"))
		return false;
	// Smoke lands in the alpha run; its base pair carries the authored alpha
	// byte (0xC0C0C0 -> 0).
	return check(frame.alpha.size() == 2 * 7 &&
			near(frame.alpha[6], 0.0f) &&
			near(frame.alpha[3], 192.0f / 255.0f),
			"smoke styles split into the alpha family with authored alpha");
}

bool empty_and_short_channel_contract() {
	r::TracerRibbonFrame frame;
	r::compile_tracer_ribbons(nullptr, 0, {0.0f, 0.0f, 0.0f}, frame);
	if (!check(frame.channels == 0 && frame.additive.empty() && frame.alpha.empty(),
			"no channels compile to an empty frame")) return false;
	const float lone[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	r::TracerChannelInput single{1, 0, 1, lone};
	r::compile_tracer_ribbons(&single, 1, {0.0f, 0.0f, 10.0f}, frame);
	return check(frame.channels == 1 && frame.additive.empty(),
			"a one-point channel counts but emits no geometry");
}

} // namespace

int main() {
	if (!style_table_contract()) return 1;
	if (!ribbon_geometry_contract()) return 1;
	if (!family_split_and_join_contract()) return 1;
	if (!empty_and_short_channel_contract()) return 1;
	std::puts("renderer_tracer_frame_test ok");
	return 0;
}
