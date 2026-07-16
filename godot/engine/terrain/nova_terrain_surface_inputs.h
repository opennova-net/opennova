#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace godot {

class NovaTerrainData;
class NovaTerrainTileInfo;

// Owns the allocation-heavy, retail-faithful textures consumed by both the
// runtime and ONED terrain shaders. Callers refresh only the input family that
// changed at an editor transaction boundary, then apply the cached inputs to
// any compatible ShaderMaterial.
class NovaTerrainSurfaceInputs : public RefCounted {
	GDCLASS(NovaTerrainSurfaceInputs, RefCounted)

private:
	Ref<NovaTerrainData> terrain_data;
	Ref<NovaTerrainTileInfo> tile_info_override;
	bool tile_overlay_enabled = true;

	Ref<Texture2D> detail_coefficient_texture;
	Ref<Texture2D> normalized_blend_texture;
	Ref<Texture2D> paired_detail_textures[3];
	Ref<Texture2D> paired_detail2_texture;
	Ref<Texture2D> mipped_colormap_texture;
	Ref<Texture2D> heightfield_normal_texture;
	Ref<Texture2D> tile_overlay_texture;

protected:
	static void _bind_methods();

public:
	void set_terrain_data(const Ref<NovaTerrainData> &p_data);
	Ref<NovaTerrainData> get_terrain_data() const;

	void set_tile_info_override(const Ref<NovaTerrainTileInfo> &p_info);
	Ref<NovaTerrainTileInfo> get_tile_info_override() const;

	void set_tile_overlay_enabled(bool p_enabled);
	bool get_tile_overlay_enabled() const;

	bool rebuild(const Ref<NovaTerrainData> &p_data,
			const Ref<NovaTerrainTileInfo> &p_tile_info = Ref<NovaTerrainTileInfo>(),
			bool p_tile_overlay_enabled = true);
	bool rebuild_blend();
	bool rebuild_heightfield();
	bool rebuild_detail_textures();
	bool rebuild_tile_overlay();

	void clear_derived_textures();
	void clear_tile_overlay();
	bool apply_to_material(const Ref<ShaderMaterial> &p_material) const;

	Ref<Texture2D> get_colormap_texture() const;
	Ref<Texture2D> get_detailmap_texture() const;
	Ref<Texture2D> get_blend_texture() const;
	Ref<Texture2D> get_detail_c1_texture() const;
	Ref<Texture2D> get_detail_c2_texture() const;
	Ref<Texture2D> get_detail_c3_texture() const;
	Ref<Texture2D> get_normalized_blend_texture() const;
	Ref<Texture2D> get_detail_coefficient_texture() const;
	Ref<Texture2D> get_paired_detail_texture(int p_layer) const;
	Ref<Texture2D> get_detail2_texture() const;
	Ref<Texture2D> get_heightfield_normal_texture() const;
	Ref<Texture2D> get_tile_overlay_texture() const;
	int get_detail_density() const;
	int get_detail2_density() const;

	bool has_normalized_blend() const;
	bool has_detail_coefficient() const;
	bool has_paired_detail(int p_layer) const;
	bool has_detail2() const;
	bool has_heightfield_normal() const;
	bool has_tile_overlay() const;
	Dictionary get_diagnostics() const;
};

} // namespace godot
