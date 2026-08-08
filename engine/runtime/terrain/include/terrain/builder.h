#pragma once

#include <cpt/cpt.h>
#include <tpj/tpj.h>

#include <functional>
#include <string>

namespace opennova {

struct TerrainBuildProgress {
	std::string phase;
	std::string message;
	int current = 0;
	int total = 0;
	float ratio = 0.0f;
};

using TerrainBuildProgressCallback = std::function<void(const TerrainBuildProgress &)>;

struct TerrainBuildOptions {
	DepthFormat depth_format = DepthFormat::DPTH;
	bool smooth_depthmap = true;
	bool rasterize_depth = true;
};

void build_terrain(const TpjProject &project,
                   const std::string &output_dir,
                   const TerrainBuildOptions &options = {},
                   const TerrainBuildProgressCallback &progress_callback = {});

} // namespace opennova

