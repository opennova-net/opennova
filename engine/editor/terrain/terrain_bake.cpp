#include "terrain_bake.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <thread>
#include <utility>

#include "mesh_simp.h"
#include "terrain_mesh.h"
#include "tristrip.h"

namespace opennova::editor::trngen {

namespace {

constexpr int kTileSize = 1024;
constexpr int kMinTileSize = 64;
// The tile files' name prefix: TrnGen writes `<output dir>/<output>S<depth>_<xx>_<yy>.tml`; the
// files here live in a map of their own, so the prefix names nothing but the files.
constexpr const char *kPrefix = "";

int count_depth_levels(int tile_size, int min_tile_size) {
	int levels = 0;
	int current_size = tile_size;
	while (current_size >= min_tile_size) {
		++levels;
		current_size >>= 1;
	}
	return levels;
}

void set_header_text(char *buffer, size_t size, const std::string &value) {
	std::memset(buffer, 0, size);
	std::memcpy(buffer, value.data(), std::min(value.size(), size - 1));
}

const std::vector<uint8_t> *find_file(const TileFiles &files, const std::string &name) {
	const auto at = files.find(name);
	return at == files.end() ? nullptr : &at->second;
}

CptTile build_tile_from_mesh(const MeshData &mesh, int current_size, bool is_single_tile) {
	CptTile tile;
	tile.tile_x = mesh.tile_x;
	tile.tile_y = mesh.tile_y;
	tile.vertex_count = static_cast<uint16_t>(mesh.vertex_data.size());
	tile.tile_size = static_cast<uint16_t>(current_size);
	tile.is_single = is_single_tile;
	tile.vertex_indices.resize(mesh.vertex_data.size() * 2u);

	for (size_t i = 0; i < mesh.vertex_data.size(); ++i) {
		const uint32_t packed = mesh.vertex_data[i];
		tile.vertex_indices[i * 2u + 0u] = static_cast<uint16_t>(packed & 0xFFFFu);
		tile.vertex_indices[i * 2u + 1u] = static_cast<uint16_t>(packed >> 16);
	}

	for (int lod = 0; lod < 8; ++lod) {
		const auto &entry = mesh.entries[lod * 2];
		auto &target = tile.lods[lod];
		target.indices = entry.index_data;
		target.max_index = entry.max_index;
		target.is_strip = entry.is_strip();
	}

	return tile;
}

// The .tms pass over the .tml files: each node's mesh read back, its faces made strips and its
// vertices put in the strips' order, written as its .tms; the root's .tms is an empty mesh (the
// export never reads it: below). [orig: TrnGen.exe build_terrain_thread @ 0x4013A0, the
// multi-resolution tile loop; sub_404480 @ 0x404480 via thunk sub_404610]
void write_tms_tiles(TileFiles &files) {
	int sz = kTileSize;
	int depth = 0;
	while (sz >= kMinTileSize) {
		bool level_done = false;
		for (int y = 0; y < kTileSize && !level_done; y += sz) {
			for (int x = 0; x < kTileSize && !level_done; x += sz) {
				char tml_name[64];
				char tms_name[64];
				std::snprintf(tml_name, sizeof(tml_name), "%sS%d.tml", kPrefix, depth);
				std::snprintf(tms_name, sizeof(tms_name), "%sS%d.tms", kPrefix, depth);
				bool is_single = true;
				if (!find_file(files, tml_name)) {
					is_single = false;
					std::snprintf(tml_name, sizeof(tml_name), "%sS%d_%02x_%02x.tml", kPrefix, depth, x >> 4, y >> 4);
					std::snprintf(tms_name, sizeof(tms_name), "%sS%d_%02x_%02x.tms", kPrefix, depth, x >> 4, y >> 4);
				}
				const std::vector<uint8_t> *tml = find_file(files, tml_name);
				if (!tml) continue;

				// A tile that does not read back is skipped, its .tms not written.
				MeshData mesh;
				bool readable = true;
				if (depth != 0) {
					readable = MeshData::read_bytes(*tml, mesh);
					if (readable) remap_vertex_ordering(mesh);
				}
				if (readable) files[tms_name] = mesh.write_bytes();
				if (is_single) level_done = true;
			}
		}
		sz >>= 1;
		depth++;
	}
}

// The CPT export over the .tms files: a tile per node, level by level, the rows of a level outer
// and its columns inner (the order the game reads them in [orig: Terrain_LoadLodStorage @
// 0x603845..0x603C9A]); a level of one `S<depth>` file is that one tile. The root level reads the
// deepest level's files (TrnGen's `file_depth`): its one tile is the first leaf's mesh, sized as
// the root; the game never draws the root, its sectors being the 512 quadrants below it. The
// header's +4 dword is the level count over the root's size; the game reads its high word, the
// level count [orig: Terrain_LoadLodStorage @ 0x60380E..0x60381C].
// [orig: TrnGen.exe build_terrain_thread @ 0x4013A0, its CPT export]
void export_terrain_cpt(const TileFiles &files, const TerrainBakeInput &input,
                        std::vector<uint16_t> depth_buffer, CptFile &cpt) {
	cpt = CptFile{};
	cpt.depth_format = input.depth_format;
	set_header_text(cpt.header.terrain_name, sizeof(cpt.header.terrain_name), input.terrain_name);
	set_header_text(cpt.header.creator, sizeof(cpt.header.creator), input.creator);
	set_header_text(cpt.header.version, sizeof(cpt.header.version), input.version);
	cpt.depth_buffer = std::move(depth_buffer);

	const int depth_count = count_depth_levels(kTileSize, kMinTileSize);
	int current_size = kTileSize;
	int depth_level = 0;
	while (current_size >= kMinTileSize) {
		bool level_done = false;
		for (int y = 0; y < kTileSize && !level_done; y += current_size) {
			for (int x = 0; x < kTileSize && !level_done; x += current_size) {
				const int file_depth = depth_level == 0 ? depth_count - 1 : depth_level;
				char filename[64];
				bool is_single_tile = false;
				MeshData mesh;
				bool loaded = false;
				std::snprintf(filename, sizeof(filename), "%sS%d.tms", kPrefix, file_depth);
				if (const std::vector<uint8_t> *bytes = find_file(files, filename)) {
					loaded = MeshData::read_bytes(*bytes, mesh);
					is_single_tile = loaded;
				}
				if (!loaded) {
					std::snprintf(filename, sizeof(filename), "%sS%d_%02x_%02x.tms", kPrefix, file_depth, x >> 4,
					              y >> 4);
					if (const std::vector<uint8_t> *bytes = find_file(files, filename))
						loaded = MeshData::read_bytes(*bytes, mesh);
				}
				if (!loaded) continue;
				cpt.tiles.push_back(build_tile_from_mesh(mesh, current_size, is_single_tile));
				if (is_single_tile) level_done = true;
			}
		}
		current_size >>= 1;
		++depth_level;
	}

	const uint32_t header_patch =
		(static_cast<uint32_t>(depth_level) << 16u) | static_cast<uint16_t>(kTileSize);
	cpt.header.reserved[0] = static_cast<uint8_t>(header_patch);
	cpt.header.reserved[1] = static_cast<uint8_t>(header_patch >> 8);
	cpt.header.reserved[2] = static_cast<uint8_t>(header_patch >> 16);
	cpt.header.reserved[3] = static_cast<uint8_t>(header_patch >> 24);
}

} // namespace

// [orig: TrnGen.exe build_terrain_thread @ 0x4013A0]
// Each output pixel = 32 * the sum of its 2x2 block (itself, right, below, below-right), wrapping
// at 1024.
std::vector<uint16_t> smooth_depthmap(const std::vector<uint8_t> &raw) {
	std::vector<uint16_t> out(static_cast<size_t>(kDepthSide) * kDepthSide);
	for (int row = 0; row < kDepthSide; row++) {
		const int row_next = (row + 1) & 0x3FF;
		const int row_cur = row & 0x3FF;
		for (int col = 0; col < kDepthSide; col++) {
			const int col_next = (col + 1) & 0x3FF;
			const int col_cur = col & 0x3FF;
			out[static_cast<size_t>(row) * kDepthSide + col] = static_cast<uint16_t>(
				32 * (raw[static_cast<size_t>(row_next) * kDepthSide + col_next] +
				      raw[static_cast<size_t>(row_cur) * kDepthSide + col_next] +
				      raw[static_cast<size_t>(row_next) * kDepthSide + col_cur] +
				      raw[static_cast<size_t>(row_cur) * kDepthSide + col_cur]));
		}
	}
	return out;
}

// [orig: TrnGen.exe build_terrain_thread @ 0x4013A0]
// Pipeline: depth map (smoothed) -> base meshes -> quadtree (meshes, LODs, .tml files, the leaves
// rasterized) -> .tms files -> CPT export. TrnGen also exports a CPT from the .tml files before the
// .tms pass, which the .tms export then overwrites; that first export is not made here.
bool bake_terrain(const TerrainBakeInput &input, CptFile &out, std::string &error) {
	const size_t texels = static_cast<size_t>(kDepthSide) * kDepthSide;
	const bool has8 = !input.depth8.empty();
	const bool has16 = !input.depth16.empty();
	if (has8 == has16) {
		error = "the terrain bake takes one depth map, 8-bit or 16-bit";
		return false;
	}
	if ((has8 && input.depth8.size() != texels) || (has16 && input.depth16.size() != texels)) {
		error = "the depth map is not 1024 x 1024";
		return false;
	}
	try {
		std::vector<uint16_t> smoothed = has8 ? smooth_depthmap(input.depth8) : input.depth16;

		BaseMeshSet base;
		base.locks = input.locks;
		generate_base_terrain_meshes(base);

		TileFiles files;
		std::vector<uint16_t> rasterized_depth = smoothed;
		{
			std::unique_ptr<QuadtreeNode> root = build_quadtree(kTileSize, kMinTileSize);
			QuadtreeContext ctx;
			ctx.tile_size = kTileSize;
			ctx.min_tile_size = kMinTileSize;
			ctx.depth_buffer = smoothed.data();
			ctx.rasterized_depth = &rasterized_depth;
			ctx.output_prefix = kPrefix;
			ctx.base = &base;
			ctx.files = &files;
			unsigned threads = input.threads;
			if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
			process_quadtree(root.get(), ctx, threads);
		}
		MeshSimp_FreeAll();

		write_tms_tiles(files);
		export_terrain_cpt(files, input, input.rasterize_depth ? std::move(rasterized_depth) : std::move(smoothed),
		                   out);
	} catch (const std::exception &e) {
		MeshSimp_FreeAll();
		error = std::string("the terrain bake failed: ") + e.what();
		return false;
	}
	return true;
}

bool bake_terrain_bytes(const TerrainBakeInput &input, std::vector<uint8_t> &out, std::string &error) {
	CptFile cpt;
	if (!bake_terrain(input, cpt, error)) return false;
	try {
		out = cpt.write_bytes();
	} catch (const std::exception &e) {
		error = std::string("the terrain's .cpt cannot be written: ") + e.what();
		return false;
	}
	return true;
}

} // namespace opennova::editor::trngen
