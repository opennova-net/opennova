#include "render/frame_fx.h"
#include "render/frame_fx_compositor_effect.h"
#include "render/q3_frame_adapter.h"
#include "render/q3_source_registry.h"
#include "render/rd_glsl.h"
#include "render/rd_fullscreen.h"
#include "util/color_convert.h"
#include "render/rd_timestamp_span.h"
#include "render/rd_uniforms.h"
#include "render/target_projection_xr_interface.h"
#include "render/world_environment_lookup.h"
#include "util/string_convert.h"

#include <base/crt/crt_rng.h>
#include <runtime/world/tick_count_clock.h> // frame_tick_count_ms (the screen effects' GetTickCount)

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/classes/render_scene_buffers.hpp>
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

namespace {

// A normal gameplay camera keeps precisely the beauty layers left after the
// player/fly camera removes the FP body, shadow-only, and the twelve
// slot-capture bits: world, water, the terrain-shadow receiver plumbing bit,
// the no-mirror world layer, the foliage blanket bit (mirror-excluded — the
// witnessed reflection context collects no foliage, env-tod-re.md #30), and
// the first-person viewmodel (bit 11 — the gun draws inside the beauty pass
// through its shader-side renderfov projection and depth band, retail's
// "viewmodel first" step), and the empty-sector flat terrain fallback (bit
// 18, which the water mirror excludes: docs/terrain/terrain-re.md, "Empty-sector
// flat fallback"). Q3 omits the plumbing bit but not the viewmodel: the gun's
// rigid glow-capable strips copy like a world object's and draw under this
// camera's (the world's) projection against the beauty depth, where the gun's
// band depth rejects a copy over its own opaque strips and a renderfov other
// than the world fov moves the copy off the gun (runtime/renderer/q3_frame.h
// q3_object_source_admitted). That gives shaders a collision-free exact-mask signature
// without admitting caster or slot-capture geometry anywhere.
constexpr std::uint32_t kBeautyCameraMask = 0x0CD78C01u; // + MAIN_VIEW bits 20/22/23, the no-reflection bits 26/27 (visual_layers.h)
// The display decode table's texel count (FrameFx::install_display_table).
// The tonemap reads it through a linear, clamped sampler at u = the
// sRGB-encoded number, so texel i holds the decode of its centre
// (i + 0.5) / N and the read interpolates the curve between centres: at 4096
// texels the interpolation error stays under 3e-8 and the clamped half texel
// at each end under 0.08 of an 8-bit step.
constexpr int kDisplayDecodeTableTexels = 4096;

// The decode table as Godot's tonemap reads a 1D colour correction: one row,
// each channel the transfer kGlslGammaToLinear applies (the exact sRGB
// inverse), in 16-bit unsigned-normalised texels. The tonemap filters three
// reads of it a pixel, and a 64-bit texel costs the texture units fewer filter
// cycles than a 128-bit float one. Every 8-bit source value still comes back
// to its own byte (the largest error is 0.07 of a step), and a blended value
// rounds to the pair's byte but for about 1 in 1,500 at a rounding edge (1 in
// 12,500 with float texels; the decode pass's half-float store moved about 1 in
// 100; D-NET-1369).
Ref<ImageTexture> make_display_decode_table() {
	PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(kDisplayDecodeTableTexels) * 4 *
			static_cast<int64_t>(sizeof(uint16_t)));
	uint16_t *texels = reinterpret_cast<uint16_t *>(bytes.ptrw());
	for (int i = 0; i < kDisplayDecodeTableTexels; ++i) {
		const double encoded = (static_cast<double>(i) + 0.5) /
				static_cast<double>(kDisplayDecodeTableTexels);
		const double decoded = encoded < 0.04045 ?
				encoded / 12.92 :
				std::pow((encoded + 0.055) / 1.055, 2.4);
		const uint16_t value = static_cast<uint16_t>(std::floor(decoded * 65535.0 + 0.5));
		texels[i * 4 + 0] = value;
		texels[i * 4 + 1] = value;
		texels[i * 4 + 2] = value;
		texels[i * 4 + 3] = 65535;
	}
	const Ref<Image> image = Image::create_from_data(kDisplayDecodeTableTexels, 1,
			false, Image::FORMAT_RGBA16, bytes);
	return ImageTexture::create_from_image(image);
}

} // namespace

