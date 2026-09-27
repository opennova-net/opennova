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
#include <godot_cpp/variant/packed_float32_array.hpp>
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

#include "lights/effect_light_report.h" // EffectLightReport (get_report)
#include "lights/light_spawn.h"

namespace godot {

class EffectLightReport;

class EnvLightValues;
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
	// The LGHT-record light (lights/light_spawn.h ModelLightSpawn): the
	// owner-attach facts on the record are what
	// opennova::renderer::resolve_model_light_owner decides from. 0 for a
	// null request.
	int64_t spawn_model_light(const Ref<ModelLightSpawn> &p_config);
	// Transient glow spawn (muzzle / impact / death / round legs — the
	// light_scene.h witness map; lights/light_spawn.h GlowSpawn). 0 for a
	// null request.
	int64_t spawn_glow(const Ref<GlowSpawn> &p_config);
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
	// witnessed <= 3 into the report rows. The gameplay object pass is
	// render_model_frame below; this camera-global path publishes nothing.
	int render_frame(const Vector3 &p_camera_world, float p_query_radius,
			const Vector3 &p_ambient_scale, int p_time_ms, Weather *p_weather);
	// The same census select for an on-demand report refresh; leaves the
	// report's selection_mode / owner_isolation as the gameplay pass set them.
	int census_frame(const Vector3 &p_camera_world, float p_query_radius,
			const Vector3 &p_ambient_scale, int p_time_ms, Weather *p_weather);

	// The per-draw gameplay pass [orig: Light_SelectAndEnableForDraw @0x5ab9d0
	// and the collectors' group gate @0x5d91f8 per draw context, see
	// docs/render/render-lighting-re.md]: one draw context
	// per visible ObjectModel. The owner group is what that submit declares
	// (renderer::submit_owner_group): p_owner_entities names the drawn entity,
	// which only a person's skinned draws keep; every other draw declares
	// entity 0 with its rigid ROBJ section, splitting per visible ROBJ only
	// where that section can matter (an interior group at section zero). The
	// interior group = the building it currently stands inside + that blink
	// volume's section. A nonzero p_robj_scoped row expands a building into one context
	// per visible ROBJ: the building becomes its own interior group at section
	// zero and owner_group_section names the current ROBJ, exactly matching the
	// retail re-scope @0x5d8ff7. All arrays are parallel. Returns the number of
	// models (not expanded draw contexts) that received at least one light.
	// Optional query arrays override the source for camera-relative FP parts;
	// nested head/husk models share their root entity query and groups.
	int render_model_frame(const TypedArray<Node3D> &p_models,
			const PackedInt64Array &p_owner_entities,
			const PackedInt64Array &p_interior_owners,
			const PackedInt32Array &p_interior_sections,
			const PackedByteArray &p_robj_scoped,
			const Vector3 &p_ambient_scale, int p_time_ms, Weather *p_weather,
			const PackedVector3Array &p_entity_positions = PackedVector3Array(),
			const PackedInt32Array &p_entity_bound_radii_q16 = PackedInt32Array());
	// The weapon Inset pass's draws (renderer/scene_overlay.h
	// kInsetOverlayOrder: retail runs the scene core again for that view, and
	// each draw selects its lights there): the same selection over the same
	// parallel arrays for the models the Inset draws through their own twins
	// (ObjectModel's view twins), a building's rows over the ROBJs the twins
	// draw, written to the twins. Models drawn the same in both views keep
	// the main pass's selection on their node. The report stays the main
	// pass's. Returns the models that received at least one light.
	int render_inset_model_frame(const TypedArray<Node3D> &p_models,
			const PackedInt64Array &p_owner_entities,
			const PackedInt64Array &p_interior_owners,
			const PackedInt32Array &p_interior_sections,
			const PackedByteArray &p_robj_scoped,
			const Vector3 &p_ambient_scale, int p_time_ms, Weather *p_weather,
			const PackedVector3Array &p_entity_positions,
			const PackedInt32Array &p_entity_bound_radii_q16);

