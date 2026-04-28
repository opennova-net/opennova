#include "nova_particle_emitter.h"

#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include "util/texture_path_resolver.h"

#include <algorithm>
#include <memory>

using namespace godot;

namespace {

Ref<NovaParticleCurveRef> choose_curve(const Ref<NovaParticleCurveRef> &primary,
		const Ref<NovaParticleCurveRef> &fallback) {
	if (primary.is_valid() && primary->get_present()) {
		return primary;
	}
	return fallback;
}

} // namespace

NovaParticleEmitter::NovaParticleEmitter() {
	emitter.def = nullptr;
}

NovaParticleEmitter::~NovaParticleEmitter() = default;

void NovaParticleEmitter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_def", "p_def"), &NovaParticleEmitter::set_def);
	ClassDB::bind_method(D_METHOD("get_def"), &NovaParticleEmitter::get_def);
	ClassDB::bind_method(D_METHOD("set_tables", "p_tables"), &NovaParticleEmitter::set_tables);
	ClassDB::bind_method(D_METHOD("get_tables"), &NovaParticleEmitter::get_tables);
	ClassDB::bind_method(D_METHOD("set_seed", "p_seed"), &NovaParticleEmitter::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &NovaParticleEmitter::get_seed);
	ClassDB::bind_method(D_METHOD("set_auto_advance", "p_value"), &NovaParticleEmitter::set_auto_advance);
	ClassDB::bind_method(D_METHOD("get_auto_advance"), &NovaParticleEmitter::get_auto_advance);
	ClassDB::bind_method(D_METHOD("set_time_scale", "p_value"), &NovaParticleEmitter::set_time_scale);
	ClassDB::bind_method(D_METHOD("get_time_scale"), &NovaParticleEmitter::get_time_scale);
	ClassDB::bind_method(D_METHOD("set_shader_material", "p_material"), &NovaParticleEmitter::set_shader_material);
	ClassDB::bind_method(D_METHOD("get_shader_material"), &NovaParticleEmitter::get_shader_material);
	ClassDB::bind_method(D_METHOD("set_texture_dir", "p_dir"), &NovaParticleEmitter::set_texture_dir);
	ClassDB::bind_method(D_METHOD("get_texture_dir"), &NovaParticleEmitter::get_texture_dir);
	ClassDB::bind_method(D_METHOD("get_resolved_texture_path", "layer_index"),
			&NovaParticleEmitter::get_resolved_texture_path);

	ClassDB::bind_method(D_METHOD("play"), &NovaParticleEmitter::play);
	ClassDB::bind_method(D_METHOD("stop"), &NovaParticleEmitter::stop);
	ClassDB::bind_method(D_METHOD("restart"), &NovaParticleEmitter::restart);
	ClassDB::bind_method(D_METHOD("advance", "dt"), &NovaParticleEmitter::advance);
	ClassDB::bind_method(D_METHOD("get_alive_count"), &NovaParticleEmitter::get_alive_count);
	ClassDB::bind_method(D_METHOD("get_visual_layer_count"), &NovaParticleEmitter::get_visual_layer_count);
	ClassDB::bind_method(D_METHOD("get_rendered_instance_count"), &NovaParticleEmitter::get_rendered_instance_count);
	ClassDB::bind_method(D_METHOD("get_textured_layer_count"), &NovaParticleEmitter::get_textured_layer_count);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "def", PROPERTY_HINT_RESOURCE_TYPE, "NovaParticleDef"),
			"set_def", "get_def");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "tables", PROPERTY_HINT_TYPE_STRING,
			String::num(Variant::OBJECT) + "/" + String::num(PROPERTY_HINT_RESOURCE_TYPE) + ":NovaParticleTable"),
			"set_tables", "get_tables");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_advance"), "set_auto_advance", "get_auto_advance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_scale"), "set_time_scale", "get_time_scale");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "shader_material", PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial"),
			"set_shader_material", "get_shader_material");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "texture_dir", PROPERTY_HINT_DIR), "set_texture_dir", "get_texture_dir");
}

