#include "env/nova_water.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/canvas_item_material.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/viewport.hpp>

#include "env/env_render_camera.h"
#include "env/nova_mission_environment.h"
#include "env/nova_weather.h"
#include "render/nova_framefx.h"
#include "object/nova_object_shader_cache.h"

#include <renderer/render_order.h>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

void Water::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_environment_path", "path"),
			&Water::set_environment_path);
	ClassDB::bind_method(D_METHOD("get_environment_path"),
			&Water::get_environment_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "environment_path"),
			"set_environment_path", "get_environment_path");
	ClassDB::bind_method(D_METHOD("set_weather_path", "path"),
			&Water::set_weather_path);
	ClassDB::bind_method(D_METHOD("get_weather_path"), &Water::get_weather_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "weather_path"),
			"set_weather_path", "get_weather_path");
	ClassDB::bind_method(D_METHOD("set_terrain_data", "data"),
			&Water::set_terrain_data);
	ClassDB::bind_method(D_METHOD("get_terrain_data"), &Water::get_terrain_data);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_data",
						 PROPERTY_HINT_RESOURCE_TYPE, "TerrainData"),
			"set_terrain_data", "get_terrain_data");
	ClassDB::bind_method(D_METHOD("set_water_height", "value"),
			&Water::set_water_height);
	ClassDB::bind_method(D_METHOD("get_water_height"), &Water::get_water_height);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "water_height",
						 PROPERTY_HINT_RANGE, "-100,200,0.1"),
			"set_water_height", "get_water_height");
	ClassDB::bind_method(D_METHOD("set_water_alpha", "value"),
			&Water::set_water_alpha);
	ClassDB::bind_method(D_METHOD("get_water_alpha"), &Water::get_water_alpha);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "water_alpha",
						 PROPERTY_HINT_RANGE, "0,1,0.01"),
			"set_water_alpha", "get_water_alpha");

	ClassDB::bind_method(D_METHOD("set_mission_water_height_override", "value"),
			&Water::set_mission_water_height_override);
	ClassDB::bind_method(D_METHOD("set_world_rendering_enabled", "value"),
			&Water::set_world_rendering_enabled);
	ClassDB::bind_method(D_METHOD("release_runtime_renderer_resources"),
			&Water::release_runtime_renderer_resources);
	ClassDB::bind_method(D_METHOD("is_water_active"), &Water::is_water_active);
	ClassDB::bind_method(D_METHOD("is_water_render_active"),
			&Water::is_water_render_active);
	ClassDB::bind_method(D_METHOD("build"), &Water::build);
	ClassDB::bind_method(D_METHOD("is_built"), &Water::is_built);
	ClassDB::bind_method(D_METHOD("get_water_material"),
			&Water::get_water_material);
	ClassDB::bind_method(D_METHOD("get_noise_color_texture"),
			&Water::get_noise_color_texture);
	ClassDB::bind_method(D_METHOD("get_mesh_instance"),
			&Water::get_mesh_instance);
	ClassDB::bind_method(D_METHOD("get_reflection_viewport"),
			&Water::get_reflection_viewport);
	ClassDB::bind_method(D_METHOD("get_reflection_camera"),
			&Water::get_reflection_camera);
	// The externally-callable render-frame drive (the _process body): the
	// test harness drives frames here; the engine's virtual delegates in.
	ClassDB::bind_method(D_METHOD("advance_frame", "delta"),
			&Water::advance_frame);
	ClassDB::bind_method(D_METHOD("set_visible_terrain_bounds", "valid",
								"min_height", "max_height"),
			&Water::set_visible_terrain_bounds);
	ClassDB::bind_method(D_METHOD("set_blink_water_visible", "visible"),
			&Water::set_blink_water_visible);
	ClassDB::bind_method(D_METHOD("is_water_pass_active"),
			&Water::is_water_pass_active);

	ClassDB::bind_integer_constant(get_class_static(), "",
			"VISUAL_LAYER_WORLD", VISUAL_LAYER_WORLD);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"VISUAL_LAYER_WATER", VISUAL_LAYER_WATER);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"VISUAL_LAYER_ENVIRONMENT_CAPTURE",
			VISUAL_LAYER_ENVIRONMENT_CAPTURE);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"VISUAL_LAYER_VIEWMODEL", VISUAL_LAYER_VIEWMODEL);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"VISUAL_LAYER_FP_BODY_SHADOW_ONLY", VISUAL_LAYER_FP_BODY_SHADOW_ONLY);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"VISUAL_LAYER_STATIC_SHADOW_CASTER", VISUAL_LAYER_STATIC_SHADOW_CASTER);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"VISUAL_LAYER_DYNAMIC_SHADOW_CASTER", VISUAL_LAYER_DYNAMIC_SHADOW_CASTER);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"VISUAL_LAYER_TERRAIN_SHADOW_RECEIVER",
			VISUAL_LAYER_TERRAIN_SHADOW_RECEIVER);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"VISUAL_LAYER_WORLD_NO_MIRROR", VISUAL_LAYER_WORLD_NO_MIRROR);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"VISUAL_LAYER_SHADOW_CASTER_MASK", VISUAL_LAYER_SHADOW_CASTER_MASK);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"VISUAL_LAYER_SLOT_CAPTURE_MASK", VISUAL_LAYER_SLOT_CAPTURE_MASK);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"REFLECTION_CULL_MASK", REFLECTION_CULL_MASK);
}

