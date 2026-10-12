#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <formats/cpt/cpt.h>

#include "mesh_data.h"

namespace opennova::trngen {

// A node's .cpt tile from its .tms mesh: the packed vertex pairs and the eight LOD lists the file
// keeps (every other entry), sized as the level's nodes. [orig: TrnGen.exe build_terrain_thread @
// 0x4013A0, its CPT export]
CptTile build_tile_from_mesh(const MeshData& mesh, int size, bool is_single);

// A node's .cpt tile from its .tml bytes the way the bake makes it: the .tml read back, its faces made
// strips and its vertices put in their order, written as its .tms, read back again and exported, each
// step through the TPM1 codec as on disk (mesh_data.h). False, with `error`, for bytes that do not read
// back or a strip buffer the port refuses (tristrip.h). [orig: TrnGen.exe build_terrain_thread @
// 0x4013A0, the multi-resolution tile loop and the CPT export; sub_404480 @ 0x404480]
bool cpt_tile_from_tml(const std::vector<uint8_t>& tml, int size, CptTile& out, std::string& error);

} // namespace opennova::trngen
