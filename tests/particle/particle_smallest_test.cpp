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
	return std::string(OPENNOVA_SOURCE_DIR) + "/fixtures/particle/synth_smallest.ptl";
#else
	return "fixtures/particle/synth_smallest.ptl";
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

	if (!expect(file.effects.empty(), "synth_smallest.ptl has no effectdefs")) return 1;
	if (!expect(file.particles.empty(), "synth_smallest.ptl has no particledefs")) return 1;
	if (!expect(file.tables.size() == 1, "synth_smallest.ptl has exactly one tabledef")) return 1;
	if (!expect(file.table_handles.empty(), "synth_smallest.ptl has no edithandles")) return 1;

	const opennova::particle::TableDef &table = file.tables[0];
	if (!expect(table.id == "synth_ramp", "tabledef id parses")) return 1;
	if (!expect(table.rows.size() == 32, "tabledef has 32 rows")) return 1;

	// Spot-check the ramp corners (row i, byte j = 255 - 8 i - j, floored at 1).
	if (!expect(table.rows.front()[0] == 255, "tl1 first byte parses")) return 1;
	if (!expect(table.rows.front()[7] == 248, "tl1 last byte parses")) return 1;
	if (!expect(table.rows.back()[0] == 7, "tl32 first byte parses")) return 1;
	if (!expect(table.rows.back()[7] == 1, "tl32 last byte parses")) return 1;

	return 0;
}
