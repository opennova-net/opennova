#include "terrain_bake_state.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace opennova::trngen {

namespace {

constexpr int kRecordFloats = 10; // a 40-byte vertex record
constexpr int kMergedLevel = 14;  // the section a parent merges [orig: TrnGen.exe QuadtreeNode_Process @ 0x403470]

} // namespace

int cpt_tile_index(int depth, int x, int y) noexcept {
	const int n = 1 << depth;
	const int stride = 1024 >> depth;
	return kCptLevelOffset[depth] + (y / stride) * n + x / stride;
}

void capture_node_state(const QuadtreeNode &node, NodeBakeState &out) {
	out = NodeBakeState{};
	const LODMeshData *mesh = node.mesh_data.get();
	if (!mesh || !mesh->vertex_data) return;
	const auto *records = reinterpret_cast<const float *>(mesh->vertex_data);

	// The section: what LODMeshData_LoadSectionDedup reads of it, the records up to the section's count
	// or a face's index past it, those below the mesh's count (an index at or past it reads as zeros).
	const LODLevel &level = mesh->levels[kMergedLevel];
	BakeSection &section = out.section;
	section.vertex_count = level.vertex_count;
	section.mesh_vertex_count = mesh->vertex_count;
	int reach = level.vertex_count;
	if (level.face_index_data && level.face_count > 0) {
		section.faces.assign(level.face_index_data, level.face_index_data + level.face_count * 3);
		for (uint16_t index : section.faces) reach = std::max(reach, static_cast<int>(index) + 1);
	}
	// (The section's own count never passes the mesh's: a reduction only removes vertices.)
	const int kept = std::max(0, std::min(std::max(level.vertex_count, std::min(reach, mesh->vertex_count)),
	                                      mesh->vertex_capacity));
	section.records.assign(records, records + static_cast<size_t>(kept) * kRecordFloats);

	// A leaf's draw-back: its level-0 faces and the positions they index.
	if (!node.is_leaf) return;
	const LODLevel &level0 = mesh->levels[0];
	if (!level0.face_index_data || level0.face_count <= 0) return;
	LeafDrawBack &draw = out.draw_back;
	draw.faces.assign(level0.face_index_data, level0.face_index_data + level0.face_count * 3);
	int used = 0;
	for (uint16_t index : draw.faces) used = std::max(used, static_cast<int>(index) + 1);
	draw.positions.resize(static_cast<size_t>(used) * 3u);
	for (int i = 0; i < used; ++i)
		std::memcpy(&draw.positions[static_cast<size_t>(i) * 3u], records + static_cast<size_t>(i) * kRecordFloats,
		            3 * sizeof(float));
}

void load_section_mesh(const BakeSection &section, LODMeshData &mesh) {
	LODMeshData_Init(&mesh);
	mesh.vertex_count = section.mesh_vertex_count;
	if (!section.records.empty()) {
		mesh.vertex_data = static_cast<uint8_t *>(std::malloc(section.records.size() * sizeof(float)));
		std::memcpy(mesh.vertex_data, section.records.data(), section.records.size() * sizeof(float));
		mesh.vertex_capacity = static_cast<int>(section.records.size() / kRecordFloats);
	}
	LODLevel &level = mesh.levels[kMergedLevel];
	level.vertex_count = section.vertex_count;
	level.face_count = static_cast<int>(section.faces.size() / 3u);
	if (!section.faces.empty()) {
		level.face_index_data = static_cast<uint16_t *>(std::malloc(section.faces.size() * sizeof(uint16_t)));
		std::memcpy(level.face_index_data, section.faces.data(), section.faces.size() * sizeof(uint16_t));
	}
}

} // namespace opennova::trngen
