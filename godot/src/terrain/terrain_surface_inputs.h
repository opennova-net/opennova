#pragma once

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rect2i.hpp>

#include <runtime/terrain/texture_preprocess.h>

#include <runtime/terrain_query/coords.h>

#include <array>

namespace godot {

class TerrainData;
class TerrainTileInfo;

// Owns the allocation-heavy, retail-faithful textures consumed by runtime
// terrain shaders. Callers rebuild them when terrain inputs change, then apply
// the cached inputs to any compatible ShaderMaterial.
class TerrainSurfaceInputs : public RefCounted {
	GDCLASS(TerrainSurfaceInputs, RefCounted)

private:
	Ref<TerrainData> terrain_data;
	Ref<TerrainTileInfo> tile_info_override;
	// The session's texcompression_level (the fresh profile's until handed one).
	int texcompression_level = 1;

	Ref<Texture2D> detail_coefficient_texture;
	Ref<Texture2D> normalized_blend_texture;
	// The normalized blend map the texture was last made from, its level 0 (what a painted region updates).
	opennova::terrain::Rgba8Image normalized_blend;
	Ref<Texture2D> detail_layer_textures[3];
	Ref<Texture2D> paired_detail2_texture;
	Ref<Texture2D> heightfield_normal_texture;
	// The heightfield normal atlas the texture was made from, kept so a region of it is made again
	// (refresh_heightfield_region) and the tile cache's quadrants are cut from it without a read-back.
	opennova::terrain::Rgba8Image heightfield_normal_image;

protected:
	static void _bind_methods();

public:
	void set_terrain_data(const Ref<TerrainData> &p_data);
	Ref<TerrainData> get_terrain_data() const;

	void set_tile_info_override(const Ref<TerrainTileInfo> &p_info);
	Ref<TerrainTileInfo> get_tile_info_override() const;

	// The session's game.cfg texcompression_level: the detail layers' DXT
	// request (rebuild_detail_textures) and the colormap quadrants' the tile
	// cache builds (renderer/texture_compression.h, D-RMAT-24).
	void set_texcompression_level(int p_level) { texcompression_level = p_level; }
	int get_texcompression_level() const { return texcompression_level; }

	bool rebuild(const Ref<TerrainData> &p_data,
			const Ref<TerrainTileInfo> &p_tile_info = Ref<TerrainTileInfo>());
	bool rebuild_blend();
	// The detail blend map painted (ADR 0046 S25): `p_rect` of `p_map` (the whole map as it now stands)
	// normalized as a load normalizes it (normalize_detail_blend_map, a texel at a time) into the normalized map
	// the blend texture was made from, and the texture updated from it in place (its levels generated as a build
	// generates them; the material keeps sampling the same texture). A map of another size than the texture's is
	// normalized whole into a new one. False where the map does not convert.
	bool update_blendmap_region(const Ref<Image> &p_map, const Rect2i &p_rect);
	bool rebuild_heightfield();
	// The heightfield normals over the heights `p_rects` changed (the loaded .cpt's, patched in place: an
	// editor's sculpt) made again (opennova::terrain::refresh_heightfield_normal_map) and the texture updated
	// from them in place (its RID kept: the materials sampling it keep it); `r_written` the boxes of the normals
	// written, each inside one quadrant. False where no heightfield was built from the .cpt's heights (a live
	// heightmap image, none at all): the caller rebuilds.
	bool refresh_heightfield_region(const std::vector<opennova::terrain::CoordsRect> &p_rects,
			std::vector<opennova::terrain::CoordsRect> &r_written);
	const opennova::terrain::Rgba8Image &get_heightfield_normal_image() const { return heightfield_normal_image; }
	bool rebuild_detail_textures();

	void clear_derived_textures();
	bool apply_to_material(const Ref<ShaderMaterial> &p_material) const;

	Ref<Texture2D> get_colormap_texture() const;
	Ref<Texture2D> get_detailmap_texture() const;
	Ref<Texture2D> get_blend_texture() const;
	Ref<Texture2D> get_detail_c1_texture() const;
	Ref<Texture2D> get_detail_c2_texture() const;
	Ref<Texture2D> get_detail_c3_texture() const;
	Ref<Texture2D> get_normalized_blend_texture() const;
	// The normalized blend map the texture was last made or updated from, its level 0 (a read seam: a headless
	// run's texture reads back as it was first made); null for none.
	Ref<Image> get_normalized_blend_image() const;
	Ref<Texture2D> get_detail_coefficient_texture() const;
	Ref<Texture2D> get_detail_layer_texture(int p_layer) const;
	Ref<Texture2D> get_detail2_texture() const;
	Ref<Texture2D> get_heightfield_normal_texture() const;
	int get_detail_density() const;
	int get_detail2_density() const;

	bool has_normalized_blend() const;
	bool has_detail_coefficient() const;
	bool has_detail_layer(int p_layer) const;
	bool has_detail2() const;
	bool has_heightfield_normal() const;
	Dictionary get_diagnostics() const;
};

} // namespace godot
