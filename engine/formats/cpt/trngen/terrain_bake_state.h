#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <formats/cpt/cpt.h>

#include "lod_mesh_data.h"
#include "quadtree_node.h"
#include "terrain_bake.h"

namespace opennova::trngen {

// What a bake keeps so that a region of its depth map can be baked again (terrain_region_bake.h) to the
// bytes a whole bake of the new map makes. TrnGen.exe re-bakes no region: its tile cache takes a node's
// tile file only while the file is newer than the depth map, so any edit of the map rebuilds every node
// [orig: TrnGen.exe sub_402720 @ 0x402790..0x4027AF, the depth map's time from BuildTerrain @ 0x4014BA via
// InitializeData @ 0x40395C (the MiscTools build)]. A node is made from its children's meshes in memory,
// freed once the node is made [orig: TrnGen.exe QuadtreeNode_Process @ 0x403470], so a region bake keeps,
// for every node, what its parent's merge reads of it, and for every leaf what the draw-back writes.

// The .cpt's tile count: the 1024 root down to the 64 leaves, 1 + 4 + 16 + 64 + 256.
inline constexpr int kCptTileCount = 341;
// The levels' first tile index (depth 0 to 4).
inline constexpr int kCptLevelOffset[5] = {0, 1, 5, 21, 85};

// A node's .cpt tile index: its level's first, then its row of the level's nodes, then its column (the
// order the export writes and the game reads [orig: Terrain_LoadLodStorage @ 0x603845..0x603C9A]). `x` and
// `y` are the node's corner in texels, `depth` 0 (the root) to 4 (the leaves).
int cpt_tile_index(int depth, int x, int y) noexcept;

// A node's LOD 14 section as its parent's merge reads it (LODMeshData_LoadSectionDedup): the section's
// vertex count, its faces, the vertex records from the first up to the furthest the merge reads (the
// section's count or a face's index past it), and the mesh's own vertex count, below which an index past
// the section is copied and above which it is zeroed.
struct BakeSection {
	int vertex_count = 0;
	int mesh_vertex_count = 0;
	std::vector<uint16_t> faces;   // three indices a face
	std::vector<float> records;    // ten floats a vertex
};

// A leaf's level-0 faces, in the order the draw-back writes them, and the positions they index (x, the
// height, z) [orig: TrnGen.exe sub_402A20 @ 0x402A20].
struct LeafDrawBack {
	std::vector<float> positions;  // three floats a vertex
	std::vector<uint16_t> faces;   // three indices a face
};

struct NodeBakeState {
	BakeSection section;
	LeafDrawBack draw_back;        // leaves only
};

// A bake's state: the base meshes and the lock flags it was made with, and every node's section (and a
// leaf's draw-back) by .cpt tile index. Its parts are immutable and shared: a copy of the state shares them, and a
// region bake's landing (apply_region_bake) replaces only the nodes it remade, so a history keeping a state per
// step (the editor's sculpting) keeps each node once.
struct TerrainBakeState {
	std::shared_ptr<const BaseMeshSet> base;
	std::vector<std::shared_ptr<const NodeBakeState>> nodes;

	bool empty() const noexcept {
		return !base || !base->generated || nodes.size() != static_cast<size_t>(kCptTileCount);
	}
};

// The bake (bake_terrain), keeping its state: `out` is the same .cpt bake_terrain makes.
bool bake_terrain_state(const TerrainBakeInput &input, CptFile &out, TerrainBakeState &state,
                        std::string &error);

// A node's part of the state from its mesh, while the mesh is alive (before its parent frees it).
void capture_node_state(const QuadtreeNode &node, NodeBakeState &out);

// A mesh holding just what the merge reads of a kept section: an untouched child's stand-in, which the
// merge reads as it read the child's own mesh. `mesh` is initialized here; free it with LODMeshData_Free.
void load_section_mesh(const BakeSection &section, LODMeshData &mesh);

} // namespace opennova::trngen