void NovaParticleEmitter::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY:
			_ensure_visual_setup();
			if (def.is_valid()) {
				_refresh_emitter();
			}
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
	if (quad_mesh.is_null()) {
		quad_mesh.instantiate();
		quad_mesh->set_size(Vector2(1.0f, 1.0f));
	}

	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		if (mmi_layers[i] == nullptr) {
			mmi_layers[i] = memnew(MultiMeshInstance3D);
			mmi_layers[i]->set_name(String("ParticleLayer") + String::num_int64(i + 1));
			add_child(mmi_layers[i]);
			mmi_layers[i]->set_owner(get_owner());
		}
		if (multimeshes[i].is_null()) {
			multimeshes[i].instantiate();
			multimeshes[i]->set_transform_format(MultiMesh::TRANSFORM_3D);
			multimeshes[i]->set_use_colors(true);
			multimeshes[i]->set_mesh(quad_mesh);
		}
		if (layer_materials[i].is_null()) {
			layer_materials[i] = _make_layer_material();
		}
		mmi_layers[i]->set_multimesh(multimeshes[i]);
		mmi_layers[i]->set_material_override(layer_materials[i]);
	}
}

Ref<ShaderMaterial> NovaParticleEmitter::_make_layer_material() const {
	if (shader_material.is_valid()) {
		Ref<ShaderMaterial> copy = shader_material->duplicate();
		if (copy.is_valid()) {
			return copy;
		}
		Ref<ShaderMaterial> mat;
		mat.instantiate();
		mat->set_shader(shader_material->get_shader());
		return mat;
	}

	Ref<ShaderMaterial> mat;
	mat.instantiate();
	return mat;
}

void NovaParticleEmitter::_clear_multimeshes() {
	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		if (multimeshes[i].is_valid()) {
			multimeshes[i]->set_instance_count(0);
		}
	}
}

void NovaParticleEmitter::_refresh_emitter() {
	if (def.is_valid()) {
		native_def = std::make_unique<opennova::particle::ParticleDef>(def->to_native());
		emitter.max_particles = native_def->emit_maxoverride > 0 ?
				static_cast<std::size_t>(native_def->emit_maxoverride) : 256u;
		opennova::particle::emitter_init(emitter, native_def.get(),
				opennova::particle::Vec3{0, 0, 0},
				static_cast<std::uint32_t>(seed));
	} else {
		native_def.reset();
		emitter.def = nullptr;
		emitter.particles.clear();
	}
	_clear_multimeshes();
}

Ref<NovaParticleTable> NovaParticleEmitter::_find_table(const String &id) const {
	if (id.is_empty()) {
		return Ref<NovaParticleTable>();
	}
	for (int i = 0; i < tables.size(); ++i) {
		Ref<NovaParticleTable> table = tables[i];
		if (table.is_valid() && table->get_id() == id) {
			return table;
		}
	}
	return Ref<NovaParticleTable>();
}

float NovaParticleEmitter::_sample_curve(const Ref<NovaParticleCurveRef> &curve, float t, float fallback) const {
	if (curve.is_null() || !curve->get_present()) {
		return fallback;
	}
	Ref<NovaParticleTable> table = _find_table(curve->get_name());
	if (table.is_null()) {
		return fallback;
	}
	float sample_t = std::clamp(t, 0.0f, 1.0f);
	if (curve->get_reverse()) {
		sample_t = 1.0f - sample_t;
	}
	float value = static_cast<float>(table->sample(sample_t)) / 255.0f;
	if (curve->get_inverse()) {
		value = 1.0f - value;
	}
	return value;
}

Color NovaParticleEmitter::_layer_color(const Ref<NovaParticleGraphicLayer> &layer, std::uint8_t slot) const {
	if (layer.is_valid() && layer->get_present() && layer->get_color_overrides_set()) {
		switch (slot & 3u) {
			case 0:
				return layer->get_color1();
			case 1:
				return layer->get_color2();
			case 2:
				return layer->get_color3_prop();
			default:
				return layer->get_color4();
		}
	}
	if (def.is_valid()) {
		switch (slot & 3u) {
			case 0:
				return def->get_color1();
			case 1:
				return def->get_color2();
			case 2:
				return def->get_color3_prop();
			default:
				return def->get_color4();
		}
	}
	return Color(1.0f, 1.0f, 1.0f, 1.0f);
}

void NovaParticleEmitter::_refresh_layer_materials(
		const std::array<Ref<NovaParticleGraphicLayer>, MAX_VISUAL_LAYERS> &layers,
		const std::array<bool, MAX_VISUAL_LAYERS> &present) {
	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		if (layer_materials[i].is_null()) {
			layer_materials[i] = _make_layer_material();
		}
		if (mmi_layers[i] != nullptr) {
			mmi_layers[i]->set_material_override(layer_materials[i]);
		}

		String texture_name;
		if (present[i] && layers[i].is_valid()) {
			texture_name = layers[i]->get_texture();
		}

		if (texture_name != layer_texture_names[i]) {
			layer_texture_names[i] = texture_name;
			layer_texture_paths[i] = String();
			layer_textures[i].unref();
			if (!texture_dir.is_empty() && !texture_name.is_empty()) {
				layer_texture_paths[i] = opennova::resolve_texture_path(texture_dir, texture_name);
				layer_textures[i] = opennova::load_texture_from_dir(texture_dir, texture_name);
			}
		}

		layer_materials[i]->set_shader_parameter("has_texture", layer_textures[i].is_valid());
		layer_materials[i]->set_shader_parameter("albedo_tex", layer_textures[i]);
	}
}

