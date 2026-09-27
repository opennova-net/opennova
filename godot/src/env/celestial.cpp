#include "env/celestial.h"
#include "render/frame_fx.h"

#include <cmath>
#include <vector>

#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

#include <formats/threedi/threedi_panm.h>
#include <formats/threedi/threedi_panm_pose.h>
#include <runtime/renderer/render_order.h>

#include "util/axes.h"
#include "env/env_convert.h"
#include "env/env_render_camera.h"
#include "env/mission_environment.h"
#include "object/object_data.h"
#include "object/object_model.h"

namespace godot {

namespace {

// Whether every part of every LOD of the model poses view-aligned (PANM
// rotation type 3, celestial_frame.h): the stock sun, moon and glare models
// all do. A model with any other part keeps the identity render rotation.
bool panm_view_aligned(const opennova::threedi::Threedi3di3 &p_model) {
	if (p_model.lods == nullptr || p_model.lod_count == 0) {
		return false;
	}
	std::vector<opennova::threedi::ThreediPartAnimation> nodes;
	for (size_t lod_index = 0; lod_index < p_model.lod_count; ++lod_index) {
		const opennova::threedi::ThreediLod &lod = p_model.lods[lod_index];
		if (!opennova::threedi::threedi_panm_effective_for_lod(p_model,
					static_cast<int>(lod_index), nodes)) {
			return false;
		}
		std::vector<bool> posed(lod.render_object_count, false);
		for (const opennova::threedi::ThreediPartAnimation &node : nodes) {
			if (opennova::threedi::threedi_panm_rotation_type(node.flags) != 3) {
				return false;
			}
			if (node.subobject_index < posed.size()) {
				posed[node.subobject_index] = true;
			}
		}
		for (const bool part_posed : posed) {
			if (!part_posed) {
				return false;
			}
		}
	}
	return true;
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
	ClassDB::bind_method(D_METHOD("set_environment_capture_layer_mask", "mask"),
			&Celestial::set_environment_capture_layer_mask);
	ClassDB::bind_method(D_METHOD("get_environment_capture_layer_mask"),
			&Celestial::get_environment_capture_layer_mask);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "environment_capture_layer_mask",
			PROPERTY_HINT_LAYERS_3D_RENDER),
			"set_environment_capture_layer_mask",
			"get_environment_capture_layer_mask");
	ClassDB::bind_method(D_METHOD("set_sky_pass_gates", "beauty_drawn", "mirror_drawn"),
			&Celestial::set_sky_pass_gates);
	ClassDB::bind_method(D_METHOD("is_sky_beauty_pass_drawn"),
			&Celestial::is_sky_beauty_pass_drawn);
	ClassDB::bind_method(D_METHOD("is_sky_mirror_pass_drawn"),
			&Celestial::is_sky_mirror_pass_drawn);
	ClassDB::bind_method(D_METHOD("set_inset_view", "camera"), &Celestial::set_inset_view);
	ClassDB::bind_static_method("Celestial",
			D_METHOD("source_material_uses_additive", "source"),
			&Celestial::source_material_uses_additive);
	// The externally-callable render-frame drive: the
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
	ClassDB::bind_method(D_METHOD("get_overlay_body_node", "body"),
			&Celestial::get_overlay_body_node);
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

void Celestial::set_environment_capture_layer_mask(uint32_t p_mask) {
	environment_capture_layer_mask_ = p_mask;
	for (const String &key : { String("sun"), String("moon") }) {
		Body *body = bodies_.getptr(key);
		if (body != nullptr) {
			_stamp_environment_capture_layer(body->model);
		}
	}
}

void Celestial::set_sky_pass_gates(bool p_beauty_drawn, bool p_mirror_drawn) {
	if (p_beauty_drawn == sky_beauty_pass_drawn_ &&
			p_mirror_drawn == sky_mirror_pass_drawn_) {
		return;
	}
	sky_beauty_pass_drawn_ = p_beauty_drawn;
	sky_mirror_pass_drawn_ = p_mirror_drawn;
	_apply_sky_pass_gates();
}

void Celestial::_apply_sky_pass_gates() {
	for (const String &key : { String("sun"), String("moon") }) {
		const Body *body = bodies_.getptr(key);
		if (body == nullptr) {
			continue;
		}
		_set_body_parameter(*body, "u_sky_beauty_drawn", sky_beauty_pass_drawn_);
		_set_body_parameter(*body, "u_sky_mirror_drawn", sky_mirror_pass_drawn_);
	}
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
	if (!glare_occlusion_) {
		glare_occlusion_ = std::make_unique<GlareOcclusion>();
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
		int rung;
		opennova::renderer::Q3Source q3_source;
		bool q3_drawn;
	};
	// The witnessed load policy (celestial_frame.h carries the cites): the
	// sky bracket draws the discs BEFORE all world alpha; the glare is the
	// frame's final draw. Rungs are single-sourced from
	// engine/runtime/renderer/render_order (REN-3); the glow and the glint
	// draw in the post-particle overlay stage (their meshes leave every
	// camera), so they keep the default rung. The bloom pass redraws
	// the discs and the glow, never the glint (FrameFX_RenderGlowSource
	// @ 0x582a77 / @ 0x582a80).
	const Spec wanted[] = {
		{ "sun", env_data->get_sun_3di(), opennova::renderer::kRungSkyBody,
				opennova::renderer::Q3Source::CelestialBody, true },
		{ "moon", env_data->get_moon_3di(), opennova::renderer::kRungSkyBody,
				opennova::renderer::Q3Source::CelestialBody, true },
		{ "glare", env_data->get_glare_3di(), opennova::renderer::kRungAlphaCameraSide,
				opennova::renderer::Q3Source::SunGlow, true },
		// The water-reflected sun glint reuses the glare 3DI, mirrored below
		// the eye [orig: Environment_UpdateSunGlare @ 0x5ad130 submits
		// g_CelestialGlareModel at camera + sun * 128 with the height term
		// negated, flags 0x110, see docs/env/env-tod-re.md].
		{ "glint", env_data->get_glare_3di(), opennova::renderer::kRungAlphaCameraSide,
				opennova::renderer::Q3Source::SunGlow, false },
	};
	// Rebuild only when the set of names actually changed (undo/scrub safe).
	HashMap<String, String> signature;
	for (const Spec &spec : wanted) {
		signature[spec.key] = spec.name;
	}
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
		// The discs render INTO the water mirror (its sky bracket re-enters
		// Render_CelestialBodies) — keep the bodies on the mirror-visible
		// world layer, unlike the filtered world entities (the witness rides
		// the water mirror record); the glow and glint gate the mirror pass
		// out in their shader.
		model->set_mirror_reflected(true);
		// The sky bracket and the mirror's redraw submit outside the sector
		// walks, so no body ever takes the CLIP technique.
		model->set_water_mirror_clip_wave(opennova::env::MirrorClipWave::kNone);
		model->set_object_data(data);
		// Every celestial submit uses the IDENTITY world rotation in RENDER
		// axes (Render_CelestialBodies @ 0x5acaa0 sun/moon,
		// Render_SkyboxSunGlow @ 0x5acd00 + Environment_UpdateSunGlare @ 0x5ad130
		// glare/glint - docs/env/env-tod-re.md): the authored quads face
		// render +Z = EAST. ObjectData maps model (x, y, z) into Godot
		// (-x, y, z) (object_data_geometry.cpp godot_position), which
		// under an identity basis leaves the quad facing Godot +Z (south) -
		// edge-on at a sunrise/sunset pose, the "squashed oval sun". The
		// +90 degree yaw about +Y is the exact composition
		// R * import(v) == render_float_to_godot(v) (util/axes.h), restoring
		// the retail east-facing placement. Positive axis on purpose:
		// godot-cpp Basis(axis, angle) diverges from core for negative axes.
		// That is the model's world rotation; a view-aligned body's parts
		// then turn with the camera on top of it (advance_frame).
		model->set_basis(Basis(Vector3(0.0f, 1.0f, 0.0f),
				static_cast<real_t>(Math_PI) * 0.5f));
		// The body renders through its AUTHORED material (celestial_frame.h):
		// the model's own surface materials take only the sky-pass placement
		// hook (object/vertex_standard.gdshaderinc u_sky_*) and the fixed
		// frame rung.
		model->set_render_rung_override(spec.rung);
		Body body;
		body.model = model;
		body.disc = spec.key == "sun" || spec.key == "moon";
		body.view_aligned = panm_view_aligned(data->native_model());
		body.q3_source = spec.q3_source;
		body.q3_drawn = spec.q3_drawn;
		_bind_body_surfaces(body);
		bodies_[spec.key] = body;
	}
	_apply_sky_pass_gates();
}

