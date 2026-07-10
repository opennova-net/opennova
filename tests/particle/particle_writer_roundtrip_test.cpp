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
	if (!near(a.emit_rate, b.emit_rate))       { diff = "emit_rate"; return false; }
	if (a.emit_burst != b.emit_burst)          { diff = "emit_burst"; return false; }
	if (a.emit_shape != b.emit_shape)          { diff = "emit_shape"; return false; }
	if (!vec3_equal(a.emit_shape_size, b.emit_shape_size)) { diff = "emit_shape_size"; return false; }
	if (!near(a.age, b.age))               { diff = "age"; return false; }
	if (!near(a.scale, b.scale))           { diff = "scale"; return false; }
	if (!near(a.alpha, b.alpha))           { diff = "alpha"; return false; }
	if (!colors_equal(a.color1, b.color1)) { diff = "color1"; return false; }
	if (!colors_equal(a.color2, b.color2)) { diff = "color2"; return false; }
	if (!colors_equal(a.color3, b.color3)) { diff = "color3"; return false; }
	if (!colors_equal(a.color4, b.color4)) { diff = "color4"; return false; }
	if (!vec3_equal(a.gravity_mask, b.gravity_mask)) { diff = "gravity_mask"; return false; }
	if (!near(a.gravity, b.gravity))       { diff = "gravity"; return false; }
	if (!near(a.drag, b.drag))             { diff = "drag"; return false; }
	if (!near(a.spread, b.spread))         { diff = "spread"; return false; }
	if (!curves_equal(a.scale_func, b.scale_func))   { diff = "scale_func"; return false; }
	if (!curves_equal(a.alpha_func, b.alpha_func))   { diff = "alpha_func"; return false; }
	if (!curves_equal(a.red_func, b.red_func))       { diff = "red_func"; return false; }
	if (!curves_equal(a.green_func, b.green_func))   { diff = "green_func"; return false; }
	if (!curves_equal(a.blue_func, b.blue_func))     { diff = "blue_func"; return false; }
	for (std::size_t i = 0; i < a.graphics.size(); ++i) {
		std::string sub_diff;
		if (!graphics_equal(a.graphics[i], b.graphics[i], sub_diff)) {
			diff = "graphics[" + std::to_string(i) + "]." + sub_diff;
			return false;
		}
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
