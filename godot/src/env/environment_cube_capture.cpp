#include "env/environment_cube_capture.h"

#include <cstdint>

#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "env/water.h"
#include "render/world_environment_lookup.h"
#include "resource_index/resource_root.h" // session_title (the cube eye's clearance)

namespace godot {
namespace {

const Vector3 kFaceDirections[EnvironmentCubeCapture::kFaceCount] = {
	// Retail face order 0..5 = +X -X +Y -Y +Z -Z (EnvCube_Update
	// @0x6106a0 -> GTexture_RenderCubeMapFace @0x6864d0 - docs/render/render-lighting-re.md).
	// The retail render-float -> Godot world map swaps X/Z. Therefore Godot's
	// cube layers LEFT/RIGHT/FRONT/BACK receive retail faces 5/4/1/0. The
	// improper axis map also converts D3D's left-handed camera basis into
	// Godot's right-handed basis without an image mirror.
	Vector3(-1.0f, 0.0f, 0.0f), // Cubemap LEFT; retail face 5 (-Z).
	Vector3(1.0f, 0.0f, 0.0f),  // RIGHT; retail face 4 (+Z).
	Vector3(0.0f, -1.0f, 0.0f), // BOTTOM; retail face 3 (-Y).
	Vector3(0.0f, 1.0f, 0.0f),  // TOP; retail face 2 (+Y).
	Vector3(0.0f, 0.0f, -1.0f), // FRONT; retail face 1 (-X).
	Vector3(0.0f, 0.0f, 1.0f),  // BACK; retail face 0 (+X).
};

const Vector3 kFaceUps[EnvironmentCubeCapture::kFaceCount] = {
	Vector3(0.0f, 1.0f, 0.0f),  // -X, up +Y.
	Vector3(0.0f, 1.0f, 0.0f),  // +X, up +Y.
	Vector3(1.0f, 0.0f, 0.0f),  // -Y; retail up +Z -> Godot +X.
	Vector3(-1.0f, 0.0f, 0.0f), // +Y; retail up -Z -> Godot -X.
	Vector3(0.0f, 1.0f, 0.0f),  // -Z, up +Y.
	Vector3(0.0f, 1.0f, 0.0f),  // +Z, up +Y.
};

// The RD cubemap's layers run +X,-X,+Y,-Y,+Z,-Z, which is deliberately NOT
// RenderingServer::CubeMapLayer's LEFT,RIGHT,BOTTOM,TOP,FRONT,BACK enum order
// used for the capture viewports. Keep the conversion explicit; the D3D12
// orientation probe samples the published cube through the final
// samplerCube rather than trusting either naming convention.
constexpr int kCubeLayerToCapture[EnvironmentCubeCapture::kFaceCount] = {
	1, 0, 3, 2, 5, 4,
};

// Side faces reflect U. Bottom/top anti-transpose/transpose (the former
// rotate-and-flip); their mapped retail up axes are +X/-X rather than a
// side-face +Y.
EnvironmentCubeBlit::Orientation orientation_for_capture(int capture) {
	if (capture == 2)
		return EnvironmentCubeBlit::Orientation::AntiTranspose;
	if (capture == 3)
		return EnvironmentCubeBlit::Orientation::Transpose;
	return EnvironmentCubeBlit::Orientation::MirrorU;
}

// The ported renderer configuration is the Forward+ path on any
// RenderingDevice driver (D3D12 and Vulkan alike); the Compatibility
// renderer has no device and never publishes a cube.
bool is_selected_renderer(RenderingServer *p_rs) {
	return p_rs != nullptr &&
			p_rs->get_current_rendering_method() == "forward_plus" &&
			p_rs->get_rendering_device() != nullptr;
}

} // namespace

void EnvironmentCubeCapture::_bind_methods() {
	ClassDB::bind_static_method("EnvironmentCubeCapture", D_METHOD("get_global_writes"),
			&EnvironmentCubeCapture::get_global_writes);
	ClassDB::bind_method(D_METHOD("set_terrain_data", "terrain_data"),
			&EnvironmentCubeCapture::set_terrain_data);
	ClassDB::bind_method(D_METHOD("get_terrain_data"),
			&EnvironmentCubeCapture::get_terrain_data);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_data",
			PROPERTY_HINT_RESOURCE_TYPE, "TerrainData"),
			"set_terrain_data", "get_terrain_data");

	ClassDB::bind_method(D_METHOD("set_cube_quality_word", "word"),
			&EnvironmentCubeCapture::set_cube_quality_word);
	ClassDB::bind_method(D_METHOD("get_capture_size"),
			&EnvironmentCubeCapture::get_capture_size);
	ClassDB::bind_method(D_METHOD("advance_frame", "player_position"),
			&EnvironmentCubeCapture::advance_frame);
	ClassDB::bind_method(D_METHOD("create_cube_device"),
			&EnvironmentCubeCapture::create_cube_device);
	ClassDB::bind_method(D_METHOD("is_cube_ready"),
			&EnvironmentCubeCapture::is_cube_ready);
	ClassDB::bind_method(D_METHOD("is_globals_held"), &EnvironmentCubeCapture::is_globals_held);
	ClassDB::bind_method(D_METHOD("is_capture_pending"),
			&EnvironmentCubeCapture::is_capture_pending);
	ClassDB::bind_method(D_METHOD("get_render_frame_index"),
			&EnvironmentCubeCapture::get_render_frame_index);
	ClassDB::bind_method(D_METHOD("get_capture_origin"),
			&EnvironmentCubeCapture::get_capture_origin);
	ClassDB::bind_method(D_METHOD("get_environment_cube"),
			&EnvironmentCubeCapture::get_environment_cube);
	ClassDB::bind_method(D_METHOD("get_device_publish_count"),
			&EnvironmentCubeCapture::get_device_publish_count);
	ClassDB::bind_method(D_METHOD("get_device_failure"),
			&EnvironmentCubeCapture::get_device_failure);
	ClassDB::bind_method(D_METHOD("get_face_directions"),
			&EnvironmentCubeCapture::get_face_directions);
	ClassDB::bind_method(D_METHOD("get_face_up_vectors"),
			&EnvironmentCubeCapture::get_face_up_vectors);

	ClassDB::bind_integer_constant(get_class_static(), "", "FACE_COUNT",
			kFaceCount);
	ClassDB::bind_integer_constant(get_class_static(), "", "REFRESH_FRAMES",
			kRefreshFrames);
	ClassDB::bind_integer_constant(get_class_static(), "", "SKY_DIM_BYTE",
			kSkyDimByte);
}