FrameFx::FrameFx() = default;

FrameFx::~FrameFx() = default;

void FrameFx::register_q3_object_material(const Ref<Material> &p_material,
		const opennova::renderer::ObjectMaterialClassification &p_classification,
		const Q3ObjectMaterialValues &p_values) {
	Q3SourceRegistry::register_object_material(p_material, p_classification, p_values);
}

void FrameFx::clone_q3_object_material(const Ref<Material> &p_source,
		const Ref<Material> &p_clone) {
	Q3SourceRegistry::clone_object_material(p_source, p_clone);
}

void FrameFx::update_q3_object_runtime(const Ref<Material> &p_material,
		const opennova::renderer::MaterialRuntime &p_runtime) {
	Q3SourceRegistry::update_object_runtime(p_material, p_runtime);
}

void FrameFx::update_q3_object_diffuse(const Ref<Material> &p_material,
		const Ref<Texture2D> &p_diffuse, float p_max_lod) {
	Q3SourceRegistry::update_object_diffuse(p_material, p_diffuse, p_max_lod);
}

void FrameFx::set_q3_object_self_lum(const Ref<Material> &p_material,
		const Vector3 &p_self_lum) {
	opennova::renderer::ObjectMaterialClassification classification;
	Q3ObjectMaterialValues values;
	if (!Q3SourceRegistry::object_material(p_material, classification, values))
		return;
	values.runtime.rgb_r = static_cast<float>(p_self_lum.x);
	values.runtime.rgb_g = static_cast<float>(p_self_lum.y);
	values.runtime.rgb_b = static_cast<float>(p_self_lum.z);
	Q3SourceRegistry::update_object_runtime(p_material, values.runtime);
}

void FrameFx::set_q3_object_alpha(const Ref<Material> &p_material, float p_alpha) {
	opennova::renderer::ObjectMaterialClassification classification;
	Q3ObjectMaterialValues values;
	if (!Q3SourceRegistry::object_material(p_material, classification, values))
		return;
	values.runtime.alpha = p_alpha;
	Q3SourceRegistry::update_object_runtime(p_material, values.runtime);
}

void FrameFx::set_q3_object_diffuse(const Ref<Material> &p_material,
		const Ref<Texture2D> &p_diffuse, float p_max_lod) {
	Q3SourceRegistry::update_object_diffuse(p_material, p_diffuse, p_max_lod);
}

void FrameFx::publish_q3_water(GeometryInstance3D *p_source,
		const Q3WaterMaterialValues &p_water) {
	Q3SourceRegistry::publish_water(p_source, p_water);
}

void FrameFx::register_q3_object_source(GeometryInstance3D *p_source,
		const Ref<Material> &p_material, Skeleton3D *p_rigid_skeleton,
		int p_rigid_bone) {
	Q3SourceRegistry::register_object_source(p_source, p_material,
			p_rigid_skeleton, p_rigid_bone);
}

void FrameFx::unregister_q3_source(GeometryInstance3D *p_source) {
	Q3SourceRegistry::unregister_source(p_source);
}

void FrameFx::set_q3_celestial_self_lum(GeometryInstance3D *p_source,
		const Vector3 &p_self_lum) {
	Q3SourceRegistry::set_celestial_self_lum(p_source,
			{p_self_lum.x, p_self_lum.y, p_self_lum.z});
}

void FrameFx::set_q3_celestial_pose(GeometryInstance3D *p_source,
		const Transform3D &p_global_transform) {
	Q3SourceRegistry::set_celestial_pose(p_source, p_global_transform);
}

bool FrameFx::q3_object_material_classification(const Ref<Material> &p_material,
		opennova::renderer::ObjectMaterialClassification &r_classification) {
	return Q3SourceRegistry::object_material_classification(p_material,
			r_classification);
}

