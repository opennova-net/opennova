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
	return std::string(OPENNOVA_SOURCE_DIR) + "/fixtures/particle/boatwake.ptl";
#else
	return "fixtures/particle/boatwake.ptl";
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

	if (!expect(file.tables.size() == 1, "boatwake.ptl has 1 tabledef")) return 1;
	if (!expect(file.table_handles.size() == 1, "boatwake.ptl has 1 tabledef_edithandles")) return 1;

	const opennova::particle::TableDef &table = file.tables[0];
	if (!expect(table.id == "Wake_Table", "table id parses")) return 1;
	if (!expect(table.rows.size() == 32, "Wake_Table has 32 rows")) return 1;

	const opennova::particle::TableEditHandles &handles = file.table_handles[0];
	if (!expect(handles.table_id == "Wake_Table", "edithandles tableid parses")) return 1;
	if (!expect(handles.handlecount == 0, "edithandles handlecount parses")) return 1;
	if (!expect(handles.tightness == 0, "edithandles tightness parses")) return 1;

	// Validates that the trailing `};` after `}` (boatwake.ptl:45) does not blow up.
	// Implicit by reaching this line.
	return 0;
}