void EnvironmentCubeCapture::set_terrain_data(
		const Ref<TerrainData> &p_data) {
	terrain_data_ = p_data;
	force_pending_ = true;
	// The node outlives a mission: no frame of this one samples the last one's
	// cube; its first capture readies it again.
	cube_ready_ = false;
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr && !globals_held_) {
		_write_global(rs, "opennova_environment_cube_ready", false);
	}
}

int64_t EnvironmentCubeCapture::get_device_publish_count() const {
	return blit_ ? static_cast<int64_t>(blit_->published_cubes()) : 0;
}

String EnvironmentCubeCapture::get_device_failure() const {
	return blit_ ? blit_->failure() : String();
}

PackedVector3Array EnvironmentCubeCapture::get_face_directions() const {
	PackedVector3Array result;
	result.resize(kFaceCount);
	for (int i = 0; i < kFaceCount; ++i) {
		result.set(i, kFaceDirections[i]);
	}
	return result;
}

PackedVector3Array EnvironmentCubeCapture::get_face_up_vectors() const {
	PackedVector3Array result;
	result.resize(kFaceCount);
	for (int i = 0; i < kFaceCount; ++i) {
		result.set(i, kFaceUps[i]);
	}
	return result;
}

void EnvironmentCubeCapture::_notification(int p_what) {
	if (p_what == NOTIFICATION_ENTER_TREE) {
		// First entry and every re-entry after an exit-tree release: the
		// shader global is unready until the first copy of this residency has
		// been wired, and that copy is forced regardless of the cadence.
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs != nullptr && !globals_held_) {
			_write_global(rs, "opennova_environment_cube_ready", false);
		}
		force_pending_ = true;
	} else if (p_what == NOTIFICATION_READY) {
		_ensure_capture_nodes();
		_ensure_draw_effect();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		_publish_inactive();
	}
}

