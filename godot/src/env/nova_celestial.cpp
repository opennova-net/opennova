#include "env/nova_celestial.h"

#include <cmath>

#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

#include <renderer/render_order.h>

#include "env/env_render_camera.h"
#include "env/nova_mission_environment.h"
#include "object/nova_object_data.h"
#include "object/nova_object_model.h"

namespace godot {

namespace {

Vector3 to_vector3(const opennova::env::Vec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

Vector3 to_vector3(const opennova::env::Rgb &rgb) {
	return Vector3(rgb.r, rgb.g, rgb.b);
}

opennova::env::Vec3 to_vec3(const Vector3 &v) {
	return opennova::env::Vec3{static_cast<float>(v.x),
			static_cast<float>(v.y), static_cast<float>(v.z)};
}

} // namespace

void Celestial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_environment_path", "path"),
			&Celestial::set_environment_path);
	ClassDB::bind_method(D_METHOD("get_environment_path"),
			&Celestial::get_environment_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "environment_path"),
			"set_environment_path", "get_environment_path");
	ClassDB::bind_method(D_METHOD("set_terrain_data", "data"),
			&Celestial::set_terrain_data);
	ClassDB::bind_method(D_METHOD("get_terrain_data"),
			&Celestial::get_terrain_data);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_data",
						 PROPERTY_HINT_RESOURCE_TYPE, "TerrainData"),
			"set_terrain_data", "get_terrain_data");
	ClassDB::bind_method(D_METHOD("set_resource_root", "root"),
			&Celestial::set_resource_root);
	ClassDB::bind_static_method("Celestial",
			D_METHOD("source_material_uses_additive", "source"),
			&Celestial::source_material_uses_additive);
	// The externally-callable render-frame drive (the _process body): the
	// test harness drives frames here; the engine's virtual delegates in.
	ClassDB::bind_method(D_METHOD("advance_frame", "delta"),
			&Celestial::advance_frame);
}

void Celestial::set_environment_path(const NodePath &p_path) {
	environment_path_ = p_path;
	env_node_id_ = ObjectID();
}

void Celestial::set_terrain_data(const Ref<TerrainData> &p_data) {
	terrain_data_ = p_data;
}

void Celestial::set_resource_root(const Ref<ResourceRoot> &p_root) {
	resource_root_ = p_root;
	loaded_names_.clear();
	_rebuild_if_needed();
}

MissionEnvironment *Celestial::_env_node() {
	if (env_node_id_.is_valid()) {
		MissionEnvironment *env = Object::cast_to<MissionEnvironment>(
				ObjectDB::get_instance(env_node_id_));
		if (env != nullptr && env->is_inside_tree()) {
			return env;
		}
	}
	if (environment_path_.is_empty() ||
			(!is_inside_tree() && environment_path_.is_absolute())) {
		return nullptr;
	}
	MissionEnvironment *env = Object::cast_to<MissionEnvironment>(
			get_node_or_null(environment_path_));
	env_node_id_ = env != nullptr ? ObjectID(env->get_instance_id())
								  : ObjectID();
	return env;
}

Ref<EnvFile> Celestial::_env_data() {
	MissionEnvironment *env = _env_node();
	return env != nullptr ? env->get_environment_data() : Ref<EnvFile>();
}

void Celestial::_ready() {
	set_process(true);
	if (glare_occlusion_.is_null()) {
		glare_occlusion_.instantiate();
	}
	if (star_core_.is_null()) {
		star_core_.instantiate();
	}
	_rebuild_if_needed();
}

