#include <formats/particle/parser.h>

#include <base/io/strutil.h>

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

std::string fixture_path() {
#ifdef OPENNOVA_SOURCE_DIR
	return std::string(OPENNOVA_SOURCE_DIR) + "/fixtures/particle/synth_multi_layer.ptl";
#else
	return "fixtures/particle/synth_multi_layer.ptl";
#endif
}

bool test_graphic_hydration_follows_source_order() {
	// The game's reader takes a block a line at a time: a layer's key goes to the layer the last graphic line took
	// (the first, before any), a graphic line copies the particle as it stands at that line, over what an earlier
	// key wrote to its layer, and a particle key after the graphic lines reaches no layer [orig:
	// CParticleDefEntry_ParseGraphicProperty @ 0x5E3550, the copies @ 0x5E3635..0x5E36EC] (D-PTL-38).
	const std::string source = opennova::strutil::normalized_crlf_line_ends(R"PTL(
[particledef]
{
	id = ordered;
	g2_scale = 9.000;
	g2_alpha = 0.250;
	graphic1 = first.tga, blend;
	g1_color2 = 90, 91, 92;
	graphic2 = second.tga, additive;
	alpha = 0.750;
	scale = 2.000;
	scale_adj = 0.500;
	color1 = 10, 11, 12;
	color2 = 20, 21, 22;
	color3 = 30, 31, 32;
	color4 = 40, 41, 42;
}
)PTL");
	opennova::particle::ParticleFile file;
	opennova::particle::ParseError error;
	opennova::particle::load_particles_from_buffer(source.data(), source.size(), file, error);
	if (!expect(file.particles.size() == 1, "source-order particle count")) return false;
	const auto &def = file.particles[0];
	const auto &g1 = def.graphics[0];
	const auto &g2 = def.graphics[1];
	if (!expect(g1.present && g2.present, "two graphic lines, two layers")) return false;
	if (!expect(g1.scale == 1.0f && g1.scale_adj == 0.0f && g1.alpha == 1.0f,
			"graphic1 takes the particle's defaults over the early g2 keys")) return false;
	if (!expect(g1.color1.r == 255 && g1.color1.g == 255 && g1.color1.b == 255,
			"graphic1 takes the particle's colour as it stood, the default white")) return false;
	if (!expect(g1.color2.r == 90 && g1.color2.g == 91 && g1.color2.b == 92,
			"graphic1's own key after its line wins")) return false;
	if (!expect(g2.scale == 1.0f && g2.alpha == 1.0f && g2.color4.r == 255,
			"graphic2 takes the particle as it stood at its line")) return false;
	return expect(def.alpha == 0.75f && def.scale == 2.0f && def.color4.r == 40,
			"the particle's keys after the graphic lines are its own alone");
}

} // namespace

int main() {
	if (!test_graphic_hydration_follows_source_order()) return 1;

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

	// synth_multi_layer.ptl carries four effectdefs (dirt, foliage, metal, wood hits)
	// and three particledefs. Be lenient on counts; assert the anchors.
	if (!expect(file.effects.size() >= 4, "synth_multi_layer.ptl has >= 4 effectdefs")) return 1;
	if (!expect(file.particles.size() >= 3, "synth_multi_layer.ptl has >= 3 particledefs")) return 1;

	// synth_fol_puf: 3 graphic layers (graphic1=puf_a, graphic2=puf_b, graphic3=PufC).
	const opennova::particle::ParticleDef *folpuf = file.find_particle("synth_fol_puf");
	if (!expect(folpuf != nullptr, "synth_fol_puf particle resolves")) return 1;

	if (!expect(folpuf->flags == (opennova::particle::particle_flag::EmitVector |
										 opennova::particle::particle_flag::AmbientColor),
				"flags reads a list of names")) return 1;

	if (!expect(folpuf->graphics[0].present && folpuf->graphics[0].index == 1,
				"synth_fol_puf graphic1 is present at index 1")) return 1;
	if (!expect(folpuf->graphics[1].present && folpuf->graphics[1].index == 2,
				"synth_fol_puf graphic2 is present at index 2")) return 1;
	if (!expect(folpuf->graphics[2].present && folpuf->graphics[2].index == 3,
				"synth_fol_puf graphic3 is present at index 3")) return 1;
	if (!expect(!folpuf->graphics[3].present, "synth_fol_puf graphic4 is absent")) return 1;

	if (!expect(folpuf->graphics[0].texture == "puf_a.tga", "graphic1 texture")) return 1;
	if (!expect(folpuf->graphics[1].texture == "puf_b.tga", "graphic2 texture")) return 1;
	if (!expect(folpuf->graphics[2].texture == "PufC.tga", "graphic3 texture (case-preserving)")) return 1;

	if (!expect(folpuf->graphics[0].blend_mode == opennova::particle::BlendMode::Blend, "graphic1 blend = blend"))
		return 1;
	// Layers 2 and 3 write no curve of their own: each holds the particle's, copied at its graphic line.
	if (!expect(folpuf->graphics[2].alpha_func.name == "synth_fade" && folpuf->graphics[2].scale_func.reverse,
				"graphic3 holds the particle's curves")) return 1;

	if (!expect(folpuf->alpha_func.name == "synth_fade" && !folpuf->alpha_func.reverse,
				"alpha_func parses without reverse")) return 1;
	if (!expect(folpuf->scale_func.name == "synth_grow" && folpuf->scale_func.reverse,
				"scale_func parses with reverse")) return 1;

	// Per-layer color overrides: graphic1 color1 is (200, 160, 120).
	const opennova::particle::GraphicLayer &g1 = folpuf->graphics[0];
	if (!expect(g1.color1.r == 200 && g1.color1.g == 160 && g1.color1.b == 120,
				"graphic1 color1 parses")) return 1;

	// synth_flash: only graphic1 with flash.tga, additive
	const opennova::particle::ParticleDef *flash = file.find_particle("synth_flash");
	if (!expect(flash != nullptr, "synth_flash resolves")) return 1;
	if (!expect(flash->graphics[0].texture == "flash.tga", "synth_flash texture")) return 1;
	if (!expect(flash->graphics[0].blend_mode == opennova::particle::BlendMode::Additive,
				"synth_flash blend = additive")) return 1;
	if (!expect(!flash->graphics[1].present, "synth_flash has no graphic2")) return 1;

	return 0;
}
