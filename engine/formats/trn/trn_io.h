#pragma once

#include "trn.h"

#include <istream>
#include <ostream>
#include <string>

namespace opennova {

// Parses a .trn config. Returns false (with `error` set) when the retail
// admission gate rejects it: empty polytrn_colormap / polytrn_detailmap /
// polytrn_polydata, or a polytrn_sectors row count or polytrn_sectorcount
// that is > 16 or not a power of two [orig: Terrain_LoadEnvironmentConfig
// @0x610940]. `out` is left partially filled on rejection.
bool load_trn(std::istream &input, TrnConfig &out, std::string &error);
bool save_trn(std::ostream &output, const TrnConfig &cfg, std::string &error);

} // namespace opennova
