#include "env/celestial.h"

#include <cmath>

#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

#include <runtime/renderer/render_order.h>

#include "env/env_axes.h"
#include "env/env_render_camera.h"
#include "env/mission_environment.h"
#include "object/object_data.h"
#include "object/object_model.h"

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
	// The frozen-fixture glare settle + the diagnostics snapshot (the
	// capture-refresh seam; header carries the cites).
	ClassDB::bind_method(D_METHOD("settle_glare_occlusion", "max_frames"),
			&Celestial::settle_glare_occlusion, DEFVAL(64));
	ClassDB::bind_method(D_METHOD("get_diagnostics"),
			&Celestial::get_diagnostics);
	ClassDB::bind_method(D_METHOD("get_sun_veil_alpha"),
			&Celestial::get_sun_veil_alpha);
	ClassDB::bind_method(D_METHOD("get_sun_veil_stopdown"),
			&Celestial::get_sun_veil_stopdown);
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
				opennova::renderer::kRungSkyBody, "sun" },
		{ "moon", env_data->get_moon_3di(), false,
				opennova::renderer::kRungSkyBody, "moon" },
		{ "glare", env_data->get_glare_3di(), true,
				opennova::renderer::kRungSunGlow, "sun" },
		// The water-reflected sun glint reuses the glare 3DI, mirrored below
		// the eye [orig: update_sun_glare @ 0x5ad130 submits
		// Celestial_GlareModel at camera + sun * 128 with the height term
		// negated, additive 0x110, see docs/env/env-tod-re.md].
		{ "glint", env_data->get_glare_3di(), true,
				opennova::renderer::kRungSunGlow, "sun" },
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
		// Every celestial submit uses the IDENTITY world rotation in RENDER
		// axes (render_celestial_bodies @ 0x5acaa0 sun/moon,
		// render_skybox_sun_glow @ 0x5acd00 + update_sun_glare @ 0x5ad130
		// glare/glint - docs/env/env-tod-re.md): the authored quads face
		// render +Z = EAST. ObjectData maps model (x, y, z) into Godot
		// (-x, y, z) (object_data_geometry.cpp godot_position), which
		// under an identity basis leaves the quad facing Godot +Z (south) -
		// edge-on at a sunrise/sunset pose, the "squashed oval sun". The
		// +90 degree yaw about +Y is the exact composition
		// R * import(v) == render_float_to_godot(v) (env_axes.h), restoring
		// the retail east-facing placement. Positive axis on purpose:
		// godot-cpp Basis(axis, angle) diverges from core for negative axes.
		model->set_basis(Basis(Vector3(0.0f, 1.0f, 0.0f),
				static_cast<real_t>(Math_PI) * 0.5f));
		Ref<ShaderMaterial> material =
				_make_celestial_material(spec.additive, spec.priority);
		Body body;
		body.model = model;
		const InstalledMaterials installed =
				_apply_material_override(model, material);
		body.materials = installed.materials;
		body.tint = spec.tint;
		if (spec.key == "sun" || spec.key == "moon") {
			// The disc bodies far-pin in BOTH shaders: the authored sun/moon
			// materials are additive, so their surfaces render through
			// celestial_additive (the shader carries the witness note).
			for (const Ref<ShaderMaterial> &surface_material :
					body.materials) {
				surface_material->set_shader_parameter("u_depth_far_pin",
						true);
			}
		}
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
// in our celestial material, and return the ACTUAL installed materials with
// the blend each surface was given. ObjectModel puts its generated material
// in GeometryInstance3D.material_override; clear that whole-mesh override
// AFTER harvesting its texture so these surface overrides own the draw and
// remain the objects updated by the TOD pass.
Celestial::InstalledMaterials Celestial::_apply_material_override(
		Node3D *p_model, const Ref<ShaderMaterial> &p_base_material) {
	InstalledMaterials installed;
	const bool base_additive =
			p_base_material->get_shader() == celestial_additive_shader_;
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
			const bool additive = base_additive ||
					source_material_uses_additive(src);
			if (additive && !base_additive) {
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
			installed.materials.push_back(material);
		}
	}
	return installed;
}