void Celestial::_bind_body_surfaces(Body &p_body) {
	ObjectModel *model = p_body.model;
	p_body.meshes.clear();
	p_body.materials.clear();
	p_body.material_indices.clear();
	p_body.build_serial = model->get_scene_build_serial();
	_collect_meshes(model, p_body.meshes);
	const Array surface_materials = model->get_surface_materials();
	const PackedInt32Array surface_indices = model->get_surface_material_indices();
	for (MeshInstance3D *mesh_instance : p_body.meshes) {
		// The vertex stage relocates the body for the active render-pass
		// camera. Keep the source-camera AABB/occlusion result from
		// rejecting the mirror pass before that relocation reaches the GPU.
		mesh_instance->set_extra_cull_margin(1.0e6);
		mesh_instance->set_ignore_occlusion_culling(true);
		mesh_instance->set_cast_shadows_setting(
				GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		const Ref<ShaderMaterial> material = mesh_instance->get_material_override();
		int material_index = -1;
		for (int64_t i = 0; i < surface_materials.size(); ++i) {
			if (Ref<ShaderMaterial>(surface_materials[i]) == material) {
				material_index = surface_indices[i];
				break;
			}
		}
		p_body.materials.push_back(material);
		p_body.material_indices.push_back(material_index);
		// The Q3 redraw: the discs and the glow are the bloom pass's own
		// submits, drawn only for a glow-capable (LUM) material, blended as
		// that material is classified (the registry reads the
		// classification); the glint is never redrawn.
		opennova::renderer::ObjectMaterialClassification classification;
		const bool glow = p_body.q3_drawn && material.is_valid() &&
				FrameFx::q3_object_material_classification(material,
						classification) &&
				classification.is_glow_capable;
		if (glow) {
			FrameFx::register_q3_source(mesh_instance, p_body.q3_source);
		} else {
			FrameFx::unregister_q3_source(mesh_instance);
		}
	}
	_set_body_parameter(p_body, "u_sky_body", true);
	_set_body_parameter(p_body, "u_sky_far_pin", p_body.disc);
	if (p_body.disc) {
		_stamp_environment_capture_layer(model);
		_set_body_parameter(p_body, "u_sky_beauty_drawn", sky_beauty_pass_drawn_);
		_set_body_parameter(p_body, "u_sky_mirror_drawn", sky_mirror_pass_drawn_);
	} else {
		// The glow and the glint are main-frame draws: the water mirror's
		// scene never submits them in its base pass; the frame advance
		// opens the beauty pass when retail submits them.
		_set_body_parameter(p_body, "u_sky_mirror_drawn", false);
		_set_body_parameter(p_body, "u_sky_beauty_drawn", false);
	}
}

void Celestial::_rebind_rebuilt_bodies() {
	// A model scene rebuild (or a non-retained LOD swap) mints fresh surface
	// materials: bind them to the sky hook and the Q3 redraw again.
	for (KeyValue<String, Body> &kv : bodies_) {
		Body &body = kv.value;
		if (body.model != nullptr &&
				body.model->get_scene_build_serial() != body.build_serial) {
			_bind_body_surfaces(body);
		}
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

bool Celestial::source_material_uses_additive(const Ref<Material> &p_source) {
	opennova::renderer::ObjectMaterialClassification classification;
	return FrameFx::q3_object_material_classification(p_source, classification) &&
			classification.blend == opennova::renderer::ObjectBlendMode::Additive;
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

void Celestial::_stamp_environment_capture_layer(Node3D *p_model) {
	if (p_model == nullptr || environment_capture_layer_mask_ == 0) {
		return;
	}
	Vector<MeshInstance3D *> meshes;
	_collect_meshes(p_model, meshes);
	for (MeshInstance3D *mesh : meshes) {
		mesh->set_layer_mask(mesh->get_layer_mask() |
				environment_capture_layer_mask_);
	}
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

void Celestial::set_inset_view(Camera3D *p_camera) {
	inset_camera_id_ = p_camera != nullptr ? ObjectID(p_camera->get_instance_id()) : ObjectID();
}

void Celestial::advance_frame(double p_delta) {
	inset_glint_.model = nullptr;
	inset_glint_.drawn = false;
	inset_glint_.upl = 0;
	if (bodies_.is_empty()) {
		_rebuild_if_needed();
		if (bodies_.is_empty()) {
			_publish_idle_veil();
			return;
		}
	}
	_rebind_rebuilt_bodies();
	MissionEnvironment *env = _env_node();
	if (env == nullptr || !env->is_loaded()) {
		_publish_idle_veil();
		return;
	}
	const opennova::env::EnvironmentState &state = env->state();

	Camera3D *cam = _resolve_camera();
	const Vector3 cam_pos =
			cam != nullptr ? cam->get_global_position() : Vector3();
	body_anchor_ = cam_pos;

	// The GODOT-world sun direction and view forward (util/axes.h swap — the
	// 2026-08-20 correction: the earlier identity mapping placed every body
	// 90 degrees off in yaw and mirrored, the 03tr-sun-sky "sun rises in the
	// wrong place" half of the fixture's divergence).
	const Vector3 sun_dir = render_float_to_godot(state.sun_direction());
	const Vector3 forward = cam != nullptr
			? -cam->get_global_transform().basis.get_column(2).normalized()
			: Vector3(0.0f, 0.0f, -1.0f);
	// A view-aligned body's parts take the camera's inverse view rotation
	// (celestial_frame.h). The build's +90 degree yaw is that rotation for
	// the reference camera, level and looking east (Godot +X, columns right
	// +Z, up +Y, back -X); the live camera's rotation relative to the
	// reference composes ahead of it. The one turn serves every pass that
	// shares the camera's rotation (the beauty, the bloom redraw).
	if (cam != nullptr) {
		const Basis identity_render(Vector3(0.0f, 1.0f, 0.0f),
				static_cast<real_t>(Math_PI) * 0.5f);
		const Basis east_camera(Vector3(0.0f, 0.0f, 1.0f), Vector3(0.0f, 1.0f, 0.0f),
				Vector3(-1.0f, 0.0f, 0.0f));
		const Basis view_turn = cam->get_global_transform().basis.orthonormalized() *
				east_camera.transposed();
		for (KeyValue<String, Body> &kv : bodies_) {
			if (kv.value.view_aligned && kv.value.model != nullptr) {
				kv.value.model->set_basis(view_turn * identity_render);
			}
		}
	}
	// The frame builders run in the witnessed render-float axes; the camera
	// enters and the placement leaves through the util/axes.h swap.
	const opennova::env::Vec3 cam_rf = godot_to_render_float(cam_pos);
	const opennova::env::CelestialDiscsFrame discs =
			opennova::env::build_celestial_discs_frame(state, cam_rf);
	if (Body *sun = bodies_.getptr("sun")) {
		// camera + direction * 64, FULL camera height, identity rotation;
		// the far pin puts every world surface in front (celestial_frame.h).
		sun->model->set_global_position(render_float_to_godot(discs.sun_position));
		_set_body_parameter(*sun, "u_sky_anchor_camera", cam_pos);
		_set_body_upl(*sun, discs.sun_upl, discs.sun_q3_upl);
	}
	if (Body *moon = bodies_.getptr("moon")) {
		moon->model->set_global_position(render_float_to_godot(discs.moon_position));
		_set_body_parameter(*moon, "u_sky_anchor_camera", cam_pos);
		_set_body_upl(*moon, discs.moon_upl, discs.moon_q3_upl);
	}
	if (Body *glare = bodies_.getptr("glare")) {
		// env #14 (closed): the engine's glare ray sequence (coarse gate ray,
		// two jittered fine rays, the window tick) over this terrain's line
		// of sight (runtime/environment/glare_occlusion.h).
		_advance_glare_occlusion(state, cam_pos, sun_dir);
		const int view_dot_fixed = opennova::io::float_to_fp16_16(forward.dot(sun_dir));
		const opennova::env::GlareFrame frame = opennova::env::build_glare_frame(
				state, cam_rf, view_dot_fixed, glare_occlusion_->get_brightness());
		glare->model->set_global_position(render_float_to_godot(frame.position));
		_set_body_parameter(*glare, "u_sky_anchor_camera", cam_pos);
		_set_body_upl(*glare, frame.upl, frame.q3_upl);
		// A non-positive beauty alpha submits nothing; the bloom pass's own
		// alpha decides its redraw, which reads the node through the Q3
		// registry, so the node stays in the tree while either draws.
		glare->drawn = frame.drawn;
		_set_body_parameter(*glare, "u_sky_beauty_drawn", frame.drawn);
		glare->model->set_visible(frame.drawn || frame.q3_drawn);
	}
	if (Body *glint = bodies_.getptr("glint")) {
		// The water-reflected sun glint runs its own leg (accumulator,
		// mirrored placement, CPU alpha) [orig: Environment_UpdateSunGlare @ 0x5ad130,
		// see docs/env/env-tod-re.md].
		_advance_water_glint(state, cam_pos, sun_dir, forward, *glint);
		// The weapon Inset pass calls it again at its own camera, before the
		// frame's veil below reads the accumulator (env::WaterGlintState).
		_advance_inset_water_glint(state, cam_pos, sun_dir, *glint);
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
	if (bodies_.has("sun") && glare_occlusion_ != nullptr && cam != nullptr) {
		const int sun_dim_fixed =
				opennova::io::float_to_fp16_16(state.sun_dim_pct());
		const int overcast_fixed =
				opennova::io::float_to_fp16_16(state.overcast_blend());
		const int view_dot_fixed = opennova::io::float_to_fp16_16(forward.dot(sun_dir));
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
				const int dot2 = opennova::io::float_to_fp16_16(forward.dot(to_glint));
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

// The submit alpha lands in the model's UPL_INTENSITY register, which its
// authored RgbGen style 113 reads into SelfLumColor (celestial_frame.h); the
// bloom pass's redraw evaluates the same RgbGen at its own value, published
// to the Q3 source per surface.
void Celestial::_set_body_upl(Body &p_body, int32_t p_upl, int32_t p_q3_upl) {
	p_body.last_upl = p_upl;
	p_body.last_q3_upl = p_q3_upl;
	p_body.model->set_ctrl_override_native({}, opennova::env::kCelestialUplRegister,
			p_upl);
	const Ref<ObjectData> data = p_body.model->get_object_data();
	if (data.is_null()) {
		return;
	}
	opennova::renderer::ControlRegisterValues q3_registers{};
	q3_registers[static_cast<size_t>(opennova::env::kCelestialUplRegister)] = p_q3_upl;
	for (int i = 0; i < p_body.meshes.size(); ++i) {
		opennova::renderer::MaterialRuntime runtime;
		if (p_body.material_indices[i] < 0 ||
				!data->eval_material_runtime_native(p_body.material_indices[i], 0,
						q3_registers, runtime)) {
			continue;
		}
		FrameFx::set_q3_celestial_self_lum(p_body.meshes[i],
				Vector3(runtime.rgb_r, runtime.rgb_g, runtime.rgb_b));
	}
	_set_body_q3_pose(p_body, data, p_q3_upl);
}

// The node pose carries the beauty submit's register value; the bloom-pass
// redraw poses the parts at its own (celestial_frame.h GlareFrame), which
// moves the glare's UPL-driven part scales. Each surface keeps its offset
// from its part node, and only that part's pose is swapped.
void Celestial::_set_body_q3_pose(Body &p_body, const Ref<ObjectData> &p_data,
		int32_t p_q3_upl) {
	if (!p_body.q3_drawn) {
		return;
	}
	ObjectModel *model = p_body.model;
	Dictionary registers;
	registers[String(opennova::threedi::threedi_ctrl_register_name(
			static_cast<size_t>(opennova::env::kCelestialUplRegister)))] = p_q3_upl;
	const Dictionary pose = p_data->evaluate_panm(model->get_active_lod(),
			model->get_animation_time_ms(), registers);
	const Dictionary part_nodes = model->get_render_part_nodes();
	for (MeshInstance3D *mesh : p_body.meshes) {
		Transform3D q3_transform = mesh->get_global_transform();
		for (Node *node = mesh; node != nullptr && node != model; node = node->get_parent()) {
			Node3D *part = Object::cast_to<Node3D>(node);
			if (part == nullptr) {
				continue;
			}
			bool matched = false;
			const Array parts = part_nodes.keys();
			for (int64_t k = 0; k < parts.size(); ++k) {
				if (Object::cast_to<Node3D>(part_nodes[parts[k]]) != part) {
					continue;
				}
				Node3D *parent = part->get_parent_node_3d();
				if (parent != nullptr && pose.has(parts[k])) {
					const Transform3D surface_offset =
							part->get_global_transform().affine_inverse() *
							mesh->get_global_transform();
					q3_transform = parent->get_global_transform() *
							Transform3D(pose[parts[k]]) * surface_offset;
				}
				matched = true;
				break;
			}
			if (matched) {
				break;
			}
		}
		FrameFx::set_q3_celestial_pose(mesh, q3_transform);
	}
}

Celestial::OverlayBodies Celestial::get_overlay_bodies() const {
	const auto row = [this](const char *p_key) {
		OverlayBody out;
		const Body *body = bodies_.getptr(String(p_key));
		if (body != nullptr) {
			out.model = body->model;
			out.upl = body->last_upl;
			out.drawn = body->drawn;
		}
		return out;
	};
	OverlayBodies out;
	out.glare = row("glare");
	out.glint = row("glint");
	return out;
}

Celestial::MirrorRedraw Celestial::get_mirror_redraw(const Vector3 &p_mirror_forward) {
	MirrorRedraw out;
	out.anchor = body_anchor_;
	if (Body *sun = bodies_.getptr("sun")) {
		out.sun = sun->model;
	}
	if (Body *moon = bodies_.getptr("moon")) {
		out.moon = moon->model;
	}
	Body *glare = bodies_.getptr("glare");
	MissionEnvironment *env = _env_node();
	if (glare == nullptr || env == nullptr || !env->is_loaded()) {
		return out;
	}
	const opennova::env::EnvironmentState &state = env->state();
	const Vector3 sun_dir = render_float_to_godot(state.sun_direction());
	const int view_dot_fixed =
			opennova::io::float_to_fp16_16(p_mirror_forward.normalized().dot(sun_dir));
	const int32_t upl = opennova::env::mirror_glare_upl(state, view_dot_fixed);
	const Ref<ObjectData> data = glare->model->get_object_data();
	if (upl <= 0 || data.is_null()) {
		return out;
	}
	out.glare = glare->model;
	out.glare_drawn = true;
	opennova::renderer::ControlRegisterValues registers{};
	registers[static_cast<size_t>(opennova::env::kCelestialUplRegister)] = upl;
	for (int i = 0; i < glare->meshes.size(); ++i) {
		opennova::renderer::MaterialRuntime runtime;
		if (glare->material_indices[i] < 0 ||
				!data->eval_material_runtime_native(glare->material_indices[i], 0,
						registers, runtime)) {
			continue;
		}
		out.glare_self_lum[glare->meshes[i]] = { runtime.rgb_r, runtime.rgb_g, runtime.rgb_b };
	}
	return out;
}

Node3D *Celestial::get_overlay_body_node(const String &p_body) const {
	const OverlayBodies bodies = get_overlay_bodies();
	if (p_body == "glare") {
		return bodies.glare.model;
	}
	if (p_body == "glint") {
		return bodies.glint.model;
	}
	return nullptr;
}

int Celestial::settle_glare_occlusion(int p_max_frames) {
	if (!glare_occlusion_) {
		glare_occlusion_ = std::make_unique<GlareOcclusion>();
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
	// The dead-band step never snaps onto the target, so a settled
	// accumulator HOLDS: stop once the brightness has been unchanged across
	// eight consecutive frames (a full window turnover at any jitter phase)
	// after the window itself is full (4 frames of 2 samples). The
	// water-glint accumulator settles alongside on the same frames (its
	// snap-through +-16 chase converges within the same cap), each frame
	// stepping every scene pass's call the live frame makes: the main
	// scene's, then the weapon Inset pass's while one is set (set_inset_view).
	Body *glint_body = bodies_.getptr("glint");
	int held = 0;
	int last = glare_occlusion_->get_brightness();
	for (int frame = 0; frame < p_max_frames; ++frame) {
		_advance_glare_occlusion(state, cam_pos, sun_dir);
		if (glint_body != nullptr) {
			_advance_water_glint(state, cam_pos, sun_dir, forward,
					*glint_body);
			_advance_inset_water_glint(state, cam_pos, sun_dir, *glint_body);
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
	if (glare_occlusion_ != nullptr) {
		glare["brightness"] = glare_occlusion_->get_brightness();
		glare["window"] = glare_occlusion_->get_window();
		glare["ray_length"] = glare_occlusion_->get_ray_length();
	}
	diag["glare_occlusion"] = glare;
	Dictionary glint;
	glint["brightness"] = water_glint_.brightness;
	glint["window"] = static_cast<int>(water_glint_.window);
	glint["frame_index"] = static_cast<int64_t>(water_glint_.frame_index);
	// The weapon Inset pass's call this frame (set_inset_view).
	glint["inset_drawn"] = inset_glint_.drawn;
	glint["inset_upl"] = inset_glint_.upl;
	glint["inset_eye"] = inset_glint_.eye;
	diag["water_glint"] = glint;
	Dictionary veil;
	veil["glare"] = sun_veil_glare_;
	veil["alpha"] = get_sun_veil_alpha();
	veil["stopdown"] = sun_veil_stopdown_;
	diag["sun_veil"] = veil;
	Dictionary bodies;
	for (const KeyValue<String, Body> &kv : bodies_) {
		Dictionary body;
		body["upl"] = kv.value.last_upl;
		body["q3_upl"] = kv.value.last_q3_upl;
		body["drawn"] = kv.value.drawn;
		body["visible"] = kv.value.model != nullptr &&
				kv.value.model->is_visible();
		// How many of the model's live surface instances draw a material
		// that carries the sky hook (u_sky_body), of how many.
		int hooked = 0;
		int surfaces = 0;
		if (kv.value.model != nullptr) {
			Vector<MeshInstance3D *> meshes;
			_collect_meshes(kv.value.model, meshes);
			for (MeshInstance3D *mesh : meshes) {
				const Ref<ShaderMaterial> material = mesh->get_material_override();
				if (material.is_null() || !mesh->is_visible()) {
					continue;
				}
				++surfaces;
				if (bool(material->get_shader_parameter("u_sky_body"))) {
					++hooked;
				}
			}
		}
		body["sky_hooked_surfaces"] = hooked;
		body["surfaces"] = surfaces;
		bodies[kv.key] = body;
	}
	diag["bodies"] = bodies;
	diag["built"] = !bodies_.is_empty();
	return diag;
}

// Terrain line of sight between two Godot points, the glare and glint rays'
// shared form: the ported boolean raycast; TerrainData.raycast_terrain reports
// the miss as all-NAN, so clear = the hit is NAN. No terrain loaded = clear
// (nothing occludes) — the editor-guard divergence from retail's null-atlas
// return-HIT, kept deliberately: an unloaded world has nothing to block the
// sun (docs/terrain/terrain-re.md §Runtime terrain queries).
bool Celestial::_segment_clear(const Vector3 &p_from, const Vector3 &p_to) {
	if (terrain_data_.is_null() || !terrain_data_->is_loaded()) {
		return true;
	}
	const Vector3 hit = terrain_data_->raycast_terrain(p_from, p_to);
	return std::isnan(hit.x);
}

void Celestial::_advance_glare_occlusion(const opennova::env::EnvironmentState &p_state,
		const Vector3 &p_cam_pos, const Vector3 &p_sun_dir) {
	glare_occlusion_->advance(godot_to_mission(p_cam_pos), godot_to_mission(p_sun_dir),
			p_state.fog_level(),
			[this](const opennova::env::Vec3 &p_from, const opennova::env::Vec3 &p_to) {
				return _segment_clear(mission_to_godot(p_from), mission_to_godot(p_to));
			});
}

void Celestial::_advance_water_glint(
		const opennova::env::EnvironmentState &p_state,
		const Vector3 &p_cam_pos, const Vector3 &p_sun_dir,
		const Vector3 &p_forward, Body &p_body) {
	// The glint law is the engine's advance_water_glint (celestial_frame.h);
	// this leg owns the terrain rays and the model writes. No water = no glint.
	if (p_body.model == nullptr) {
		return;
	}
	if (!p_state.has_water_height() || p_state.water_height() == 0.0f) {
		p_body.drawn = false;
		p_body.model->set_visible(false);
		return;
	}
	const opennova::env::WaterGlintFrame frame = opennova::env::advance_water_glint(
			p_state, godot_to_mission(p_cam_pos), godot_to_mission(p_sun_dir),
			godot_to_mission(p_forward), water_glint_,
			[this](const opennova::env::Vec3 &a, const opennova::env::Vec3 &b) {
				return _segment_clear(mission_to_godot(a), mission_to_godot(b));
			});
	const Vector3 mirrored = mission_to_godot(frame.mirrored_sun);
	p_body.model->set_global_position(p_cam_pos + mirrored * 128.0f);
	_set_body_parameter(p_body, "u_sky_anchor_camera", p_cam_pos);
	_set_body_upl(p_body, frame.upl, 0);
	p_body.drawn = frame.drawn;
	_set_body_parameter(p_body, "u_sky_beauty_drawn", frame.drawn);
	p_body.model->set_visible(frame.drawn);
}

void Celestial::_advance_inset_water_glint(const opennova::env::EnvironmentState &p_state,
		const Vector3 &p_main_eye, const Vector3 &p_sun_dir, Body &p_body) {
	Camera3D *inset = Object::cast_to<Camera3D>(ObjectDB::get_instance(inset_camera_id_));
	if (inset == nullptr || !inset->is_inside_tree() || p_body.model == nullptr) {
		return;
	}
	// The same water test as the main call (no water = no glint).
	if (!p_state.has_water_height() || p_state.water_height() == 0.0f) {
		return;
	}
	const Transform3D eye = inset->get_global_transform();
	const Vector3 forward = -eye.basis.get_column(2).normalized();
	const opennova::env::WaterGlintFrame frame = opennova::env::advance_water_glint(
			p_state, godot_to_mission(eye.origin), godot_to_mission(p_sun_dir),
			godot_to_mission(forward), water_glint_,
			[this](const opennova::env::Vec3 &a, const opennova::env::Vec3 &b) {
				return _segment_clear(mission_to_godot(a), mission_to_godot(b));
			});
	inset_glint_.model = p_body.model;
	inset_glint_.drawn = frame.drawn;
	inset_glint_.upl = frame.upl;
	inset_glint_.eye = eye.origin;
	inset_glint_.forward = forward;
	// The main call placed the body at its eye + the mirrored sun * 128; the
	// Inset draws the same direction from its own eye.
	inset_glint_.offset = eye.origin - p_main_eye;
	inset_glint_.self_lum.clear();
	const Ref<ObjectData> data = p_body.model->get_object_data();
	if (data.is_null()) {
		return;
	}
	opennova::renderer::ControlRegisterValues registers{};
	registers[static_cast<size_t>(opennova::env::kCelestialUplRegister)] = frame.upl;
	for (int i = 0; i < p_body.meshes.size(); ++i) {
		opennova::renderer::MaterialRuntime runtime;
		if (p_body.material_indices[i] < 0 ||
				!data->eval_material_runtime_native(p_body.material_indices[i], 0, registers,
						runtime)) {
			continue;
		}
		inset_glint_.self_lum[p_body.meshes[i]] = { runtime.rgb_r, runtime.rgb_g, runtime.rgb_b };
	}
}

} // namespace godot
