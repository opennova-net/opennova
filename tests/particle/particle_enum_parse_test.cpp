// Verifies the typed enum projections (BlendMode, move bits, particle flags,
// CurveRef inverse) parse and stringify correctly against fixture content.

#include <particle/particle.h>
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

std::string fixture_path(const char *name) {
#ifdef OPENNOVA_SOURCE_DIR
	return std::string(OPENNOVA_SOURCE_DIR) + "/fixtures/particle/" + name;
#else
	return std::string("fixtures/particle/") + name;
#endif
}

opennova::particle::ParticleFile load(const char *fixture) {
	std::ifstream stream(fixture_path(fixture), std::ios::binary);
	opennova::particle::ParticleFile file;
	opennova::particle::ParseError error;
	if (!stream || !opennova::particle::load_particles(stream, file, error)) {
		std::fprintf(stderr, "FAIL: cannot load %s: %s\n", fixture, error.message.c_str());
		std::exit(1);
	}
	return file;
}

} // namespace

int main() {
	using namespace opennova::particle;

	// 1. Pure enum lookup helpers — independent of any fixture.
	if (!expect(parse_blend_mode("additive") == BlendMode::Additive, "additive parses")) return 1;
	if (!expect(parse_blend_mode("blend")    == BlendMode::Blend,    "blend parses")) return 1;
	if (!expect(parse_blend_mode("distort")  == BlendMode::Distort,  "distort parses")) return 1;
	// "bumpadd" contains "bump"; engine's chained strstr returns Bump first.
	// Mirror exactly so corpus parses identically.
	if (!expect(parse_blend_mode("bumpadd")  == BlendMode::Bump,     "bumpadd resolves to bump (engine quirk)")) return 1;
	if (!expect(parse_blend_mode("")         == BlendMode::Blend,    "empty string defaults to Blend")) return 1;

	if (!expect(parse_move_bits("NORMAL")    == move_flag::Normal,    "move NORMAL")) return 1;
	if (!expect(parse_move_bits("GRAVITATE") == move_flag::Gravitate, "move GRAVITATE")) return 1;
	if (!expect(parse_move_bits("ORBIT")     == move_flag::Orbit,     "move ORBIT")) return 1;
	if (!expect(parse_move_bits("UNKNOWN")   == 0,                    "unknown move = 0 bits")) return 1;

	if (!expect(parse_particle_flags("TOPALIGN") == particle_flag::TopAlign, "TOPALIGN")) return 1;
	const std::uint32_t both = parse_particle_flags("EMITVECTOR AMBIENTCOLOR");
	if (!expect((both & particle_flag::EmitVector) != 0,   "EMITVECTOR bit")) return 1;
	if (!expect((both & particle_flag::AmbientColor) != 0, "AMBIENTCOLOR bit")) return 1;

	// 2. Format helpers — engine FlagTable_BuildString @ 0x428fe0 seeds a LEADING
	// space then appends "<name> " per matched bit, so the value is
	// " NAME1 NAME2 " (leading + single-separator + trailing space). See D1.
	const std::string move_str = format_move_bits(move_flag::Normal);
	if (!expect(move_str == " NORMAL ", "format_move_bits emits leading+trailing space")) return 1;
	const std::string flags_str = format_particle_flags(particle_flag::EmitVector | particle_flag::AmbientColor);
	if (!expect(flags_str == " EMITVECTOR AMBIENTCOLOR ", "format_particle_flags follows table order")) return 1;

	// 3. Fixture-driven projections — buildup.ptl: flags=TOPALIGN, move=GRAVITATE,
	// graphic1 = line.tga, additive; scale_func = table12 reverse;
	const ParticleFile buildup = load("buildup.ptl");
	const ParticleDef *p = buildup.find_particle("Buildup dots");
	if (!expect(p != nullptr, "Buildup dots resolves")) return 1;
	if (!expect(p->flags == particle_flag::TopAlign, "buildup flags = TopAlign bit")) return 1;
	if (!expect(p->move == move_flag::Gravitate, "buildup move = Gravitate bit")) return 1;
	if (!expect(p->scale_func.reverse && !p->scale_func.inverse, "scale_func reverse only")) return 1;
	if (!expect(p->graphics[0].blend_mode == BlendMode::Additive, "graphic1 blend = Additive enum")) return 1;

	// 4. Per-graphic blend modes via 30MM.ptl 30mmFolPuf (graphic1..3 all 'blend').
	const ParticleFile thirty = load("30MM.ptl");
	const ParticleDef *fol = thirty.find_particle("30mmFolPuf");
	if (!expect(fol != nullptr, "30mmFolPuf resolves")) return 1;
	if (!expect(fol->flags & particle_flag::EmitVector, "30mmFolPuf has EmitVector bit")) return 1;
	if (!expect(fol->flags & particle_flag::AmbientColor, "30mmFolPuf has AmbientColor bit")) return 1;
	if (!expect(fol->graphics[0].blend_mode == BlendMode::Blend, "graphic1 = blend")) return 1;
	if (!expect(fol->graphics[1].blend_mode == BlendMode::Blend, "graphic2 = blend")) return 1;
	if (!expect(fol->graphics[2].blend_mode == BlendMode::Blend, "graphic3 = blend")) return 1;
	if (!expect(fol->scale_func.reverse, "30mmFolPuf scale_func reverse")) return 1;

	return 0;
}
