#include <particle/parser.h>

#include <cmath>
#include <cstdio>
#include <fstream>
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

std::string fixture_path() {
#ifdef OPENNOVA_SOURCE_DIR
	return std::string(OPENNOVA_SOURCE_DIR) + "/fixtures/particle/buildup.ptl";
#else
	return "fixtures/particle/buildup.ptl";
#endif
}

} // namespace

int main() {
	std::ifstream stream(fixture_path(), std::ios::binary);
	if (!stream) {
		std::fprintf(stderr, "FAIL: cannot open %s\n", fixture_path().c_str());
		return 1;
	}

	opennova::particle::ParticleFile file;
	opennova::particle::ParseError error;
	if (!opennova::particle::load_particles(stream, file, error)) {
		std::fprintf(stderr, "FAIL: parse error at line %d: %s\n", error.line, error.message.c_str());
		return 1;
	}

	if (!expect(file.effects.size() == 1, "buildup.ptl has one effectdef")) return 1;
	if (!expect(file.particles.size() == 1, "buildup.ptl has one particledef")) return 1;

	const opennova::particle::EffectDef &effect = file.effects[0];
	if (!expect(effect.id == "Buildup", "effect id parses")) return 1;
	if (!expect(effect.pdefs.size() == 1, "effect has one pdef ref")) return 1;
	if (!expect(effect.pdefs[0] == "Buildup dots", "pdef id with whitespace parses")) return 1;

	const opennova::particle::ParticleDef &particle = file.particles[0];
	if (!expect(particle.id == "Buildup dots", "particle id with whitespace parses")) return 1;
	if (!expect(file.find_particle("bUILDUP DOTS") == &particle,
			"find_particle folds case [orig: CEffectWorld_FindParticleDefByName @ 0x5e41d0]")) return 1;
	if (!expect(particle.flags_raw == "TOPALIGN", "flags strip surrounding whitespace")) return 1;
	if (!expect(particle.move_raw == "GRAVITATE", "move enum parses")) return 1;
	if (!expect(near(particle.gravity, 600.0f), "gravity parses as float")) return 1;
	if (!expect(near(particle.spread, 180.0f), "spread parses as float")) return 1;
	if (!expect(particle.emit_burst == 1, "emit_burst parses as int")) return 1;
	if (!expect(particle.emit_shape == 2, "emit_shape parses as int")) return 1;
	if (!expect(near(particle.emit_shape_size.x, 10.0f), "emit_shape_size parses as vec3.x")) return 1;
	if (!expect(near(particle.emit_shape_size.y, 10.0f), "emit_shape_size parses as vec3.y")) return 1;

	if (!expect(particle.scale_func.present, "scale_func is present")) return 1;
	if (!expect(particle.scale_func.name == "table12", "scale_func name parses")) return 1;
	if (!expect(particle.scale_func.reverse, "scale_func reverse flag parses")) return 1;
	if (!expect(particle.red_func.name == "buildup_red", "red_func name parses")) return 1;
	if (!expect(!particle.red_func.reverse, "red_func reverse defaults false")) return 1;

	const opennova::particle::GraphicLayer &g1 = particle.graphics[0];
	if (!expect(g1.present, "graphic1 is present")) return 1;
	if (!expect(g1.index == 1, "graphic1 has index 1")) return 1;
	if (!expect(g1.texture == "line.tga", "graphic1 texture parses")) return 1;
	if (!expect(g1.blend_mode_raw == "additive", "graphic1 blend mode parses lowercase")) return 1;
	if (!expect(g1.flip_frames == 1, "g1 flip_frames parses")) return 1;
	if (!expect(g1.flip_rate == 8, "g1 flip_rate parses")) return 1;
	if (!expect(near(g1.scale, 2.0f), "g1 scale parses (override)")) return 1;
	if (!expect(g1.scale_func.name == "table12" && g1.scale_func.reverse, "g1 scale_func parses with reverse")) return 1;

	if (!expect(!particle.graphics[1].present, "graphic2 is absent")) return 1;
	if (!expect(!particle.graphics[2].present, "graphic3 is absent")) return 1;
	if (!expect(!particle.graphics[3].present, "graphic4 is absent")) return 1;

	// find_particle helper
	const opennova::particle::ParticleDef *found = file.find_particle("Buildup dots");
	if (!expect(found != nullptr && found->id == "Buildup dots", "find_particle resolves by id")) return 1;
	if (!expect(file.find_particle("nope") == nullptr, "find_particle returns null on miss")) return 1;

	constexpr char oversized_flipbook[] =
			"[particledef]\n"
			"{\n"
			"id = Oversized Flipbook;\n"
			"graphic1 = frame.tga, additive;\n"
			"g1_flip_frames = 999999;\n"
			"}\n";
	opennova::particle::ParticleFile bounded_file;
	opennova::particle::ParseError bounded_error;
	if (!opennova::particle::load_particles_from_buffer(oversized_flipbook,
			sizeof(oversized_flipbook) - 1, bounded_file, bounded_error)) {
		std::fprintf(stderr, "FAIL: bounded flipbook parse error at line %d: %s\n",
				bounded_error.line, bounded_error.message.c_str());
		return 1;
	}
	if (!expect(bounded_file.particles.size() == 1,
			"bounded flipbook fixture has one particledef")) return 1;
	if (!expect(bounded_file.particles[0].graphics[0].flip_frames ==
			opennova::particle::kMaxParticleFlipFrames,
			"authored flip_frames is capped at the shared runtime limit")) return 1;

	return 0;
}