void Celestial::_rebuild_if_needed() {
	Ref<EnvFile> env_data = _env_data();
	if (env_data.is_null() || resource_root_.is_null()) {
		return;
	}
	struct Spec {
		String key;
		String name;
		bool additive;
		int priority;
		String tint;
	};
	// The witnessed load policy (celestial_frame.h carries the cites): the
	// sky pass draws star field then bodies BEFORE all world alpha; the
	// glare is the frame's final draw. Rungs are single-sourced from
	// engine/runtime/renderer/render_order (REN-3).
	const Spec wanted[] = {
		{ "sun", env_data->get_sun_3di(), false,
				renderer::kRungSkyBody, "sun" },
		{ "moon", env_data->get_moon_3di(), false,
				renderer::kRungSkyBody, "moon" },
		{ "glare", env_data->get_glare_3di(), true,
				renderer::kRungSunGlow, "sun" },
	};
	// Rebuild only when the set of names actually changed (undo/scrub safe).
	HashMap<String, String> signature;
	for (const Spec &spec : wanted) {
		signature[spec.key] = spec.name;
	}
	signature["star"] = env_data->get_star_3di();
	if (signature.size() == loaded_names_.size()) {
		bool same = true;
		for (const KeyValue<String, String> &kv : signature) {
			const String *found = loaded_names_.getptr(kv.key);
			if (found == nullptr || *found != kv.value) {
				same = false;
				break;
			}
		}
		if (same) {
			return;
		}
	}
	loaded_names_ = signature;

	for (int i = get_child_count() - 1; i >= 0; --i) {
		Node *child = get_child(i);
		remove_child(child);
		child->queue_free();
	}
	bodies_.clear();
	star_mmi_ = nullptr;
	_build_star_field(env_data->get_star_3di());

	for (const Spec &spec : wanted) {
		if (spec.name.strip_edges().is_empty()) {
			continue;
		}
		Ref<ObjectData> data = _load_object_data(spec.name);
		if (data.is_null()) {
			continue;
		}
		ObjectModel *model = memnew(ObjectModel);
		model->set_name("Celestial_" + spec.key);
		add_child(model);
		// Celestial bodies + the sun glow render INTO the water mirror —
		// keep them on the mirror-visible world layer, unlike the filtered
		// world entities (the witness rides the water mirror record).
		model->set_mirror_reflected(true);
		model->set_object_data(data);
		Ref<ShaderMaterial> material =
				_make_celestial_material(spec.additive, spec.priority);
		Body body;
		body.model = model;
		body.materials = _apply_material_override(model, material);
		body.tint = spec.tint;
		if (spec.key == "glare") {
			for (const Ref<ShaderMaterial> &surface_material : body.materials) {
				surface_material->set_shader_parameter("u_glare_view_fade",
						true);
			}
		}
		bodies_[spec.key] = body;
	}
}

Ref<ObjectData> Celestial::_load_object_data(const String &p_graphic) {
	String model_name = p_graphic;
	if (model_name.get_extension().is_empty()) {
		model_name += ".3di";
	}
	Ref<ObjectData> data;
	data.instantiate();
	if (data->open_from_resource_root(resource_root_, model_name) == OK) {
		return data;
	}
	return Ref<ObjectData>();
}

Ref<ShaderMaterial> Celestial::_make_celestial_material(bool p_additive,
		int p_priority) {
	if (celestial_shader_.is_null()) {
		celestial_shader_ = ResourceLoader::get_singleton()->load(
				"res://shaders/celestial.gdshader");
		celestial_additive_shader_ = ResourceLoader::get_singleton()->load(
				"res://shaders/celestial_additive.gdshader");
	}
	Ref<ShaderMaterial> material;
	material.instantiate();
	material->set_shader(p_additive ? celestial_additive_shader_
									: celestial_shader_);
	material->set_render_priority(p_priority);
	if (p_additive) {
		// Star default opacity — a stand-in until the witnessed per-star
		// twinkle lands (env #33; the 0x2000/0x10000 value stems from the
		// dead variant). The glare overwrites its opacity per frame.
		material->set_shader_parameter("u_opacity",
				static_cast<float>(0x2000) / 65536.0f);
	}
	return material;
}