void FrameFx::register_q3_source(GeometryInstance3D *p_source,
		opennova::renderer::Q3Source p_kind) {
	Q3SourceRegistry::register_source(p_source, p_kind);
}

void FrameFx::publish_q3_geometry(GeometryInstance3D *p_source, int p_surface,
		const Array &p_arrays) {
	Q3SourceRegistry::publish_geometry(p_source, p_surface, p_arrays);
}

void FrameFx::invalidate_q3_source(GeometryInstance3D *p_source) {
	Q3SourceRegistry::invalidate_source(p_source);
}

void FrameFx::invalidate_q3_instances(GeometryInstance3D *p_source) {
	Q3SourceRegistry::invalidate_instances(p_source);
}

void FrameFx::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_backend_report"),
			&FrameFx::get_backend_report);
	ClassDB::bind_method(D_METHOD("advance_frame"),
			&FrameFx::advance_frame);
	ClassDB::bind_method(D_METHOD("shutdown"), &FrameFx::shutdown);
	ClassDB::bind_method(D_METHOD("get_q3_target_image"),
			&FrameFx::get_q3_target_image);
	ClassDB::bind_method(D_METHOD("set_view_effects", "red_word", "camera_mode",
			"local_dead", "in_session", "death_elapsed_ticks", "thermal_view",
			"monitor_view", "nvg_active", "death_screen_active",
			"binoculars_view_active", "scoped_selector", "sighted_selector"),
			&FrameFx::set_view_effects_values);
	ClassDB::bind_method(D_METHOD("advance_screen_effects"),
			&FrameFx::advance_screen_effects);
	ClassDB::bind_method(D_METHOD("set_nvg_sights_card", "textures", "rects", "blends"),
			&FrameFx::set_nvg_sights_card);
	ClassDB::bind_method(D_METHOD("init_mission_textures"),
			&FrameFx::init_mission_textures);
	ClassDB::bind_method(D_METHOD("has_mission_textures"),
			&FrameFx::has_mission_textures);
	ClassDB::bind_static_method("FrameFx",
			D_METHOD("invalidate_q3_source", "source"),
			&FrameFx::invalidate_q3_source);
	ClassDB::bind_static_method("FrameFx",
			D_METHOD("set_q3_object_self_lum", "material", "self_lum"),
			&FrameFx::set_q3_object_self_lum);
	ClassDB::bind_static_method("FrameFx",
			D_METHOD("set_q3_object_alpha", "material", "alpha"),
			&FrameFx::set_q3_object_alpha);
	ClassDB::bind_static_method("FrameFx",
			D_METHOD("set_q3_object_diffuse", "material", "diffuse", "max_lod"),
			&FrameFx::set_q3_object_diffuse);
	BIND_CONSTANT(kBeautyCameraMask);
}

Ref<Image> FrameFx::get_q3_target_image() const {
	return terminal_effect_.is_valid() ?
			terminal_effect_->capture_q3_target() : Ref<Image>();
}

void FrameFx::build_compositor() {
	if (shutdown_)
		return;
	if (terminal_effect_.is_null())
		terminal_effect_.instantiate();
	terminal_effect_->set_gpu_timing_enabled(gpu_timing_enabled_);
	// A rebuilt terminal effect (tree re-entry) keeps the mission's texture.
	if (!scanline_texels_.empty())
		terminal_effect_->publish_scanline_texels(scanline_texels_);
}

void FrameFx::set_gpu_timing_enabled(bool p_enabled) {
	gpu_timing_enabled_ = p_enabled;
	if (terminal_effect_.is_valid())
		terminal_effect_->set_gpu_timing_enabled(p_enabled);
}

void FrameFx::install_compositor() {
	if (shutdown_)
		return;
	Node *scope = get_parent() != nullptr ? get_parent() : this;
	WorldEnvironment *world_environment = find_world_environment(scope);
	if (world_environment == nullptr || terminal_effect_.is_null())
		return;
	world_environment_id_ = ObjectID(world_environment->get_instance_id());
	previous_compositor_ = world_environment->get_compositor();
	installed_compositor_ = compositor_with_terminal(previous_compositor_,
			terminal_effect_);
	world_environment->set_compositor(installed_compositor_);
	install_display_table(world_environment->get_environment());
}