bool Celestial::source_material_uses_additive(const Ref<Material> &p_source) {
	// ObjectModel stamps its classification's blend fact onto every material
	// it generates (object_model_materials.cpp).
	return p_source.is_valid() &&
			bool(p_source->get_meta("_opennova_blend_additive", false));
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

Camera3D *Celestial::_resolve_camera() {
	Camera3D *cam = Object::cast_to<Camera3D>(
			ObjectDB::get_instance(cached_cam_id_));
	if (cam == nullptr || !cam->is_inside_tree() || !cam->is_current()) {
		cam = find_env_render_camera(this);
		cached_cam_id_ = cam != nullptr ? ObjectID(cam->get_instance_id())
										: ObjectID();
	}
	return cam;
}

void Celestial::_publish_idle_veil() {
	// The retail veil writer is gated on the sun model
	// [orig: Environment_ApplySunVeilAndExposureStopdown @0x5ad8ba, see
	// docs/env/env-tod-re.md]: no bodies, no veil. Clear the pair and push a
	// zero alpha so a reload never inherits the last mission's veil. The
	// global is process-wide (another Celestial may have written it), so
	// there is no per-instance latch — one RS call per idle frame.
	sun_veil_glare_ = 0;
	sun_veil_stopdown_ = 0;
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		rs->global_shader_parameter_set("opennova_sun_veil_alpha", 0.0f);
	}
}

