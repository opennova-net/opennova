#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <particle/emitter.h>

#include "nova_particle_curve_ref.h"
#include "nova_particle_def.h"
#include "nova_particle_graphic_layer.h"
#include "nova_particle_table.h"

namespace godot {

// Node3D that owns a portable opennova::particle::Emitter and visualizes its
// particles as camera-facing quad batches, one MeshInstance3D per pdef graphic
// layer.
//
// Engine reference: CParticleEmitter_AdvanceFrame @ 0x5e6570 (sim) +
// CParticleEmitter_BuildBillboardQuads @ 0x5e6d60 (render quads).
class NovaParticleEmitter : public Node3D {
	GDCLASS(NovaParticleEmitter, Node3D)

private:
	static constexpr int MAX_VISUAL_LAYERS = 4;

	Ref<NovaParticleDef> def;
	TypedArray<NovaParticleTable> tables;
	int seed = 1;
	bool auto_advance = true;
	float time_scale = 1.0f;
	bool playing = false;
	// Runtime PTL graphics with no resolved texture are intentionally invisible,
	// matching retail. Editor previews may opt into a diagnostic soft disc.
	bool procedural_fallback_enabled = false;
	// Emission direction for cone-shaped defs (Emitter::forward). The node's
	// transform does NOT feed the simulator — quads render world-space
	// top-level — so spawn sites set this explicitly (the original's spawn
	// descriptor carries a direction vector).
	Vector3 emission_forward = Vector3(0, 0, 1);
	String texture_dir;
	// Owner texture seam: when valid, called with the graphic-layer texture NAME
	// and expected to return a Texture2D (or null). Lets the game runtime serve
	// textures from its mounted archives (the engine reads particle textures
	// from tga\ + the mounted volumes [orig: CEffectSystem_Init @ 0x5f6070]);
	// texture_dir stays the loose-file editor path.
	Callable texture_provider;

	opennova::particle::Emitter emitter;
	std::unique_ptr<opennova::particle::ParticleDef> native_def;

	std::array<MeshInstance3D *, MAX_VISUAL_LAYERS> mesh_layers{};
	std::array<Ref<ArrayMesh>, MAX_VISUAL_LAYERS> layer_meshes;
	// CParticleDefEntry_ParseBlendMode @ 0x5e29f0 → 8 distinct D3D blend
	// states; each maps to a dedicated `.gdshader` under modtools/particle/
	// shaders/. Materials are per-layer ShaderMaterials so the texture +
	// has_texture uniforms stay layer-scoped while the shader resource is
	// shared across all emitters via the static cache below.
	std::array<Ref<ShaderMaterial>, MAX_VISUAL_LAYERS> layer_materials;
	// Per-emitter shader cache so each emitter holds its own Ref<Shader>
	// values; clears with the emitter, avoiding leaked-Shader RID warnings on
	// engine shutdown (which destroys RenderingServer before global statics).
	std::array<Ref<Shader>, 8> blend_shader_cache;
	std::array<Ref<Texture2D>, MAX_VISUAL_LAYERS> layer_textures;
	std::array<String, MAX_VISUAL_LAYERS> layer_texture_names;
	// Texture identity includes the authored "flip_frames" count: per-frame files are
	// assembled into one horizontal strip, so changing the count must reload it.
	std::array<int, MAX_VISUAL_LAYERS> layer_texture_frame_counts{};
	std::array<String, MAX_VISUAL_LAYERS> layer_texture_paths;
	std::array<int, MAX_VISUAL_LAYERS> layer_blend_modes{};
	std::array<int, MAX_VISUAL_LAYERS> layer_quad_counts{};
	std::array<int, MAX_VISUAL_LAYERS> layer_last_flip_frames{};
	std::array<int, MAX_VISUAL_LAYERS> layer_last_flip_frame{};
	// CParticleManager_BuildTextureAtlases @ 0x5e8db0: per-emitter atlas
	// combining all present graphic layers' textures into one image. The
	// atlas is bound to every layer material's `albedo_tex`; per-layer
	// `baked_uv_rects` are updated to atlas coordinates by
	// `bake_atlas_layout`. Cache invalidates on any per-layer
	// (width, height, present) change to skip rebuild work.
	Ref<ImageTexture> atlas_texture;
	std::array<int, MAX_VISUAL_LAYERS> atlas_layer_widths{};
	std::array<int, MAX_VISUAL_LAYERS> atlas_layer_heights{};
	std::array<bool, MAX_VISUAL_LAYERS> atlas_layer_present{};
	Ref<Texture2D> fallback_texture;
	int last_render_batch_count = 0;
	int last_sorted_depth_count = 0;
	float debug_first_rotation = 0.0f;
	int debug_first_flip_frame = 0;
	int debug_first_blend_mode = 0;
	bool debug_static_billboard = false;
	Color debug_first_color = Color(1.0f, 1.0f, 1.0f, 1.0f);
	Color debug_first_lit_color = Color(1.0f, 1.0f, 1.0f, 1.0f);
	PackedVector3Array debug_first_quad_vertices;