void NovaParticleEmitter::_update_multimesh() {
	if (def.is_null() || native_def == nullptr) {
		_clear_multimeshes();
		return;
	}
	_ensure_visual_setup();

	std::array<Ref<NovaParticleGraphicLayer>, MAX_VISUAL_LAYERS> layers;
	std::array<bool, MAX_VISUAL_LAYERS> present{};
	bool has_present_layer = false;
	TypedArray<NovaParticleGraphicLayer> graphics = def->get_graphics();
	const int graphic_count = std::min<int>(graphics.size(), MAX_VISUAL_LAYERS);
	for (int i = 0; i < graphic_count; ++i) {
		Ref<NovaParticleGraphicLayer> layer = graphics[i];
		layers[i] = layer;
		if (layer.is_valid() && layer->get_present()) {
			present[i] = true;
			has_present_layer = true;
		}
	}
	if (!has_present_layer) {
		present[0] = true;
	}
	_refresh_layer_materials(layers, present);

	const int alive = static_cast<int>(emitter.particles.size());
	std::array<int, MAX_VISUAL_LAYERS> counts{};
	int fallback_layer = 0;
	for (int layer_idx = 0; layer_idx < MAX_VISUAL_LAYERS; ++layer_idx) {
		if (present[layer_idx]) {
			fallback_layer = layer_idx;
			break;
		}
	}
	auto render_layer_for_particle = [&](const opennova::particle::Particle &p) {
		const int particle_layer = std::clamp<int>(static_cast<int>(p.graphic_layer), 0, MAX_VISUAL_LAYERS - 1);
		return present[particle_layer] ? particle_layer : fallback_layer;
	};

	for (const opennova::particle::Particle &p : emitter.particles) {
		++counts[render_layer_for_particle(p)];
	}
	for (int layer_idx = 0; layer_idx < MAX_VISUAL_LAYERS; ++layer_idx) {
		if (multimeshes[layer_idx].is_valid()) {
			const int target_count = present[layer_idx] ? counts[layer_idx] : 0;
			if (multimeshes[layer_idx]->get_instance_count() != target_count) {
				multimeshes[layer_idx]->set_instance_count(target_count);
			}
		}
	}
	if (alive == 0) {
		return;
	}

	std::array<int, MAX_VISUAL_LAYERS> cursors{};
	for (int i = 0; i < alive; ++i) {
		const opennova::particle::Particle &p = emitter.particles[static_cast<std::size_t>(i)];
		const int layer_idx = render_layer_for_particle(p);
		if (!present[layer_idx] || multimeshes[layer_idx].is_null()) {
			continue;
		}
		const float t = p.lifetime > 0.0f ?
				std::clamp(1.0f - (p.age / p.lifetime), 0.0f, 1.0f) : 1.0f;
		const Ref<NovaParticleGraphicLayer> layer = layers[layer_idx];
		float base_scale = def->get_scale_value();
		float layer_alpha = 1.0f;
		Ref<NovaParticleCurveRef> scale_curve = def->get_scale_func();
		Ref<NovaParticleCurveRef> alpha_curve = def->get_alpha_func();
		Ref<NovaParticleCurveRef> red_curve = def->get_red_func();
		Ref<NovaParticleCurveRef> green_curve = def->get_green_func();
		Ref<NovaParticleCurveRef> blue_curve = def->get_blue_func();
		if (layer.is_valid() && layer->get_present()) {
			if (layer->get_scale_value() > 0.0f) {
				base_scale = layer->get_scale_value();
			}
			layer_alpha = layer->get_alpha();
			scale_curve = choose_curve(layer->get_scale_func(), scale_curve);
			alpha_curve = choose_curve(layer->get_alpha_func(), alpha_curve);
			red_curve = choose_curve(layer->get_red_func(), red_curve);
			green_curve = choose_curve(layer->get_green_func(), green_curve);
			blue_curve = choose_curve(layer->get_blue_func(), blue_curve);
		}
		if (base_scale <= 0.0f) {
			base_scale = 1.0f;
		}

		const float scale_mult = _sample_curve(scale_curve, t, 1.0f);
		const float alpha_mult = _sample_curve(alpha_curve, t, 1.0f);
		const float red_mult = _sample_curve(red_curve, t, 1.0f);
		const float green_mult = _sample_curve(green_curve, t, 1.0f);
		const float blue_mult = _sample_curve(blue_curve, t, 1.0f);

		Transform3D xf;
		const float s = std::max(0.01f, p.scale * base_scale * scale_mult);
		xf.basis = Basis().scaled(Vector3(s, s, s));
		xf.origin = Vector3(p.position.x, p.position.y, p.position.z);
		const int instance_idx = cursors[layer_idx]++;
		multimeshes[layer_idx]->set_instance_transform(instance_idx, xf);

		Color color = _layer_color(layer, p.color_slot);
		color.r = std::clamp(color.r * red_mult, 0.0f, 1.0f);
		color.g = std::clamp(color.g * green_mult, 0.0f, 1.0f);
		color.b = std::clamp(color.b * blue_mult, 0.0f, 1.0f);
		color.a = std::clamp((static_cast<float>(p.alpha) / 255.0f) * layer_alpha * alpha_mult,
				0.0f, 1.0f);
		multimeshes[layer_idx]->set_instance_color(instance_idx, color);
	}
}