void Water::set_environment_path(const NodePath &p_path) {
	environment_path_ = p_path;
	env_node_id_ = ObjectID();
}

void Water::set_weather_path(const NodePath &p_path) {
	weather_path_ = p_path;
	weather_node_id_ = ObjectID();
}

MissionEnvironment *Water::_env_node() {
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

Weather *Water::_weather_node() {
	if (weather_node_id_.is_valid()) {
		Weather *weather = Object::cast_to<Weather>(
				ObjectDB::get_instance(weather_node_id_));
		if (weather != nullptr && weather->is_inside_tree()) {
			return weather;
		}
	}
	if (weather_path_.is_empty() ||
			(!is_inside_tree() && weather_path_.is_absolute())) {
		return nullptr;
	}
	Weather *weather =
			Object::cast_to<Weather>(get_node_or_null(weather_path_));
	weather_node_id_ = weather != nullptr ? ObjectID(weather->get_instance_id())
										  : ObjectID();
	return weather;
}

void Water::set_terrain_data(const Ref<TerrainData> &p_data) {
	terrain_data_ = p_data;
	// Terrain carries the map's water height. It's assigned after the .trn
	// loads — long after _ready — so recompute here too, or the plane stays
	// at the scene default instead of dropping to the map's level. A present
	// terrain height beats the .env one (witnessed precedence, env #28).
	_recompute_terrain_water_fallback();
	_apply_environment_water_height();
}

void Water::set_water_height(float p_value) {
	water_height_ = p_value;
	// The strip vertices carry the plane height themselves (the mesh node
	// stays pinned at the world origin) — no node repositioning here.
	_sync_render_activity();
}

void Water::set_mission_water_height_override(float p_value) {
	mission_water_height_override_ = p_value;
	_apply_environment_water_height();
}

void Water::set_world_rendering_enabled(bool p_value) {
	world_rendering_enabled_ = p_value;
	_sync_render_activity();
}

void Water::release_runtime_renderer_resources() {
	set_process(false);
	world_rendering_enabled_ = false;
	RenderingServer::get_singleton()->global_shader_parameter_set(
			"opennova_water_active", false);
	RenderingServer::get_singleton()->global_shader_parameter_set(
			"opennova_water_reflection_clip_active", false);
	has_drawable_surface_ = false;
	if (reflection_viewport_ != nullptr) {
		reflection_viewport_->set_update_mode(SubViewport::UPDATE_DISABLED);
	}
	if (reflection_camera_ != nullptr) {
		reflection_camera_->clear_current();
	}
	if (water_material_.is_valid()) {
		water_material_->set_shader_parameter("u_has_reflection", false);
		water_material_->set_shader_parameter("u_reflection", Variant());
		water_material_->set_shader_parameter("u_noise_color", Variant());
		water_material_->set_shader_parameter("u_noise_normal", Variant());
	}
	if (mesh_instance_ != nullptr) {
		Ref<ArrayMesh> mesh = mesh_instance_->get_mesh();
		if (mesh.is_valid()) {
			mesh->clear_surfaces();
		}
		mesh_instance_->set_mesh(Ref<Mesh>());
		memdelete(mesh_instance_);
		mesh_instance_ = nullptr;
	}
	if (reflection_viewport_ != nullptr) {
		memdelete(reflection_viewport_);
		reflection_viewport_ = nullptr;
		reflection_camera_ = nullptr;
	}
	noise_color_tex_.unref();
	noise_normal_tex_.unref();
	noise_color_img_.unref();
	noise_normal_img_.unref();
	water_material_.unref();
	cached_cam_id_ = ObjectID();
	built_ = false;
}

// Publish this water plane's height as the session's transparent water-split
// (the g_WaterSplitHeightFloat equivalent; the witness rides the
// render-order record) so
// blended world materials can take their far/camera-side rung; cleared when
// the water node leaves the tree.
void Water::_push_water_split_height() {
	if (!built_ || !is_inside_tree()) {
		return;
	}
	const bool active = is_water_pass_active() && is_visible_in_tree();
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->global_shader_parameter_set("opennova_water_active", active);
	rs->global_shader_parameter_set("opennova_water_height", water_height_);
	if (!active) {
		rs->global_shader_parameter_set(
				"opennova_water_reflection_clip_active", false);
	}
	if (active) {
		Camera3D *cam = Object::cast_to<Camera3D>(
				ObjectDB::get_instance(cached_cam_id_));
		const bool camera_above = cam == nullptr ||
				cam->get_camera_transform().get_origin().y >= water_height_;
		ObjectShaderCache::get_singleton()->set_water_plane(
				water_height_, camera_above);
	} else {
		ObjectShaderCache::get_singleton()->clear_water_plane();
	}
}

void Water::set_visible_terrain_bounds(bool p_valid, float p_min_height,
		float p_max_height) {
	terrain_bounds_valid_ = p_valid;
	terrain_min_height_ = p_min_height;
	terrain_max_height_ = p_max_height;
}

void Water::set_blink_water_visible(bool p_visible) {
	blink_water_visible_ = p_visible;
}

void Water::_sync_render_activity() {
	if (!built_) {
		return;
	}
	const bool world_active =
			is_water_pass_active() && is_inside_tree() && is_visible_in_tree();
	if (mesh_instance_ != nullptr) {
		mesh_instance_->set_visible(world_active);
	}
	// Do not spend a permanent UPDATE_ALWAYS pass on an absent, hidden, or
	// off-screen surface. The strip march re-arms this after it produces
	// rows.
	const bool reflection_active = world_active && has_drawable_surface_ &&
			cached_cam_id_.is_valid();
	if (reflection_viewport_ != nullptr) {
		reflection_viewport_->set_update_mode(reflection_active
						? SubViewport::UPDATE_ALWAYS
						: SubViewport::UPDATE_DISABLED);
	}
	if (water_material_.is_valid()) {
		water_material_->set_shader_parameter("u_has_reflection",
				reflection_active);
	}
	_push_water_split_height();
	if (!world_active) {
		_clear_strip_surfaces();
	}
}

void Water::_notification(int p_what) {
	if (p_what == NOTIFICATION_VISIBILITY_CHANGED && built_) {
		_sync_render_activity();
	}
}

void Water::_exit_tree() {
	// Only clear a split that could have been pushed (_push_water_split_height
	// requires `built`): an unconditional call here CREATED the shader-cache
	// singleton during scene teardown on every quit — the never-freed
	// extension object behind the packaging boot-smoke teardown AV.
	if (built_) {
		ObjectShaderCache::get_singleton()->clear_water_plane();
		RenderingServer::get_singleton()->global_shader_parameter_set(
				"opennova_water_active", false);
		RenderingServer::get_singleton()->global_shader_parameter_set(
				"opennova_water_reflection_clip_active", false);
	}
	// Reflection teardown: stop the offscreen renders and disarm the shader's
	// reflection branch — the u_water_color fallback takes over if the
	// material outlives the node. Re-armed by the next build().
	if (reflection_viewport_ != nullptr) {
		reflection_viewport_->set_update_mode(SubViewport::UPDATE_DISABLED);
	}
	if (water_material_.is_valid()) {
		water_material_->set_shader_parameter("u_has_reflection", false);
		water_material_->set_shader_parameter("u_reflection", Variant());
	}
}

void Water::_ready() {
	set_process(true);
	if (water_core_.is_null()) {
		water_core_.instantiate();
	}
	_recompute_terrain_water_fallback();
	_apply_environment_water_height();
	build();
}

// The map's water height (engine half-world units) from the loaded terrain
// (water_frame.h carries the witness/precedence cites).
void Water::_recompute_terrain_water_fallback() {
	terrain_water_height_ = 0.0f;
	if (terrain_data_.is_valid() && terrain_data_->is_loaded()) {
		const float raw = terrain_data_->get_water_height();
		if (raw != 0.0f) {
			terrain_water_height_ = raw * 0.5f;
		}
	}
}

void Water::_apply_environment_water_height() {
	MissionEnvironment *env = _env_node();
	opennova::env::WaterHeightRungs rungs;
	rungs.has_mission_override = !std::isnan(mission_water_height_override_);
	rungs.mission_override = rungs.has_mission_override
			? mission_water_height_override_
			: 0.0f;
	rungs.terrain_height = terrain_water_height_;
	rungs.has_loaded_terrain =
			terrain_data_.is_valid() && terrain_data_->is_loaded();
	const float resolved = opennova::env::resolve_water_height(rungs,
			env != nullptr ? &env->state() : nullptr, water_height_);
	if (resolved != water_height_) {
		set_water_height(resolved);
	}
}

void Water::build() {
	if (mesh_instance_ != nullptr) {
		mesh_instance_->queue_free();
		mesh_instance_ = nullptr;
	}
	built_ = false;
	if (water_core_.is_null()) {
		water_core_.instantiate();
	}
	water_material_.instantiate();
	Ref<Shader> shader = ResourceLoader::get_singleton()->load(
			"res://shaders/water.gdshader");
	water_material_->set_shader(shader);
	// The water surface draws between the two water-side transparent
	// brackets (the ladder lives in engine/runtime/renderer/render_order,
	// REN-3).
	water_material_->set_render_priority(renderer::kRungWater);

	// The witnessed screen-marched strip mesh is LIVE (env #29): every frame
	// rebuilds the surface from WaterCore.strip_build, so the mesh starts
	// empty. Remaining variants: the LOW tier (water detail <= 1 sin-table Y
	// displacement) and the nightvision redraw.
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh_instance_ = memnew(MeshInstance3D);
	mesh_instance_->set_mesh(mesh);
	// Strip vertices are ABSOLUTE world positions (the plane height rides
	// the rows, not the node): pin the mesh at the world origin; top_level
	// guards against a transformed parent node.
	mesh_instance_->set_as_top_level(true);
	mesh_instance_->set_position(Vector3());
	mesh_instance_->set_cast_shadows_setting(
			GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	mesh_instance_->set_gi_mode(GeometryInstance3D::GI_MODE_DISABLED);
	// The water surface rides its OWN visual layer (bit 10) instead of the
	// default bit 0: every normal view still renders the water while the
	// mirror camera masks this one bit out — the witnessed offscreen
	// prerender never draws the water surface itself (water_mirror.h).
	mesh_instance_->set_layer_mask(VISUAL_LAYER_WATER);
	add_child(mesh_instance_);
	built_ = true;

	// The witnessed per-frame noise texture pair (created once, updated per
	// frame; math behind the WaterCore binding).
	const int size = water_core_->get_texture_size();
	water_core_->update(0);
	noise_color_img_ = Image::create_from_data(size, size, false,
			Image::FORMAT_RGBA8, water_core_->get_color_rgba8());
	noise_normal_img_ = Image::create_from_data(size, size, false,
			Image::FORMAT_RGBA8, water_core_->get_normal_rgba8());
	noise_color_tex_ = ImageTexture::create_from_image(noise_color_img_);
	noise_normal_tex_ = ImageTexture::create_from_image(noise_normal_img_);
	water_material_->set_shader_parameter("u_noise_color", noise_color_tex_);
	water_material_->set_shader_parameter("u_noise_normal", noise_normal_tex_);

	// The reflection RTT (env #30): the mirrored scene renders offscreen and
	// the strip shader samples it as t2. The reimpl form is a SubViewport on
	// the SAME World3D with a mirrored camera; Godot renders SubViewports
	// ahead of the viewport that samples them, preserving the witnessed
	// prerender order (water_mirror.h carries the cites).
	if (reflection_viewport_ == nullptr) {
		reflection_viewport_ = memnew(SubViewport);
		reflection_viewport_->set_name("WaterReflectionViewport");
		// The mirror renders the LIVE world, not a copy.
		reflection_viewport_->set_use_own_world_3d(false);
		reflection_viewport_->set_handle_input_locally(false);
		// Retail renders through this square RTT, independent of display
		// size.
		reflection_viewport_->set_size(
				Vector2i(opennova::env::kReflectionRttSize,
						opennova::env::kReflectionRttSize));
		add_child(reflection_viewport_);
		reflection_camera_ = memnew(Camera3D);
		reflection_camera_->set_name("WaterReflectionCamera");
		// Every retail pass writes gamma-domain numeric values (no D3DSAMP_SRGBTEXTURE /
		// D3DRS_SRGBWRITEENABLE in the device sweeps; the mirror scene renders through
		// Water_RenderReflectedWorldScene @0x5c8510 into the Water_CreateReflectionRenderTarget
		// @0x5c08b0 RTT - docs/render/render-material-re.md Color pipeline, docs/env/env-tod-re.md
		// #30), so this RTT
		// needs exactly one display decode before Godot's sRGB output encode
		// for its stored bytes to be the retail gamma texels the water shader
		// samples raw - and the witnessed dim quad below multiplies those
		// BYTES. A decode-only terminal effect on the mirror camera (no Q3
		// source, so no FrameFX composite) provides it; the camera must not
		// inherit the beauty WorldEnvironment's chain. An HDR 2D target would
		// skip the encode but run the canvas dim in linear space (0x40/255
		// becomes ~0.05), which is the wrong domain for that multiply.
		Ref<FrameFxCompositorEffect> decode_effect;
		decode_effect.instantiate();
		Ref<Compositor> capture_compositor;
		capture_compositor.instantiate();
		TypedArray<Ref<CompositorEffect>> capture_effects;
		Ref<CompositorEffect> generic_decode = decode_effect;
		capture_effects.push_back(generic_decode);
		capture_compositor->set_compositor_effects(capture_effects);
		reflection_camera_->set_compositor(capture_compositor);
		// The witnessed mirror scene: sky/terrain/celestials/foliage plus the
		// flag-0x400 world population — vehicles by item type and records
		// whose BMS attribute authors Reflective. It has no water surface, FP
		// overlay, or player/person render leg. _update_reflection_camera
		// re-adds WORLD_NO_MIRROR below water, where the retail collectors
		// run unfiltered.
		reflection_camera_->set_cull_mask(REFLECTION_CULL_MASK);
		reflection_viewport_->add_child(reflection_camera_);
		reflection_camera_->make_current();
		// The witnessed post-scene dim: retail multiplies the finished mirror
		// RTT by 64/255 before the water shader ever samples it. This
		// viewport's canvas pass composites over its 3D scene, so a
		// full-target multiply ColorRect is the same one-quad structural
		// port (constant + witness map: env/water_mirror.h
		// kReflectionDimFactor; celestial-after-dim residual noted there).
		CanvasLayer *dim_layer = memnew(CanvasLayer);
		dim_layer->set_name("ReflectionDimLayer");
		reflection_viewport_->add_child(dim_layer);
		ColorRect *dim_rect = memnew(ColorRect);
		dim_rect->set_name("ReflectionDim");
		Ref<CanvasItemMaterial> dim_material;
		dim_material.instantiate();
		dim_material->set_blend_mode(CanvasItemMaterial::BLEND_MODE_MUL);
		dim_rect->set_material(dim_material);
		const float dim = opennova::env::kReflectionDimFactor;
		dim_rect->set_color(Color(dim, dim, dim, 1.0f));
		dim_rect->set_size(Vector2(
				static_cast<float>(opennova::env::kReflectionRttSize),
				static_cast<float>(opennova::env::kReflectionRttSize)));
		dim_layer->add_child(dim_rect);
	}
	reflection_viewport_->set_update_mode(SubViewport::UPDATE_DISABLED);
	// Hold the RTT in a named Ref: passing the get_texture() temporary
	// straight into the Variant argument crashed at scene instantiation
	// (godot-cpp rvalue-Ref conversion hazard).
	Ref<ViewportTexture> rtt = reflection_viewport_->get_texture();
	water_material_->set_shader_parameter("u_reflection", rtt);
	water_material_->set_shader_parameter("u_has_reflection", false);
	_sync_render_activity();
}

void Water::_process(double p_delta) {
	advance_frame(p_delta);
}

void Water::advance_frame(double p_delta) {
	if (!built_ || water_material_.is_null()) {
		return;
	}
	MissionEnvironment *env = _env_node();
	// Live environment edits can change the fallback height. A standalone
	// water node with no authoritative env/terrain keeps its direct property.
	if (env != nullptr && env->is_loaded()) {
		_apply_environment_water_height();
	}
	// The murk uniform feed stays for world/probe compatibility even though
	// the shader's murk role moved to the per-vertex COLOR.a (env #29).
	water_material_->set_shader_parameter("u_water_murk", water_alpha_);

	Camera3D *cam = Object::cast_to<Camera3D>(
			ObjectDB::get_instance(cached_cam_id_));
	if (cam == nullptr || !cam->is_inside_tree() || !cam->is_current()) {
		cam = find_env_render_camera(this);
		cached_cam_id_ = cam != nullptr ? ObjectID(cam->get_instance_id())
										: ObjectID();
	}
	// The witnessed per-frame gate: no visible terrain at or below the water
	// and no Blink-visible water last frame means no prerender, no noise
	// regeneration and no strip this frame.
	if (!is_water_pass_active() || !is_visible_in_tree() || cam == nullptr) {
		_clear_strip_surfaces();
		_sync_render_activity();
		return;
	}
	// Camera3D's public render transform includes h_offset/v_offset;
	// global_position does not. Classify and march from the same effective
	// eye the main viewport actually renders.
	const Vector3 cam_pos = cam->get_camera_transform().get_origin();

	// env #30: refresh the mirror camera before this frame's strip rebuild —
	// the SubViewport renders ahead of the main view, like the witnessed
	// prerender.
	_update_reflection_camera(cam);

	// Regenerate the animated noise pair once per rendered water frame;
	// unlike the fixed-62 Hz weather clock, this is explicitly render-driven.
	frame_counter_ += 1;
	water_core_->update(frame_counter_);
	const int size = water_core_->get_texture_size();
	noise_color_img_->set_data(size, size, false, Image::FORMAT_RGBA8,
			water_core_->get_color_rgba8());
	noise_normal_img_->set_data(size, size, false, Image::FORMAT_RGBA8,
			water_core_->get_normal_rgba8());
	noise_color_tex_->update(noise_color_img_);
	noise_normal_tex_->update(noise_normal_img_);

	// Strip inputs (engine derivation; defaults keep envless owners
	// marching).
	const opennova::env::WaterFrameInputs inputs =
			opennova::env::build_water_frame_inputs(
					env != nullptr ? &env->state() : nullptr, water_alpha_);
	float murk = inputs.murk;
	float fog_end = inputs.fog_end;
	Vector4 uv_state(1.0f, 0.2f, 0.0f, 0.0f);
	const Color lit(inputs.lit.r, inputs.lit.g, inputs.lit.b);
	Ref<EnvFile> env_data;
	if (inputs.env_loaded && env != nullptr) {
		env_data = env->get_environment_data();
		water_material_->set_shader_parameter("u_water_color",
				Vector3(inputs.lit.r, inputs.lit.g, inputs.lit.b));
		// The witnessed UV transform (scale/bias from the fog-distance INT
		// part, offsets from the layer-1 cloud accumulators + 32x camera).
		// The weather node owns the shared accumulators; standalone owners
		// tick the engine fallback core.
		Weather *weather = _weather_node();
		if (weather != nullptr) {
			uv_state = weather->get_water_uv_state(cam_pos.x, cam_pos.z,
					fog_end);
		} else {
			fallback_scroll_.advance(p_delta, env->state().sky_speed());
			const opennova::env::WaterUvState state =
					opennova::env::water_uv_state(
							fallback_scroll_.core.cloud_scroll,
							static_cast<float>(cam_pos.x),
							static_cast<float>(cam_pos.z), fog_end);
			uv_state = Vector4(state.scale, state.bias, state.offset_u,
					state.offset_v);
		}
		water_material_->set_shader_parameter("u_water_uv", uv_state);
		water_material_->set_shader_parameter("u_fog_color",
				env->get_scene_fog_color());
		water_material_->set_shader_parameter("u_water_murk", murk);
	}

	_rebuild_strip_mesh(cam, cam_pos, murk, fog_end, uv_state, lit, env_data);
	_sync_render_activity();
}

// Mirrors the live camera about the water plane into the reflection
// SubViewport. The witnessed mirror form, side-dependent collection filter,
// and horizontal-preserving square projection live in
// environment/water_mirror.h; this leg extracts the source camera, installs
// the typed record, and pushes the UV registration scale.
void Water::_update_reflection_camera(Camera3D *p_cam) {
	if (reflection_viewport_ == nullptr || reflection_camera_ == nullptr) {
		return;
	}
	if (p_cam == nullptr || !p_cam->is_inside_tree()) {
		// No live view: the strip build clears too — the stale mirror image
		// is never sampled.
		return;
	}
	Viewport *viewport = p_cam->get_viewport();
	if (viewport == nullptr) {
		return;
	}
	const Vector2 source_size = viewport->get_visible_rect().size;
	// A one-pixel viewport is a real transient state while ONED swaps or
	// lays out workspaces. The strip builder below already treats either
	// dimension <= 1 as non-drawable; stop the mirror projection here too,
	// before an extreme aspect asks Camera3D for an out-of-range FOV.
	if (source_size.x <= 1.0f || source_size.y <= 1.0f) {
		return;
	}

	const Transform3D xform = p_cam->get_global_transform();
	opennova::env::MirrorSourceView source;
	const auto to_vec3 = [](const Vector3 &v) {
		return opennova::env::Vec3{static_cast<float>(v.x),
				static_cast<float>(v.y), static_cast<float>(v.z)};
	};
	source.basis_x = to_vec3(xform.get_basis().get_column(0));
	source.basis_y = to_vec3(xform.get_basis().get_column(1));
	source.basis_z = to_vec3(xform.get_basis().get_column(2));
	source.origin = to_vec3(xform.get_origin());
	switch (p_cam->get_projection()) {
		case Camera3D::PROJECTION_ORTHOGONAL:
			source.projection = opennova::env::MirrorProjection::kOrthogonal;
			break;
		case Camera3D::PROJECTION_FRUSTUM:
			source.projection = opennova::env::MirrorProjection::kFrustum;
			break;
		default:
			source.projection = opennova::env::MirrorProjection::kPerspective;
			break;
	}
	source.fov_deg = p_cam->get_fov();
	source.ortho_size = p_cam->get_size();
	source.frustum_size = p_cam->get_size();
	source.frustum_offset_x = p_cam->get_frustum_offset().x;
	source.frustum_offset_y = p_cam->get_frustum_offset().y;
	source.keep_aspect_height =
			p_cam->get_keep_aspect_mode() == Camera3D::KEEP_HEIGHT;
	source.viewport_width = source_size.x;
	source.viewport_height = source_size.y;
	source.v_offset = p_cam->get_v_offset();

	const opennova::env::WaterMirrorView view =
			opennova::env::build_water_mirror_view(source, water_height_);
	const auto to_v3 = [](const opennova::env::Vec3 &v) {
		return Vector3(v.x, v.y, v.z);
	};
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->global_shader_parameter_set("opennova_water_reflection_eye",
			to_v3(view.origin));
	rs->global_shader_parameter_set("opennova_water_reflection_clip_active",
			!view.below_water);
	reflection_camera_->set_global_transform(Transform3D(
			Basis(to_v3(view.basis_x), to_v3(view.basis_y), to_v3(view.basis_z)),
			to_v3(view.origin)));
	reflection_camera_->set_cull_mask(view.below_water
					? (REFLECTION_CULL_MASK | VISUAL_LAYER_WORLD_NO_MIRROR)
					: REFLECTION_CULL_MASK);
	reflection_camera_->set_keep_aspect_mode(Camera3D::KEEP_WIDTH);
	switch (source.projection) {
		case opennova::env::MirrorProjection::kOrthogonal:
			reflection_camera_->set_orthogonal(view.horizontal_size,
					p_cam->get_near(), p_cam->get_far());
			break;
		case opennova::env::MirrorProjection::kFrustum:
			reflection_camera_->set_frustum(view.horizontal_size,
					Vector2(view.frustum_offset_x, view.frustum_offset_y),
					p_cam->get_near(), p_cam->get_far());
			break;
		case opennova::env::MirrorProjection::kPerspective:
		default:
			reflection_camera_->set_perspective(view.horizontal_fov_deg,
					p_cam->get_near(), p_cam->get_far());
			break;
	}
	// These offsets are independent of the projection mode and are otherwise
	// lost when the reflection camera is rebuilt from the source transform.
	reflection_camera_->set_h_offset(p_cam->get_h_offset());
	reflection_camera_->set_v_offset(view.v_offset);
	water_material_->set_shader_parameter("u_reflection_uv_scale",
			Vector2(view.uv_scale_x, view.uv_scale_y));
}

// Rebuilds the surface from the witnessed screen march (env #29; row layout
// notes ride env_water_render.h). Vertices are absolute world positions;
// COLOR carries the row diffuse, CUSTOM1 the row specular, CUSTOM0 =
// (depth, rhw, screen U, screen V), CUSTOM2 the texm3x2 perturbation basis,
// TEX_UV the witnessed world/32 pair (carried for parity/debug).
void Water::_rebuild_strip_mesh(Camera3D *p_cam, const Vector3 &p_cam_pos,
		float p_murk, float p_fog_end, const Vector4 &p_uv_state,
		const Color &p_lit, const Ref<EnvFile> &p_env_data) {
	if (mesh_instance_ == nullptr) {
		return;
	}
	Ref<ArrayMesh> mesh = mesh_instance_->get_mesh();
	if (mesh.is_null()) {
		return;
	}
	if (p_cam == nullptr || !p_cam->is_inside_tree()) {
		_clear_strip_surfaces();
		return;
	}
	Viewport *viewport = p_cam->get_viewport();
	if (viewport == nullptr) {
		_clear_strip_surfaces();
		return;
	}
	const Vector2i vp_size = Vector2i(viewport->get_visible_rect().size);
	if (vp_size.x <= 1 || vp_size.y <= 1) {
		_clear_strip_surfaces();
		return;
	}
	// The camera-side gate and the pass fog end both ride the underwater
	// flag: above water the pass fog end is the smoothed fog distance
	// attenuated by the overcast blend (the native env curve); below the
	// surface the murk visibility curve replaces the weather fog distance.
	const bool underwater = p_cam_pos.y < water_height_;
	MissionEnvironment *env = _env_node();
	float pass_fog_end = EnvFile::fog_end_above_water(p_fog_end,
			env != nullptr ? env->get_overcast_blend() : 0.0f);
	if (underwater && p_env_data.is_valid()) {
		pass_fog_end = p_env_data->get_fog_end_underwater();
	}
	// The adjusted camera transform includes Camera3D h/v offsets, keeping
	// the screen-marched row coordinates registered to the actual main view.
	water_core_->strip_set_view(p_cam->get_camera_transform(),
			p_cam->get_camera_projection(), vp_size, pass_fog_end);
	// The nightvision redraw variant is a FrameFX pass, not ported yet.
	const int rows = water_core_->strip_build(water_height_, p_murk, p_lit,
			p_uv_state.x, p_uv_state.y, underwater, false);
	if (rows < 2) {
		// Plane off-screen or a sub-2-row march — nothing submits.
		_clear_strip_surfaces();
		return;
	}
	has_drawable_surface_ = true;
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = water_core_->strip_positions();
	arrays[Mesh::ARRAY_COLOR] = water_core_->strip_colors();
	arrays[Mesh::ARRAY_TEX_UV] = water_core_->strip_uv0();
	arrays[Mesh::ARRAY_CUSTOM0] = water_core_->strip_custom0();
	arrays[Mesh::ARRAY_CUSTOM1] = water_core_->strip_custom1();
	arrays[Mesh::ARRAY_CUSTOM2] = water_core_->strip_custom2();
	arrays[Mesh::ARRAY_INDEX] = water_core_->strip_indices();
	mesh->clear_surfaces();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, Array(),
			Dictionary(),
			(Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT) |
					(Mesh::ARRAY_CUSTOM_RGBA_FLOAT
							<< Mesh::ARRAY_FORMAT_CUSTOM1_SHIFT) |
					(Mesh::ARRAY_CUSTOM_RGBA_FLOAT
							<< Mesh::ARRAY_FORMAT_CUSTOM2_SHIFT));
	mesh->surface_set_material(0, water_material_);
	// The witnessed per-side material swap: camera-above -> the blend
	// material, underwater -> the opaque one — ported as the shader's
	// u_underwater_view branch.
	water_material_->set_shader_parameter("u_underwater_view", underwater);
}

void Water::_clear_strip_surfaces() {
	has_drawable_surface_ = false;
	if (mesh_instance_ == nullptr) {
		return;
	}
	Ref<ArrayMesh> mesh = mesh_instance_->get_mesh();
	if (mesh.is_valid() && mesh->get_surface_count() > 0) {
		mesh->clear_surfaces();
	}
}

} // namespace godot
