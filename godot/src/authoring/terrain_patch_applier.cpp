#include "authoring/terrain_patch_applier.h"

#include "terrain/foliage_dispatcher.h"
#include "terrain/terrain.h"
#include "terrain/terrain_data.h"
#include "terrain/terrain_tile_info.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/time.hpp>

#include <algorithm>
#include <cstring>

namespace godot {

bool TerrainPatchApplier::apply(const std::vector<opennova::editor::HeightsDevicePatch> &patches, TerrainData &data,
		Terrain &terrain) {
	regions_ = 0;
	tiles_ = 0;
	std::vector<opennova::terrain::CoordsRect> rects;
	std::vector<int> replaced;
	uint64_t last = taken_;
	for (const opennova::editor::HeightsDevicePatch &patch : patches) {
		if (patch.serial <= taken_) continue;
		last = std::max(last, patch.serial);
		for (const opennova::editor::HeightsDevicePatch::Region &region : patch.regions) {
			const opennova::terrain::CoordsRect rect{region.x, region.y, region.w, region.h};
			if (region.heights.size() != size_t(region.w) * size_t(region.h) ||
					!data.patch_heights(rect, region.heights.data(), size_t(region.w))) {
				taken_ = last;
				failed_ = true;
				return false;
			}
			// Each region its own rect (a change across the atlas's wrap arrives as two): one refresh shows them all.
			rects.push_back(rect);
			++regions_;
		}
		if (!patch.tile_indices.empty()) {
			if (!data.replace_tiles(patch.tile_indices, patch.tiles)) {
				taken_ = last;
				failed_ = true;
				return false;
			}
			replaced.insert(replaced.end(), patch.tile_indices.begin(), patch.tile_indices.end());
			tiles_ += int(patch.tile_indices.size());
		}
	}
	taken_ = last;
	if (rects.empty() && replaced.empty()) return true;
	std::sort(replaced.begin(), replaced.end());
	replaced.erase(std::unique(replaced.begin(), replaced.end()), replaced.end());
	// The foliage grows again with a landing (its tiles: the stroke's end, an undo, a redo), not every sample.
	if (terrain.refresh_region(rects, replaced, !replaced.empty())) return true;
	failed_ = true;
	return false;
}

void TerrainPatchApplier::settle_colour(Terrain &terrain, uint64_t now_ms) {
	constexpr uint64_t kSettleMs = 250;
	if (!colour_due_ || now_ms < colour_at_ms_ + kSettleMs) return;
	colour_due_ = false;
	const Ref<TerrainData> data = terrain.get_terrain_data();
	if (data.is_valid()) data->refresh_colormap_texture();
}

bool TerrainPatchApplier::apply_maps(const std::vector<opennova::editor::MapsDevicePatch> &patches, Terrain &terrain,
		FoliageDispatcher *foliage, const std::function<void(const std::string &name, uint64_t stamp)> &took) {
	using Kind = opennova::editor::MapsDevicePatch::Kind;
	maps_applied_ = 0;
	for (const opennova::editor::MapsDevicePatch &patch : patches) {
		if (patch.serial <= maps_taken_) continue;
		maps_taken_ = patch.serial;
		bool done = true;
		if (patch.kind == Kind::Colour || patch.kind == Kind::Blend) {
			// The changed texels as a picture of their own, put in the live map at their place.
			PackedByteArray bytes;
			bytes.resize(int64_t(patch.rgba.size()));
			if (!patch.rgba.empty()) std::memcpy(bytes.ptrw(), patch.rgba.data(), patch.rgba.size());
			const Ref<Image> region = patch.w > 0 && patch.h > 0 &&
							patch.rgba.size() == size_t(patch.w) * size_t(patch.h) * 4
					? Image::create_from_data(patch.w, patch.h, false, Image::FORMAT_RGBA8, bytes)
					: Ref<Image>();
			done = region.is_valid() &&
					(patch.kind == Kind::Colour ? terrain.update_colormap_region(region, Vector2i(patch.x, patch.y))
												: terrain.update_blendmap_region(region, Vector2i(patch.x, patch.y)));
			if (done && patch.kind == Kind::Colour) {
				colour_due_ = true;
				colour_at_ms_ = Time::get_singleton()->get_ticks_msec();
			}
		} else if (patch.kind == Kind::Foliage) {
			// The codes into the foliage map the terrain's foliage reads, then the cells over the ground that reads them
			// grown again (every sector drawing the quadrant: the patch's world rects, the Godot plane's).
			const Ref<TerrainData> data = terrain.get_terrain_data();
			done = data.is_valid() && patch.indices.size() == size_t(patch.w) * size_t(patch.h) &&
					data->write_foliagemap_rect(patch.x, patch.y, patch.w, patch.h, patch.indices.data());
			if (done && foliage != nullptr)
				for (const opennova::terrain::TerrainWorldRect &rect : patch.world)
					foliage->invalidate_cells(Rect2i(rect.x0, rect.z0, rect.x1 - rect.x0, rect.z1 - rect.z0));
		} else if (patch.kind == Kind::Tiles) {
			// The override the terrain draws its tiles from, its entries replaced in place (its `changed`: the
			// terrain's pages and the foliage's blocker follow it); one made where it held none.
			Ref<TerrainTileInfo> tiles = terrain.get_tile_info_override();
			if (tiles.is_valid()) {
				// The same placement (a file opened or saved, no record moved) changes nothing drawn: no `changed`,
				// which would place the foliage again.
				const opennova::TilFile held = tiles->to_native();
				const std::vector<opennova::TilOverlayEntry> &was = held.entries;
				const std::vector<opennova::TilOverlayEntry> &now = patch.tiles.entries;
				const bool same = was.size() == now.size() &&
						std::equal(was.begin(), was.end(), now.begin(),
								[](const opennova::TilOverlayEntry &a, const opennova::TilOverlayEntry &b) {
									return a.x_fixed == b.x_fixed && a.z_fixed == b.z_fixed &&
											a.tile_index == b.tile_index && a.flags == b.flags && a.layer == b.layer;
								});
				if (!same) tiles->copy_from_native(patch.tiles);
			} else {
				tiles.instantiate();
				tiles->copy_from_native(patch.tiles);
				terrain.set_tile_info_override(tiles);
			}
		}
		if (!done) {
			// The file's read keeps its old stamp: the caller's next build reads it again.
			maps_refused_ = true;
			continue;
		}
		for (const opennova::FileStamp &file : patch.files) took(file.name, file.stamp);
		++maps_applied_;
	}
	return !maps_refused_;
}

} // namespace godot
