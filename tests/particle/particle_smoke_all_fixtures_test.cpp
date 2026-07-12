// Smoke test: every .ptl fixture under fixtures/particle/ must parse without
// error. Field assertions live in the focused tests; this is the regression
// net for new fixtures.
//
// Starts disabled (DISABLED TRUE in tests/CMakeLists.txt) until the full
// 77-file corpus is mirrored into fixtures/particle/.

#include <particle/parser.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

std::string fixtures_dir() {
#ifdef OPENNOVA_SOURCE_DIR
	return std::string(OPENNOVA_SOURCE_DIR) + "/fixtures/particle";
#else
	return "fixtures/particle";
#endif
}

} // namespace

int main() {
	int failures = 0;
	int parsed = 0;
	int row_width_violations = 0;

	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(fixtures_dir())) {
		if (!entry.is_regular_file() || entry.path().extension() != ".ptl") {
			continue;
		}

		opennova::particle::ParticleFile file;
		opennova::particle::ParseError error;
		if (!opennova::particle::load_particles_from_file(entry.path().string(), file, error)) {
			std::fprintf(stderr, "FAIL: %s line %d: %s\n",
					entry.path().filename().string().c_str(),
					error.line, error.message.c_str());
			++failures;
			continue;
		}

		// Row-width invariant: every TableDef row must have exactly 8 entries.
		// Our struct enforces 8 at compile time, but if the source had != 8
		// values per row we silently dropped the row in apply_table_key — track
		// that as a soft failure here.
		for (const opennova::particle::TableDef &table : file.tables) {
			// We can't tell from the parsed struct if rows were dropped, but
			// we can assert the typical 32-row shape and flag outliers.
			if (table.rows.size() != 32) {
				std::fprintf(stderr, "WARN: %s tabledef '%s' has %zu rows (expected 32)\n",
						entry.path().filename().string().c_str(),
						table.id.c_str(), table.rows.size());
				++row_width_violations;
			}
		}

		++parsed;
	}

	std::fprintf(stderr, "smoke: parsed %d / %d failed; %d table-row-shape warnings\n",
			parsed, failures, row_width_violations);

	return failures == 0 ? 0 : 1;
}
