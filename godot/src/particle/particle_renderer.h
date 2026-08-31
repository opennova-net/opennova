#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "particle/effect_scene.h"

namespace godot {

// Scene-graph presenter for the portable particle simulation (ADR 0043).
// Every live emitter presents as pooled MultiMeshInstance3D billboard quads —
// per-instance transform/color/custom written from the fixed-tick frame
// snapshot — with StandardMaterial3D blend/flipbook/proximity-fade doing the
// device work Godot already owns. The witnessed decode (curve LUTs, flipbook
// clock, spawn colors, LOD divisor, camera pull) stays in the snapshot walk;
// effects, emitters, and particles remain values in EffectScene.
class ParticleRenderer : public Node3D {
	GDCLASS(ParticleRenderer, Node3D)

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
	Ref<EffectScene> scene_;
	Callable texture_provider_;
	String texture_dir_;
	bool hidden_ = false;
	bool procedural_fallback_enabled_ = false;

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

	void set_hidden(bool p_hidden);
	bool get_hidden() const;
	void set_procedural_fallback_enabled(bool p_enabled);
	bool get_procedural_fallback_enabled() const;

	// Builds the texture catalog and the full material set ahead of first
	// spawn so the loading screen absorbs pipeline compilation instead of the
	// first effect. Presenting through scene materials leaves nothing device-
	// owned to cancel; clear_warm_pipelines is retained as the paired no-op.
	void warm_pipelines(const Vector3 &p_position);
	void clear_warm_pipelines();
	// Hides and releases the presenter pool. EXIT_TREE calls it too; the next
	// ENTER_TREE clears the latch so a re-added renderer presents again.
	void shutdown();

	// Presents the latest fixed-tick scene snapshot. Process-driven rendering
	// calls this once per display frame; tests and previews may call it
	// explicitly after advancing a scene.
	void render_now();

	// Presenter diagnostics are plain values read from the retained frame
	// stats; no mesh or material references escape the F3/debug seam.
	int64_t get_rendered_quad_count() const;
	int64_t get_draw_command_count() const;
	Dictionary get_debug_draw_list_report() const;
	Array get_debug_emitter_bounds() const;
	PackedStringArray get_unresolved_texture_names() const;
};

} // namespace godot
