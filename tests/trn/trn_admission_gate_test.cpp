// Pins load_trn's admission gate, the tail of the retail terrain-config loader
// [orig: Terrain_LoadEnvironmentConfig @0x610940]: a config with an empty
// polytrn_colormap, polytrn_detailmap or polytrn_polydata, or a polytrn_sectors
// row count / polytrn_sectorcount above 16 or not a power of two, is rejected
// with a descriptive error. save_trn still omits an empty polydata line (the
// writer never emits blank values); the reader is what refuses the result.

#include <formats/trn/trn.h>
#include <formats/trn/trn_io.h>

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

opennova::TrnConfig valid_config() {
	opennova::TrnConfig cfg;
	cfg.name = "Gate";
	cfg.colormap = "gate_c.tga";
	cfg.detailmap = "gate_dm.tga";
	cfg.polydata = "gate.cpt";
	cfg.detail_density = 128;
	cfg.detail_density2 = 8;
	cfg.sector_count = 8;
	cfg.sector_rows = 8;
	return cfg;
}

std::string serialise(const opennova::TrnConfig &cfg) {
	std::ostringstream out;
	std::string err;
	if (!opennova::save_trn(out, cfg, err)) {
		std::fprintf(stderr, "FAIL: save_trn: %s\n", err.c_str());
		return std::string();
	}
	return out.str();
}

bool loads(const std::string &text, std::string &err) {
	opennova::TrnConfig loaded;
	std::istringstream in(text);
	err.clear();
	return opennova::load_trn(in, loaded, err);
}

bool expect_rejected(const std::string &text, const char *needle, const char *message) {
	std::string err;
	if (!expect(!loads(text, err), message)) {
		return false;
	}
	if (!expect(err.find(needle) != std::string::npos, message)) {
		std::fprintf(stderr, "  error was: '%s'\n", err.c_str());
		return false;
	}
	return true;
}

} // namespace