// Walk the model's MeshInstance3D parts, reuse each surface's diffuse texture
// in our celestial material, and return the ACTUAL installed materials.
// ObjectModel puts its generated material in
// GeometryInstance3D.material_override; clear that whole-mesh override AFTER
// harvesting its texture so these surface overrides own the draw and remain
// the objects updated by the TOD pass.
Vector<Ref<ShaderMaterial>> Celestial::_apply_material_override(Node3D *p_model,
		const Ref<ShaderMaterial> &p_base_material) {
	Vector<Ref<ShaderMaterial>> installed;
	Vector<MeshInstance3D *> meshes;
	_collect_meshes(p_model, meshes);
	for (MeshInstance3D *mesh_instance : meshes) {
		// The vertex shader relocates celestials for the active render-pass
		// camera. Keep the source-camera AABB/occlusion result from rejecting
		// the mirror pass before that relocation reaches the GPU.
		mesh_instance->set_extra_cull_margin(1.0e6);
		mesh_instance->set_ignore_occlusion_culling(true);
		mesh_instance->set_cast_shadows_setting(
				GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		Ref<Mesh> mesh = mesh_instance->get_mesh();
		const int surface_count =
				mesh.is_valid() ? mesh->get_surface_count() : 0;
		Vector<Ref<ShaderMaterial>> surface_materials;
		for (int surface = 0; surface < surface_count; ++surface) {
			Ref<Material> src = mesh_instance->get_active_material(surface);
			Ref<ShaderMaterial> material = p_base_material->duplicate();
			// Preserve the source material's blend classification while
			// replacing only the celestial placement/tint shader logic.
			if (source_material_uses_additive(src)) {
				material->set_shader(celestial_additive_shader_);
			}
			Ref<ShaderMaterial> src_shader = src;
			if (src_shader.is_valid()) {
				const Variant diffuse =
						src_shader->get_shader_parameter("u_diffuse");
				if (diffuse.booleanize()) {
					material->set_shader_parameter("u_diffuse", diffuse);
				}
			}
			surface_materials.push_back(material);
		}
		// ObjectModel's whole-mesh override otherwise wins over the celestial
		// surface materials on the render path.
		mesh_instance->set_material_override(Ref<Material>());
		for (int surface = 0; surface < surface_count; ++surface) {
			const Ref<ShaderMaterial> &material = surface_materials[surface];
			mesh_instance->set_surface_override_material(surface, material);
			installed.push_back(material);
		}
	}
	return installed;
}

bool Celestial::source_material_uses_additive(const Ref<Material> &p_source) {
	Ref<ShaderMaterial> shader_material = p_source;
	if (shader_material.is_null()) {
		return false;
	}
	Ref<Shader> shader = shader_material->get_shader();
	return shader.is_valid() && shader->get_code().contains("blend_add");
}

void Celestial::_collect_meshes(Node *p_node,
		Vector<MeshInstance3D *> &r_out) {
	MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(p_node);
	if (mesh != nullptr) {
		r_out.push_back(mesh);
	}
	for (int i = 0; i < p_node->get_child_count(); ++i) {
		_collect_meshes(p_node->get_child(i), r_out);
	}
}

void Celestial::_process(double p_delta) {
	advance_frame(p_delta);
}

void Celestial::advance_frame(double p_delta) {
	if (bodies_.is_empty()) {
		_rebuild_if_needed();
		if (bodies_.is_empty()) {
			return;
		}
	}
	MissionEnvironment *env = _env_node();
	if (env == nullptr || !env->is_loaded()) {
		return;
	}
	const opennova::env::EnvironmentState &state = env->state();

	Camera3D *cam = Object::cast_to<Camera3D>(
			ObjectDB::get_instance(cached_cam_id_));
	if (cam == nullptr || !cam->is_inside_tree() || !cam->is_current()) {
		cam = find_env_render_camera(this);
		cached_cam_id_ = cam != nullptr ? ObjectID(cam->get_instance_id())
										: ObjectID();
	}
	const Vector3 cam_pos =
			cam != nullptr ? cam->get_global_position() : Vector3();

	if (star_mmi_ != nullptr) {
		Ref<ShaderMaterial> star_material = star_mmi_->get_material_override();
		if (star_material.is_valid()) {
			star_material->set_shader_parameter("u_tint",
					to_vector3(state.sky_ambient()));
			star_material->set_shader_parameter("u_anchor_camera_world",
					cam_pos);
			// Keep every instance local to a camera-anchored MMI. Absolute
			// per-star transforms leave the MultiMesh AABB at the world
			// origin and disappear once a mission camera travels far enough.
			star_mmi_->set_global_position(cam_pos);
		}
	}
	// The active light (sun by day, moon at night) drives the near-light
	// cull (celestial_frame.h carries the cite).
	_update_star_field(env->get_light_direction());

	for (const KeyValue<String, Body> &kv : bodies_) {
		const Body &body = kv.value;
		opennova::env::CelestialBodyFrame frame;
		if (kv.key == "moon") {
			frame = opennova::env::build_moon_frame(state, to_vec3(cam_pos));
		} else if (kv.key == "glare") {
			// env #14 (closed): two jittered terrain rays per frame feed the
			// witnessed 8-sample window + dead-band hysteresis.
			const Vector3 sun_dir = to_vector3(state.sun_direction());
			const float ray_length = glare_occlusion_->get_ray_length();
			const bool visible_a = _glare_ray_clear(cam_pos, sun_dir,
					ray_length, glare_occlusion_->get_ray_jitter_a());
			const bool visible_b = _glare_ray_clear(cam_pos, sun_dir,
					ray_length, glare_occlusion_->get_ray_jitter_b());
			glare_occlusion_->tick(visible_a, visible_b, state.fog_level());
			frame = opennova::env::build_glare_frame(state, to_vec3(cam_pos),
					glare_occlusion_->get_brightness());
			body.model->set_visible(frame.opacity > 0.0f);
			_set_body_parameter(body, "u_glare_direction", sun_dir);
		} else {
			frame = opennova::env::build_sun_frame(state, to_vec3(cam_pos));
		}
		// camera + direction * 64, FULL camera height, identity rotation;
		// below the horizon the terrain depth-occludes the body, like
		// retail's draw order (celestial_frame.h).
		body.model->set_global_position(to_vector3(frame.position));
		_set_body_parameter(body, "u_anchor_camera_world", cam_pos);
		_set_body_parameter(body, "u_tint",
				body.tint == "moon" ? to_vector3(state.moon_color())
									: to_vector3(state.sun_color()));
		_set_body_parameter(body, "u_opacity", frame.opacity);
	}
	(void)p_delta;
}

void Celestial::_set_body_parameter(const Body &p_body,
		const StringName &p_parameter, const Variant &p_value) {
	for (const Ref<ShaderMaterial> &material : p_body.materials) {
		if (material.is_valid()) {
			material->set_shader_parameter(p_parameter, p_value);
		}
	}
}

// env #33: the star field owner — one MultiMesh of camera-facing quads under
// the additive celestial shader, textured with the star 3DI's diffuse. The
// witnessed placement, near-light cull, and twinkle accumulator run in
// engine/formats/env behind the StarField binding.
void Celestial::_build_star_field(const String &p_star_name) {
	if (p_star_name.strip_edges().is_empty() || resource_root_.is_null()) {
		return;
	}
	Ref<ObjectData> data = _load_object_data(p_star_name);
	if (data.is_null()) {
		return;
	}
	Ref<Texture2D> diffuse = data->load_material_texture(0, 0);
	Ref<ShaderMaterial> material =
			_make_celestial_material(true, renderer::kRungSkyStars);
	if (diffuse.is_valid()) {
		material->set_shader_parameter("u_diffuse", diffuse);
	}
	material->set_shader_parameter("u_opacity", 1.0f);
	material->set_shader_parameter("u_billboard", true);
	Ref<MultiMesh> mm;
	mm.instantiate();
	mm->set_transform_format(MultiMesh::TRANSFORM_3D);
	mm->set_use_colors(true);
	Ref<QuadMesh> quad;
	quad.instantiate();
	quad->set_size(Vector2(1.0f, 1.0f));
	mm->set_mesh(quad);
	mm->set_instance_count(star_core_->get_count());
	star_mmi_ = memnew(MultiMeshInstance3D);
	star_mmi_->set_name("StarField");
	star_mmi_->set_multimesh(mm);
	star_mmi_->set_material_override(material);
	// Instances stay local to this camera-anchored node. The field spans the
	// whole sky around that local origin; cull as one conservative unit and
	// bypass main-view occlusion because the reflection pass reanchors it.
	star_mmi_->set_custom_aabb(
			AABB(Vector3(-600, -600, -600), Vector3(1200, 1200, 1200)));
	star_mmi_->set_extra_cull_margin(1.0e6);
	star_mmi_->set_ignore_occlusion_culling(true);
	star_mmi_->set_cast_shadows_setting(
			GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	add_child(star_mmi_);
	star_core_->regenerate(1);
}

void Celestial::_update_star_field(const Vector3 &p_light_dir) {
	if (star_mmi_ == nullptr) {
		return;
	}
	Ref<MultiMesh> mm = star_mmi_->get_multimesh();
	const PackedFloat32Array buf = star_core_->tick_frame(p_light_dir);
	if (buf.is_empty() || mm.is_null()) {
		return;
	}
	const int count = star_core_->get_count();
	for (int i = 0; i < count; ++i) {
		const int o = i * 6;
		const bool star_visible = buf[o + 5] > 0.5f;
		if (!star_visible) {
			mm->set_instance_transform(i,
					Transform3D(Basis().scaled(Vector3()), Vector3()));
			continue;
		}
		const Vector3 offset(buf[o], buf[o + 1], buf[o + 2]);
		const float scale = buf[o + 3];
		const float brightness = buf[o + 4];
		// Orientation is deliberately identity here. The additive vertex
		// shader rebuilds the quad from the active pass camera's right/up
		// basis, which is the only way one MultiMesh can billboard correctly
		// in both viewports.
		mm->set_instance_transform(i,
				Transform3D(Basis().scaled(Vector3(scale, scale, scale)),
						offset));
		mm->set_instance_color(i,
				Color(brightness, brightness, brightness));
	}
}

// Terrain line-of-sight for the glare: the ported boolean raycast form over
// the jittered segment (camera -> camera + sun_dir * ray_length + jitter).
// TerrainData.raycast_terrain reports the miss as all-NAN, so clear = the
// hit is NAN. No terrain loaded = clear (nothing occludes) — the
// editor-guard divergence from retail's null-atlas return-HIT, kept
// deliberately: an unloaded world has nothing to block the sun
// (docs/terrain/terrain-re.md §Runtime terrain queries).
bool Celestial::_glare_ray_clear(const Vector3 &p_from,
		const Vector3 &p_sun_dir, float p_ray_length, const Vector3 &p_jitter) {
	if (terrain_data_.is_null() || !terrain_data_->is_loaded()) {
		return true;
	}
	const Vector3 hit = terrain_data_->raycast_terrain(p_from,
			p_from + p_sun_dir * p_ray_length + p_jitter);
	return std::isnan(hit.x);
}

} // namespace godot