void EnvironmentCubeCapture::_ensure_capture_nodes() {
	if (viewports_[0] != nullptr) {
		return;
	}
	for (int i = 0; i < kFaceCount; ++i) {
		SubViewport *viewport = memnew(SubViewport);
		viewport->set_name(vformat("EnvironmentCubeFace%d", i));
		viewport->set_size(Vector2i(face_size_, face_size_));
		viewport->set_update_mode(SubViewport::UPDATE_DISABLED);
		viewport->set_clear_mode(SubViewport::CLEAR_MODE_ALWAYS);
		viewport->set_transparent_background(false);
		viewport->set_disable_3d(false);
		viewport->set_use_own_world_3d(false);
		viewport->set_handle_input_locally(false);
		// The retail callback disables lights. The admitted sky/celestial
		// shaders are unshaded; suppressing the atlas also prevents accidental
		// light work if that contract regresses.
		viewport->set_positional_shadow_atlas_size(0);
		// Every retail pass writes gamma-domain numeric values and only the
		// beauty camera carries the terminal display decode. Godot's tonemap
		// would sRGB-encode this offscreen target a second time, so keep it
		// HDR 2D: the texture then stores exactly the numbers the shaders
		// wrote, which is what the raw-sampling blit expects. (The world
		// environment's display decode table, FrameFx::install_display_table,
		// clamps them to [0, 1] on the way, as the blit's own clamp does.)
		viewport->set_use_hdr_2d(true);
		add_child(viewport);

		Camera3D *camera = memnew(Camera3D);
		camera->set_name("Camera");
		Ref<Compositor> capture_compositor;
		capture_compositor.instantiate();
		camera->set_compositor(capture_compositor);
		camera->set_projection(Camera3D::PROJECTION_PERSPECTIVE);
		camera->set_fov(opennova::renderer::kEnvironmentCubeFaceFovDegrees);
		camera->set_near(opennova::renderer::kEnvironmentCubeFaceNear);
		camera->set_far(opennova::renderer::kEnvironmentCubeFaceFar);
		camera->set_cull_mask(Water::VISUAL_LAYER_ENVIRONMENT_CAPTURE);
		viewport->add_child(camera);
		camera->make_current();

		viewports_[i] = viewport;
		cameras_[i] = camera;
	}
}

void EnvironmentCubeCapture::advance_frame(
		const Vector3 &p_player_position) {
	_ensure_capture_nodes();
	_ensure_draw_effect();
	// The previous request's copy ran in its own frame's draw; this closes it
	// (and copies here when no beauty view drew that frame, or recovers a
	// dummy/headless renderer or a temporarily unavailable target).
	if (capture_pending_ && _publish_completed_capture()) {
		capture_pending_ = false;
	}

	if (!capture_pending_ &&
			opennova::renderer::environment_cube_refresh_due(
					render_frame_index_, force_pending_, cube_ready_)) {
		// Retail renders all six faces ahead of the scene's draws in the frame
		// that samples them. Here the six UPDATE_ONCE faces render with this
		// frame's ordinary draw, ahead of the beauty view, and the copy into
		// the cube runs in that view's pre-opaque pass of the same draw
		// (EnvironmentCubeCompositorEffect), instead of a re-entrant
		// force_draw() that re-rendered every viewport from inside the frame
		// pipeline.
		_request_capture(p_player_position);
	}
	++render_frame_index_;
}

