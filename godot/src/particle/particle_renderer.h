#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "particle/effect_scene.h"

namespace godot {

class Camera3D;

// Thin Godot adapter for the portable particle scene/frame modules. World
// draw lists are immutable values consumed by the ordered compositor effects:
// a water-far PRE_TRANSPARENT subset, a camera-side POST_TRANSPARENT subset,
// and two consecutive POST_TRANSPARENT mirror subsets. Only the explicitly
// diagnosed FirstPerson tool path uses ArrayMesh. Effects, emitters, and
// particles remain values in EffectScene.
class ParticleRenderer : public Node3D {
	GDCLASS(ParticleRenderer, Node3D)

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
	Ref<EffectScene> scene_;
	Callable texture_provider_;
	String texture_dir_;
	ObjectID environment_source_;
	ObjectID reflection_camera_;
	float water_height_ = 0.0f;
	bool hidden_ = false;
	bool shutdown_ = false;
	bool procedural_fallback_enabled_ = false;
	std::vector<Node *> warm_nodes_;

	void _invalidate_catalog();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	ParticleRenderer();
	~ParticleRenderer() override;

	void set_scene(const Ref<EffectScene> &p_scene);
	Ref<EffectScene> get_scene() const;

	void set_texture_provider(const Callable &p_provider);
	Callable get_texture_provider() const;
	void set_texture_dir(const String &p_texture_dir);
	String get_texture_dir() const;
	void set_environment_source(Node *p_source);
	Node *get_environment_source() const;
	// One exact render-plane handoff: the height partitions World emitters and
	// the mirror camera receives its own camera-correct pair of submissions.
	void set_water_plane(float p_height, Camera3D *p_reflection_camera);

	void set_hidden(bool p_hidden);
	bool get_hidden() const;
	void set_procedural_fallback_enabled(bool p_enabled);
	bool get_procedural_fallback_enabled() const;

	// Deterministic pipeline warm for the loading screen: one centimeter quad
	// per FirstPerson spatial shader plus a one-shot request for all eight
	// World RenderingDevice pipelines on the compositor's real framebuffer.
	// Warming by spawning effects alone is timing-dependent (delayed emitters
	// emit nothing during the warm frames). clear_warm_pipelines frees the
	// quads and cancels an unserviced compositor request.
	void warm_pipelines(const Vector3 &p_position);
	void clear_warm_pipelines();
	// Process-exit boundary for compositor callbacks and device-owned effects.
	void shutdown();

	// Compiles the latest fixed-tick scene snapshot for both render domains and
	// publishes an immutable World copy across the render-thread boundary.
	// Process-driven rendering calls this automatically; tests and previews may
	// call it explicitly after advancing a scene.
	void render_now();

	// Renderer-owned diagnostics are plain values. No MeshInstance or material
	// references escape through the F3/debug seam. They are read from each
	// compiler's retained draw list on demand, so a render spends nothing on
	// them and the first report after render_now() is already live: no
	// capture flag exists to forget.
	int64_t get_rendered_quad_count() const;
	int64_t get_draw_command_count() const;
	Dictionary get_debug_draw_list_report() const;
	Array get_debug_emitter_bounds() const;
	PackedStringArray get_unresolved_texture_names() const;
};

} // namespace godot
