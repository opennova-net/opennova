// Verifies save_trn/load_trn round-trips a TRN with empty polydata. The
// Godot editor writes project-mode .trn files with no polydata since the
// "Make CPT optional" change — this test locks in that the libs/trn parser
// handles that shape without regressions (the polydata is consumer-enforced
// at the NovaTerrainData level, not the trn_io level).

#include <trn/trn.h>
#include <trn/trn_io.h>

#include <cstdio>
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

} // namespace

int main() {
	// Save a .trn with polydata explicitly empty (editor Save Project mode).
	opennova::TrnConfig saved;
	saved.name = "ProjectMode";
	saved.colormap = "projectmode_c.tga";
	saved.detailmap = "projectmode_dm.tga";
	saved.detailblendmap = "projectmode_d1.tga";
	saved.charmap = "projectmode_m.pcx";
	saved.foliagemap = "projectmode_f.pcx";
	saved.tilestrip = "projectmode_t.tga";
	saved.polydata = "";  // project mode — no CPT
	saved.detail_density = 128;
	saved.detail_density2 = 8;
	saved.sector_count = 8;
	saved.origin_x = -4;
	saved.origin_y = -4;
	saved.water_height = 21;

	std::ostringstream out;
	std::string err;
	if (!expect(opennova::save_trn(out, saved, err),
	            "save_trn must accept empty polydata")) {
		std::fprintf(stderr, "  err: %s\n", err.c_str());
		return 1;
	}

	const std::string serialised = out.str();

	// save_trn should omit the polytrn_polydata line entirely when empty
	// (not emit `polytrn_polydata        ` with a blank value).
	if (!expect(serialised.find("polytrn_polydata") == std::string::npos,
	            "save_trn must not emit polytrn_polydata when polydata is empty")) {
		std::fprintf(stderr, "  serialised output:\n%s\n", serialised.c_str());
		return 1;
	}

	// Load it back and confirm the fields round-trip.
	opennova::TrnConfig loaded;
	std::istringstream in(serialised);
	if (!expect(opennova::load_trn(in, loaded, err),
	            "load_trn must accept a TRN with no polydata line")) {
		std::fprintf(stderr, "  err: %s\n", err.c_str());
		return 1;
	}

	if (!expect(loaded.polydata.empty(),
	            "loaded polydata must be empty when source has no polytrn_polydata line")) {
		std::fprintf(stderr, "  loaded.polydata = '%s'\n", loaded.polydata.c_str());
		return 1;
	}
	if (!expect(loaded.name == "ProjectMode", "terrain name round-trip")) return 1;
	if (!expect(loaded.colormap == "projectmode_c.tga", "colormap round-trip")) return 1;
	if (!expect(loaded.charmap == "projectmode_m.pcx", "charmap round-trip")) return 1;
	if (!expect(loaded.foliagemap == "projectmode_f.pcx", "foliagemap round-trip")) return 1;
	if (!expect(loaded.tilestrip == "projectmode_t.tga", "tilestrip round-trip")) return 1;

	std::printf("OK: TRN round-trips with empty polydata (project-mode save)\n");
	return 0;
}
