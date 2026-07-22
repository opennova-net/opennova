// Round-trip parity: load a fixture, serialize via save_particles, re-parse the
// serialized output, and compare the parsed structs for logical equivalence.
// This catches both writer bugs (fields not emitted) and parser bugs (fields
// not re-readable from our own output).

#include <particle/parser.h>
#include <particle/particle.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool near(float actual, float expected, float epsilon = 0.0001f) {
	return std::fabs(actual - expected) <= epsilon;
}

bool curves_equal(const opennova::particle::CurveRef &a, const opennova::particle::CurveRef &b) {
	return a.name == b.name && a.reverse == b.reverse && a.inverse == b.inverse && a.present == b.present;
}

bool colors_equal(const opennova::particle::Color3 &a, const opennova::particle::Color3 &b) {
	return a.r == b.r && a.g == b.g && a.b == b.b;
}

bool vec3_equal(const opennova::particle::Vec3 &a, const opennova::particle::Vec3 &b) {
	return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

bool graphics_equal(const opennova::particle::GraphicLayer &a, const opennova::particle::GraphicLayer &b, std::string &diff) {
	if (a.present != b.present)        { diff = "present"; return false; }
	if (a.index != b.index)            { diff = "index"; return false; }
	if (a.texture != b.texture)        { diff = "texture"; return false; }
	if (a.blend_mode != b.blend_mode)  { diff = "blend_mode"; return false; }
	if (a.flip_frames != b.flip_frames){ diff = "flip_frames"; return false; }
	if (a.flip_rate != b.flip_rate)    { diff = "flip_rate"; return false; }
	if (!colors_equal(a.color1, b.color1)) { diff = "color1"; return false; }
	if (!colors_equal(a.color2, b.color2)) { diff = "color2"; return false; }
	if (!colors_equal(a.color3, b.color3)) { diff = "color3"; return false; }
	if (!colors_equal(a.color4, b.color4)) { diff = "color4"; return false; }
	if (a.color_overrides_set != b.color_overrides_set) { diff = "color_overrides_set"; return false; }
	if (!near(a.alpha, b.alpha))           { diff = "alpha"; return false; }
	if (!near(a.scale, b.scale))           { diff = "scale"; return false; }
	if (!near(a.scale_adj, b.scale_adj))   { diff = "scale_adj"; return false; }
	if (!curves_equal(a.scale_func, b.scale_func)) { diff = "scale_func"; return false; }
	if (!curves_equal(a.alpha_func, b.alpha_func)) { diff = "alpha_func"; return false; }
	if (!curves_equal(a.red_func, b.red_func))     { diff = "red_func"; return false; }
	if (!curves_equal(a.green_func, b.green_func)) { diff = "green_func"; return false; }
	if (!curves_equal(a.blue_func, b.blue_func))   { diff = "blue_func"; return false; }
	return true;
}

bool particles_equal(const opennova::particle::ParticleDef &a, const opennova::particle::ParticleDef &b, std::string &diff) {
	if (a.id != b.id)               { diff = "id"; return false; }
	if (a.child_id != b.child_id)   { diff = "child_id"; return false; }
	if (a.flags != b.flags)         { diff = "flags"; return false; }
	if (a.move != b.move)           { diff = "move"; return false; }
	if (!near(a.lod, b.lod))        { diff = "lod"; return false; }
	if (!near(a.emit_dur, b.emit_dur))         { diff = "emit_dur"; return false; }
	if (!near(a.emit_dur_adj, b.emit_dur_adj)) { diff = "emit_dur_adj"; return false; }
	if (!near(a.emit_rate, b.emit_rate))       { diff = "emit_rate"; return false; }
	if (!near(a.emit_rate_adj, b.emit_rate_adj)) { diff = "emit_rate_adj"; return false; }
	if (!curves_equal(a.emit_rate_func, b.emit_rate_func)) { diff = "emit_rate_func"; return false; }
	if (!near(a.emit_delay, b.emit_delay))     { diff = "emit_delay"; return false; }
	if (a.emit_burst != b.emit_burst)          { diff = "emit_burst"; return false; }
	if (a.emit_maxoverride != b.emit_maxoverride) { diff = "emit_maxoverride"; return false; }
	if (a.emit_shape != b.emit_shape)          { diff = "emit_shape"; return false; }
	if (!vec3_equal(a.emit_shape_size, b.emit_shape_size)) { diff = "emit_shape_size"; return false; }
	if (!vec3_equal(a.emit_shape_size_skip, b.emit_shape_size_skip)) { diff = "emit_shape_size_skip"; return false; }
	if (!near(a.y_offset, b.y_offset))     { diff = "y_offset"; return false; }
	if (!near(a.z_offset, b.z_offset))     { diff = "z_offset"; return false; }
	if (!near(a.age, b.age))               { diff = "age"; return false; }
	if (!near(a.age_adj, b.age_adj))       { diff = "age_adj"; return false; }
	if (!near(a.scale, b.scale))           { diff = "scale"; return false; }
	if (!near(a.scale_adj, b.scale_adj))   { diff = "scale_adj"; return false; }
	if (!near(a.alpha, b.alpha))           { diff = "alpha"; return false; }
	if (!colors_equal(a.color1, b.color1)) { diff = "color1"; return false; }
	if (!colors_equal(a.color2, b.color2)) { diff = "color2"; return false; }
	if (!colors_equal(a.color3, b.color3)) { diff = "color3"; return false; }
	if (!colors_equal(a.color4, b.color4)) { diff = "color4"; return false; }
	if (!near(a.bump_scale, b.bump_scale)) { diff = "bump_scale"; return false; }
	if (!vec3_equal(a.orientation, b.orientation)) { diff = "orientation"; return false; }
	if (!vec3_equal(a.orientationadj, b.orientationadj)) { diff = "orientationadj"; return false; }
	if (!near(a.yaw_rot, b.yaw_rot)) { diff = "yaw_rot"; return false; }
	if (!near(a.yaw_rot_adj, b.yaw_rot_adj)) { diff = "yaw_rot_adj"; return false; }
	if (!near(a.pitch_rot, b.pitch_rot)) { diff = "pitch_rot"; return false; }
	if (!near(a.pitch_rot_adj, b.pitch_rot_adj)) { diff = "pitch_rot_adj"; return false; }
	if (!near(a.roll_rot, b.roll_rot)) { diff = "roll_rot"; return false; }
	if (!near(a.roll_rot_adj, b.roll_rot_adj)) { diff = "roll_rot_adj"; return false; }
	if (!near(a.speed, b.speed)) { diff = "speed"; return false; }
	if (!near(a.speed_adj, b.speed_adj)) { diff = "speed_adj"; return false; }
	if (!near(a.elastic, b.elastic)) { diff = "elastic"; return false; }
	if (!vec3_equal(a.gravity_mask, b.gravity_mask)) { diff = "gravity_mask"; return false; }
	if (!near(a.gravity, b.gravity))       { diff = "gravity"; return false; }
	if (!near(a.drag, b.drag))             { diff = "drag"; return false; }
	if (!near(a.spread, b.spread))         { diff = "spread"; return false; }
	if (!near(a.spread_skip, b.spread_skip)) { diff = "spread_skip"; return false; }
	if (!near(a.orbitalspeed, b.orbitalspeed)) { diff = "orbitalspeed"; return false; }
	if (!near(a.orbitalspeed_adj, b.orbitalspeed_adj)) { diff = "orbitalspeed_adj"; return false; }
	if (!vec3_equal(a.orbital_axis, b.orbital_axis)) { diff = "orbital_axis"; return false; }
	if (!curves_equal(a.scale_func, b.scale_func))   { diff = "scale_func"; return false; }
	if (!curves_equal(a.alpha_func, b.alpha_func))   { diff = "alpha_func"; return false; }
	if (!curves_equal(a.red_func, b.red_func))       { diff = "red_func"; return false; }
	if (!curves_equal(a.green_func, b.green_func))   { diff = "green_func"; return false; }
	if (!curves_equal(a.blue_func, b.blue_func))     { diff = "blue_func"; return false; }
	if (a.collide_sounds != b.collide_sounds) { diff = "collide_sounds"; return false; }
	if (a.unknown_keys != b.unknown_keys) { diff = "unknown_keys"; return false; }
	for (std::size_t i = 0; i < a.graphics.size(); ++i) {
		std::string sub_diff;
		if (!graphics_equal(a.graphics[i], b.graphics[i], sub_diff)) {
			diff = "graphics[" + std::to_string(i) + "]." + sub_diff;
			return false;
		}
	}
	return true;
}

bool roundtrip_synthetic_full_particle() {
	using namespace opennova::particle;
	ParticleFile first;
	ParticleDef p;
	p.id = "synthetic full particle";
	p.child_id = "child particle";
	p.flags = particle_flag::EmitVector | particle_flag::AmbientColor;
	p.move = move_flag::Normal | move_flag::Orbit;
	p.lod = 0.125f;
	p.emit_dur = 1.250f;
	p.emit_dur_adj = 0.125f;
	p.emit_rate = 12.500f;
	p.emit_rate_adj = 2.250f;
	p.emit_rate_func = {"emit curve", true, false, true};
	p.emit_delay = 0.375f;
	p.emit_burst = 3;
	p.emit_maxoverride = 77;
	p.emit_shape = 2;
	p.emit_shape_size = {1.0f, 2.0f, 3.0f};
	p.emit_shape_size_skip = {0.1f, 0.2f, 0.3f};
	p.y_offset = 0.625f;
	p.z_offset = -0.750f;
	p.age = 4.500f;
	p.age_adj = 0.875f;
	p.scale = 2.250f;
	p.scale_adj = 0.125f;
	p.scale_func = {"scale curve", false, true, true};
	p.alpha = 0.875f;
	p.alpha_func = {"alpha curve", true, true, true};
	p.red_func = {"red curve", false, false, true};
	p.green_func = {"green curve", true, false, true};
	p.blue_func = {"blue curve", false, true, true};
	p.color1 = {1, 2, 3};
	p.color2 = {4, 5, 6};
	p.color3 = {7, 8, 9};
	p.color4 = {10, 11, 12};
	p.bump_scale = 0.625f;
	p.orientation = {10.0f, 20.0f, 30.0f};
	p.orientationadj = {1.0f, 2.0f, 3.0f};
	p.yaw_rot = 4.0f; p.yaw_rot_adj = 5.0f;
	p.pitch_rot = 6.0f; p.pitch_rot_adj = 7.0f;
	p.roll_rot = 8.0f; p.roll_rot_adj = 9.0f;
	p.speed = 10.0f; p.speed_adj = 11.0f;
	p.elastic = 0.375f;
	p.gravity = -12.0f;
	p.gravity_mask = {0.25f, 0.5f, 0.75f};
	p.drag = 1.125f;
	p.spread = 40.0f; p.spread_skip = 5.0f;
	p.orbitalspeed = 2.5f; p.orbitalspeed_adj = 1.5f;
	p.orbital_axis = {0.0f, 0.0f, 1.0f};
	p.collide_sounds[3] = "impact sound";
	p.unknown_keys.emplace_back("future_mode", "precise");
	p.unknown_keys.emplace_back("future_value", "one, two");
	p.unknown_keys.emplace_back("g1_future_mode", "first");
	p.unknown_keys.emplace_back("g1_future_mode", "second");
	GraphicLayer &g2 = p.graphics[1];
	g2.index = 2;
	g2.present = true;
	g2.texture = "sparse texture.tga";
	g2.blend_mode_raw = "additive";
	g2.blend_mode = BlendMode::Additive;
	g2.flip_frames = 4;
	g2.flip_rate = 6;
	g2.color1 = {21, 22, 23};
	g2.color2 = {24, 25, 26};
	g2.color3 = {27, 28, 29};
	g2.color4 = {30, 31, 32};
	g2.color_overrides_set = true;
	g2.alpha = 0.375f;
	g2.scale = 3.250f;
	g2.scale_adj = 0.750f;
	g2.alpha_func = {"layer-only alpha", false, true, true};
	first.particles.push_back(p);

	std::ostringstream serialized;
	std::string write_error;
	if (!save_particles(serialized, first, write_error)) return false;
	const std::string text = serialized.str();
	if (!expect(text.find("graphic2") != std::string::npos,
			"sparse graphic keeps authored slot")) return false;
	if (!expect(text.find("graphic1") == std::string::npos,
			"sparse graphic does not compact to slot 1")) return false;
	if (!expect(text.find("g2_alpha\t=") != std::string::npos,
			"per-layer alpha is serialized")) return false;
	if (!expect(text.find("g2_alpha_func\t=") != std::string::npos,
			"per-layer-only curve is serialized")) return false;
	if (!expect(text.find("future_mode") != std::string::npos,
			"unknown particle key is serialized")) return false;
	if (!expect(text.find("g1_future_mode") != std::string::npos,
			"graphic-shaped unknown particle key is serialized")) return false;
	if (!expect(text.find("emit_rate_func\t= emit curve reverse;") != std::string::npos,
			"reverse-only curve uses the retail writer token")) return false;
	if (!expect(text.find("scale_func\t= scale curve invert;") != std::string::npos,
			"inverse curve uses retail's invert spelling")) return false;
	if (!expect(text.find("alpha_func\t= alpha curve invert reverse;") != std::string::npos,
			"combined modifiers use retail's invert-first order")) return false;

	std::istringstream replay(text);
	ParticleFile second;
	ParseError error;
	if (!load_particles(replay, second, error)) return false;
	if (!expect(second.particles.size() == 1, "synthetic particle re-parses")) return false;
	std::string diff;
	if (!particles_equal(first.particles[0], second.particles[0], diff)) {
		std::fprintf(stderr, "FAIL: synthetic field '%s' differs\n", diff.c_str());
		return false;
	}
	return true;
}

std::string fixture_path(const char *name) {
#ifdef OPENNOVA_SOURCE_DIR
	return std::string(OPENNOVA_SOURCE_DIR) + "/fixtures/particle/" + name;
#else
	return std::string("fixtures/particle/") + name;
#endif
}

bool roundtrip_one(const char *fixture) {
	std::ifstream stream(fixture_path(fixture), std::ios::binary);
	if (!stream) {
		std::fprintf(stderr, "FAIL: cannot open %s\n", fixture);
		return false;
	}
	opennova::particle::ParticleFile first;
	opennova::particle::ParseError error;
	if (!opennova::particle::load_particles(stream, first, error)) {
		std::fprintf(stderr, "FAIL: %s parse 1: %s\n", fixture, error.message.c_str());
		return false;
	}

	std::ostringstream serialized;
	std::string write_error;
	if (!opennova::particle::save_particles(serialized, first, write_error)) {
		std::fprintf(stderr, "FAIL: %s save: %s\n", fixture, write_error.c_str());
		return false;
	}

	std::istringstream replay(serialized.str());
	opennova::particle::ParticleFile second;
	if (!opennova::particle::load_particles(replay, second, error)) {
		std::fprintf(stderr, "FAIL: %s parse 2: %s\n--- emitted ---\n%s\n", fixture, error.message.c_str(), serialized.str().c_str());
		return false;
	}

	if (first.effects.size() != second.effects.size()) {
		std::fprintf(stderr, "FAIL: %s effects count %zu != %zu\n", fixture, first.effects.size(), second.effects.size());
		return false;
	}
	for (std::size_t i = 0; i < first.effects.size(); ++i) {
		if (first.effects[i].id != second.effects[i].id || first.effects[i].pdefs != second.effects[i].pdefs) {
			std::fprintf(stderr, "FAIL: %s effect[%zu] mismatch\n", fixture, i);
			return false;
		}
	}

	if (first.particles.size() != second.particles.size()) {
		std::fprintf(stderr, "FAIL: %s particles count %zu != %zu\n", fixture, first.particles.size(), second.particles.size());
		return false;
	}
	for (std::size_t i = 0; i < first.particles.size(); ++i) {
		std::string diff;
		if (!particles_equal(first.particles[i], second.particles[i], diff)) {
			std::fprintf(stderr, "FAIL: %s particle[%zu] '%s' field '%s' differs\n",
					fixture, i, first.particles[i].id.c_str(), diff.c_str());
			return false;
		}
	}

	if (first.tables.size() != second.tables.size()) {
		std::fprintf(stderr, "FAIL: %s tables count %zu != %zu\n", fixture, first.tables.size(), second.tables.size());
		return false;
	}
	for (std::size_t i = 0; i < first.tables.size(); ++i) {
		if (first.tables[i].id != second.tables[i].id || first.tables[i].rows != second.tables[i].rows) {
			std::fprintf(stderr, "FAIL: %s table[%zu] mismatch\n", fixture, i);
			return false;
		}
	}

	return true;
}

} // namespace

int main() {
	if (!roundtrip_synthetic_full_particle()) return 1;

	const char *fixtures[] = {
		"troytabl.ptl",
		"buildup.ptl",
		"stock.ptl",
		"30MM.ptl",
		"boatwake.ptl",
	};

	int failures = 0;
	for (const char *f : fixtures) {
		if (!roundtrip_one(f)) ++failures;
	}
	if (!expect(failures == 0, "all fixtures round-trip equivalently")) return 1;
	return 0;
}
