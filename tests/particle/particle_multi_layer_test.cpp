#include <particle/parser.h>

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
	return std::string(OPENNOVA_SOURCE_DIR) + "/fixtures/particle/30MM.ptl";
#else
	return "fixtures/particle/30MM.ptl";
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

	// 30MM.ptl ships at least 4 effectdefs (30mmDirtHit, FolHit, MetalHit, WoodHit)
	// and a richer particle catalogue. Be lenient on counts; assert the anchors.
	if (!expect(file.effects.size() >= 4, "30MM.ptl has >= 4 effectdefs")) return 1;
	if (!expect(file.particles.size() >= 3, "30MM.ptl has >= 3 particledefs")) return 1;

	// 30mmFolPuf: 3 graphic layers (graphic1=dirtpuf, graphic2=thinpuf, graphic3=CDirtpuf).
	const opennova::particle::ParticleDef *folpuf = file.find_particle("30mmFolPuf");
	if (!expect(folpuf != nullptr, "30mmFolPuf particle resolves")) return 1;

	if (!expect(folpuf->flags_raw == "EMITVECTOR AMBIENTCOLOR",
				"flags preserves multi-token whitespace-separated value")) return 1;

	if (!expect(folpuf->graphics[0].present && folpuf->graphics[0].index == 1,
				"30mmFolPuf graphic1 is present at index 1")) return 1;
	if (!expect(folpuf->graphics[1].present && folpuf->graphics[1].index == 2,
				"30mmFolPuf graphic2 is present at index 2")) return 1;
	if (!expect(folpuf->graphics[2].present && folpuf->graphics[2].index == 3,
				"30mmFolPuf graphic3 is present at index 3")) return 1;
	if (!expect(!folpuf->graphics[3].present, "30mmFolPuf graphic4 is absent")) return 1;

	if (!expect(folpuf->graphics[0].texture == "dirtpuf.tga", "graphic1 texture")) return 1;
	if (!expect(folpuf->graphics[1].texture == "thinpuf.tga", "graphic2 texture")) return 1;
	if (!expect(folpuf->graphics[2].texture == "CDirtpuf.tga", "graphic3 texture (case-preserving)")) return 1;

	if (!expect(folpuf->graphics[0].blend_mode_raw == "blend", "graphic1 blend = blend")) return 1;

	if (!expect(folpuf->alpha_func.name == "table1" && !folpuf->alpha_func.reverse,
				"alpha_func parses without reverse")) return 1;
	if (!expect(folpuf->scale_func.name == "table4" && folpuf->scale_func.reverse,
				"scale_func parses with reverse")) return 1;

	// Per-layer color overrides — color1 should match (220, 183, 140).
	const opennova::particle::GraphicLayer &g1 = folpuf->graphics[0];
	if (!expect(g1.color_overrides_set, "graphic1 has explicit color overrides")) return 1;
	if (!expect(g1.color1.r == 220 && g1.color1.g == 183 && g1.color1.b == 140,
				"graphic1 color1 parses")) return 1;

	// 30mmFlash: only graphic1 with mbFlash2.tga, additive
	const opennova::particle::ParticleDef *flash = file.find_particle("30mmFlash");
	if (!expect(flash != nullptr, "30mmFlash resolves")) return 1;
	if (!expect(flash->graphics[0].texture == "mbFlash2.tga", "30mmFlash texture")) return 1;
	if (!expect(flash->graphics[0].blend_mode_raw == "additive", "30mmFlash blend = additive")) return 1;
	if (!expect(!flash->graphics[1].present, "30mmFlash has no graphic2")) return 1;

	return 0;
}
