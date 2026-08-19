#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <renderer/light_scene.h>

namespace godot {

class Weather;

// Godot adapter for the portable EffectWorld dynamic light pool
// (renderer::LightScene — engine/runtime/renderer/light_scene.h carries the
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
	// intensity (float), owner_entity / owner_section (int),
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
	// Retire the currently published shader selection without destroying the
	// live mission pool (for camera loss and world-transition frames).
	void clear_render_output();

	// The 62 Hz lifecycle decay — call once per fixed tick.
	void advance_fixed_tick();

	// Report/debug leg: query the pool around a camera point and select the
	// witnessed <= 4 into the report rows. The gameplay object pass is
	// render_model_frame below; this camera-global path publishes nothing.
	int render_frame(const Vector3 &p_camera_world, float p_query_radius,
			const Vector3 &p_ambient_scale, int p_time_ms, Object *p_weather);

	// The per-draw gameplay pass (retail: update_light_slots @0x5abc50 per
	// draw context, see docs/render/render-lighting-re.md): one draw context
	// per visible ObjectModel, owner group = that model's entity id, then the
	// selected <= 4 written as per-instance shader parameters on the model's
	// surfaces. p_models and p_owner_entities are parallel arrays. Returns
	// the number of models that received at least one light.
	int render_model_frame(const TypedArray<Node3D> &p_models,
			const PackedInt64Array &p_owner_entities,
			const Vector3 &p_ambient_scale, int p_time_ms, Object *p_weather);

	int live_count() const;
	Dictionary get_report() const;

protected:
	static void _bind_methods();

private:
	renderer::LightScene scene_;
	std::array<renderer::SelectedLight, renderer::LightScene::kSelectLimit>
			selected_{};
	size_t selected_count_ = 0;
	String selection_mode_ = "none";
	String owner_isolation_ = "none";
	int last_models_ = 0;
	int last_lit_models_ = 0;
};

} // namespace godot
