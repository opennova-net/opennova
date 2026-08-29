#include <formats/particle/parser.h>

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

std::string fixture_path() {
#ifdef OPENNOVA_SOURCE_DIR
	return std::string(OPENNOVA_SOURCE_DIR) + "/fixtures/particle/synth_multi_section.ptl";
#else
	return "fixtures/particle/synth_multi_section.ptl";
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

	if (!expect(file.effects.size() == 7, "synth_multi_section.ptl has 7 effectdefs")) return 1;
	if (!expect(file.particles.size() == 2, "synth_multi_section.ptl has 2 particledefs")) return 1;

	// Effect ordering matches source.
	if (!expect(file.effects[0].id == "stockeffect", "effect[0] id")) return 1;
	if (!expect(file.effects[1].id == "synth_burst", "effect[1] id")) return 1;
	if (!expect(file.effects.back().id == "synth_smoke", "last effect id")) return 1;

	// Every effect references one pdef named "blank".
	for (const opennova::particle::EffectDef &effect : file.effects) {
		if (!expect(effect.pdefs.size() == 1, "every effect has one pdef")) return 1;
		if (!expect(effect.pdefs[0] == "blank", "every effect points at the blank particle")) return 1;
	}

	// "blank" particle: graphic1 has empty texture but a blend mode of "blend".
	const opennova::particle::ParticleDef *blank = file.find_particle("blank");
	if (!expect(blank != nullptr, "blank particle resolves")) return 1;
	const opennova::particle::GraphicLayer &g1 = blank->graphics[0];
	if (!expect(g1.present, "blank graphic1 is present (declared)")) return 1;
	if (!expect(g1.texture.empty(), "blank graphic1 texture is empty")) return 1;
	if (!expect(g1.blend_mode_raw == "blend", "blank graphic1 blend mode is 'blend'")) return 1;

	// synth_flare: graphic1 = flare.tga, additive
	const opennova::particle::ParticleDef *stockparticl = file.find_particle("synth_flare");
	if (!expect(stockparticl != nullptr, "synth_flare resolves")) return 1;
	if (!expect(stockparticl->graphics[0].texture == "flare.tga", "synth_flare g1 texture parses")) return 1;
	if (!expect(stockparticl->graphics[0].blend_mode_raw == "additive", "synth_flare g1 blend mode parses")) return 1;

	return 0;
}