void NovaParticleEmitter::set_def(const Ref<NovaParticleDef> &p_def) {
	def = p_def;
	if (is_inside_tree()) {
		_refresh_emitter();
	}
}

Ref<NovaParticleDef> NovaParticleEmitter::get_def() const { return def; }

void NovaParticleEmitter::set_tables(const TypedArray<NovaParticleTable> &p_tables) {
	tables = p_tables;
	if (is_inside_tree()) {
		_update_multimesh();
	}
}

TypedArray<NovaParticleTable> NovaParticleEmitter::get_tables() const { return tables; }

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
	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		layer_materials[i].unref();
		layer_texture_names[i] = String();
		layer_texture_paths[i] = String();
		layer_textures[i].unref();
	}
	if (is_inside_tree()) {
		_ensure_visual_setup();
		_update_multimesh();
	}
}

Ref<ShaderMaterial> NovaParticleEmitter::get_shader_material() const { return shader_material; }

void NovaParticleEmitter::set_texture_dir(const String &p_dir) {
	if (texture_dir == p_dir) {
		return;
	}
	texture_dir = p_dir;
	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		layer_texture_names[i] = String();
		layer_texture_paths[i] = String();
		layer_textures[i].unref();
	}
	if (is_inside_tree()) {
		_update_multimesh();
	}
}

String NovaParticleEmitter::get_texture_dir() const { return texture_dir; }

String NovaParticleEmitter::get_resolved_texture_path(int p_layer_index) const {
	if (p_layer_index < 0 || p_layer_index >= MAX_VISUAL_LAYERS) {
		return String();
	}
	return layer_texture_paths[p_layer_index];
}

void NovaParticleEmitter::play() {
	if (def.is_null()) {
		return;
	}
	if (is_inside_tree()) {
		_ensure_visual_setup();
	}
	_refresh_emitter();
	playing = true;
}

void NovaParticleEmitter::stop() {
	playing = false;
	emitter.particles.clear();
	_clear_multimeshes();
}

void NovaParticleEmitter::restart() {
	if (def.is_null()) {
		return;
	}
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

int NovaParticleEmitter::get_visual_layer_count() const {
	if (def.is_null()) {
		return 0;
	}
	int count = 0;
	TypedArray<NovaParticleGraphicLayer> graphics = def->get_graphics();
	const int graphic_count = std::min<int>(graphics.size(), MAX_VISUAL_LAYERS);
	for (int i = 0; i < graphic_count; ++i) {
		Ref<NovaParticleGraphicLayer> layer = graphics[i];
		if (layer.is_valid() && layer->get_present()) {
			++count;
		}
	}
	return count > 0 ? count : 1;
}

int NovaParticleEmitter::get_rendered_instance_count() const {
	int total = 0;
	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		if (multimeshes[i].is_valid()) {
			total += multimeshes[i]->get_instance_count();
		}
	}
	return total;
}

int NovaParticleEmitter::get_textured_layer_count() const {
	int total = 0;
	for (int i = 0; i < MAX_VISUAL_LAYERS; ++i) {
		if (layer_textures[i].is_valid()) {
			++total;
		}
	}
	return total;
}