void EnvironmentCubeCapture::force_capture() {
	force_pending_ = true;
}

void EnvironmentCubeCapture::_request_capture(
		const Vector3 &p_player_position) {
	capture_origin_ = p_player_position;
	const bool has_terrain = terrain_data_.is_valid();
	const float terrain_y = has_terrain ?
			terrain_data_->get_height_world_bilinear(
					Vector3(p_player_position.x, 0.0f, p_player_position.z)) :
			0.0f;
	capture_origin_.y = opennova::renderer::environment_cube_eye_height(
			ResourceRoot::session_title(Ref<ResourceRoot>()), p_player_position.y,
			has_terrain, terrain_y);

	for (int i = 0; i < kFaceCount; ++i) {
		cameras_[i]->look_at_from_position(capture_origin_,
				capture_origin_ + kFaceDirections[i], kFaceUps[i]);
		cameras_[i]->force_update_transform();
		viewports_[i]->set_update_mode(SubViewport::UPDATE_ONCE);
	}
	capture_pending_ = true;
	force_pending_ = false;
	copy_armed_ = false;
	// The copy is armed for this frame's draw, and the cube (made by the
	// mission load's render subsystems) is wired now, so the frame that
	// renders the faces samples them.
	RenderingServer *rs = RenderingServer::get_singleton();
	if (is_selected_renderer(rs) && _arm_copy(rs) && blit_->cube_texture().is_valid()) {
		_wire_cube(rs);
	}
}

// The six face targets the copy reads, armed on the render thread so a
// threaded server's draw of an earlier frame never takes it.
bool EnvironmentCubeCapture::_arm_copy(RenderingServer *p_rs) {
	_ensure_blit();
	if (blit_->device_failed()) {
		return false;
	}
	EnvironmentCubeBlit::Request request;
	for (int layer = 0; layer < kFaceCount; ++layer) {
		const int capture = kCubeLayerToCapture[layer];
		const RID viewport_texture =
				p_rs->viewport_get_texture(viewports_[capture]->get_viewport_rid());
		if (!viewport_texture.is_valid()) {
			return false;
		}
		request[layer].viewport_texture = viewport_texture;
		request[layer].orientation = orientation_for_capture(capture);
	}
	armed_request_ = request;
	++requested_publishes_;
	copy_armed_ = true;
	p_rs->call_on_render_thread(callable_mp(this,
			&EnvironmentCubeCapture::_arm_on_render_thread));
	return true;
}

// The six rendered faces are copied on the render thread into the RD
// cubemap (orientation and the 0x60 byte multiply applied there) in the
// request frame's draw; the next advance closes the capture. A request that
// could not arm its copy arms it here, a frame with no beauty view to run the
// armed copy copies here from the faces the request rendered (the default
// render model runs the callable before this returns), and a failed copy is
// armed again.
bool EnvironmentCubeCapture::_publish_completed_capture() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!is_selected_renderer(rs)) {
		return false;
	}
	_ensure_blit();
	if (blit_->device_failed()) {
		// Latched until the next exit-tree release: the shader/pipeline/cube
		// creation failed, the object shaders keep their hemisphere fallback,
		// and the reason is reported once here and through get_device_failure.
		if (!warned_device_failure_) {
			warned_device_failure_ = true;
			UtilityFunctions::push_warning(
					"EnvironmentCubeCapture: the environment cube device copy is "
					"disabled: " + blit_->failure());
		}
		return false;
	}
	if (!copy_armed_) {
		_arm_copy(rs);
		return false;
	}
	if (blit_->completed_requests() < requested_publishes_) {
		rs->call_on_render_thread(callable_mp(this,
				&EnvironmentCubeCapture::_publish_on_render_thread));
		if (blit_->completed_requests() < requested_publishes_) {
			return false;
		}
	}
	copy_armed_ = false;
	if (!blit_->last_request_ok()) {
		_arm_copy(rs);
		return false;
	}
	return _wire_cube(rs);
}