// Retail's backbuffer holds the gamma-domain bytes every draw blended and the
// display shows them as they are (shaders/color.gdshaderinc carries the device
// witness). Ours writes those numbers into Godot's float scene buffer, whose
// tonemap sRGB-encodes them into an 8-bit target; the terminal effect's decode
// pass cancelled that encode, a full-frame read and write of the scene buffer
// per view (1.07 ms of a 1440p frame, L3-s31). Godot's tonemap applies an
// environment's colour correction after its encode (tonemap.glsl: the
// adjustments' encode, then the table), so with the adjustments at unity and
// this decode table as the correction it writes the gamma number itself into
// the 8-bit target: the bytes the decode and encode pair gave, inside the pass
// the frame runs anyway. Every view drawn with the world's environment takes
// it and the terminal effect runs no decode for them.
void FrameFx::install_display_table(const Ref<Environment> &p_environment) {
	if (p_environment.is_null() || terminal_effect_.is_null())
		return;
	if (display_table_.is_null())
		display_table_ = make_display_decode_table();
	table_environment_ = p_environment;
	previous_adjustment_ = AdjustmentState{ p_environment->is_adjustment_enabled(),
		p_environment->get_adjustment_brightness(),
		p_environment->get_adjustment_contrast(),
		p_environment->get_adjustment_saturation(),
		p_environment->get_adjustment_color_correction() };
	p_environment->set_adjustment_brightness(1.0f);
	p_environment->set_adjustment_contrast(1.0f);
	p_environment->set_adjustment_saturation(1.0f);
	p_environment->set_adjustment_color_correction(display_table_);
	p_environment->set_adjustment_enabled(true);
	terminal_effect_->set_display_table_environment(p_environment->get_rid());
}

void FrameFx::uninstall_display_table() {
	if (terminal_effect_.is_valid())
		terminal_effect_->set_display_table_environment(RID());
	if (table_environment_.is_valid() &&
			table_environment_->get_adjustment_color_correction().ptr() == display_table_.ptr()) {
		table_environment_->set_adjustment_enabled(previous_adjustment_.enabled);
		table_environment_->set_adjustment_brightness(previous_adjustment_.brightness);
		table_environment_->set_adjustment_contrast(previous_adjustment_.contrast);
		table_environment_->set_adjustment_saturation(previous_adjustment_.saturation);
		table_environment_->set_adjustment_color_correction(previous_adjustment_.color_correction);
	}
	table_environment_.unref();
	previous_adjustment_ = AdjustmentState();
}

void FrameFx::uninstall_compositor() {
	uninstall_display_table();
	WorldEnvironment *world_environment =
			world_environment_from_id(world_environment_id_);
	if (world_environment != nullptr &&
			world_environment->get_compositor() == installed_compositor_)
		world_environment->set_compositor(previous_compositor_);
	installed_compositor_.unref();
	previous_compositor_.unref();
	world_environment_id_ = ObjectID();
}

void FrameFx::restore_synced_camera_mask() {
	if (!has_synced_camera_mask_ || !synced_camera_id_.is_valid()) {
		has_synced_camera_mask_ = false;
		synced_camera_id_ = ObjectID();
		return;
	}
	Camera3D *camera = Object::cast_to<Camera3D>(ObjectDB::get_instance(
			static_cast<std::uint64_t>(synced_camera_id_)));
	if (camera != nullptr && camera->get_cull_mask() == kBeautyCameraMask)
		camera->set_cull_mask(synced_camera_original_mask_);
	has_synced_camera_mask_ = false;
	synced_camera_id_ = ObjectID();
}

