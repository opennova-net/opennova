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
	// witnessed <= 4 into the report rows. The gameplay presentation is
	// sync_scene_lights below; this camera-global path publishes nothing.
	int render_frame(const Vector3 &p_camera_world, float p_query_radius,
			const Vector3 &p_ambient_scale, int p_time_ms, Weather *p_weather);

	// ADR 0043: present the whole pool as real scene OmniLight3D nodes under
	// p_parent — one node per alive, un-hidden instance, position/color/range
	// from the engine's collect_scene_lights fold (record color x blend x
	// gain x RgbGen flicker) at the MODULATE2X light energy. Replaces the
	// retired per-draw select + instance-uniform / static-atlas delivery; the
	// owner/interior group gate and the 3-per-strip cap were D3D light-slot
	// technique and do not survive (range bounds the bleed). Returns the live
	// light count.
	int sync_scene_lights(Node3D *p_parent, const Vector3 &p_ambient_scale,
			int p_time_ms, Weather *p_weather);

	// The procedural corona texture "texlightcrn" as RGBA8 bytes,
	// corona_texture_size() square — opennova::renderer::corona_texture_argb carries
	// the law; this only unpacks the words for Image::create_from_data.
	static int corona_texture_size();
	static PackedByteArray corona_texture_rgba8();

	// The render-slot dominant-light query (SlotShadow's per-slot pick):
	// entity-centered collect + group-gated params, no D3D-fill boost
	// [orig: RenderSlot_UpdateEntityLight @0x5d6a30, see
	// docs/render/render-lighting-re.md]. A C++ seam (not script-bound):
	// fills r_out with the planner's typed inputs (positions in Godot world).
	void slot_shadow_lights(const Vector3 &p_world_pos, float p_radius,
			const Vector3 &p_ambient_scale, int p_time_ms, Weather *p_weather,
			std::vector<opennova::renderer::SlotPointLight> &r_out);

	// The corona billboard rows for this frame [orig:
	// EffectWorld_RenderLightCoronas @0x5aaf40 — witness comment on
	// opennova::renderer::LightScene::collect_corona_quads]: one Dictionary per
	// additive camera-facing quad, keys position (Vector3 Godot world),
	// half_size (float world units), color (Color, premultiplied additive
	// including the segment fade and the fog-to-black fold). models/
	// owner_entities are the SAME parallel arrays the per-model light pass
	// walks — models carrying an occlusion section-mask verdict gate their
	// owned coronas on the visible-section bit; fog is
	// {enabled, type, start, end} (primary device fog, color forced black
	// [orig: CD3DDevice_SetFogAndBlendMode(dev, 2) @0x5aafb6]). The
	// presenter (effect_light_director.gd) feeds the rows into a MultiMesh;
	// marshalling only.
	TypedArray<Dictionary> collect_corona_rows(const Vector3 &p_camera_pos,
			const Vector3 &p_camera_forward, const Vector3 &p_ambient_scale,
			int p_time_ms, int p_frame_index, Weather *p_weather,
			const TypedArray<Node3D> &p_models,
			const PackedInt64Array &p_owner_entities,
			const Dictionary &p_fog);

	Dictionary get_report() const;

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
	opennova::renderer::LightScene scene_;
	std::array<opennova::renderer::SelectedLight, opennova::renderer::LightScene::kSelectLimit>
			selected_{};
	size_t selected_count_ = 0;
	String selection_mode_ = "none";
	String owner_isolation_ = "none";
	// The scene omni presentation (ADR 0043): one OmniLight3D per live pool
	// instance, pooled and reused across frames.
	std::vector<ObjectID> omni_light_ids_;
	int last_scene_lights_ = 0;
};

} // namespace godot