	// The same entity-cube selection for static MultiMesh rows. Each row carries
	// its ENTITY origin and initialized Q16 bound radius, shared by every ROBJ
	// of that entity. All arrays are parallel. Inactive/carved rows stay zero so their
	// stable INSTANCE_CUSTOM.x identity never has to move. The RGBAF atlas is
	// published as opennova_static_point_light_rows: count in texel 0.x, then
	// four (world position.xyz, attenuation2)/(color.rgb, range) pairs, then
	// in the last texel the row's per-entry lighting state (effectScale,
	// interior lerp flag, daylight t, 0 -- the u_entity_light lane a MultiMesh
	// instance cannot carry; renderer::static_row_entity_lighting), defaulting
	// to the outdoor (1, 0, 0, 0) for a row p_entity_lights does not cover.
	// Returns the number of active rows that received at least one light.
	int render_static_frame(
			const PackedVector3Array &p_entity_positions,
			const PackedInt32Array &p_entity_bound_radii_q16,
			const PackedInt64Array &p_owner_entities,
			const PackedInt32Array &p_owner_sections,
			const PackedInt64Array &p_interior_owners,
			const PackedInt32Array &p_interior_sections,
			const PackedByteArray &p_active,
			const Vector3 &p_ambient_scale, int p_time_ms, Weather *p_weather,
			int64_t p_rows_revision = -1,
			const PackedVector4Array &p_entity_lights = PackedVector4Array());

	// The procedural corona texture "texlightcrn" as RGBA8 bytes,
	// corona_texture_size() square — opennova::renderer::corona_texture_argb carries
	// the law; this only unpacks the words for Image::create_from_data.
	static int corona_texture_size();
	static PackedByteArray corona_texture_rgba8();

	// Light owner ids for the two identity domains that are not Godot
	// ObjectIDs: a decoded wire handle (16-bit, zero valid) and a static
	// source index, each tagged into a nonzero range disjoint from ObjectIDs
	// so zero stays retail's unowned/world sentinel. 0 for a negative input.
	static constexpr int64_t WIRE_OWNER_TAG = int64_t(1) << 48;
	static constexpr int64_t STATIC_OWNER_TAG = int64_t(2) << 48;
	static constexpr int64_t WIRE_HANDLE_MASK = 0xFFFF;
	static int64_t owner_id_for_wire(int64_t p_wire_handle);
	static int64_t owner_id_for_static_source(int64_t p_source_index);

	// The render-slot dominant-light query (SlotShadow's per-slot pick):
	// entity-centered collect + group-gated params, no D3D-fill boost
	// [orig: RenderSlot_UpdateEntityLight @0x5d6a30, see
	// docs/render/render-lighting-re.md]. A C++ seam (not script-bound):
	// fills r_out with the planner's typed inputs (positions in Godot world).
	void slot_shadow_lights(const Vector3 &p_world_pos, float p_radius,
			int64_t p_interior_owner, int p_interior_section,
			const Vector3 &p_ambient_scale, int p_time_ms, Weather *p_weather,
			std::vector<opennova::renderer::SlotPointLight> &r_out);

