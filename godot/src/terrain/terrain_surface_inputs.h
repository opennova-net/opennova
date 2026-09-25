#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

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

	Ref<Texture2D> detail_coefficient_texture;
	Ref<Texture2D> normalized_blend_texture;
	Ref<Texture2D> detail_layer_textures[3];
	Ref<Texture2D> paired_detail2_texture;
	Ref<Texture2D> heightfield_normal_texture;

protected:
	static void _bind_methods();

public:
	void set_terrain_data(const Ref<TerrainData> &p_data);
	Ref<TerrainData> get_terrain_data() const;

	void set_tile_info_override(const Ref<TerrainTileInfo> &p_info);
	Ref<TerrainTileInfo> get_tile_info_override() const;

	bool rebuild(const Ref<TerrainData> &p_data,
			const Ref<TerrainTileInfo> &p_tile_info = Ref<TerrainTileInfo>());
	bool rebuild_blend();
	bool rebuild_heightfield();
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
