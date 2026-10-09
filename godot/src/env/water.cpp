#include "env/water.h"
#include "render/frame_fx.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/viewport.hpp>

#include "env/env_render_camera.h"
#include "env/mission_environment.h"
#include "object/object_shader_cache.h"
#include "player/local_player_presenter.h"
#include "render/d3d9_raster_device.h"
#include "render/target_projection_xr_interface.h"
#include "world/game_world.h"

#include <runtime/renderer/render_order.h>
#include <runtime/terrain/quadtree.h> // water_pass_active (retail's g_WaterActive)
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
	ClassDB::bind_method(D_METHOD("get_water_alpha"), &Water::get_water_alpha);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "water_alpha",
						 PROPERTY_HINT_RANGE, "0,1,0.01"),
			"", "get_water_alpha");

	ClassDB::bind_method(D_METHOD("set_mission_water_height_override", "value"),
			&Water::set_mission_water_height_override);
	ClassDB::bind_method(D_METHOD("set_world_rendering_enabled", "value"),
			&Water::set_world_rendering_enabled);
	ClassDB::bind_method(D_METHOD("set_mirror_enabled", "value"), &Water::set_mirror_enabled);
	ClassDB::bind_method(D_METHOD("is_mirror_enabled"), &Water::is_mirror_enabled);
	ClassDB::bind_method(D_METHOD("is_globals_held"), &Water::is_globals_held);
	ClassDB::bind_static_method("Water", D_METHOD("get_global_writes"), &Water::get_global_writes);
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
	ClassDB::bind_method(D_METHOD("get_night_vision_mesh_instance"),
			&Water::get_night_vision_mesh_instance);
	ClassDB::bind_method(D_METHOD("get_reflection_viewport"),
			&Water::get_reflection_viewport);
	ClassDB::bind_method(D_METHOD("get_reflection_camera"),
			&Water::get_reflection_camera);
	// The externally-callable render-frame drive: the
	// test harness drives frames here; the engine's virtual delegates in.
	ClassDB::bind_method(D_METHOD("advance_frame", "delta"),
			&Water::advance_frame);
	ClassDB::bind_method(D_METHOD("set_visible_terrain_bounds", "valid",
								"min_height", "max_height"),
			&Water::set_visible_terrain_bounds);
	ClassDB::bind_method(D_METHOD("set_blink_water_visible", "visible"),
			&Water::set_blink_water_visible);
	ClassDB::bind_method(D_METHOD("set_blink_water_views", "main", "inset"),
			&Water::set_blink_water_views);
	ClassDB::bind_method(D_METHOD("set_noise_frame_counter", "counter"),
			&Water::set_noise_frame_counter);
	ClassDB::bind_method(D_METHOD("get_noise_frame_counter"),
			&Water::get_noise_frame_counter);
	ClassDB::bind_method(D_METHOD("set_mirror_scene_outdoors", "outdoors"),
			&Water::set_mirror_scene_outdoors);
	ClassDB::bind_method(D_METHOD("is_mirror_scene_outdoors"),
			&Water::is_mirror_scene_outdoors);
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
			"VISUAL_LAYER_TERRAIN_FOLIAGE", VISUAL_LAYER_TERRAIN_FOLIAGE);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"VISUAL_LAYER_TERRAIN_FLAT_FALLBACK",
			VISUAL_LAYER_TERRAIN_FLAT_FALLBACK);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"VISUAL_LAYER_SHADOW_CASTER_MASK", VISUAL_LAYER_SHADOW_CASTER_MASK);
	ClassDB::bind_integer_constant(get_class_static(), "",
			"REFLECTION_CULL_MASK", REFLECTION_CULL_MASK);
}

void Water::set_environment_path(const NodePath &p_path) {
	environment_path_ = p_path;
	env_node_id_ = ObjectID();
}

