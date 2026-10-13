#pragma once

#include <memory>

namespace opennova::editor {

class ProjectCheck;

// The tile placement type's project check (ADR 0046 S25 B5; DocumentType::project_check, documents/project_check.h):
// what each mission's tiles come to as the game's load reads them, across the files that decide it. These are
// Problems rows after the build's gate (the game draws each of them, nothing refused):
// - til.empty: a mission's own .til that holds no tile, which the game reads in place of its terrain's tiles
//   [orig: PolyTrn_LoadTerrainConfig @ 0x60E6D2..0x60E6E5], so the mission draws none of them;
// - til.index_past_atlas: tiles of the placement a mission draws (its own .til, else its terrain's) naming a cell
//   past the tile set the mission draws them from, which the game draws from the set's last row of texels (D-TIL-7),
//   or every tile of a set under 64 texels high (D-TIL-8).
// It reads the missions' headers, their terrains' configurations and placements and the tile sets' headers through
// the project's files (the open documents standing in), keeps what it made and makes it again only when one of
// those files or the project's missions moved.
std::unique_ptr<ProjectCheck> make_til_project_check();

} // namespace opennova::editor
