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

#include "nova_effect_scene.h"

namespace godot {

// Thin Godot adapter for the portable particle scene/frame modules. World
// packets are immutable values consumed by a POST_TRANSPARENT compositor
// effect; only the explicitly diagnosed FirstPerson tool path uses ArrayMesh.
// Effects, emitters, and particles remain values in NovaEffectScene.
class NovaParticleRenderer : public Node3D {
	GDCLASS(NovaParticleRenderer, Node3D)

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
	Ref<NovaEffectScene> scene_;
	Callable texture_provider_;
	String texture_dir_;
	ObjectID environment_source_;
	bool hidden_ = false;
	bool procedural_fallback_enabled_ = false;
	std::vector<Node *> warm_nodes_;

	void _invalidate_catalog();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	NovaParticleRenderer();
	~NovaParticleRenderer() override;

	void set_scene(const Ref<NovaEffectScene> &p_scene);
	Ref<NovaEffectScene> get_scene() const;

	void set_texture_provider(const Callable &p_provider);
	Callable get_texture_provider() const;
	void set_texture_dir(const String &p_texture_dir);
	String get_texture_dir() const;
	void set_environment_source(Node *p_source);
	Node *get_environment_source() const;

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

	// Compiles the latest fixed-tick scene snapshot for both render domains and
	// publishes an immutable World copy across the render-thread boundary.
	// Process-driven rendering calls this automatically; tests and previews may
	// call it explicitly after advancing a scene.
	void render_now();

	// Renderer-owned diagnostics are plain values. No MeshInstance or material
	// references escape through the F3/debug seam.
	int64_t get_rendered_quad_count() const;
	int64_t get_draw_command_count() const;
	Dictionary get_debug_packet_report() const;
	Array get_debug_emitter_bounds() const;
	PackedStringArray get_unresolved_texture_names() const;
};

} // namespace godot
