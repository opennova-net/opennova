#include <formats/particle/parser.h>

#include <cstdio>
#include <cstring>
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
	return std::string(OPENNOVA_SOURCE_DIR) + "/fixtures/particle/synth_table_handles.ptl";
#else
	return "fixtures/particle/synth_table_handles.ptl";
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

	if (!expect(file.tables.size() == 1, "synth_table_handles.ptl has 1 tabledef")) return 1;
	if (!expect(file.table_handles.size() == 1, "synth_table_handles.ptl has 1 tabledef_edithandles")) return 1;

	const opennova::particle::TableDef &table = file.tables[0];
	if (!expect(table.id == "synth_wake", "table id parses")) return 1;
	if (!expect(table.rows.size() == 32, "synth_wake has 32 rows")) return 1;

	const opennova::particle::TableEditHandles &handles = file.table_handles[0];
	if (!expect(handles.table_id == "synth_wake", "edithandles tableid parses")) return 1;
	if (!expect(handles.handlecount == 0, "edithandles handlecount parses")) return 1;
	if (!expect(handles.tightness == 0, "edithandles tightness parses")) return 1;

	// The shipped boatwake.ptl closes its edithandles block with `};` after the
	// `}`: the stray `;` must not blow up the section parser.
	const char *stray_terminator =
			"[tabledef]\n{\n\tid = stray;\n\ttl1 = 1, 2, 3, 4, 5, 6, 7, 8;\n}\n"
			"[tabledef_edithandles]\n{\n\ttableid = stray;\n\thandlecount = 0;\n\ttightness = 0;\n};\n";
	opennova::particle::ParticleFile stray;
	if (!opennova::particle::load_particles_from_buffer(stray_terminator, std::strlen(stray_terminator), stray,
			error)) {
		std::fprintf(stderr, "FAIL: stray `};` parse error at line %d: %s\n", error.line, error.message.c_str());
		return 1;
	}
	if (!expect(stray.tables.size() == 1 && stray.table_handles.size() == 1, "the stray `};` block still parses"))
		return 1;
	return 0;
}
