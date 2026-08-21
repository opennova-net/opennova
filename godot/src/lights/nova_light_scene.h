#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <renderer/light_scene.h>

#include <vector>

namespace godot {

class Weather;

// Godot adapter for the portable EffectWorld dynamic light pool
// (renderer::LightScene — engine/runtime/renderer/light_scene.h carries the
// witness map). It marshals Godot world coordinates and exposes one evaluated
// active-light snapshot; EffectLightDirector owns the native Godot nodes.
class LightScene : public RefCounted {
	GDCLASS(LightScene, RefCounted)

public:
	// Spawn/mutation handles cross the Variant boundary as opaque positive
	// int64 leases: retail's slot word in bits 0..15, generation in bits 16..47.
	// config keys: position / direction (Vector3 Godot world), atten_start /
	// atten_end (float world units), target (bool), spot_angle_degrees (float),
	// color_start / color_end (Color), style / phase / rate (int), intensity
	// (float), owner_entity / owner_section (int),
	// disable_corona / disable_terrain / disable_objects (bool).
	int64_t spawn_model_light(const Dictionary &p_config);
	// Transient glow spawn (muzzle / impact / death / round legs — the
	// light_scene.h witness map). config keys: position (Vector3), radius
	// (float world units), color (Color), fade_mode / fade_duration (int),
	// owner_entity / owner_section (int), disable_corona / disable_terrain /
	// disable_objects (bool).
	int64_t spawn_glow(const Dictionary &p_config);
	void despawn(int64_t p_handle);
	void set_light_position(int64_t p_handle, const Vector3 &p_world);
	// The witnessed instance re-arm setters (fade mode/duration, blend).
	void set_light_fade(int64_t p_handle, int p_mode, int p_duration);
	void set_light_blend(int64_t p_handle, float p_amount);
	bool is_alive(int64_t p_handle) const;
	void clear();
	// Retire the currently published diagnostic snapshot without destroying
	// the live mission pool (for camera loss and world-transition frames).
	void clear_render_output();

	// The 62 Hz lifecycle decay — call once per fixed tick.
	void advance_fixed_tick();

	// Evaluate every visible live pool entry for the native clustered-light
	// presenter. Rows carry kind, position, direction, sRGB color, energy,
	// range, spot angle, participation flags, and the opaque handle.
	TypedArray<Dictionary> collect_active_rows(int p_time_ms, Object *p_weather);

	// The corona billboard rows for this frame (retail:
	// EffectWorld_RenderLightCoronas @0x5aaf40 — witness comment on
	// renderer::LightScene::collect_corona_quads): one Dictionary per
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
			int p_time_ms, int p_frame_index, Object *p_weather,
			const TypedArray<Node3D> &p_models,
			const PackedInt64Array &p_owner_entities,
			const Dictionary &p_fog);

	int live_count() const;
	Dictionary get_report() const;

protected:
	static void _bind_methods();

private:
	renderer::LightScene scene_;
	std::vector<renderer::ActiveLight> active_;
	String selection_mode_ = "none";
	String owner_isolation_ = "none";
};

} // namespace godot
