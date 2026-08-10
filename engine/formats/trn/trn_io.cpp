#include "trn/trn_io.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace opennova {

namespace {

static std::string unquote(const std::string &value) {
	if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
		return value.substr(1, value.size() - 2);
	}
	return value;
}

static void parse_foliage_attribs(std::istringstream &iss, FoliageDef &def) {
	std::string token;
	while (iss >> token) {
		token = unquote(token);
		if (token == "forceon") {
			def.attrib_flags = static_cast<uint8_t>(def.attrib_flags | FOLIAGE_ATTRIB_FORCE_ON);
		} else if (token == "shadow") {
			def.attrib_flags = static_cast<uint8_t>(def.attrib_flags | FOLIAGE_ATTRIB_SHADOW);
		}
	}
}

} // namespace

bool load_trn(std::istream &f, TrnConfig &out, std::string &error) {
	(void)error;

	std::string line;
	while (std::getline(f, line)) {
		auto sc = line.find(';');
		if (sc != std::string::npos) {
			line = line.substr(0, sc);
		}

		std::istringstream iss(line);
		std::string key;
		if (!(iss >> key)) {
			continue;
		}

		if (key == "foliage") {
			if (out.foliage_defs.size() < 4) {
				FoliageDef def;
				std::string fline;
				while (std::getline(f, fline)) {
					auto fsc = fline.find(';');
					if (fsc != std::string::npos) {
						fline = fline.substr(0, fsc);
					}
					std::istringstream fiss(fline);
					std::string fk;
					if (!(fiss >> fk)) {
						continue;
					}
					if (fk == "end") {
						break;
					}
					std::string fv;
					if (!(fiss >> fv)) {
						continue;
					}
					if (fk == "graphic") {
						def.graphic = unquote(fv);
					} else if (fk == "color_lower") {
						def.color_lower = std::atoi(fv.c_str());
					} else if (fk == "color_upper") {
						def.color_upper = std::atoi(fv.c_str());
					} else if (fk == "match") {
						def.match = std::atoi(fv.c_str());
					} else if (fk == "attrib") {
						def.attrib_flags = 0;
						def.attrib_flags = static_cast<uint8_t>(def.attrib_flags | (
							fv == "forceon" ? FOLIAGE_ATTRIB_FORCE_ON :
							fv == "shadow" ? FOLIAGE_ATTRIB_SHADOW : 0));
						parse_foliage_attribs(fiss, def);
					}
				}
				out.foliage_defs.push_back(foliage_normalize_def(def));
			}
			continue;
		}

		std::string val;
		if (!(iss >> val)) {
			continue;
		}

		if (key == "terrain_name") {
			out.name = unquote(val);
		} else if (key == "polytrn_colormap") {
			out.colormap = unquote(val);
		} else if (key == "polytrn_detailmap_c1") {
			out.detailmap_c1 = unquote(val);
		} else if (key == "polytrn_detailmap_c2") {
			out.detailmap_c2 = unquote(val);
		} else if (key == "polytrn_detailmap_c3") {
			out.detailmap_c3 = unquote(val);
		} else if (key == "polytrn_detailblendmap") {
			out.detailblendmap = unquote(val);
		} else if (key == "polytrn_polydata") {
			out.polydata = unquote(val);
		} else if (key == "polytrn_detailmap") {
			out.detailmap = unquote(val);
		} else if (key == "polytrn_detailmap2") {
			out.detailmap2 = unquote(val);
		} else if (key == "polytrn_detailmapdist") {
			out.detailmapdist = unquote(val);
		} else if (key == "polytrn_detailmapdist2") {
			out.detailmapdist2 = unquote(val);
		} else if (key == "polytrn_detaildensity") {
			out.detail_density = std::atoi(val.c_str());
		} else if (key == "polytrn_detaildensity2") {
			out.detail_density2 = std::atoi(val.c_str());
		} else if (key == "polytrn_sectorcount") {
			out.sector_count = std::atoi(val.c_str());
		} else if (key == "polytrn_wrapx") {
			out.wrap_x = std::atoi(val.c_str());
		} else if (key == "polytrn_wrapy") {
			out.wrap_y = std::atoi(val.c_str());
		} else if (key == "lock_topleft") {
			out.lock_topleft.x = std::atoi(val.c_str());
			std::string val2;
			if (iss >> val2) {
				out.lock_topleft.y = std::atoi(val2.c_str());
			}
		} else if (key == "lock_topright") {
			out.lock_topright.x = std::atoi(val.c_str());
			std::string val2;
			if (iss >> val2) {
				out.lock_topright.y = std::atoi(val2.c_str());
			}
		} else if (key == "lock_bottomleft") {
			out.lock_bottomleft.x = std::atoi(val.c_str());
			std::string val2;
			if (iss >> val2) {
				out.lock_bottomleft.y = std::atoi(val2.c_str());
			}
		} else if (key == "lock_bottomright") {
			out.lock_bottomright.x = std::atoi(val.c_str());
			std::string val2;
			if (iss >> val2) {
				out.lock_bottomright.y = std::atoi(val2.c_str());
			}
		} else if (key == "horizon") {
			out.horizon = std::atof(val.c_str());
		} else if (key == "polytrn_origin") {
			out.origin_x = std::atoi(val.c_str());
			std::string val2;
			if (iss >> val2) {
				out.origin_y = std::atoi(val2.c_str());
			}
		} else if (key == "polytrn_sectors") {
			if (out.sector_rows < 16) {
				int col = 0;
				out.sector_grid[out.sector_rows][col++] = std::atoi(val.c_str());
				std::string tok;
				while (col < 16 && col < out.sector_count && (iss >> tok)) {
					out.sector_grid[out.sector_rows][col++] = std::atoi(tok.c_str());
				}
				out.sector_rows++;
			}
		} else if (key == "water_height") {
			out.water_height = std::atoi(val.c_str());
		} else if (key == "polytrn_charmap") {
			out.charmap = unquote(val);
		} else if (key == "polytrn_foliagemap") {
			out.foliagemap = unquote(val);
		} else if (key == "polytrn_tilestrip") {
			out.tilestrip = unquote(val);
		} else if (key == "polytrn_tileinfo") {
			out.tileinfo = unquote(val);
		}
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
	f << nl;
	f << "water_height     " << cfg.water_height << nl;
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
		if (normalized.match >= 0) {
			f << "  match           " << normalized.match << nl;
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

} // namespace opennova