	// The corona billboard walk for this frame [orig:
	// EffectWorld_RenderLightCoronas @0x5aaf40 — witness comment on
	// opennova::renderer::LightScene::collect_corona_quads]: the segment
	// quads (centre, half size, the premultiplied additive colour including
	// the segment fade and the fog-to-black fold) the post-particle overlay
	// stage draws (renderer/scene_overlay.h). models/owner_entities are the
	// SAME parallel arrays the per-model light pass walks — models carrying an
	// occlusion section-mask verdict gate their owned coronas on the
	// visible-section bit; fog is the environment's EnvLightValues (null = no
	// fog; the primary device fog with the color forced black [orig:
	// CD3DDevice_SetFogAndBlendMode(dev, 2) @0x5aafb6]). Returns the row count
	// (the walk's semantics are the renderer_light_scene ctest's).
	int collect_corona_rows(const Vector3 &p_camera_pos,
			const Vector3 &p_camera_forward, const Vector3 &p_ambient_scale,
			int p_time_ms, int p_frame_index, Weather *p_weather,
			const TypedArray<Node3D> &p_models,
			const PackedInt64Array &p_owner_entities,
			const Ref<EnvLightValues> &p_fog);
	// The last collected quads, for the overlay stage (mission-space centres).
	const std::vector<opennova::renderer::LightCoronaQuad> &last_corona_quads() const {
		return corona_quads_scratch_;
	}
	// The weapon Inset pass's own corona walk (renderer/scene_overlay.h
	// kInsetOverlayOrder carries the witness) into r_quads: the same walk over
	// the Inset camera and its phase, each drawn owner gated on its Inset
	// section mask (ObjectModel::get_inset_view_section_mask). Returns the
	// quad count.
	int collect_inset_corona_rows(const Vector3 &p_camera_pos,
			const Vector3 &p_camera_forward, const Vector3 &p_ambient_scale,
			int p_time_ms, int p_frame_index, Weather *p_weather,
			const TypedArray<Node3D> &p_models,
			const PackedInt64Array &p_owner_entities,
			const Ref<EnvLightValues> &p_fog,
			std::vector<opennova::renderer::LightCoronaQuad> &r_quads);
	// Test seam: the last collect as kCoronaRowFloats per row (the Godot-world
	// centre xyz, the half size, the colour rgb).
	static constexpr int kCoronaRowFloats = 7;
	PackedFloat32Array get_last_corona_rows() const;

	// The terrain leg of the pool: per terrain patch, the <= 16 world lights
	// whose AABB overlaps the patch and which the authored terrain flag admits,
	// as the rows the terrain shader re-draws the patch with (the collect, the
	// gates and the ps.1.1 pass's constant live portable in
	// opennova::renderer::LightScene::collect_terrain_pass_rows, witness map in
	// renderer/light_terrain_pass.h). A C++
	// seam for the Terrain device: patch bounds in mission 16.16 (the helper
	// opennova::renderer::terrain_patch_light_bounds folds the render frame), the env
	// light-state gain, the time + weather the flicker reads, and the packed
	// g_EnvTerrainColorRecip the recip factor unpacks. Returns the row total.
	size_t collect_terrain_light_rows(
			const opennova::renderer::TerrainLightPatchBounds *p_patches,
			size_t p_patch_count, const Vector3 &p_ambient_scale, int p_time_ms,
			Weather *p_weather, uint32_t p_recip_packed,
			opennova::renderer::TerrainLightPatchRows *r_rows) const;
	// The same leg over Godot-world AABBs (one per patch), rows as
	// Dictionaries {position (Vector3 Godot world), inv_scale, color (Vector3,
	// the pass's c0 colour), handle} — the GUT seam the terrain device
	// test drives without a built Terrain.
	Array collect_terrain_light_rows_for_bounds(
			const TypedArray<AABB> &p_world_aabbs, const Vector3 &p_ambient_scale,
			int p_time_ms, Weather *p_weather, int p_recip_packed) const;

	// The falloff texture of the terrain light pass, as RGBA8 bytes:
	// "texlight2d" (terrain_light_texture_size() square — the ps.1.1 pass
	// samples it on both falloff stages, the ground-plane disc and the height
	// coordinate; the "texlightspot1d" retail also builds sits in a texture
	// slot no terrain pass reads). The law lives portable in
	// opennova::renderer::falloff_texture_light2d_argb; this only unpacks the
	// words for Image::create_from_data.
	static int terrain_light_texture_size();
	static PackedByteArray terrain_light_disc_rgba8();
	// The cube-normalize map the ps.1.1 pass's stage 0 samples: one face of
	// terrain_light_cube_size() square per call, faces in the +X, -X, +Y, -Y,
	// +Z, -Z layer order (Cubemap.create_from_images). The law is portable in
	// opennova::renderer::cube_normalize_texel_argb.
	static int terrain_light_cube_size();
	static PackedByteArray terrain_light_cube_face_rgba8(int p_face);