void FrameFx::advance_frame() {
	if (shutdown_ || terminal_effect_.is_null())
		return;
	Viewport *viewport = get_viewport();
	Camera3D *camera = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
	const bool active = camera != nullptr && is_visible_in_tree();
	if (!active) {
		restore_synced_camera_mask();
		terminal_effect_->clear_q3_frame();
		return;
	}
	const ObjectID camera_id(camera->get_instance_id());
	if (!has_synced_camera_mask_ || synced_camera_id_ != camera_id) {
		restore_synced_camera_mask();
		synced_camera_id_ = camera_id;
		synced_camera_original_mask_ = camera->get_cull_mask();
		has_synced_camera_mask_ = true;
	}
	// Standardize the highest-quality retail beauty pass. The layers removed
	// here are rendered only by their dedicated capture/viewmodel devices.
	camera->set_cull_mask(kBeautyCameraMask);
	Node *scope = get_parent() != nullptr ? get_parent() : this;
	terminal_effect_->compile_q3_frame(scope, viewport, camera);
}

void FrameFx::set_view_effects(const opennova::renderer::FrameFxViewInputs &p_view) {
	view_effects_ = p_view;
}

void FrameFx::set_view_effects_values(int p_red_word, int p_camera_mode,
		bool p_local_dead, bool p_in_session, int p_death_elapsed_ticks,
		bool p_thermal_view, bool p_monitor_view, bool p_nvg_active,
		bool p_death_screen_active, bool p_binoculars_view_active,
		bool p_scoped_selector, bool p_sighted_selector) {
	opennova::renderer::FrameFxViewInputs view;
	view.red_word = p_red_word;
	view.camera_mode = p_camera_mode;
	view.local_dead = p_local_dead;
	view.in_session = p_in_session;
	view.death_elapsed_ticks = p_death_elapsed_ticks;
	view.thermal_view = p_thermal_view;
	view.monitor_view = p_monitor_view;
	view.nvg_active = p_nvg_active;
	view.death_screen_active = p_death_screen_active;
	view.binoculars_view_active = p_binoculars_view_active;
	view.scoped_selector = p_scoped_selector;
	view.sighted_selector = p_sighted_selector;
	set_view_effects(view);
}

void FrameFx::set_nvg_sights_card(const TypedArray<Texture2D> &p_textures,
		const PackedFloat32Array &p_rects, const PackedInt32Array &p_blends) {
	nvg_sights_.clear();
	RenderingServer *server = RenderingServer::get_singleton();
	const int64_t count = std::min<int64_t>(p_textures.size(),
			std::min<int64_t>(p_rects.size() / 4, p_blends.size()));
	for (int64_t i = 0; i < count; ++i) {
		const Ref<Texture2D> texture = p_textures[i];
		if (texture.is_null() || server == nullptr)
			continue;
		NvgViewDevice::SightsRow row;
		// The linear (UNORM) view: the frame's numbers are gamma-domain.
		row.texture = server->texture_get_rd_texture(texture->get_rid(), false);
		row.x1 = p_rects[i * 4 + 0];
		row.y1 = p_rects[i * 4 + 1];
		row.x2 = p_rects[i * 4 + 2];
		row.y2 = p_rects[i * 4 + 3];
		row.blend = p_blends[i];
		if (row.texture.is_valid())
			nvg_sights_.push_back(row);
	}
}

void FrameFx::set_distortion_drawer(
		const std::shared_ptr<FrameFxDistortionDrawer> &p_drawer) {
	distortion_drawer_ = p_drawer;
}