void Celestial::advance_frame(double p_delta) {
	if (bodies_.is_empty()) {
		_rebuild_if_needed();
		if (bodies_.is_empty()) {
			_publish_idle_veil();
			return;
		}
	}
	MissionEnvironment *env = _env_node();
	if (env == nullptr || !env->is_loaded()) {
		_publish_idle_veil();
		return;
	}
	const opennova::env::EnvironmentState &state = env->state();

	Camera3D *cam = _resolve_camera();
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
	// cull (celestial_frame.h carries the cite). Deliberately the RAW
	// render-float tuple: the star instance directions it dots against live
	// in the same axes engine-side, and the instance PLACEMENT crosses the
	// env_axes.h swap where the transforms are stamped, so the culled star
	// and the placed star agree.
	_update_star_field(to_vector3(state.light_direction()));

	// The GODOT-world sun direction and view forward (env_axes.h swap — the
	// 2026-08-20 correction: the earlier identity mapping placed every body
	// 90 degrees off in yaw and mirrored, the 03tr-sun-sky "sun rises in the
	// wrong place" half of the fixture's divergence).
	const Vector3 sun_dir = render_float_to_godot(state.sun_direction());
	const Vector3 forward = cam != nullptr
			? -cam->get_global_transform().basis.get_column(2).normalized()
			: Vector3(0.0f, 0.0f, -1.0f);
	for (KeyValue<String, Body> &kv : bodies_) {
		Body &body = kv.value;
		opennova::env::CelestialBodyFrame frame;
		// The frame builders run in the witnessed render-float axes; the
		// camera enters and the placement leaves through the env_axes.h swap.
		const opennova::env::Vec3 cam_rf = godot_to_render_float(cam_pos);
		if (kv.key == "moon") {
			frame = opennova::env::build_moon_frame(state, cam_rf);
		} else if (kv.key == "glint") {
			// The water-reflected sun glint runs its own leg (accumulator,
			// mirrored placement, CPU alpha) [orig: update_sun_glare
			// @ 0x5ad130, see docs/env/env-tod-re.md].
			const float alpha = _advance_water_glint(state, cam_pos, sun_dir,
					forward, body);
			body.last_opacity = alpha;
			continue;
		} else if (kv.key == "glare") {
			// env #14 (closed): ONE coarse unjittered gate ray with the
			// witnessed start-height lift, then two jittered fine rays from
			// the exact camera height, feed the 8-sample window + dead-band
			// hysteresis [orig: render_skybox_sun_glow @ 0x5acd9e..0x5acf7f
			// — the fine rays' entity leg keeps the documented sun-occlusion
			// statics posture (render-lighting-re.md D-RLIT-2/D-RLIT-3), see docs/env/env-tod-re.md].
			const float ray_length = glare_occlusion_->get_ray_length();
			const Vector3 lift(0.0f, opennova::env::glare_coarse_start_lift(
					glare_occlusion_->get_frame_index()), 0.0f);
			const bool coarse_clear = _glare_ray_clear(cam_pos + lift,
					sun_dir, ray_length, Vector3());
			const bool visible_a = coarse_clear && _glare_ray_clear(cam_pos,
					sun_dir, ray_length, glare_occlusion_->get_ray_jitter_a());
			const bool visible_b = coarse_clear && _glare_ray_clear(cam_pos,
					sun_dir, ray_length, glare_occlusion_->get_ray_jitter_b());
			glare_occlusion_->tick(visible_a, visible_b, state.fog_level());
			frame = opennova::env::build_glare_frame(state, cam_rf,
					glare_occlusion_->get_brightness());
			body.model->set_visible(frame.opacity > 0.0f);
			_set_body_parameter(body, "u_glare_direction", sun_dir);
		} else {
			frame = opennova::env::build_sun_frame(state, cam_rf);
		}
		// camera + direction * 64, FULL camera height, identity rotation;
		// below the horizon the terrain depth-occludes the body, like
		// retail's draw order (celestial_frame.h).
		body.model->set_global_position(render_float_to_godot(frame.position));
		_set_body_parameter(body, "u_anchor_camera_world", cam_pos);
		_set_body_parameter(body, "u_tint",
				body.tint == "moon" ? to_vector3(state.moon_color())
									: to_vector3(state.sun_color()));
		_set_body_parameter(body, "u_opacity", frame.opacity);
		body.last_opacity = frame.opacity;
	}

	// The sun-glare screen veil + exposure stop-down, once per frame after
	// the occlusion tick [orig: Environment_ApplySunVeilAndExposureStopdown
	// @ 0x5ad8b0 from Render_ProcessMainSceneFrame @ 0x5cac4b, gated on the
	// sun model, see docs/env/env-tod-re.md]: dot(view_forward, sun) in 16.16 through the witnessed
	// dot^32/dot^128 chain (env::sun_veil_from_dot), plus the water-reflected
	// SECONDARY term at the glint brightness >> 2 when water exists, both
	// sums clamped 192 (env::sun_veil_combine). The veil alpha global
	// feeds the PlayerViewEffects white overlay; the stop-down is read back
	// by the world's veil leg into Weather.
	sun_veil_glare_ = 0;
	sun_veil_stopdown_ = 0;
	if (bodies_.has("sun") && glare_occlusion_.is_valid() && cam != nullptr) {
		const int sun_dim_fixed =
				opennova::env::detail::to_fixed_16_16(state.sun_dim_pct());
		const int overcast_fixed =
				opennova::env::detail::to_fixed_16_16(state.overcast_blend());
		const int view_dot_fixed = static_cast<int>(
				forward.dot(sun_dir) * 65536.0f);
		opennova::env::SunVeil veil = opennova::env::sun_veil_from_dot(
				view_dot_fixed, glare_occlusion_->get_brightness(),
				sun_dim_fixed, overcast_fixed);
		if (state.has_water_height() && state.water_height() != 0.0f) {
			// The secondary reflected-sun ray: direction to the UNJITTERED
			// glint point (view_z 0 [orig: @ 0x5ad628, see docs/env/env-tod-re.md]), brightness >> 2
			// [orig: @ 0x5ad6a7, see docs/env/env-tod-re.md].
			const opennova::env::Vec3 cam_m = godot_to_mission(cam_pos);
			const opennova::env::Vec3 sun_m = godot_to_mission(sun_dir);
			opennova::env::Vec3 point_m;
			if (opennova::env::water_glint_point(cam_m, sun_m,
					state.water_height(), 0.0f, point_m)) {
				const Vector3 point_g = mission_to_godot(point_m);
				const Vector3 to_glint = (point_g - cam_pos).normalized();
				const int dot2 = static_cast<int>(
						forward.dot(to_glint) * 65536.0f);
				const opennova::env::SunVeil secondary =
						opennova::env::sun_veil_from_dot(dot2,
								water_glint_.brightness >> 2, sun_dim_fixed,
								overcast_fixed);
				veil = opennova::env::sun_veil_combine(veil, secondary);
			}
		}
		sun_veil_glare_ = veil.glare;
		sun_veil_stopdown_ = veil.stopdown;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		rs->global_shader_parameter_set("opennova_sun_veil_alpha",
				get_sun_veil_alpha());
	}
	(void)p_delta;
}

