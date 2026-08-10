#pragma once

#include <trn/trn.h>

#include <string>
#include <vector>

namespace opennova {

using TpjLockCoord = TerrainLockCoord;

struct TpjProject {
	using LockCoord = TpjLockCoord;

	std::string terrain_name;
	std::string creator;
	std::string path;
	std::string depthmap;
	std::string output;
	LockCoord lock_topleft;
	LockCoord lock_topright;
	LockCoord lock_bottomleft;
	LockCoord lock_bottomright;
	std::string charmap;
	std::string foliagemap;
	std::string tilestrip;
	std::string tileinfo;
	std::vector<FoliageDef> foliage_defs;
	bool has_metadata = false;
};

} // namespace opennova
