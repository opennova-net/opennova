#include "env/environment_cube_capture.h"

#include <algorithm>
#include <cstdint>

#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "env/water.h"

namespace godot {
namespace {

const Vector3 kFaceDirections[EnvironmentCubeCapture::kFaceCount] = {
	// Retail face order 0..5 = +X -X +Y -Y +Z -Z (update_environment_cubemap
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

// ImageTextureLayered::create_from_images requires +X,-X,+Y,-Y,+Z,-Z,
// which is deliberately NOT RenderingServer::CubeMapLayer's
// LEFT,RIGHT,BOTTOM,TOP,FRONT,BACK enum order used for the capture viewports.
// Keep the conversion explicit; the D3D12 orientation probe samples the
// resulting Cubemap rather than trusting either naming convention.
constexpr int kCubemapImageToCapture[EnvironmentCubeCapture::kFaceCount] = {
	1, 0, 3, 2, 5, 4,
};

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
	ClassDB::bind_method(D_METHOD("set_terrain_data", "terrain_data"),
			&EnvironmentCubeCapture::set_terrain_data);
	ClassDB::bind_method(D_METHOD("get_terrain_data"),
			&EnvironmentCubeCapture::get_terrain_data);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_data",
			PROPERTY_HINT_RESOURCE_TYPE, "TerrainData"),
			"set_terrain_data", "get_terrain_data");

	ClassDB::bind_method(D_METHOD("advance_frame", "player_position"),
			&EnvironmentCubeCapture::advance_frame);
	ClassDB::bind_method(D_METHOD("force_capture"),
			&EnvironmentCubeCapture::force_capture);
	ClassDB::bind_method(D_METHOD("is_cube_ready"),
			&EnvironmentCubeCapture::is_cube_ready);
	ClassDB::bind_method(D_METHOD("is_capture_pending"),
			&EnvironmentCubeCapture::is_capture_pending);
	ClassDB::bind_method(D_METHOD("get_render_frame_index"),
			&EnvironmentCubeCapture::get_render_frame_index);
	ClassDB::bind_method(D_METHOD("get_capture_origin"),
			&EnvironmentCubeCapture::get_capture_origin);
	ClassDB::bind_method(D_METHOD("get_environment_cube"),
			&EnvironmentCubeCapture::get_environment_cube);
	ClassDB::bind_method(D_METHOD("get_face_directions"),
			&EnvironmentCubeCapture::get_face_directions);
	ClassDB::bind_method(D_METHOD("get_face_up_vectors"),
			&EnvironmentCubeCapture::get_face_up_vectors);

	ClassDB::bind_integer_constant(get_class_static(), "", "FACE_COUNT",
			kFaceCount);
	ClassDB::bind_integer_constant(get_class_static(), "", "CAPTURE_SIZE",
			kCaptureSize);
	ClassDB::bind_integer_constant(get_class_static(), "", "REFRESH_FRAMES",
			kRefreshFrames);
	ClassDB::bind_integer_constant(get_class_static(), "", "SKY_DIM_BYTE",
			kSkyDimByte);
}