// The cube on its shader global (once per device cube; the ready flag says
// whether it holds this mission's faces).
bool EnvironmentCubeCapture::_bind_cube(RenderingServer *p_rs) {
	const RID cube = blit_ ? blit_->cube_texture() : RID();
	if (!cube.is_valid()) {
		return false;
	}
	if (environment_cube_.is_null() ||
			environment_cube_->get_texture_rd_rid() != cube) {
		if (environment_cube_.is_null()) {
			environment_cube_.instantiate();
		}
		environment_cube_->set_texture_rd_rid(cube);
		if (!globals_held_) {
			_write_global(p_rs, "opennova_environment_cube", environment_cube_);
		}
	}
	return true;
}

bool EnvironmentCubeCapture::_wire_cube(RenderingServer *p_rs) {
	if (!_bind_cube(p_rs)) {
		return false;
	}
	wired_publishes_ = requested_publishes_;
	cube_ready_ = true;
	if (!globals_held_) {
		_write_global(p_rs, "opennova_environment_cube_ready", true);
	}
	return true;
}

void EnvironmentCubeCapture::republish_globals() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		return;
	}
	if (environment_cube_.is_valid()) {
		_write_global(rs, "opennova_environment_cube", environment_cube_);
	} else {
		_write_global(rs, "opennova_environment_cube", Variant());
	}
	_write_global(
			rs, "opennova_environment_cube_ready", cube_ready_ && environment_cube_.is_valid());
}

void EnvironmentCubeCapture::_write_global(
		RenderingServer *p_rs, const StringName &p_name, const Variant &p_value) {
	++global_writes_;
	p_rs->global_shader_parameter_set(p_name, p_value);
}

void EnvironmentCubeCapture::publish_absent() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		return;
	}
	_write_global(rs, "opennova_environment_cube_ready", false);
	_write_global(rs, "opennova_environment_cube", Variant());
}

void EnvironmentCubeCapture::create_cube_device() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!is_inside_tree() || !is_selected_renderer(rs)) {
		return;
	}
	_ensure_blit();
	_ensure_draw_effect();
	rs->call_on_render_thread(callable_mp(this, &EnvironmentCubeCapture::_create_on_render_thread));
	// Bound as the load makes it (unready), so the frame of the first capture
	// changes no texture binding of the materials that sample it.
	_bind_cube(rs);
}

void EnvironmentCubeCapture::set_cube_quality_word(int p_word) {
	const int size = opennova::renderer::environment_cube_face_size(p_word);
	if (size == face_size_) {
		return;
	}
	face_size_ = size;
	for (SubViewport *viewport : viewports_) {
		if (viewport != nullptr) {
			viewport->set_size(Vector2i(face_size_, face_size_));
		}
	}
	// The cube of the old side goes as an exit's does (_publish_inactive), and the mission's
	// create_cube_device makes it again at the new one, as the game's mission start makes both
	// cubes again (renderer/environment_cube.h cube_quality_word).
	if (blit_) {
		_publish_inactive();
	}
}

void EnvironmentCubeCapture::_ensure_blit() {
	if (!blit_) {
		blit_ = std::make_unique<EnvironmentCubeBlit>(face_size_);
	}
	if (draw_effect_.is_valid()) {
		draw_effect_->set_blit(blit_.get());
	}
}

