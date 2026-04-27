#pragma once

#include <foliage/foliage.h>

#include <string>
#include <vector>

namespace opennova {

struct TrnConfig {
	std::string name;
	std::string colormap;
	std::string detailmap_c1;
	std::string detailmap_c2;
	std::string detailmap_c3;
	std::string detailblendmap;
	std::string polydata;
	std::string detailmap;
	std::string detailmap2;
	std::string detailmapdist;
	std::string detailmapdist2;
	int detail_density = 128;
	int detail_density2 = 8;
	int sector_count = 0;
	int origin_x = 0;
	int origin_y = 0;
	int water_height = 0;
	int wrap_x = 0;
	int wrap_y = 0;
	double horizon = 0.0;
	int sector_grid[16][16] = {};
	int sector_rows = 0;
	std::string foliagemap;
	std::vector<FoliageDef> foliage_defs;
	std::string charmap;
	std::string tilestrip;
	std::string tileinfo;
};

} // namespace opennova
