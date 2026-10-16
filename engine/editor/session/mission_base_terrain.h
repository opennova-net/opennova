#pragma once

// The terrain the original editor measures a mission's entities over (S24's .mis heights, S25's follow pass): its
// read of a terrain's .trn and of the .cpt that .trn names, and its sampler's terrain over a height atlas
// (mission::MisTerrain, mission::mis_terrain_height). Read by hand_base_heights (DocumentSet: the heights a mission's
// .mis is written over) and by the follow pass (terrain_follow.h: the entities a sculpt's bake moves).

#include <cstdint>
#include <string>
#include <vector>

#include <formats/mission/mission_mis.h>
#include <formats/trn/trn.h>

namespace opennova {
class FileSource;
}

namespace opennova::editor {

// The original editor's read of a terrain's .trn for its base heights: the .trn alone (its own parser over the file,
// the game's later passes over overcast.def and the mission's .env never run [orig: JOTACmed.exe sub_464DE0 @ 0x464de0
// -> sub_463C90 @ 0x463c90]) and its last polytrn_scale line by the CRT's atol (128 where it has none [orig:
// JOTACmed.exe, the arm @ 0x464a38; the default, sub_464B60 @ 0x464bb6]). The .trn is read through the game's parser
// and admission gate (load_mission_trn with no later texts): a terrain the game refuses (no polydata, a sector count or
// row count that is no power of two) samples no height, where the original editor would; and a .trn naming a
// polytrn_sectorcount but no polytrn_sectors row, whose grid the original editor fills with the .trn's other words
// (scale, wraps) and the game with no sector, samples none either. No shipped terrain is either. `trn_file`: the
// .trn's name as `files` finds it. False where it does not read.
bool read_base_trn(const FileSource &files, const std::string &trn_file, TrnConfig &trn, int32_t &scale);

// The terrain named `terrain` (a mission header's, no extension) as the original editor reads it: its .trn
// (read_base_trn) and the .cpt its polytrn_polydata names, its DPTH or CDEP depth words into a cleared 1024 x 1024
// grid, so a CDEP of fewer words leaves the rest 0 [orig: JOTACmed.exe sub_44C2B0 @ 0x44c2b0, the clear @ 0x44c2e0,
// the DPTH arm @ 0x44c64b, the CDEP arm @ 0x44c5e7]; `cpt_stamp` the .cpt's stamp in `files`. False where either does
// not read.
bool read_base_terrain(const FileSource &files, const std::string &terrain, int32_t &scale, TrnConfig &trn,
		std::vector<uint16_t> &atlas, uint64_t &cpt_stamp);

// The sampler's terrain (mission::mis_terrain_height's) over `heights` (1024 x 1024 depth words, the .cpt's atlas as
// the game loads it) with the .trn's own sector grid, origin, wraps and scale. `trn` and `heights` outlive it.
mission::MisTerrain base_terrain_sampler(const TrnConfig &trn, int32_t scale, const uint16_t *heights);

} // namespace opennova::editor
