#include "nova_particle_emitter.h"

#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <algorithm>

using namespace godot;

NovaParticleEmitter::NovaParticleEmitter() {
	emitter.def = nullptr;
}

NovaParticleEmitter::~NovaParticleEmitter() = default;

void NovaParticleEmitter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_def", "p_def"), &NovaParticleEmitter::set_def);
	ClassDB::bind_method(D_METHOD("get_def"), &NovaParticleEmitter::get_def);
	ClassDB::bind_method(D_METHOD("set_seed", "p_seed"), &NovaParticleEmitter::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &NovaParticleEmitter::get_seed);
	ClassDB::bind_method(D_METHOD("set_auto_advance", "p_value"), &NovaParticleEmitter::set_auto_advance);
	ClassDB::bind_method(D_METHOD("get_auto_advance"), &NovaParticleEmitter::get_auto_advance);
	ClassDB::bind_method(D_METHOD("set_time_scale", "p_value"), &NovaParticleEmitter::set_time_scale);
	ClassDB::bind_method(D_METHOD("get_time_scale"), &NovaParticleEmitter::get_time_scale);
	ClassDB::bind_method(D_METHOD("set_shader_material", "p_material"), &NovaParticleEmitter::set_shader_material);
	ClassDB::bind_method(D_METHOD("get_shader_material"), &NovaParticleEmitter::get_shader_material);

	ClassDB::bind_method(D_METHOD("play"), &NovaParticleEmitter::play);
	ClassDB::bind_method(D_METHOD("stop"), &NovaParticleEmitter::stop);
	ClassDB::bind_method(D_METHOD("restart"), &NovaParticleEmitter::restart);
	ClassDB::bind_method(D_METHOD("advance", "dt"), &NovaParticleEmitter::advance);
	ClassDB::bind_method(D_METHOD("get_alive_count"), &NovaParticleEmitter::get_alive_count);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "def", PROPERTY_HINT_RESOURCE_TYPE, "NovaParticleDef"),
			"set_def", "get_def");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_advance"), "set_auto_advance", "get_auto_advance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_scale"), "set_time_scale", "get_time_scale");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "shader_material", PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial"),
			"set_shader_material", "get_shader_material");
}

void NovaParticleEmitter::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY:
			_ensure_visual_setup();
			set_process(true);
			break;
		case NOTIFICATION_PROCESS: {
			if (!playing || !auto_advance) {
				return;
			}
			advance(static_cast<float>(get_process_delta_time()) * time_scale);
			break;
		}
		default:
			break;
	}
}

void NovaParticleEmitter::_ensure_visual_setup() {
	if (mmi == nullptr) {
		mmi = memnew(MultiMeshInstance3D);
		add_child(mmi);
		mmi->set_owner(get_owner());
	}
	if (quad_mesh.is_null()) {
		quad_mesh.instantiate();
		quad_mesh->set_size(Vector2(1.0f, 1.0f));
	}
	if (multimesh.is_null()) {
		multimesh.instantiate();
		multimesh->set_transform_format(MultiMesh::TRANSFORM_3D);
		multimesh->set_use_colors(true);
		multimesh->set_mesh(quad_mesh);
	}
	mmi->set_multimesh(multimesh);

	if (shader_material.is_valid()) {
		quad_mesh->set_material(shader_material);
	} else {
		// Default fallback: built-in material so we still see *something* in the
		// editor before the custom shader is wired up.
		Ref<StandardMaterial3D> mat;
		mat.instantiate();
		mat->set_billboard_mode(BaseMaterial3D::BILLBOARD_ENABLED);
		mat->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
		mat->set_blend_mode(BaseMaterial3D::BLEND_MODE_ADD);
		mat->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		quad_mesh->set_material(mat);
	}
}

void NovaParticleEmitter::_bind_def_to_native() {
	emitter.def = nullptr;
}

void NovaParticleEmitter::_refresh_emitter() {
	if (def.is_valid()) {
		// We materialize a native ParticleDef and own it on the emitter side.
		// The simulator captures by pointer; we keep a long-lived snapshot to
		// back the Emitter::def pointer.
		static thread_local opennova::particle::ParticleDef snapshot;
		snapshot = def->to_native();
		emitter.def = &snapshot;
		opennova::particle::emitter_init(emitter, &snapshot,
				opennova::particle::Vec3{0, 0, 0},
				static_cast<std::uint32_t>(seed));
	} else {
		emitter.def = nullptr;
	}
	if (multimesh.is_valid()) {
		multimesh->set_instance_count(0);
	}
}

void NovaParticleEmitter::_update_multimesh() {
	if (multimesh.is_null() || mmi == nullptr) {
		return;
	}
	const int alive = static_cast<int>(emitter.particles.size());
	if (multimesh->get_instance_count() != alive) {
		multimesh->set_instance_count(alive);
	}
	for (int i = 0; i < alive; ++i) {
		const opennova::particle::Particle &p = emitter.particles[static_cast<std::size_t>(i)];
		Transform3D xf;
		// scale grows from 0..1 over lifetime; multiply by def.scale for world units.
		const float def_scale = def.is_valid() ? def->get_scale_value() : 1.0f;
		const float s = std::max(0.01f, p.scale * def_scale);
		xf.basis = Basis().scaled(Vector3(s, s, s));
		xf.origin = Vector3(p.position.x, p.position.y, p.position.z);
		multimesh->set_instance_transform(i, xf);
		const Color color(static_cast<float>(p.color.r) / 255.0f,
				static_cast<float>(p.color.g) / 255.0f,
				static_cast<float>(p.color.b) / 255.0f,
				static_cast<float>(p.alpha) / 255.0f);
		multimesh->set_instance_color(i, color);
	}
}

void NovaParticleEmitter::set_def(const Ref<NovaParticleDef> &p_def) {
	def = p_def;
	if (is_inside_tree()) {
		_refresh_emitter();
	}
}
Ref<NovaParticleDef> NovaParticleEmitter::get_def() const { return def; }

void NovaParticleEmitter::set_seed(int p_seed) {
	seed = p_seed;
	if (is_inside_tree()) {
		_refresh_emitter();
	}
}
int NovaParticleEmitter::get_seed() const { return seed; }

void NovaParticleEmitter::set_auto_advance(bool p_value) { auto_advance = p_value; }
bool NovaParticleEmitter::get_auto_advance() const { return auto_advance; }

void NovaParticleEmitter::set_time_scale(float p_value) {
	time_scale = std::max(0.0f, p_value);
}
float NovaParticleEmitter::get_time_scale() const { return time_scale; }

void NovaParticleEmitter::set_shader_material(const Ref<ShaderMaterial> &p_material) {
	shader_material = p_material;
	if (quad_mesh.is_valid()) {
		quad_mesh->set_material(shader_material);
	}
}
Ref<ShaderMaterial> NovaParticleEmitter::get_shader_material() const { return shader_material; }

void NovaParticleEmitter::play() {
	if (def.is_null()) {
		return;
	}
	_refresh_emitter();
	playing = true;
}

void NovaParticleEmitter::stop() {
	playing = false;
	emitter.particles.clear();
	if (multimesh.is_valid()) multimesh->set_instance_count(0);
}

void NovaParticleEmitter::restart() {
	_refresh_emitter();
	playing = true;
}

void NovaParticleEmitter::advance(float dt) {
	if (!playing || emitter.def == nullptr) {
		return;
	}
	opennova::particle::emitter_advance(emitter, dt);
	_update_multimesh();
}

int NovaParticleEmitter::get_alive_count() const {
	return static_cast<int>(emitter.particles.size());
}