	// The pool's diagnostic snapshot (lights/effect_light_report.h).
	Ref<EffectLightReport> get_report() const;
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
	// The camera-global select behind render_frame and census_frame.
	int camera_global_select(const Vector3 &p_camera_world, float p_query_radius,
			const Vector3 &p_ambient_scale, int p_time_ms, Weather *p_weather);
	// count + three (posr, color) pairs + the entity lighting lane.
	static constexpr int STATIC_LIGHT_ROW_TEXELS = 10;
	static constexpr int STATIC_LIGHT_ROW_LANE_TEXEL = STATIC_LIGHT_ROW_TEXELS - 1;
	struct StaticCachedSelection {
		int atlas_row = 0;
		opennova::renderer::LightActiveGroups groups{};
		std::array<opennova::renderer::LightHandle,
				opennova::renderer::LightScene::kSelectLimit> handles{};
		size_t count = 0;
		// Any cached handle carries an RgbGen style: the row's color varies
		// per frame and must re-select even on quiet frames.
		bool animated = false;
		// The re-select count of the last frame that evaluated this row (a
		// skipped row's inputs are unchanged, so its count carries over).
		size_t last_count = 0;
	};
	// The corona frame-input build behind collect_corona_rows (owner masks
	// live in the caller's vector for the call); `p_inset_view` reads each
	// owner's Inset section mask instead of the main view's.
	void build_corona_inputs(const Vector3 &p_camera_pos,
			const Vector3 &p_camera_forward, const Vector3 &p_ambient_scale,
			int p_time_ms, int p_frame_index, Weather *p_weather,
			const TypedArray<Node3D> &p_models,
			const PackedInt64Array &p_owner_entities,
			const Ref<EnvLightValues> &p_fog, bool p_inset_view,
			std::vector<opennova::renderer::LightCoronaOwnerMask> &r_owner_masks,
			opennova::renderer::LightCoronaFrameInputs &r_inputs) const;
	// render_model_frame and render_inset_model_frame: one pass over one view.
	int select_model_frame(const TypedArray<Node3D> &p_models,
			const PackedInt64Array &p_owner_entities,
			const PackedInt64Array &p_interior_owners,
			const PackedInt32Array &p_interior_sections,
			const PackedByteArray &p_robj_scoped,
			const Vector3 &p_ambient_scale, int p_time_ms, Weather *p_weather,
			const PackedVector3Array &p_entity_positions,
			const PackedInt32Array &p_entity_bound_radii_q16, bool p_inset_view);

	opennova::renderer::LightScene scene_;
	// Reused per-frame corona scratch (the fill path runs every frame).
	std::vector<opennova::renderer::LightCoronaOwnerMask> corona_masks_scratch_;
	std::vector<opennova::renderer::LightCoronaQuad> corona_quads_scratch_;
	std::array<opennova::renderer::SelectedLight, opennova::renderer::LightScene::kSelectLimit>
			selected_{};
	size_t selected_count_ = 0;
	// select_for_draws is const for its callers but refreshes the mutable
	// compact cache / cell grid / gather scratch: single-threaded by contract
	// (the main thread's frame), never called concurrently on one scene.
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
	// Steady-frame dirty-row inputs: rows re-evaluate only when the pool's
	// color revision or the ambient gain moved, or the row is gen-animated.
	uint64_t static_cached_color_revision_ = 0;
	std::array<float, 3> static_cached_ambient_{};
	// static_light_rows_bytes_ holds the resident texture payload for the
	// cached row set (the in-place steady path's precondition); the output
	// clear zeroes that payload and drops the claim.
	bool static_bytes_resident_ = false;
	int64_t static_cached_rows_revision_ = -1;
	int static_cached_row_count_ = -1;
	int static_cached_active_draws_ = 0;
	int static_row_count_ = 0;
	int last_static_draws_ = 0;
	int last_lit_static_draws_ = 0;
};

} // namespace godot