void EnvironmentCubeCapture::set_terrain_data(
		const Ref<TerrainData> &p_data) {
	terrain_data_ = p_data;
	force_pending_ = true;
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
	if (p_what == NOTIFICATION_READY) {
		_ensure_capture_nodes();
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs != nullptr) {
			rs->global_shader_parameter_set(
					"opennova_environment_cube_ready", false);
		}
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
		viewport->set_size(Vector2i(kCaptureSize, kCaptureSize));
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
		// wrote, which is what the raw-sampling consumer expects.
		viewport->set_use_hdr_2d(true);
		add_child(viewport);

		Camera3D *camera = memnew(Camera3D);
		camera->set_name("Camera");
		Ref<Compositor> capture_compositor;
		capture_compositor.instantiate();
		camera->set_compositor(capture_compositor);
		camera->set_projection(Camera3D::PROJECTION_PERSPECTIVE);
		camera->set_fov(90.0f);
		camera->set_near(0.5f);
		camera->set_far(1000.0f);
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
	// Recovery only: a dummy/headless renderer or a temporarily unavailable
	// target can leave the prior request pending.
	if (capture_pending_ && _publish_completed_capture()) {
		capture_pending_ = false;
	}

	const bool cadence_due = (render_frame_index_ % kRefreshFrames) == 0;
	if (!capture_pending_ && (force_pending_ || !cube_ready_ || cadence_due)) {
		// Retail renders all six faces in the offscreen-preparation leg of the
		// same frame. Here the six UPDATE_ONCE faces render with this frame's
		// ordinary draw and the readbacks publish at the next advance_frame:
		// one frame of latency on a 128-frame cadence, instead of a re-entrant
		// force_draw() that re-rendered every viewport (the beauty frame
		// included) from inside the frame pipeline.
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
	capture_origin_.y += 1.0f;
	if (terrain_data_.is_valid()) {
		const float terrain_y = terrain_data_->get_height_world_bilinear(
				Vector3(p_player_position.x, 0.0f, p_player_position.z));
		capture_origin_.y = std::max(capture_origin_.y, terrain_y + 10.0f);
	}

	for (int i = 0; i < kFaceCount; ++i) {
		cameras_[i]->look_at_from_position(capture_origin_,
				capture_origin_ + kFaceDirections[i], kFaceUps[i]);
		cameras_[i]->force_update_transform();
		viewports_[i]->set_update_mode(SubViewport::UPDATE_ONCE);
	}
	capture_pending_ = true;
	force_pending_ = false;
}

Ref<Image> EnvironmentCubeCapture::_to_retail_dimmed_face(
		const Ref<Image> &p_source, int p_orientation) {
	if (p_source.is_null() || p_source->is_empty()) {
		return Ref<Image>();
	}
	Ref<Image> image = p_source->duplicate();
	image->convert(Image::FORMAT_RGBA8);
	// A camera render looks outward from the cube center; Godot's Cubemap
	// layer convention addresses the corresponding face as viewed inward.
	// Reflect U once when crossing that boundary. The Forward+ D3D12 probe
	// samples center/up/right markers through the final samplerCube and pins
	// this independently for all six faces.
	if (p_orientation == 1) {
		image->flip_x();
	} else if (p_orientation == 2) {
		image->rotate_90(CLOCKWISE);
		image->flip_y();
	} else if (p_orientation == 3) {
		image->rotate_90(COUNTERCLOCKWISE);
		image->flip_y();
	}
	// The face target is HDR 2D, so the readback holds the gamma-domain
	// numbers the sky/celestial shaders wrote (no sRGB encode); the RGBA8
	// conversion above quantizes them to retail framebuffer bytes. The retail
	// quad multiplies them by vertex diffuse 0x60 under SRC=DESTCOLOR/DST=ZERO.
	PackedByteArray pixels = image->get_data();
	uint8_t *write = pixels.ptrw();
	for (int64_t i = 0; i + 3 < pixels.size(); i += 4) {
		for (int channel = 0; channel < 3; ++channel) {
			const uint32_t product =
					static_cast<uint32_t>(write[i + channel]) * kSkyDimByte;
			write[i + channel] = static_cast<uint8_t>((product + 127u) / 255u);
		}
		write[i + 3] = 255;
	}
	return Image::create_from_data(kCaptureSize, kCaptureSize, false,
			Image::FORMAT_RGBA8, pixels);
}

bool EnvironmentCubeCapture::_publish_completed_capture() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!is_selected_renderer(rs)) {
		return false;
	}
	TypedArray<Ref<Image>> faces;
	for (int layer = 0; layer < kFaceCount; ++layer) {
		const int capture = kCubemapImageToCapture[layer];
		Ref<ViewportTexture> texture = viewports_[capture]->get_texture();
		if (texture.is_null()) {
			return false;
		}
		// Side faces reflect U. Bottom/top rotate clockwise/counterclockwise
		// and reflect their post-rotation Y; their mapped retail up axes are
		// +X/-X rather than a side-face +Y.
		const int orientation = capture == 2 ? 2 : capture == 3 ? 3 : 1;
		Ref<Image> face = _to_retail_dimmed_face(
				texture->get_image(), orientation);
		if (face.is_null() || face->get_width() != kCaptureSize ||
				face->get_height() != kCaptureSize) {
			return false;
		}
		faces.push_back(face);
	}

	if (environment_cube_.is_null()) {
		environment_cube_.instantiate();
		if (environment_cube_->create_from_images(faces) != OK) {
			environment_cube_.unref();
			return false;
		}
	} else {
		for (int layer = 0; layer < kFaceCount; ++layer) {
			environment_cube_->update_layer(faces[layer], layer);
		}
	}

	cube_ready_ = true;
	rs->global_shader_parameter_set(
			"opennova_environment_cube", environment_cube_);
	rs->global_shader_parameter_set(
			"opennova_environment_cube_ready", true);
	return true;
}

void EnvironmentCubeCapture::_publish_inactive() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		rs->global_shader_parameter_set(
				"opennova_environment_cube_ready", false);
		rs->global_shader_parameter_set(
				"opennova_environment_cube", Variant());
	}
	capture_pending_ = false;
	cube_ready_ = false;
	environment_cube_.unref();
}

} // namespace godot