float Celestial::get_sun_veil_alpha() const {
	// The draw gate (env::sun_veil_draws) and the quad's alpha byte. The glare
	// stays <= 255 over the authored domain: sun_veil_from_dot caps at 255
	// before a SunDim fold that only scales down for SunDim >= 0 (stock data
	// never writes it), and the water term clamps the sum at 192. A float
	// alpha past 1 would saturate in the blend exactly as a 255 byte does,
	// so no device clamp is added.
	if (!opennova::env::sun_veil_draws(sun_veil_glare_)) {
		return 0.0f;
	}
	return static_cast<float>(sun_veil_glare_) / 255.0f;
}

int Celestial::get_sun_veil_stopdown() const {
	return sun_veil_stopdown_;
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
			_make_celestial_material(true, opennova::renderer::kRungSkyStars);
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
		// The instance offsets arrive in the render-float axes
		// (star_offset_render_float3); place them through the env_axes.h
		// swap so the near-light cull's hidden star is the one the viewer
		// sees beside the bright body.
		const Vector3 offset(buf[o + 2], buf[o + 1], buf[o]);
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

int Celestial::settle_glare_occlusion(int p_max_frames) {
	if (glare_occlusion_.is_null()) {
		glare_occlusion_.instantiate();
	}
	MissionEnvironment *env = _env_node();
	if (env == nullptr || !env->is_loaded()) {
		return glare_occlusion_->get_brightness();
	}
	const opennova::env::EnvironmentState &state = env->state();
	Camera3D *cam = _resolve_camera();
	const Vector3 cam_pos =
			cam != nullptr ? cam->get_global_position() : Vector3();
	const Vector3 sun_dir = render_float_to_godot(state.sun_direction());
	const Vector3 forward = cam != nullptr
			? -cam->get_global_transform().basis.get_column(2).normalized()
			: Vector3(0.0f, 0.0f, -1.0f);
	const float ray_length = glare_occlusion_->get_ray_length();
	// The dead-band step never snaps onto the target, so a settled
	// accumulator HOLDS: stop once the brightness has been unchanged across
	// eight consecutive frames (a full window turnover at any jitter phase)
	// after the window itself is full (4 frames of 2 samples). The
	// water-glint accumulator settles alongside on the same frames (its
	// snap-through +-16 chase converges within the same cap).
	Body *glint_body = bodies_.getptr("glint");
	int held = 0;
	int last = glare_occlusion_->get_brightness();
	for (int frame = 0; frame < p_max_frames; ++frame) {
		const Vector3 lift(0.0f, opennova::env::glare_coarse_start_lift(
				glare_occlusion_->get_frame_index()), 0.0f);
		const bool coarse_clear = _glare_ray_clear(cam_pos + lift, sun_dir,
				ray_length, Vector3());
		const bool visible_a = coarse_clear && _glare_ray_clear(cam_pos,
				sun_dir, ray_length, glare_occlusion_->get_ray_jitter_a());
		const bool visible_b = coarse_clear && _glare_ray_clear(cam_pos,
				sun_dir, ray_length, glare_occlusion_->get_ray_jitter_b());
		glare_occlusion_->tick(visible_a, visible_b, state.fog_level());
		if (glint_body != nullptr) {
			_advance_water_glint(state, cam_pos, sun_dir, forward,
					*glint_body);
		}
		const int brightness = glare_occlusion_->get_brightness();
		held = brightness == last ? held + 1 : 0;
		last = brightness;
		if (frame >= 3 && held >= 8) {
			break;
		}
	}
	return last;
}

Dictionary Celestial::get_diagnostics() const {
	Dictionary diag;
	Dictionary glare;
	if (glare_occlusion_.is_valid()) {
		glare["brightness"] = glare_occlusion_->get_brightness();
		glare["window"] = glare_occlusion_->get_window();
		glare["ray_length"] = glare_occlusion_->get_ray_length();
	}
	diag["glare_occlusion"] = glare;
	Dictionary glint;
	glint["brightness"] = water_glint_.brightness;
	glint["window"] = static_cast<int>(water_glint_.window);
	diag["water_glint"] = glint;
	Dictionary veil;
	veil["glare"] = sun_veil_glare_;
	veil["alpha"] = get_sun_veil_alpha();
	veil["stopdown"] = sun_veil_stopdown_;
	diag["sun_veil"] = veil;
	Dictionary bodies;
	for (const KeyValue<String, Body> &kv : bodies_) {
		Dictionary body;
		body["opacity"] = kv.value.last_opacity;
		body["visible"] = kv.value.model != nullptr &&
				kv.value.model->is_visible();
		bodies[kv.key] = body;
	}
	diag["bodies"] = bodies;
	diag["built"] = !bodies_.is_empty();
	return diag;
}

bool Celestial::_segment_clear(const Vector3 &p_from, const Vector3 &p_to) {
	if (terrain_data_.is_null() || !terrain_data_->is_loaded()) {
		return true;
	}
	const Vector3 hit = terrain_data_->raycast_terrain(p_from, p_to);
	return std::isnan(hit.x);
}

float Celestial::_advance_water_glint(
		const opennova::env::EnvironmentState &p_state,
		const Vector3 &p_cam_pos, const Vector3 &p_sun_dir,
		const Vector3 &p_forward, Body &p_body) {
	// [orig: update_sun_glare @ 0x5ad130, once per main scene render from
	// Terrain_RenderSceneWithReflection @ 0x5c96c0, see docs/env/env-tod-re.md]: one sample per frame —
	// the reflected-sun point on the water (with the 0.25 * (frame & 3)
	// reflected-height jitter and the +-2 point x/z jitter), visible when
	// the point sees BOTH the sun (point -> camera + sun * 2048) and the
	// camera over terrain (the entity ray keeps the documented sun-occlusion
	// statics posture, like the sky glow) — then the +-16 chase toward
	// popcount * 64 and the mirrored glare-model submit at camera +
	// sun * 128 with the height term negated. No water = no glint.
	if (p_body.model == nullptr) {
		return 0.0f;
	}
	if (!p_state.has_water_height() || p_state.water_height() == 0.0f) {
		p_body.model->set_visible(false);
		return 0.0f;
	}
	const opennova::env::Vec3 cam_m = godot_to_mission(p_cam_pos);
	const opennova::env::Vec3 sun_m = godot_to_mission(p_sun_dir);
	const float view_z_jitter =
			0.25f * static_cast<float>(water_glint_.frame_index & 3u);
	opennova::env::Vec3 point_m;
	bool visible = opennova::env::water_glint_point(cam_m, sun_m,
			p_state.water_height(), view_z_jitter, point_m);
	if (visible) {
		// The +-2 unit point jitter [orig: @ 0x5ad26a..0x5ad27e, see docs/env/env-tod-re.md] — mission
		// x (godot x) and mission z = height (godot y).
		point_m.x += (water_glint_.frame_index & 1u) ? 2.0f : -2.0f;
		point_m.z += (water_glint_.frame_index & 2u) ? 2.0f : -2.0f;
		const Vector3 point_g = mission_to_godot(point_m);
		visible = _segment_clear(point_g,
						  p_cam_pos + p_sun_dir * 2048.0f) &&
				_segment_clear(point_g, p_cam_pos);
	}
	opennova::env::water_glint_tick(water_glint_, visible);

	// Placement: camera + sun * 128 with the HEIGHT term negated (the
	// mirrored glint below the eye [orig: @ 0x5ad1ba..0x5ad213 — the float
	// matrix stores (-(camY + sunY*128), camZ - sunZ*128, camX + sunX*128),
	// the mission -> render-float map of exactly that mirrored point, see docs/env/env-tod-re.md]).
	const Vector3 mirrored(p_sun_dir.x, -p_sun_dir.y, p_sun_dir.z);
	p_body.model->set_global_position(p_cam_pos + mirrored * 128.0f);
	_set_body_parameter(p_body, "u_anchor_camera_world", p_cam_pos);
	_set_body_parameter(p_body, "u_tint", to_vector3(p_state.sun_color()));
	// Alpha: the view dot of the MIRRORED sun direction [orig: @ 0x5ad384
	// negates the height term before the view transform, see docs/env/env-tod-re.md] through the
	// witnessed (dot^4 - 28672/65536) x brightness chain.
	const int dot_fixed = static_cast<int>(
			p_forward.dot(mirrored) * 65536.0f);
	const float alpha = static_cast<float>(opennova::env::water_glint_alpha_fixed(
			dot_fixed, water_glint_.brightness,
			opennova::env::detail::to_fixed_16_16(p_state.sun_dim_pct()))) /
			65536.0f;
	_set_body_parameter(p_body, "u_opacity", alpha);
	p_body.model->set_visible(alpha > 0.0f && water_glint_.brightness > 0);
	return alpha;
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