// The draw's copy joins the compositor of the WorldEnvironment the beauty view
// draws with (created when it has none), followed into whatever compositor the
// environment holds now: FrameFx and DisplayDecode replace it with a fresh one
// carrying the previous effects.
void EnvironmentCubeCapture::_ensure_draw_effect() {
	if (!is_inside_tree()) {
		return;
	}
	if (draw_effect_.is_null()) {
		draw_effect_.instantiate();
		draw_effect_->set_blit(blit_.get());
	}
	WorldEnvironment *environment = Object::cast_to<WorldEnvironment>(
			ObjectDB::get_instance(world_environment_id_));
	if (environment == nullptr || !environment->is_inside_tree()) {
		environment = find_world_environment(get_parent() != nullptr ? get_parent() : this);
		if (environment == nullptr) {
			return;
		}
		world_environment_id_ = ObjectID(environment->get_instance_id());
	}
	Ref<Compositor> compositor = environment->get_compositor();
	if (compositor.is_null()) {
		compositor.instantiate();
		environment->set_compositor(compositor);
	}
	if (draw_effect_installed_into_ == compositor) {
		return;
	}
	_uninstall_draw_effect();
	TypedArray<Ref<CompositorEffect>> effects = compositor->get_compositor_effects();
	bool present = false;
	for (int64_t i = 0; i < effects.size(); ++i) {
		Ref<CompositorEffect> existing = effects[i];
		if (existing.is_valid() &&
				existing->get_instance_id() == draw_effect_->get_instance_id()) {
			present = true;
			break;
		}
	}
	if (!present) {
		effects.push_back(draw_effect_);
		compositor->set_compositor_effects(effects);
	}
	draw_effect_installed_into_ = compositor;
}

void EnvironmentCubeCapture::_uninstall_draw_effect() {
	if (draw_effect_installed_into_.is_valid() && draw_effect_.is_valid()) {
		const TypedArray<Ref<CompositorEffect>> effects =
				draw_effect_installed_into_->get_compositor_effects();
		TypedArray<Ref<CompositorEffect>> kept;
		for (int64_t i = 0; i < effects.size(); ++i) {
			Ref<CompositorEffect> existing = effects[i];
			if (existing.is_valid() &&
					existing->get_instance_id() == draw_effect_->get_instance_id()) {
				continue;
			}
			kept.push_back(existing);
		}
		draw_effect_installed_into_->set_compositor_effects(kept);
	}
	draw_effect_installed_into_.unref();
}

void EnvironmentCubeCapture::_arm_on_render_thread() {
	if (blit_) {
		blit_->arm(armed_request_);
	}
}

void EnvironmentCubeCapture::_publish_on_render_thread() {
	if (blit_) {
		blit_->publish_armed();
	}
}

void EnvironmentCubeCapture::_create_on_render_thread() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (blit_ && rs != nullptr) {
		blit_->create_device(rs->get_rendering_device());
	}
}

void EnvironmentCubeCapture::_release_on_render_thread() {
	if (blit_) {
		blit_->release();
	}
}

void EnvironmentCubeCapture::_publish_inactive() {
	RenderingServer *rs = RenderingServer::get_singleton();
	// A held capture leaves the pair to the picture that published it (E13).
	if (rs != nullptr && !globals_held_) {
		publish_absent();
	}
	if (environment_cube_.is_valid()) {
		environment_cube_->set_texture_rd_rid(RID());
		environment_cube_.unref();
	}
	// The draw's copy leaves the compositor and lets go of the blit; the drain
	// below finishes any callback already running with it.
	_uninstall_draw_effect();
	if (draw_effect_.is_valid()) {
		draw_effect_->set_blit(nullptr);
	}
	if (blit_ && rs != nullptr && rs->get_rendering_device() != nullptr) {
		// Drain any copy still queued, then free the device objects on the
		// render thread while the device is live (the FrameFx exit pattern).
		rs->call_on_render_thread(callable_mp(this,
				&EnvironmentCubeCapture::_release_on_render_thread));
		rs->force_sync();
	}
	blit_.reset();
	requested_publishes_ = 0;
	wired_publishes_ = 0;
	copy_armed_ = false;
	warned_device_failure_ = false;
	capture_pending_ = false;
	cube_ready_ = false;
	force_pending_ = true;
}

} // namespace godot
