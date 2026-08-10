#pragma once

#include <cpt/cpt.h>

#include <functional>
#include <string>

namespace opennova {

using TilePackProgressCallback = std::function<void(int, int, const std::string &)>;

void export_terrain_cpt(const std::string &output_prefix,
                        const std::string &tile_ext,
                        const std::string &cpt_path,
                        int tile_size,
                        int min_tile_size,
                        const std::string &terrain_name,
                        const std::string &creator,
                        DepthFormat depth_format,
                        const TilePackProgressCallback &progress_callback = {});

} // namespace opennova
