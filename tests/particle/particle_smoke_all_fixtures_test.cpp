// Smoke test: every .ptl under fixtures/particle/ must parse without error
// (field assertions live in the focused tests; this is the regression net
// for a new fixture). With <OPENNOVA_JO_ASSETS> set, the same sweep runs
// over the extracted retail .ptl corpus; without it that leg prints SKIP-LEG
// and the committed sweep alone decides the result.

#include <formats/particle/parser.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include "common/retail_paths.h"

namespace {

std::string fixtures_dir() {
#ifdef OPENNOVA_SOURCE_DIR
	return std::string(OPENNOVA_SOURCE_DIR) + "/fixtures/particle";
#else
	return "fixtures/particle";
#endif
}

bool has_ptl_extension(const std::filesystem::path &path) {
	std::string ext = path.extension().string();
	for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return ext == ".ptl";
}

// Parse every .ptl directly under `dir`; returns the number of failures and
// reports the parsed count. A file whose TableDef rows are not the usual 32
// is a soft warning (apply_table_key drops malformed rows silently, so the
// shape is the only tell).
int sweep(const std::string &dir, const char *label, int &parsed) {
	int failures = 0;
	int row_shape_warnings = 0;
	parsed = 0;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(dir)) {
		if (!entry.is_regular_file() || !has_ptl_extension(entry.path())) continue;
		opennova::particle::ParticleFile file;
		opennova::particle::ParseError error;
		if (!opennova::particle::load_particles_from_file(entry.path().string(), file, error)) {
			std::fprintf(stderr, "FAIL: %s: %s line %d: %s\n", label,
					entry.path().filename().string().c_str(), error.line, error.message.c_str());
			++failures;
			continue;
		}
		for (const opennova::particle::TableDef &table : file.tables) {
			if (table.rows.size() != 32) {
				std::fprintf(stderr, "WARN: %s: %s tabledef '%s' has %zu rows (expected 32)\n", label,
						entry.path().filename().string().c_str(), table.id.c_str(), table.rows.size());
				++row_shape_warnings;
			}
		}
		++parsed;
	}
	std::fprintf(stderr, "smoke (%s): parsed %d, %d failed, %d table-row-shape warnings\n",
			label, parsed, failures, row_shape_warnings);
	return failures;
}

} // namespace

int main() {
	int parsed = 0;
	int failures = sweep(fixtures_dir(), "fixtures/particle", parsed);
	if (parsed == 0) {
		std::fprintf(stderr, "FAIL: no .ptl fixtures found under %s\n", fixtures_dir().c_str());
		return 1;
	}

	const std::string assets = retail::assets();
	if (assets.empty()) {
		retail::skip_leg("OPENNOVA_JO_ASSETS (the extracted retail .ptl corpus)");
	} else {
		int retail_parsed = 0;
		failures += sweep(assets, "OPENNOVA_JO_ASSETS", retail_parsed);
		if (retail_parsed == 0)
			std::fprintf(stderr, "WARN: no .ptl files directly under %s\n", assets.c_str());
	}
	return failures == 0 ? 0 : 1;
}