	void _ensure_visual_setup();
	void _ensure_fallback_texture();
	Ref<ShaderMaterial> _make_layer_material(int p_blend_mode);
	Ref<Shader> _get_blend_shader(int p_blend_mode);
	static String _shader_path_for_blend(int p_blend_mode);
	void _clear_meshes();
	void _refresh_emitter();
	void _refresh_layer_materials(const std::array<Ref<NovaParticleGraphicLayer>, MAX_VISUAL_LAYERS> &layers,
			const std::array<bool, MAX_VISUAL_LAYERS> &present);
	void _rebuild_atlas_texture(const std::array<bool, MAX_VISUAL_LAYERS> &present);
	void _update_meshes();

	Ref<NovaParticleTable> _find_table(const String &id, bool p_modified) const;
	float _sample_curve(const Ref<NovaParticleCurveRef> &curve, float t, float fallback) const;
	Color _layer_color(const Ref<NovaParticleGraphicLayer> &layer, std::uint8_t slot) const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	NovaParticleEmitter();
	~NovaParticleEmitter() override;

	void set_def(const Ref<NovaParticleDef> &p_def);
	Ref<NovaParticleDef> get_def() const;

	void set_tables(const TypedArray<NovaParticleTable> &p_tables);
	TypedArray<NovaParticleTable> get_tables() const;

	void set_seed(int p_seed);
	int get_seed() const;

	void set_auto_advance(bool p_value);
	bool get_auto_advance() const;

	void set_time_scale(float p_value);
	float get_time_scale() const;

	void set_procedural_fallback_enabled(bool p_value);
	bool get_procedural_fallback_enabled() const;

	void set_texture_dir(const String &p_dir);
	String get_texture_dir() const;
	String get_resolved_texture_path(int p_layer_index) const;

	void set_texture_provider(const Callable &p_provider);
	Callable get_texture_provider() const;

	void set_color_tint(const Color &p_tint);
	Color get_color_tint() const;

	void set_spring_const(float p_value);
	float get_spring_const() const;

	void set_lod_divisor(int p_value);
	int get_lod_divisor() const;

	void set_kill_plane_mode(int p_value);
	int get_kill_plane_mode() const;
	void set_kill_plane_y(float p_value);
	float get_kill_plane_y() const;

	void set_emission_forward(const Vector3 &p_forward);
	Vector3 get_emission_forward() const;

	Vector3 get_debug_last_translation_delta() const;
	Vector3 get_debug_first_layer_aabb_center() const;
	Color get_debug_first_lit_color() const;
	Ref<ImageTexture> get_debug_atlas_texture() const;
	Ref<ShaderMaterial> get_debug_layer_material(int p_layer_index) const;

	void play();
	void stop();
	void stop_emitting(); // cease spawning, let alive particles drain (detach semantics)
	void restart();
	void advance(float dt);

	bool is_finite() const;
	bool is_finished() const;
	int get_alive_count() const;
	int get_visual_layer_count() const;
	int get_rendered_instance_count() const;
	int get_textured_layer_count() const;
	PackedStringArray get_unresolved_texture_names() const;
	int get_render_batch_count() const;
	int get_sorted_depth_count() const;
	float get_debug_first_rotation() const;
	int get_debug_first_flip_frame() const;
	int get_debug_first_blend_mode() const;
	bool get_debug_static_billboard() const;
	String get_debug_first_shader_path() const;
	Color get_debug_first_color() const;
	PackedVector3Array get_debug_first_quad_vertices() const;
};

} // namespace godot