int main() {
	std::string err;

	// The valid shape is admitted.
	if (!expect(loads(serialise(valid_config()), err), "a complete config is admitted")) {
		std::fprintf(stderr, "  err: %s\n", err.c_str());
		return 1;
	}

	// Empty polydata: the writer omits the line, the reader refuses the result.
	opennova::TrnConfig no_polydata = valid_config();
	no_polydata.polydata = "";
	const std::string no_polydata_text = serialise(no_polydata);
	if (!expect(no_polydata_text.find("polytrn_polydata") == std::string::npos,
			"save_trn must not emit polytrn_polydata when polydata is empty")) return 1;
	if (!expect_rejected(no_polydata_text, "polytrn_polydata",
			"an empty polydata name is rejected")) return 1;

	opennova::TrnConfig no_colormap = valid_config();
	no_colormap.colormap = "";
	if (!expect_rejected(serialise(no_colormap), "polytrn_colormap",
			"an empty colormap name is rejected")) return 1;

	opennova::TrnConfig no_detailmap = valid_config();
	no_detailmap.detailmap = "";
	if (!expect_rejected(serialise(no_detailmap), "polytrn_detailmap",
			"an empty detailmap name is rejected")) return 1;

	// sectorcount: > 16 and non-power-of-two both reject; 0, 1, 2, 4, 8, 16 admit.
	for (int count : {3, 5, 6, 7, 9, 12, 15}) {
		opennova::TrnConfig cfg = valid_config();
		cfg.sector_count = count;
		cfg.sector_rows = 1;
		if (!expect_rejected(serialise(cfg), "polytrn_sectorcount",
				"a sectorcount that is not a power of two is rejected")) {
			std::fprintf(stderr, "  sector_count = %d\n", count);
			return 1;
		}
	}
	// save_trn emits sector_count columns from the 16-wide grid, so the > 16
	// cases are raw text.
	for (const char *count : {"17", "32", "64"}) {
		const std::string text =
				"terrain_name wide\r\npolytrn_colormap c.tga\r\npolytrn_detailmap d.tga\r\n"
				"polytrn_polydata p.cpt\r\npolytrn_sectorcount " + std::string(count) + "\r\n";
		if (!expect_rejected(text, "polytrn_sectorcount",
				"a sectorcount above 16 is rejected")) {
			std::fprintf(stderr, "  sector_count = %s\n", count);
			return 1;
		}
	}
	for (int count : {1, 2, 4, 8, 16}) {
		opennova::TrnConfig cfg = valid_config();
		cfg.sector_count = count;
		cfg.sector_rows = 1;
		if (!expect(loads(serialise(cfg), err), "a power-of-two sectorcount <= 16 is admitted")) {
			std::fprintf(stderr, "  sector_count = %d err: %s\n", count, err.c_str());
			return 1;
		}
	}
	// `((n - 1) & n) != 0` never fires for 0: a config with no sectorcount admits.
	if (!expect(loads("terrain_name zero\r\npolytrn_colormap c.tga\r\npolytrn_detailmap d.tga\r\n"
			"polytrn_polydata p.cpt\r\n", err),
			"a config without sector lines passes the power-of-two check as zero")) {
		std::fprintf(stderr, "  err: %s\n", err.c_str());
		return 1;
	}

	// Row count: the number of polytrn_sectors lines, counted past the 16 the
	// grid stores.
	for (int rows : {3, 5, 6, 7, 9, 12, 15}) {
		opennova::TrnConfig cfg = valid_config();
		cfg.sector_rows = rows;
		if (!expect_rejected(serialise(cfg), "polytrn_sectors row count",
				"a polytrn_sectors row count that is not a power of two is rejected")) {
			std::fprintf(stderr, "  sector_rows = %d\n", rows);
			return 1;
		}
	}
	{
		std::string text = serialise(valid_config());
		for (int extra = 0; extra < 9; ++extra) {
			text += "polytrn_sectors\t\t\t0\t0\t0\t0\t0\t0\t0\t0\r\n";
		}
		if (!expect_rejected(text, "polytrn_sectors row count",
				"more than 16 polytrn_sectors lines are rejected")) return 1;
	}

	// The gate itself over a record (trn_refusal), in its order, the row count the caller's: what load_trn's
	// walk passes (every line read) or what a record's writer writes (max(1, sector_rows)).
	{
		using opennova::TrnRefusal;
		using opennova::trn_refusal;
		const opennova::TrnConfig valid = valid_config();
		if (!expect(trn_refusal(valid, valid.sector_rows) == TrnRefusal::None, "the gate takes a complete record")) return 1;
		opennova::TrnConfig none = valid;
		none.colormap.clear();
		none.detailmap.clear();
		none.polydata.clear();
		none.sector_count = 3;
		if (!expect(trn_refusal(none, 3) == TrnRefusal::NoColormap, "the colour map is the gate's first leg")) return 1;
		none.colormap = "c.tga";
		if (!expect(trn_refusal(none, 3) == TrnRefusal::NoDetailmap, "the detail map its second")) return 1;
		none.detailmap = "d.tga";
		if (!expect(trn_refusal(none, 3) == TrnRefusal::NoPolydata, "the height data its third")) return 1;
		none.polydata = "p.cpt";
		if (!expect(trn_refusal(none, 3) == TrnRefusal::SectorRows, "the row count before the width")) return 1;
		if (!expect(trn_refusal(none, 4) == TrnRefusal::SectorCount, "then the width")) return 1;
		none.sector_count = 16;
		if (!expect(trn_refusal(none, 0) == TrnRefusal::None && trn_refusal(none, 16) == TrnRefusal::None &&
				trn_refusal(none, 17) == TrnRefusal::SectorRows && trn_refusal(none, 32) == TrnRefusal::SectorRows,
				"rows: zero and the powers of two to 16")) return 1;
		none.sector_count = 32;
		if (!expect(trn_refusal(none, 1) == TrnRefusal::SectorCount, "a width past the grid's side")) return 1;
		none.sector_count = -4;
		if (!expect(trn_refusal(none, 1) == TrnRefusal::SectorCount, "a negative width is no power of two")) return 1;
		if (!expect(opennova::kTerrainGridSide == 16, "the grid is 16 sectors a side")) return 1;
	}

	std::printf("OK: load_trn admits only configs the retail loader admits\n");
	return 0;
}
