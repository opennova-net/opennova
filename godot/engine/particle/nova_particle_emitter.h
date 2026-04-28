#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <particle/emitter.h>

#include "nova_particle_curve_ref.h"
#include "nova_particle_def.h"
#include "nova_particle_graphic_layer.h"
#include "nova_particle_table.h"

namespace godot {

// Node3D that owns a portable opennova::particle::Emitter and visualizes its
// particles via one MultiMeshInstance3D per pdef graphic layer.
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
	String texture_dir;

	opennova::particle::Emitter emitter;
	std::unique_ptr<opennova::particle::ParticleDef> native_def;

	std::array<MultiMeshInstance3D *, MAX_VISUAL_LAYERS> mmi_layers{};
	std::array<Ref<MultiMesh>, MAX_VISUAL_LAYERS> multimeshes;
	std::array<Ref<ShaderMaterial>, MAX_VISUAL_LAYERS> layer_materials;
	std::array<Ref<Texture2D>, MAX_VISUAL_LAYERS> layer_textures;
	std::array<String, MAX_VISUAL_LAYERS> layer_texture_names;
	std::array<String, MAX_VISUAL_LAYERS> layer_texture_paths;
	Ref<QuadMesh> quad_mesh;
	Ref<ShaderMaterial> shader_material;

	void _ensure_visual_setup();
	Ref<ShaderMaterial> _make_layer_material() const;
	void _clear_multimeshes();
	void _refresh_emitter();
	void _refresh_layer_materials(const std::array<Ref<NovaParticleGraphicLayer>, MAX_VISUAL_LAYERS> &layers,
			const std::array<bool, MAX_VISUAL_LAYERS> &present);
	void _update_multimesh();

	Ref<NovaParticleTable> _find_table(const String &id) const;
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

	void set_shader_material(const Ref<ShaderMaterial> &p_material);
	Ref<ShaderMaterial> get_shader_material() const;

	void set_texture_dir(const String &p_dir);
	String get_texture_dir() const;
	String get_resolved_texture_path(int p_layer_index) const;

	void play();
	void stop();
	void restart();
	void advance(float dt);

	int get_alive_count() const;
	int get_visual_layer_count() const;
	int get_rendered_instance_count() const;
	int get_textured_layer_count() const;
};

} // namespace godot
