#pragma once

#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <particle/emitter.h>

#include "nova_particle_def.h"

namespace godot {

// Node3D that owns a portable opennova::particle::Emitter and visualizes its
// particles via MultiMeshInstance3D + ShaderMaterial. The simulator runs in
// _process(); the MultiMesh transform array updates each frame.
//
// Engine reference: CParticleEmitter_AdvanceFrame @ 0x5e6570 (sim) +
// CParticleEmitter_BuildBillboardQuads @ 0x5e6d60 (render quads).
class NovaParticleEmitter : public Node3D {
	GDCLASS(NovaParticleEmitter, Node3D)

private:
	Ref<NovaParticleDef> def;
	int seed = 1;
	bool auto_advance = true;
	float time_scale = 1.0f;
	bool playing = false;

	opennova::particle::Emitter emitter;

	MultiMeshInstance3D *mmi = nullptr;
	Ref<MultiMesh> multimesh;
	Ref<QuadMesh> quad_mesh;
	Ref<ShaderMaterial> shader_material;

	void _ensure_visual_setup();
	void _refresh_emitter();
	void _update_multimesh();
	void _bind_def_to_native();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	NovaParticleEmitter();
	~NovaParticleEmitter() override;

	void set_def(const Ref<NovaParticleDef> &p_def);
	Ref<NovaParticleDef> get_def() const;

	void set_seed(int p_seed);
	int get_seed() const;

	void set_auto_advance(bool p_value);
	bool get_auto_advance() const;

	void set_time_scale(float p_value);
	float get_time_scale() const;

	void set_shader_material(const Ref<ShaderMaterial> &p_material);
	Ref<ShaderMaterial> get_shader_material() const;

	void play();
	void stop();
	void restart();
	void advance(float dt);

	int get_alive_count() const;
};

} // namespace godot
