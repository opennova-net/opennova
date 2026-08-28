#pragma once

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <vector>

#include <godot_cpp/variant/color.hpp>

#include <runtime/renderer/light_scene.h>
#include <runtime/renderer/light_terrain_pass.h>
#include <runtime/renderer/render_slot_shadow.h>

namespace godot {

class Weather;

// Godot adapter for the portable EffectWorld dynamic light pool
// (opennova::renderer::LightScene — engine/runtime/renderer/light_scene.h carries the
// witness map). Marshalling only: Godot world <-> mission 16.16 conversion,
// the per-frame select, and the RenderingServer global-parameter push the
// technique shaders consume. Owns no Nodes and renders nothing itself.
class LightScene : public RefCounted {
	GDCLASS(LightScene, RefCounted)

public:
	// Spawn/mutation handles cross the Variant boundary as opaque positive
	// int64 leases: retail's slot word in bits 0..15, generation in bits 16..47.
	// config keys: position (Vector3 Godot world), atten_end (float world
	// units), color_start / color_end (Color), style / phase / rate (int),
	// intensity (float), disable_corona / disable_terrain / disable_objects
	// (bool), plus the owner-attach facts opennova::renderer::resolve_model_light_owner
	// decides from: attach_bone (int, the record's authored subobject),
	// spawning_entity (int), spawner_is_building (bool), and
	// blink_owner_entity / blink_section (int) for the blink box the spawning
	// entity stands in.
	int64_t spawn_model_light(const Dictionary &p_config);
	// Transient glow spawn (muzzle / impact / death / round legs — the
	// light_scene.h witness map). config keys: position (Vector3), radius
	// (float world units), color (Color), fade_mode / fade_duration (int),
	// owner_entity / owner_section (int), disable_corona / disable_terrain /
	// disable_objects (bool).
	int64_t spawn_glow(const Dictionary &p_config);
	void despawn(int64_t p_handle);
	void set_light_position(int64_t p_handle, const Vector3 &p_world);
	// The witnessed instance re-arm setters (fade mode/duration, owner group,
	// position, blend). Entity_UpdateMuzzleGlowEffect applies all four to the
	// one entity+0x1B4 lease; the witness lives with the engine pool in
	// engine/runtime/renderer/light_scene.h (muzzle glow section).
	void set_light_fade(int64_t p_handle, int p_mode, int p_duration);
	void set_light_owner(int64_t p_handle, int64_t p_owner_entity,
			int p_owner_section);
	void set_light_blend(int64_t p_handle, float p_amount);
	bool is_alive(int64_t p_handle) const;
	void clear();
	// Retire the currently published shader selection without destroying the
	// live mission pool (for camera loss and world-transition frames).
	void clear_render_output();

	// The 62 Hz lifecycle decay — call once per fixed tick.
	void advance_fixed_tick();

	// Report/debug leg: query the pool around a camera point and select the
	// witnessed <= 4 into the report rows. The gameplay object pass is
	// render_model_frame below; this camera-global path publishes nothing.
	int render_frame(const Vector3 &p_camera_world, float p_query_radius,
			const Vector3 &p_ambient_scale, int p_time_ms, Weather *p_weather);

	// The per-draw gameplay pass (retail: update_light_slots @0x5abc50 per
	// draw context, see docs/render/render-lighting-re.md): one draw context
	// per visible ObjectModel, owner group = that model's entity id, interior
	// group = the building it currently stands inside + that blink volume's
	// section. A nonzero p_robj_scoped row expands a building into one context
	// per visible ROBJ: the building becomes its own interior group at section
	// zero and owner_group_section names the current ROBJ, exactly matching the
	// retail re-scope @0x5d8ff7. All arrays are parallel. Returns the number of
	// models (not expanded draw contexts) that received at least one light.
	int render_model_frame(const TypedArray<Node3D> &p_models,
			const PackedInt64Array &p_owner_entities,
			const PackedInt64Array &p_interior_owners,
			const PackedInt32Array &p_interior_sections,
			const PackedByteArray &p_robj_scoped,
			const Vector3 &p_ambient_scale, int p_time_ms, Weather *p_weather);

	// The same witnessed per-draw selection for static MultiMesh rows. Bounds
	// are position/size pairs in atlas-row order; the remaining arrays are
	// parallel to rows. Inactive (destroyed/carved) rows stay zero so their
	// stable INSTANCE_CUSTOM.x identity never has to move. The RGBAF atlas is
	// published as opennova_static_point_light_rows: count in texel 0.x, then
	// four (world position.xyz, attenuation2)/(color.rgb, range) pairs.
	// Returns the number of active rows that received at least one light.
	int render_static_frame(
			const PackedVector3Array &p_bounds_position_size,
			const PackedInt64Array &p_owner_entities,
			const PackedInt32Array &p_owner_sections,
			const PackedInt64Array &p_interior_owners,
			const PackedInt32Array &p_interior_sections,
			const PackedByteArray &p_active,
			const Vector3 &p_ambient_scale, int p_time_ms, Weather *p_weather,
			int64_t p_rows_revision = -1);

	// The procedural corona texture "texlightcrn" as RGBA8 bytes,
	// corona_texture_size() square — opennova::renderer::corona_texture_argb carries
	// the law; this only unpacks the words for Image::create_from_data.
	static int corona_texture_size();
	static PackedByteArray corona_texture_rgba8();

	// The render-slot dominant-light query (SlotShadow's per-slot pick):
	// entity-centered collect + group-gated params, no D3D-fill boost
	// (retail: RenderSlot_UpdateEntityLight @0x5d6a30, see
	// docs/render/render-lighting-re.md). A C++ seam (not script-bound):
	// fills r_out with the planner's typed inputs (positions in Godot world).
	void slot_shadow_lights(const Vector3 &p_world_pos, float p_radius,
			const Vector3 &p_ambient_scale, int p_time_ms, Weather *p_weather,
			std::vector<opennova::renderer::SlotPointLight> &r_out);