void FrameFx::advance_screen_effects() {
	if (shutdown_ || terminal_effect_.is_null())
		return;
	opennova::renderer::FrameFxFrameInputs inputs;
	inputs.view = view_effects_;
	inputs.distortion_present = distortion_drawer_ &&
			distortion_drawer_->frame_has_distortion();
	// GetTickCount's millisecond clock, the main loop's (engine world/tick_count_clock.h); only its
	// low bits reach the angle.
	inputs.clock_ms = opennova::world::frame_tick_count_ms();
	auto frame = std::make_shared<FrameFxScreenFrame>();
	frame->frame_id = ++screen_frame_id_;
	frame->plan = opennova::renderer::plan_frame_fx(inputs, planner_state_,
			[]() { return opennova::crt::crt_rand15(); });
	frame->distortion = distortion_drawer_;
	// The lens spans the surface's overlay rect, (0, 0)..(W - 1, H - 1)
	// (engine: runtime/hud/scope_circle_mask.h, over overlayCtx), rebuilt only
	// when the surface resizes.
	if (frame->plan.nvg.lens) {
		Viewport *viewport = get_viewport();
		const Vector2 surface = viewport != nullptr ? viewport->get_visible_rect().size : Vector2();
		const Vector2i size(static_cast<int>(surface.x), static_cast<int>(surface.y));
		if (size.x > 0 && size.y > 0) {
			if (!nvg_lens_ || size != nvg_lens_size_) {
				nvg_lens_ = std::make_shared<const opennova::renderer::NvgScopeLens>(
						opennova::renderer::build_nvg_scope_lens(0, 0, size.x - 1, size.y - 1,
								opennova::renderer::kNvgSceneSide));
				nvg_lens_size_ = size;
			}
			frame->lens = nvg_lens_;
			frame->screen_size = size;
		}
	}
	if (frame->plan.nvg.sighted)
		frame->nvg_sights = nvg_sights_;
	terminal_effect_->publish_screen_effects(frame);
}

void FrameFx::create_glow_source_device() {
	if (!shutdown_ && terminal_effect_.is_valid())
		terminal_effect_->create_glow_source_device();
}

void FrameFx::init_view_effects() {
	if (!shutdown_ && terminal_effect_.is_valid())
		terminal_effect_->create_view_effects_device();
}

void FrameFx::create_pixel_shaders() {
	if (!shutdown_ && terminal_effect_.is_valid())
		terminal_effect_->create_frame_fx_device();
}

void FrameFx::init_mission_textures() {
	scanline_texels_ = opennova::renderer::frame_fx_scanline_texels(
			[]() { return opennova::crt::crt_rand15(); });
	if (terminal_effect_.is_valid())
		terminal_effect_->publish_scanline_texels(scanline_texels_);
}

void FrameFx::shutdown() {
	if (shutdown_)
		return;
	shutdown_ = true;

	Ref<FrameFxCompositorEffect> effect = terminal_effect_;
	if (effect.is_valid())
		effect->set_enabled(false);
	restore_synced_camera_mask();
	uninstall_compositor();

	// Detaching only changes the next render setup. Drain any callback already
	// submitted before freeing the RenderingDevice objects it may still read.
	RenderingServer *server = RenderingServer::get_singleton();
	if (server != nullptr && server->get_rendering_device() != nullptr)
		server->force_sync();
	if (effect.is_valid())
		effect->release_device_resources();
	terminal_effect_.unref();
	effect.unref();
}

void FrameFx::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		// Re-entry after an exit-tree (or explicit) shutdown, the same
		// contract DisplayDecode keeps: the released terminal effect is rebuilt
		// by the READY leg below, so clear the latch and ask for that leg again
		// (READY fires only once on its own).
		if (shutdown_) {
			shutdown_ = false;
			request_ready();
		}
	} else if (p_what == NOTIFICATION_READY) {
		build_compositor();
		install_compositor();
		// One placement-independent sync so a headless/no-pipeline embedder
		// still boots with coherent views; the live per-frame sync is the
		// ordered GameWorld leg, never a process callback.
		advance_frame();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		shutdown();
	}
}

Dictionary FrameFx::get_backend_report() const {
	Dictionary result = terminal_effect_.is_valid() ?
			terminal_effect_->get_backend_report() :
			Dictionary();
	result["beauty_camera_mask"] = static_cast<int64_t>(kBeautyCameraMask);
	result["q3_working_height"] = static_cast<int64_t>(opennova::renderer::kFrameFxWorkSide);
	result["shutdown"] = shutdown_;
	WorldEnvironment *world_environment =
			world_environment_from_id(world_environment_id_);
	result["terminal_compositor_installed"] = world_environment != nullptr &&
			world_environment->get_compositor() == installed_compositor_;
	return result;
}
