#include <formats/trn/trn_io.h>

#include <base/io/ascii_config.h>
#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iterator>
#include <sstream>

// [orig: PolyTrn_LoadTerrainConfig @0x60e3d0 -> Terrain_ParseConfigCallback @0x60f330 — the .trn
//  key parser (foliage attribs: "forceon" @0x60f58b; `match` stores up to 7 byte args per
//  slot at +0x108..); Terrain_LoadEnvironmentConfig @0x610940 — the admission gate at its tail]

namespace opennova {

bool load_trn(std::istream &f, TrnConfig &out, std::string &error) {
	int seen_sector_rows = 0;
	// The parser's state: inside a `foliage` block, and the blocks an `end`
	// has closed [orig: dword_31BC904, dword_31BC900].
	bool in_foliage = false;
	int foliage_closed = 0;
	FoliageDef def;

	// The lines and tokens are the shared retail walk's (io::for_each_config_line:
	// a CR LF pair ends a line and nothing else does, an unterminated last line
	// loses its final byte, `;` or `//` outside quotes cuts a line, and space,
	// comma or tab separates tokens) [orig: File_ParseASCIIFile @0x53D810]. A key
	// compares without case and reads its values by token, numbers through the
	// CRT's atol and atof (io::retail_atol, io::retail_atof), whatever the line's
	// count [orig: Terrain_ParseConfigCallback @0x60f330, the stricmp of
	// tokens[1] in every arm].
	const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	io::for_each_config_line(text.data(), text.size(), [&](const io::ConfigTokens &tokens) {
		const std::string key = strutil::to_lower(tokens.tokens[0]);
		const char *value = tokens.token(1);

		// Inside a block every line is the block's (a `foliage` line too): the
		// first four blocks read their keys into their slots, and from the fifth
		// on nothing closes the block, so the rest of the file is read by no arm
		// [orig: the dword_31BC904 test, then `dword_31BC900 < 4` around every
		// block key, `end` bumping the count].
		if (in_foliage) {
			if (foliage_closed >= 4) return;
			if (key == "end") {
				out.foliage_defs.push_back(foliage_normalize_def(def));
				++foliage_closed;
				in_foliage = false;
			} else if (key == "graphic") {
				def.graphic = value;
			} else if (key == "color_lower") {
				def.color_lower = io::retail_atol(value);
			} else if (key == "color_upper") {
				def.color_upper = io::retail_atol(value);
			} else if (key == "match") {
				// Up to 7 args as bytes, to the line's count; only the first
				// FOLIAGE_MATCH_CODES are ever consumed (foliage.h), so the rest
				// are read and dropped here [orig: the match arm, `idx < 8` and
				// `idx >= count` @0x60f330].
				def.match.fill(FOLIAGE_MATCH_UNSET);
				for (int i = 1; i < tokens.count && i < 8; ++i)
					if (i - 1 < FOLIAGE_MATCH_CODES)
						def.match[static_cast<size_t>(i - 1)] =
								static_cast<uint8_t>(io::retail_atol(tokens.tokens[i]));
			} else if (key == "attrib") {
				// Each arg ORs its flag; nothing clears them [orig: the attrib
				// arm, "forceon" @0x60f58b ORs 1 and "shadow" 2].
				for (int i = 1; i < tokens.count && i < 8; ++i) {
					if (strutil::iequals(tokens.tokens[i], "forceon"))
						def.attrib_flags = static_cast<uint8_t>(def.attrib_flags | FOLIAGE_ATTRIB_FORCE_ON);
					if (strutil::iequals(tokens.tokens[i], "shadow"))
						def.attrib_flags = static_cast<uint8_t>(def.attrib_flags | FOLIAGE_ATTRIB_SHADOW);
				}
			}
			return;
		}

		if (key == "foliage") {
			in_foliage = true;
			def = FoliageDef();
		} else if (key == "terrain_name") {
			out.name = value;
		} else if (key == "terrain_creator") {
			out.creator = value;
		} else if (key == "polytrn_colormap") {
			out.colormap = value;
		} else if (key == "polytrn_detailmap_c1") {
			out.detailmap_c1 = value;
		} else if (key == "polytrn_detailmap_c2") {
			out.detailmap_c2 = value;
		} else if (key == "polytrn_detailmap_c3") {
			out.detailmap_c3 = value;
		} else if (key == "polytrn_detailblendmap") {
			out.detailblendmap = value;
		} else if (key == "polytrn_polydata") {
			out.polydata = value;
		} else if (key == "polytrn_detailmap") {
			out.detailmap = value;
		} else if (key == "polytrn_detailmap2") {
			out.detailmap2 = value;
		} else if (key == "polytrn_detailmapdist") {
			out.detailmapdist = value;
		} else if (key == "polytrn_detailmapdist2") {
			out.detailmapdist2 = value;
		} else if (key == "polytrn_detaildensity") {
			out.detail_density = io::retail_atol(value);
		} else if (key == "polytrn_detaildensity2") {
			out.detail_density2 = io::retail_atol(value);
		} else if (key == "polytrn_sectorcount") {
			out.sector_count = io::retail_atol(value);
		} else if (key == "polytrn_wrapx") {
			out.wrap_x = io::retail_atol(value);
		} else if (key == "polytrn_wrapy") {
			out.wrap_y = io::retail_atol(value);
		} else if (key == "lock_topleft") {
			out.lock_topleft.x = io::retail_atol(value);
			out.lock_topleft.y = io::retail_atol(tokens.token(2));
		} else if (key == "lock_topright") {
			out.lock_topright.x = io::retail_atol(value);
			out.lock_topright.y = io::retail_atol(tokens.token(2));
		} else if (key == "lock_bottomleft") {
			out.lock_bottomleft.x = io::retail_atol(value);
			out.lock_bottomleft.y = io::retail_atol(tokens.token(2));
		} else if (key == "lock_bottomright") {
			out.lock_bottomright.x = io::retail_atol(value);
			out.lock_bottomright.y = io::retail_atol(tokens.token(2));
		} else if (key == "horizon") {
			out.horizon = io::retail_atof(value);
		} else if (key == "polytrn_origin") {
			out.origin_x = io::retail_atol(value);
			out.origin_y = io::retail_atol(tokens.token(2));
		} else if (key == "polytrn_sectors") {
			// A row reads polytrn_sectorcount columns (16 at most) whatever the
			// line's count, a short line's missing ones from the slots an earlier
			// line left; every row line counts (+5960), and the gate below
			// rejects more than 16, which the grid has no room for, so they are
			// counted, not stored [orig: the polytrn_sectors arm @0x60f330,
			// dword_31BCB30].
			++seen_sector_rows;
			if (out.sector_rows < 16) {
				for (int col = 0; col < 16 && col < out.sector_count; ++col)
					out.sector_grid[out.sector_rows][col] = io::retail_atol(tokens.token(1 + col));
				out.sector_rows++;
			}
		} else if (key == "water_height") {
			out.water_height = io::retail_atol(value);
		} else if (key == "water_rgb") {
			// The environment reader's arms over the terrain's lines (trn.h): a colour's three bytes as
			// atol of tokens 2..4 packed at the load's envscale of 1, held to a byte [orig:
			// TimeOfDay_ParseProperty @ 0x57caf6; Color_ScaleRGBAndPack @ 0x57f890], the murk's atof held at
			// 0.99 [orig: TimeOfDay_ParseProperty @ 0x57cb7c..0x57cba9].
			out.water_rgb_set = true;
			for (int c = 0; c < 3; ++c)
				out.water_rgb[static_cast<size_t>(c)] = std::clamp(io::retail_atol(tokens.token(1 + c)), 0, 255);
		} else if (key == "water_murk") {
			out.water_murk_set = true;
			out.water_murk = std::min(static_cast<float>(io::retail_atof(value)), 0.99f);
		} else if (key == "polytrn_charmap") {
			out.charmap = value;
		} else if (key == "polytrn_foliagemap") {
			out.foliagemap = value;
		} else if (key == "polytrn_tilestrip") {
			out.tilestrip = value;
		} else if (key == "polytrn_tileinfo") {
			out.tileinfo = value;
		}
	});
	// A block no `end` closed (the file ran out, or its `end` is the
	// unterminated last line, read `en`) still wrote its keys into its slot, and
	// the runtime takes all four slots whose graphic is named, not the closed
	// count, so the block is a definition [orig: the block keys write slot
	// dword_31BC900 directly; Terrain_Init copies all four 0x218-byte slots
	// @0x60FD11..0x60FD16 (sub_5FF4C0); Foliage_RemapPixelToDefMask @0x5FF4E0
	// gates each slot on its graphic's first byte]. JO:CA's Dvxi4.trn and
	// Dvxi4_c.trn end on such a block.
	if (in_foliage && foliage_closed < 4) out.foliage_defs.push_back(foliage_normalize_def(def));

	// The admission gate [orig: Terrain_LoadEnvironmentConfig @0x610940 tail]:
	// the config is rejected (returns 0) when the colormap (+256), detailmap
	// (+512) or polydata (+3072) name is empty, when the `polytrn_sectors` row
	// count (+5960) or `polytrn_sectorcount` (+5956) exceeds 16, or when either
	// is not a power of two (`((n - 1) & n) != 0`). Its last leg,
	// `Terrain_ShiftHeightmapRows @0x60f190`, never rejects: it extends the
	// parsed grid to 16 columns and rows (periodically under wrap, else the
	// last column and row copied outward) and returns 0 on every path
	// [orig: @0x60F2A5..0x60F317, @0x60F31A]; the extension below is it.
	if (out.colormap.empty()) {
		error = "TRN rejected: polytrn_colormap is empty";
		return false;
	}
	if (out.detailmap.empty()) {
		error = "TRN rejected: polytrn_detailmap is empty";
		return false;
	}
	if (out.polydata.empty()) {
		error = "TRN rejected: polytrn_polydata is empty";
		return false;
	}
	const auto power_of_two_or_zero = [](int n) { return ((n - 1) & n) == 0; };
	if (seen_sector_rows > 16 || !power_of_two_or_zero(seen_sector_rows)) {
		error = "TRN rejected: polytrn_sectors row count " + std::to_string(seen_sector_rows) +
			" is not a power of two <= 16";
		return false;
	}
	if (out.sector_count > 16 || !power_of_two_or_zero(out.sector_count)) {
		error = "TRN rejected: polytrn_sectorcount " + std::to_string(out.sector_count) +
			" is not a power of two <= 16";
		return false;
	}

	const int rows = std::max(out.sector_rows, 1);
	const int cols = std::max(out.sector_count, 1);

	for (int r = 0; r < rows; ++r) {
		for (int c = cols; c < 16; ++c) {
			out.sector_grid[r][c] = out.wrap_x ? out.sector_grid[r][c % cols] : out.sector_grid[r][cols - 1];
		}
	}
	for (int r = rows; r < 16; ++r) {
		for (int c = 0; c < 16; ++c) {
			out.sector_grid[r][c] = out.wrap_y ? out.sector_grid[r % rows][c] : out.sector_grid[rows - 1][c];
		}
	}

	return true;
}

bool save_trn(std::ostream &f, const TrnConfig &cfg, std::string &error) {
	const char *nl = "\r\n";

	f << "terrain_name     \"" << cfg.name << "\"" << nl;
	if (!cfg.creator.empty()) f << "terrain_creator  \"" << cfg.creator << "\"" << nl;
	f << nl;
	f << "water_height     " << cfg.water_height << nl;
	if (cfg.water_rgb_set)
		f << "water_rgb        " << cfg.water_rgb[0] << "," << cfg.water_rgb[1] << "," << cfg.water_rgb[2] << nl;
	if (cfg.water_murk_set) {
		// The fewest digits from six that the parser's atof reads back as the same float.
		std::string murk;
		for (int digits = 6; digits <= 9; ++digits) {
			std::ostringstream out;
			out << std::setprecision(digits) << std::defaultfloat << cfg.water_murk;
			murk = out.str();
			if (static_cast<float>(io::retail_atof(murk.c_str())) == cfg.water_murk) break;
		}
		f << "water_murk       " << murk << nl;
	}
	f << "horizon          " << cfg.horizon << nl;
	f << nl;

	if (!cfg.colormap.empty()) {
		f << "polytrn_colormap         " << cfg.colormap << nl;
	}
	if (!cfg.detailmap.empty()) {
		f << "polytrn_detailmap        " << cfg.detailmap << nl;
	}
	if (!cfg.detailmap_c1.empty()) {
		f << "polytrn_detailmap_c1     " << cfg.detailmap_c1 << nl;
	}
	if (!cfg.detailmap_c2.empty()) {
		f << "polytrn_detailmap_c2     " << cfg.detailmap_c2 << nl;
	}
	if (!cfg.detailmap_c3.empty()) {
		f << "polytrn_detailmap_c3     " << cfg.detailmap_c3 << nl;
	}
	if (!cfg.detailmap2.empty()) {
		f << "polytrn_detailmap2       " << cfg.detailmap2 << nl;
	}
	if (!cfg.detailmapdist.empty()) {
		f << "polytrn_detailmapdist    " << cfg.detailmapdist << nl;
	}
	if (!cfg.detailmapdist2.empty()) {
		f << "polytrn_detailmapdist2   " << cfg.detailmapdist2 << nl;
	}
	if (!cfg.polydata.empty()) {
		f << "polytrn_polydata         " << cfg.polydata << nl;
	}
	if (!cfg.tilestrip.empty()) {
		f << "polytrn_tilestrip        " << cfg.tilestrip << nl;
	}
	if (!cfg.charmap.empty()) {
		f << "polytrn_charmap          " << cfg.charmap << nl;
	}
	if (!cfg.foliagemap.empty()) {
		f << "polytrn_foliagemap       " << cfg.foliagemap << nl;
	}
	if (!cfg.detailblendmap.empty()) {
		f << "polytrn_detailblendmap   " << cfg.detailblendmap << nl;
	}
	if (!cfg.tileinfo.empty()) {
		f << "polytrn_tileinfo         " << cfg.tileinfo << nl;
	}
	f << nl;

	f << "polytrn_detaildensity\t\t" << cfg.detail_density << nl;
	f << "polytrn_detaildensity2\t\t" << cfg.detail_density2 << nl;
	f << "polytrn_sectorcount\t\t" << cfg.sector_count << nl;
	f << "polytrn_wrapx\t\t\t" << cfg.wrap_x << nl;
	f << "polytrn_wrapy\t\t\t" << cfg.wrap_y << nl;
	const bool has_quadrant_locks =
		cfg.lock_topleft.x != 0 || cfg.lock_topleft.y != 0 ||
		cfg.lock_topright.x != 0 || cfg.lock_topright.y != 0 ||
		cfg.lock_bottomleft.x != 0 || cfg.lock_bottomleft.y != 0 ||
		cfg.lock_bottomright.x != 0 || cfg.lock_bottomright.y != 0;
	if (has_quadrant_locks) {
		f << "lock_topleft\t\t\t" << cfg.lock_topleft.x << "\t" << cfg.lock_topleft.y << nl;
		f << "lock_topright\t\t\t" << cfg.lock_topright.x << "\t" << cfg.lock_topright.y << nl;
		f << "lock_bottomleft\t\t" << cfg.lock_bottomleft.x << "\t" << cfg.lock_bottomleft.y << nl;
		f << "lock_bottomright\t" << cfg.lock_bottomright.x << "\t" << cfg.lock_bottomright.y << nl;
	}
	f << nl;
	f << "polytrn_origin\t\t\t" << cfg.origin_x << "\t" << cfg.origin_y << nl;
	f << nl;

	const int rows = cfg.sector_rows > 0 ? cfg.sector_rows : 1;
	const int cols = cfg.sector_count > 0 ? cfg.sector_count : 1;
	for (int r = 0; r < rows; ++r) {
		f << "polytrn_sectors\t\t\t";
		for (int c = 0; c < cols; ++c) {
			f << cfg.sector_grid[r][c];
			if (c < cols - 1) {
				f << "\t";
			}
		}
		f << nl;
	}

	for (const auto &def : cfg.foliage_defs) {
		const FoliageDef normalized = foliage_normalize_def(def);
		f << nl << "foliage" << nl;
		if (!normalized.graphic.empty()) {
			f << "  graphic         " << normalized.graphic << nl;
		}
		f << "  color_lower     " << normalized.color_lower << nl;
		f << "  color_upper     " << normalized.color_upper << nl;
		if (foliage_def_match_count(normalized) > 0) {
			// Every authored code on the one `match` line, in retail's arg order.
			f << "  match           ";
			bool wrote = false;
			for (int code : normalized.match) {
				if (code < 0) {
					continue;
				}
				if (wrote) {
					f << " ";
				}
				f << code;
				wrote = true;
			}
			f << nl;
		}
		if (normalized.attrib_flags != 0) {
			f << "  attrib          ";
			bool wrote = false;
			if ((normalized.attrib_flags & FOLIAGE_ATTRIB_SHADOW) != 0) {
				f << "shadow";
				wrote = true;
			}
			if ((normalized.attrib_flags & FOLIAGE_ATTRIB_FORCE_ON) != 0) {
				if (wrote) {
					f << " ";
				}
				f << "forceon";
			}
			f << nl;
		}
		f << "end" << nl;
	}

	if (!f.good()) {
		error = "Write error for TRN";
		return false;
	}
	return true;
}

std::string trn_mission_tilestrip(const TrnConfig &trn,
		const std::string &mission_tile_set) {
	// An empty mission name leaves the .trn value [orig: the NUL test
	// @ 0x6109C8 skipping to @ 0x610A24].
	if (mission_tile_set.empty() || mission_tile_set.front() == '\0') {
		return trn.tilestrip;
	}
	// Everything from the FIRST '.' becomes ".TGA"; without a dot ".TGA" is
	// appended [orig: Path_ReplaceOrAppendExtension @ 0x53C7C0..0x53C7CD].
	std::string atlas = mission_tile_set.substr(0, mission_tile_set.find('\0'));
	const std::size_t dot = atlas.find('.');
	if (dot != std::string::npos) atlas.resize(dot);
	return atlas + ".TGA";
}

} // namespace opennova