	// The corona billboard rows for this frame (retail:
	// EffectWorld_RenderLightCoronas @0x5aaf40 — witness comment on
	// opennova::renderer::LightScene::collect_corona_quads): one Dictionary per
	// additive camera-facing quad, keys position (Vector3 Godot world),
	// half_size (float world units), color (Color, premultiplied additive
	// including the segment fade and the fog-to-black fold). models/
	// owner_entities are the SAME parallel arrays the per-model light pass
	// walks — models carrying an occlusion section-mask verdict gate their
	// owned coronas on the visible-section bit; fog is
	// {enabled, type, start, end} (primary device fog, color forced black
	// (retail: CD3DDevice_SetFogAndBlendMode(dev, 2) @0x5aafb6)). The
	// presenter (effect_light_director.gd) feeds the rows into a MultiMesh;
	// marshalling only.
	TypedArray<Dictionary> collect_corona_rows(const Vector3 &p_camera_pos,
			const Vector3 &p_camera_forward, const Vector3 &p_ambient_scale,
			int p_time_ms, int p_frame_index, Weather *p_weather,
			const TypedArray<Node3D> &p_models,
			const PackedInt64Array &p_owner_entities,
			const Dictionary &p_fog);

	// The terrain leg of the pool: per terrain patch, the <= 16 world lights
	// whose AABB overlaps the patch and which the authored terrain flag admits,
	// as the rows the terrain shader re-draws the patch with (retail: the
	// per-light else-arm of render_terrain_sector_batch @0x6092A0 ->
	// Light_SetupTerrainProjectedPass @0x5AA830 — the collect, the gates and
	// the pixel constants live portable in opennova::renderer::LightScene::
	// collect_terrain_pass_rows, see docs/render/render-lighting-re.md). A C++
	// seam for the Terrain device: patch bounds in mission 16.16 (the helper
	// opennova::renderer::terrain_patch_light_bounds folds the render frame), the env
	// light-state gain, the time + weather the flicker reads, and the packed
	// Env_TerrainColorRecip the recip factor unpacks. Returns the row total.
	size_t collect_terrain_light_rows(
			const opennova::renderer::TerrainLightPatchBounds *p_patches,
			size_t p_patch_count, const Vector3 &p_ambient_scale, int p_time_ms,
			Weather *p_weather, uint32_t p_recip_packed,
			opennova::renderer::TerrainLightPatchRows *r_rows) const;
	// The same leg over Godot-world AABBs (one per patch), rows as
	// Dictionaries {position (Vector3 Godot world), inv_scale, color (Vector3,
	// the c4..c6 pixel constants), handle} — the GUT seam the terrain device
	// test drives without a built Terrain.
	Array collect_terrain_light_rows_for_bounds(
			const TypedArray<AABB> &p_world_aabbs, const Vector3 &p_ambient_scale,
			int p_time_ms, Weather *p_weather, int p_recip_packed) const;

	// The two procedural textures the terrain pass samples, as RGBA8 bytes:
	// "texlight2d" (terrain_light_texture_size() square, the disc on the ground
	// plane) and "texlightspot1d" (terrain_light_texture_size() x
	// terrain_light_strip_rows(), the height strip). The laws live portable in
	// opennova::renderer::falloff_texture_light2d_argb / _spot1d_argb; this only unpacks
	// the words for Image::create_from_data.
	static int terrain_light_texture_size();
	static int terrain_light_strip_rows();
	static PackedByteArray terrain_light_disc_rgba8();
	static PackedByteArray terrain_light_strip_rgba8();

	Dictionary get_report() const;
	// Read-only diagnostics snapshot of the last uploaded RGBAF atlas. The
	// returned Image owns copied bytes; tests/tools cannot mutate render state.
	Ref<Image> get_static_light_rows_image() const;

protected:
public:
	// The witnessed muzzle-glow / death-flash spawn constants, re-exported
	// from opennova::renderer::LightScene for the presenting director.
	static float muzzle_glow_radius();
	static Color muzzle_glow_color();
	static int muzzle_glow_fade_mode();
	static int muzzle_glow_fade_ticks();
	static Color death_flash_color();
	static int death_flash_fade_mode();
	static int death_flash_fade_ticks();

protected:
	static void _bind_methods();

private:
	static constexpr int STATIC_LIGHT_ROW_TEXELS = 9;
	struct StaticCachedSelection {
		int atlas_row = 0;
		opennova::renderer::LightActiveGroups groups{};
		std::array<opennova::renderer::LightHandle,
				opennova::renderer::LightScene::kSelectLimit> handles{};
		size_t count = 0;
	};
	opennova::renderer::LightScene scene_;
	std::array<opennova::renderer::SelectedLight, opennova::renderer::LightScene::kSelectLimit>
			selected_{};
	size_t selected_count_ = 0;
	String selection_mode_ = "none";
	String owner_isolation_ = "none";
	int last_models_ = 0;
	int last_lit_models_ = 0;
	Ref<Image> static_light_rows_image_;
	Ref<ImageTexture> static_light_rows_texture_;
	PackedByteArray static_light_rows_bytes_;
	PackedByteArray static_light_rows_scratch_;
	std::vector<StaticCachedSelection> static_cached_selections_;
	uint64_t static_cached_scene_revision_ = 0;
	int64_t static_cached_rows_revision_ = -1;
	int static_cached_row_count_ = -1;
	int static_cached_active_draws_ = 0;
	int static_row_count_ = 0;
	int last_static_draws_ = 0;
	int last_lit_static_draws_ = 0;
};

} // namespace godot