MissionEnvironment *Water::_env_node() {
	return resolve_cached_node<MissionEnvironment>(*this, environment_path_, env_node_id_);
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

void Water::set_mirror_enabled(bool p_value) {
	if (mirror_enabled_ == p_value) {
		return;
	}
	mirror_enabled_ = p_value;
	_sync_render_activity();
}

int64_t Water::global_writes_ = 0;

void Water::publish_absent() {
	++global_writes_;
	RenderingServer::get_singleton()->global_shader_parameter_set("opennova_water_active", false);
	RenderingServer::get_singleton()->global_shader_parameter_set("opennova_water_height", 0.0f);
	ObjectShaderCache::get_singleton()->clear_water_plane();
}

void Water::release_runtime_renderer_resources() {
	world_rendering_enabled_ = false;
	RenderingServer *server = RenderingServer::get_singleton();
	if (server != nullptr) {
		server->global_shader_parameter_set("opennova_water_active", false);
	}
	// The session's water split this node pushed leaves with it (the later
	// _exit_tree no longer clears it once built_ drops below): a stale plane
	// would keep ranking blended strips and arming the mirror's CLIP draws
	// for worlds that have no water.
	if (built_) {
		ObjectShaderCache::get_singleton()->clear_water_plane();
	}
	has_drawable_surface_ = false;
	if (reflection_viewport_ != nullptr) {
		reflection_viewport_->set_update_mode(SubViewport::UPDATE_DISABLED);
	}
	if (reflection_camera_ != nullptr) {
		reflection_camera_->clear_current();
	}
	_release_reflection_decode();
	if (water_material_.is_valid()) {
		water_material_->set_shader_parameter("u_has_reflection", false);
		water_material_->set_shader_parameter("u_reflection", Variant());
		water_material_->set_shader_parameter("u_noise_color", Variant());
		water_material_->set_shader_parameter("u_noise_normal", Variant());
	}
	for (MeshInstance3D **strip : {&mesh_instance_, &night_vision_mesh_instance_}) {
		if (*strip == nullptr) {
			continue;
		}
		Ref<ArrayMesh> mesh = (*strip)->get_mesh();
		if (mesh.is_valid()) {
			mesh->clear_surfaces();
		}
		(*strip)->set_mesh(Ref<Mesh>());
		memdelete(*strip);
		*strip = nullptr;
	}
	if (reflection_viewport_ != nullptr) {
		TargetProjectionXrInterface::release(reflection_viewport_);
		memdelete(reflection_viewport_);
		reflection_viewport_ = nullptr;
		reflection_camera_ = nullptr;
	}
	reflection_environment_.unref();
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
	if (!built_ || !is_inside_tree() || globals_held_) {
		return;
	}
	const bool active = is_water_pass_active() && is_visible_in_tree();
	++global_writes_;
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->global_shader_parameter_set("opennova_water_active", active);
	rs->global_shader_parameter_set("opennova_water_height", water_height_);
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

bool Water::is_water_pass_active() const {
	return is_water_render_active() &&
			opennova::water_pass_active(terrain_bounds_valid_, terrain_min_height_,
					terrain_max_height_, water_height_, blink_water_visible_);
}

void Water::set_blink_water_visible(bool p_visible) {
	blink_water_visible_ = p_visible;
}

void Water::set_blink_water_views(bool p_main, bool p_inset) {
	set_visible(p_main || p_inset);
	if (mesh_instance_ == nullptr) {
		return;
	}
	const uint32_t layer = p_main == p_inset ? uint32_t(VISUAL_LAYER_WATER)
			: p_main ? uint32_t(visual_layers::MAIN_VIEW_WATER)
					 : uint32_t(visual_layers::INSET_VIEW);
	if (mesh_instance_->get_layer_mask() != layer) {
		mesh_instance_->set_layer_mask(layer);
	}
}

void Water::set_noise_frame_counter(uint32_t p_counter) {
	frame_counter_ = static_cast<int>(p_counter);
	frame_counter_fed_ = true;
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
			cached_cam_id_.is_valid() && mirror_enabled_;
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
	if (!is_water_render_active() || !is_inside_tree() || !is_visible_in_tree()) {
		_clear_night_vision_surfaces();
	}
}

void Water::_notification(int p_what) {
	if (p_what == NOTIFICATION_VISIBILITY_CHANGED && built_) {
		_sync_render_activity();
	} else if (p_what == NOTIFICATION_ENTER_TREE && built_ &&
			reflection_camera_ != nullptr &&
			reflection_decode_effect_.is_null()) {
		// Re-entry after an EXIT_TREE release: the retained mirror camera needs
		// a fresh decode effect (the released one stays shut down).
		_install_reflection_decode();
	}
}

// The mirror camera's decode-only terminal effect: one FrameFxCompositorEffect
// on a compositor the camera owns (never the beauty WorldEnvironment's chain).
void Water::_install_reflection_decode() {
	if (reflection_camera_ == nullptr) {
		return;
	}
	reflection_decode_effect_.instantiate();
	reflection_compositor_.instantiate();
	TypedArray<Ref<CompositorEffect>> capture_effects;
	Ref<CompositorEffect> generic_decode = reflection_decode_effect_;
	capture_effects.push_back(generic_decode);
	reflection_compositor_->set_compositor_effects(capture_effects);
	reflection_camera_->set_compositor(reflection_compositor_);
}

// Idempotent release of the mirror decode chain, the same EXIT_TREE leg
// FrameFx runs: disable the effect, detach the mirror camera's compositor,
// drain a callback already queued for the mirror while RenderingDevice is
// live, then free the effect-owned device resources. A Water freed outside
// release_runtime_renderer_resources() (GUT fixtures, embedder previews)
// otherwise leaks its RD chain.
void Water::_release_reflection_decode() {
	Ref<FrameFxCompositorEffect> decode_effect = reflection_decode_effect_;
	if (decode_effect.is_valid())
		decode_effect->set_enabled(false);
	if (reflection_camera_ != nullptr &&
			reflection_camera_->get_compositor() == reflection_compositor_)
		reflection_camera_->set_compositor(Ref<Compositor>());
	if (reflection_compositor_.is_valid())
		reflection_compositor_->set_compositor_effects(
				TypedArray<Ref<CompositorEffect>>());
	RenderingServer *server = RenderingServer::get_singleton();
	if (decode_effect.is_valid() && server != nullptr &&
			server->get_rendering_device() != nullptr)
		server->force_sync();
	if (decode_effect.is_valid())
		decode_effect->release_device_resources();
	reflection_decode_effect_.unref();
	reflection_compositor_.unref();
	decode_effect.unref();
}

void Water::_exit_tree() {
	// Only clear a split that could have been pushed (_push_water_split_height
	// requires `built`): an unconditional call here CREATED the shader-cache
	// singleton during scene teardown on every quit — the never-freed
	// extension object behind the packaging boot-smoke teardown AV.
	if (built_ && !globals_held_) {
		++global_writes_;
		ObjectShaderCache::get_singleton()->clear_water_plane();
		RenderingServer::get_singleton()->global_shader_parameter_set(
				"opennova_water_active", false);
	}
	// Reflection teardown: stop the offscreen renders and disarm the shader's
	// reflection branch — the u_water_color fallback takes over if the
	// material outlives the node. Re-armed by the next build(); the next
	// mirror update serves the raster again.
	if (reflection_viewport_ != nullptr) {
		reflection_viewport_->set_update_mode(SubViewport::UPDATE_DISABLED);
		TargetProjectionXrInterface::release(reflection_viewport_);
	}
	if (water_material_.is_valid()) {
		water_material_->set_shader_parameter("u_has_reflection", false);
		water_material_->set_shader_parameter("u_reflection", Variant());
	}
	// The mirror decode chain never outlives the node's time in the tree;
	// ENTER_TREE re-installs it on the retained camera.
	_release_reflection_decode();
}

void Water::_ready() {
	if (!water_core_) {
		water_core_ = std::make_unique<WaterCore>();
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
			terrain_water_height_ = raw * opennova::env::kWaterHeightUnit;
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
	for (MeshInstance3D **strip : {&mesh_instance_, &night_vision_mesh_instance_}) {
		if (*strip != nullptr) {
			(*strip)->queue_free();
			*strip = nullptr;
		}
	}
	built_ = false;
	if (!water_core_) {
		water_core_ = std::make_unique<WaterCore>();
	}
	water_material_.instantiate();
	Ref<Shader> shader = ResourceLoader::get_singleton()->load(
			"res://shaders/water.gdshader");
	water_material_->set_shader(shader);
	// The water surface draws between the two water-side transparent
	// brackets (the ladder lives in engine/runtime/renderer/render_order,
	// REN-3).
	water_material_->set_render_priority(opennova::renderer::kRungWater);

	// The witnessed screen-marched strip mesh is LIVE (env #29): every frame
	// rebuilds the surface from WaterCore::strip_build, so the mesh starts
	// empty. The LOW tier (water detail <= 1 sin-table Y displacement) is
	// unreachable at the shipped detail (waterQuality clamps to [1, 3] and 3
	// runs the detailed tier).
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
	// The FrameFX bloom pass redraws the strip with the nightvision row
	// colors into the Q3 target (retail FrameFX_RenderGlowSource @ 0x582a59..
	// 0x582a5d -> Render_WaterSurface(0, 1)): its own surface, drawn by no
	// camera (layer mask 0), only by the typed Q3 WaterNightVision pass.
	Ref<ArrayMesh> night_vision_mesh;
	night_vision_mesh.instantiate();
	night_vision_mesh_instance_ = memnew(MeshInstance3D);
	night_vision_mesh_instance_->set_name("WaterNightVisionStrip");
	night_vision_mesh_instance_->set_mesh(night_vision_mesh);
	night_vision_mesh_instance_->set_as_top_level(true);
	night_vision_mesh_instance_->set_position(Vector3());
	night_vision_mesh_instance_->set_cast_shadows_setting(
			GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
	night_vision_mesh_instance_->set_gi_mode(GeometryInstance3D::GI_MODE_DISABLED);
	night_vision_mesh_instance_->set_layer_mask(0);
	add_child(night_vision_mesh_instance_);
	FrameFx::register_q3_source(night_vision_mesh_instance_,
			opennova::renderer::Q3Source::Water);
	built_ = true;

	// The witnessed per-frame noise texture pair (created once, updated per
	// frame; math behind the WaterCore device helper).
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
		// Retail's 512 x 512 RTT; each mirror update serves its raster the
		// main view's frustum (_update_reflection_camera), non-square texels.
		static_assert(opennova::env::kReflectionRttSize ==
						TargetProjectionXrInterface::kTargetSide,
				"the mirror RTT is the XR projection interface's square");
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
		_install_reflection_decode();
		// The witnessed mirror scene: sky/terrain/celestials plus the
		// flag-0x400 world population — vehicles by item type and records
		// whose BMS attribute authors Reflective. It has no water surface, FP
		// overlay, player/person render leg, foliage blanket, or empty-sector
		// flat terrain (the prerender view skips empty sectors whenever the
		// mission has water; this mirror only renders for a nonzero height,
		// see docs/terrain/terrain-re.md "Empty-sector flat fallback").
		// _update_reflection_camera re-adds WORLD_NO_MIRROR below water,
		// where the retail collectors run unfiltered.
		reflection_camera_->set_cull_mask(REFLECTION_CULL_MASK);
		reflection_viewport_->add_child(reflection_camera_);
		reflection_camera_->make_current();
		// The witnessed post-scene dim and the sun/moon/glow redraw after it
		// close the mirror target in its overlay pass, after the mirror's
		// particles and coronas (runtime/renderer/scene_overlay.h
		// kMirrorOverlayOrder; GameWorld's scene_overlay leg). The second
		// fullscreen quad retail draws after the far band
		// (g_WaterShaderAdditiveFlat, ONE/ONE of 0xFF000000, Render_MainScene
		// @ 0x5c190c..0x5c1990) only saturates the RTT alpha, which neither
		// water program reads, so the mirror draws no counterpart.
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

void Water::advance_frame(double) {
	if (!built_ || water_material_.is_null()) {
		return;
	}
	MissionEnvironment *env = _env_node();
	// Live environment edits can change the fallback height. A standalone
	// water node with no authoritative env/terrain keeps its direct property.
	if (env != nullptr && env->is_loaded()) {
		_apply_environment_water_height();
	}

	Camera3D *cam = Object::cast_to<Camera3D>(
			ObjectDB::get_instance(cached_cam_id_));
	if (cam == nullptr || !cam->is_inside_tree() || !cam->is_current()) {
		cam = find_env_render_camera(this);
		cached_cam_id_ = cam != nullptr ? ObjectID(cam->get_instance_id())
										: ObjectID();
	}
	if (!is_water_render_active() || !is_visible_in_tree() || cam == nullptr) {
		_clear_strip_surfaces();
		_clear_night_vision_surfaces();
		_sync_render_activity();
		return;
	}
	// The view the world is drawn through: while the local view presenter's
	// target is live the surface shows the TARGET's pixels stretched over it
	// (LocalPlayerPresenter::view_projection), so the strip march and the
	// mirror register to the target's camera, projection and raster; the
	// surface camera then carries only the frustum's culling superset.
	const DrawingView drawing = _drawing_view(cam);
	Camera3D *view_cam = drawing.camera;
	// Camera3D's public render transform includes h_offset/v_offset;
	// global_position does not. Classify and march from the same effective
	// eye the drawing viewport actually renders.
	const Vector3 cam_pos = view_cam->get_camera_transform().get_origin();
	// The two callers of Render_WaterSurface: the beauty pass per side, gated
	// on g_WaterActive (no visible terrain at or below the water and no
	// Blink-visible water last frame means no prerender and no strip), and the
	// FrameFX bloom pass's nightvision redraw, which is the above-water call
	// alone and not gated on g_WaterActive. Both skip an eye exactly on the
	// plane (environment/water_frame.h water_surface_sides).
	const opennova::env::WaterSurfaceSides sides =
			opennova::env::water_surface_sides(static_cast<float>(cam_pos.y),
					water_height_);
	const bool beauty_active =
			is_water_pass_active() && (sides.above || sides.underwater);
	const bool night_vision_active = sides.above;
	if (!beauty_active) {
		_clear_strip_surfaces();
	}
	if (!night_vision_active) {
		_clear_night_vision_surfaces();
	}
	if (!beauty_active && !night_vision_active) {
		_sync_render_activity();
		return;
	}

	// env #30: refresh the mirror camera before this frame's strip rebuild —
	// the SubViewport renders ahead of the main view, like the witnessed
	// prerender (itself gated on g_WaterActive).
	if (beauty_active) {
		_update_reflection_camera(view_cam, drawing.projection, drawing.frame_perspective);
	}

	// Regenerate the animated noise pair once per rendered water frame, at
	// the world's entity-update count when one is fed (set_noise_frame_counter),
	// else at this Water's own render-frame count. Both Render_WaterSurface
	// calls regenerate it at the same counter, so one update serves the frame.
	// [orig: Render_WaterSurface @0x5c3326 -> Water_GenerateNoiseTextures
	//  @0x5C0360, its counter @0x5C0366]
	if (!frame_counter_fed_) frame_counter_ += 1;
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
	// The strip's per-vertex depth curve from the smoothed fog distance's
	// integer word (environment/env_water_render.h carries the witness). No
	// texcoord transform exists: the noise pair samples the rows' absolute
	// world/32 texcoords.
	const opennova::env::WaterDepthCurve depth_curve =
			opennova::env::water_depth_curve(fog_end);
	const Color lit(inputs.lit.r, inputs.lit.g, inputs.lit.b);
	Ref<EnvFile> env_data;
	if (inputs.env_loaded && env != nullptr) {
		env_data = env->get_environment_data();
		water_material_->set_shader_parameter("u_water_color",
				Vector3(inputs.lit.r, inputs.lit.g, inputs.lit.b));
		water_material_->set_shader_parameter("u_fog_color",
				env->get_scene_fog_color());
	}

	if (beauty_active) {
		const int rows = _march_strip(drawing, sides.underwater, false, murk, fog_end,
				depth_curve, lit, env_data);
		if (rows < 2) {
			// Plane off-screen or a sub-2-row march — nothing submits.
			_clear_strip_surfaces();
		} else {
			has_drawable_surface_ = true;
			_upload_strip(mesh_instance_, _strip_arrays(), water_material_);
			// The witnessed per-side material swap: camera-above -> the blend
			// material, underwater -> the opaque one — ported as the shader's
			// u_underwater_view branch.
			water_material_->set_shader_parameter("u_underwater_view",
					sides.underwater);
		}
	}
	if (night_vision_active) {
		// The bloom pass's call is Render_WaterSurface(0, 1): the above-water
		// march with the nightvision row colors (flat 0.1 base, no specular
		// RGB) (retail FrameFX_RenderGlowSource @ 0x582a59..0x582a5d;
		// Render_WaterSurface @ 0x5c3489..0x5c3492).
		const int rows = _march_strip(drawing, false, true, murk, fog_end, depth_curve,
				lit, env_data);
		if (rows < 2) {
			_clear_night_vision_surfaces();
		} else {
			const Array arrays = _strip_arrays();
			_upload_strip(night_vision_mesh_instance_, arrays, water_material_);
			// Hand the arrays over so the focused Q3 cache re-packs them from
			// memory instead of reading the freshly uploaded surface back through
			// the server in the same frame.
			FrameFx::publish_q3_geometry(night_vision_mesh_instance_, 0, arrays);
		}
	}
	_sync_render_activity();
}

// The world (this node's parent) registers its local view presenter
// (GameWorld::local_view_presenter); a standalone water node has none and
// draws through its own viewport's camera.
Water::DrawingView Water::_drawing_view(Camera3D *p_surface_cam) const {
	DrawingView view;
	GameWorld *world = Object::cast_to<GameWorld>(get_parent());
	LocalPlayerPresenter *presenter = world != nullptr ? world->local_view_presenter() : nullptr;
	Camera3D *through = presenter != nullptr ? presenter->projection_camera() : nullptr;
	SubViewport *target = presenter != nullptr ? presenter->projection_viewport() : nullptr;
	// The strip march and the mirror take the original's own projection, the
	// presenter's screen projection with its D3D9 raster shift taken out: the
	// march runs in the original's window coordinates and the mirror rasterises
	// on its own RTT (env::WaterMirrorView::raster_shift).
	if (through != nullptr && through->is_inside_tree() && target != nullptr) {
		view.camera = through;
		view.projection = presenter->screen_projection();
		view.frame_perspective = true;
		// The target's raster: its node size, or the served square while the
		// NVG raster draws through TargetProjectionXrInterface.
		view.raster = target->get_size();
		return view;
	}
	view.camera = p_surface_cam;
	if (p_surface_cam != nullptr && p_surface_cam->is_inside_tree()) {
		if (presenter != nullptr && presenter->camera() == p_surface_cam) {
			view.projection = presenter->screen_projection();
			view.frame_perspective = true;
		} else {
			view.projection = p_surface_cam->get_camera_projection();
		}
		if (Viewport *viewport = p_surface_cam->get_viewport()) {
			view.raster = Vector2i(viewport->get_visible_rect().size);
		}
	}
	return view;
}

// Installs the reflected-scene camera into the reflection SubViewport: the
// drawing camera mirrored about the water plane at or above it, unchanged
// below it, under the source's own frustum. The witnessed form,
// side-dependent collection filter and target size live in
// environment/water_mirror.h; this leg extracts the source camera (its
// viewport is the one it draws: the surface, or the live aspect-mode or NVG
// target) and the projection it draws with, installs the typed record and
// serves the mirror's 512 x 512 raster that frustum through
// TargetProjectionXrInterface: a camera would draw the square's own ratio,
// retail draws the main view's field into it (non-square texels).
void Water::_update_reflection_camera(Camera3D *p_cam, const Projection &p_projection,
		bool p_frame_perspective) {
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
	// A one-pixel viewport is a real transient state while a viewport resizes or
	// lays out workspaces. The strip builder below already treats either
	// dimension <= 1 as non-drawable; stop the mirror projection here too,
	// before an extreme aspect asks Camera3D for an out-of-range FOV.
	if (source_size.x <= 1.0f || source_size.y <= 1.0f) {
		return;
	}
	// The mirror draws under the device's one texfilter mode like the main
	// view (engine renderer/texture_filter.h): the terrain detail family's
	// hardware anisotropy is the viewport's, so the mirror takes the view's.
	if (reflection_viewport_->get_anisotropic_filtering_level() !=
			viewport->get_anisotropic_filtering_level()) {
		reflection_viewport_->set_anisotropic_filtering_level(
				viewport->get_anisotropic_filtering_level());
	}
	// The drawn frustum's width over height: proj[1][1] / proj[0][0] for the
	// perspective, orthogonal and frustum forms alike (the NVG raster's served
	// matrix included, whose aspect its square target does not carry).
	const real_t focal_x = p_projection.columns[0][0];
	const real_t focal_y = p_projection.columns[1][1];
	if (!(focal_x > 0.0f) || !(focal_y > 0.0f)) {
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
	// The frame's cameras draw their perspective through a frustum form only
	// for their raster's half pixel; the mirror takes the perspective itself
	// (its fov and keep mode), as the reflected pass takes the frame's plain
	// projection (the witness at env::WaterMirrorView::raster_shift).
	switch (p_frame_perspective ? Camera3D::PROJECTION_PERSPECTIVE : p_cam->get_projection()) {
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
	source.aspect = static_cast<float>(focal_y / focal_x);
	source.v_offset = p_cam->get_v_offset();

	const opennova::env::WaterMirrorView view =
			opennova::env::build_water_mirror_view(source, water_height_);
	const auto to_v3 = [](const opennova::env::Vec3 &v) {
		return Vector3(v.x, v.y, v.z);
	};
	// The reflected-scene clip needs no per-frame arming: the object and
	// terrain shaders discard below the water in the one pass whose camera
	// omits the water layer, on both sides of the plane (water_mirror.h).
	reflection_camera_->set_global_transform(Transform3D(
			Basis(to_v3(view.basis_x), to_v3(view.basis_y), to_v3(view.basis_z)),
			to_v3(view.origin)));
	reflection_camera_->set_cull_mask(view.below_water
					? (REFLECTION_CULL_MASK | VISUAL_LAYER_WORLD_NO_MIRROR | visual_layers::MAIN_VIEW_NO_MIRROR)
					: REFLECTION_CULL_MASK);
	reflection_camera_->set_keep_aspect_mode(view.keep_aspect_height ?
					Camera3D::KEEP_HEIGHT : Camera3D::KEEP_WIDTH);
	switch (view.projection) {
		case opennova::env::MirrorProjection::kOrthogonal:
			reflection_camera_->set_orthogonal(view.size,
					p_cam->get_near(), p_cam->get_far());
			break;
		case opennova::env::MirrorProjection::kFrustum:
			reflection_camera_->set_frustum(view.size,
					Vector2(view.frustum_offset_x, view.frustum_offset_y),
					p_cam->get_near(), p_cam->get_far());
			break;
		case opennova::env::MirrorProjection::kPerspective:
		default:
			reflection_camera_->set_perspective(view.fov_deg,
					p_cam->get_near(), p_cam->get_far());
			break;
	}
	// The camera node's own frame (the mirror viewport's visible rect) is the
	// source's, so its node-side projection -- the particle renderer's mirror
	// view reads it -- is the source camera's; the raster is the served one.
	const Vector2i source_frame(source_size);
	if (reflection_viewport_->get_size_2d_override() != source_frame) {
		reflection_viewport_->set_size_2d_override(source_frame);
	}
	// These offsets are independent of the projection mode and are otherwise
	// lost when the reflection camera is rebuilt from the source transform.
	reflection_camera_->set_h_offset(p_cam->get_h_offset());
	reflection_camera_->set_v_offset(view.v_offset);
	// The mirror image sits on the RTT's own D3D9 pixel centres
	// (env::WaterMirrorView::raster_shift): a perspective mirror's node draws
	// through that half texel, so its projection meets the served raster. An
	// orthogonal or frustum source (an editor or test camera) keeps its node's
	// own frustum; the served raster below takes the half texel in every form.
	if (view.projection == opennova::env::MirrorProjection::kPerspective) {
		draw_camera_through_d3d9_raster(reflection_camera_, view.raster_shift);
	}
	// The raster: the mirror frustum at the source's aspect over the 512
	// square, the matrix Camera3D builds for these settings over a viewport of
	// that aspect (its frustum form takes no keep-aspect flip).
	Projection mirror_projection;
	switch (view.projection) {
		case opennova::env::MirrorProjection::kOrthogonal:
			mirror_projection = Projection::create_orthogonal_aspect(view.size, view.aspect,
					p_cam->get_near(), p_cam->get_far(), !view.keep_aspect_height);
			break;
		case opennova::env::MirrorProjection::kFrustum:
			mirror_projection = Projection::create_frustum_aspect(view.size, view.aspect,
					Vector2(view.frustum_offset_x, view.frustum_offset_y),
					p_cam->get_near(), p_cam->get_far());
			break;
		case opennova::env::MirrorProjection::kPerspective:
		default:
			mirror_projection = Projection::create_perspective(view.fov_deg, view.aspect,
					p_cam->get_near(), p_cam->get_far(), !view.keep_aspect_height);
			break;
	}
	mirror_projection = ndc_translation(view.raster_shift) * mirror_projection;
	TargetProjectionXrInterface::serve(reflection_viewport_,
			reflection_camera_->get_camera_transform(), mirror_projection);
	_apply_reflection_clear();
}

// The mirror target's own clear (EnvironmentState::water_mirror_clear_color:
// the skyfog outdoors, black under the indoors letter, no thermal, waterline
// or NVG leg) through the mirror camera's own BG_COLOR environment -- the
// ClearColor settings, ambient off and the rest default -- instead of the
// shared World's beauty clear, pre-encoded like the beauty clear
// (GameWorld::update_frame_clear_color). Without a loaded environment the
// camera keeps the World's clear.
void Water::_apply_reflection_clear() {
	if (reflection_camera_ == nullptr) {
		return;
	}
	MissionEnvironment *env = _env_node();
	if (env == nullptr || !env->is_loaded()) {
		if (reflection_camera_->get_environment().is_valid()) {
			reflection_camera_->set_environment(Ref<Environment>());
		}
		return;
	}
	if (reflection_environment_.is_null()) {
		reflection_environment_.instantiate();
		reflection_environment_->set_background(Environment::BG_COLOR);
		reflection_environment_->set_ambient_source(Environment::AMBIENT_SOURCE_DISABLED);
	}
	if (reflection_camera_->get_environment() != reflection_environment_) {
		reflection_camera_->set_environment(reflection_environment_);
	}
	const opennova::env::Rgb clear =
			env->state().water_mirror_clear_color(mirror_scene_outdoors_);
	const Color encoded = Color(clear.r, clear.g, clear.b).linear_to_srgb();
	if (reflection_environment_->get_bg_color() != encoded) {
		reflection_environment_->set_bg_color(encoded);
	}
}

void Water::set_mirror_scene_outdoors(bool p_outdoors) {
	mirror_scene_outdoors_ = p_outdoors;
	_apply_reflection_clear();
}

// One screen march (env #29; row layout notes ride env_water_render.h) for the
// side's pass fog end: above water the smoothed fog distance attenuated by the
// overcast blend (the native env curve); below the surface the murk
// visibility curve replaces the weather fog distance.
int Water::_march_strip(const DrawingView &p_view, bool p_underwater, bool p_nightvision,
		float p_murk, float p_fog_end,
		const opennova::env::WaterDepthCurve &p_depth_curve,
		const Color &p_lit, const Ref<EnvFile> &p_env_data) {
	Camera3D *cam = p_view.camera;
	if (cam == nullptr || !cam->is_inside_tree()) {
		return 0;
	}
	const Vector2i vp_size = p_view.raster;
	if (vp_size.x <= 1 || vp_size.y <= 1) {
		return 0;
	}
	MissionEnvironment *env = _env_node();
	float pass_fog_end = EnvFile::fog_end_above_water(p_fog_end,
			env != nullptr ? env->get_overcast_blend() : 0.0f);
	if (p_underwater && p_env_data.is_valid()) {
		pass_fog_end = p_env_data->get_fog_end_underwater();
	}
	// The adjusted camera transform includes Camera3D h/v offsets, keeping
	// the screen-marched row coordinates registered to the view that draws
	// the strip (the surface, or the live aspect-mode or NVG target whose
	// pixels the blit stretches over it) under the projection and raster it
	// draws with.
	water_core_->strip_set_view(cam->get_camera_transform(), p_view.projection, vp_size,
			pass_fog_end);
	// The retail scene projection the strip depth is tested against: near 0.2,
	// far = the same fog word + 1 (renderer::scene_far_plane).
	water_material_->set_shader_parameter("u_scene_depth_range",
			Vector2(opennova::env::kWaterSceneNear,
					opennova::renderer::scene_far_plane(p_fog_end)));
	return water_core_->strip_build(water_height_, p_murk, p_lit,
			p_depth_curve.scale, p_depth_curve.bias, p_underwater, p_nightvision);
}

// The last march as surface arrays. Vertices are absolute world positions;
// COLOR carries the row diffuse, CUSTOM1 the row specular, CUSTOM0 =
// (depth, rhw, screen U, screen V), CUSTOM2 the texm3x2 perturbation basis,
// TEX_UV the witnessed render-basis world x/32, z/32 pair both noise
// textures sample.
Array Water::_strip_arrays() const {
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = water_core_->strip_positions();
	arrays[Mesh::ARRAY_COLOR] = water_core_->strip_colors();
	arrays[Mesh::ARRAY_TEX_UV] = water_core_->strip_uv0();
	arrays[Mesh::ARRAY_CUSTOM0] = water_core_->strip_custom0();
	arrays[Mesh::ARRAY_CUSTOM1] = water_core_->strip_custom1();
	arrays[Mesh::ARRAY_CUSTOM2] = water_core_->strip_custom2();
	arrays[Mesh::ARRAY_INDEX] = water_core_->strip_indices();
	return arrays;
}

void Water::_upload_strip(MeshInstance3D *p_mesh_instance, const Array &p_arrays,
		const Ref<ShaderMaterial> &p_material) {
	if (p_mesh_instance == nullptr) {
		return;
	}
	Ref<ArrayMesh> mesh = p_mesh_instance->get_mesh();
	if (mesh.is_null()) {
		return;
	}
	mesh->clear_surfaces();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, p_arrays, Array(),
			Dictionary(),
			(Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT) |
					(Mesh::ARRAY_CUSTOM_RGBA_FLOAT
							<< Mesh::ARRAY_FORMAT_CUSTOM1_SHIFT) |
					(Mesh::ARRAY_CUSTOM_RGBA_FLOAT
							<< Mesh::ARRAY_FORMAT_CUSTOM2_SHIFT));
	mesh->surface_set_material(0, p_material);
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

void Water::_clear_night_vision_surfaces() {
	if (night_vision_mesh_instance_ == nullptr) {
		return;
	}
	Ref<ArrayMesh> mesh = night_vision_mesh_instance_->get_mesh();
	if (mesh.is_valid() && mesh->get_surface_count() > 0) {
		mesh->clear_surfaces();
		// The focused Q3 record lists this strip's surface: the clear is a
		// rebuild too, so its surface list is re-read (to none) before the
		// next compile instead of drawing the last published strip.
		FrameFx::invalidate_q3_source(night_vision_mesh_instance_);
	}
}

} // namespace godot
